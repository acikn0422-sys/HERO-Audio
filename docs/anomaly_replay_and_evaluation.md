# 异常录音、确定性回放与 held-out 评价协议

本协议把实时演示变成可以复核的研究闭环：实时阶段只负责采集原始证据，离线阶段用同一份 float32
样本重复运行检测器，人工标签独立于预测生成，最后使用最优一对一匹配计算事件指标。

## 1. 数据链和不可混用的口径

```text
CoreAudio 实时录音
  ├── capture.wav                 供人回听的 mono PCM16
  ├── capture-analysis-f32.wav    供程序回放的 mono IEEE float32
  ├── anomaly-summary.json        原始 live capture integrity 证据
  └── human-labels.csv            人工独立填写
                 │
                 v
hero-audio-anomaly-replay
  ├── anomaly-frames.csv
  ├── anomaly-events.csv
  └── anomaly-summary.json        回放参数和确定性证据
                 │
                 v
hero-audio-anomaly-eval + evaluation-manifest.csv
  ├── matches.csv
  ├── session-metrics.csv
  └── metrics.json
```

`capture-analysis-f32.wav` 保存消费线程实际分析的有限 float32 样本，不经过 PCM16 量化。它是正式回放
输入；`capture.wav` 只用于人听。若 live 阶段 dropped hop 或 render error 非零，原始
`anomaly-summary.json` 会令该 session 失去正式评价资格。回放摘要不能替代这份 live 完整性证据。

检测延迟必须保留 scope：

- `software_after_callback_estimate`：live 事件的算法延迟加 callback 后排队/计算估计，不含设备和驱动；
- `algorithm_only_replay`：确定性回放的纯算法结构延迟，不是实时端到端延迟。

评价器拒绝在同一个 split 中混合两种 scope。

## 2. Terminal：构建和测试

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
```

## 3. 录音前先锁定实验表

在听取 held-out 结果前先记录：

1. 哪些 session 是 `development`，哪些是 `held-out`；
2. 麦克风位置、设备工况和正常稳态的定义；
3. 将要制造的事件数、类别和大致顺序，但不要把预测时间当标签；
4. development 上允许尝试的参数网格和选择规则；
5. held-out 只运行一次的冻结参数版本。

第一轮合法标签固定为：

- `normal_background`：正常稳态背景；
- `normal_transition`：允许发生但容易误报的状态变化；
- `anomaly_impact`：非预期单次敲击/碰撞；
- `anomaly_burst`：非预期短时连续突发。

本版本只研究 steady 状态的瞬态，不声称识别具体机器故障。

## 4. Terminal：采集一个 session

development 示例：

```bash
./scripts/capture_transient_anomaly_session.sh \
  data/local/anomaly-dev-01 60 30 development
```

held-out 示例：

```bash
./scripts/capture_transient_anomaly_session.sh \
  data/local/anomaly-test-01 60 30 held-out
```

前 30 秒保持人工确认的正常稳态。终端出现
`anomaly_baseline_complete: monitoring has started` 后才制造预先定义的事件。脚本不会覆盖已有文件，
并创建空标签表与单 session 的 `evaluation-manifest.csv`。

## 5. 人工独立标注

只回听 `capture.wav`，在 `human-labels.csv` 中填写区间：

```csv
start_seconds,end_seconds,class,operating_state,confidence,annotator
35.120,35.180,anomaly_impact,steady,high,EY
42.500,42.900,normal_transition,steady,medium,EY
```

约束：

- 时间相对录音起点，单位秒；`end_seconds >= start_seconds`；
- `operating_state` 在本版本必须是 `steady`；
- confidence 只能是 `high`、`medium` 或 `low`；
- annotator 不得为空；
- anomaly 区间不能互相重叠；
- 不查看或复制 `anomaly-events.csv`，避免标签泄漏。

最好由不知道程序输出的人标注；若只能自己标注，也应先隐藏预测文件并记录这一限制。

## 6. Terminal：float32 确定性回放

用预注册 v1 默认参数回放：

```bash
./build/macos-arm64-release/hero-audio-anomaly-replay \
  data/local/anomaly-dev-01/capture-analysis-f32.wav \
  data/local/anomaly-dev-01/replay-v1 \
  --backend fftw \
  --baseline-seconds 30 \
  --flux-z 6 \
  --rms-z 6 \
  --peak-z 6 \
  --refractory-ms 250 \
  --operating-state steady
```

回放不 sleep、不测墙钟时间、不隐式补齐最后一个不完整 hop。相同输入、后端和参数应得到逐字节一致的
CSV/JSON。FFTW 与 reference backend 用于正确性核对；正式 CPU 性能结论仍以 FFTW3f Release 为准。

## 7. Manifest

每行对应一个独立 session：

```csv
session_id,split,audio_path,labels_path,frames_path,events_path,eligibility_summary_path
anomaly-dev-01,development,capture-analysis-f32.wav,human-labels.csv,replay-v1/anomaly-frames.csv,replay-v1/anomaly-events.csv,anomaly-summary.json
```

相对路径从 manifest 所在目录解析。特别注意：

- `frames_path` 和 `events_path` 可以指向回放结果；
- `eligibility_summary_path` 必须指向原始 live session 的 `anomaly-summary.json`；
- `audio_path` 必须是 mono IEEE float32 32-bit WAV；
- 只有 `baseline_complete=true`、`capture_integrity_pass=true` 且完整性来源为
  `live_coreaudio_stats` 的 session 才能进入正式结果。

脚本生成的单 session manifest 可直接评价。多 session 研究时，把各行汇总到一份新 manifest，表头只
保留一次；不要把 `configs/anomaly_dataset_manifest.csv` 本身当作实验结果覆盖。

## 8. Terminal：只评价 development

```bash
./build/macos-arm64-release/hero-audio-anomaly-eval \
  data/local/anomaly-dev-01/evaluation-manifest.csv \
  results/processed/anomaly-dev-01-evaluation \
  --split development \
  --tolerance-ms 50
```

预测是时间点，人工异常标签是时间区间。预测落在区间内时误差为 0；在区间外但距离边界不超过
±50 ms 仍是候选。动态规划先最大化匹配数，再最小化总区间误差；每个预测和每个真实异常最多使用
一次。未匹配预测为 FP，未匹配异常为 FN。一个异常附近三个预测最多产生 1 TP，其余为 FP。

`false alarms/hour = FP × 3600 / monitoring_seconds`。监测时间从第一行 monitoring frame 到 float32
录音结尾，不含基线校准。P50/P95 detection delay 只使用已匹配 TP，定义为：

```text
预测 onset + 事件记录的 software delay - 人工区间 start
```

它的具体口径由输出中的 `detection_delay_scope` 明示。

## 9. development 调参、冻结、held-out 一次评价

推荐的研究纪律是：

1. 先把允许尝试的参数组合写入工程日志；
2. 每个组合回放到不同的新目录，不能覆盖旧结果；
3. 只用 `--split development` 比较 F1、false alarms/hour 和延迟；
4. 按预先写下的选择规则确定参数，例如先满足误报上限，再在可行组合中最大化 F1；
5. 更新并提交 `configs/transient_anomaly_v1.json`，提升 schema/model 版本，记录 commit hash；
6. 从此不再改参数；
7. 使用冻结参数回放 held-out session；
8. 只运行一次 `--split held-out` 并保留原始输出。

held-out 命令形式相同：

```bash
./build/macos-arm64-release/hero-audio-anomaly-eval \
  path/to/combined-manifest.csv \
  results/processed/anomaly-held-out-final \
  --split held-out \
  --tolerance-ms 50
```

若 held-out 不理想，结论应如实报告。根据 held-out 改阈值后再次测试，已经不再是同一份独立最终测试；
新模型必须使用新的 held-out 数据。

## 10. 结果文件

- `matches.csv`：所有 TP 的预测/标注索引、区间误差、决策时刻和 detection delay；
- `session-metrics.csv`：逐 session 的 TP/FP/FN、P/R/F1、误报率、正常切换误报和延迟；
- `metrics.json`：所选 split 的 pooled 总指标、匹配规则、delay scope 与按异常类别 recall。

这一闭环可以回答“在严格定义的稳态工况中，当前规则对预定义瞬态异常是否有效”。它不能回答未采样
设备、未定义异常类别、设备前端延迟或真实故障诊断问题。
