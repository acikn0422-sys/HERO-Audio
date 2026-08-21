#include "hero_audio/causal_onset.hpp"
#include "hero_audio/fft_backend.hpp"
#include "hero_audio/spectral_flux.hpp"
#include "hero_audio/wav_reader.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
  if (argc > 5) {
    std::cerr << "Usage: hero-audio [input.wav] [spectral-flux.csv] [onsets.csv] "
                 "[diagnostics.csv]\n";
    return 2;
  }

  std::cout << "HERO-Audio v0.1 scaffold\nAvailable FFT backends:\n";
  for (const auto backend : hero_audio::available_fft_backends()) {
    std::cout << "  - " << hero_audio::backend_name(backend) << '\n';
  }

  if (argc >= 2) {
    try {
      const auto audio = hero_audio::read_wav(argv[1]);
      auto backend_kind = hero_audio::FFTBackendKind::Reference;
      for (const auto available : hero_audio::available_fft_backends()) {
        if (available == hero_audio::FFTBackendKind::FFTW) {
          backend_kind = available;
        }
      }
      auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
      const auto flux =
          hero_audio::compute_spectral_flux(audio.mono_samples, audio.sample_rate_hz, *fft);
      const hero_audio::CausalOnsetConfig onset_config;
      const auto onset_analysis = hero_audio::analyze_causal_onsets(flux, onset_config);
      const auto &onsets = onset_analysis.events;
      const auto maximum =
          std::max_element(flux.begin(), flux.end(), [](const auto &left, const auto &right) {
            return left.spectral_flux < right.spectral_flux;
          });
      std::cout << "WAV input:\n"
                << "  sample_rate_hz: " << audio.sample_rate_hz << '\n'
                << "  source_channels: " << audio.source_channels << '\n'
                << "  mono_samples: " << audio.mono_samples.size() << '\n'
                << "  duration_seconds: " << std::fixed << std::setprecision(6)
                << audio.duration_seconds() << '\n'
                << "Spectral Flux:\n"
                << "  backend: " << fft->name() << '\n'
                << "  frames: " << flux.size() << '\n'
                << "  maximum: " << (maximum == flux.end() ? 0.0F : maximum->spectral_flux) << '\n'
                << "Causal Onsets:\n"
                << "  count: " << onsets.size() << '\n'
                << "  history_frames: " << onset_config.threshold_history_frames << '\n'
                << "  stddev_multiplier: " << onset_config.threshold_stddev_multiplier << '\n'
                << "  refractory_ms: " << onset_config.refractory_seconds * 1000.0 << '\n';
      if (argc >= 3) {
        hero_audio::write_spectral_flux_csv(argv[2], flux);
        std::cout << "  csv: " << argv[2] << '\n';
      }
      if (argc >= 4) {
        hero_audio::write_onsets_csv(argv[3], onsets);
        std::cout << "  onsets_csv: " << argv[3] << '\n';
      }
      if (argc >= 5) {
        hero_audio::write_onset_diagnostics_csv(argv[4], onset_analysis.diagnostics);
        std::cout << "  diagnostics_csv: " << argv[4] << '\n';
      }
    } catch (const std::exception &error) {
      std::cerr << "Error: " << error.what() << '\n';
      return 1;
    }
  }
  return 0;
}
