#include "hero_audio/fft_backend.hpp"

#include <fftw3.h>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace hero_audio {
namespace {

class FFTWBackend final : public FFTBackend {
public:
  explicit FFTWBackend(std::size_t size) : size_(size) {
    if (size_ == 0 || size_ > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument("FFTW size must fit in a positive int");
    }
    input_ = fftwf_alloc_real(size_);
    output_ = fftwf_alloc_complex(size_ / 2 + 1);
    if (input_ == nullptr || output_ == nullptr) {
      fftwf_free(input_);
      fftwf_free(output_);
      throw std::bad_alloc();
    }
    plan_ = fftwf_plan_dft_r2c_1d(static_cast<int>(size_), input_, output_, FFTW_MEASURE);
    if (plan_ == nullptr) {
      fftwf_free(input_);
      fftwf_free(output_);
      throw std::runtime_error("FFTW plan creation failed");
    }
  }

  ~FFTWBackend() override {
    fftwf_destroy_plan(plan_);
    fftwf_free(input_);
    fftwf_free(output_);
  }
  FFTWBackend(const FFTWBackend &) = delete;
  FFTWBackend &operator=(const FFTWBackend &) = delete;

  [[nodiscard]] std::string_view name() const noexcept override { return "fftw3f"; }
  [[nodiscard]] std::size_t fft_size() const noexcept override { return size_; }

  void execute(std::span<const float> input,
               std::span<std::complex<float>> output) override {
    if (input.size() != size_ || output.size() != size_ / 2 + 1) {
      throw std::invalid_argument("FFT buffer size mismatch");
    }
    std::copy(input.begin(), input.end(), input_);
    fftwf_execute(plan_);
    for (std::size_t bin = 0; bin < output.size(); ++bin) {
      output[bin] = {output_[bin][0], output_[bin][1]};
    }
  }

private:
  std::size_t size_;
  float *input_{nullptr};
  fftwf_complex *output_{nullptr};
  fftwf_plan plan_{nullptr};
};

} // namespace

std::unique_ptr<FFTBackend> make_fftw_backend(std::size_t fft_size) {
  return std::make_unique<FFTWBackend>(fft_size);
}

} // namespace hero_audio
