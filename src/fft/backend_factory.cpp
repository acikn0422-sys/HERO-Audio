#include "hero_audio/fft_backend.hpp"

#include <stdexcept>

namespace hero_audio {

std::unique_ptr<FFTBackend> make_reference_backend(std::size_t fft_size);
#ifdef HERO_AUDIO_HAS_FFTW3F
std::unique_ptr<FFTBackend> make_fftw_backend(std::size_t fft_size);
#endif

std::unique_ptr<FFTBackend> make_fft_backend(FFTBackendKind kind, std::size_t fft_size) {
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
  throw std::invalid_argument("Unknown FFT backend");
}

std::vector<FFTBackendKind> available_fft_backends() {
  std::vector<FFTBackendKind> backends{FFTBackendKind::Reference};
#ifdef HERO_AUDIO_HAS_FFTW3F
  backends.push_back(FFTBackendKind::FFTW);
#endif
  return backends;
}

std::string_view backend_name(FFTBackendKind kind) noexcept {
  switch (kind) {
  case FFTBackendKind::Reference:
    return "reference-radix2";
  case FFTBackendKind::FFTW:
    return "fftw3f";
  }
  return "unknown";
}

} // namespace hero_audio
