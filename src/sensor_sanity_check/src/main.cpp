#include "chunk_reader.h"
#include "sanity_config.h"
#include "timestamp_extractor.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000LL;
constexpr int kTopGapCount = 5;

struct SensorSpec {
  std::string name;
  SensorKind kind = SensorKind::RawImage;
  double gap_threshold_s = 0.1;
};

struct TimestampSample {
  int64_t timestamp_ns = 0;
  uint32_t file_index = 0;
};

struct GapInfo {
  int64_t start_ns = 0;
  int64_t end_ns = 0;
  double duration_s = 0.0;
  std::string start_file;
  std::string end_file;
};

struct SensorReport {
  SensorSpec spec;
  fs::path directory;
  std::string status = "ok";
  size_t total_files = 0;
  uint64_t records = 0;
  uint64_t bytes = 0;
  size_t timestamps = 0;
  size_t missing_timestamps = 0;
  size_t zero_raw_records = 0;
  int64_t start_ns = 0;
  int64_t end_ns = 0;
  double duration_s = 0.0;
  double frequency_hz = 0.0;
  std::vector<std::string> invalid_files;
  std::vector<GapInfo> gaps;
  GapInfo longest_interval;
  bool has_longest_interval = false;
};

struct CliOptions {
  fs::path data_dir = "/root/sensor_receiver/data";
  bool detailed = false;
  bool show_help = false;
  bool valid = true;
};

std::vector<SensorSpec> default_specs() {
  return {
      {"cam_left", SensorKind::RawImage, 0.07},
      {"cam_right", SensorKind::RawImage, 0.07},
      {"imu", SensorKind::Imu, 0.011},
      {"gnss", SensorKind::Gnss, 0.11},
  };
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_chunk_file(const fs::path& path) {
  return ends_with(path.filename().string(), "_chunk.dat");
}

std::string timestamp_to_string(int64_t ns) {
  if (ns <= 0) {
    return "N/A";
  }
  std::time_t seconds = static_cast<std::time_t>(ns / kNsPerSecond);
  const int64_t remainder = ns % kNsPerSecond;
  std::tm tm_time {};
#ifdef _WIN32
  localtime_s(&tm_time, &seconds);
#else
  localtime_r(&seconds, &tm_time);
#endif
  std::ostringstream out;
  out << std::put_time(&tm_time, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setw(3) << std::setfill('0') << (remainder / 1'000'000);
  return out.str();
}

void print_usage(const char* app) {
  std::cout << "Usage: " << app << " [--dir <path>] [--detailed]\n\n"
            << "Options:\n"
            << "  --dir <path>      Dataset directory or root containing data_* dirs\n"
            << "  --detailed, -d    Print all detected gaps\n"
            << "  --help, -h        Show this help\n";
}

CliOptions parse_args(int argc, char** argv) {
  CliOptions options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--dir") {
      if (i + 1 >= argc) {
        std::cerr << "--dir requires a value\n";
        options.valid = false;
        return options;
      }
      options.data_dir = argv[++i];
    } else if (arg.rfind("--dir=", 0) == 0) {
      options.data_dir = arg.substr(6);
    } else if (arg == "--detailed" || arg == "-d") {
      options.detailed = true;
    } else if (arg == "--help" || arg == "-h") {
      options.show_help = true;
      return options;
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      options.valid = false;
      return options;
    }
  }
  return options;
}

std::string resolve_config_path(const char* executable_path) {
  const fs::path exec_dir = fs::absolute(executable_path).parent_path();
  const fs::path candidates[] = {
      exec_dir / "config.yaml",
      fs::current_path() / "config.yaml",
      (exec_dir / "../config.yaml").lexically_normal(),
  };
  for (const auto& candidate : candidates) {
    if (fs::exists(candidate)) {
      return candidate.string();
    }
  }
  return {};
}

void apply_thresholds(std::vector<SensorSpec>* specs,
                      const std::unordered_map<std::string, double>& values) {
  for (auto& spec : *specs) {
    auto it = values.find(to_lower(spec.name));
    if (it != values.end()) {
      spec.gap_threshold_s = it->second;
    }
  }
}

std::vector<fs::path> collect_chunk_files(const fs::path& dir) {
  std::vector<fs::path> files;
  if (!fs::exists(dir) || !fs::is_directory(dir)) {
    return files;
  }
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && is_chunk_file(entry.path())) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<GapInfo> detect_gaps(const std::vector<TimestampSample>& timestamps,
                                 const std::vector<std::string>& file_names,
                                 double threshold_s) {
  std::vector<GapInfo> gaps;
  if (timestamps.size() < 2) {
    return gaps;
  }
  const int64_t threshold_ns =
      static_cast<int64_t>(threshold_s * static_cast<double>(kNsPerSecond));
  for (size_t i = 1; i < timestamps.size(); ++i) {
    const int64_t delta_ns =
        timestamps[i].timestamp_ns - timestamps[i - 1].timestamp_ns;
    if (delta_ns <= threshold_ns) {
      continue;
    }
    gaps.push_back({
        timestamps[i - 1].timestamp_ns,
        timestamps[i].timestamp_ns,
        static_cast<double>(delta_ns) / static_cast<double>(kNsPerSecond),
        file_names[timestamps[i - 1].file_index],
        file_names[timestamps[i].file_index],
    });
  }
  return gaps;
}

SensorReport analyze_sensor(const fs::path& dataset, const SensorSpec& spec) {
  SensorReport report;
  report.spec = spec;
  report.directory = dataset / spec.name;

  if (!fs::exists(report.directory) || !fs::is_directory(report.directory)) {
    report.status = "missing";
    return report;
  }

  const auto files = collect_chunk_files(report.directory);
  report.total_files = files.size();
  if (files.empty()) {
    report.status = "no-files";
    return report;
  }

  std::vector<std::string> file_names;
  file_names.reserve(files.size());
  for (const auto& path : files) {
    file_names.push_back(path.filename().string());
  }

  std::vector<TimestampSample> timestamps;
  bool needs_sort = false;
  for (size_t file_index = 0; file_index < files.size(); ++file_index) {
    try {
      const ChunkReadStats stats = for_each_record_header(
          files[file_index],
          [&](const std::vector<uint8_t>& header, uint32_t raw_size) {
            if (spec.kind == SensorKind::RawImage && raw_size == 0) {
              ++report.zero_raw_records;
            }
            const auto ts = extract_sensor_timestamp_ns(header, spec.kind);
            if (!ts.has_value()) {
              ++report.missing_timestamps;
              return;
            }
            if (!timestamps.empty() && *ts < timestamps.back().timestamp_ns) {
              needs_sort = true;
            }
            timestamps.push_back({*ts, static_cast<uint32_t>(file_index)});
          });
      report.records += stats.records;
      report.bytes += stats.bytes;
      if (stats.records == 0) {
        report.invalid_files.push_back(file_names[file_index] + ": no records");
      }
    } catch (const std::exception& ex) {
      report.invalid_files.push_back(file_names[file_index] + ": " + ex.what());
    }
  }

  if (timestamps.empty()) {
    report.status = "no-valid-timestamps";
    return report;
  }

  if (needs_sort) {
    std::sort(timestamps.begin(), timestamps.end(),
              [](const TimestampSample& a, const TimestampSample& b) {
                if (a.timestamp_ns != b.timestamp_ns) {
                  return a.timestamp_ns < b.timestamp_ns;
                }
                return a.file_index < b.file_index;
              });
  }

  report.timestamps = timestamps.size();
  report.start_ns = timestamps.front().timestamp_ns;
  report.end_ns = timestamps.back().timestamp_ns;
  report.duration_s =
      report.timestamps > 1
          ? static_cast<double>(report.end_ns - report.start_ns) /
                static_cast<double>(kNsPerSecond)
          : 0.0;
  report.frequency_hz =
      report.timestamps > 1 && report.duration_s > 0.0
          ? static_cast<double>(report.timestamps - 1) / report.duration_s
          : 0.0;

  int64_t max_delta_ns = -1;
  for (size_t i = 1; i < timestamps.size(); ++i) {
    const int64_t delta_ns =
        timestamps[i].timestamp_ns - timestamps[i - 1].timestamp_ns;
    if (delta_ns > max_delta_ns) {
      max_delta_ns = delta_ns;
      report.longest_interval = {
          timestamps[i - 1].timestamp_ns,
          timestamps[i].timestamp_ns,
          static_cast<double>(delta_ns) / static_cast<double>(kNsPerSecond),
          file_names[timestamps[i - 1].file_index],
          file_names[timestamps[i].file_index],
      };
      report.has_longest_interval = true;
    }
  }
  report.gaps = detect_gaps(timestamps, file_names, spec.gap_threshold_s);
  return report;
}

bool dataset_contains_any_sensor(const fs::path& path,
                                 const std::vector<SensorSpec>& specs) {
  for (const auto& spec : specs) {
    if (fs::is_directory(path / spec.name)) {
      return true;
    }
  }
  return false;
}

std::vector<fs::path> find_datasets(const fs::path& root,
                                    const std::vector<SensorSpec>& specs) {
  std::vector<fs::path> datasets;
  if (dataset_contains_any_sensor(root, specs)) {
    datasets.push_back(root);
    return datasets;
  }
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_directory() && dataset_contains_any_sensor(entry.path(), specs)) {
      datasets.push_back(entry.path());
    }
  }
  std::sort(datasets.begin(), datasets.end());
  return datasets;
}

void print_sensor_report(const SensorReport& report) {
  std::cout << "\n[" << report.spec.name << "]\n";
  if (report.status == "missing") {
    std::cout << "  Status: missing\n";
    return;
  }
  if (report.status == "no-files") {
    std::cout << "  Status: no _chunk.dat files in " << report.directory << '\n';
    return;
  }
  if (report.status == "no-valid-timestamps") {
    std::cout << "  Status: no valid timestamps\n";
    std::cout << "  Files : " << report.total_files
              << " invalid=" << report.invalid_files.size()
              << " missing_ts=" << report.missing_timestamps << '\n';
    return;
  }

  std::cout << "  Directory: " << report.directory << '\n';
  std::cout << "  Files    : " << report.total_files << '\n';
  std::cout << "  Records  : " << report.records << '\n';
  std::cout << "  Samples  : " << report.timestamps << '\n';
  std::cout << "  Start    : " << timestamp_to_string(report.start_ns) << '\n';
  std::cout << "  End      : " << timestamp_to_string(report.end_ns) << '\n';
  std::cout << "  Duration : " << std::fixed << std::setprecision(3)
            << report.duration_s << " s\n";
  std::cout << "  Freq     : " << std::fixed << std::setprecision(2)
            << report.frequency_hz << " Hz\n";
  if (report.missing_timestamps > 0 || report.zero_raw_records > 0 ||
      !report.invalid_files.empty()) {
    std::cout << "  Invalid  : files=" << report.invalid_files.size()
              << " missing_ts=" << report.missing_timestamps
              << " zero_raw=" << report.zero_raw_records << '\n';
  }
  if (report.gaps.empty()) {
    std::cout << "  Gaps     : none (threshold " << std::fixed
              << std::setprecision(3) << report.spec.gap_threshold_s << "s)\n";
    if (report.has_longest_interval) {
      std::cout << "             Longest interval "
                << report.longest_interval.duration_s << "s, files "
                << report.longest_interval.start_file << " -> "
                << report.longest_interval.end_file << '\n';
    }
    return;
  }

  std::cout << "  Gaps     : " << report.gaps.size() << " gap(s)\n";
  auto top = report.gaps;
  std::sort(top.begin(), top.end(),
            [](const GapInfo& a, const GapInfo& b) {
              return a.duration_s > b.duration_s;
            });
  if (top.size() > kTopGapCount) {
    top.resize(kTopGapCount);
  }
  for (const auto& gap : top) {
    std::cout << "    - " << timestamp_to_string(gap.start_ns) << " -> "
              << timestamp_to_string(gap.end_ns) << " (" << std::fixed
              << std::setprecision(3) << gap.duration_s << "s)\n";
  }
}

void print_dataset_report(const fs::path& dataset,
                          const std::vector<SensorReport>& reports) {
  const std::string separator(80, '=');
  std::cout << separator << '\n';
  std::cout << "DATASET: " << dataset << '\n';
  std::cout << separator << '\n';
  for (const auto& report : reports) {
    print_sensor_report(report);
  }
}

void print_detailed_gaps(
    const std::vector<std::pair<fs::path, std::vector<SensorReport>>>& all) {
  std::cout << "\n" << std::string(80, '=') << '\n'
            << "DETAILED DATA GAPS REPORT\n"
            << std::string(80, '=') << '\n';
  bool any = false;
  for (const auto& item : all) {
    for (const auto& report : item.second) {
      for (const auto& gap : report.gaps) {
        any = true;
        std::cout << item.first.filename().string() << " "
                  << report.spec.name << " "
                  << timestamp_to_string(gap.start_ns) << " -> "
                  << timestamp_to_string(gap.end_ns) << " "
                  << std::fixed << std::setprecision(3)
                  << gap.duration_s << "s files " << gap.start_file
                  << " -> " << gap.end_file << '\n';
      }
    }
  }
  if (!any) {
    std::cout << "No data gaps found.\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options = parse_args(argc, argv);
  if (options.show_help) {
    print_usage(argv[0]);
    return 0;
  }
  if (!options.valid) {
    print_usage(argv[0]);
    return 1;
  }

  std::vector<SensorSpec> specs = default_specs();
  const std::string config_path = resolve_config_path(argv[0]);
  if (!config_path.empty()) {
    try {
      apply_thresholds(&specs, load_gap_thresholds(config_path));
    } catch (const std::exception& ex) {
      std::cerr << "warning: " << ex.what() << ", using defaults\n";
    }
  }

  const fs::path root = fs::absolute(options.data_dir);
  if (!fs::exists(root)) {
    std::cerr << "path not found: " << root << '\n';
    return 1;
  }

  const auto datasets = find_datasets(root, specs);
  if (datasets.empty()) {
    std::cerr << "no sensor_recorder data directories found under " << root
              << '\n';
    return 1;
  }

  std::vector<std::pair<fs::path, std::vector<SensorReport>>> all_reports;
  for (const auto& dataset : datasets) {
    std::vector<SensorReport> reports;
    for (const auto& spec : specs) {
      reports.push_back(analyze_sensor(dataset, spec));
    }
    print_dataset_report(dataset, reports);
    all_reports.emplace_back(dataset, std::move(reports));
  }
  if (options.detailed) {
    print_detailed_gaps(all_reports);
  }
  return 0;
}
