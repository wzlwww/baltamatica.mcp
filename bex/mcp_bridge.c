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
 * small: execute_code, run_script, clear_workspace, list_variables, and
 * get_variable are implemented with Baltamatica SDK APIs. get_variable also
 * includes a structured JSON value for small real numeric/logical arrays.
 * Binary matrix transfer remains a later PR.
 *
 * The server loop intentionally runs on the BEX invocation thread. Calling
 * Baltamatica interpreter APIs from a background pthread is not reliable across
 * runtimes, while the invocation thread can evaluate commands and open GUI
 * figures.
 */

#include "bex/bex.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 31415
#define MAX_REQUEST_SIZE 65536
#define MAX_FIELD_SIZE 32768
#define MAX_RESPONSE_SIZE 65536
#define MAX_COMMAND_SIZE 65536
#define MAX_VARIABLES 512
#define ARRAY_LINE_WIDTH 120
#define MAX_VALUE_ELEMENTS 512

static int g_stop_requested = 0;
static int g_bridge_port = BRIDGE_PORT;

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

static bool is_valid_variable_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
        return false;
    }
    for (size_t i = 1; name[i] != '\0'; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }
    return true;
}

static int lookup_variable(const char *name, bxArray **value)
{
    if (!is_valid_variable_name(name)) {
        return 1;
    }
    *value = NULL;
    return bxEvalIn("base", name, value);
}

static void format_size(const bxArray *value, char *buffer, size_t buffer_size)
{
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

static long long estimate_bytes(const bxArray *value)
{
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

static void array_to_output(const bxArray *value, char *buffer, size_t buffer_size)
{
    baSize required;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    required = bxArrayToCStr(value, ARRAY_LINE_WIDTH, 0, NULL, 0) + 1;
    if (required <= 1) {
        return;
    }

    bxArrayToCStr(value, ARRAY_LINE_WIDTH, 1, buffer, (baSize)buffer_size);
    buffer[buffer_size - 1] = '\0';

    if ((size_t)required > buffer_size) {
        const char *suffix = "\n... output truncated ...";
        size_t suffix_len = strlen(suffix);
        if (buffer_size > suffix_len + 1) {
            memcpy(buffer + buffer_size - suffix_len - 1, suffix, suffix_len + 1);
        }
    }
}

static void append_json(char *buffer, size_t buffer_size, size_t *used, const char *fmt, ...)
{
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

static const char *class_name_from_id(bxClassID class_id)
{
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
    case bxTABLE_CLASS:
        return "table";
    case bxTIMETABLE_CLASS:
        return "timetable";
    case bxDATETIME_CLASS:
        return "datetime";
    case bxDURATION_CLASS:
        return "duration";
    case bxCALENDAR_DURATION_CLASS:
        return "calendar_duration";
    case bxCATEGORICAL_CLASS:
        return "categorical";
    default:
        return "unknown";
    }
}

static bool append_value_number(const bxArray *value, baSize index, char *buffer, size_t buffer_size, size_t *used)
{
    bxClassID class_id = bxGetClassID(value);

    switch (class_id) {
    case bxDOUBLE_CLASS: {
        const double *data = bxGetDoublesRO(value);
        if (data == NULL) {
            return false;
        }
        if (isfinite(data[index])) {
            append_json(buffer, buffer_size, used, "%.17g", data[index]);
        } else {
            append_json(buffer, buffer_size, used, "null");
        }
        return true;
    }
    case bxSINGLE_CLASS: {
        const float *data = bxGetSinglesRO(value);
        if (data == NULL) {
            return false;
        }
        if (isfinite((double)data[index])) {
            append_json(buffer, buffer_size, used, "%.9g", (double)data[index]);
        } else {
            append_json(buffer, buffer_size, used, "null");
        }
        return true;
    }
    case bxINT8_CLASS: {
        const int8_t *data = bxGetInt8sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%d", (int)data[index]);
        return true;
    }
    case bxINT16_CLASS: {
        const int16_t *data = bxGetInt16sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%d", (int)data[index]);
        return true;
    }
    case bxINT32_CLASS: {
        const int32_t *data = bxGetInt32sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%d", data[index]);
        return true;
    }
    case bxINT64_CLASS: {
        const int64_t *data = bxGetInt64sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%lld", (long long)data[index]);
        return true;
    }
    case bxUINT8_CLASS: {
        const uint8_t *data = bxGetUInt8sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%u", (unsigned int)data[index]);
        return true;
    }
    case bxUINT16_CLASS: {
        const uint16_t *data = bxGetUInt16sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%u", (unsigned int)data[index]);
        return true;
    }
    case bxUINT32_CLASS: {
        const uint32_t *data = bxGetUInt32sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%u", data[index]);
        return true;
    }
    case bxUINT64_CLASS: {
        const uint64_t *data = bxGetUInt64sRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%llu", (unsigned long long)data[index]);
        return true;
    }
    case bxLOGICAL_CLASS: {
        const bool *data = bxGetLogicalsRO(value);
        if (data == NULL) {
            return false;
        }
        append_json(buffer, buffer_size, used, "%s", data[index] ? "true" : "false");
        return true;
    }
    default:
        return false;
    }
}

static bool is_structured_value_supported(const bxArray *value)
{
    bxClassID class_id = bxGetClassID(value);

    if (bxIsComplex(value)) {
        return false;
    }

    switch (class_id) {
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
        return true;
    default:
        return false;
    }
}

static void array_to_json_value(const bxArray *value, char *buffer, size_t buffer_size)
{
    bxClassID class_id = bxGetClassID(value);
    baSize ndim = bxGetNumberOfDimensions(value);
    const baSize *dims = bxGetDimensions(value);
    baSize elements = bxGetNumberOfElements(value);
    baSize emitted = elements < MAX_VALUE_ELEMENTS ? elements : MAX_VALUE_ELEMENTS;
    char size_text[128];
    char escaped_class[256];
    size_t used = 0;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';

    format_size(value, size_text, sizeof(size_text));
    json_escape(class_name_from_id(class_id), escaped_class, sizeof(escaped_class));

    append_json(buffer, buffer_size, &used, "{\"supported\":");

    if (!is_structured_value_supported(value)) {
        append_json(
            buffer,
            buffer_size,
            &used,
            "false,\"type\":\"unsupported\",\"class_name\":\"%s\",\"size\":\"%s\","
            "\"element_count\":%lld,\"reason\":\"Only real numeric and logical arrays are supported.\"}",
            escaped_class,
            size_text,
            (long long)elements);
        return;
    }

    append_json(
        buffer,
        buffer_size,
        &used,
        "true,\"type\":\"%s\",\"class_name\":\"%s\",\"size\":\"%s\",\"dims\":[",
        class_id == bxLOGICAL_CLASS ? "logical_array" : "numeric_array",
        escaped_class,
        size_text);

    if (ndim <= 0 || dims == NULL) {
        append_json(buffer, buffer_size, &used, "%lld,%lld", (long long)bxGetM(value), (long long)bxGetN(value));
    } else {
        for (baSize i = 0; i < ndim; ++i) {
            append_json(buffer, buffer_size, &used, "%s%lld", i == 0 ? "" : ",", (long long)dims[i]);
        }
    }

    append_json(
        buffer,
        buffer_size,
        &used,
        "],\"encoding\":\"column-major\",\"element_count\":%lld,\"truncated\":%s,"
        "\"data\":[",
        (long long)elements,
        elements > MAX_VALUE_ELEMENTS ? "true" : "false");

    for (baSize i = 0; i < emitted; ++i) {
        if (i > 0) {
            append_json(buffer, buffer_size, &used, ",");
        }
        if (!append_value_number(value, i, buffer, buffer_size, &used)) {
            append_json(buffer, buffer_size, &used, "null");
        }
    }

    append_json(buffer, buffer_size, &used, "]}");
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

static void make_variable_list_response(
    const char *id,
    const char **names,
    int name_count,
    char *response,
    size_t response_size)
{
    char escaped_id[256];
    size_t used;
    bool first_item = true;

    json_escape(id, escaped_id, sizeof(escaped_id));
    used = (size_t)snprintf(
        response,
        response_size,
        "{\"id\":\"%s\",\"success\":true,\"output\":\"\",\"variables\":[",
        escaped_id);

    for (int i = 0; i < name_count && i < MAX_VARIABLES && used + 1 < response_size; ++i) {
        bxArray *value = NULL;
        char size_text[128];
        char escaped_name[256];
        char escaped_size[256];
        char escaped_class[256];
        const char *class_name = "";
        int status = lookup_variable(names[i], &value);

        if (status != 0 || value == NULL) {
            continue;
        }

        format_size(value, size_text, sizeof(size_text));
        class_name = bxTypeCStr(value);
        json_escape(names[i], escaped_name, sizeof(escaped_name));
        json_escape(size_text, escaped_size, sizeof(escaped_size));
        json_escape(class_name ? class_name : "", escaped_class, sizeof(escaped_class));

        int written = snprintf(
            response + used,
            used < response_size ? response_size - used : 0,
            "%s{\"name\":\"%s\",\"size\":\"%s\",\"bytes\":%lld,\"class_name\":\"%s\",\"attributes\":\"\"}",
            first_item ? "" : ",",
            escaped_name,
            escaped_size,
            estimate_bytes(value),
            escaped_class);
        bxDestroyArray(value);
        if (written < 0) {
            break;
        }
        first_item = false;
        if (used + (size_t)written >= response_size) {
            used = response_size - 1;
            break;
        }
        used += (size_t)written;
    }

    snprintf(response + used, used < response_size ? response_size - used : 0, "]}\n");
}

static void make_variable_value_response(
    const char *id,
    const char *output,
    const char *value_json,
    char *response,
    size_t response_size)
{
    char escaped_id[256];
    char escaped_output[MAX_FIELD_SIZE];

    json_escape(id, escaped_id, sizeof(escaped_id));
    json_escape(output, escaped_output, sizeof(escaped_output));
    snprintf(
        response,
        response_size,
        "{\"id\":\"%s\",\"success\":true,\"output\":\"%s\",\"value\":%s,\"artifacts\":[]}\n",
        escaped_id,
        escaped_output,
        value_json ? value_json : "null");
}

static int eval_command(const char *command)
{
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

static void process_request(const char *request, char *response, size_t response_size)
{
    char id[256];
    char method[128];
    char code[MAX_FIELD_SIZE];
    char file_path[MAX_FIELD_SIZE];
    char name[MAX_FIELD_SIZE];
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

    if (strcmp(method, "status") == 0) {
        char escaped_id[256];
        json_escape(id, escaped_id, sizeof(escaped_id));
        snprintf(
            response,
            response_size,
            "{\"id\":\"%s\",\"success\":true,\"output\":\"MCP bridge ready\","
            "\"host\":\"%s\",\"port\":%d,\"artifacts\":[]}\n",
            escaped_id,
            BRIDGE_HOST,
            g_bridge_port);
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

    if (strcmp(method, "list_variables") == 0) {
        const char **names = NULL;
        int name_count = 0;

        bxGetVariableNames(&names, &name_count);
        make_variable_list_response(id, names, name_count, response, response_size);
        if (names != NULL) {
            bxFreeVariableNames(names);
        }
        return;
    }

    if (strcmp(method, "get_variable") == 0) {
        bxArray *value = NULL;
        char output[MAX_FIELD_SIZE];
        char value_json[MAX_FIELD_SIZE];

        if (!extract_json_string(request, "name", name, sizeof(name))) {
            make_response(id, false, "", "BAD_REQUEST", "Missing params.name.", response, response_size);
            return;
        }
        if (!is_valid_variable_name(name)) {
            make_response(id, false, "", "BAD_REQUEST", "Invalid variable name.", response, response_size);
            return;
        }

        status = lookup_variable(name, &value);
        if (status != 0 || value == NULL) {
            make_response(id, false, "", "VARIABLE_ERROR", "Variable lookup failed.", response, response_size);
            return;
        }

        array_to_output(value, output, sizeof(output));
        array_to_json_value(value, value_json, sizeof(value_json));
        bxDestroyArray(value);
        make_variable_value_response(id, output, value_json, response, response_size);
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

static int server_loop(int port)
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
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        bxPrintf("MCP bridge failed to bind %s:%d: %d\n", BRIDGE_HOST, port, errno);
        close(server_fd);
        return errno;
    }
    if (listen(server_fd, 8) != 0) {
        bxPrintf("MCP bridge failed to listen: %d\n", errno);
        close(server_fd);
        return errno;
    }

    bxPrintf("MCP bridge listening on %s:%d\n", BRIDGE_HOST, port);

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
    int port = BRIDGE_PORT;

    if (nrhs > 0) {
        int err = 0;
        int requested_port = bxAsInt(prhs[0], &err);
        if (err == 0 && requested_port > 0 && requested_port <= 65535) {
            port = requested_port;
        } else {
            bxPrintf("Invalid MCP bridge port; using default %d\n", BRIDGE_PORT);
        }
    }

    g_stop_requested = 0;
    g_bridge_port = port;
    bxPrintf("MCP bridge ready at %s:%d\n", BRIDGE_HOST, port);
    status = server_loop(port);

    if (nlhs > 0) {
        plhs[0] = bxCreateDoubleScalar((double)status);
    }
}
