#include "hero_audio/fft_backend.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace hero_audio {
namespace {

class ReferenceBackend final : public FFTBackend {
public:
  explicit ReferenceBackend(std::size_t size) : size_(size) {
    if (size_ == 0 || (size_ & (size_ - 1)) != 0) {
      throw std::invalid_argument("Reference FFT size must be a non-zero power of two");
    }
    scratch_.resize(size_);
  }

  [[nodiscard]] std::string_view name() const noexcept override { return "reference-radix2"; }
  [[nodiscard]] std::size_t fft_size() const noexcept override { return size_; }

  void execute(std::span<const float> input,
               std::span<std::complex<float>> output) override {
    if (input.size() != size_ || output.size() != size_ / 2 + 1) {
      throw std::invalid_argument("FFT buffer size mismatch");
    }

    for (std::size_t i = 0; i < size_; ++i) {
      scratch_[bit_reverse(i)] = {input[i], 0.0F};
    }

    for (std::size_t length = 2; length <= size_; length *= 2) {
      const float angle = -2.0F * std::numbers::pi_v<float> / static_cast<float>(length);
      const std::complex<float> root{std::cos(angle), std::sin(angle)};
      for (std::size_t start = 0; start < size_; start += length) {
        std::complex<float> twiddle{1.0F, 0.0F};
        for (std::size_t offset = 0; offset < length / 2; ++offset) {
          const auto even = scratch_[start + offset];
          const auto odd = scratch_[start + offset + length / 2] * twiddle;
          scratch_[start + offset] = even + odd;
          scratch_[start + offset + length / 2] = even - odd;
          twiddle *= root;
        }
      }
    }

    for (std::size_t bin = 0; bin < output.size(); ++bin) {
      output[bin] = scratch_[bin];
    }
  }

private:
  [[nodiscard]] std::size_t bit_reverse(std::size_t value) const noexcept {
    std::size_t result = 0;
    for (std::size_t remaining = size_; remaining > 1; remaining >>= 1) {
      result = (result << 1U) | (value & 1U);
      value >>= 1U;
    }
    return result;
  }

  std::size_t size_;
  std::vector<std::complex<float>> scratch_;
};

} // namespace

std::unique_ptr<FFTBackend> make_reference_backend(std::size_t fft_size) {
  return std::make_unique<ReferenceBackend>(fft_size);
}

} // namespace hero_audio
