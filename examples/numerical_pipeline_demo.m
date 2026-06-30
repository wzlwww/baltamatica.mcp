% numerical_pipeline_demo.m
% A compact numerical workflow for exercising the Baltamatica MCP CLI backend.

fprintf('\n=== Baltamatica MCP Numerical Pipeline Demo ===\n');

rng(2026);

% Build a symmetric positive-definite system with a known solution.
n = 6;
M = rand(n, n);
A = M' * M + eye(n) * 0.5;
x_true = (1:n)';
b = A * x_true;
x_solved = A \ b;
residual_norm = norm(A * x_solved - b);

fprintf('\n[1] Linear system solve\n');
fprintf('Residual norm: %.6e\n', residual_norm);
fprintf('Solution error: %.6e\n', norm(x_solved - x_true));

% Estimate the dominant eigenvalue by power iteration.
v = ones(n, 1);
lambda_history = zeros(12, 1);
for k = 1:12
    v = A * v;
    v = v / norm(v);
    lambda_history(k) = (v' * A * v) / (v' * v);
end
dominant_lambda = lambda_history(end);

fprintf('\n[2] Power iteration\n');
fprintf('Dominant eigenvalue estimate: %.6f\n', dominant_lambda);
fprintf('Last three estimates: %.6f %.6f %.6f\n', lambda_history(10), lambda_history(11), lambda_history(12));

% Monte Carlo estimate of E[exp(-X^2)] for X ~ U(0, 1).
samples = 50000;
u = rand(samples, 1);
mc_values = exp(-(u .^ 2));
mc_mean = mean(mc_values);
mc_std_error = std(mc_values) / sqrt(samples);

fprintf('\n[3] Monte Carlo estimate\n');
fprintf('E[exp(-X^2)], X~U(0,1): %.6f +/- %.6f\n', mc_mean, 1.96 * mc_std_error);

% Aggregate results into a compact report matrix for later inspection.
report = [
    residual_norm, norm(x_solved - x_true);
    dominant_lambda, lambda_history(12) - lambda_history(11);
    mc_mean, mc_std_error
];

fprintf('\n[4] Report matrix\n');
disp(report);

fprintf('Demo complete. Variables left in workspace: A, x_solved, lambda_history, report, mc_mean.\n');
