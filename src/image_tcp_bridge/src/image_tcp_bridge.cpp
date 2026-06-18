#include "ImuMsg.pb.h"
#include "RawImageMsg.pb.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <zmq.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace {

std::atomic<bool> g_run{true};

struct Config {
  std::string cam_left_endpoint = "ipc:///tmp/cam_left";
  std::string cam_right_endpoint = "ipc:///tmp/cam_right";
  std::string imu_endpoint = "ipc:///tmp/imu_data";
  std::string cam_left_topic = "cam_left_topic";
  std::string cam_right_topic = "cam_right_topic";
  std::string imu_topic = "imu_topic";
  std::string image_pub_endpoint = "tcp://*:5560";
  std::string control_endpoint = "tcp://*:5561";
  std::string save_dir = "/root/sensor_receiver/snapshot";
  int out_width = 1280;
  int out_height = 720;
  int jpeg_quality = 80;
  int stat_interval_ms = 1000;
  int duration_sec = 0;
  bool quiet = false;
};

struct DecodeStats {
  uint64_t count = 0;
  uint64_t last_count = 0;
  double total_ms = 0.0;
  double interval_ms = 0.0;
  double max_ms = 0.0;
  double last_ms = 0.0;
};

struct ImageState {
  cv::Mat latest_bgr;
  int64_t latest_sec = 0;
  int32_t latest_nsec = 0;
  uint32_t latest_width = 0;
  uint32_t latest_height = 0;
  std::string latest_encoding;
  uint64_t publish_count = 0;
  uint64_t last_publish_count = 0;
  DecodeStats decode;
  DecodeStats resize;
  DecodeStats encode;
};

struct ImuSample {
  int64_t sec = 0;
  int32_t nsec = 0;
  double ax = 0.0;
  double ay = 0.0;
  double az = 0.0;
  double gx = 0.0;
  double gy = 0.0;
  double gz = 0.0;
};

struct SaveJob {
  int id = 0;
  std::chrono::steady_clock::time_point end_time;
  std::ofstream imu_file;
};

void on_signal(int) {
  g_run.store(false);
}

double ms_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
}

void add_sample(DecodeStats* stats, double ms) {
  ++stats->count;
  stats->last_ms = ms;
  stats->total_ms += ms;
  stats->interval_ms += ms;
  stats->max_ms = std::max(stats->max_ms, ms);
}

void usage(const char* app) {
  std::fprintf(stderr,
      "Usage: %s [opts]\n"
      "  --image-pub <tcp://*:port>     output compressed image PUB endpoint (default tcp://*:5560)\n"
      "  --control <tcp://*:port>       save command REP endpoint (default tcp://*:5561)\n"
      "  --save-dir <dir>               output snapshot dir (default /root/sensor_receiver/snapshot)\n"
      "  --output-path <dir>            alias of --save-dir\n"
      "  --out-w <n> --out-h <n>        resized output size (default 1280x720)\n"
      "  --jpeg-quality <1..100>        output JPEG quality (default 80)\n"
      "  --duration <sec>               stop after N seconds (0=until Ctrl-C)\n"
      "  --quiet                        reduce per-event logging\n"
      "  --cam-left-endpoint <ipc://>   default ipc:///tmp/cam_left\n"
      "  --cam-right-endpoint <ipc://>  default ipc:///tmp/cam_right\n"
      "  --imu-endpoint <ipc://>        default ipc:///tmp/imu_data\n",
      app);
}

bool parse_args(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--image-pub") {
      const char* v = need_value("--image-pub");
      if (!v) return false;
      cfg->image_pub_endpoint = v;
    } else if (arg == "--control") {
      const char* v = need_value("--control");
      if (!v) return false;
      cfg->control_endpoint = v;
    } else if (arg == "--save-dir" || arg == "--output-path") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      cfg->save_dir = v;
    } else if (arg == "--out-w") {
      const char* v = need_value("--out-w");
      if (!v) return false;
      cfg->out_width = std::atoi(v);
    } else if (arg == "--out-h") {
      const char* v = need_value("--out-h");
      if (!v) return false;
      cfg->out_height = std::atoi(v);
    } else if (arg == "--jpeg-quality") {
      const char* v = need_value("--jpeg-quality");
      if (!v) return false;
      cfg->jpeg_quality = std::max(1, std::min(100, std::atoi(v)));
    } else if (arg == "--duration") {
      const char* v = need_value("--duration");
      if (!v) return false;
      cfg->duration_sec = std::atoi(v);
    } else if (arg == "--quiet") {
      cfg->quiet = true;
    } else if (arg == "--cam-left-endpoint") {
      const char* v = need_value("--cam-left-endpoint");
      if (!v) return false;
      cfg->cam_left_endpoint = v;
    } else if (arg == "--cam-right-endpoint") {
      const char* v = need_value("--cam-right-endpoint");
      if (!v) return false;
      cfg->cam_right_endpoint = v;
    } else if (arg == "--imu-endpoint") {
      const char* v = need_value("--imu-endpoint");
      if (!v) return false;
      cfg->imu_endpoint = v;
    } else if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return cfg->out_width > 0 && cfg->out_height > 0;
}

bool ensure_dir(const std::string& dir) {
  if (dir.empty()) return false;
  std::string cur;
  size_t pos = 0;
  if (dir[0] == '/') {
    cur = "/";
    pos = 1;
  }
  while (pos <= dir.size()) {
    size_t slash = dir.find('/', pos);
    std::string part = dir.substr(pos, slash == std::string::npos
                                         ? std::string::npos
                                         : slash - pos);
    if (!part.empty()) {
      if (!cur.empty() && cur.back() != '/') cur += '/';
      cur += part;
      if (::mkdir(cur.c_str(), 0777) < 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir %s failed: %s\n", cur.c_str(),
                     std::strerror(errno));
        return false;
      }
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  return true;
}

bool recv_multipart(zmq::socket_t& socket, std::string* topic,
                    zmq::message_t* payload, zmq::message_t* raw_payload,
                    bool* has_raw_payload) {
  zmq::message_t topic_msg;
  auto topic_ret = socket.recv(topic_msg, zmq::recv_flags::none);
  if (!topic_ret) return false;
  if (!socket.get(zmq::sockopt::rcvmore)) return false;
  auto payload_ret = socket.recv(*payload, zmq::recv_flags::none);
  if (!payload_ret) return false;

  topic->assign(static_cast<const char*>(topic_msg.data()), topic_msg.size());
  *has_raw_payload = false;
  if (socket.get(zmq::sockopt::rcvmore)) {
    auto raw_ret = socket.recv(*raw_payload, zmq::recv_flags::none);
    if (!raw_ret) return false;
    *has_raw_payload = true;
  }
  while (socket.get(zmq::sockopt::rcvmore)) {
    zmq::message_t extra;
    (void)socket.recv(extra, zmq::recv_flags::none);
  }
  return true;
}

bool raw_image_to_mat(const mgx10v::proto::RawImage& msg,
                      const void* raw_payload, size_t raw_payload_size,
                      cv::Mat* image) {
  if (msg.width() == 0 || msg.height() == 0 || msg.step() == 0) return false;

  const int width = static_cast<int>(msg.width());
  const int height = static_cast<int>(msg.height());
  const size_t step = msg.step();
  const std::string& enc = msg.encoding();
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  if (raw_payload) {
    data = static_cast<const uint8_t*>(raw_payload);
    data_size = raw_payload_size;
  } else if (!msg.data().empty()) {
    data = reinterpret_cast<const uint8_t*>(msg.data().data());
    data_size = msg.data().size();
  } else {
    return false;
  }

  auto enough = [&](size_t min_step, int rows) {
    return step >= min_step && data_size >= step * static_cast<size_t>(rows);
  };

  if (enc == "bgr8" || enc == "8UC3" || enc == "rgb8") {
    if (!enough(static_cast<size_t>(width) * 3, height)) return false;
    *image = cv::Mat(height, width, CV_8UC3, const_cast<uint8_t*>(data), step);
    return true;
  }
  if (enc == "bgra8" || enc == "rgba8" || enc == "8UC4") {
    if (!enough(static_cast<size_t>(width) * 4, height)) return false;
    *image = cv::Mat(height, width, CV_8UC4, const_cast<uint8_t*>(data), step);
    return true;
  }
  if (enc == "mono8" || enc == "8UC1") {
    if (!enough(static_cast<size_t>(width), height)) return false;
    *image = cv::Mat(height, width, CV_8UC1, const_cast<uint8_t*>(data), step);
    return true;
  }
  return false;
}

bool mat_to_bgr(const mgx10v::proto::RawImage& msg, const cv::Mat& image,
                cv::Mat* bgr) {
  const std::string& enc = msg.encoding();
  if (enc == "bgr8" || enc == "8UC3") {
    *bgr = image;
    return true;
  }
  if (enc == "rgb8") {
    cv::cvtColor(image, *bgr, cv::COLOR_RGB2BGR);
    return true;
  }
  if (enc == "bgra8" || enc == "8UC4") {
    cv::cvtColor(image, *bgr, cv::COLOR_BGRA2BGR);
    return true;
  }
  if (enc == "rgba8") {
    cv::cvtColor(image, *bgr, cv::COLOR_RGBA2BGR);
    return true;
  }
  if (enc == "mono8" || enc == "8UC1") {
    cv::cvtColor(image, *bgr, cv::COLOR_GRAY2BGR);
    return true;
  }
  return false;
}

std::string ts_string(int64_t sec, int32_t nsec) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld.%09d",
                static_cast<long long>(sec), nsec);
  return std::string(buf);
}

std::string json_header(const char* stream_name,
                        const mgx10v::proto::RawImage& msg,
                        int out_width, int out_height,
                        size_t compressed_size) {
  std::ostringstream os;
  os << "{\"stream\":\"" << stream_name << "\","
     << "\"sec\":" << msg.timestamp().seconds() << ","
     << "\"nsec\":" << msg.timestamp().nanos() << ","
     << "\"src_width\":" << msg.width() << ","
     << "\"src_height\":" << msg.height() << ","
     << "\"width\":" << out_width << ","
     << "\"height\":" << out_height << ","
     << "\"encoding\":\"jpg\","
     << "\"source_encoding\":\"" << msg.encoding() << "\","
     << "\"size\":" << compressed_size << "}";
  return os.str();
}

bool publish_resized_jpeg(zmq::socket_t& pub, const char* stream_name,
                          const mgx10v::proto::RawImage& msg,
                          const cv::Mat& bgr, const Config& cfg,
                          ImageState* state) {
  cv::Mat resized;
  auto resize_start = std::chrono::steady_clock::now();
  cv::resize(bgr, resized, cv::Size(cfg.out_width, cfg.out_height), 0.0, 0.0,
             cv::INTER_AREA);
  add_sample(&state->resize, ms_since(resize_start));

  std::vector<uint8_t> encoded;
  std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, cfg.jpeg_quality};
  auto encode_start = std::chrono::steady_clock::now();
  if (!cv::imencode(".jpg", resized, encoded, params)) return false;
  add_sample(&state->encode, ms_since(encode_start));

  std::string header =
      json_header(stream_name, msg, cfg.out_width, cfg.out_height,
                  encoded.size());
  pub.send(zmq::buffer(stream_name, std::strlen(stream_name)),
           zmq::send_flags::sndmore);
  pub.send(zmq::buffer(header), zmq::send_flags::sndmore);
  pub.send(zmq::buffer(encoded.data(), encoded.size()), zmq::send_flags::none);
  ++state->publish_count;
  return true;
}

bool handle_image(const char* stream_name, const zmq::message_t& payload,
                  const zmq::message_t* raw_payload, const Config& cfg,
                  zmq::socket_t& image_pub, ImageState* state) {
  auto start = std::chrono::steady_clock::now();
  mgx10v::proto::RawImage msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  cv::Mat image;
  const void* raw_data = raw_payload ? raw_payload->data() : nullptr;
  const size_t raw_size = raw_payload ? raw_payload->size() : 0;
  if (!raw_image_to_mat(msg, raw_data, raw_size, &image) || image.empty()) {
    return false;
  }

  cv::Mat bgr;
  if (!mat_to_bgr(msg, image, &bgr) || bgr.empty()) return false;
  state->latest_bgr = bgr.clone();
  state->latest_sec = msg.timestamp().seconds();
  state->latest_nsec = msg.timestamp().nanos();
  state->latest_width = msg.width();
  state->latest_height = msg.height();
  state->latest_encoding = msg.encoding();
  add_sample(&state->decode, ms_since(start));

  return publish_resized_jpeg(image_pub, stream_name, msg, state->latest_bgr,
                              cfg, state);
}

bool decode_imu(const zmq::message_t& payload, ImuSample* out) {
  mgx10v::proto::ImuMsg msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    return false;
  }
  out->sec = msg.header().stamp().seconds();
  out->nsec = msg.header().stamp().nanos();
  out->ax = msg.linear_acceleration().x();
  out->ay = msg.linear_acceleration().y();
  out->az = msg.linear_acceleration().z();
  out->gx = msg.angular_velocity().x();
  out->gy = msg.angular_velocity().y();
  out->gz = msg.angular_velocity().z();
  return true;
}

void write_imu_sample(const ImuSample& sample, std::ofstream* file) {
  if (!file || !file->is_open()) return;
  (*file) << ts_string(sample.sec, sample.nsec) << ' '
          << sample.ax << ' ' << sample.ay << ' ' << sample.az << ' '
          << sample.gx << ' ' << sample.gy << ' ' << sample.gz << '\n';
}

int next_save_id(const std::string& save_dir) {
  const std::string path = save_dir + "/image_timestamps.txt";
  std::ifstream in(path);
  int id = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    int current = 0;
    if (is >> current) id = std::max(id, current);
  }
  return id + 1;
}

bool append_image_timestamp(const std::string& map_path, int id,
                            const std::string& image_name,
                            const char* stream_name,
                            const ImageState& image) {
  std::ofstream map(map_path, std::ios::app);
  if (!map.is_open()) return false;
  map << id << ' ' << image_name << ' ' << stream_name << ' '
      << ts_string(image.latest_sec, image.latest_nsec) << ' '
      << image.latest_width << 'x' << image.latest_height << ' '
      << image.latest_encoding << '\n';
  return true;
}

bool start_save_job(const Config& cfg, const ImageState& left,
                    const ImageState& right,
                    int* next_id, std::deque<SaveJob>* jobs,
                    std::string* reply) {
  if (left.latest_bgr.empty()) {
    *reply = "ERR no left image received yet";
    return false;
  }
  if (right.latest_bgr.empty()) {
    *reply = "ERR no right image received yet";
    return false;
  }
  const std::string left_dir = cfg.save_dir + "/cam_left";
  const std::string right_dir = cfg.save_dir + "/cam_right";
  if (!ensure_dir(left_dir) || !ensure_dir(right_dir)) {
    *reply = "ERR cannot create save dir";
    return false;
  }

  const int id = (*next_id)++;
  const std::string left_name = std::to_string(id) + "_left.png";
  const std::string right_name = std::to_string(id) + "_right.png";
  if (!cv::imwrite(left_dir + "/" + left_name, left.latest_bgr)) {
    *reply = "ERR failed to write left image";
    return false;
  }
  if (!cv::imwrite(right_dir + "/" + right_name, right.latest_bgr)) {
    *reply = "ERR failed to write right image";
    return false;
  }

  const std::string map_path = cfg.save_dir + "/image_timestamps.txt";
  if (!append_image_timestamp(map_path, id, "cam_left/" + left_name,
                              "cam_left", left) ||
      !append_image_timestamp(map_path, id, "cam_right/" + right_name,
                              "cam_right", right)) {
    *reply = "ERR failed to write image timestamp map";
    return false;
  }

  SaveJob job;
  job.id = id;
  job.end_time = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  const std::string imu_path = cfg.save_dir + "/" + std::to_string(id) +
                               "_imu.txt";
  job.imu_file.open(imu_path, std::ios::out);
  if (!job.imu_file.is_open()) {
    *reply = "ERR failed to write imu file";
    return false;
  }
  job.imu_file << "ts ax ay az gx gy gz\n";
  jobs->push_back(std::move(job));

  std::ostringstream os;
  os << "OK saved " << left_name << " ts="
     << ts_string(left.latest_sec, left.latest_nsec) << " and "
     << right_name << " ts=" << ts_string(right.latest_sec, right.latest_nsec)
     << " recording imu 3s";
  *reply = os.str();
  return true;
}

void finish_expired_jobs(std::deque<SaveJob>* jobs) {
  const auto now = std::chrono::steady_clock::now();
  while (!jobs->empty() && now >= jobs->front().end_time) {
    jobs->front().imu_file.flush();
    jobs->front().imu_file.close();
    jobs->pop_front();
  }
}

void print_stats(const char* name, ImageState* state, double interval_s,
                 double elapsed_s) {
  const uint64_t cur = state->publish_count - state->last_publish_count;
  const double cur_hz = interval_s > 0.0 ? cur / interval_s : 0.0;
  const double avg_hz = elapsed_s > 0.0 ? state->publish_count / elapsed_s : 0.0;
  auto avg_ms = [](const DecodeStats& s) {
    return s.count > 0 ? s.total_ms / s.count : 0.0;
  };
  std::printf("%s pub=%llu cur=%.2fHz avg=%.2fHz src=%ux%u enc=%s "
              "decode_ms last=%.3f avg=%.3f resize_ms last=%.3f avg=%.3f "
              "jpeg_ms last=%.3f avg=%.3f\n",
              name, static_cast<unsigned long long>(state->publish_count),
              cur_hz, avg_hz, state->latest_width, state->latest_height,
              state->latest_encoding.c_str(), state->decode.last_ms,
              avg_ms(state->decode), state->resize.last_ms,
              avg_ms(state->resize), state->encode.last_ms,
              avg_ms(state->encode));
  state->last_publish_count = state->publish_count;
  state->decode.interval_ms = 0.0;
  state->resize.interval_ms = 0.0;
  state->encode.interval_ms = 0.0;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, &cfg)) {
    usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (!ensure_dir(cfg.save_dir)) return 2;
  int next_id = next_save_id(cfg.save_dir);

  zmq::context_t context(1);
  zmq::socket_t sub(context, zmq::socket_type::sub);
  sub.set(zmq::sockopt::linger, 0);
  sub.set(zmq::sockopt::rcvhwm, 20);
  sub.connect(cfg.cam_left_endpoint);
  sub.connect(cfg.cam_right_endpoint);
  sub.connect(cfg.imu_endpoint);
  sub.set(zmq::sockopt::subscribe, cfg.cam_left_topic);
  sub.set(zmq::sockopt::subscribe, cfg.cam_right_topic);
  sub.set(zmq::sockopt::subscribe, cfg.imu_topic);

  zmq::socket_t image_pub(context, zmq::socket_type::pub);
  image_pub.set(zmq::sockopt::linger, 0);
  image_pub.set(zmq::sockopt::sndhwm, 5);
  image_pub.bind(cfg.image_pub_endpoint);

  zmq::socket_t control(context, zmq::socket_type::rep);
  control.set(zmq::sockopt::linger, 0);
  control.bind(cfg.control_endpoint);

  std::printf("image_tcp_bridge running\n");
  std::printf("  input: %s/%s %s/%s %s/%s\n",
              cfg.cam_left_endpoint.c_str(), cfg.cam_left_topic.c_str(),
              cfg.cam_right_endpoint.c_str(), cfg.cam_right_topic.c_str(),
              cfg.imu_endpoint.c_str(), cfg.imu_topic.c_str());
  std::printf("  output: %s JPEG %dx%d quality=%d\n",
              cfg.image_pub_endpoint.c_str(), cfg.out_width, cfg.out_height,
              cfg.jpeg_quality);
  std::printf("  control: %s save_dir=%s next_id=%d\n",
              cfg.control_endpoint.c_str(), cfg.save_dir.c_str(), next_id);

  ImageState left;
  ImageState right;
  DecodeStats imu_stats;
  uint64_t bad_topic = 0;
  uint64_t decode_fail = 0;
  uint64_t command_count = 0;
  std::deque<SaveJob> save_jobs;

  const auto start = std::chrono::steady_clock::now();
  auto last_print = start;

  while (g_run.load()) {
    if (cfg.duration_sec > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count() >=
            cfg.duration_sec) {
      break;
    }

    zmq::pollitem_t items[] = {
        {sub.handle(), 0, ZMQ_POLLIN, 0},
        {control.handle(), 0, ZMQ_POLLIN, 0},
    };
    zmq::poll(items, 2, std::chrono::milliseconds(50));

    if (items[0].revents & ZMQ_POLLIN) {
      std::string topic;
      zmq::message_t payload;
      zmq::message_t raw_payload;
      bool has_raw_payload = false;
      if (!recv_multipart(sub, &topic, &payload, &raw_payload,
                          &has_raw_payload)) {
        ++decode_fail;
      } else if (topic == cfg.cam_left_topic) {
        if (!handle_image("cam_left", payload,
                          has_raw_payload ? &raw_payload : nullptr,
                          cfg, image_pub, &left)) {
          ++decode_fail;
        }
      } else if (topic == cfg.cam_right_topic) {
        if (!handle_image("cam_right", payload,
                          has_raw_payload ? &raw_payload : nullptr,
                          cfg, image_pub, &right)) {
          ++decode_fail;
        }
      } else if (topic == cfg.imu_topic) {
        ImuSample sample;
        auto imu_start = std::chrono::steady_clock::now();
        if (decode_imu(payload, &sample)) {
          add_sample(&imu_stats, ms_since(imu_start));
          for (auto& job : save_jobs) {
            write_imu_sample(sample, &job.imu_file);
          }
        } else {
          ++decode_fail;
        }
      } else {
        ++bad_topic;
      }
    }

    if (items[1].revents & ZMQ_POLLIN) {
      zmq::message_t request;
      auto ret = control.recv(request, zmq::recv_flags::none);
      std::string reply = "ERR empty command";
      if (ret) {
        std::string cmd(static_cast<const char*>(request.data()),
                        request.size());
        if (cmd == "s" || cmd == "save" || cmd.find("\"save\"") != std::string::npos) {
          ++command_count;
          (void)start_save_job(cfg, left, right, &next_id, &save_jobs,
                               &reply);
        } else {
          reply = "ERR unknown command";
        }
      }
      control.send(zmq::buffer(reply), zmq::send_flags::none);
      if (!cfg.quiet) std::printf("[control] %s\n", reply.c_str());
    }

    finish_expired_jobs(&save_jobs);

    const auto now = std::chrono::steady_clock::now();
    const double interval_ms =
        std::chrono::duration<double, std::milli>(now - last_print).count();
    if (interval_ms >= cfg.stat_interval_ms) {
      const double interval_s = interval_ms / 1000.0;
      const double elapsed_s =
          std::chrono::duration<double>(now - start).count();
      std::printf("\n[stat] elapsed=%.3fs decode_fail=%llu bad_topic=%llu "
                  "imu=%llu active_save_jobs=%zu commands=%llu\n",
                  elapsed_s, static_cast<unsigned long long>(decode_fail),
                  static_cast<unsigned long long>(bad_topic),
                  static_cast<unsigned long long>(imu_stats.count),
                  save_jobs.size(),
                  static_cast<unsigned long long>(command_count));
      print_stats("cam_left", &left, interval_s, elapsed_s);
      print_stats("cam_right", &right, interval_s, elapsed_s);
      std::fflush(stdout);
      last_print = now;
    }
  }

  for (auto& job : save_jobs) {
    job.imu_file.flush();
    job.imu_file.close();
  }

  std::printf("done. left=%llu right=%llu imu=%llu decode_fail=%llu "
              "bad_topic=%llu commands=%llu\n",
              static_cast<unsigned long long>(left.publish_count),
              static_cast<unsigned long long>(right.publish_count),
              static_cast<unsigned long long>(imu_stats.count),
              static_cast<unsigned long long>(decode_fail),
              static_cast<unsigned long long>(bad_topic),
              static_cast<unsigned long long>(command_count));
  return 0;
}
