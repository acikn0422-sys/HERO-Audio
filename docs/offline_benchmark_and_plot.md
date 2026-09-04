# 整文件 benchmark 与首张诊断图：完整实现说明

本文对应 `codex/offline-benchmark-plot` 阶段。源文件本身带有设计注释；本文说明它们如何连接，避免
把离线整文件时间、单 hop 计算时间和流式 detection delay 混为一个指标。

## 1. 数据流

```text
FFTW3f plan 初始化（单独计时）
        │
        v
WAV read → mono float32 → Hann/FFT → Spectral Flux
                                      │
                                      v
                         causal threshold / peak picking
                                      │
                         ┌────────────┴────────────┐
                         v                         v
                     onset CSV              diagnostics CSV
                         │                         │
                         v                         v
                 Precision/Recall/F1       Matplotlib PNG
```

## 2. C++ benchmark 源码

公共接口位于 `include/hero_audio/offline_benchmark.hpp`：

- `OfflineBenchmarkConfig`：warm-up、measured runs 与算法配置；
- `OfflineBenchmarkRun`：每次运行的原始 elapsed、RTF、frame/onset 数；
- `OfflineBenchmarkSummary`：median、P95、P99、median/minimum RTF；
- `OfflineBenchmarkResult`：输入、backend、plan 初始化时间、raw runs 与汇总。

实现位于 `src/benchmark/offline_benchmark.cpp`。`measure_pipeline_once()` 使用
`std::chrono::steady_clock`，其计时边界为：

```cpp
const auto start = BenchmarkClock::now();
const auto audio = read_wav(input_path);
const auto flux = compute_spectral_flux(...);
const auto onsets = detect_causal_onsets(...);
std::ostringstream serialized_onsets;
write_onsets_csv(serialized_onsets, onsets);
const auto end = BenchmarkClock::now();
```

这里选择内存输出流，是为了计入 onset 格式化成本，同时排除 SSD、文件系统缓存和同步策略造成的噪声。
raw/summary 文件在所有 measured runs 完成后写出，不在主计时区间内。

`linear_percentile()` 先排序再使用 Type-7 线性插值。五个样本 `[1,2,3,4,5]` 的 P95 是 4.8，
P99 是 4.96；测试用这个例子锁住定义。

命令行入口 `src/benchmark_main.cpp` 先围绕 `make_fft_backend()` 测量 plan 初始化，再调用
`benchmark_offline_wav()`。`--backend fftw` 在 FFTW 不可用时直接失败，防止 reference FFT 被误标成
正式 CPU 性能。

## 3. 逐帧 threshold 诊断源码

`include/hero_audio/causal_onset.hpp` 定义：

- `OnsetDiagnosticFrame`：每帧的 flux、causal threshold 与状态；
- `CausalOnsetFrameResult`：一次状态转移产生的事件和诊断；
- `CausalOnsetAnalysis`：完整事件列表和逐帧诊断列表。

`src/dsp/causal_onset.cpp` 的 `process_with_diagnostics()` 同时完成检测和诊断。threshold 始终在当前
flux 加入 history 前计算，因此 diagnostics 不破坏因果性。`process()` 只是返回同一函数的 event，
不存在第二套检测实现。

## 4. Python 与其他脚本

- `scripts/generate_synthetic_clicks.py`：生成 5 秒、5 个已知 onset 的 48 kHz PCM16 WAV；
- `scripts/plot_onset_diagnostics.py`：只读取 C++ diagnostics/onset CSV 并绘图，不重新检测；
- `scripts/bootstrap_analysis.sh`：创建 `.venv` 并安装完全固定的绘图依赖；
- `scripts/run_synthetic_demo.sh`：运行完整 Release+FFTW3f 闭环。

首次运行：

```bash
brew bundle
./scripts/bootstrap_analysis.sh
./scripts/run_synthetic_demo.sh
```

## 5. 输出解释

raw benchmark CSV 每行是一项 measured run。摘要 JSON 中：

- `backend_initialization_ms` 不进入 steady-state 分位数；
- `median_elapsed_ms`、`p95_elapsed_ms`、`p99_elapsed_ms` 是整文件主指标；
- `median_real_time_factor` 是典型处理倍率；
- `minimum_real_time_factor` 是本组最慢 measured run 的处理倍率。

首张图中蓝线是 Spectral Flux，橙线是只看历史帧的 threshold，红色倒三角是预测 onset，绿色虚线是
已知 reference onset。合成数据得到 F1=1.0 只用于验证闭环正确，不可作为真实音乐数据准确率结论。
