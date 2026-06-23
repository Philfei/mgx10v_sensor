#include "GnssMsg.pb.h"
#include "ImuMsg.pb.h"
#include "RawImageMsg.pb.h"
#include "recorder_config.h"
#include "receiver_runtime.h"
#include "record_writer.h"

#include <google/protobuf/timestamp.pb.h>
#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<bool> g_run{true};
std::atomic<bool> g_signal_stop{false};
std::mutex g_log_mutex;

struct Args {
  std::string config_path;
  int duration_sec = 0;
  bool quiet = false;
};

struct TopicStats {
  uint64_t received = 0;
  uint64_t saved = 0;
  uint64_t parse_fail = 0;
  uint64_t missing_raw = 0;
};

void on_signal(int) {
  g_signal_stop.store(true);
  g_run.store(false);
}

void print_usage(const char* app) {
  std::fprintf(stderr,
               "Usage: %s [--config config.yaml] [--duration SEC] [--quiet]\n",
               app);
}

bool parse_args(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--config") {
      const char* value = need_value("--config");
      if (!value) {
        return false;
      }
      args->config_path = value;
    } else if (arg == "--duration") {
      const char* value = need_value("--duration");
      if (!value) {
        return false;
      }
      args->duration_sec = std::atoi(value);
    } else if (arg == "--quiet") {
      args->quiet = true;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return true;
}

std::string resolve_config_path(const char* executable_path,
                                const std::string& explicit_path) {
  namespace fs = std::filesystem;
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
  return (fs::current_path() / "config.yaml").string();
}

int64_t timestamp_to_ns(const google::protobuf::Timestamp& stamp) {
  return static_cast<int64_t>(stamp.seconds()) * 1'000'000'000LL +
         static_cast<int64_t>(stamp.nanos());
}

bool recv_multipart(zmq::socket_t& socket, std::string* topic,
                    zmq::message_t* payload, zmq::message_t* raw_payload,
                    bool* has_raw_payload) {
  zmq::message_t topic_msg;
  auto topic_ret = socket.recv(topic_msg, zmq::recv_flags::none);
  if (!topic_ret) {
    return false;
  }
  if (!socket.get(zmq::sockopt::rcvmore)) {
    return false;
  }

  auto payload_ret = socket.recv(*payload, zmq::recv_flags::none);
  if (!payload_ret) {
    return false;
  }

  topic->assign(static_cast<const char*>(topic_msg.data()), topic_msg.size());
  *has_raw_payload = false;
  if (socket.get(zmq::sockopt::rcvmore)) {
    auto raw_ret = socket.recv(*raw_payload, zmq::recv_flags::none);
    if (!raw_ret) {
      return false;
    }
    *has_raw_payload = true;
  }

  while (socket.get(zmq::sockopt::rcvmore)) {
    zmq::message_t extra;
    (void)socket.recv(extra, zmq::recv_flags::none);
  }
  return true;
}

std::string message_to_string(const zmq::message_t& message) {
  return std::string(static_cast<const char*>(message.data()),
                     message.size());
}

bool process_raw_image(const TopicConfig& topic, const zmq::message_t& payload,
                       const zmq::message_t& raw_payload,
                       bool has_raw_payload, RecordWriter* writer,
                       TopicStats* stats) {
  mgx10v::proto::RawImage msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    ++stats->parse_fail;
    return false;
  }

  std::string raw;
  if (has_raw_payload) {
    raw = message_to_string(raw_payload);
  } else if (!msg.data().empty()) {
    raw = msg.data();
  } else {
    ++stats->missing_raw;
  }

  writer->append(topic.name, timestamp_to_ns(msg.timestamp()),
                 message_to_string(payload), std::move(raw));
  ++stats->saved;
  return true;
}

bool process_imu(const TopicConfig& topic, const zmq::message_t& payload,
                 RecordWriter* writer, TopicStats* stats) {
  mgx10v::proto::ImuMsg msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    ++stats->parse_fail;
    return false;
  }

  writer->append(topic.name, timestamp_to_ns(msg.header().stamp()),
                 message_to_string(payload), {});
  ++stats->saved;
  return true;
}

bool process_gnss(const TopicConfig& topic, const zmq::message_t& payload,
                  RecordWriter* writer, TopicStats* stats) {
  mgx10v::proto::GnssMsg msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    ++stats->parse_fail;
    return false;
  }

  writer->append(topic.name, timestamp_to_ns(msg.header().stamp()),
                 message_to_string(payload), {});
  ++stats->saved;
  return true;
}

void log_status(const TopicConfig& topic, const TopicStats& stats,
                uint64_t last_saved, double elapsed_s) {
  const double hz = elapsed_s > 0.0
                        ? static_cast<double>(stats.saved - last_saved) /
                              elapsed_s
                        : 0.0;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cout << topic.name << " type=" << topic_type_name(topic.type)
            << " saved=" << stats.saved << " hz=" << std::fixed
            << std::setprecision(2) << hz
            << " parse_fail=" << stats.parse_fail
            << " missing_raw=" << stats.missing_raw << std::endl;
}

void receive_topic(const ReceiverRuntime runtime, RecordWriter* writer,
                   bool quiet) {
  const TopicConfig& topic = runtime.topic;
  TopicStats stats;

  try {
    zmq::context_t context(runtime.context_io_threads);
    zmq::socket_t subscriber(context, zmq::socket_type::sub);
    subscriber.set(zmq::sockopt::linger, 0);
    subscriber.set(zmq::sockopt::subscribe, topic.topic);
    subscriber.set(zmq::sockopt::rcvtimeo, 500);
    subscriber.connect(topic.endpoint);

    {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      std::cout << "recording " << topic.name << " endpoint="
                << topic.endpoint << " topic=" << topic.topic
                << " type=" << topic_type_name(topic.type)
                << " context_id=" << runtime.context_id
                << " io_threads=" << runtime.context_io_threads << std::endl;
    }

    auto last_log = std::chrono::steady_clock::now();
    uint64_t last_saved = 0;

    while (g_run.load()) {
      zmq::message_t payload;
      zmq::message_t raw_payload;
      std::string received_topic;
      bool has_raw_payload = false;
      if (!recv_multipart(subscriber, &received_topic, &payload,
                          &raw_payload, &has_raw_payload)) {
        continue;
      }

      ++stats.received;
      if (received_topic != topic.topic) {
        continue;
      }

      switch (topic.type) {
        case TopicType::RawImage:
          process_raw_image(topic, payload, raw_payload, has_raw_payload,
                            writer, &stats);
          break;
        case TopicType::Imu:
          process_imu(topic, payload, writer, &stats);
          break;
        case TopicType::Gnss:
          process_gnss(topic, payload, writer, &stats);
          break;
      }

      const auto now = std::chrono::steady_clock::now();
      if (!quiet && now - last_log >= std::chrono::seconds(1)) {
        const double elapsed_s =
            std::chrono::duration<double>(now - last_log).count();
        log_status(topic, stats, last_saved, elapsed_s);
        last_saved = stats.saved;
        last_log = now;
      }
    }
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "receiver for " << topic.name << " stopped: " << e.what()
              << std::endl;
  }

  if (!quiet) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "stopped " << topic.name << " saved=" << stats.saved
              << " parse_fail=" << stats.parse_fail
              << " missing_raw=" << stats.missing_raw << std::endl;
  }
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
    const std::string config_path =
        resolve_config_path(argv[0], args.config_path);
    RecorderConfig config = load_recorder_config(config_path);
    RecordWriter writer(config.data_root, config.chunk_duration_s);
    const std::vector<ReceiverRuntime> runtimes =
        make_receiver_runtimes(config.topics);
    for (const ReceiverRuntime& runtime : runtimes) {
      writer.set_stream_flush_on_close(runtime.topic.name,
                                       runtime.topic.type != TopicType::RawImage);
    }

    std::cout << "sensor_recorder config=" << config_path << std::endl;
    std::cout << "session_dir=" << writer.session_dir()
              << " chunk_duration_s=" << config.chunk_duration_s
              << " topics=" << runtimes.size() << std::endl;

    std::vector<std::thread> threads;
    threads.reserve(runtimes.size());
    for (const ReceiverRuntime& runtime : runtimes) {
      threads.emplace_back(receive_topic, runtime, &writer, args.quiet);
    }

    const auto start = std::chrono::steady_clock::now();
    while (g_run.load()) {
      if (args.duration_sec > 0 &&
          std::chrono::steady_clock::now() - start >=
              std::chrono::seconds(args.duration_sec)) {
        g_run.store(false);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (auto& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    const auto close_mode = g_signal_stop.load()
                                ? RecordWriter::CloseMode::DropPending
                                : RecordWriter::CloseMode::Flush;
    writer.close(close_mode);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "sensor_recorder failed: " << e.what() << std::endl;
    return 1;
  }
}
