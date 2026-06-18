#include "replay_config.h"
#include "replay_index.h"
#include "replay_timing.h"

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_run{true};

struct Args {
  fs::path data_dir;
  std::string config_path;
  double speed = 1.0;
  bool loop = false;
  bool quiet = false;
};

void on_signal(int) {
  g_run.store(false);
}

void print_usage(const char* app) {
  std::cerr << "Usage: " << app
            << " --dir <data_dir> [--config config.yaml] [--speed N] [--loop] [--quiet]\n"
            << "  --speed 1.0  realtime replay (default)\n"
            << "  --speed 0    publish as fast as possible\n";
}

bool parse_args(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--dir") {
      const char* v = value("--dir");
      if (!v) return false;
      args->data_dir = v;
    } else if (arg == "--config") {
      const char* v = value("--config");
      if (!v) return false;
      args->config_path = v;
    } else if (arg == "--speed") {
      const char* v = value("--speed");
      if (!v) return false;
      args->speed = std::stod(v);
      if (args->speed < 0.0) {
        std::cerr << "--speed must be >= 0\n";
        return false;
      }
    } else if (arg == "--loop") {
      args->loop = true;
    } else if (arg == "--quiet") {
      args->quiet = true;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
  }
  if (args->data_dir.empty()) {
    std::cerr << "--dir is required\n";
    return false;
  }
  return true;
}

std::string resolve_config_path(const char* executable_path,
                                const std::string& explicit_path) {
  if (!explicit_path.empty()) {
    return explicit_path;
  }
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

void unlink_ipc_endpoint(const std::string& endpoint) {
  constexpr const char* prefix = "ipc://";
  if (endpoint.rfind(prefix, 0) != 0) {
    return;
  }
  const std::string path = endpoint.substr(6);
  if (!path.empty()) {
    unlink(path.c_str());
  }
}

std::string publisher_key(const ReplayEvent& event) {
  return event.endpoint + "\n" + event.topic;
}

class RawPublisher {
public:
  RawPublisher(zmq::context_t* context, std::string endpoint, std::string topic)
      : endpoint_(std::move(endpoint)),
        topic_(std::move(topic)),
        socket_(*context, zmq::socket_type::pub) {
    socket_.set(zmq::sockopt::linger, 0);
    socket_.set(zmq::sockopt::sndhwm, 100);
    unlink_ipc_endpoint(endpoint_);
    socket_.bind(endpoint_);
  }

  void publish(const std::vector<uint8_t>& header, const std::string& raw) {
    socket_.send(zmq::buffer(topic_), zmq::send_flags::sndmore);
    if (raw.empty()) {
      socket_.send(zmq::const_buffer(header.data(), header.size()),
                   zmq::send_flags::none);
      return;
    }
    socket_.send(zmq::const_buffer(header.data(), header.size()),
                 zmq::send_flags::sndmore);
    socket_.send(zmq::const_buffer(raw.data(), raw.size()),
                 zmq::send_flags::none);
  }

  const std::string& endpoint() const { return endpoint_; }
  const std::string& topic() const { return topic_; }

private:
  std::string endpoint_;
  std::string topic_;
  zmq::socket_t socket_;
};

void sleep_until_event(int64_t first_event_ns, int64_t event_ns,
                       std::chrono::steady_clock::time_point replay_start,
                       double speed) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - replay_start)
          .count();
  const int64_t delay =
      replay_delay_ns(first_event_ns, event_ns, elapsed, speed);
  if (delay > 0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
  }
}

uint64_t replay_once(const std::vector<ReplayEvent>& events,
                     std::unordered_map<std::string, RawPublisher*>* publishers,
                     double speed, bool quiet) {
  uint64_t sent = 0;
  const int64_t first_event_ns =
      events.empty() ? 0 : events.front().timestamp_ns;
  const auto replay_start = std::chrono::steady_clock::now();
  auto last_log = std::chrono::steady_clock::now();
  uint64_t last_sent = 0;

  for (const ReplayEvent& event : events) {
    if (!g_run.load()) break;
    sleep_until_event(first_event_ns, event.timestamp_ns, replay_start, speed);

    auto it = publishers->find(publisher_key(event));
    if (it == publishers->end()) {
      continue;
    }
    const std::string raw =
        event.kind == ReplaySensorKind::RawImage ? read_event_raw(event)
                                                 : std::string();
    it->second->publish(event.header, raw);
    ++sent;

    const auto now = std::chrono::steady_clock::now();
    if (!quiet && now - last_log >= std::chrono::seconds(1)) {
      const double elapsed = std::chrono::duration<double>(now - last_log).count();
      const double hz = elapsed > 0.0
                            ? static_cast<double>(sent - last_sent) / elapsed
                            : 0.0;
      std::cout << "replayed=" << sent << "/" << events.size()
                << " hz=" << hz << std::endl;
      last_sent = sent;
      last_log = now;
    }
  }
  return sent;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, &args)) {
    print_usage(argv[0]);
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  try {
    const std::string config_path = resolve_config_path(argv[0], args.config_path);
    const ReplayConfig config = load_replay_config(config_path);
    const fs::path dataset = fs::absolute(args.data_dir);
    if (!fs::exists(dataset)) {
      std::cerr << "data directory not found: " << dataset << std::endl;
      return 1;
    }

    const std::vector<ReplayEvent> events =
        load_replay_events(dataset, config.topics);
    if (events.empty()) {
      std::cerr << "no replay events found under " << dataset << std::endl;
      return 1;
    }

    zmq::context_t context(1);
    std::vector<std::unique_ptr<RawPublisher>> publisher_storage;
    std::unordered_map<std::string, RawPublisher*> publishers;
    for (const ReplayEvent& event : events) {
      const std::string key = publisher_key(event);
      if (publishers.find(key) != publishers.end()) {
        continue;
      }
      publisher_storage.push_back(std::make_unique<RawPublisher>(
          &context, event.endpoint, event.topic));
      publishers[key] = publisher_storage.back().get();
      if (!args.quiet) {
        std::cout << "bound " << event.endpoint << " topic=" << event.topic
                  << std::endl;
      }
    }

    if (!args.quiet) {
      std::cout << "loaded " << events.size() << " events from " << dataset
                << " speed=" << args.speed << " loop=" << (args.loop ? "yes" : "no")
                << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    uint64_t total_sent = 0;
    do {
      total_sent += replay_once(events, &publishers, args.speed, args.quiet);
    } while (g_run.load() && args.loop);

    if (!args.quiet) {
      std::cout << "replay done, sent=" << total_sent << std::endl;
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "sensor_replay failed: " << ex.what() << std::endl;
    return 1;
  }
}
