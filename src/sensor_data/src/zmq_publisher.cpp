#include "zmq_publisher.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <ctime>
#include <utility>
#include <unistd.h>

ZmqPublisher::ZmqPublisher(zmq::context_t& context, std::string endpoint,
                           std::string topic, ZmqPublisherOptions options)
    : endpoint_(std::move(endpoint)),
      topic_(std::move(topic)),
      socket_(context, zmq::socket_type::pub) {
  socket_.set(zmq::sockopt::linger, 0);
  socket_.set(zmq::sockopt::sndhwm, options.send_high_water_mark);
}

bool ZmqPublisher::bind() {
  try {
    unlink_ipc_endpoint(endpoint_);
    socket_.bind(endpoint_);
    return true;
  } catch (const zmq::error_t& e) {
    std::fprintf(stderr, "ZMQ bind failed: %s endpoint=%s\n", e.what(),
                 endpoint_.c_str());
    return false;
  }
}

bool ZmqPublisher::publish(const google::protobuf::Message& message) {
  std::string payload;
  if (!message.SerializeToString(&payload)) {
    std::fprintf(stderr, "protobuf SerializeToString failed\n");
    return false;
  }

  try {
    socket_.send(zmq::buffer(topic_), zmq::send_flags::sndmore);
    socket_.send(zmq::buffer(payload), zmq::send_flags::none);
    return true;
  } catch (const zmq::error_t& e) {
    std::fprintf(stderr, "ZMQ send failed: %s endpoint=%s topic=%s\n", e.what(),
                 endpoint_.c_str(), topic_.c_str());
    return false;
  }
}

bool ZmqPublisher::publish_with_raw_payload(
    const google::protobuf::Message& header, const void* data, size_t size) {
  if (!data && size > 0) return false;

  std::string payload;
  if (!header.SerializeToString(&payload)) {
    std::fprintf(stderr, "protobuf SerializeToString failed\n");
    return false;
  }

  try {
    socket_.send(zmq::buffer(topic_), zmq::send_flags::sndmore);
    socket_.send(zmq::buffer(payload), zmq::send_flags::sndmore);
    socket_.send(zmq::const_buffer(data, size), zmq::send_flags::none);
    return true;
  } catch (const zmq::error_t& e) {
    std::fprintf(stderr, "ZMQ send failed: %s endpoint=%s topic=%s\n", e.what(),
                 endpoint_.c_str(), topic_.c_str());
    return false;
  }
}

void set_timestamp_from_ns(google::protobuf::Timestamp* stamp, int64_t ns) {
  if (!stamp) return;
  if (ns < 0) ns = 0;
  stamp->set_seconds(ns / 1000000000LL);
  stamp->set_nanos(static_cast<int32_t>(ns % 1000000000LL));
}

double hhmmss_to_seconds_of_day(double hhmmss) {
  int hh = static_cast<int>(hhmmss / 10000.0);
  int mm = static_cast<int>((hhmmss - hh * 10000.0) / 100.0);
  double ss = hhmmss - hh * 10000.0 - mm * 100.0;
  return hh * 3600.0 + mm * 60.0 + ss;
}

void set_timestamp_from_hhmmss(google::protobuf::Timestamp* stamp,
                               double hhmmss) {
  double sod = hhmmss_to_seconds_of_day(hhmmss);
  if (sod < 0.0) sod = 0.0;

  double sec_double = 0.0;
  double frac = std::modf(sod, &sec_double);
  auto sec_of_day = static_cast<int64_t>(sec_double);
  auto nsec = static_cast<int32_t>(std::llround(frac * 1e9));
  if (nsec >= 1000000000) {
    ++sec_of_day;
    nsec -= 1000000000;
  }
  if (nsec < 0) nsec = 0;

  std::time_t now = std::time(nullptr);
  std::tm utc {};
  gmtime_r(&now, &utc);
  utc.tm_hour = 0;
  utc.tm_min = 0;
  utc.tm_sec = 0;
  auto sec = static_cast<int64_t>(timegm(&utc)) + sec_of_day;

  constexpr int64_t kHalfDaySec = 12 * 60 * 60;
  constexpr int64_t kDaySec = 24 * 60 * 60;
  const auto now_sec = static_cast<int64_t>(now);
  if (sec - now_sec > kHalfDaySec) {
    sec -= kDaySec;
  } else if (now_sec - sec > kHalfDaySec) {
    sec += kDaySec;
  }

  stamp->set_seconds(sec);
  stamp->set_nanos(nsec);
}

void unlink_ipc_endpoint(const std::string& endpoint) {
  constexpr const char* kPrefix = "ipc://";
  if (endpoint.rfind(kPrefix, 0) != 0) return;
  std::string path = endpoint.substr(6);
  if (!path.empty()) {
    unlink(path.c_str());
  }
}
