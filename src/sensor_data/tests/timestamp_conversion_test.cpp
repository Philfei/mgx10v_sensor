#include "zmq_publisher.h"

#include <google/protobuf/timestamp.pb.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace {

int64_t expected_unix_seconds_today_utc(double hhmmss) {
  int hh = static_cast<int>(hhmmss / 10000.0);
  int mm = static_cast<int>((hhmmss - hh * 10000.0) / 100.0);
  double ss = hhmmss - hh * 10000.0 - mm * 100.0;

  std::time_t now = std::time(nullptr);
  std::tm utc {};
  gmtime_r(&now, &utc);
  utc.tm_hour = 0;
  utc.tm_min = 0;
  utc.tm_sec = 0;

  return static_cast<int64_t>(timegm(&utc)) + hh * 3600 + mm * 60 +
         static_cast<int64_t>(ss);
}

}  // namespace

int main() {
  google::protobuf::Timestamp stamp;
  set_timestamp_from_hhmmss(&stamp, 123456.789);

  const int64_t expected_seconds =
      expected_unix_seconds_today_utc(123456.789);
  if (stamp.seconds() != expected_seconds) {
    std::fprintf(stderr,
                 "expected Unix seconds %lld, got %lld\n",
                 static_cast<long long>(expected_seconds),
                 static_cast<long long>(stamp.seconds()));
    return 1;
  }

  const int32_t expected_nanos = 789000000;
  if (std::abs(stamp.nanos() - expected_nanos) > 1000) {
    std::fprintf(stderr, "expected nanos near %d, got %d\n",
                 expected_nanos, stamp.nanos());
    return 1;
  }

  return 0;
}
