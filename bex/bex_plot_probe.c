/*
 * bex_plot_probe.c - BEX probe for Baltamatica plotting support.
 *
 * Compile from the repository root:
 *   /Applications/Baltamatica.app/Contents/MacOS/bex bex/bex_plot_probe.c
 *
 * Then call from Baltamatica:
 *   addpath("/path/to/baltamatica.mcp")
 *   bex_plot_probe
 *
 * The probe calls Baltamatica functions through the BEX SDK and writes a small
 * status file for manual verification. It intentionally does not export or
 * persist the figure; PR6 only verifies that a GUI-loaded BEX can open a plot.
 */

#include "bex/bex.h"

#include <stdio.h>

#define PROBE_STATUS_FILE "/tmp/baltamatica_mcp_bex_plot_probe.txt"
#define PROBE_EXPR_BUFFER_SIZE 4096

static const char *DEFAULT_PLOT_EXPR =
    "x = 0:0.1:6.28; "
    "y = sin(x); "
    "figure; "
    "plot(x, y); "
    "title('BEX plot probe'); "
    "drawnow;";

static void write_legacy_probe_status(int status, const char *expr)
{
    FILE *fp = fopen(PROBE_STATUS_FILE, "w");
    if (fp == NULL) {
        bxPrintf("BEX plot probe could not write status file: %s\n", PROBE_STATUS_FILE);
        return;
    }

    fprintf(fp, "status=%d\n", status);
    fprintf(fp, "status_file=%s\n", PROBE_STATUS_FILE);
    fprintf(fp, "expr=%s\n", expr);
    fclose(fp);
}

static int run_call_sin_probe(void)
{
    bxArray *input = bxCreateDoubleScalar(1.0);
    bxArray *outputs[1] = {0};
    const bxArray *inputs[1] = {input};
    int status = bxCallBaltamatica(1, outputs, 1, inputs, "sin");

    if (outputs[0] != 0) {
        bxDestroyArray(outputs[0]);
    }
    bxDestroyArray(input);
    return status;
}

static int run_call_plot_probe(void)
{
    bxArray *x = bxCreateDoubleMatrix(1, 3, bxREAL);
    bxArray *y = bxCreateDoubleMatrix(1, 3, bxREAL);
    double *x_data = bxGetDoublesRW(x);
    double *y_data = bxGetDoublesRW(y);
    const bxArray *inputs[2] = {x, y};
    int status;

    x_data[0] = 1.0;
    x_data[1] = 2.0;
    x_data[2] = 3.0;
    y_data[0] = 1.0;
    y_data[1] = 4.0;
    y_data[2] = 9.0;

    status = bxCallBaltamatica(0, 0, 2, inputs, "plot");

    bxDestroyArray(x);
    bxDestroyArray(y);
    return status;
}

static int run_suite_probe(void)
{
    FILE *fp = fopen(PROBE_STATUS_FILE, "w");
    bxArray *evalin_output = 0;
    int eval_expression_status = bxEvalString("1+1;");
    int eval_assignment_status = bxEvalString("bex_probe_value=42;");
    int evalin_expression_status = bxEvalIn("base", "[1 2 3]", &evalin_output);
    int call_sin_status = run_call_sin_probe();
    int call_plot_status = run_call_plot_probe();
    int core_status = eval_expression_status || evalin_expression_status || call_sin_status;

    if (evalin_output != 0) {
        bxDestroyArray(evalin_output);
    }

    if (fp == NULL) {
        bxPrintf("BEX plot probe could not write status file: %s\n", PROBE_STATUS_FILE);
        return call_plot_status;
    }

    fprintf(fp, "status_file=%s\n", PROBE_STATUS_FILE);
    fprintf(fp, "eval_expression=%d\n", eval_expression_status);
    fprintf(fp, "eval_assignment=%d\n", eval_assignment_status);
    fprintf(fp, "evalin_expression=%d\n", evalin_expression_status);
    fprintf(fp, "call_sin=%d\n", call_sin_status);
    fprintf(fp, "call_plot=%d\n", call_plot_status);
    fprintf(fp, "core=%d\n", core_status);
    fprintf(fp, "plot=%d\n", call_plot_status);
    fclose(fp);

    bxPrintf("BEX_PLOT_PROBE_EVAL_EXPRESSION=%d\n", eval_expression_status);
    bxPrintf("BEX_PLOT_PROBE_EVAL_ASSIGNMENT=%d\n", eval_assignment_status);
    bxPrintf("BEX_PLOT_PROBE_EVALIN_EXPRESSION=%d\n", evalin_expression_status);
    bxPrintf("BEX_PLOT_PROBE_CALL_SIN=%d\n", call_sin_status);
    bxPrintf("BEX_PLOT_PROBE_CALL_PLOT=%d\n", call_plot_status);
    return call_plot_status;
}

static const char *probe_expression_from_args(int nrhs, const bxArray *prhs[], char *buffer)
{
    if (bxAsCStr(prhs[0], buffer, PROBE_EXPR_BUFFER_SIZE) != 0) {
        bxErrMsgTxt("bex_plot_probe expects an optional string plotting expression.");
        return DEFAULT_PLOT_EXPR;
    }

    return buffer;
}

BEX_EXPORT void bexFunction(int nlhs, bxArray *plhs[], int nrhs, const bxArray *prhs[])
{
    char expr_buffer[PROBE_EXPR_BUFFER_SIZE];
    const char *expr;
    int status;

    if (nrhs == 0) {
        status = run_suite_probe();
    } else {
        expr = probe_expression_from_args(nrhs, prhs, expr_buffer);
        status = bxEvalString(expr);
        write_legacy_probe_status(status, expr);
    }

    bxPrintf("BEX_PLOT_PROBE_STATUS=%d\n", status);
    bxPrintf("BEX_PLOT_PROBE_STATUS_FILE=%s\n", PROBE_STATUS_FILE);

    if (nlhs > 0) {
        plhs[0] = bxCreateDoubleScalar((double)status);
    }
}
