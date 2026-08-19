#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hero_audio {

enum class FFTBackendKind { Reference, FFTW };

class FFTBackend {
public:
  virtual ~FFTBackend() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::size_t fft_size() const noexcept = 0;

  // R2C output uses the unnormalised forward-transform convention and contains
  // fft_size()/2 + 1 bins. Implementations must be safe for repeated execution.
  virtual void execute(std::span<const float> input,
                       std::span<std::complex<float>> output) = 0;
};

[[nodiscard]] std::unique_ptr<FFTBackend> make_fft_backend(FFTBackendKind kind,
                                                           std::size_t fft_size);
[[nodiscard]] std::vector<FFTBackendKind> available_fft_backends();
[[nodiscard]] std::string_view backend_name(FFTBackendKind kind) noexcept;

} // namespace hero_audio
