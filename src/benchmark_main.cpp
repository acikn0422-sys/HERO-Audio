#include "hero_audio/fft_backend.hpp"
#include "hero_audio/offline_benchmark.hpp"

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
  std::size_t warmup_runs{3};
  std::size_t measured_runs{5};
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
    throw std::invalid_argument("Usage: hero-audio-bench input.wav raw-runs.csv summary.json "
                                "[--warmup 3] [--runs 5] [--backend auto|fftw|reference]");
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
    if (option == "--warmup") {
      result.warmup_runs = parse_size(value, option);
    } else if (option == "--runs") {
      result.measured_runs = parse_size(value, option);
    } else if (option == "--backend") {
      result.backend = value;
      if (result.backend != "auto" && result.backend != "fftw" && result.backend != "reference") {
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

    // Plan construction is intentionally measured around the factory call and
    // reported separately from every steady-state whole-file run.
    const auto initialization_start = std::chrono::steady_clock::now();
    auto fft = hero_audio::make_fft_backend(backend_kind, 1024);
    const auto initialization_end = std::chrono::steady_clock::now();
    const double backend_initialization_ms =
        std::chrono::duration<double, std::milli>(initialization_end - initialization_start)
            .count();

    const auto benchmark =
        hero_audio::benchmark_offline_wav(arguments.input, *fft, backend_initialization_ms,
                                          hero_audio::OfflineBenchmarkConfig{
                                              .warmup_runs = arguments.warmup_runs,
                                              .measured_runs = arguments.measured_runs,
                                          });
    hero_audio::write_offline_benchmark_runs_csv(arguments.raw_csv, benchmark.runs);
    hero_audio::write_offline_benchmark_summary_json(arguments.summary_json, benchmark);

    const auto &summary = benchmark.summary;
    std::cout << std::fixed << std::setprecision(6) << "metric: WAV read -> onset CSV serialized\n"
              << "backend: " << benchmark.backend_name << '\n'
              << "backend_initialization_ms: " << benchmark.backend_initialization_ms << '\n'
              << "warmup_runs: " << benchmark.config.warmup_runs << '\n'
              << "measured_runs: " << benchmark.config.measured_runs << '\n'
              << "median_elapsed_ms: " << summary.median_elapsed_ms << '\n'
              << "p95_elapsed_ms: " << summary.p95_elapsed_ms << '\n'
              << "p99_elapsed_ms: " << summary.p99_elapsed_ms << '\n'
              << "median_real_time_factor: " << summary.median_real_time_factor << '\n'
              << "minimum_real_time_factor: " << summary.minimum_real_time_factor << '\n'
              << "raw_csv: " << arguments.raw_csv.string() << '\n'
              << "summary_json: " << arguments.summary_json.string() << '\n';
    if (backend_kind == hero_audio::FFTBackendKind::Reference) {
      std::cout << "warning: reference-radix2 is a correctness backend, not the official CPU "
                   "performance baseline\n";
    }
    return 0;
  } catch (const std::invalid_argument &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
