#include "serial_port.h"
#include "zmq_publisher.h"

#include "ImuMsg.pb.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <zmq.hpp>

namespace {

constexpr const char* kDefaultPort = "/dev/ttyS4";
constexpr int kDefaultBaud = 115200;
constexpr int kFrameSize = 52;
constexpr int kImuSendHighWaterMark = 5000;

volatile sig_atomic_t g_stop = 0;

struct Config {
  std::string port = kDefaultPort;
  int baud = kDefaultBaud;
  std::string endpoint = "ipc:///tmp/imu_data";
  std::string topic = "imu_topic";
  std::string frame_id = "mems_imu";
  int duration_sec = 0;
  bool quiet = false;
};

struct ImuFrame {
  uint64_t recv_mono_ns = 0;
  double utc = 0.0;
  float acc_x_g = 0.0f;
  float acc_y_g = 0.0f;
  float acc_z_g = 0.0f;
  float gyro_x_rad_s = 0.0f;
  float gyro_y_rad_s = 0.0f;
  float gyro_z_rad_s = 0.0f;
};

void on_signal(int) {
  g_stop = 1;
}

bool bin_checksum_ok(const uint8_t* frame) {
  uint16_t sum = 0;
  for (int i = 0; i < 48; ++i) {
    sum += frame[i];
  }

  uint16_t stored = frame[48] | (static_cast<uint16_t>(frame[49]) << 8);
  return sum == stored && frame[50] == 'e' && frame[51] == 'd';
}

ImuFrame parse_imu(const uint8_t* f) {
  ImuFrame out;
  out.recv_mono_ns = monotonic_ns();

  memcpy(&out.utc, f + 4, 8);
  memcpy(&out.acc_x_g, f + 12, 4);
  memcpy(&out.acc_y_g, f + 16, 4);
  memcpy(&out.acc_z_g, f + 20, 4);
  memcpy(&out.gyro_x_rad_s, f + 24, 4);
  memcpy(&out.gyro_y_rad_s, f + 28, 4);
  memcpy(&out.gyro_z_rad_s, f + 32, 4);
  return out;
}

void print_usage(const char* argv0) {
  printf("Usage: %s [--port /dev/ttyS4] [--baud 115200] [--duration SEC] "
         "[--endpoint ipc:///tmp/imu_data] [--topic imu_topic] [--quiet]\n",
         argv0);
}

bool parse_args(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--port") {
      const char* v = need_value("--port");
      if (!v) return false;
      cfg->port = v;
    } else if (arg == "--baud") {
      const char* v = need_value("--baud");
      if (!v) return false;
      cfg->baud = atoi(v);
    } else if (arg == "--duration") {
      const char* v = need_value("--duration");
      if (!v) return false;
      cfg->duration_sec = atoi(v);
    } else if (arg == "--endpoint") {
      const char* v = need_value("--endpoint");
      if (!v) return false;
      cfg->endpoint = v;
    } else if (arg == "--topic") {
      const char* v = need_value("--topic");
      if (!v) return false;
      cfg->topic = v;
    } else if (arg == "--frame-id") {
      const char* v = need_value("--frame-id");
      if (!v) return false;
      cfg->frame_id = v;
    } else if (arg == "--quiet") {
      cfg->quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      return false;
    }
  }
  return true;
}

void fill_covariance(google::protobuf::RepeatedField<double>* field,
                     double diagonal) {
  field->Clear();
  for (int i = 0; i < 9; ++i) {
    field->Add((i == 0 || i == 4 || i == 8) ? diagonal : 0.0);
  }
}

void fill_imu_msg(const Config& cfg, const ImuFrame& frame,
                  mgx10v::proto::ImuMsg* msg) {
  msg->Clear();
  set_timestamp_from_hhmmss(msg->mutable_header()->mutable_stamp(), frame.utc);
  msg->mutable_header()->set_frame_id(cfg.frame_id);

  msg->mutable_orientation()->set_x(0.0);
  msg->mutable_orientation()->set_y(0.0);
  msg->mutable_orientation()->set_z(0.0);
  msg->mutable_orientation()->set_w(1.0);
  fill_covariance(msg->mutable_orientation_covariance(), -1.0);

  msg->mutable_angular_velocity()->set_x(frame.gyro_x_rad_s);
  msg->mutable_angular_velocity()->set_y(frame.gyro_y_rad_s);
  msg->mutable_angular_velocity()->set_z(frame.gyro_z_rad_s);
  fill_covariance(msg->mutable_angular_velocity_covariance(), 0.0);

  msg->mutable_linear_acceleration()->set_x(frame.acc_x_g);
  msg->mutable_linear_acceleration()->set_y(frame.acc_y_g);
  msg->mutable_linear_acceleration()->set_z(frame.acc_z_g);
  fill_covariance(msg->mutable_linear_acceleration_covariance(), 0.0);
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, &cfg)) {
    print_usage(argv[0]);
    return 2;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  zmq::context_t context(1);
  ZmqPublisherOptions publisher_options;
  publisher_options.send_high_water_mark = kImuSendHighWaterMark;
  ZmqPublisher publisher(context, cfg.endpoint, cfg.topic, publisher_options);
  if (!publisher.bind()) return 2;

  int fd = open_serial_port(cfg.port, cfg.baud);
  if (fd < 0) return 1;

  printf("imu_sensor_data publishing ImuMsg from %s @ %d to %s topic=%s\n",
         cfg.port.c_str(), cfg.baud, cfg.endpoint.c_str(), cfg.topic.c_str());

  const uint64_t start_ns = monotonic_ns();
  const uint64_t duration_ns = cfg.duration_sec > 0
      ? static_cast<uint64_t>(cfg.duration_sec) * 1000000000ull
      : 0;

  std::vector<uint8_t> buf;
  buf.reserve(4096);
  uint64_t imu_count = 0;
  uint64_t publish_fail = 0;

  while (!g_stop) {
    if (duration_ns > 0 && monotonic_ns() - start_ns >= duration_ns) {
      break;
    }

    uint8_t tmp[512];
    int n = serial_read_with_timeout(fd, tmp, sizeof(tmp), 100);
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("read");
      break;
    }
    if (n == 0) continue;

    buf.insert(buf.end(), tmp, tmp + n);
    if (buf.size() > 4096) {
      buf.erase(buf.begin(), buf.begin() + (buf.size() - 4096));
    }

    bool progress = true;
    while (progress) {
      progress = false;
      for (size_t i = 0; i + kFrameSize <= buf.size(); ++i) {
        if (buf[i] == 'f' && buf[i + 1] == 'm' && buf[i + 2] == 'i' &&
            buf[i + 3] == 'm') {
          if (bin_checksum_ok(buf.data() + i)) {
            ImuFrame frame = parse_imu(buf.data() + i);
            mgx10v::proto::ImuMsg msg;
            fill_imu_msg(cfg, frame, &msg);
            if (publisher.publish(msg)) {
              ++imu_count;
              if (!cfg.quiet) {
                printf("[IMU] utc=%012.3f count=%llu\n", frame.utc,
                       static_cast<unsigned long long>(imu_count));
              }
            } else {
              ++publish_fail;
            }
            buf.erase(buf.begin(), buf.begin() + i + kFrameSize);
            progress = true;
            break;
          }
        }
      }

      if (!progress && buf.size() > static_cast<size_t>(kFrameSize * 2)) {
        buf.erase(buf.begin(), buf.end() - kFrameSize);
        progress = true;
      }
    }
  }

  close(fd);
  printf("done. imu=%llu publish_fail=%llu\n",
         static_cast<unsigned long long>(imu_count),
         static_cast<unsigned long long>(publish_fail));
  return 0;
}
