# HERO-Audio

面向实时音频起始点检测的异构计算与运营优化研究。本仓库当前为 **v0.1 项目骨架**，首要平台是
macOS／Apple Silicon；第一阶段仅实现和验证 C++ CPU 检测闭环。

## 当前锁定的技术基线

- C++20、CMake 3.25+、Ninja；
- 正式 CPU FFT：FFTW3f（float32、1024-point R2C）；
- 项目内 radix-2 FFT 只用于正确性验证；
- 48 kHz、frame 1024、hop 256、Hann window；
- causal threshold；
- onset 评价采用 ±50 ms、最大匹配数优先且总误差最小的一对一匹配。

完整指标定义见 [docs/benchmark_protocol.md](docs/benchmark_protocol.md)。
从 Terminal、C++、CMake 到 GitHub 首次发布的分步说明见
[docs/step_by_step_implementation.md](docs/step_by_step_implementation.md)。

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
```

当前 WAV reader 支持 little-endian RIFF/WAVE、整数 PCM 8/16/24/32-bit、IEEE float32
以及 WAVE_FORMAT_EXTENSIBLE 中对应的 PCM/float 子格式。多声道输入按算术平均转换为 mono float32；
当前阶段不进行重采样。

Spectral Flux 默认使用 frame size 1024、hop size 256 和对称 Hann window。只处理完整帧，不对
尾部进行隐式补零。CSV 分别记录 frame start、center 和数据完整可用时刻；第一帧因为没有前一帧，
flux 固定为零。构建中存在 FFTW3f 时 CLI 优先使用 FFTW，否则使用 reference backend。

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
src/dsp/                  Hann、分帧与 Spectral Flux
tests/                    正确性测试
configs/                  版本化实验配置
docs/                     指标与实验协议
scripts/                  环境安装和元数据采集
results/raw/              不修改的原始结果
results/processed/        可重新生成的处理结果
```

## FFTW 许可提醒

FFTW 采用 GPL 许可。研究代码可保留自己的许可，但分发链接 FFTW 的二进制文件时必须评估并遵守
GPL 要求。若未来需要非 GPL 的商业分发方案，应单独选择或采购兼容后端；这不改变本研究中的
FFTW3f CPU baseline。
