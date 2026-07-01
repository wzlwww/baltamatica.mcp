/*
 * mcp_bridge.c - Minimal BEX bridge for baltamatica.mcp.
 *
 * Build from Baltamatica:
 *   bex "mcp_bridge.c"
 *
 * Start from Baltamatica:
 *   mcp_bridge              % listens on 127.0.0.1:31415
 *   mcp_bridge(43141)       % listens on 127.0.0.1:43141
 *
 * The PR7 bridge is intentionally small and blocking. Run it in a dedicated
 * Baltamatica session, then point the MCP server at the same host and port.
 */

#include "bex/bex.h"
#include "mcp_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET mcp_socket_t;
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcp_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mcp_socket_t;
#define MCP_INVALID_SOCKET (-1)
#define mcp_close_socket close
#endif

typedef struct mcp_request_t {
    char id[BALTAMATICA_MCP_MAX_ID];
    char method[BALTAMATICA_MCP_MAX_METHOD];
    char code[BALTAMATICA_MCP_MAX_CODE];
    char file_path[BALTAMATICA_MCP_MAX_PATH];
} mcp_request_t;

static int mcp_socket_valid(mcp_socket_t socket_fd) {
    return socket_fd != MCP_INVALID_SOCKET;
}

static int mcp_socket_startup(void) {
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return 1;
#endif
}

static void mcp_socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static int mcp_parse_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    (void)plhs;
    if (nlhs > 0) {
        bxErrMsgTxt("mcp_bridge does not return output arguments.");
        return -1;
    }
    if (nrhs > 1) {
        bxErrMsgTxt("mcp_bridge accepts at most one optional port argument.");
        return -1;
    }
    if (nrhs == 0) {
        return BALTAMATICA_MCP_DEFAULT_PORT;
    }
    if (!bxIsDouble(prhs[0]) || !bxIsScalar(prhs[0])) {
        bxErrMsgTxt("mcp_bridge port must be a numeric scalar.");
        return -1;
    }

    {
        const double *value = bxGetDoublesRO(prhs[0]);
        int port = value ? (int)(*value) : -1;
        if (port <= 0 || port > 65535) {
            bxErrMsgTxt("mcp_bridge port must be between 1 and 65535.");
            return -1;
        }
        return port;
    }
}

static const char *mcp_skip_ws(const char *cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static int mcp_hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int mcp_append_utf8(unsigned int codepoint, char *out, size_t out_size, size_t *index) {
    if (codepoint <= 0x7F) {
        if (*index + 1 >= out_size) {
            return 0;
        }
        out[(*index)++] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF) {
        if (*index + 2 >= out_size) {
            return 0;
        }
        out[(*index)++] = (char)(0xC0 | (codepoint >> 6));
        out[(*index)++] = (char)(0x80 | (codepoint & 0x3F));
        return 1;
    }
    if (codepoint <= 0xFFFF) {
        if (*index + 3 >= out_size) {
            return 0;
        }
        out[(*index)++] = (char)(0xE0 | (codepoint >> 12));
        out[(*index)++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[(*index)++] = (char)(0x80 | (codepoint & 0x3F));
        return 1;
    }
    if (codepoint <= 0x10FFFF) {
        if (*index + 4 >= out_size) {
            return 0;
        }
        out[(*index)++] = (char)(0xF0 | (codepoint >> 18));
        out[(*index)++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[(*index)++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[(*index)++] = (char)(0x80 | (codepoint & 0x3F));
        return 1;
    }
    return 0;
}

static int mcp_read_json_string(const char *cursor, char *out, size_t out_size) {
    size_t index = 0;
    if (*cursor != '"') {
        return 0;
    }
    ++cursor;
    while (*cursor && *cursor != '"') {
        unsigned char ch = (unsigned char)*cursor++;
        if (ch == '\\') {
            char escaped = *cursor++;
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                ch = (unsigned char)escaped;
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'u': {
                unsigned int codepoint = 0;
                int i;
                for (i = 0; i < 4; ++i) {
                    int value = mcp_hex_value(cursor[i]);
                    if (value < 0) {
                        return 0;
                    }
                    codepoint = (codepoint << 4) | (unsigned int)value;
                }
                cursor += 4;
                if (!mcp_append_utf8(codepoint, out, out_size, &index)) {
                    return 0;
                }
                continue;
            }
            default:
                return 0;
            }
        }
        if (index + 1 >= out_size) {
            return 0;
        }
        out[index++] = (char)ch;
    }
    if (*cursor != '"') {
        return 0;
    }
    out[index] = '\0';
    return 1;
}

static int mcp_json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[128];
    const char *cursor;
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return 0;
    }
    cursor = strstr(json, pattern);
    if (!cursor) {
        return 0;
    }
    cursor += strlen(pattern);
    cursor = mcp_skip_ws(cursor);
    if (*cursor != ':') {
        return 0;
    }
    cursor = mcp_skip_ws(cursor + 1);
    return mcp_read_json_string(cursor, out, out_size);
}

static int mcp_parse_request(const char *line, mcp_request_t *request, char *error, size_t error_size) {
    memset(request, 0, sizeof(*request));
    if (!mcp_json_get_string(line, "id", request->id, sizeof(request->id))) {
        snprintf(error, error_size, "Missing or invalid request id.");
        return 0;
    }
    if (!mcp_json_get_string(line, "method", request->method, sizeof(request->method))) {
        snprintf(error, error_size, "Missing or invalid method.");
        return 0;
    }
    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE) == 0) {
        if (!mcp_json_get_string(line, "code", request->code, sizeof(request->code))) {
            snprintf(error, error_size, "execute_code requires params.code.");
            return 0;
        }
    } else if (strcmp(request->method, BALTAMATICA_MCP_METHOD_RUN_SCRIPT) == 0) {
        if (!mcp_json_get_string(line, "file_path", request->file_path, sizeof(request->file_path))) {
            snprintf(error, error_size, "run_script requires params.file_path.");
            return 0;
        }
    }
    return 1;
}

static void mcp_send_text(mcp_socket_t client_fd, const char *text) {
    send(client_fd, text, (int)strlen(text), 0);
}

static void mcp_json_write_escaped(mcp_socket_t client_fd, const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    while (*cursor) {
        char buffer[8];
        unsigned char ch = *cursor++;
        switch (ch) {
        case '"':
            mcp_send_text(client_fd, "\\\"");
            break;
        case '\\':
            mcp_send_text(client_fd, "\\\\");
            break;
        case '\n':
            mcp_send_text(client_fd, "\\n");
            break;
        case '\r':
            mcp_send_text(client_fd, "\\r");
            break;
        case '\t':
            mcp_send_text(client_fd, "\\t");
            break;
        default:
            if (ch < 0x20) {
                snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                mcp_send_text(client_fd, buffer);
            } else {
                send(client_fd, (const char *)&ch, 1, 0);
            }
            break;
        }
    }
}

static void mcp_send_success(mcp_socket_t client_fd, const char *id, const char *output) {
    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id);
    mcp_send_text(client_fd, "\",\"success\":true,\"output\":\"");
    mcp_json_write_escaped(client_fd, output ? output : "");
    mcp_send_text(client_fd, "\",\"artifacts\":[]}\n");
}

static void mcp_send_error(
    mcp_socket_t client_fd,
    const char *id,
    const char *code,
    const char *message) {
    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id ? id : "");
    mcp_send_text(client_fd, "\",\"success\":false,\"output\":\"\",\"error\":{\"code\":\"");
    mcp_json_write_escaped(client_fd, code);
    mcp_send_text(client_fd, "\",\"message\":\"");
    mcp_json_write_escaped(client_fd, message);
    mcp_send_text(client_fd, "\"},\"artifacts\":[]}\n");
}

static int mcp_recv_line(mcp_socket_t client_fd, char *line, size_t line_size) {
    size_t index = 0;
    while (index + 1 < line_size) {
        char ch;
        int received = recv(client_fd, &ch, 1, 0);
        if (received <= 0) {
            return 0;
        }
        if (ch == '\n') {
            line[index] = '\0';
            return 1;
        }
        if (ch != '\r') {
            line[index++] = ch;
        }
    }
    line[line_size - 1] = '\0';
    return -1;
}

static void mcp_escape_baltamatica_string(const char *input, char *output, size_t output_size) {
    size_t index = 0;
    while (*input && index + 1 < output_size) {
        if (*input == '\'' && index + 2 < output_size) {
            output[index++] = '\'';
            output[index++] = '\'';
            ++input;
        } else {
            output[index++] = *input++;
        }
    }
    output[index] = '\0';
}

static void mcp_handle_request(mcp_socket_t client_fd, const mcp_request_t *request, int *stop_server) {
    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE) == 0) {
        int status = bxEvalString(request->code);
        if (status == 0) {
            mcp_send_success(client_fd, request->id, "");
        } else {
            mcp_send_error(
                client_fd,
                request->id,
                BALTAMATICA_MCP_ERROR_EVAL,
                "Baltamatica rejected execute_code.");
        }
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_RUN_SCRIPT) == 0) {
        char escaped[BALTAMATICA_MCP_MAX_PATH * 2];
        char command[BALTAMATICA_MCP_MAX_PATH * 2 + 16];
        mcp_escape_baltamatica_string(request->file_path, escaped, sizeof(escaped));
        snprintf(command, sizeof(command), "run('%s')", escaped);
        if (bxEvalString(command) == 0) {
            mcp_send_success(client_fd, request->id, "");
        } else {
            mcp_send_error(
                client_fd,
                request->id,
                BALTAMATICA_MCP_ERROR_SCRIPT,
                "Baltamatica rejected run_script.");
        }
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_CLEAR_WORKSPACE) == 0) {
        if (bxEvalString("clear") == 0) {
            mcp_send_success(client_fd, request->id, "");
        } else {
            mcp_send_error(
                client_fd,
                request->id,
                BALTAMATICA_MCP_ERROR_EVAL,
                "Baltamatica rejected clear_workspace.");
        }
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_SHUTDOWN) == 0) {
        *stop_server = 1;
        mcp_send_success(client_fd, request->id, "shutting down");
        return;
    }

    mcp_send_error(
        client_fd,
        request->id,
        BALTAMATICA_MCP_ERROR_BAD_REQUEST,
        "Unsupported BEX bridge method in PR7.");
}

static void mcp_handle_client(mcp_socket_t client_fd, int *stop_server) {
    char line[BALTAMATICA_MCP_MAX_LINE];
    while (!*stop_server) {
        int status = mcp_recv_line(client_fd, line, sizeof(line));
        if (status == 0) {
            return;
        }
        if (status < 0) {
            mcp_send_error(client_fd, "", BALTAMATICA_MCP_ERROR_BAD_REQUEST, "Request line too long.");
            return;
        }

        {
            mcp_request_t request;
            char error[BALTAMATICA_MCP_MAX_ERROR];
            if (!mcp_parse_request(line, &request, error, sizeof(error))) {
                char id[BALTAMATICA_MCP_MAX_ID] = "";
                mcp_json_get_string(line, "id", id, sizeof(id));
                mcp_send_error(client_fd, id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, error);
                continue;
            }
            mcp_handle_request(client_fd, &request, stop_server);
        }
    }
}

static int mcp_listen_loop(int port) {
    struct sockaddr_in address;
    mcp_socket_t server_fd = MCP_INVALID_SOCKET;
    int stop_server = 0;
    int reuse = 1;

    if (!mcp_socket_startup()) {
        bxErrMsgTxt("Failed to initialize socket runtime.");
        return 1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!mcp_socket_valid(server_fd)) {
        mcp_socket_cleanup();
        bxErrMsgTxt("Failed to create BEX bridge socket.");
        return 1;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        mcp_close_socket(server_fd);
        mcp_socket_cleanup();
        bxErrMsgTxt("Failed to bind BEX bridge socket.");
        return 1;
    }

    if (listen(server_fd, BALTAMATICA_MCP_BACKLOG) != 0) {
        mcp_close_socket(server_fd);
        mcp_socket_cleanup();
        bxErrMsgTxt("Failed to listen on BEX bridge socket.");
        return 1;
    }

    while (!stop_server) {
        mcp_socket_t client_fd = accept(server_fd, NULL, NULL);
        if (!mcp_socket_valid(client_fd)) {
            continue;
        }
        mcp_handle_client(client_fd, &stop_server);
        mcp_close_socket(client_fd);
    }

    mcp_close_socket(server_fd);
    mcp_socket_cleanup();
    return 0;
}

void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    int port = mcp_parse_port(nlhs, plhs, nrhs, prhs);
    if (port < 0) {
        return;
    }
    (void)mcp_listen_loop(port);
}
