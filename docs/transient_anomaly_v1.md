# 稳态瞬态声学异常 v1：定义、实现与实验规则

本阶段在既有 CoreAudio → `StreamingProcessor` 主链之后增加一个可审计的单类异常层。它不训练
神经网络，不执行第二次 FFT，也不改变 onset、hop latency 或 FFTW 基线的语义。

## 1. v1 到底检测什么

v1 的研究对象被严格限定为：

> 在操作者明确声明设备处于 steady 状态，且开头连续 30 秒已经人工确认为正常的前提下，寻找相对
> 该正常基线显著偏大的、由 causal onset 确认的瞬态声学事件。

因此 `unexpected_transient` 只是“值得复核的异常瞬态”，不是具体故障类别。程序不能仅凭声音回答
轴承损坏、碰撞危险或产品缺陷，也不能把 startup、shutdown 与 steady 混为一个分布。

## 2. 数据流与计时边界

```text
CoreAudio hop (256 mono float32 samples)
            │
            ├── 原主链：Hann → FFTW → Spectral Flux → causal onset
            │              └── 原 hop compute 计时到此结束
            │
            └── AcousticFeatureExtractor（旁路，不做 FFT）
                    ├── 复用现有 Spectral Flux
                    ├── 1024-sample frame RMS
                    ├── absolute peak
                    └── zero-crossing rate
                              │
                              v
                    TransientAnomalyDetector
                    ├── operating-state gate
                    ├── 30 s verified-normal median/MAD
                    ├── frozen baseline
                    └── confirmed-onset event score
```

`summary.json` 中的 core hop P95 仍只测原 `StreamingProcessor::push_hop()`。异常层耗时另写为
`anomaly-summary.json` 的 `p95_downstream_anomaly_analysis_ms`，两者不得相加后仍称原始 FFT/onset
benchmark。

## 3. 特征

对与 FFT 完全相同的 1024-sample 原始帧计算：

```text
frame_rms = sqrt(sum(x[n]^2) / 1024)
peak_absolute = max(abs(x[n]))
zero_crossing_rate = sign changes / 1023
spectral_flux = 直接复用 StreamingProcessor 已算出的值
```

zero-crossing rate 在 v1 只用于记录与解释，不进入最终阈值。这样可以先观察它是否有区分力，再在
development 数据上预注册下一版本；不能看完 held-out test 后临时把它加入规则。

## 4. 30 秒正常基线

只有 `--operating-state steady` 的帧会进入校准。默认 48 kHz、hop 256 时，需要：

```text
ceil(30 × 48000 / 256) = 5625 feature frames
```

最初还要等待 1024 samples 填满第一帧，所以要获得校准后的监测数据，录音必须略长于 30 秒；正式
实验建议至少 60 秒。前 30 秒必须由人确认：设备工况稳定、没有故意制造的异常、麦克风位置不变。

每个特征分别保存 median 与 MAD，并使用：

```text
robust_scale = max(1.4826 × MAD, relative floor, absolute floor)
positive_z = max(0, (value - median) / robust_scale)
```

绝对/相对 floor 防止安静录音出现 MAD=0 时除零。第 5625 个校准帧完成后统计量被冻结；监测期事件
不会更新基线。若校准尚未完成时发生队列丢帧或时间轴断裂，已收集的校准数据全部丢弃并重新开始；
已完成的冻结基线在断裂后保留，但不会跨断裂确认 onset。
若未完成校准时 operating state 离开 `steady`，同样清空当前校准；返回 `steady` 后必须重新获得连续
30 秒 verified-normal 数据。

## 5. 事件评分

只有既有 causal detector 已确认的 onset 才会评分：

```text
score = max(flux_positive_z / 6,
            rms_positive_z / 6,
            peak_positive_z / 6)
```

`score >= 1` 产生异常候选；相邻异常还有 250 ms refractory，期间的高分事件保留审计记录，但不重复
发出 `anomaly-events.csv`。没有 onset 时，即使一帧 RMS 很高也不会直接发出 v1 瞬态异常。

## 6. Terminal 运行

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release

./build/macos-arm64-release/hero-audio-live \
  data/local/anomaly-session-01 \
  --seconds 60 \
  --backend fftw \
  --operating-state steady \
  --anomaly-baseline-seconds 30
```

操作时前 30 秒保持正常稳态；30 秒之后才制造待测的敲击、摩擦或其他已定义事件。如果真实设备并不在
稳态，必须传 `idle`、`startup` 或 `shutdown`；这些状态会被 gate，不参与 v1 校准或异常输出。

也可以直接运行交互脚本：

```bash
./scripts/capture_transient_anomaly_session.sh
```

它默认录制 60 秒，并在结束后创建只有表头的 `human-labels.csv`。终端出现
`anomaly_baseline_complete: monitoring has started` 以前不要制造测试异常。

## 7. 新输出

- `anomaly-frames.csv`：每个分析帧的原始特征、校准进度、监测 z-score、onset 评分和异常耗时；
- `anomaly-events.csv`：实际发出的 `unexpected_transient`，包含全局时间、score、主导特征和 z-score；
- `anomaly-summary.json`：实际参数、基线是否完成、冻结统计、事件计数、capture integrity 和限制声明。
- `capture-analysis-f32.wav`：保存 live 分析用的 mono float32 样本，供相同参数确定性回放。

若 `baseline_complete=false`，本次 session 只能用于排错，不能用于异常准确率结论。若
`capture_integrity_pass=false`，同样不能进入正式实验。

## 8. 人工标注和正式评价

至少准备 development 与 held-out 两组独立 session。建议人工标注文件：

```csv
start_seconds,end_seconds,class,operating_state,confidence,annotator
35.120,35.180,anomaly_impact,steady,high,EY
42.500,42.900,normal_transition,steady,medium,EY
```

第一轮类别可以锁定为：

- `normal_background`：正常稳态背景；
- `normal_transition`：业务允许但容易触发的声音；
- `anomaly_impact`：非预期敲击或碰撞；
- `anomaly_burst`：非预期短时连续突发。

二分类评价时前两类归 normal，后两类归 anomaly。阈值只能使用 development sessions 调整，之后冻结
并在 held-out sessions 上报告 event Precision、Recall、F1、false alarms/hour 和 P95 detection delay。
当前代码与无标注录音只能证明计算流程成立，不能证明业务准确率。

现在可使用 `hero-audio-anomaly-replay` 生成可重复事件，再用 `hero-audio-anomaly-eval` 读取 manifest、
人工区间标签与原始 live 完整性证据。完整命令、匹配公式、delay scope、development 调参和 held-out
冻结协议见 `docs/anomaly_replay_and_evaluation.md`。

## 9. 源码位置

- `include/hero_audio/acoustic_features.hpp`、`src/dsp/acoustic_features.cpp`：旁路特征；
- `include/hero_audio/anomaly_detector.hpp`、`src/dsp/anomaly_detector.cpp`：gate、基线和评分；
- `include/hero_audio/anomaly_output.hpp`、`src/dsp/anomaly_output.cpp`：可单测的 CSV 序列化；
- `tests/test_anomaly_detector.cpp`：同步、冻结、防污染、refractory 和断裂测试；
- `include/hero_audio/anomaly_replay.hpp`、`src/dsp/anomaly_replay.cpp`：逐 256 samples 的确定性回放；
- `include/hero_audio/anomaly_evaluation.hpp`、`src/evaluation/anomaly_evaluation.cpp`：人工标签解析、最优
  一对一匹配和 pooled 指标；
- `configs/transient_anomaly_v1.json`：预注册的 v1 语义和默认参数；
- `src/live_main.cpp`：实时输出接线，不改变原核心计时边界。
