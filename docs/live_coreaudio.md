# CoreAudio 实时输入：实现、运行与实验边界

这一阶段把已经验证过的 `StreamingProcessor` 接到 macOS 默认输入设备。它完成真正按墙钟时间到达的
麦克风采集、实时分析、录音和证据输出，但仍只检测 onset/transient，不宣称识别任意业务异常。

## 1. 线程与数据流

```text
默认输入设备
    │ AUHAL / device-native sample rate
    v
CoreAudio real-time callback
    ├── AudioUnitRender → preallocated mono float32 buffer
    ├── aggregate exactly 256 samples
    └── non-blocking push
             │
             v
64-hop fixed SPSC queue
             │
             v
main consumer thread
    ├── StreamingProcessor: Hann → FFTW → flux → causal onset
    ├── downstream feature extraction → frozen-baseline transient score
    ├── capture.wav (PCM16)
    ├── hop-measurements.csv
    ├── onsets.csv
    ├── summary.json
    ├── anomaly-frames.csv / anomaly-events.csv
    └── anomaly-summary.json
```

callback 和 consumer 分开，是因为声卡线程有硬实时约束。FFT、磁盘或终端输出偶尔变慢时，不能让
callback 等待。`SpscHopQueue<256,64>` 在对象内部拥有全部存储，没有 `new`、mutex 或 condition
variable；ARM64/macOS 上使用的 atomic 类型由编译期 `is_always_lock_free` 约束。

## 2. 核心文件

- `include/hero_audio/spsc_hop_queue.hpp`：固定容量 SPSC queue 与 `AudioHopBlock`；
- `include/hero_audio/coreaudio_input.hpp`：平台接口、设备元数据与 capture stats；
- `src/audio/coreaudio_input.cpp`：AUHAL 配置、预分配 render 和 callback 聚合；
- `include/hero_audio/wav_writer.hpp`、`src/audio/wav_writer.cpp`：消费线程 PCM16 写盘；
- `src/live_main.cpp`：实时消费、检测、断点恢复、CSV/JSON；
- `tests/test_live_support.cpp`：队列容量/顺序、50,000-hop 并发传输和 WAV round trip；
- `scripts/capture_five_live_samples.sh`：五次人工控制的独立采集。

## 3. CoreAudio 配置

适配器使用 `kAudioUnitSubType_HALOutput`：

1. 启用 input bus 1，关闭 output bus 0；
2. 显式选择当前默认输入设备；
3. 读取设备原生 sample rate；
4. 在 input bus 的 client/output scope 设置 mono packed float32；
5. 查询 `MaximumFramesPerSlice` 并一次性分配 render buffer；
6. 设置 `ShouldAllocateBuffer=0`，由本项目提供预分配内存；
7. 注册 input callback，再初始化并启动 AudioUnit。

callback 可能一次给出任意 frame count，所以使用一个 256-sample partial buffer 跨 callback 聚合。
每个完整 block 保存 sequence、绝对 first sample index 和 callback host time。

## 4. 队列满和输入错误

队列满时不能阻塞 callback，当前 hop 会被丢弃并记录。消费者发现 sequence 或 sample index 不连续后：

- 在录音时间轴补静音，保持后续人工标注时间不漂移；
- `StreamingProcessor::reset()`，清除不连续的前一频谱与 threshold history；
- 创建新 segment，后续 onset 时间再加绝对 segment offset；
- 不允许跨断点比较频谱或确认 peak。

`capture_integrity_pass` 只有在 dropped hop 和 AudioUnit render error 都为零时才为 true。失败 session
仍保留用于诊断，但不能进入正式性能或准确率数据。

## 5. Terminal 运行

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release

./build/macos-arm64-release/hero-audio-live \
  data/local/manual-session-01 \
  --seconds 60 \
  --backend fftw \
  --operating-state steady \
  --anomaly-baseline-seconds 30
```

第一次运行时，在 macOS 弹窗选择允许。如果没有收到音频，到：

```text
System Settings → Privacy & Security → Microphone → Terminal
```

打开权限，再使用一个新的输出目录运行。程序故意拒绝覆盖旧 session，以免实验数据被静默替换。

## 6. 输出解释

`capture.wav` 是 mono PCM16。若发生已知断点，对应时间会写入静音。`hop-measurements.csv` 每行包含：

- absolute sequence/sample/time；
- 该行之前是否发生 discontinuity；
- callback 到 consumer 的等待；
- `push_hop()` compute；
- 实际 sample rate 对应的 deadline；
- 是否产生 analysis frame 或 onset。

`onsets.csv` 包含 signal time、audio emitted time、算法结构延迟、callback 后处理时间和 estimated
software detection delay。最后一项是：

```text
algorithm delay + callback-to-consumer + current hop compute
```

它不包含声音到达麦克风、设备缓冲或驱动在 callback 前的时间。完整声学端到端延迟需要扬声器/线路
回环或外部同步仪器，不可以用这个软件指标冒充。

## 7. 五段独立音频

运行：

```bash
./scripts/capture_five_live_samples.sh 10
```

脚本创建带 UTC 时间戳的 `data/local/live-five-*` 目录，依次提示五种场景。前三段属于 development，
后两段 held-out。每段都会创建只有表头的 `references.csv`；需要人工听 `capture.wav`，逐行填写真实
onset 秒数。预测文件 `onsets.csv` 不能作为人工 reference，否则评价会发生数据泄漏。

`data/local/` 已加入 `.gitignore`，因为录音可能包含人声或环境隐私。正式报告只提交获得许可且完成
匿名化的数据，或者提交数据来源、hash、统计结果和复现说明。

## 8. 关于“异常”

onset 只回答“频谱是否突然变化”，并不知道变化是正常拍手、机器故障还是背景噪声。当前新增的 v1
研究层把问题进一步限定为“稳态正常基线之外的瞬态复核候选”：它复用 flux，加上 RMS、absolute peak
和 zero-crossing rate，先收集 30 秒 verified-normal median/MAD，再冻结基线并只给 confirmed onset
评分。完整规则见 `docs/transient_anomaly_v1.md`。

这仍不等于故障类型识别。必须用独立人工标签报告 event Precision/Recall/F1、false alarms/hour 和检测
延迟后，才能作准确率陈述；无标签 live 输出只能证明实时计算闭环成立。
