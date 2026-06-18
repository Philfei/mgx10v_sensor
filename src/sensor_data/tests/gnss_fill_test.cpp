#define main gnss_sensor_data_main
#include "../src/gnss_sensor_data.cpp"
#undef main

#include <cmath>
#include <cstdio>

namespace {

bool near(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

GgaData make_gga(double utc) {
  GgaData gga;
  gga.valid = true;
  gga.utc = utc;
  gga.lat = 40.0654975367;
  gga.lon = 116.2285746000;
  gga.alt = 59.455;
  gga.fix_quality = 1;
  gga.satellites = 21;
  gga.hdop = 1.02;
  gga.diff_age = 0.0;
  return gga;
}

GstData make_gst(double utc) {
  GstData gst;
  gst.valid = true;
  gst.utc = utc;
  gst.rms = 0.614;
  gst.lat_std = 0.402;
  gst.lon_std = 0.328;
  gst.alt_std = 0.327;
  return gst;
}

RmcData make_rmc(double utc) {
  RmcData rmc;
  rmc.valid = true;
  rmc.utc = utc;
  return rmc;
}

bool expect_recent_gst_fills_blh_std() {
  Config cfg;
  mgx10v::proto::GnssMsg msg;
  GgaData gga = make_gga(123912.800);
  GstData gst = make_gst(123912.800);

  fill_from_gga_gst(cfg, gga, &gst, &msg);

  if (msg.blh_std_size() != 3) {
    std::fprintf(stderr, "expected 3 blh_std values, got %d\n",
                 msg.blh_std_size());
    return false;
  }
  if (!near(msg.blh_std(0), gst.lat_std) ||
      !near(msg.blh_std(1), gst.lon_std) ||
      !near(msg.blh_std(2), gst.alt_std)) {
    std::fprintf(stderr, "expected GST std values in blh_std\n");
    return false;
  }
  return true;
}

bool expect_nmea_cache_waits_for_gga_gst_rmc() {
  Config cfg;
  NmeaEpochCache cache;
  GgaData gga = make_gga(123912.800);
  GstData gst = make_gst(123912.800);
  RmcData rmc = make_rmc(123912.800);

  if (cache.update(cfg, gga).has_value()) {
    std::fprintf(stderr, "GGA alone must not publish\n");
    return false;
  }
  if (cache.update(cfg, rmc).has_value()) {
    std::fprintf(stderr, "GGA+RMC must not publish without GST\n");
    return false;
  }

  auto msg = cache.update(cfg, gst);
  if (!msg.has_value()) {
    std::fprintf(stderr, "GGA+RMC+GST should publish\n");
    return false;
  }
  if (msg->blh_std_size() != 3) {
    std::fprintf(stderr, "expected 3 blh_std values, got %d\n",
                 msg->blh_std_size());
    return false;
  }
  if (!near(msg->blh_std(0), gst.lat_std) ||
      !near(msg->blh_std(1), gst.lon_std) ||
      !near(msg->blh_std(2), gst.alt_std)) {
    std::fprintf(stderr, "expected cached GST std values in blh_std\n");
    return false;
  }
  return true;
}

bool expect_nmea_cache_does_not_mix_adjacent_epochs() {
  Config cfg;
  NmeaEpochCache cache;
  GgaData gga = make_gga(123912.800);
  GstData previous_gst = make_gst(123912.700);
  RmcData previous_rmc = make_rmc(123912.700);

  if (cache.update(cfg, previous_gst).has_value()) {
    std::fprintf(stderr, "GST alone must not publish\n");
    return false;
  }
  if (cache.update(cfg, previous_rmc).has_value()) {
    std::fprintf(stderr, "GST+RMC without GGA must not publish\n");
    return false;
  }
  if (cache.update(cfg, gga).has_value()) {
    std::fprintf(stderr, "GGA must not publish with previous epoch GST/RMC\n");
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!expect_recent_gst_fills_blh_std()) return 1;
  if (!expect_nmea_cache_waits_for_gga_gst_rmc()) return 1;
  if (!expect_nmea_cache_does_not_mix_adjacent_epochs()) return 1;
  return 0;
}
