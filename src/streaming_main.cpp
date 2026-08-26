#include "hero_audio/fft_backend.hpp"
#include "hero_audio/streaming_benchmark.hpp"

#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct Arguments {
  std::filesystem::path input;
  std::filesystem::path raw_csv;
  std::filesystem::path summary_json;
  std::size_t warmup_passes{1};
  std::size_t measured_passes{5};
  std::string backend{"auto"};
};

std::size_t parse_size(std::string_view text, std::string_view option) {
  std::size_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::invalid_argument(std::string(option) + " requires a non-negative integer");
  }
  return value;
}

Arguments parse_arguments(int argc, char **argv) {
  if (argc < 4) {
    throw std::invalid_argument(
        "Usage: hero-audio-stream input.wav raw-hops.csv summary.json "
        "[--warmup-passes 1] [--passes 5] [--backend auto|fftw|reference]");
  }
  Arguments result{
      .input = argv[1],
      .raw_csv = argv[2],
      .summary_json = argv[3],
  };
  for (int index = 4; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (index + 1 >= argc) {
      throw std::invalid_argument(std::string(option) + " requires a value");
    }
    const std::string_view value = argv[++index];
    if (option == "--warmup-passes") {
      result.warmup_passes = parse_size(value, option);
    } else if (option == "--passes") {
      result.measured_passes = parse_size(value, option);
    } else if (option == "--backend") {
      result.backend = value;
      if (result.backend != "auto" && result.backend != "fftw" &&
          result.backend != "reference") {
        throw std::invalid_argument("--backend must be auto, fftw, or reference");
      }
    } else {
      throw std::invalid_argument("Unknown option: " + std::string(option));
    }
  }
  return result;
}

hero_audio::FFTBackendKind select_backend(std::string_view requested) {
  if (requested == "reference") {
    return hero_audio::FFTBackendKind::Reference;
  }
  if (requested == "fftw") {
    for (const auto kind : hero_audio::available_fft_backends()) {
      if (kind == hero_audio::FFTBackendKind::FFTW) {
        return kind;
      }
    }
    throw std::runtime_error("FFTW3f was requested but is unavailable in this build");
  }
  for (const auto kind : hero_audio::available_fft_backends()) {
    if (kind == hero_audio::FFTBackendKind::FFTW) {
      return kind;
    }
  }
  return hero_audio::FFTBackendKind::Reference;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto backend_kind = select_backend(arguments.backend);

    // FFTW planning belongs to initialization, not steady-state hop compute.
    const auto initialization_start = std::chrono::steady_clock::now();
    auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
    const auto initialization_end = std::chrono::steady_clock::now();
    const double backend_initialization_ms =
        std::chrono::duration<double, std::milli>(initialization_end - initialization_start)
            .count();

    const auto benchmark = hero_audio::benchmark_streaming_wav(
        arguments.input, *fft, backend_initialization_ms,
        hero_audio::StreamingBenchmarkConfig{
            .warmup_passes = arguments.warmup_passes,
            .measured_passes = arguments.measured_passes,
        });
    hero_audio::write_streaming_hop_measurements_csv(arguments.raw_csv,
                                                     benchmark.measurements);
    hero_audio::write_streaming_benchmark_summary_json(arguments.summary_json, benchmark);

    const auto &summary = benchmark.summary;
    std::cout << std::fixed << std::setprecision(6)
              << "metric: steady-state hop compute\n"
              << "timed_scope: hop insert -> causal detection complete\n"
              << "backend: " << benchmark.backend_name << '\n'
              << "backend_initialization_ms: " << benchmark.backend_initialization_ms << '\n'
              << "sample_rate_hz: " << benchmark.sample_rate_hz << '\n'
              << "hop_size_samples: " << benchmark.config.spectral_flux.hop_size_samples << '\n'
              << "hop_period_ms: " << summary.hop_period_ms << '\n'
              << "measured_hop_count: " << summary.measured_hop_count << '\n'
              << "p50_compute_ms: " << summary.p50_compute_ms << '\n'
              << "p95_compute_ms: " << summary.p95_compute_ms << '\n'
              << "p99_compute_ms: " << summary.p99_compute_ms << '\n'
              << "maximum_compute_ms: " << summary.maximum_compute_ms << '\n'
              << "deadline_miss_count: " << summary.deadline_miss_count << '\n'
              << "p95_within_hop_period: "
              << (summary.p95_within_hop_period ? "true" : "false") << '\n'
              << "ignored_tail_sample_count: " << benchmark.ignored_tail_sample_count << '\n'
              << "offline_consistency: "
              << (benchmark.consistency.overall_matches ? "PASS" : "FAIL") << '\n'
              << "maximum_flux_absolute_error: "
              << benchmark.consistency.maximum_flux_absolute_error << '\n'
              << "raw_csv: " << arguments.raw_csv.string() << '\n'
              << "summary_json: " << arguments.summary_json.string() << '\n';
    if (backend_kind == hero_audio::FFTBackendKind::Reference) {
      std::cout << "warning: reference-radix2 is a correctness backend, not the official CPU "
                   "performance baseline\n";
    }
    return benchmark.consistency.overall_matches ? 0 : 3;
  } catch (const std::invalid_argument &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
