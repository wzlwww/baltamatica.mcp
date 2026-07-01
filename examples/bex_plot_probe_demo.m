% BEX plot probe demo.
%
% Compile first from the repository root:
%   /Applications/Baltamatica.app/Contents/MacOS/bex bex/bex_plot_probe.c
%
% Then run this script from Baltamatica with the repository root on path.

status_file = "/tmp/baltamatica_mcp_bex_plot_probe.txt";

status = bex_plot_probe();
fprintf("BEX plot probe status: %d\n", status);
fprintf("BEX plot probe status file: %s\n", status_file);
