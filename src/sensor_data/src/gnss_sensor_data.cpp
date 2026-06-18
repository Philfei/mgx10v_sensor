#include "serial_port.h"
#include "zmq_publisher.h"

#include "GnssMsg.pb.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <zmq.hpp>

namespace {

constexpr const char* kDefaultPort = "/dev/ttyS8";
constexpr int kDefaultBaud = 115200;
constexpr int kFrameSize = 95;
constexpr double kMaxGgaGstDeltaSec = 0.01;
constexpr int kGnssSendHighWaterMark = 5000;

volatile sig_atomic_t g_stop = 0;

struct Config {
  std::string port = kDefaultPort;
  int baud = kDefaultBaud;
  std::string endpoint = "ipc:///tmp/gnss_data";
  std::string topic = "gnss_topic";
  std::string frame_id = "gnss";
  int duration_sec = 0;
  bool quiet = false;
};

struct GnssFrame {
  uint64_t recv_mono_ns = 0;
  double utc = 0.0;
  double lat = 0.0;
  double lon = 0.0;
  double alt = 0.0;
  float vn = 0.0f;
  float ve = 0.0f;
  float vd = 0.0f;
  float pvar_n = 0.0f;
  float pvar_e = 0.0f;
  float pvar_alt = 0.0f;
  float hrms = 0.0f;
  float vrms = 0.0f;
  float hdop = 0.0f;
  float vdop = 0.0f;
  uint8_t sat_count = 0;
  uint8_t fix_status = 0;
  uint8_t diff_age = 0;
};

struct GgaData {
  bool valid = false;
  double utc = 0.0;
  double lat = 0.0;
  double lon = 0.0;
  double alt = 0.0;
  int fix_quality = 0;
  int satellites = 0;
  double hdop = 0.0;
  double diff_age = 0.0;
};

struct GstData {
  bool valid = false;
  double utc = 0.0;
  double rms = 0.0;
  double lat_std = 0.0;
  double lon_std = 0.0;
  double alt_std = 0.0;
};

struct RmcData {
  bool valid = false;
  double utc = 0.0;
  bool active = false;
};

struct PendingNmeaEpoch {
  std::optional<GgaData> gga;
  std::optional<GstData> gst;
  std::optional<RmcData> rmc;
};

void on_signal(int) {
  g_stop = 1;
}

bool bin_checksum_ok(const uint8_t* frame) {
  uint16_t sum = 0;
  for (int i = 0; i < 91; ++i) {
    sum += frame[i];
  }

  uint16_t stored = frame[91] | (static_cast<uint16_t>(frame[92]) << 8);
  return sum == stored && frame[93] == 'e' && frame[94] == 'd';
}

GnssFrame parse_bin_gnss(const uint8_t* f) {
  GnssFrame out;
  out.recv_mono_ns = monotonic_ns();

  float res1 = 0.0f;
  float res2 = 0.0f;
  float heading = 0.0f;
  memcpy(&out.utc, f + 4, 8);
  memcpy(&out.lat, f + 12, 8);
  memcpy(&out.lon, f + 20, 8);
  memcpy(&out.alt, f + 28, 8);
  memcpy(&out.vn, f + 36, 4);
  memcpy(&out.ve, f + 40, 4);
  memcpy(&out.vd, f + 44, 4);
  memcpy(&out.pvar_n, f + 48, 4);
  memcpy(&out.pvar_e, f + 52, 4);
  memcpy(&out.pvar_alt, f + 56, 4);
  memcpy(&res1, f + 60, 4);
  memcpy(&res2, f + 64, 4);
  memcpy(&heading, f + 68, 4);
  memcpy(&out.hrms, f + 72, 4);
  memcpy(&out.vrms, f + 76, 4);
  memcpy(&out.hdop, f + 80, 4);
  memcpy(&out.vdop, f + 84, 4);
  out.sat_count = f[88];
  out.fix_status = f[89];
  out.diff_age = f[90];
  (void)res1;
  (void)res2;
  (void)heading;
  return out;
}

bool nmea_checksum_ok(const char* line) {
  const char* star = strchr(line, '*');
  if (!star || line[0] != '$') return false;

  unsigned int calc = 0;
  for (const char* p = line + 1; p < star; ++p) {
    calc ^= static_cast<unsigned char>(*p);
  }

  unsigned int stored = 0;
  sscanf(star + 1, "%2x", &stored);
  return calc == stored;
}

std::vector<std::string> split_nmea_fields(const char* line) {
  std::vector<std::string> out;
  const char* begin = line;
  const char* end = strchr(line, '*');
  if (!end) end = line + strlen(line);
  for (const char* p = begin; p <= end; ++p) {
    if (p == end || *p == ',') {
      out.emplace_back(begin, p - begin);
      begin = p + 1;
    }
  }
  return out;
}

double parse_double(const std::string& s, double fallback = 0.0) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  return end && *end == '\0' ? v : fallback;
}

int parse_int(const std::string& s, int fallback = 0) {
  if (s.empty()) return fallback;
  char* end = nullptr;
  long v = strtol(s.c_str(), &end, 10);
  return end && *end == '\0' ? static_cast<int>(v) : fallback;
}

double parse_latlon_deg(const std::string& ddmm, const std::string& hemi) {
  double raw = parse_double(ddmm, 0.0);
  int deg = static_cast<int>(raw / 100.0);
  double minutes = raw - deg * 100.0;
  double value = deg + minutes / 60.0;
  if (hemi == "S" || hemi == "W") value = -value;
  return value;
}

bool parse_gga(const char* line, GgaData* out) {
  std::vector<std::string> f = split_nmea_fields(line);
  if (f.size() < 10) return false;
  if (f[0].size() < 6 || f[0].substr(f[0].size() - 3) != "GGA") return false;

  GgaData g;
  g.utc = parse_double(f[1]);
  g.lat = parse_latlon_deg(f[2], f[3]);
  g.lon = parse_latlon_deg(f[4], f[5]);
  g.fix_quality = parse_int(f[6]);
  g.satellites = parse_int(f[7]);
  g.hdop = parse_double(f[8]);
  g.alt = parse_double(f[9]);
  if (f.size() > 13) g.diff_age = parse_double(f[13]);
  g.valid = g.fix_quality > 0;
  *out = g;
  return true;
}

bool parse_gst(const char* line, GstData* out) {
  std::vector<std::string> f = split_nmea_fields(line);
  if (f.size() < 9) return false;
  if (f[0].size() < 6 || f[0].substr(f[0].size() - 3) != "GST") return false;

  GstData g;
  g.utc = parse_double(f[1]);
  g.rms = parse_double(f[2]);
  g.lat_std = parse_double(f[6]);
  g.lon_std = parse_double(f[7]);
  g.alt_std = parse_double(f[8]);
  g.valid = true;
  *out = g;
  return true;
}

bool parse_rmc(const char* line, RmcData* out) {
  std::vector<std::string> f = split_nmea_fields(line);
  if (f.size() < 3) return false;
  if (f[0].size() < 6 || f[0].substr(f[0].size() - 3) != "RMC") return false;

  RmcData r;
  r.utc = parse_double(f[1]);
  r.active = f[2] == "A";
  r.valid = !f[1].empty();
  *out = r;
  return true;
}

bool same_nmea_second(double a, double b) {
  double delta =
      fabs(hhmmss_to_seconds_of_day(a) - hhmmss_to_seconds_of_day(b));
  if (delta > 12.0 * 60.0 * 60.0) {
    delta = 24.0 * 60.0 * 60.0 - delta;
  }
  return delta <= kMaxGgaGstDeltaSec;
}

void add_repeated(google::protobuf::RepeatedField<double>* field,
                  std::initializer_list<double> values) {
  field->Clear();
  for (double value : values) field->Add(value);
}

void fill_common_header(const Config& cfg, double utc,
                        mgx10v::proto::GnssMsg* msg) {
  set_timestamp_from_hhmmss(msg->mutable_header()->mutable_stamp(), utc);
  msg->mutable_header()->set_frame_id(cfg.frame_id);
  msg->mutable_time()->set_week(0);
  msg->mutable_time()->set_tow(hhmmss_to_seconds_of_day(utc));
}

void fill_from_binary(const Config& cfg, const GnssFrame& g,
                      mgx10v::proto::GnssMsg* msg) {
  msg->Clear();
  fill_common_header(cfg, g.utc, msg);
  msg->set_pos_type(g.fix_status);
  add_repeated(msg->mutable_blh(), {g.lat, g.lon, g.alt});
  add_repeated(msg->mutable_blh_std(),
               {sqrt(std::max(0.0f, g.pvar_n)),
                sqrt(std::max(0.0f, g.pvar_e)),
                sqrt(std::max(0.0f, g.pvar_alt))});
  add_repeated(msg->mutable_vel(), {g.ve, g.vn, -g.vd});
  msg->set_pos_sol_status(g.fix_status > 0 ? 0 : 1);
  msg->set_diff_age(g.diff_age);
  msg->set_sol_age(0.0);
  msg->set_solnsvs(g.sat_count);
  add_repeated(msg->mutable_blh_cov(), {g.pvar_n, 0.0, 0.0,
                                        0.0, g.pvar_e, 0.0,
                                        0.0, 0.0, g.pvar_alt});
  msg->set_hrms(g.hrms);
  msg->set_vrms(g.vrms);
  msg->set_hdop(g.hdop);
  msg->set_vdop(g.vdop);
}

void fill_from_gga_gst(const Config& cfg, const GgaData& gga,
                       const GstData* gst,
                       mgx10v::proto::GnssMsg* msg) {
  msg->Clear();
  fill_common_header(cfg, gga.utc, msg);
  msg->set_pos_type(gga.fix_quality);
  add_repeated(msg->mutable_blh(), {gga.lat, gga.lon, gga.alt});
  add_repeated(msg->mutable_vel(), {0.0, 0.0, 0.0});
  msg->set_pos_sol_status(gga.fix_quality > 0 ? 0 : 1);
  msg->set_diff_age(gga.diff_age);
  msg->set_sol_age(0.0);
  msg->set_solnsvs(gga.satellites);
  msg->set_hdop(static_cast<float>(gga.hdop));

  if (gst && gst->valid && same_nmea_second(gga.utc, gst->utc)) {
    const double lat_var = gst->lat_std * gst->lat_std;
    const double lon_var = gst->lon_std * gst->lon_std;
    const double alt_var = gst->alt_std * gst->alt_std;
    add_repeated(msg->mutable_blh_std(),
                 {gst->lat_std, gst->lon_std, gst->alt_std});
    add_repeated(msg->mutable_blh_cov(), {lat_var, 0.0, 0.0,
                                          0.0, lon_var, 0.0,
                                          0.0, 0.0, alt_var});
    msg->set_hrms(static_cast<float>(
        gst->rms > 0.0 ? gst->rms : sqrt(lat_var + lon_var)));
    msg->set_vrms(static_cast<float>(gst->alt_std));
  }
}

int64_t nmea_epoch_key_ms(double utc) {
  return static_cast<int64_t>(
      llround(hhmmss_to_seconds_of_day(utc) * 1000.0));
}

class NmeaEpochCache {
public:
  std::optional<mgx10v::proto::GnssMsg> update(const Config& cfg,
                                               const GgaData& gga) {
    const int64_t key = nmea_epoch_key_ms(gga.utc);
    pending_[key].gga = gga;
    return pop_if_ready(cfg, key);
  }

  std::optional<mgx10v::proto::GnssMsg> update(const Config& cfg,
                                               const GstData& gst) {
    const int64_t key = nmea_epoch_key_ms(gst.utc);
    pending_[key].gst = gst;
    return pop_if_ready(cfg, key);
  }

  std::optional<mgx10v::proto::GnssMsg> update(const Config& cfg,
                                               const RmcData& rmc) {
    const int64_t key = nmea_epoch_key_ms(rmc.utc);
    pending_[key].rmc = rmc;
    return pop_if_ready(cfg, key);
  }

private:
  std::optional<mgx10v::proto::GnssMsg> pop_if_ready(const Config& cfg,
                                                     int64_t key) {
    prune_old_epochs(key);
    auto it = pending_.find(key);
    if (it == pending_.end()) {
      return std::nullopt;
    }

    PendingNmeaEpoch& epoch = it->second;
    if (!epoch.gga.has_value() || !epoch.gst.has_value() ||
        !epoch.rmc.has_value()) {
      return std::nullopt;
    }

    mgx10v::proto::GnssMsg msg;
    fill_from_gga_gst(cfg, *epoch.gga, &*epoch.gst, &msg);
    pending_.erase(it);
    return msg;
  }

  void prune_old_epochs(int64_t latest_key) {
    constexpr int64_t kKeepWindowMs = 2000;
    while (!pending_.empty() &&
           pending_.begin()->first + kKeepWindowMs < latest_key) {
      pending_.erase(pending_.begin());
    }
  }

  std::map<int64_t, PendingNmeaEpoch> pending_;
};

void print_usage(const char* argv0) {
  printf("Usage: %s [--port /dev/ttyS8] [--baud 115200] [--duration SEC] "
         "[--endpoint ipc:///tmp/gnss_data] [--topic gnss_topic] [--quiet]\n",
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
  publisher_options.send_high_water_mark = kGnssSendHighWaterMark;
  ZmqPublisher publisher(context, cfg.endpoint, cfg.topic, publisher_options);
  if (!publisher.bind()) return 2;

  int fd = open_serial_port(cfg.port, cfg.baud);
  if (fd < 0) return 1;

  printf("gnss_sensor_data publishing GnssMsg from %s @ %d to %s topic=%s\n",
         cfg.port.c_str(), cfg.baud, cfg.endpoint.c_str(), cfg.topic.c_str());

  const uint64_t start_ns = monotonic_ns();
  const uint64_t duration_ns = cfg.duration_sec > 0
      ? static_cast<uint64_t>(cfg.duration_sec) * 1000000000ull
      : 0;

  std::vector<uint8_t> buf;
  buf.reserve(4096);
  uint64_t gnss_count = 0;
  uint64_t nmea_count = 0;
  uint64_t publish_fail = 0;
  NmeaEpochCache nmea_cache;

  auto publish_ready_nmea =
      [&](const std::optional<mgx10v::proto::GnssMsg>& ready) {
        if (!ready.has_value()) {
          return;
        }
        if (publisher.publish(*ready)) {
          ++gnss_count;
          if (!cfg.quiet && ready->blh_size() >= 2) {
            printf("[GNSS] tow=%.3f lat=%.9f lon=%.9f count=%llu\n",
                   ready->time().tow(), ready->blh(0), ready->blh(1),
                   static_cast<unsigned long long>(gnss_count));
          }
        } else {
          ++publish_fail;
        }
      };

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
            buf[i + 3] == 'g') {
          if (bin_checksum_ok(buf.data() + i)) {
            GnssFrame g = parse_bin_gnss(buf.data() + i);
            mgx10v::proto::GnssMsg msg;
            fill_from_binary(cfg, g, &msg);
            if (publisher.publish(msg)) {
              ++gnss_count;
            } else {
              ++publish_fail;
            }
            buf.erase(buf.begin(), buf.begin() + i + kFrameSize);
            progress = true;
            break;
          }
        }
      }
      if (progress) continue;

      for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != '$') continue;

        int end = -1;
        for (size_t j = i + 1; j < buf.size(); ++j) {
          if (buf[j] == '\n') {
            end = static_cast<int>(j);
            break;
          }
        }
        if (end < 0) break;

        bool printable = true;
        for (int k = static_cast<int>(i) + 1; k < end; ++k) {
          if (buf[k] < 0x20 && buf[k] != '\r') {
            printable = false;
            break;
          }
        }
        if (!printable) {
          buf.erase(buf.begin(), buf.begin() + i + 1);
          progress = true;
          break;
        }

        char line[256] = {};
        int len = end - static_cast<int>(i);
        if (len > 0 && len < static_cast<int>(sizeof(line))) {
          memcpy(line, buf.data() + i, len);
          if (line[len - 1] == '\r') line[len - 1] = '\0';
          if (nmea_checksum_ok(line)) {
            ++nmea_count;
            GstData gst;
            RmcData rmc;
            GgaData gga;
            if (parse_gst(line, &gst)) {
              publish_ready_nmea(nmea_cache.update(cfg, gst));
            } else if (parse_rmc(line, &rmc)) {
              publish_ready_nmea(nmea_cache.update(cfg, rmc));
            } else if (parse_gga(line, &gga)) {
              publish_ready_nmea(nmea_cache.update(cfg, gga));
            }
          }
        }
        buf.erase(buf.begin(), buf.begin() + end + 1);
        progress = true;
        break;
      }

      if (!progress && buf.size() > static_cast<size_t>(kFrameSize * 2)) {
        buf.erase(buf.begin(), buf.end() - kFrameSize);
        progress = true;
      }
    }
  }

  close(fd);
  printf("done. gnss=%llu nmea=%llu publish_fail=%llu\n",
         static_cast<unsigned long long>(gnss_count),
         static_cast<unsigned long long>(nmea_count),
         static_cast<unsigned long long>(publish_fail));
  return 0;
}
