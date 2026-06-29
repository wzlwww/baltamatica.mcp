# 蒙特卡洛方法估算 Pi
# 用法: 在北太天元中运行此脚本

fprintf('\n--- Monte Carlo Pi Estimation ---\n');

N = 1e6;
tic;
x = rand(N, 1);
y = rand(N, 1);
inside = sum(x.^2 + y.^2 <= 1);
pi_est = 4 * inside / N;
elapsed = toc;

fprintf('Samples:      %1.0e\n', N);
fprintf('Estimated Pi: %1.6f\n', pi_est);
fprintf('Error:        %1.6f\n', abs(pi_est - pi));
fprintf('Time:         %1.4f sec\n', elapsed);
