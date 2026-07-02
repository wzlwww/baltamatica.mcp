/*
 * mcp_bridge.c - Minimal BEX bridge for baltamatica.mcp.
 *
 * Build from Baltamatica:
 *   bex "mcp_bridge.c"
 *
 * Start from Baltamatica:
 *   mcp_bridge              % foreground, listens on 127.0.0.1:31415 (blocks)
 *   mcp_bridge(43141)       % foreground on 127.0.0.1:43141
 *   mcp_bridge('background') % detached listener, frees the command line
 *
 * Stop from Baltamatica:
 *   mcp_bridge('stop')       % stops a running bridge (default port)
 *   mcp_bridge('stop', 43141)
 *
 * The foreground form blocks the command line. If it is interrupted with
 * Ctrl-C, the listening socket is tracked in process-global state so a later
 * mcp_bridge('stop') closes it directly (releasing the port) and a later
 * mcp_bridge() reclaims it instead of failing to bind. A background bridge is
 * stopped by waking its accept loop over the wire. Every command optionally
 * returns a numeric status (0 = ok) as its single output argument.
 *
 * Variable reads include text output plus structured JSON for small real
 * numeric and logical arrays.
 */

#include "bex/bex.h"
#include "mcp_protocol.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET mcp_socket_t;
typedef HANDLE mcp_thread_t;
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcp_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mcp_socket_t;
typedef pthread_t mcp_thread_t;
#define MCP_INVALID_SOCKET (-1)
#define mcp_close_socket close
#endif

#define MCP_ARRAY_LINE_WIDTH 120
#define MCP_MAX_OUTPUT 32768
#define MCP_MAX_VALUE_JSON 262144
#define MCP_MAX_VARIABLES 512
#define MCP_MAX_VALUE_ELEMENTS 512
#define MCP_MAX_VALUE_DEPTH 6
#define MCP_MAX_STRUCT_ELEMENTS 64
#define MCP_MAX_CELL_ELEMENTS 256

/* Control-char sentinel (unit separators) that a normal disp/fprintf never
 * emits; marks the start of a captured error message in execute_code output. */
#define MCP_ERR_MARKER "\x1fMCP_EXEC_ERROR\x1f"

typedef struct mcp_request_t {
    char id[BALTAMATICA_MCP_MAX_ID];
    char method[BALTAMATICA_MCP_MAX_METHOD];
    char code[BALTAMATICA_MCP_MAX_CODE];
    char file_path[BALTAMATICA_MCP_MAX_PATH];
    char name[BALTAMATICA_MCP_MAX_METHOD];
    char dtype[32];         /* set_variable: numpy-style dtype */
    baSize dims[2];         /* set_variable: [rows, cols] */
    int ndims;              /* set_variable: number of dims parsed */
    char *data_b64;         /* set_variable: heap-owned base64 payload */
} mcp_request_t;

/*
 * Unified, process-global state for the single listening bridge. Tracking the
 * server socket globally (instead of in a local variable of the accept loop)
 * lets `mcp_bridge('stop')` release the port even when the foreground accept
 * loop has already been torn down by Ctrl-C, and lets a fresh `mcp_bridge()`
 * reclaim a leaked socket instead of failing to bind.
 */
static volatile int mcp_server_active = 0;      /* a bridge is currently listening */
static volatile int mcp_server_background = 0;  /* it runs on a detached thread */
static volatile int mcp_server_port = 0;
static volatile int mcp_server_stop = 0;        /* request the accept loop to exit */
static mcp_socket_t mcp_server_fd = MCP_INVALID_SOCKET;
static mcp_thread_t mcp_background_thread;

static int mcp_socket_valid(mcp_socket_t socket_fd) {
    return socket_fd != MCP_INVALID_SOCKET;
}

static void mcp_server_state_clear(void) {
    mcp_server_fd = MCP_INVALID_SOCKET;
    mcp_server_port = 0;
    mcp_server_active = 0;
    mcp_server_background = 0;
    mcp_server_stop = 0;
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

static void mcp_set_close_on_exec(mcp_socket_t socket_fd) {
#ifndef _WIN32
    int flags = fcntl(socket_fd, F_GETFD, 0);
    if (flags >= 0) {
        (void)fcntl(socket_fd, F_SETFD, flags | FD_CLOEXEC);
    }
#else
    (void)socket_fd;
#endif
}

static int mcp_validate_outputs(int nlhs, bxArray *plhs[]) {
    (void)plhs;
    if (nlhs > 1) {
        bxErrMsgTxt("mcp_bridge returns at most one status value.");
        return -1;
    }
    return 0;
}

static void mcp_set_status_output(int nlhs, bxArray *plhs[], double status) {
    if (nlhs > 0) {
        plhs[0] = bxCreateDoubleScalar(status);
    }
}

static int mcp_parse_port_arg(const bxArray *arg) {
    const double *value;
    int port;

    if (!bxIsDouble(arg) || !bxIsScalar(arg)) {
        bxErrMsgTxt("mcp_bridge port must be a numeric scalar.");
        return -1;
    }

    value = bxGetDoublesRO(arg);
    port = value ? (int)(*value) : -1;
    if (port <= 0 || port > 65535) {
        bxErrMsgTxt("mcp_bridge port must be between 1 and 65535.");
        return -1;
    }
    return port;
}

static int mcp_parse_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    if (mcp_validate_outputs(nlhs, plhs) != 0) {
        return -1;
    }
    if (nrhs > 1) {
        bxErrMsgTxt("mcp_bridge accepts an optional port, or 'stop' with an optional port.");
        return -1;
    }
    if (nrhs == 0) {
        return BALTAMATICA_MCP_DEFAULT_PORT;
    }
    return mcp_parse_port_arg(prhs[0]);
}

static int mcp_array_command_equals(const bxArray *value, const char *expected) {
    char raw[64];
    char normalized[16];
    size_t out_index = 0;
    size_t raw_index;

    raw[0] = '\0';
    if (bxIsChar(value)) {
        const char *chars = bxGetCharsRO(value);
        baSize elements = bxGetNumberOfElements(value);
        baSize limit = elements < (baSize)(sizeof(raw) - 1) ? elements : (baSize)(sizeof(raw) - 1);
        if (chars == NULL) {
            return 0;
        }
        for (baSize i = 0; i < limit; ++i) {
            raw[i] = chars[i];
        }
        raw[limit] = '\0';
    } else if (bxIsString(value)) {
        const char *text = bxGetString(value, 0);
        if (text == NULL) {
            return 0;
        }
        snprintf(raw, sizeof(raw), "%s", text);
    } else {
        return 0;
    }

    for (raw_index = 0; raw[raw_index] != '\0' && out_index + 1 < sizeof(normalized); ++raw_index) {
        unsigned char ch = (unsigned char)raw[raw_index];
        if (isalpha(ch)) {
            normalized[out_index++] = (char)tolower(ch);
        }
    }
    normalized[out_index] = '\0';
    return strcmp(normalized, expected) == 0;
}

static int mcp_array_is_stop_command(const bxArray *value) {
    return mcp_array_command_equals(value, "stop");
}

static int mcp_array_is_background_command(const bxArray *value) {
    return mcp_array_command_equals(value, "background") || mcp_array_command_equals(value, "bg");
}

static int mcp_parse_command_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[], const char *command) {
    if (mcp_validate_outputs(nlhs, plhs) != 0) {
        return -1;
    }
    if (nrhs > 2) {
        char message[128];
        snprintf(message, sizeof(message), "mcp_bridge('%s') accepts at most one optional port argument.", command);
        bxErrMsgTxt(message);
        return -1;
    }
    if (nrhs == 1) {
        return BALTAMATICA_MCP_DEFAULT_PORT;
    }
    return mcp_parse_port_arg(prhs[1]);
}

static int mcp_parse_stop_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    return mcp_parse_command_port(nlhs, plhs, nrhs, prhs, "stop");
}

static int mcp_parse_background_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    return mcp_parse_command_port(nlhs, plhs, nrhs, prhs, "background");
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

/* Parse a JSON integer array like "dims":[2,3] into out; returns element count. */
static int mcp_json_get_int_array(const char *json, const char *key, baSize *out, int max_out) {
    char pattern[128];
    const char *cursor;
    int count = 0;
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
    if (*cursor != '[') {
        return 0;
    }
    ++cursor;
    while (count < max_out) {
        char *end;
        long parsed;
        cursor = mcp_skip_ws(cursor);
        if (*cursor == ']') {
            break;
        }
        parsed = strtol(cursor, &end, 10);
        if (end == cursor) {
            return 0;
        }
        out[count++] = (baSize)parsed;
        cursor = mcp_skip_ws(end);
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        if (*cursor == ']') {
            break;
        }
        return 0;
    }
    return count;
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
    } else if (strcmp(request->method, BALTAMATICA_MCP_METHOD_GET_VARIABLE) == 0) {
        if (!mcp_json_get_string(line, "name", request->name, sizeof(request->name))) {
            snprintf(error, error_size, "get_variable requires params.name.");
            return 0;
        }
    } else if (strcmp(request->method, BALTAMATICA_MCP_METHOD_SET_VARIABLE) == 0) {
        size_t cap;
        if (!mcp_json_get_string(line, "name", request->name, sizeof(request->name))) {
            snprintf(error, error_size, "set_variable requires params.name.");
            return 0;
        }
        if (!mcp_json_get_string(line, "dtype", request->dtype, sizeof(request->dtype))) {
            snprintf(error, error_size, "set_variable requires params.dtype.");
            return 0;
        }
        request->ndims = mcp_json_get_int_array(line, "dims", request->dims, 2);
        if (request->ndims != 2) {
            snprintf(error, error_size, "set_variable requires params.dims as [rows, cols].");
            return 0;
        }
        cap = strlen(line) + 1;
        request->data_b64 = (char *)malloc(cap);
        if (request->data_b64 == NULL) {
            snprintf(error, error_size, "Out of memory parsing set_variable.");
            return 0;
        }
        if (!mcp_json_get_string(line, "data_b64", request->data_b64, cap)) {
            free(request->data_b64);
            request->data_b64 = NULL;
            snprintf(error, error_size, "set_variable requires params.data_b64.");
            return 0;
        }
    }
    return 1;
}

static void mcp_send_all(mcp_socket_t client_fd, const char *buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int chunk = send(client_fd, buffer + sent, (int)(length - sent), 0);
        if (chunk <= 0) {
            return;  /* peer closed or error; nothing else we can do here */
        }
        sent += (size_t)chunk;
    }
}

static void mcp_send_text(mcp_socket_t client_fd, const char *text) {
    mcp_send_all(client_fd, text, strlen(text));
}

/* Stream raw bytes as standard base64 without line breaks, sending in chunks so
 * arbitrarily large arrays never need a full in-memory encode buffer. */
static void mcp_stream_base64(mcp_socket_t client_fd, const unsigned char *data, size_t length) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char out[4096];
    size_t out_index = 0;
    size_t i = 0;

    while (i + 3 <= length) {
        unsigned int triple = ((unsigned int)data[i] << 16) |
                              ((unsigned int)data[i + 1] << 8) |
                              (unsigned int)data[i + 2];
        out[out_index++] = table[(triple >> 18) & 0x3F];
        out[out_index++] = table[(triple >> 12) & 0x3F];
        out[out_index++] = table[(triple >> 6) & 0x3F];
        out[out_index++] = table[triple & 0x3F];
        i += 3;
        if (out_index > sizeof(out) - 4) {
            mcp_send_all(client_fd, out, out_index);
            out_index = 0;
        }
    }

    if (length - i == 1) {
        unsigned int triple = (unsigned int)data[i] << 16;
        out[out_index++] = table[(triple >> 18) & 0x3F];
        out[out_index++] = table[(triple >> 12) & 0x3F];
        out[out_index++] = '=';
        out[out_index++] = '=';
    } else if (length - i == 2) {
        unsigned int triple = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8);
        out[out_index++] = table[(triple >> 18) & 0x3F];
        out[out_index++] = table[(triple >> 12) & 0x3F];
        out[out_index++] = table[(triple >> 6) & 0x3F];
        out[out_index++] = '=';
    }
    if (out_index > 0) {
        mcp_send_all(client_fd, out, out_index);
    }
}

static int mcp_base64_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode standard base64 (ignoring whitespace) into out; returns decoded length. */
static size_t mcp_base64_decode(const char *in, unsigned char *out, size_t out_cap) {
    int quad[4];
    int qn = 0;
    size_t written = 0;
    for (const char *p = in; *p; ++p) {
        int value;
        if (*p == '=') {
            break;
        }
        value = mcp_base64_char(*p);
        if (value < 0) {
            continue;  /* skip newlines / stray whitespace */
        }
        quad[qn++] = value;
        if (qn == 4) {
            if (written + 3 > out_cap) return written;
            out[written++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            out[written++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
            out[written++] = (unsigned char)(((quad[2] & 0x3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn >= 2 && written < out_cap) {
        out[written++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
    }
    if (qn == 3 && written < out_cap) {
        out[written++] = (unsigned char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    }
    return written;
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

static void mcp_send_status(mcp_socket_t client_fd, const char *id, int port) {
    char port_text[32];
    snprintf(port_text, sizeof(port_text), "%d", port);
    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id);
    mcp_send_text(client_fd, "\",\"success\":true,\"output\":\"MCP bridge ready\",\"host\":\"");
    mcp_json_write_escaped(client_fd, BALTAMATICA_MCP_DEFAULT_HOST);
    mcp_send_text(client_fd, "\",\"port\":");
    mcp_send_text(client_fd, port_text);
    mcp_send_text(client_fd, ",\"artifacts\":[]}\n");
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

static void mcp_print_bridge_message(const char *phase, const char *preposition, int port) {
    (void)bxPrintf(
        "MCP bridge %s %s %s:%d\n",
        phase,
        preposition,
        BALTAMATICA_MCP_DEFAULT_HOST,
        port);
}

static void mcp_print_bridge_text(const char *message, int port) {
    (void)bxPrintf("MCP bridge %s %s:%d\n", message, BALTAMATICA_MCP_DEFAULT_HOST, port);
}

static int mcp_eval_command(const char *command) {
    bxArray *args[1];
    int status;

    args[0] = bxCreateString(command);
    if (args[0] == NULL) {
        return 1;
    }
    status = bxCallBaltamatica(0, NULL, 1, (const bxArray **)args, "eval");
    bxDestroyArray(args[0]);
    return status;
}

/*
 * Run user code with evalc so its console output is captured, wrapped in
 * try/catch so an error becomes captured text (prefixed by MCP_ERR_MARKER)
 * instead of aborting. On return: *captured holds the text, and the function
 * result is 0 on success or 1 if the code raised an error.
 */
static int mcp_eval_capture(const char *code, char *captured, size_t captured_size, int *is_error) {
    char *wrapped;
    bxArray *args[1];
    bxArray *plhs[1] = {NULL};
    char *marker;
    int status;
    size_t wrapped_size = strlen(code) + sizeof(MCP_ERR_MARKER) + 128;

    *is_error = 0;
    if (captured_size > 0) {
        captured[0] = '\0';
    }

    wrapped = (char *)malloc(wrapped_size);
    if (wrapped == NULL) {
        return 1;
    }
    /* Statements are newline-separated (not ';'-separated) so an unsuppressed
     * statement like "x = 40" still auto-displays "x = 40" for capture. clear of
     * a never-created variable is silent, so cleanup is unconditional. */
    snprintf(
        wrapped,
        wrapped_size,
        "try\n%s\ncatch mcp_err__\nfprintf('" MCP_ERR_MARKER "%%s', mcp_err__);\nend\nclear mcp_err__",
        code);

    args[0] = bxCreateString(wrapped);
    free(wrapped);
    if (args[0] == NULL) {
        return 1;
    }
    status = bxCallBaltamatica(1, plhs, 1, (const bxArray **)args, "evalc");
    bxDestroyArray(args[0]);

    if (status != 0) {
        /* evalc itself failed (e.g. syntax error before try/catch took hold). */
        if (plhs[0] != NULL) {
            bxDestroyArray(plhs[0]);
        }
        *is_error = 1;
        return 1;
    }

    if (plhs[0] != NULL) {
        if (bxAsCStr(plhs[0], captured, (baSize)captured_size) < 0 && captured_size > 0) {
            captured[0] = '\0';
        }
        bxDestroyArray(plhs[0]);
    }

    marker = strstr(captured, MCP_ERR_MARKER);
    if (marker != NULL) {
        /* Shift the error message to the front and drop the sentinel + any
         * partial stdout that preceded it. */
        memmove(captured, marker + strlen(MCP_ERR_MARKER),
                strlen(marker + strlen(MCP_ERR_MARKER)) + 1);
        *is_error = 1;
        return 1;
    }
    return 0;
}

static int mcp_is_valid_variable_name(const char *name) {
    size_t index;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (!isalpha((unsigned char)name[0])) {
        return 0;
    }
    for (index = 1; name[index] != '\0'; ++index) {
        unsigned char ch = (unsigned char)name[index];
        if (!isalnum(ch) && ch != '_') {
            return 0;
        }
    }
    return 1;
}

/*
 * Report whether a variable exists in the base workspace without evaluating it.
 * Reading a value still requires bxEvalIn (there is no direct value getter), but
 * bxEvalIn echoes its error to the GUI command window when the name is missing.
 * Callers use this to avoid that echo for absent variables.
 */
static int mcp_variable_exists(const char *name) {
    const char **names = NULL;
    int count = 0;
    int found = 0;

    bxGetVariableNames(&names, &count);
    for (int i = 0; names != NULL && i < count; ++i) {
        if (names[i] != NULL && strcmp(names[i], name) == 0) {
            found = 1;
            break;
        }
    }
    if (names != NULL) {
        bxFreeVariableNames(names);
    }
    return found;
}

static int mcp_lookup_variable(const char *name, bxArray **value) {
    if (!mcp_is_valid_variable_name(name)) {
        return 1;
    }
    *value = NULL;
    return bxEvalIn("base", name, value);
}

static void mcp_format_size(const bxArray *value, char *buffer, size_t buffer_size) {
    baSize ndim = bxGetNumberOfDimensions(value);
    const baSize *dims = bxGetDimensions(value);
    size_t used = 0;
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (ndim <= 0 || dims == NULL) {
        snprintf(buffer, buffer_size, "%lldx%lld", (long long)bxGetM(value), (long long)bxGetN(value));
        return;
    }
    for (baSize i = 0; i < ndim; ++i) {
        int written = snprintf(
            buffer + used,
            used < buffer_size ? buffer_size - used : 0,
            "%s%lld",
            i == 0 ? "" : "x",
            (long long)dims[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
        if (used >= buffer_size) {
            buffer[buffer_size - 1] = '\0';
            break;
        }
    }
}

static long long mcp_estimate_bytes(const bxArray *value) {
    long long elements = (long long)bxGetNumberOfElements(value);
    long long item_size = 0;
    switch (bxGetClassID(value)) {
    case bxINT8_CLASS:
    case bxUINT8_CLASS:
    case bxLOGICAL_CLASS:
        item_size = 1;
        break;
    case bxINT16_CLASS:
    case bxUINT16_CLASS:
        item_size = 2;
        break;
    case bxINT32_CLASS:
    case bxUINT32_CLASS:
    case bxSINGLE_CLASS:
        item_size = 4;
        break;
    case bxINT64_CLASS:
    case bxUINT64_CLASS:
    case bxDOUBLE_CLASS:
        item_size = 8;
        break;
    default:
        item_size = 0;
        break;
    }
    if (bxIsComplex(value)) {
        item_size *= 2;
    }
    return elements > 0 && item_size > 0 ? elements * item_size : 0;
}

static const char *mcp_class_name_from_id(bxClassID class_id) {
    switch (class_id) {
    case bxDOUBLE_CLASS:
        return "double";
    case bxSINGLE_CLASS:
        return "single";
    case bxINT8_CLASS:
        return "int8";
    case bxINT16_CLASS:
        return "int16";
    case bxINT32_CLASS:
        return "int32";
    case bxINT64_CLASS:
        return "int64";
    case bxUINT8_CLASS:
        return "uint8";
    case bxUINT16_CLASS:
        return "uint16";
    case bxUINT32_CLASS:
        return "uint32";
    case bxUINT64_CLASS:
        return "uint64";
    case bxLOGICAL_CLASS:
        return "logical";
    case bxCHAR_CLASS:
        return "char";
    case bxSTRING_CLASS:
        return "string";
    case bxSTRUCT_CLASS:
        return "struct";
    case bxCELL_CLASS:
        return "cell";
    default:
        return "unknown";
    }
}

static int mcp_structured_value_supported(const bxArray *value) {
    if (bxIsComplex(value)) {
        return 0;
    }
    switch (bxGetClassID(value)) {
    case bxDOUBLE_CLASS:
    case bxSINGLE_CLASS:
    case bxINT8_CLASS:
    case bxINT16_CLASS:
    case bxINT32_CLASS:
    case bxINT64_CLASS:
    case bxUINT8_CLASS:
    case bxUINT16_CLASS:
    case bxUINT32_CLASS:
    case bxUINT64_CLASS:
    case bxLOGICAL_CLASS:
        return 1;
    default:
        return 0;
    }
}

/*
 * Types that can be transferred as a raw column-major binary payload. Unlike
 * mcp_structured_value_supported this includes complex double/single (stored
 * interleaved re,im, which matches numpy complex layout). Integers are never
 * complex in Baltamatica.
 */
static int mcp_binary_value_supported(const bxArray *value) {
    switch (bxGetClassID(value)) {
    case bxDOUBLE_CLASS:
    case bxSINGLE_CLASS:
    case bxINT8_CLASS:
    case bxINT16_CLASS:
    case bxINT32_CLASS:
    case bxINT64_CLASS:
    case bxUINT8_CLASS:
    case bxUINT16_CLASS:
    case bxUINT32_CLASS:
    case bxUINT64_CLASS:
    case bxLOGICAL_CLASS:
        return 1;
    default:
        return 0;
    }
}

/*
 * Resolve the read-only data pointer, byte length, and numpy-style dtype for a
 * binary-supported array. Returns 1 on success (and fills the out params), 0 if
 * the type is unsupported or the pointer could not be obtained.
 */
static int mcp_numeric_raw(const bxArray *value, const void **ptr, size_t *nbytes, const char **dtype) {
    baSize elements = bxGetNumberOfElements(value);
    size_t n = elements > 0 ? (size_t)elements : 0;
    int is_complex = bxIsComplex(value);

    *ptr = NULL;
    *nbytes = 0;
    *dtype = "";

    switch (bxGetClassID(value)) {
    case bxDOUBLE_CLASS:
        if (is_complex) {
            *ptr = bxGetComplexDoublesRO(value); *nbytes = n * 16; *dtype = "complex128";
        } else {
            *ptr = bxGetDoublesRO(value); *nbytes = n * 8; *dtype = "float64";
        }
        break;
    case bxSINGLE_CLASS:
        if (is_complex) {
            *ptr = bxGetComplexSinglesRO(value); *nbytes = n * 8; *dtype = "complex64";
        } else {
            *ptr = bxGetSinglesRO(value); *nbytes = n * 4; *dtype = "float32";
        }
        break;
    case bxINT8_CLASS:   *ptr = bxGetInt8sRO(value);   *nbytes = n;     *dtype = "int8";   break;
    case bxINT16_CLASS:  *ptr = bxGetInt16sRO(value);  *nbytes = n * 2; *dtype = "int16";  break;
    case bxINT32_CLASS:  *ptr = bxGetInt32sRO(value);  *nbytes = n * 4; *dtype = "int32";  break;
    case bxINT64_CLASS:  *ptr = bxGetInt64sRO(value);  *nbytes = n * 8; *dtype = "int64";  break;
    case bxUINT8_CLASS:  *ptr = bxGetUInt8sRO(value);  *nbytes = n;     *dtype = "uint8";  break;
    case bxUINT16_CLASS: *ptr = bxGetUInt16sRO(value); *nbytes = n * 2; *dtype = "uint16"; break;
    case bxUINT32_CLASS: *ptr = bxGetUInt32sRO(value); *nbytes = n * 4; *dtype = "uint32"; break;
    case bxUINT64_CLASS: *ptr = bxGetUInt64sRO(value); *nbytes = n * 8; *dtype = "uint64"; break;
    case bxLOGICAL_CLASS: *ptr = bxGetLogicalsRO(value); *nbytes = n;   *dtype = "bool";   break;
    default:
        return 0;
    }
    return *ptr != NULL;
}

static void mcp_append_json(char *buffer, size_t buffer_size, size_t *used, const char *fmt, ...) {
    va_list args;
    int written;
    if (*used >= buffer_size) {
        return;
    }
    va_start(args, fmt);
    written = vsnprintf(buffer + *used, buffer_size - *used, fmt, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    if (*used + (size_t)written >= buffer_size) {
        *used = buffer_size - 1;
        buffer[*used] = '\0';
        return;
    }
    *used += (size_t)written;
}

static int mcp_append_value_number(const bxArray *value, baSize index, char *buffer, size_t buffer_size, size_t *used) {
    switch (bxGetClassID(value)) {
    case bxDOUBLE_CLASS: {
        const double *data = bxGetDoublesRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, isfinite(data[index]) ? "%.17g" : "null", data[index]);
        return 1;
    }
    case bxSINGLE_CLASS: {
        const float *data = bxGetSinglesRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, isfinite((double)data[index]) ? "%.9g" : "null", (double)data[index]);
        return 1;
    }
    case bxINT8_CLASS: {
        const int8_t *data = bxGetInt8sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%d", (int)data[index]);
        return 1;
    }
    case bxINT16_CLASS: {
        const int16_t *data = bxGetInt16sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%d", (int)data[index]);
        return 1;
    }
    case bxINT32_CLASS: {
        const int32_t *data = bxGetInt32sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%d", data[index]);
        return 1;
    }
    case bxINT64_CLASS: {
        const int64_t *data = bxGetInt64sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%lld", (long long)data[index]);
        return 1;
    }
    case bxUINT8_CLASS: {
        const uint8_t *data = bxGetUInt8sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%u", (unsigned int)data[index]);
        return 1;
    }
    case bxUINT16_CLASS: {
        const uint16_t *data = bxGetUInt16sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%u", (unsigned int)data[index]);
        return 1;
    }
    case bxUINT32_CLASS: {
        const uint32_t *data = bxGetUInt32sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%u", data[index]);
        return 1;
    }
    case bxUINT64_CLASS: {
        const uint64_t *data = bxGetUInt64sRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%llu", (unsigned long long)data[index]);
        return 1;
    }
    case bxLOGICAL_CLASS: {
        const bool *data = bxGetLogicalsRO(value);
        if (data == NULL) {
            return 0;
        }
        mcp_append_json(buffer, buffer_size, used, "%s", data[index] ? "true" : "false");
        return 1;
    }
    default:
        return 0;
    }
}

static void mcp_array_to_output(const bxArray *value, char *buffer, size_t buffer_size) {
    baSize required;
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    required = bxArrayToCStr(value, MCP_ARRAY_LINE_WIDTH, 0, NULL, 0) + 1;
    if (required <= 1) {
        return;
    }
    bxArrayToCStr(value, MCP_ARRAY_LINE_WIDTH, 1, buffer, (baSize)buffer_size);
    buffer[buffer_size - 1] = '\0';
    if ((size_t)required > buffer_size) {
        const char *suffix = "\n... output truncated ...";
        size_t suffix_len = strlen(suffix);
        if (buffer_size > suffix_len + 1) {
            memcpy(buffer + buffer_size - suffix_len - 1, suffix, suffix_len + 1);
        }
    }
}

/* Append a JSON-escaped string (without surrounding quotes) into the buffer. */
static void mcp_append_escaped(char *buffer, size_t buffer_size, size_t *used, const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    while (*cursor) {
        unsigned char ch = *cursor++;
        switch (ch) {
        case '"':  mcp_append_json(buffer, buffer_size, used, "\\\""); break;
        case '\\': mcp_append_json(buffer, buffer_size, used, "\\\\"); break;
        case '\n': mcp_append_json(buffer, buffer_size, used, "\\n"); break;
        case '\r': mcp_append_json(buffer, buffer_size, used, "\\r"); break;
        case '\t': mcp_append_json(buffer, buffer_size, used, "\\t"); break;
        default:
            if (ch < 0x20) {
                mcp_append_json(buffer, buffer_size, used, "\\u%04x", (unsigned int)ch);
            } else {
                mcp_append_json(buffer, buffer_size, used, "%c", (int)ch);
            }
            break;
        }
    }
}

/*
 * Append one variable as a JSON value at *used (composable, so structs and cells
 * can recurse into their members). Numeric leaves use a bounded column-major
 * data array; nested numeric arrays are not streamed as binary. Depth and
 * element counts are capped so the bounded output buffer cannot be overrun.
 */
static void mcp_append_value(const bxArray *value, char *buffer, size_t buffer_size, size_t *used, int depth) {
    bxClassID class_id = bxGetClassID(value);
    baSize ndim = bxGetNumberOfDimensions(value);
    const baSize *dims = bxGetDimensions(value);
    baSize elements = bxGetNumberOfElements(value);
    char size_text[128];

    mcp_format_size(value, size_text, sizeof(size_text));

    if (depth > MCP_MAX_VALUE_DEPTH) {
        mcp_append_json(
            buffer, buffer_size, used,
            "{\"supported\":false,\"type\":\"truncated\",\"class_name\":\"%s\",\"size\":\"%s\","
            "\"reason\":\"maximum nesting depth reached\"}",
            mcp_class_name_from_id(class_id), size_text);
        return;
    }

    mcp_append_json(buffer, buffer_size, used, "{\"supported\":");

    if (class_id == bxCHAR_CLASS) {
        char text[MCP_MAX_OUTPUT];
        /* Prefer the clean string form (e.g. "hello"); fall back to the display
         * form for multi-row char matrices that bxAsCStr cannot convert. */
        if (bxAsCStr(value, text, (baSize)sizeof(text)) < 0) {
            mcp_array_to_output(value, text, sizeof(text));
        }
        mcp_append_json(
            buffer, buffer_size, used,
            "true,\"type\":\"char\",\"class_name\":\"char\",\"size\":\"%s\",\"element_count\":%lld,\"text\":\"",
            size_text, (long long)elements);
        mcp_append_escaped(buffer, buffer_size, used, text);
        mcp_append_json(buffer, buffer_size, used, "\"}");
        return;
    }

    if (class_id == bxSTRING_CLASS) {
        baSize cap = elements < MCP_MAX_VALUE_ELEMENTS ? elements : MCP_MAX_VALUE_ELEMENTS;
        mcp_append_json(
            buffer, buffer_size, used,
            "true,\"type\":\"string\",\"class_name\":\"string\",\"size\":\"%s\","
            "\"element_count\":%lld,\"truncated\":%s,\"encoding\":\"column-major\",\"data\":[",
            size_text, (long long)elements, elements > MCP_MAX_VALUE_ELEMENTS ? "true" : "false");
        for (baSize i = 0; i < cap; ++i) {
            const char *text = bxGetString(value, i);
            mcp_append_json(buffer, buffer_size, used, "%s\"", i == 0 ? "" : ",");
            mcp_append_escaped(buffer, buffer_size, used, text);
            mcp_append_json(buffer, buffer_size, used, "\"");
        }
        mcp_append_json(buffer, buffer_size, used, "]}");
        return;
    }

    if (class_id == bxSTRUCT_CLASS) {
        baSize nfields = bxGetNumberOfFields(value);
        baSize elem_cap = elements < MCP_MAX_STRUCT_ELEMENTS ? elements : MCP_MAX_STRUCT_ELEMENTS;
        mcp_append_json(
            buffer, buffer_size, used,
            "true,\"type\":\"struct\",\"class_name\":\"struct\",\"size\":\"%s\","
            "\"element_count\":%lld,\"truncated\":%s,\"fields\":[",
            size_text, (long long)elements, elements > MCP_MAX_STRUCT_ELEMENTS ? "true" : "false");
        for (baSize k = 0; k < nfields; ++k) {
            mcp_append_json(buffer, buffer_size, used, "%s\"", k == 0 ? "" : ",");
            mcp_append_escaped(buffer, buffer_size, used, bxGetFieldNameByNumber(value, (int)k));
            mcp_append_json(buffer, buffer_size, used, "\"");
        }
        mcp_append_json(buffer, buffer_size, used, "],\"data\":[");
        for (baSize e = 0; e < elem_cap; ++e) {
            mcp_append_json(buffer, buffer_size, used, "%s{", e == 0 ? "" : ",");
            for (baSize k = 0; k < nfields; ++k) {
                const bxArray *field = bxGetFieldByNumberRO(value, e, (int)k);
                mcp_append_json(buffer, buffer_size, used, "%s\"", k == 0 ? "" : ",");
                mcp_append_escaped(buffer, buffer_size, used, bxGetFieldNameByNumber(value, (int)k));
                mcp_append_json(buffer, buffer_size, used, "\":");
                if (field != NULL) {
                    mcp_append_value(field, buffer, buffer_size, used, depth + 1);
                } else {
                    mcp_append_json(buffer, buffer_size, used, "null");
                }
            }
            mcp_append_json(buffer, buffer_size, used, "}");
        }
        mcp_append_json(buffer, buffer_size, used, "]}");
        return;
    }

    if (class_id == bxCELL_CLASS) {
        baSize cap = elements < MCP_MAX_CELL_ELEMENTS ? elements : MCP_MAX_CELL_ELEMENTS;
        mcp_append_json(
            buffer, buffer_size, used,
            "true,\"type\":\"cell\",\"class_name\":\"cell\",\"size\":\"%s\","
            "\"element_count\":%lld,\"truncated\":%s,\"encoding\":\"column-major\",\"dims\":[",
            size_text, (long long)elements, elements > MCP_MAX_CELL_ELEMENTS ? "true" : "false");
        if (ndim <= 0 || dims == NULL) {
            mcp_append_json(buffer, buffer_size, used, "%lld,%lld", (long long)bxGetM(value), (long long)bxGetN(value));
        } else {
            for (baSize i = 0; i < ndim; ++i) {
                mcp_append_json(buffer, buffer_size, used, "%s%lld", i == 0 ? "" : ",", (long long)dims[i]);
            }
        }
        mcp_append_json(buffer, buffer_size, used, "],\"data\":[");
        for (baSize i = 0; i < cap; ++i) {
            const bxArray *element = bxGetCellRO(value, i);
            if (i > 0) {
                mcp_append_json(buffer, buffer_size, used, ",");
            }
            if (element != NULL) {
                mcp_append_value(element, buffer, buffer_size, used, depth + 1);
            } else {
                mcp_append_json(buffer, buffer_size, used, "null");
            }
        }
        mcp_append_json(buffer, buffer_size, used, "]}");
        return;
    }

    if (!mcp_structured_value_supported(value)) {
        mcp_append_json(
            buffer, buffer_size, used,
            "false,\"type\":\"unsupported\",\"class_name\":\"%s\",\"size\":\"%s\","
            "\"element_count\":%lld,\"reason\":\"Type not yet serialized; see output for text form.\"}",
            mcp_class_name_from_id(class_id), size_text, (long long)elements);
        return;
    }

    /* Real numeric / logical leaf: bounded column-major JSON data array. */
    {
        baSize emitted = elements < MCP_MAX_VALUE_ELEMENTS ? elements : MCP_MAX_VALUE_ELEMENTS;
        mcp_append_json(
            buffer, buffer_size, used,
            "true,\"type\":\"%s\",\"class_name\":\"%s\",\"size\":\"%s\",\"dims\":[",
            class_id == bxLOGICAL_CLASS ? "logical_array" : "numeric_array",
            mcp_class_name_from_id(class_id), size_text);
        if (ndim <= 0 || dims == NULL) {
            mcp_append_json(buffer, buffer_size, used, "%lld,%lld", (long long)bxGetM(value), (long long)bxGetN(value));
        } else {
            for (baSize i = 0; i < ndim; ++i) {
                mcp_append_json(buffer, buffer_size, used, "%s%lld", i == 0 ? "" : ",", (long long)dims[i]);
            }
        }
        mcp_append_json(
            buffer, buffer_size, used,
            "],\"encoding\":\"column-major\",\"element_count\":%lld,\"truncated\":%s,\"data\":[",
            (long long)elements, elements > MCP_MAX_VALUE_ELEMENTS ? "true" : "false");
        for (baSize i = 0; i < emitted; ++i) {
            if (i > 0) {
                mcp_append_json(buffer, buffer_size, used, ",");
            }
            if (!mcp_append_value_number(value, i, buffer, buffer_size, used)) {
                mcp_append_json(buffer, buffer_size, used, "null");
            }
        }
        mcp_append_json(buffer, buffer_size, used, "]}");
    }
}

static void mcp_array_to_json_value(const bxArray *value, char *buffer, size_t buffer_size) {
    size_t used = 0;
    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    mcp_append_value(value, buffer, buffer_size, &used, 0);
}

static void mcp_send_variable_value(mcp_socket_t client_fd, const char *id, const char *output, const char *value_json) {
    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id);
    mcp_send_text(client_fd, "\",\"success\":true,\"output\":\"");
    mcp_json_write_escaped(client_fd, output ? output : "");
    mcp_send_text(client_fd, "\",\"value\":");
    mcp_send_text(client_fd, value_json && value_json[0] ? value_json : "null");
    mcp_send_text(client_fd, ",\"artifacts\":[]}\n");
}

/*
 * Stream a full-fidelity numeric/logical value: metadata plus a base64 payload
 * of the raw column-major bytes. The payload is streamed directly to the socket
 * so there is no size cap and no truncation, regardless of how large the array
 * is. The Python side decodes data_b64 with the given dtype and dims.
 */
static void mcp_send_variable_binary(mcp_socket_t client_fd, const char *id, const char *output, const bxArray *value) {
    const void *ptr = NULL;
    size_t nbytes = 0;
    const char *dtype = "";
    char scratch[160];
    baSize ndim = bxGetNumberOfDimensions(value);
    const baSize *dims = bxGetDimensions(value);
    baSize elements = bxGetNumberOfElements(value);

    if (!mcp_numeric_raw(value, &ptr, &nbytes, &dtype)) {
        /* Should not happen for binary-supported types, but stay well-defined. */
        mcp_send_error(client_fd, id, BALTAMATICA_MCP_ERROR_VARIABLE, "Unable to read variable data.");
        return;
    }

    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id);
    mcp_send_text(client_fd, "\",\"success\":true,\"output\":\"");
    mcp_json_write_escaped(client_fd, output ? output : "");
    mcp_send_text(client_fd, "\",\"value\":{\"supported\":true,\"type\":\"ndarray\",\"class_name\":\"");
    mcp_json_write_escaped(client_fd, mcp_class_name_from_id(bxGetClassID(value)));

    mcp_format_size(value, scratch, sizeof(scratch));
    mcp_send_text(client_fd, "\",\"size\":\"");
    mcp_json_write_escaped(client_fd, scratch);

    mcp_send_text(client_fd, "\",\"dtype\":\"");
    mcp_send_text(client_fd, dtype);
    mcp_send_text(client_fd, "\",\"complexity\":\"");
    mcp_send_text(client_fd, bxIsComplex(value) ? "complex" : "real");
    mcp_send_text(client_fd, "\",\"byte_order\":\"little\",\"encoding\":\"base64\",\"dims\":[");
    if (ndim <= 0 || dims == NULL) {
        snprintf(scratch, sizeof(scratch), "%lld,%lld", (long long)bxGetM(value), (long long)bxGetN(value));
        mcp_send_text(client_fd, scratch);
    } else {
        for (baSize i = 0; i < ndim; ++i) {
            snprintf(scratch, sizeof(scratch), "%s%lld", i == 0 ? "" : ",", (long long)dims[i]);
            mcp_send_text(client_fd, scratch);
        }
    }
    snprintf(scratch, sizeof(scratch), "],\"element_count\":%lld,\"data_b64\":\"", (long long)elements);
    mcp_send_text(client_fd, scratch);

    mcp_stream_base64(client_fd, (const unsigned char *)ptr, nbytes);

    mcp_send_text(client_fd, "\"},\"artifacts\":[]}\n");
}

static void mcp_send_variable_list(mcp_socket_t client_fd, const char *id) {
    const char **names = NULL;
    int name_count = 0;
    int emitted = 0;
    char number[64];

    bxGetVariableNames(&names, &name_count);
    mcp_send_text(client_fd, "{\"id\":\"");
    mcp_json_write_escaped(client_fd, id);
    mcp_send_text(client_fd, "\",\"success\":true,\"output\":\"\",\"variables\":[");

    for (int i = 0; names != NULL && i < name_count && emitted < MCP_MAX_VARIABLES; ++i) {
        bxArray *value = NULL;
        char size_text[128];
        const char *class_name;

        if (mcp_lookup_variable(names[i], &value) != 0 || value == NULL) {
            continue;
        }
        mcp_format_size(value, size_text, sizeof(size_text));
        class_name = bxTypeCStr(value);

        if (emitted > 0) {
            mcp_send_text(client_fd, ",");
        }
        mcp_send_text(client_fd, "{\"name\":\"");
        mcp_json_write_escaped(client_fd, names[i]);
        mcp_send_text(client_fd, "\",\"size\":\"");
        mcp_json_write_escaped(client_fd, size_text);
        mcp_send_text(client_fd, "\",\"bytes\":");
        snprintf(number, sizeof(number), "%lld", mcp_estimate_bytes(value));
        mcp_send_text(client_fd, number);
        mcp_send_text(client_fd, ",\"class_name\":\"");
        mcp_json_write_escaped(client_fd, class_name ? class_name : "");
        mcp_send_text(client_fd, "\",\"attributes\":\"\"}");
        bxDestroyArray(value);
        ++emitted;
    }

    if (names != NULL) {
        bxFreeVariableNames(names);
    }
    mcp_send_text(client_fd, "]}\n");
}

/*
 * Inject a variable into the base workspace from a base64 column-major payload.
 * Supports float64 (double) and bool (logical) real matrices. Data is bounded by
 * the request line size, so this is for modest arrays, not huge datasets.
 */
static void mcp_handle_set_variable(mcp_socket_t client_fd, const mcp_request_t *request) {
    baSize rows = request->dims[0];
    baSize cols = request->dims[1];
    baSize elements;
    unsigned char *raw = NULL;
    size_t raw_cap;
    size_t raw_len;
    bxArray *array = NULL;

    if (!mcp_is_valid_variable_name(request->name)) {
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "Invalid variable name.");
        return;
    }
    if (rows < 0 || cols < 0) {
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "set_variable dims must be non-negative.");
        return;
    }
    elements = rows * cols;

    raw_cap = strlen(request->data_b64 ? request->data_b64 : "") + 1;
    raw = (unsigned char *)malloc(raw_cap > 0 ? raw_cap : 1);
    if (raw == NULL) {
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_INTERNAL, "Out of memory decoding set_variable.");
        return;
    }
    raw_len = mcp_base64_decode(request->data_b64 ? request->data_b64 : "", raw, raw_cap);

    if (strcmp(request->dtype, "float64") == 0) {
        if (raw_len != (size_t)elements * sizeof(double)) {
            free(raw);
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "set_variable data size does not match dims for float64.");
            return;
        }
        array = bxCreateDoubleMatrix(rows, cols, bxREAL);
        if (array != NULL) {
            double *dst = bxGetDoublesRW(array);
            if (dst != NULL && elements > 0) {
                memcpy(dst, raw, raw_len);
            }
        }
    } else if (strcmp(request->dtype, "bool") == 0) {
        if (raw_len != (size_t)elements) {
            free(raw);
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "set_variable data size does not match dims for bool.");
            return;
        }
        array = bxCreateLogicalMatrix(rows, cols);
        if (array != NULL) {
            bool *dst = bxGetLogicalsRW(array);
            if (dst != NULL) {
                for (baSize i = 0; i < elements; ++i) {
                    dst[i] = raw[i] != 0;
                }
            }
        }
    } else {
        free(raw);
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "set_variable supports only float64 and bool.");
        return;
    }

    free(raw);
    if (array == NULL) {
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_VARIABLE, "Failed to create variable array.");
        return;
    }

    if (bxAddVariable(request->name, array, bxOVERWRITE) == 1) {
        /* Ownership transferred on success; do not destroy the array. */
        mcp_send_success(client_fd, request->id, "");
    } else {
        bxDestroyArray(array);
        mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_VARIABLE, "Baltamatica rejected set_variable.");
    }
}

static void mcp_handle_request(mcp_socket_t client_fd, const mcp_request_t *request, volatile int *stop_server, int port) {
    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_STATUS) == 0) {
        mcp_send_status(client_fd, request->id, port);
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE) == 0) {
        char captured[MCP_MAX_OUTPUT];
        int is_error = 0;
        int status = mcp_eval_capture(request->code, captured, sizeof(captured), &is_error);
        if (status == 0) {
            mcp_send_success(client_fd, request->id, captured);
        } else if (is_error && captured[0] != '\0') {
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_EVAL, captured);
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
        snprintf(command, sizeof(command), "run('%s');", escaped);
        if (mcp_eval_command(command) == 0) {
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
        if (mcp_eval_command("clear;") == 0) {
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

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_LIST_VARIABLES) == 0) {
        mcp_send_variable_list(client_fd, request->id);
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_GET_VARIABLE) == 0) {
        bxArray *value = NULL;
        char output[MCP_MAX_OUTPUT];

        if (!mcp_is_valid_variable_name(request->name)) {
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "Invalid variable name.");
            return;
        }
        if (!mcp_variable_exists(request->name)) {
            /* Report the miss without calling bxEvalIn, which would echo an
             * "undefined variable" error to the GUI command window. */
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_VARIABLE, "Variable does not exist.");
            return;
        }
        if (mcp_lookup_variable(request->name, &value) != 0 || value == NULL) {
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_VARIABLE, "Variable lookup failed.");
            return;
        }
        mcp_array_to_output(value, output, sizeof(output));
        if (mcp_binary_value_supported(value)) {
            /* Full-fidelity numeric/logical transfer: stream raw bytes as base64. */
            mcp_send_variable_binary(client_fd, request->id, output, value);
        } else {
            /* char/string/struct/cell/etc.: structured JSON built into a heap
             * buffer (too large for the worker-thread stack). */
            char *value_json = (char *)malloc(MCP_MAX_VALUE_JSON);
            if (value_json != NULL) {
                mcp_array_to_json_value(value, value_json, MCP_MAX_VALUE_JSON);
                mcp_send_variable_value(client_fd, request->id, output, value_json);
                free(value_json);
            } else {
                mcp_send_variable_value(client_fd, request->id, output, "null");
            }
        }
        bxDestroyArray(value);
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_SET_VARIABLE) == 0) {
        mcp_handle_set_variable(client_fd, request);
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
        "Unsupported BEX bridge method.");
}

static void mcp_handle_client(mcp_socket_t client_fd, volatile int *stop_server, int port) {
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
                free(request.data_b64);
                continue;
            }
            mcp_handle_request(client_fd, &request, stop_server, port);
            free(request.data_b64);
        }
    }
}

static int mcp_open_server_socket(int port, mcp_socket_t *server_fd_out) {
    struct sockaddr_in address;
    mcp_socket_t server_fd = MCP_INVALID_SOCKET;
    int reuse = 1;

    *server_fd_out = MCP_INVALID_SOCKET;
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
    mcp_set_close_on_exec(server_fd);

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

    *server_fd_out = server_fd;
    return 0;
}

static int mcp_serve_socket(mcp_socket_t server_fd, int port, int print_stopped) {
    while (!mcp_server_stop) {
        mcp_socket_t client_fd = accept(server_fd, NULL, NULL);
        if (mcp_server_stop) {
            if (mcp_socket_valid(client_fd)) {
                mcp_close_socket(client_fd);
            }
            break;
        }
        if (!mcp_socket_valid(client_fd)) {
            /* accept() failed, most likely because the listening socket was
             * closed by a stop request. Leave the loop instead of spinning. */
            break;
        }
        mcp_handle_client(client_fd, &mcp_server_stop, port);
        mcp_close_socket(client_fd);
    }

    mcp_close_socket(server_fd);
    mcp_socket_cleanup();
    mcp_server_state_clear();
    if (print_stopped) {
        mcp_print_bridge_message("stopped", "on", port);
    }
    return 0;
}

/*
 * If a foreground bridge is still marked active while we are running on the
 * interpreter thread, its accept loop cannot actually be running (it would be
 * blocking this very thread). That means it was aborted with Ctrl-C, leaving
 * the listening socket open. Close it and clear the state so the port is free.
 */
static void mcp_reclaim_dead_foreground(void) {
    if (mcp_server_active && !mcp_server_background) {
        if (mcp_socket_valid(mcp_server_fd)) {
            mcp_close_socket(mcp_server_fd);
        }
        mcp_socket_cleanup();
        mcp_server_state_clear();
    }
}

static int mcp_listen_loop(int port) {
    mcp_socket_t server_fd = MCP_INVALID_SOCKET;

    mcp_reclaim_dead_foreground();
    if (mcp_server_active) {
        return 2;
    }
    if (mcp_open_server_socket(port, &server_fd) != 0) {
        return 1;
    }

    mcp_server_fd = server_fd;
    mcp_server_port = port;
    mcp_server_stop = 0;
    mcp_server_background = 0;
    mcp_server_active = 1;

    mcp_print_bridge_message("listening", "on", port);
    return mcp_serve_socket(server_fd, port, 1);
}

#ifdef _WIN32
static DWORD WINAPI mcp_background_thread_main(LPVOID ignored) {
#else
static void *mcp_background_thread_main(void *ignored) {
#endif
    (void)ignored;
    /* mcp_serve_socket clears the global server state when it exits. */
    mcp_serve_socket(mcp_server_fd, mcp_server_port, 0);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int mcp_start_background_bridge(int port) {
    mcp_socket_t server_fd = MCP_INVALID_SOCKET;

    mcp_reclaim_dead_foreground();
    if (mcp_server_active) {
        return 2;
    }
    if (mcp_open_server_socket(port, &server_fd) != 0) {
        return 1;
    }

    mcp_server_fd = server_fd;
    mcp_server_port = port;
    mcp_server_stop = 0;
    mcp_server_background = 1;
    mcp_server_active = 1;

#ifdef _WIN32
    mcp_background_thread = CreateThread(NULL, 0, mcp_background_thread_main, NULL, 0, NULL);
    if (mcp_background_thread == NULL) {
        mcp_close_socket(server_fd);
        mcp_socket_cleanup();
        mcp_server_state_clear();
        return 1;
    }
    CloseHandle(mcp_background_thread);
#else
    if (pthread_create(&mcp_background_thread, NULL, mcp_background_thread_main, NULL) != 0) {
        mcp_close_socket(server_fd);
        mcp_socket_cleanup();
        mcp_server_state_clear();
        return 1;
    }
    pthread_detach(mcp_background_thread);
#endif

    return 0;
}

static int mcp_send_shutdown_request(int port) {
    const char *request = "{\"id\":\"stop\",\"method\":\"shutdown\",\"params\":{}}\n";
    struct sockaddr_in address;
    mcp_socket_t client_fd = MCP_INVALID_SOCKET;
    int sent;

    if (!mcp_socket_startup()) {
        bxErrMsgTxt("Failed to initialize socket runtime.");
        return 2;
    }

    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!mcp_socket_valid(client_fd)) {
        mcp_socket_cleanup();
        bxErrMsgTxt("Failed to create BEX bridge shutdown socket.");
        return 2;
    }
    mcp_set_close_on_exec(client_fd);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);

    if (connect(client_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        mcp_close_socket(client_fd);
        mcp_socket_cleanup();
        return 1;
    }

    sent = send(client_fd, request, (int)strlen(request), 0);
    if (sent <= 0) {
        mcp_close_socket(client_fd);
        mcp_socket_cleanup();
        return 2;
    }

    mcp_close_socket(client_fd);
    mcp_socket_cleanup();
    return 0;
}

/*
 * Stop a running bridge. Returns:
 *   0  a bridge was stopped (or a shutdown was delivered over the wire)
 *   1  nothing was listening on that port
 *   2  a stop request could not be delivered
 */
static int mcp_stop_bridge(int port) {
    /* A background bridge runs its accept loop on another thread. Wake that
     * blocked accept() with a real shutdown connection; the worker thread then
     * observes the stop flag and closes its own socket. */
    if (mcp_server_active && mcp_server_background && mcp_server_port == port) {
        mcp_server_stop = 1;
        return mcp_send_shutdown_request(port) == 0 ? 0 : 2;
    }

    /* A foreground bridge is tracked but, since we are executing on the
     * interpreter thread, its accept loop is not running (Ctrl-C). Close the
     * leaked listening socket directly to release the port. */
    if (mcp_server_active && !mcp_server_background && mcp_server_port == port) {
        mcp_server_stop = 1;
        mcp_reclaim_dead_foreground();
        return 0;
    }

    /* Nothing is tracked in this process for that port. Fall back to the wire
     * protocol in case a bridge is listening in another session or process. */
    {
        int status = mcp_send_shutdown_request(port);
        if (status == 0) {
            return 0;
        }
        if (status == 1) {
            return 1;
        }
        return 2;
    }
}

void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    int port;
    if (nrhs > 0 && mcp_array_is_stop_command(prhs[0])) {
        int status;
        port = mcp_parse_stop_port(nlhs, plhs, nrhs, prhs);
        if (port < 0) {
            return;
        }
        status = mcp_stop_bridge(port);
        if (status == 0) {
            mcp_print_bridge_text("stopped on", port);
        } else if (status == 1) {
            mcp_print_bridge_text("is not listening on", port);
        } else {
            mcp_print_bridge_text("stop request failed on", port);
        }
        mcp_set_status_output(nlhs, plhs, status);
        return;
    }

    if (nrhs > 0 && mcp_array_is_background_command(prhs[0])) {
        int status;
        port = mcp_parse_background_port(nlhs, plhs, nrhs, prhs);
        if (port < 0) {
            return;
        }
        status = mcp_start_background_bridge(port);
        if (status == 0) {
            mcp_print_bridge_text("background listening on", port);
        } else if (status == 2) {
            mcp_print_bridge_text("background already running on", (int)mcp_server_port);
        } else {
            mcp_print_bridge_text("background start failed on", port);
        }
        mcp_set_status_output(nlhs, plhs, status);
        return;
    }

    port = mcp_parse_port(nlhs, plhs, nrhs, prhs);
    if (port < 0) {
        return;
    }
    mcp_print_bridge_message("ready", "at", port);
    {
        int status = mcp_listen_loop(port);  /* blocks until stopped; 2 = already running */
        if (status == 2) {
            mcp_print_bridge_text("already listening on", (int)mcp_server_port);
        }
        mcp_set_status_output(nlhs, plhs, status);
    }
}
