# StreamingProcessor：完整实现与指标说明

这一阶段把原来的“整段 WAV 一次处理完”改造成可持续接收新采样的状态机，但输入仍由 WAV 重放，
还没有连接麦克风。目的有两个：证明改成流式后算法结果没有变化；测量 CPU 能否在下一个 hop 到达前
完成当前 hop 的计算。

## 1. 实际数据流

```text
WAV 在计时外解码为 mono float32
              │
              v
每次取 256 samples（5.333 ms @ 48 kHz）
              │
              v
       StreamingProcessor
       ├── 写入 1024-sample 环形缓冲
       ├── 按时间顺序重建最近窗口
       ├── Hann window
       ├── FFTW3f 1024-point R2C
       ├── magnitude positive difference
       └── causal threshold + peak confirmation
              │
              ├── SpectralFluxFrame（每个稳态 hop 一个）
              └── OnsetEvent（检测到峰时才有）
```

frame size 1024 是观察窗口，hop size 256 是每次新到达的数据量。两者不能混淆。第 4 个 hop 到达时
刚好累积 1024 samples，产生 frame 0；此后每来一个 hop，窗口向前移动 256 samples。

## 2. 核心 C++ 文件

- `include/hero_audio/streaming_processor.hpp`：状态机配置、单次调用结果与公共接口；
- `src/dsp/streaming_processor.cpp`：环形缓冲、Hann、FFT、flux 与 causal detector 串接；
- `include/hero_audio/streaming_benchmark.hpp`：raw measurement、summary 与 consistency 数据结构；
- `src/benchmark/streaming_benchmark.cpp`：warm-up、逐 hop 计时、分位数和离线核对；
- `src/streaming_main.cpp`：`hero-audio-stream` 命令行入口；
- `tests/test_streaming_processor.cpp`：启动、异常输入、reset、一致性、尾部与序列化测试。

状态机的核心调用只有一行：

```cpp
const auto result = processor.push_hop(next_256_mono_float32_samples);
```

`push_hop()` 要求输入长度恰好为 256，且所有采样为有限值。长度错误或 NaN/Infinity 会在写入环形
缓冲前被拒绝，因此失败调用不会破坏后续流状态。`reset()` 清空采样、频谱历史、causal detector 和
所有索引，让同一对象可以安全重放下一次 benchmark pass。

## 3. 为什么流式和离线结果应一致

离线 frame `i` 读取：

```text
samples[i × 256 ... i × 256 + 1023]
```

流式环形缓冲在每次插入后让 `write_position` 指向最旧采样，从该位置读取 1024 个值，得到完全相同
的时间顺序。两条路径随后使用相同 Hann、同一个 FFTBackend 约定、相同 magnitude 差、相同时间戳
公式和相同 CausalOnsetDetector。

benchmark 在所有计时结束后用离线函数重算一次，并核对：

- frame 数量与索引；
- start/center/available 时间；
- 每帧 Spectral Flux（固定绝对与相对浮点容差）；
- onset 数量、时间、发出时刻、算法延迟、flux 和 threshold。

`overall_matches=true` 才表示实现等价。CLI 在不一致时返回退出码 3，防止悄悄生成貌似正常的性能
结论。它只证明两种执行方式等价，不证明检测结果符合人工标注；Precision/Recall/F1 仍由独立评价
程序计算。

## 4. hop compute 的准确边界

每个被统计的稳态样本是：

```cpp
start = steady_clock::now();
result = processor.push_hop(hop_samples);
end = steady_clock::now();
```

它包含采样插入、窗口重建、Hann、FFT、Spectral Flux 和 causal detection。它不包含 WAV 解码、
FFTW plan 创建、结果文件写入、麦克风、CoreAudio、操作系统缓冲或驱动。前 3 个仅填充启动状态的
调用也不进入分位数，第 4 个产生 frame 0 的调用开始计入。

在 48 kHz 下：

```text
hop_period_ms = 256 / 48000 × 1000 = 5.333333 ms
```

正式条件是 `P95 compute < hop_period`，使用严格小于。单个偶发超时另由 `deadline_miss_count` 记录；
P50/P95/P99 使用与离线 benchmark 相同的 Type-7 线性插值。测试不强制性能门槛，因为 CI 或其他电脑
的调度噪声不能代表正式 M 系列实验机。

## 5. Terminal 完整运行方法

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release

./build/macos-arm64-release/hero-audio-stream \
  path/to/input.wav \
  results/raw/streaming-hop-measurements.csv \
  results/processed/streaming-benchmark-summary.json \
  --warmup-passes 1 \
  --passes 5 \
  --backend fftw
```

也可以运行 `./scripts/run_synthetic_demo.sh`，它会同时生成合成 WAV、检测/评价、离线 benchmark、
流式 benchmark 和诊断图。

raw CSV 的每一行都是一项可审计测量：pass、输入 hop、分析 frame、compute、deadline、是否按时完成和
是否发出 onset。summary JSON 保存计时定义、排除项、配置、分位数、deadline 结果、忽略尾部数和
离线一致性结果。正式报告应保留 raw CSV，不能只摘抄 P95。

## 6. 与真实麦克风和“异常检测”的关系

这一版已经完成真正的有状态、因果算法核心，但 WAV 重放速度由循环控制，并非声卡按墙钟时间推送。
下一层可以写一个 CoreAudio 输入适配器，在音频 callback 中把 mono float32 放进无锁队列，再由处理
线程每累积 256 samples 调用同一个 `StreamingProcessor`。设备延迟、callback 到处理线程排队时间和
算法计算时间必须分列报告。

“异常”是业务定义，不等于 onset。若目标转为机器异响、设备故障声或枪声/玻璃破裂等事件，需要先
确定异常类别、正常基线、标注数据与误报成本，再在 StreamingProcessor 输出后增加独立的
`EventDecision` 层。当前 onset/flux 可以作为其中一组瞬态特征，而不应被直接宣称为通用异常检测器。
