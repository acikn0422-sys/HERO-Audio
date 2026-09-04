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

流式 hop benchmark 先在计时区间外把 WAV 解码成 mono float32，再严格按 256 samples 调用一次
`StreamingProcessor::push_hop()`。单个稳态计时边界是：

```text
steady_clock start
→ 256 个新采样写入环形缓冲区
→ 重建最近 1024 samples 并应用 Hann
→ R2C FFT → magnitude → positive Spectral Flux
→ causal threshold / peak confirmation
→ 可选 onset event 返回
→ steady_clock stop
```

第一个完整 FFT 窗口需要 4 个 hop。最初 3 次只填充缓冲区、不产生分析帧，因此不进入稳态分位数；
第 4 次调用生成 frame 0，并进入统计。WAV 解码、backend/plan 初始化、raw CSV/summary JSON 写入、
音频设备、CoreAudio、操作系统缓冲和驱动均不属于 hop compute。文件末尾不足 256 samples 的尾部不
补零，数量必须写入 `ignored_tail_sample_count`。

每次正式流式 benchmark 至少运行 1 次 warm-up pass 和 5 次 measured passes，保留每个稳态 hop 的
原始时间。汇总仍使用 Type-7 的 P50、P95、P99，并同时报告 maximum、deadline miss 数与
`P95 < hop_period` 的布尔结果。测试/CI 不硬编码性能通过，因为共享 runner 不能代表正式 Apple
Silicon 实验机。

完成计时后，程序在计时区间外调用原有离线 Spectral Flux 和 causal detector。流式与离线必须拥有
相同 frame/onset 数、相同时间语义，flux 在固定绝对/相对浮点容差内相等，onset 事件也必须相等。
这项检查证明新增状态机没有改变算法；它不是检测准确率评价，后者仍需人工标注与一对一匹配。

## CoreAudio live 指标

live 模式使用当前 macOS 默认输入设备及其原生整数采样率。AUHAL 把设备数据转换为 mono float32，
callback 每累积 256 samples 形成一个 hop。callback 只允许执行预分配 `AudioUnitRender`、样本复制、
聚合、原生无锁 atomic 计数和非阻塞 SPSC 入队；不得执行 FFT、日志、文件 I/O、互斥锁或内存申请。

队列固定保存 64 个 hop。生产者是 CoreAudio callback，消费者是主分析线程。队列满时 callback 丢弃
当前完整 hop 并增加 `dropped_hop_count`，绝不等待消费者。消费者通过 sequence 与绝对 sample index
发现断点，执行以下恢复：

1. 在保存的 WAV 时间轴插入等长静音；
2. 重置 Spectral Flux 和 causal detector 历史；
3. 从下一项连续数据建立新 segment；
4. 不跨断点产生 onset。

正式 live session 要求 `dropped_hop_count=0` 且 `render_error_count=0`，否则
`capture_integrity_pass=false`，该 session 不得用于正式准确率或延迟结论。

live 模式分列三种时间：

| 字段 | 定义 |
|---|---|
| `compute_ms` | `StreamingProcessor::push_hop()` 调用时间 |
| `callback_to_consumer_ms` | CoreAudio callback 入口到消费者开始处理 |
| `estimated_software_detection_delay_ms` | 算法结构延迟 + callback 到消费者 + 当前 hop compute |

第三项仍不包含声音进入麦克风到 CoreAudio callback 之前的硬件、设备缓冲和驱动延迟，因此不能称为
完整声学端到端延迟。设备端到端测量需要外部声源/回环和共同时间基准，必须使用新的指标。

live P95 hop compute 仍必须严格小于实际 `256 / sample_rate` 周期。检测到至少一个人工可确认事件时，
报告 estimated software detection delay 的 P95，并与 30 ms 初始目标比较；没有事件时该字段为 JSON
`null`，不得人为记为通过。

整段文件同时报告 median、P95、P99 和 real-time factor。正式实现把 FFT backend/plan 创建放在
计时区间外并单独报告 `backend_initialization_ms`。每个 steady-state run 的边界是：

```text
steady_clock start
→ read_wav
→ mono float32 / frame / Hann / FFT / Spectral Flux
→ causal threshold / peak picking
→ 完整 onset CSV 序列化到内存输出流
→ steady_clock stop
```

CSV 序列化计入主指标，但保存 benchmark raw CSV/summary JSON 的文件系统写入不计入，避免磁盘型号和
缓存状态混入 CPU 算法对比。若以后研究真实存储端到端延迟，必须使用不同指标名和 schema version。

所有分位数只从独立 measured runs 计算；warm-up 和初始化样本不得进入 steady-state 统计。每组至少
5 次 measured runs。当前使用 Type-7 线性插值：排序后令 `rank=p×(N-1)`，在相邻样本间插值。

```text
RTF = audio_duration_seconds / (elapsed_ms / 1000)
```

RTF 大于 1 表示快于实时；数值越大，单位墙钟时间处理的音频越多。raw CSV 必须保留每次 elapsed、
RTF、音频时长、frame 数、onset 数和序列化字节数，不能只保存汇总值。

## Onset 一对一匹配

预测与标注先分别按时间排序。只有绝对误差不超过 50 ms 的配对才是候选边。匹配必须：

1. 首先最大化一对一匹配数量；
2. 匹配数相同时，最小化总绝对时间误差；
3. 未匹配预测为 FP，未匹配标注为 FN。

正式实现使用确定性的动态规划或等价的最优二分图匹配，不使用依赖输入顺序的贪心匹配。

当前实现使用排序后的一维序列动态规划，时间和空间复杂度均为 `O(NM)`。输出中的索引仍指向排序前
原始输入位置。若 Precision、Recall 或 F1 的分母为零，对应指标保守返回 0；静音文件不会因为
“没有标注且没有预测”而获得人为的满分。正式数据集结论优先报告汇总 TP/FP/FN 后计算的 micro
指标，并把 per-file macro 指标单独标记。

## 分帧与 Spectral Flux 时间语义

离线基线从 `frame_index × hop_size` 开始取长度为 `frame_size` 的完整帧。文件末尾不足一帧的样本
不做隐式补零；若实验需要补零，必须使用新的版本化配置并单独标记。默认 Hann window 为长度 1024
的对称形式，Spectral Flux 为当前帧与前一帧各频点 magnitude 正向差值之和，第一帧固定为零。

每个通量样本同时保存三个时间：

- `frame_start_seconds`：窗口第一个采样的时间；
- `frame_center_seconds`：窗口中心对应时间，用作候选 onset 的初始时间参考；
- `available_seconds`：完整窗口最后一个采样到达后的可计算时间边界。

检测延迟计算不得用 center time 替代 available time；两者之差体现约半帧的算法缓冲下限。

## Causal threshold 与 peak picking

帧 `t` 的阈值只使用此前最多 16 帧 `[max(0,t-16), t)`，不包含当前帧：

```text
threshold[t] = mean(history) + 1.5 × population_stddev(history)
```

当前 flux 必须严格大于 threshold 和 minimum flux，并且不小于左邻帧。候选帧等待一个右邻帧；
只有候选 flux 严格大于右邻帧才确认。若右邻 flux 相等，同一候选沿平台向后移动并保留首次跨越
阈值时的 threshold，因此平台峰选择最后一帧。确认至少增加一个 hop 的 lookahead；长度超过一帧的
平台会增加相应等待时间。
30 ms refractory 内使用“先确认峰优先”，不等待未来更强峰；这种策略保持低延迟且可真正流式执行。

onset time 使用候选帧 center，emitted time 使用右邻确认帧 available。算法延迟为两者之差；在
48 kHz、frame 1024、hop 256 下，固定结构延迟为约 16 ms（半帧 10.67 ms 加一 hop 5.33 ms）。

逐帧 diagnostics CSV 使用检测器同一次状态转移产生，不允许在 Python 中重新估算阈值。首帧因为没有
历史数据，`causal_threshold` 留空；后续行记录 threshold、是否越阈值、是否仍有待确认峰，以及该帧
到达时发出的 onset signal time。绘图只可视化这些数据，不参与检测决策。

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
