# HERO-Audio benchmark protocol v0.1

本文件锁定 v0.1 的指标语义。修改指标时必须提升配置的 `schema_version`，不得在结果产生后静默修改定义。

## 三类延迟

| 指标 | 测量区间 | 主要用途 |
|---|---|---|
| 整段文件处理时间 | 开始读取 WAV 到所有 onset 写出完成 | 离线吞吐量与 RTF |
| 单 hop 计算时间 | 256 个新采样可用到该 hop 计算完成 | 判断流式处理是否积压 |
| 流式检测延迟 | 真实 onset 时刻到检测事件发出 | 实时体验 |

在 48 kHz、hop size 256 下，hop 周期是 `256 / 48000 = 5.333 ms`，因此正式约束为
`P95(hop compute) < 5.333 ms`。流式检测使用只依赖当前及历史帧的 causal threshold，初始目标为
`P95(detection delay) < 30 ms`。音频设备、操作系统和驱动延迟单独报告，不计入算法延迟。

整段文件同时报告 median、P95、P99、吞吐量和 real-time factor。所有分位数从独立 measured runs
计算；warm-up 和初始化样本不得进入 steady-state 统计。

## Onset 一对一匹配

预测与标注先分别按时间排序。只有绝对误差不超过 50 ms 的配对才是候选边。匹配必须：

1. 首先最大化一对一匹配数量；
2. 匹配数相同时，最小化总绝对时间误差；
3. 未匹配预测为 FP，未匹配标注为 FN。

正式实现使用确定性的动态规划或等价的最优二分图匹配，不使用依赖输入顺序的贪心匹配。

## FFT 公平性

- 正式 CPU baseline：FFTW3 单精度 `fftw3f`；
- 正确性对照：项目内 radix-2 reference backend，不纳入正式性能结论；
- 后续 GPU baseline：NVIDIA cuFFT；
- Apple Accelerate/vDSP 仅作为 Apple 平台的可选对照。

所有后端统一使用 float32、1024-point R2C、相同 Hann window、batch size、输入和未归一化的
forward-transform 输出约定。FFTW plan、cuFFT plan、模块加载与 JIT 初始化分别计时，不纳入
steady-state 执行时间。GPU 必须同时报告 kernel-only 与包含 CPU/GPU 传输的端到端时间。

## 最低可复现记录

每次正式运行至少保存：Git commit、完整配置、编译器与版本、CMake preset、操作系统、芯片型号、
逻辑/物理核心数、内存、FFT 后端及版本、线程数、电源模式、原始逐次测量数据和 UTC 时间戳。
