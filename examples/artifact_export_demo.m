% artifact_export_demo.m
% Demonstrates file artifact reporting for the MCP CLI backend.

output_path = fullfile(pwd(), 'baltamatica_mcp_wave.csv');

t = linspace(0, 2 * pi, 64)';
signal = [t, sin(t), cos(t)];

fid = fopen(output_path, 'w');
if fid < 0
    error('Unable to open artifact output file: %s', output_path);
end
fprintf(fid, 't,sin_t,cos_t\n');
for k = 1:size(signal, 1)
    fprintf(fid, '%.8f,%.8f,%.8f\n', signal(k, 1), signal(k, 2), signal(k, 3));
end
fclose(fid);

fprintf('Exported waveform table: %s\n', output_path);
fprintf('BALTAMATICA_ARTIFACT=text/csv:%s\n', output_path);
