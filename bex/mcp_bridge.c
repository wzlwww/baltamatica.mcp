/*
 * mcp_bridge.c - Minimal BEX JSON-over-TCP bridge for Baltamatica.
 *
 * Build from the repository root:
 *   /Applications/Baltamatica.app/Contents/MacOS/bex bex/mcp_bridge.c
 *
 * Load from the Baltamatica GUI command window:
 *   mcp_bridge()
 *
 * The bridge listens on 127.0.0.1:31415 and accepts newline-delimited JSON
 * requests matching docs/bex-protocol.md. PR7 intentionally keeps the C side
 * small: execute_code, run_script, and clear_workspace are implemented through
 * Baltamatica's eval function; variable serialization remains a later PR.
 *
 * The server loop intentionally runs on the BEX invocation thread. Calling
 * Baltamatica interpreter APIs from a background pthread is not reliable across
 * runtimes, while the invocation thread can evaluate commands and open GUI
 * figures.
 */

#include "bex/bex.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 31415
#define MAX_REQUEST_SIZE 65536
#define MAX_FIELD_SIZE 32768
#define MAX_RESPONSE_SIZE 8192
#define MAX_COMMAND_SIZE 65536

static int g_stop_requested = 0;

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    if (dst_size == 0) {
        return;
    }

    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dst_size) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dst_size) {
                break;
            }
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static int extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[128];
    const char *pos;
    const char *cursor;
    size_t j = 0;

    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return 0;
    }
    pos += strlen(pattern);
    pos = strchr(pos, ':');
    if (pos == NULL) {
        return 0;
    }
    pos++;
    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }
    if (*pos != '"') {
        return 0;
    }
    cursor = pos + 1;

    while (*cursor != '\0' && j + 1 < out_size) {
        char c = *cursor++;
        if (c == '"') {
            out[j] = '\0';
            return 1;
        }
        if (c == '\\') {
            char esc = *cursor++;
            if (esc == '\0') {
                break;
            }
            if (esc == 'n') {
                c = '\n';
            } else if (esc == 'r') {
                c = '\r';
            } else if (esc == 't') {
                c = '\t';
            } else {
                c = esc;
            }
        }
        out[j++] = c;
    }

    out[j] = '\0';
    return 0;
}

static void escape_baltamatica_single_quotes(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    if (dst_size == 0) {
        return;
    }

    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_size; ++i) {
        if (src[i] == '\'' && j + 2 < dst_size) {
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void make_response(
    const char *id,
    bool success,
    const char *output,
    const char *error_code,
    const char *error_message,
    char *response,
    size_t response_size)
{
    char escaped_id[256];
    char escaped_output[MAX_FIELD_SIZE];
    char escaped_code[256];
    char escaped_message[MAX_FIELD_SIZE];

    json_escape(id, escaped_id, sizeof(escaped_id));
    json_escape(output ? output : "", escaped_output, sizeof(escaped_output));

    if (success) {
        snprintf(
            response,
            response_size,
            "{\"id\":\"%s\",\"success\":true,\"output\":\"%s\",\"artifacts\":[]}\n",
            escaped_id,
            escaped_output);
        return;
    }

    json_escape(error_code ? error_code : "INTERNAL_ERROR", escaped_code, sizeof(escaped_code));
    json_escape(error_message ? error_message : "", escaped_message, sizeof(escaped_message));
    snprintf(
        response,
        response_size,
        "{\"id\":\"%s\",\"success\":false,\"output\":\"%s\","
        "\"error\":{\"code\":\"%s\",\"message\":\"%s\"},\"artifacts\":[]}\n",
        escaped_id,
        escaped_output,
        escaped_code,
        escaped_message);
}

static int eval_command(const char *command)
{
    bxArray *args[1];

    args[0] = bxCreateString(command);
    if (args[0] == NULL) {
        return 1;
    }
    return bxCallBaltamatica(0, NULL, 1, (const bxArray **)args, "eval");
}

static void process_request(const char *request, char *response, size_t response_size)
{
    char id[256];
    char method[128];
    char code[MAX_FIELD_SIZE];
    char file_path[MAX_FIELD_SIZE];
    char escaped_path[MAX_FIELD_SIZE];
    char command[MAX_COMMAND_SIZE];
    int status;

    if (!extract_json_string(request, "id", id, sizeof(id))) {
        strcpy(id, "");
    }
    if (!extract_json_string(request, "method", method, sizeof(method))) {
        make_response(id, false, "", "BAD_REQUEST", "Missing method.", response, response_size);
        return;
    }

    if (strcmp(method, "shutdown") == 0) {
        g_stop_requested = 1;
        make_response(id, true, "MCP bridge shutting down.", NULL, NULL, response, response_size);
        return;
    }

    if (strcmp(method, "execute_code") == 0) {
        if (!extract_json_string(request, "code", code, sizeof(code))) {
            make_response(id, false, "", "BAD_REQUEST", "Missing params.code.", response, response_size);
            return;
        }
        status = eval_command(code);
        make_response(
            id,
            status == 0,
            "",
            "EVAL_ERROR",
            status == 0 ? "" : "Baltamatica failed to execute code.",
            response,
            response_size);
        return;
    }

    if (strcmp(method, "run_script") == 0) {
        if (!extract_json_string(request, "file_path", file_path, sizeof(file_path))) {
            make_response(
                id,
                false,
                "",
                "BAD_REQUEST",
                "Missing params.file_path.",
                response,
                response_size);
            return;
        }
        escape_baltamatica_single_quotes(file_path, escaped_path, sizeof(escaped_path));
        snprintf(command, sizeof(command), "run('%s');", escaped_path);
        status = eval_command(command);
        make_response(
            id,
            status == 0,
            "",
            "SCRIPT_ERROR",
            status == 0 ? "" : "Baltamatica failed to run script.",
            response,
            response_size);
        return;
    }

    if (strcmp(method, "clear_workspace") == 0) {
        status = eval_command("clear;");
        make_response(
            id,
            status == 0,
            "",
            "EVAL_ERROR",
            status == 0 ? "" : "Baltamatica failed to clear workspace.",
            response,
            response_size);
        return;
    }

    if (strcmp(method, "list_variables") == 0 || strcmp(method, "get_variable") == 0) {
        make_response(
            id,
            false,
            "",
            "NOT_IMPLEMENTED",
            "Variable access is planned for a later BEX PR.",
            response,
            response_size);
        return;
    }

    make_response(id, false, "", "BAD_REQUEST", "Unsupported method.", response, response_size);
}

static ssize_t read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;
    while (used + 1 < size) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buffer[used++] = c;
        if (c == '\n') {
            break;
        }
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static void handle_client(int client_fd)
{
    char request[MAX_REQUEST_SIZE];
    char response[MAX_RESPONSE_SIZE];

    while (!g_stop_requested) {
        ssize_t n = read_line(client_fd, request, sizeof(request));
        if (n <= 0) {
            break;
        }
        process_request(request, response, sizeof(response));
        send(client_fd, response, strlen(response), 0);
        if (g_stop_requested) {
            break;
        }
    }
}

static int server_loop(void)
{
    int server_fd;
    struct sockaddr_in addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        bxPrintf("MCP bridge failed to create socket: %d\n", errno);
        return errno;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        bxPrintf("MCP bridge failed to bind %s:%d: %d\n", BRIDGE_HOST, BRIDGE_PORT, errno);
        close(server_fd);
        return errno;
    }
    if (listen(server_fd, 8) != 0) {
        bxPrintf("MCP bridge failed to listen: %d\n", errno);
        close(server_fd);
        return errno;
    }

    bxPrintf("MCP bridge listening on %s:%d\n", BRIDGE_HOST, BRIDGE_PORT);

    while (!g_stop_requested) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

BEX_EXPORT void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[])
{
    int status;
    (void)nrhs;
    (void)prhs;

    g_stop_requested = 0;
    bxPrintf("MCP bridge ready at %s:%d\n", BRIDGE_HOST, BRIDGE_PORT);
    status = server_loop();

    if (nlhs > 0) {
        plhs[0] = bxCreateDoubleScalar((double)status);
    }
}
