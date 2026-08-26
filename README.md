# HERO-Audio

面向实时音频起始点检测的异构计算与运营优化研究。本仓库当前为 **v0.1 项目骨架**，首要平台是
macOS／Apple Silicon；第一阶段仅实现和验证 C++ CPU 检测闭环。

## 当前锁定的技术基线

- C++20、CMake 3.25+、Ninja；
- 正式 CPU FFT：FFTW3f（float32、1024-point R2C）；
- 项目内 radix-2 FFT 只用于正确性验证；
- 48 kHz、frame 1024、hop 256、Hann window；
- causal threshold；
- onset 评价采用 ±50 ms、最大匹配数优先且总误差最小的一对一匹配；
- 离线整文件 benchmark 报告 median、P95、P99 和 RTF；
- 流式处理器每次接收 256 samples，并报告 P50、P95、P99 hop compute；
- 自动证明流式 flux/onset 与离线结果一致，48 kHz 下要求 `P95 < 5.333 ms`；
- 逐帧 diagnostics 支持绘制 Spectral Flux、causal threshold 与 onset。

完整指标定义见 [docs/benchmark_protocol.md](docs/benchmark_protocol.md)。
从 Terminal、C++、CMake 到 GitHub 首次发布的分步说明见
[docs/step_by_step_implementation.md](docs/step_by_step_implementation.md)。
本阶段完整计时边界、C++ 源码结构和绘图字段见
[docs/offline_benchmark_and_plot.md](docs/offline_benchmark_and_plot.md)。
逐 hop 状态机、计时边界与一致性证明见
[docs/streaming_processor.md](docs/streaming_processor.md)。

GitHub Actions 只验证跨机器构建和数值正确性，不生成或发布性能结论。正式 Apple Silicon benchmark
必须在本地 M 系列机器上运行，并保存 `scripts/capture_system_info.sh` 的输出。

## macOS Apple Silicon 快速开始

需要先安装 Homebrew，然后运行：

```bash
./scripts/bootstrap_macos.sh
```

等价的手动命令：

```bash
brew bundle
cmake --preset macos-arm64-dev
cmake --build --preset macos-arm64-dev
ctest --preset macos-arm64-dev
./build/macos-arm64-dev/hero-audio
```

读取 WAV、转换为 mono float32 并计算 Spectral Flux：

```bash
./build/macos-arm64-dev/hero-audio path/to/input.wav
./build/macos-arm64-dev/hero-audio path/to/input.wav results/raw/spectral-flux.csv
./build/macos-arm64-dev/hero-audio path/to/input.wav results/raw/spectral-flux.csv results/raw/onsets.csv
./build/macos-arm64-dev/hero-audio path/to/input.wav results/raw/spectral-flux.csv \
  results/raw/onsets.csv results/raw/diagnostics.csv
```

当前 WAV reader 支持 little-endian RIFF/WAVE、整数 PCM 8/16/24/32-bit、IEEE float32
以及 WAVE_FORMAT_EXTENSIBLE 中对应的 PCM/float 子格式。多声道输入按算术平均转换为 mono float32；
当前阶段不进行重采样。

Spectral Flux 默认使用 frame size 1024、hop size 256 和对称 Hann window。只处理完整帧，不对
尾部进行隐式补零。CSV 分别记录 frame start、center 和数据完整可用时刻；第一帧因为没有前一帧，
flux 固定为零。构建中存在 FFTW3f 时 CLI 优先使用 FFTW，否则使用 reference backend。

Causal onset detector 只使用当前帧之前最多 16 帧计算 `mean + 1.5 × population_stddev`
阈值，并等待一个右侧帧确认局部最大值。默认 refractory 为 30 ms；实时状态机采用先确认峰优先，
不会为等待 refractory 内更强峰而增加额外输出延迟。onset CSV 同时记录信号时间、实际发出时间和
算法延迟。

使用人工标注评价预测 onset，默认容差为 ±50 ms：

```bash
./build/macos-arm64-dev/hero-audio-eval \
  results/raw/onsets.csv references.csv \
  --matches results/raw/matches.csv \
  --metrics results/raw/metrics.json \
  --tolerance-ms 50
```

reference CSV 可以是无表头的每行一个秒数，也可以包含 `onset_time_seconds` 列。匹配首先最大化
一对一 TP 数量，再最小化总绝对时间误差；重复预测不能重复匹配同一标注。没有预测或没有标注造成
指标分母为零时，对应 Precision、Recall 和 F1 保守记为 0。

## 整文件 benchmark 与第一张图

安装隔离的 Python 绘图环境，然后运行完整合成样例：

```bash
./scripts/bootstrap_analysis.sh
./scripts/run_synthetic_demo.sh
```

脚本会生成具有 5 个已知 onset 的 48 kHz WAV，运行正式 Release+FFTW3f 检测和一对一评价，执行
3 次 warm-up 与至少 5 次 measured runs，并生成：

```text
results/raw/synthetic-demo/offline-benchmark-runs.csv
results/raw/synthetic-demo/streaming-hop-measurements.csv
results/processed/synthetic-demo/offline-benchmark-summary.json
results/processed/synthetic-demo/streaming-benchmark-summary.json
results/processed/synthetic-demo/spectral-flux-threshold-onsets.png
```

单独运行 benchmark：

```bash
./build/macos-arm64-release/hero-audio-bench \
  path/to/input.wav \
  results/raw/offline-benchmark-runs.csv \
  results/processed/offline-benchmark-summary.json \
  --warmup 3 --runs 5 --backend fftw
```

FFTW plan 创建时间单独报告。每个 steady-state run 从开始读取 WAV 计时，到所有 onset 完成 CSV
序列化后停止；benchmark 结果文件本身的磁盘写入不进入该主指标。

## 流式 hop benchmark

下面的程序先把 WAV 解码为 mono float32，再模拟音频设备每次送入 256 个新采样。WAV 解码、FFT plan
初始化和输出文件写入均不计入 hop compute：

```bash
./build/macos-arm64-release/hero-audio-stream \
  path/to/input.wav \
  results/raw/streaming-hop-measurements.csv \
  results/processed/streaming-benchmark-summary.json \
  --warmup-passes 1 --passes 5 --backend fftw
```

前 3 个 hop 只负责填满第一个 1024-sample 窗口，不进入稳态分位数。从第 4 个 hop 开始，每个计时
样本包括新采样写入、Hann、FFTW、Spectral Flux 和 causal detection。48 kHz 下每个 hop 对应
`256 / 48000 = 5.333 ms` 音频时间，因此 `P95 compute` 必须严格低于 5.333 ms。summary JSON 还会
用现有离线流程重算同一 WAV，核对 frame/onset 数、逐帧 flux 和 onset 事件；不一致时 CLI 返回错误。

这是“用 WAV 仿真逐块到达”的算法与 CPU 调度基准，不包含麦克风、CoreAudio、操作系统缓冲和驱动
延迟。真实麦克风接入属于下一层输入适配器，不能把本指标冒充为完整设备端到端延迟。

正式 CPU 基准使用 release preset；它会在 FFTW3f 缺失时直接失败，避免误用参考 FFT：

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
```

仅检查参考实现时可使用 `portable-reference` preset。

## 目录

```text
include/hero_audio/       公共 C++ 接口
src/fft/                  FFT 后端实现与工厂
src/audio/                WAV 解码与 mono 转换
src/benchmark/            整文件与逐 hop 重复计时、统计和一致性验证
src/dsp/                  Hann、分帧、Spectral Flux、causal onset 与流式状态机
src/evaluation/           最优一对一匹配与准确率指标
tests/                    正确性测试
configs/                  版本化实验配置
docs/                     指标与实验协议
scripts/                  环境安装、合成数据、绘图和元数据采集
results/raw/              不修改的原始结果
results/processed/        可重新生成的处理结果
```

## FFTW 许可提醒

FFTW 采用 GPL 许可。研究代码可保留自己的许可，但分发链接 FFTW 的二进制文件时必须评估并遵守
GPL 要求。若未来需要非 GPL 的商业分发方案，应单独选择或采购兼容后端；这不改变本研究中的
FFTW3f CPU baseline。
