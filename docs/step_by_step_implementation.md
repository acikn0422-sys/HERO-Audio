# HERO-Audio v0.1 分步骤实现指南

本指南把操作分为 Terminal、C++、构建配置、GitHub 四部分。当前版本已完成 Apple Silicon
开发环境、FFTW3f CPU 基线、WAV/mono、Spectral Flux、causal onset 检测、最优一对一 onset
评价、整文件计时、首张诊断图，以及逐 256 samples 的流式处理与 hop 延迟统计。下一步是扩展到
至少 5 段独立音频和正式标注数据。CoreAudio 实时输入代码已经完成，但真实录音与人工标注必须由
实验者在现场采集。

## 0. 当前数据流和目录

```text
WAV ──> mono float32 ──> overlap frames ──> Hann
                                             │
                                             v
                                      FFTBackend 接口
                                      ├── FFTW3f（正式 CPU）
                                      └── radix-2（正确性）
                                             │
                                             v
Spectral Flux CSV <── positive magnitude difference
        │
        v
causal threshold + peak confirmation ──> onset CSV
                                             │
人工标注 CSV ─────────────────────────────────┤
                                             v
                             最优一对一匹配 ──> P/R/F1

同一 mono float32 数据也可以按 256 samples 重放：

WAV → 逐 hop 输入 → StreamingProcessor → flux/onset
                            ├── P50/P95/P99 hop compute
                            └── 与离线结果逐帧核对
```

主要目录：

```text
include/hero_audio/       公共 C++ 接口
src/audio/                WAV 解码和 mono 转换
src/dsp/                  Spectral Flux 与 causal onset
src/evaluation/           最优匹配和准确率指标
src/fft/                  FFT 后端
tests/                    单元与 CLI 集成测试
configs/                  实验配置
docs/                     实验协议和说明
scripts/                  安装与信息采集脚本
results/raw/              原始数据，不手工修改
results/processed/        可重新生成的数据
```

## 1. Terminal：从零安装到运行

macOS 默认交互 shell 是 zsh，但下面命令可直接在 Terminal 中执行；项目的 `.sh` 文件使用 Bash。

### 1.1 进入项目目录

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
pwd
```

`pwd` 必须输出上面的 HERO-Audio 项目目录。不要在 `/Users/eddieyu` 直接执行 Git 添加命令。

### 1.2 检查工具

```bash
uname -m
brew --version
c++ --version
```

预期分别包含 `arm64`、Homebrew 版本和 Apple Clang 版本。

### 1.3 一键安装、配置、编译和测试

```bash
./scripts/bootstrap_macos.sh
```

脚本执行顺序：

```bash
brew bundle --file Brewfile
cmake --preset macos-arm64-dev
cmake --build --preset macos-arm64-dev
ctest --preset macos-arm64-dev
```

成功标志包括：

```text
HERO-Audio: FFTW3f backend enabled
100% tests passed, 0 tests failed
```

### 1.4 运行程序

```bash
./build/macos-arm64-dev/hero-audio
```

预期输出：

```text
HERO-Audio v0.1 scaffold
Available FFT backends:
  - reference-radix2
  - fftw3f
```

### 1.5 日常开发循环

修改 C++ 文件后，不必再次运行 `brew bundle`：

```bash
cmake --build --preset macos-arm64-dev
ctest --preset macos-arm64-dev
./build/macos-arm64-dev/hero-audio
```

### 1.6 正式 Release 构建

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
```

Release preset 强制 `arm64` 和 FFTW3f。如果 FFTW 缺失，它会停止配置，防止参考 FFT 被误当作正式
CPU 性能基线。

### 1.7 保存系统信息

```bash
./scripts/capture_system_info.sh | tee results/raw/system-info.txt
```

检查输出不包含用户名之外的隐私数据后再提交。脚本不采集序列号、硬件 UUID 或 UDID。

### 1.8 从 WAV 检测并评价 onset

先让检测程序写出 Spectral Flux 和预测 onset：

```bash
./build/macos-arm64-dev/hero-audio \
  path/to/input.wav \
  results/raw/spectral-flux.csv \
  results/raw/onsets.csv
```

人工标注可使用含 `onset_time_seconds` 表头的 CSV，也可每行只写一个秒数。然后执行：

```bash
./build/macos-arm64-dev/hero-audio-eval \
  results/raw/onsets.csv \
  path/to/references.csv \
  --matches results/raw/matches.csv \
  --metrics results/raw/metrics.json \
  --tolerance-ms 50
```

终端会显示 TP、FP、FN、Precision、Recall 和 F1；两个可选输出文件分别保留匹配明细与机器可读指标。

## 2. C++：按层实现检测与评价

### 2.1 定义统一接口

文件：`include/hero_audio/fft_backend.hpp`

核心接口：

```cpp
enum class FFTBackendKind { Reference, FFTW };

class FFTBackend {
public:
  virtual ~FFTBackend() = default;
  virtual std::string_view name() const noexcept = 0;
  virtual std::size_t fft_size() const noexcept = 0;
  virtual void execute(std::span<const float> input,
                       std::span<std::complex<float>> output) = 0;
};

std::unique_ptr<FFTBackend> make_fft_backend(FFTBackendKind kind,
                                             std::size_t fft_size);
```

接口约定：

- 输入为 `float32` 实数；
- 当前正式实验使用 1024 个采样；
- R2C 输出固定为 `N/2 + 1` 个复数频点；
- forward transform 不归一化；
- 后端对象创建一次，`execute()` 重复调用；
- plan 创建时间不得混入 steady-state FFT 执行时间。

### 2.2 实现参考 radix-2 FFT

文件：`src/fft/reference_backend.cpp`

实现步骤：

1. 验证 FFT size 是非零的 2 的幂；
2. 预分配 `scratch_`，避免每次执行重新申请；
3. 把实数输入按 bit-reversal 顺序写入复数缓冲区；
4. 从长度 2 开始逐级执行 Cooley–Tukey butterfly；
5. 只复制前 `N/2 + 1` 个 R2C 频点。

关键 butterfly：

```cpp
const auto even = scratch_[start + offset];
const auto odd = scratch_[start + offset + length / 2] * twiddle;
scratch_[start + offset] = even + odd;
scratch_[start + offset + length / 2] = even - odd;
twiddle *= root;
```

这个后端用于理解算法和检查 FFTW 输出，不用于正式 CPU 性能结论。

### 2.3 实现 FFTW3f 后端

文件：`src/fft/fftw_backend.cpp`

构造阶段：

```cpp
input_ = fftwf_alloc_real(size_);
output_ = fftwf_alloc_complex(size_ / 2 + 1);
plan_ = fftwf_plan_dft_r2c_1d(
    static_cast<int>(size_), input_, output_, FFTW_MEASURE);
```

这里使用 FFTW 自己的内存分配函数，确保满足 FFTW/SIMD 对齐要求。`FFTW_MEASURE` 的 plan 创建可能
较慢，所以只在构造时执行一次。

稳态执行：

```cpp
std::copy(input.begin(), input.end(), input_);
fftwf_execute(plan_);
for (std::size_t bin = 0; bin < output.size(); ++bin) {
  output[bin] = {output_[bin][0], output_[bin][1]};
}
```

析构阶段：

```cpp
fftwf_destroy_plan(plan_);
fftwf_free(input_);
fftwf_free(output_);
```

因此资源的生命周期和后端对象一致，不会在每个 hop 重建 plan。

### 2.4 用工厂隔离可选依赖

文件：`src/fft/backend_factory.cpp`

```cpp
switch (kind) {
case FFTBackendKind::Reference:
  return make_reference_backend(fft_size);
case FFTBackendKind::FFTW:
#ifdef HERO_AUDIO_HAS_FFTW3F
  return make_fftw_backend(fft_size);
#else
  throw std::runtime_error("FFTW3f backend is not available in this build");
#endif
}
```

上层算法只依赖 `FFTBackend`，不包含 `fftw3.h`。以后加入 Accelerate 或 cuFFT 时，Spectral Flux
代码不需要重写。

### 2.5 程序入口

文件：`src/main.cpp`

`src/main.cpp` 负责 WAV 到预测 onset 的完整检测链；构建中存在 FFTW3f 时优先选择正式 CPU
后端，否则退回 reference 后端：

```cpp
const auto audio = hero_audio::read_wav(input_path);
auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
const auto flux = hero_audio::compute_spectral_flux(
    audio.mono_samples, audio.sample_rate_hz, *fft);
const auto onsets = hero_audio::detect_causal_onsets(flux);
```

`src/evaluate_main.cpp` 是独立评价入口，不重复执行音频检测，因此同一份预测可以使用不同人工标注或
容差复算指标。

### 2.6 正确性测试

文件：`tests/test_fft.cpp`

FFT 测试会对所有已编译后端运行：

1. 单位脉冲的所有频点应近似 `1 + 0i`；
2. 整周期正弦的最大频谱峰必须出现在指定频点 37。

```cpp
for (const auto kind : hero_audio::available_fft_backends()) {
  if (!test_impulse(kind) || !test_sine_peak(kind)) {
    return 1;
  }
}
```

除此之外，CTest 还覆盖 WAV reader、Spectral Flux、causal onset、流式/离线一致性、最优匹配与
评价 CLI。运行：

```bash
ctest --preset macos-arm64-dev --output-on-failure
```

当前在 macOS 上应有 10 个 CTest 测试全部通过；其中 live CLI 测试只检查构建和帮助输出，不在 CI
录制麦克风。

### 2.7 StreamingProcessor 与逐 hop 计时

公共状态机位于 `include/hero_audio/streaming_processor.hpp`，实现位于
`src/dsp/streaming_processor.cpp`。构造时预分配 1024-sample 环形缓冲、Hann、FFT 输入输出与相邻
帧 magnitude。每次 `push_hop()` 只接受 256 个有限的 mono float32 采样；错误输入在改变状态前
抛出异常。

前 5 个 hop 的状态变化如下：

```text
hop 0：收到 256，未产生 frame
hop 1：收到 512，未产生 frame
hop 2：收到 768，未产生 frame
hop 3：收到 1024，产生 frame 0
hop 4：收到 1280，产生以 sample 256 开始的 frame 1
```

`include/hero_audio/streaming_benchmark.hpp` 与 `src/benchmark/streaming_benchmark.cpp` 围绕每个产生
分析帧的 `push_hop()` 调用计时。命令行入口是 `src/streaming_main.cpp`：

```bash
./build/macos-arm64-release/hero-audio-stream \
  input.wav raw-hops.csv summary.json \
  --warmup-passes 1 --passes 5 --backend fftw
```

raw CSV 保留每个 hop 的 compute、5.333 ms deadline、是否超时和是否发出 onset；JSON 汇总 P50、
P95、P99、maximum 和 deadline miss。计时结束后再运行原有离线流程，自动核对逐帧 flux 与 onset；
因此一致性检查不会污染 hop 计时。

### 2.8 最优一对一 onset 评价

公共接口位于 `include/hero_audio/onset_evaluation.hpp`，实现位于
`src/evaluation/onset_evaluation.cpp`。输入会稳定排序，但输出仍保存原始输入索引。动态规划状态
`dp[i][j]` 表示前 `i` 个预测与前 `j` 个标注的最优结果，每个状态比较三种转移：

```text
跳过预测：dp[i-1][j]
跳过标注：dp[i][j-1]
形成匹配：dp[i-1][j-1] + (1 match, absolute_error)
```

只有 `absolute_error <= tolerance` 才允许第三种转移。状态比较先选择匹配数更多的方案；匹配数相同
时选择总绝对误差更小的方案。最后回溯 `choice` 字段得到每个一对一配对。复杂度为 `O(NM)` 时间和
`O(NM)` 空间。

指标由最终匹配数直接计算：

```text
TP = matches
FP = predictions - TP
FN = references - TP
Precision = TP / (TP + FP)
Recall = TP / (TP + FN)
F1 = 2 * Precision * Recall / (Precision + Recall)
```

任一分母为零时对应指标返回 0。单元测试另用不受序列动态规划约束的穷举搜索检查所有小规模组合，
同时覆盖重复预测、乱序输入、50 ms 包含边界和无数据情形。

## 3. 其他配置文件

### 3.1 Brewfile

```ruby
brew "cmake"
brew "ninja"
brew "pkg-config"
brew "fftw"
```

它固定“依赖名称”，具体安装版本应由每次实验的系统信息记录。

### 3.2 CMakeLists.txt

配置逻辑：

1. 创建 `hero_audio_core`；
2. 始终编译 reference backend 和 factory；
3. 查找 FFTW3f；
4. 找到时加入 `fftw_backend.cpp`、链接 `FFTW3f::fftw3f` 并定义
   `HERO_AUDIO_HAS_FFTW3F=1`；
5. 创建 `hero-audio`、`hero-audio-eval`、`hero-audio-bench`、`hero-audio-stream`、
   `hero-audio-anomaly-replay`、`hero-audio-anomaly-eval`，并在 macOS 创建 `hero-audio-live`；
6. 创建并注册 FFT、WAV 读写、SPSC 队列、Spectral Flux、causal onset、流式处理、稳态瞬态异常、
   评价、benchmark 与 CLI 测试。

项目把正式库名锁定为单精度 `fftw3f`，不能误链接 double 精度的 `fftw3`。

### 3.3 CMakePresets.json

- `macos-arm64-dev`：Debug，允许 reference-only；
- `macos-arm64-release`：Release，强制 arm64 和 FFTW3f；
- `portable-reference`：关闭 FFTW，只验证可移植参考代码。

### 3.4 baseline.json

`configs/baseline.json` 固定采样率、frame、hop、阈值模式、匹配规则、warm-up 次数、重复次数与流式
hop 计时边界。
修改实验语义时必须提升 `schema_version`，不要覆盖旧结果对应的配置。

## 4. GitHub：首次创建和上传

以下是操作说明，本指南不会自动替你发布。

### 4.1 先建立独立仓库边界

当前 HERO-Audio 的上级 `/Users/eddieyu` 已经存在 `.git`。必须先在项目目录创建嵌套的独立仓库，
否则 `git add` 可能把主目录里的其他文件纳入范围。

```bash
cd "/Users/eddieyu/Documents/Codex/2026-08-18/1-cpu-gpu-heterogeneous-computing-and"
git init
git rev-parse --show-toplevel
```

第二条命令必须输出当前 HERO-Audio 目录，而不是 `/Users/eddieyu`。不符合时不要继续。

### 4.2 检查并创建首次提交

```bash
git status --short
git add .clang-format .github .gitignore Brewfile CMakeLists.txt CMakePresets.json README.md cmake configs docs include results scripts src tests
git status --short
git commit -m "Initialize HERO-Audio Apple Silicon CPU scaffold"
```

不要执行 `git add ..`，也不要添加 `build/`、`work/` 或 ZIP；它们已被 `.gitignore` 排除或属于生成物。

### 4.3 在 GitHub 网页创建空仓库

1. 登录 GitHub；
2. 选择 **New repository**；
3. Repository name 填写 `HERO-Audio`；
4. 选择 Public 或 Private；
5. 不勾选 README、`.gitignore` 或 License，因为本地已经有文件；
6. 创建仓库。

### 4.4 连接远程并推送

把 `<YOUR_GITHUB_USERNAME>` 替换为自己的用户名：

```bash
git branch -M main
git remote add origin https://github.com/<YOUR_GITHUB_USERNAME>/HERO-Audio.git
git remote -v
git push -u origin main
```

第一次通过 HTTPS 推送时，GitHub 可能要求浏览器授权或 Personal Access Token，不能使用 GitHub
账户密码代替 token。

### 4.5 检查 GitHub Actions

推送后打开仓库的 **Actions** 页面，查看 `ci` 工作流。它只验证构建和正确性，不把 GitHub runner
的时间当作 Apple Silicon 性能基准。

### 4.6 后续日常提交

```bash
git status --short
git diff
git add <本次修改的明确文件>
git commit -m "Describe the change"
git push
```

提交前始终先看 `git diff`，避免上传音频版权材料、个人路径、密钥、大型构建目录或未匿名化数据。

## 5. 当前完成边界和下一步

当前闭环是：

```text
依赖安装 → WAV/mono float32 → frame/hop → Hann → FFT magnitude → Spectral Flux CSV
```

已经完成：

1. 基础 PCM/float WAV reader；
2. mono float32 转换；
3. frame/hop 和 Hann window；
4. FFT magnitude、Spectral Flux 与 CSV；
5. causal adaptive threshold、peak picking 与 onset CSV；
6. 最优一对一 onset matching、Precision、Recall 与 F1；
7. 整文件 median、P95、P99 与 RTF；
8. “时间—Spectral Flux—threshold—onset”可复现图。
9. 逐 256 samples 的 `StreamingProcessor`；
10. P50/P95/P99 hop compute、5.333 ms deadline 检查与离线一致性证明。
11. macOS CoreAudio 实时输入与 64-hop 有界无锁 SPSC 队列；
12. 实时 PCM16 录音、onset、逐 hop timing、capture integrity 和 summary 输出；
13. 五段独立自录音频的交互采集脚本与 development/held-out 计划；
14. 不重复 FFT 的流式声学特征提取；
15. 30 秒 verified-normal median/MAD、冻结基线、steady-state gate 与 confirmed-onset 异常评分；
16. 独立 `anomaly-frames.csv`、`anomaly-events.csv` 和 `anomaly-summary.json` 审计输出。
17. live mono IEEE float32 bit-exact 分析录音与相同 DSP 主链的确定性回放；
18. 人工异常区间标签、manifest 资格检查、最优一对一匹配、F1、false alarms/hour 和 P95 delay；
19. development 调参、参数冻结与 held-out 一次最终评价协议。

下一阶段按顺序完成第一阶段剩余数据工作：

1. 运行 `./scripts/capture_five_live_samples.sh 10`，采集至少 5 段相互独立的合法测试音频；
2. 人工填写每段 `references.csv`，保持 development 与 held-out 分离；
3. 在真实/公开标注数据上重复准确率、整文件 benchmark 与逐 hop benchmark；
4. 按 `docs/anomaly_replay_and_evaluation.md` 采集至少 60 秒的 steady session，前 30 秒人工确认正常，后半段
   注入预先定义的 `anomaly_impact` / `anomaly_burst`；
5. 用人工标签在 development 数据上锁定阈值，再在 held-out 数据上报告 event F1、false alarms/hour
   和 P95 detection delay；
6. 若研究完整设备延迟，增加外部声源/回环共同时间基准。

上述闭环完成以前，不开始 CUDA、AI、FPGA 或运营优化模型。
