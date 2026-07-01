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
 * The bridge is intentionally blocking. Run it in a dedicated Baltamatica
 * session, then point the MCP server at the same host and port. Variable
 * reads include text output plus structured JSON for small real numeric and
 * logical arrays.
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
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcp_close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int mcp_socket_t;
#define MCP_INVALID_SOCKET (-1)
#define mcp_close_socket close
#endif

#define MCP_ARRAY_LINE_WIDTH 120
#define MCP_MAX_OUTPUT 32768
#define MCP_MAX_VALUE_JSON 32768
#define MCP_MAX_VARIABLES 512
#define MCP_MAX_VALUE_ELEMENTS 512

typedef struct mcp_request_t {
    char id[BALTAMATICA_MCP_MAX_ID];
    char method[BALTAMATICA_MCP_MAX_METHOD];
    char code[BALTAMATICA_MCP_MAX_CODE];
    char file_path[BALTAMATICA_MCP_MAX_PATH];
    char name[BALTAMATICA_MCP_MAX_METHOD];
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

static int mcp_validate_no_outputs(int nlhs, bxArray *plhs[]) {
    (void)plhs;
    if (nlhs > 0) {
        bxErrMsgTxt("mcp_bridge does not return output arguments.");
        return -1;
    }
    return 0;
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
    if (mcp_validate_no_outputs(nlhs, plhs) != 0) {
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

static int mcp_array_is_stop_command(const bxArray *value) {
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
    return strcmp(normalized, "stop") == 0;
}

static int mcp_parse_stop_port(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    if (mcp_validate_no_outputs(nlhs, plhs) != 0) {
        return -1;
    }
    if (nrhs > 2) {
        bxErrMsgTxt("mcp_bridge('stop') accepts at most one optional port argument.");
        return -1;
    }
    if (nrhs == 1) {
        return BALTAMATICA_MCP_DEFAULT_PORT;
    }
    return mcp_parse_port_arg(prhs[1]);
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
    } else if (strcmp(request->method, BALTAMATICA_MCP_METHOD_GET_VARIABLE) == 0) {
        if (!mcp_json_get_string(line, "name", request->name, sizeof(request->name))) {
            snprintf(error, error_size, "get_variable requires params.name.");
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
    char command[256];
    snprintf(
        command,
        sizeof(command),
        "fprintf('MCP bridge %s %s %s:%d\\n');",
        phase,
        preposition,
        BALTAMATICA_MCP_DEFAULT_HOST,
        port);
    (void)bxEvalString(command);
}

static void mcp_print_bridge_text(const char *message, int port) {
    char command[256];
    snprintf(
        command,
        sizeof(command),
        "fprintf('MCP bridge %s %s:%d\\n');",
        message,
        BALTAMATICA_MCP_DEFAULT_HOST,
        port);
    (void)bxEvalString(command);
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

static void mcp_array_to_json_value(const bxArray *value, char *buffer, size_t buffer_size) {
    bxClassID class_id = bxGetClassID(value);
    baSize ndim = bxGetNumberOfDimensions(value);
    const baSize *dims = bxGetDimensions(value);
    baSize elements = bxGetNumberOfElements(value);
    baSize emitted = elements < MCP_MAX_VALUE_ELEMENTS ? elements : MCP_MAX_VALUE_ELEMENTS;
    char size_text[128];
    size_t used = 0;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    mcp_format_size(value, size_text, sizeof(size_text));
    mcp_append_json(buffer, buffer_size, &used, "{\"supported\":");

    if (!mcp_structured_value_supported(value)) {
        mcp_append_json(
            buffer,
            buffer_size,
            &used,
            "false,\"type\":\"unsupported\",\"class_name\":\"%s\",\"size\":\"%s\","
            "\"element_count\":%lld,\"reason\":\"Only real numeric and logical arrays are supported.\"}",
            mcp_class_name_from_id(class_id),
            size_text,
            (long long)elements);
        return;
    }

    mcp_append_json(
        buffer,
        buffer_size,
        &used,
        "true,\"type\":\"%s\",\"class_name\":\"%s\",\"size\":\"%s\",\"dims\":[",
        class_id == bxLOGICAL_CLASS ? "logical_array" : "numeric_array",
        mcp_class_name_from_id(class_id),
        size_text);
    if (ndim <= 0 || dims == NULL) {
        mcp_append_json(buffer, buffer_size, &used, "%lld,%lld", (long long)bxGetM(value), (long long)bxGetN(value));
    } else {
        for (baSize i = 0; i < ndim; ++i) {
            mcp_append_json(buffer, buffer_size, &used, "%s%lld", i == 0 ? "" : ",", (long long)dims[i]);
        }
    }
    mcp_append_json(
        buffer,
        buffer_size,
        &used,
        "],\"encoding\":\"column-major\",\"element_count\":%lld,\"truncated\":%s,\"data\":[",
        (long long)elements,
        elements > MCP_MAX_VALUE_ELEMENTS ? "true" : "false");
    for (baSize i = 0; i < emitted; ++i) {
        if (i > 0) {
            mcp_append_json(buffer, buffer_size, &used, ",");
        }
        if (!mcp_append_value_number(value, i, buffer, buffer_size, &used)) {
            mcp_append_json(buffer, buffer_size, &used, "null");
        }
    }
    mcp_append_json(buffer, buffer_size, &used, "]}");
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

static void mcp_handle_request(mcp_socket_t client_fd, const mcp_request_t *request, int *stop_server, int port) {
    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_STATUS) == 0) {
        mcp_send_status(client_fd, request->id, port);
        return;
    }

    if (strcmp(request->method, BALTAMATICA_MCP_METHOD_EXECUTE_CODE) == 0) {
        int status = mcp_eval_command(request->code);
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
        char value_json[MCP_MAX_VALUE_JSON];

        if (!mcp_is_valid_variable_name(request->name)) {
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_BAD_REQUEST, "Invalid variable name.");
            return;
        }
        if (mcp_lookup_variable(request->name, &value) != 0 || value == NULL) {
            mcp_send_error(client_fd, request->id, BALTAMATICA_MCP_ERROR_VARIABLE, "Variable lookup failed.");
            return;
        }
        mcp_array_to_output(value, output, sizeof(output));
        mcp_array_to_json_value(value, value_json, sizeof(value_json));
        bxDestroyArray(value);
        mcp_send_variable_value(client_fd, request->id, output, value_json);
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

static void mcp_handle_client(mcp_socket_t client_fd, int *stop_server, int port) {
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
            mcp_handle_request(client_fd, &request, stop_server, port);
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

    mcp_print_bridge_message("listening", "on", port);

    while (!stop_server) {
        mcp_socket_t client_fd = accept(server_fd, NULL, NULL);
        if (!mcp_socket_valid(client_fd)) {
            continue;
        }
        mcp_handle_client(client_fd, &stop_server, port);
        mcp_close_socket(client_fd);
    }

    mcp_close_socket(server_fd);
    mcp_socket_cleanup();
    mcp_print_bridge_message("stopped", "on", port);
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

void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[]) {
    int port;
    if (nrhs > 0 && mcp_array_is_stop_command(prhs[0])) {
        int status;
        port = mcp_parse_stop_port(nlhs, plhs, nrhs, prhs);
        if (port < 0) {
            return;
        }
        status = mcp_send_shutdown_request(port);
        if (status == 0) {
            mcp_print_bridge_text("stop requested on", port);
        } else if (status == 1) {
            mcp_print_bridge_text("is not listening on", port);
        } else {
            mcp_print_bridge_text("stop request failed on", port);
        }
        return;
    }

    port = mcp_parse_port(nlhs, plhs, nrhs, prhs);
    if (port < 0) {
        return;
    }
    mcp_print_bridge_message("ready", "at", port);
    (void)mcp_listen_loop(port);
}
