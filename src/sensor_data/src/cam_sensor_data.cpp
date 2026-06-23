#include "bounded_frame_queue.h"
#include "exposure_timestamp.h"
#include "types.h"
#include "zmq_publisher.h"

#include "RawImageMsg.pb.h"

#include <linux/videodev2.h>
#include <rk_aiq_user_api2_sysctl.h>
#include <rk_defines.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vpss.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/ioctl.h>
#include <string>
#include <time.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <zmq.hpp>

using namespace slam;

namespace {

constexpr uint32_t kPixFmtNV12 = 0x3231564E;
constexpr const char* kCamSyncPath = "/dev/cam_sync";
constexpr int kViChnId = 0;
constexpr int kVpssChnId = 0;
constexpr size_t kPublishQueueDepth = 2;
constexpr int kCameraCount = 2;

std::atomic<bool> g_run{true};

struct Args {
  std::string right = "/dev/video33";
  std::string left = "/dev/video42";
  std::string left_endpoint = "ipc:///tmp/cam_left";
  std::string right_endpoint = "ipc:///tmp/cam_right";
  std::string left_topic = "cam_left_topic";
  std::string right_topic = "cam_right_topic";
  std::string left_frame_id = "cam_left";
  std::string right_frame_id = "cam_right";
  int left_dev = 1;
  int right_dev = 0;
  std::string left_subdev = "/dev/v4l-subdev10";
  std::string right_subdev = "/dev/v4l-subdev5";
  int width = 1920;
  int height = 1080;
  int fps = 20;
  int duration_s = 0;
  bool aiq = true;
  std::string iq_dir = "/etc/iqfiles/";
};

void on_signal(int) {
  g_run.store(false);
}

void usage(const char* app) {
  std::fprintf(stderr,
      "Usage: %s [opts]\n"
      "  --left <path>              left RKAIQ video node (default /dev/video42)\n"
      "  --right <path>             right RKAIQ video node (default /dev/video33)\n"
      "  --left-dev <n>             MPI VI/VPSS device for left stream (default 1)\n"
      "  --right-dev <n>            MPI VI/VPSS device for right stream (default 0)\n"
      "  --left-subdev <path>       left sensor subdev for exposure (default /dev/v4l-subdev10)\n"
      "  --right-subdev <path>      right sensor subdev for exposure (default /dev/v4l-subdev5)\n"
      "  --w <n> --h <n>            capture resolution (default 1920x1080)\n"
      "  --fps <n>                  expected trigger frame rate for logs (default 20)\n"
      "  --duration <sec>           stop after N seconds (0=until Ctrl-C)\n"
      "  --iq-dir <path>            RKAIQ IQ directory (default /etc/iqfiles/)\n"
      "  --no-aiq                   do not start RKAIQ in this process\n"
      "  --left-endpoint <ipc://>   default ipc:///tmp/cam_left\n"
      "  --right-endpoint <ipc://>  default ipc:///tmp/cam_right\n"
      "  --left-topic <topic>       default cam_left_topic\n"
      "  --right-topic <topic>      default cam_right_topic\n"
      "  (publishes raw NV12: encoding=nv12, step=width, data=w*h*3/2)\n",
      app);
}

bool parse_args(int argc, char** argv, Args& args) {
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage(argv[0]);
        std::exit(1);
      }
      return argv[++i];
    };

    if (key == "--left") args.left = next();
    else if (key == "--right") args.right = next();
    else if (key == "--left-dev") args.left_dev = std::stoi(next());
    else if (key == "--right-dev") args.right_dev = std::stoi(next());
    else if (key == "--left-subdev") args.left_subdev = next();
    else if (key == "--right-subdev") args.right_subdev = next();
    else if (key == "--w" || key == "--cap-w" || key == "--out-w") args.width = std::stoi(next());
    else if (key == "--h" || key == "--cap-h" || key == "--out-h") args.height = std::stoi(next());
    else if (key == "--fps") args.fps = std::stoi(next());
    else if (key == "--duration") args.duration_s = std::stoi(next());
    else if (key == "--iq-dir") args.iq_dir = next();
    else if (key == "--no-aiq") args.aiq = false;
    else if (key == "--left-endpoint") args.left_endpoint = next();
    else if (key == "--right-endpoint") args.right_endpoint = next();
    else if (key == "--left-topic") args.left_topic = next();
    else if (key == "--right-topic") args.right_topic = next();
    else if (key == "-h" || key == "--help") {
      usage(argv[0]);
      return false;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", key.c_str());
      usage(argv[0]);
      return false;
    }
  }
  return true;
}

XCamReturn aiq_sof_cb(rk_aiq_metas_t*) { return XCAM_RETURN_NO_ERROR; }
XCamReturn aiq_err_cb(rk_aiq_err_msg_t*) { return XCAM_RETURN_NO_ERROR; }

class AiqRunner {
public:
  bool start(const std::vector<std::string>& video_nodes, const std::string& iq_dir) {
    setenv("HDR_MODE", "0", 1);
    for (int i = 0; i < 8; ++i) {
      char path[64];
      std::snprintf(path, sizeof(path), "/tmp/UNIX.domain%d", i);
      unlink(path);
    }

    for (const std::string& node : video_nodes) {
      const char* sensor = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(node.c_str());
      if (!sensor || sensor[0] == '\0') {
        std::fprintf(stderr, "rkaiq: cannot find bound sensor for %s\n", node.c_str());
        stop();
        return false;
      }

      std::printf("rkaiq: %s -> %s iq=%s\n", node.c_str(), sensor, iq_dir.c_str());
      rk_aiq_sys_ctx_t* ctx =
          rk_aiq_uapi2_sysctl_init(sensor, iq_dir.c_str(), aiq_err_cb, aiq_sof_cb);
      if (!ctx) {
        std::fprintf(stderr, "rkaiq: init failed for %s\n", sensor);
        stop();
        return false;
      }

      rk_aiq_uapi2_sysctl_setMulCamConc(ctx, true);
      if (rk_aiq_uapi2_sysctl_prepare(ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) !=
          XCAM_RETURN_NO_ERROR) {
        std::fprintf(stderr, "rkaiq: prepare failed for %s\n", sensor);
        rk_aiq_uapi2_sysctl_deinit(ctx);
        stop();
        return false;
      }
      if (rk_aiq_uapi2_sysctl_start(ctx) != XCAM_RETURN_NO_ERROR) {
        std::fprintf(stderr, "rkaiq: start failed for %s\n", sensor);
        rk_aiq_uapi2_sysctl_deinit(ctx);
        stop();
        return false;
      }
      contexts_.push_back(ctx);
    }
    return true;
  }

  void stop() {
    for (auto it = contexts_.rbegin(); it != contexts_.rend(); ++it) {
      rk_aiq_uapi2_sysctl_stop(*it, false);
      rk_aiq_uapi2_sysctl_deinit(*it);
    }
    contexts_.clear();
  }

  ~AiqRunner() { stop(); }

private:
  std::vector<rk_aiq_sys_ctx_t*> contexts_;
};

ns_t monotonic_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<ns_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

bool read_cam_sync_timestamp_us(uint64_t* trigger_us, uint64_t* current_us) {
  if (trigger_us) *trigger_us = 0;
  if (current_us) *current_us = 0;

  int fd = ::open(kCamSyncPath, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;

  uint64_t values[2] = {0, 0};
  ssize_t n = ::read(fd, values, sizeof(values));
  ::close(fd);
  if (n != static_cast<ssize_t>(sizeof(values))) return false;

  if (trigger_us) *trigger_us = values[0];
  if (current_us) *current_us = values[1];
  return values[0] > 0;
}

ns_t read_frame_timestamp_ns() {
  uint64_t trigger_us = 0;
  uint64_t current_us = 0;
  if (read_cam_sync_timestamp_us(&trigger_us, &current_us)) {
    return static_cast<ns_t>(trigger_us) * 1000LL;
  }
  return monotonic_ns();
}

struct SensorExposure {
  int raw_lines = 0;
  int gain_raw = 0;
  int exposure_us = 0;
  bool valid = false;
};

bool read_v4l2_ctrl(int fd, uint32_t id, int* value) {
  if (!value) return false;
  v4l2_control ctrl{};
  ctrl.id = id;
  if (::ioctl(fd, VIDIOC_G_CTRL, &ctrl) != 0) return false;
  *value = ctrl.value;
  return true;
}

bool read_v4l2_ext_ctrl_i64(int fd, uint32_t id, int64_t* value) {
  if (!value) return false;
  v4l2_ext_control ctrl{};
  v4l2_ext_controls ctrls{};
  ctrl.id = id;
  ctrls.count = 1;
  ctrls.controls = &ctrl;
  if (::ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) != 0) return false;
  *value = ctrl.value64;
  return true;
}

float calc_sensor_line_time_us(const std::string& subdev_path,
                               int active_width) {
  int fd = ::open(subdev_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0.0f;

  int h_blanking = 0;
  int64_t pixel_rate = 0;
  const bool ok_hblank = read_v4l2_ctrl(fd, V4L2_CID_HBLANK, &h_blanking);
  const bool ok_pixel_rate =
      read_v4l2_ext_ctrl_i64(fd, V4L2_CID_PIXEL_RATE, &pixel_rate);
  ::close(fd);

  if (!ok_hblank || !ok_pixel_rate || active_width <= 0 || h_blanking <= 0 ||
      pixel_rate <= 0) {
    return 0.0f;
  }
  return static_cast<float>(active_width + h_blanking) /
         static_cast<float>(pixel_rate) * 1'000'000.0f;
}

bool read_sensor_exposure(const std::string& subdev_path,
                          int* exposure_lines,
                          int* gain_raw) {
  int fd = ::open(subdev_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;

  int lines = 0;
  int gain = 0;
  const bool ok_exp = read_v4l2_ctrl(fd, V4L2_CID_EXPOSURE, &lines);
  const bool ok_gain = read_v4l2_ctrl(fd, V4L2_CID_ANALOGUE_GAIN, &gain);
  ::close(fd);

  if (ok_exp && exposure_lines) *exposure_lines = lines;
  if (ok_gain && gain_raw) *gain_raw = gain;
  return ok_exp;
}

class SensorExposureReader {
public:
  bool configure(int dev, const std::string& subdev_path, int active_width) {
    if (!valid_dev(dev)) return false;
    subdev_paths_[dev] = subdev_path;
    line_time_us_[dev] = calc_sensor_line_time_us(subdev_path, active_width);
    return !subdev_path.empty();
  }

  SensorExposure read(int dev) const {
    SensorExposure sample;
    if (!valid_dev(dev) || subdev_paths_[dev].empty()) return sample;

    if (!read_sensor_exposure(subdev_paths_[dev], &sample.raw_lines,
                              &sample.gain_raw)) {
      return sample;
    }
    sample.exposure_us =
        line_time_us_[dev] > 0.0f
            ? static_cast<int>(sample.raw_lines * line_time_us_[dev])
            : sample.raw_lines;
    sample.valid = sample.exposure_us > 0;
    return sample;
  }

  float line_time_us(int dev) const {
    return valid_dev(dev) ? line_time_us_[dev] : 0.0f;
  }

  const std::string& subdev_path(int dev) const {
    static const std::string empty;
    return valid_dev(dev) ? subdev_paths_[dev] : empty;
  }

private:
  static bool valid_dev(int dev) {
    return dev >= 0 && dev < kCameraCount;
  }

  std::string subdev_paths_[kCameraCount];
  float line_time_us_[kCameraCount] = {0.0f, 0.0f};
};

class CapturedFrame {
public:
  CapturedFrame() = default;
  CapturedFrame(int dev, const VIDEO_FRAME_INFO_S& frame, ns_t timestamp_ns,
                int exposure_us)
      : dev_(dev), frame_(frame), timestamp_ns_(timestamp_ns),
        exposure_us_(exposure_us), valid_(true) {}
  ~CapturedFrame() { release(); }
  CapturedFrame(const CapturedFrame&) = delete;
  CapturedFrame& operator=(const CapturedFrame&) = delete;

  CapturedFrame(CapturedFrame&& other) noexcept {
    move_from(std::move(other));
  }

  CapturedFrame& operator=(CapturedFrame&& other) noexcept {
    if (this != &other) {
      release();
      move_from(std::move(other));
    }
    return *this;
  }

  bool valid() const { return valid_; }
  ns_t timestamp_ns() const { return timestamp_ns_; }
  int exposure_us() const { return exposure_us_; }
  const VIDEO_FRAME_INFO_S& frame() const { return frame_; }

  void release() {
    if (!valid_) return;
    RK_MPI_VPSS_ReleaseChnFrame(dev_, kVpssChnId, &frame_);
    valid_ = false;
  }

private:
  void move_from(CapturedFrame&& other) {
    dev_ = other.dev_;
    frame_ = other.frame_;
    timestamp_ns_ = other.timestamp_ns_;
    exposure_us_ = other.exposure_us_;
    valid_ = other.valid_;
    other.valid_ = false;
  }

  int dev_ = -1;
  VIDEO_FRAME_INFO_S frame_{};
  ns_t timestamp_ns_ = 0;
  int exposure_us_ = 0;
  bool valid_ = false;
};

class MpiStereoCapture {
public:
  using FrameCallback = std::function<void(CapturedFrame)>;

  MpiStereoCapture() = default;
  ~MpiStereoCapture() { stop(); close(); }
  MpiStereoCapture(const MpiStereoCapture&) = delete;
  MpiStereoCapture& operator=(const MpiStereoCapture&) = delete;

  bool open(int width, int height, int left_dev, int right_dev,
            const std::string& left_subdev,
            const std::string& right_subdev) {
    width_ = width;
    height_ = height;
    left_dev_ = left_dev;
    right_dev_ = right_dev;
    if (width_ <= 0 || height_ <= 0 || !valid_dev(left_dev_) ||
        !valid_dev(right_dev_) || left_dev_ == right_dev_) {
      return false;
    }

    exposure_reader_.configure(left_dev_, left_subdev, width_);
    exposure_reader_.configure(right_dev_, right_subdev, width_);

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
      std::fprintf(stderr, "mpi: RK_MPI_SYS_Init failed\n");
      return false;
    }
    sys_inited_ = true;

    for (int dev = 0; dev < kCameraCount; ++dev) {
      if (!init_vi(dev) || !init_vpss(dev) || !bind_vi_vpss(dev)) {
        return false;
      }
    }
    return true;
  }

  bool start(FrameCallback left_cb, FrameCallback right_cb) {
    if (!sys_inited_ || running_.load()) return false;
    callbacks_[left_dev_] = std::move(left_cb);
    callbacks_[right_dev_] = std::move(right_cb);
    running_.store(true);
    for (int dev = 0; dev < kCameraCount; ++dev) {
      threads_[dev] = std::thread(&MpiStereoCapture::capture_loop, this, dev);
    }
    return true;
  }

  void stop() {
    running_.store(false);
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
  }

  void close() {
    stop();
    if (!sys_inited_) return;

    for (int dev = 0; dev < kCameraCount; ++dev) {
      if (bound_[dev]) {
        MPP_CHN_S src{};
        src.enModId = RK_ID_VI;
        src.s32DevId = dev;
        src.s32ChnId = kViChnId;
        MPP_CHN_S dst{};
        dst.enModId = RK_ID_VPSS;
        dst.s32DevId = dev;
        dst.s32ChnId = kVpssChnId;
        RK_MPI_SYS_UnBind(&src, &dst);
        bound_[dev] = false;
      }
    }

    for (int dev = 0; dev < kCameraCount; ++dev) {
      if (vpss_started_[dev]) {
        RK_MPI_VPSS_StopGrp(dev);
        vpss_started_[dev] = false;
      }
      if (vpss_created_[dev]) {
        RK_MPI_VPSS_DestroyGrp(dev);
        vpss_created_[dev] = false;
      }
      if (vi_chn_enabled_[dev]) {
        RK_MPI_VI_DisableChn(dev, kViChnId);
        vi_chn_enabled_[dev] = false;
      }
      if (vi_dev_enabled_[dev]) {
        RK_MPI_VI_DisableDev(dev);
        vi_dev_enabled_[dev] = false;
      }
    }

    RK_MPI_SYS_Exit();
    sys_inited_ = false;
  }

  int width() const { return width_; }
  int height() const { return height_; }
  int left_dev() const { return left_dev_; }
  int right_dev() const { return right_dev_; }
  float line_time_us(int dev) const { return exposure_reader_.line_time_us(dev); }
  const std::string& subdev_path(int dev) const {
    return exposure_reader_.subdev_path(dev);
  }

private:
  static bool valid_dev(int dev) {
    return dev >= 0 && dev < kCameraCount;
  }

  bool init_vi(int dev) {
    VI_DEV_ATTR_S dev_attr{};
    VI_DEV_BIND_PIPE_S bind_pipe{};

    int ret = RK_MPI_VI_GetDevAttr(dev, &dev_attr);
    if (ret == RK_ERR_VI_NOT_CONFIG) {
      ret = RK_MPI_VI_SetDevAttr(dev, &dev_attr);
      if (ret != RK_SUCCESS) {
        std::fprintf(stderr, "mpi: RK_MPI_VI_SetDevAttr(%d) failed: 0x%x\n",
                     dev, ret);
        return false;
      }
    }

    ret = RK_MPI_VI_GetDevIsEnable(dev);
    if (ret != RK_SUCCESS) {
      ret = RK_MPI_VI_EnableDev(dev);
      if (ret != RK_SUCCESS) {
        std::fprintf(stderr, "mpi: RK_MPI_VI_EnableDev(%d) failed: 0x%x\n",
                     dev, ret);
        return false;
      }
      vi_dev_enabled_[dev] = true;

      bind_pipe.u32Num = 1;
      bind_pipe.PipeId[0] = dev;
      ret = RK_MPI_VI_SetDevBindPipe(dev, &bind_pipe);
      if (ret != RK_SUCCESS) {
        std::fprintf(stderr,
                     "mpi: RK_MPI_VI_SetDevBindPipe(%d) failed: 0x%x\n",
                     dev, ret);
        return false;
      }
    }

    VI_CHN_ATTR_S chn_attr{};
    chn_attr.stIspOpt.u32BufCount = 2;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    chn_attr.stSize.u32Width = width_;
    chn_attr.stSize.u32Height = height_;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.u32Depth = 0;
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;

    ret = RK_MPI_VI_SetChnAttr(dev, kViChnId, &chn_attr);
    if (ret == RK_SUCCESS) ret = RK_MPI_VI_EnableChn(dev, kViChnId);
    if (ret != RK_SUCCESS) {
      std::fprintf(stderr, "mpi: VI chn init failed dev=%d ret=0x%x\n",
                   dev, ret);
      return false;
    }
    vi_chn_enabled_[dev] = true;
    return true;
  }

  bool init_vpss(int grp) {
    VPSS_GRP_ATTR_S grp_attr{};
    grp_attr.u32MaxW = 4096;
    grp_attr.u32MaxH = 4096;
    grp_attr.enPixelFormat = RK_FMT_YUV420SP;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;
    grp_attr.enCompressMode = COMPRESS_MODE_NONE;

    int ret = RK_MPI_VPSS_CreateGrp(grp, &grp_attr);
    if (ret != RK_SUCCESS) {
      std::fprintf(stderr, "mpi: RK_MPI_VPSS_CreateGrp(%d) failed: 0x%x\n",
                   grp, ret);
      return false;
    }
    vpss_created_[grp] = true;

    VPSS_CHN_ATTR_S chn_attr{};
    chn_attr.enChnMode = VPSS_CHN_MODE_USER;
    chn_attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;
    chn_attr.u32Width = width_;
    chn_attr.u32Height = height_;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.u32Depth = 4;

    ret = RK_MPI_VPSS_SetChnAttr(grp, kVpssChnId, &chn_attr);
    if (ret == RK_SUCCESS) ret = RK_MPI_VPSS_EnableChn(grp, kVpssChnId);
    if (ret == RK_SUCCESS) ret = RK_MPI_VPSS_StartGrp(grp);
    if (ret != RK_SUCCESS) {
      std::fprintf(stderr, "mpi: VPSS init failed grp=%d ret=0x%x\n",
                   grp, ret);
      return false;
    }
    vpss_started_[grp] = true;
    return true;
  }

  bool bind_vi_vpss(int dev) {
    MPP_CHN_S src{};
    src.enModId = RK_ID_VI;
    src.s32DevId = dev;
    src.s32ChnId = kViChnId;
    MPP_CHN_S dst{};
    dst.enModId = RK_ID_VPSS;
    dst.s32DevId = dev;
    dst.s32ChnId = kVpssChnId;
    int ret = RK_MPI_SYS_Bind(&src, &dst);
    if (ret != RK_SUCCESS) {
      std::fprintf(stderr, "mpi: bind VI%d -> VPSS%d failed: 0x%x\n",
                   dev, dev, ret);
      return false;
    }
    bound_[dev] = true;
    return true;
  }

  void capture_loop(int dev) {
    while (running_.load()) {
      VIDEO_FRAME_INFO_S video_frame{};
      int ret = RK_MPI_VPSS_GetChnFrame(dev, kVpssChnId, &video_frame, 200);
      if (ret != RK_SUCCESS) continue;

      const ns_t trigger_timestamp_ns = read_frame_timestamp_ns();
      const SensorExposure exposure = exposure_reader_.read(dev);
      const ns_t timestamp_ns = exposure_midpoint_timestamp_ns(
          trigger_timestamp_ns, exposure.exposure_us);
      CapturedFrame frame(dev, video_frame, timestamp_ns, exposure.exposure_us);
      if (callbacks_[dev]) {
        callbacks_[dev](std::move(frame));
      }
    }
  }

  int width_ = 0;
  int height_ = 0;
  int left_dev_ = 1;
  int right_dev_ = 0;
  bool sys_inited_ = false;
  bool vi_dev_enabled_[kCameraCount] = {false, false};
  bool vi_chn_enabled_[kCameraCount] = {false, false};
  bool vpss_created_[kCameraCount] = {false, false};
  bool vpss_started_[kCameraCount] = {false, false};
  bool bound_[kCameraCount] = {false, false};
  std::atomic<bool> running_{false};
  std::thread threads_[kCameraCount];
  FrameCallback callbacks_[kCameraCount];
  SensorExposureReader exposure_reader_;
};

std::shared_ptr<Frame> copy_captured_frame(const CapturedFrame& captured) {
  if (!captured.valid()) return nullptr;
  const VIDEO_FRAME_INFO_S& video_frame = captured.frame();
  void* vir_addr = RK_MPI_MB_Handle2VirAddr(video_frame.stVFrame.pMbBlk);
  if (!vir_addr) return nullptr;

  const int w = static_cast<int>(video_frame.stVFrame.u32Width);
  const int h = static_cast<int>(video_frame.stVFrame.u32Height);
  const int stride_w = static_cast<int>(video_frame.stVFrame.u32VirWidth);
  const int stride_h = static_cast<int>(video_frame.stVFrame.u32VirHeight);
  if (w <= 0 || h <= 0 || stride_w < w || stride_h < h) return nullptr;

  auto frame = std::make_shared<Frame>();
  frame->width = w;
  frame->height = h;
  frame->stride = w;
  frame->pixfmt = kPixFmtNV12;
  frame->ts_ns = captured.timestamp_ns();
  frame->exposure_us = captured.exposure_us();
  frame->data.resize(static_cast<size_t>(w) * h * 3 / 2);

  const uint8_t* src_y = static_cast<const uint8_t*>(vir_addr);
  uint8_t* dst = frame->data.data();
  for (int row = 0; row < h; ++row) {
    std::memcpy(dst + static_cast<size_t>(row) * w,
                src_y + static_cast<size_t>(row) * stride_w, w);
  }

  const uint8_t* src_uv = src_y + static_cast<size_t>(stride_w) * stride_h;
  uint8_t* dst_uv = dst + static_cast<size_t>(w) * h;
  for (int row = 0; row < h / 2; ++row) {
    std::memcpy(dst_uv + static_cast<size_t>(row) * w,
                src_uv + static_cast<size_t>(row) * stride_w, w);
  }
  return frame;
}


uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start);

// Publish the captured frame as raw NV12 (the native ISP format): no RGA color
// conversion, no JPEG encoding. Payload is the packed NV12 buffer (w*h*3/2).
bool publish_nv12_image(ZmqPublisher& pub, const Frame& frame,
                        const std::string& frame_id) {
  if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return false;
  const size_t expected =
      static_cast<size_t>(frame.width) * frame.height * 3 / 2;
  if (frame.data.size() < expected) return false;

  mgx10v::proto::RawImage msg;
  msg.Clear();
  set_timestamp_from_ns(msg.mutable_timestamp(), frame.ts_ns);
  msg.set_frame_id(frame_id);
  msg.set_width(static_cast<uint32_t>(frame.width));
  msg.set_height(static_cast<uint32_t>(frame.height));
  msg.set_encoding("nv12");
  msg.set_step(static_cast<uint32_t>(frame.width));  // Y-plane row stride
  msg.set_data_size(static_cast<uint64_t>(expected));
  msg.set_exposure_us(static_cast<uint32_t>(std::max(frame.exposure_us, 0)));

  return pub.publish_with_raw_payload(msg, frame.data.data(), expected);
}

double fps_from_count(unsigned long long count, double elapsed_s) {
  return elapsed_s > 0.0 ? static_cast<double>(count) / elapsed_s : 0.0;
}

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

void update_max(std::atomic<uint64_t>& value, uint64_t sample) {
  uint64_t old = value.load();
  while (sample > old && !value.compare_exchange_weak(old, sample)) {}
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return 1;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  zmq::context_t zmq_context(1);
  ZmqPublisher left_pub(zmq_context, args.left_endpoint, args.left_topic);
  ZmqPublisher right_pub(zmq_context, args.right_endpoint, args.right_topic);
  if (!left_pub.bind() || !right_pub.bind()) {
    return 2;
  }

  AiqRunner aiq;
  if (args.aiq && !aiq.start({args.left, args.right}, args.iq_dir)) {
    std::fprintf(stderr, "cam_sensor_data: RKAIQ start failed\n");
    return 2;
  }

  MpiStereoCapture capture;
  if (!capture.open(args.width, args.height, args.left_dev, args.right_dev,
                    args.left_subdev, args.right_subdev)) {
    std::fprintf(stderr, "cam_sensor_data: MPI camera open failed\n");
    return 3;
  }

  std::atomic<unsigned long long> left_count{0};
  std::atomic<unsigned long long> right_count{0};
  std::atomic<unsigned long long> decode_fail_count{0};
  std::atomic<unsigned long long> publish_fail_count{0};
  std::atomic<unsigned long long> published_count{0};
  std::atomic<uint64_t> publish_total_us{0};
  std::atomic<uint64_t> publish_interval_us{0};
  std::atomic<uint64_t> publish_max_us{0};
  std::atomic<uint64_t> publish_last_us{0};
  std::atomic<int> left_last_exposure_us{0};
  std::atomic<int> right_last_exposure_us{0};
  BoundedFrameQueue<CapturedFrame> left_queue(kPublishQueueDepth);
  BoundedFrameQueue<CapturedFrame> right_queue(kPublishQueueDepth);

  auto handle_frame = [&](std::shared_ptr<Frame> frame,
                          ZmqPublisher& publisher,
                          const std::string& frame_id,
                          std::atomic<unsigned long long>& frame_count) {
    if (frame->pixfmt != kPixFmtNV12) {
      ++decode_fail_count;
      return;
    }

    auto publish_start = std::chrono::steady_clock::now();
    bool published = publish_nv12_image(publisher, *frame, frame_id);
    uint64_t publish_us = elapsed_us_since(publish_start);
    ++published_count;
    publish_total_us.fetch_add(publish_us);
    publish_interval_us.fetch_add(publish_us);
    publish_last_us.store(publish_us);
    update_max(publish_max_us, publish_us);

    if (published) {
      ++frame_count;
    } else {
      ++publish_fail_count;
    }
  };

  auto publish_loop = [&](BoundedFrameQueue<CapturedFrame>& queue,
                          ZmqPublisher& publisher,
                          const std::string& frame_id,
                          std::atomic<unsigned long long>& frame_count) {
    CapturedFrame captured;
    while (queue.pop(&captured)) {
      std::shared_ptr<Frame> frame = copy_captured_frame(captured);
      captured.release();
      if (!frame) {
        ++decode_fail_count;
        continue;
      }
      handle_frame(std::move(frame), publisher, frame_id, frame_count);
    }
  };

  std::thread left_publish_thread([&] {
    publish_loop(left_queue, left_pub, args.left_frame_id, left_count);
  });
  std::thread right_publish_thread([&] {
    publish_loop(right_queue, right_pub, args.right_frame_id, right_count);
  });

  if (!capture.start([&](CapturedFrame frame) {
        left_last_exposure_us.store(frame.exposure_us());
        left_queue.push(std::move(frame));
      },
      [&](CapturedFrame frame) {
        right_last_exposure_us.store(frame.exposure_us());
        right_queue.push(std::move(frame));
      })) {
    left_queue.close();
    right_queue.close();
    if (left_publish_thread.joinable()) left_publish_thread.join();
    if (right_publish_thread.joinable()) right_publish_thread.join();
    std::fprintf(stderr, "cam_sensor_data: camera start failed\n");
    return 4;
  }

  std::printf("cam_sensor_data publishing RawImage\n");
  std::printf("  left:  %s topic=%s endpoint=%s\n", args.left.c_str(),
              args.left_topic.c_str(), args.left_endpoint.c_str());
  std::printf("  right: %s topic=%s endpoint=%s\n", args.right.c_str(),
              args.right_topic.c_str(), args.right_endpoint.c_str());
  std::printf("  requested MPI capture: %dx%d fps %d rkaiq=%s left_dev=%d right_dev=%d\n",
              args.width, args.height, args.fps, args.aiq ? "on" : "off",
              capture.left_dev(), capture.right_dev());
  std::printf("  image transport: raw NV12 (encoding=nv12, no RGA/JPEG)\n");
  std::printf("  actual capture: L=%dx%d R=%dx%d\n",
              capture.width(), capture.height(), capture.width(),
              capture.height());
  std::printf("  exposure subdev: L=%s hts=%.3f us R=%s hts=%.3f us\n",
              capture.subdev_path(capture.left_dev()).c_str(),
              capture.line_time_us(capture.left_dev()),
              capture.subdev_path(capture.right_dev()).c_str(),
              capture.line_time_us(capture.right_dev()));
  std::printf("  timestamp: cam_sync trigger + exposure_us/2 "
              "(CLOCK_MONOTONIC fallback)\n");

  auto start = std::chrono::steady_clock::now();
  auto last = start;
  unsigned long long last_left = 0;
  unsigned long long last_right = 0;
  unsigned long long last_published = 0;
  uint64_t last_left_queue_drop = 0;
  uint64_t last_right_queue_drop = 0;
  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto now = std::chrono::steady_clock::now();
    if (args.duration_s > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >=
            args.duration_s) {
      g_run.store(false);
    }

    double interval_s = std::chrono::duration<double>(now - last).count();
    if (interval_s >= 1.0) {
      double elapsed_s = std::chrono::duration<double>(now - start).count();
      unsigned long long left = left_count.load();
      unsigned long long right = right_count.load();
      unsigned long long published = published_count.load();
      unsigned long long pub_interval_count = published - last_published;
      uint64_t left_queue_drop = left_queue.dropped_count();
      uint64_t right_queue_drop = right_queue.dropped_count();
      uint64_t publish_interval = publish_interval_us.exchange(0);
      double publish_cur_ms =
          pub_interval_count > 0
              ? publish_interval / 1000.0 / pub_interval_count
              : 0.0;
      double publish_avg_ms =
          published > 0 ? publish_total_us.load() / 1000.0 / published : 0.0;
      std::printf("[stat] L=%llu cur=%.2f avg=%.2f fps R=%llu cur=%.2f "
                  "avg=%.2f fps frames=%llu publish_ms cur_avg=%.3f avg=%.3f "
                  "max=%.3f last=%.3f queue_drop L=%llu(+%llu) R=%llu(+%llu) "
                  "exposure_us L=%d R=%d decode_fail=%llu publish_fail=%llu\n",
                  left, fps_from_count(left - last_left, interval_s),
                  fps_from_count(left, elapsed_s), right,
                  fps_from_count(right - last_right, interval_s),
                  fps_from_count(right, elapsed_s), published,
                  publish_cur_ms, publish_avg_ms,
                  publish_max_us.load() / 1000.0,
                  publish_last_us.load() / 1000.0,
                  static_cast<unsigned long long>(left_queue_drop),
                  static_cast<unsigned long long>(
                      left_queue_drop - last_left_queue_drop),
                  static_cast<unsigned long long>(right_queue_drop),
                  static_cast<unsigned long long>(
                      right_queue_drop - last_right_queue_drop),
                  left_last_exposure_us.load(),
                  right_last_exposure_us.load(),
                  decode_fail_count.load(),
                  publish_fail_count.load());
      std::fflush(stdout);
      last = now;
      last_left = left;
      last_right = right;
      last_published = published;
      last_left_queue_drop = left_queue_drop;
      last_right_queue_drop = right_queue_drop;
    }
  }

  capture.stop();
  left_queue.close();
  right_queue.close();
  if (left_publish_thread.joinable()) left_publish_thread.join();
  if (right_publish_thread.joinable()) right_publish_thread.join();
  std::printf("done. left=%llu right=%llu frames=%llu "
              "publish_ms avg=%.3f max=%.3f "
              "queue_drop L=%llu R=%llu decode_fail=%llu publish_fail=%llu\n",
              left_count.load(), right_count.load(), published_count.load(),
              published_count.load() > 0
                  ? publish_total_us.load() / 1000.0 / published_count.load()
                  : 0.0,
              publish_max_us.load() / 1000.0,
              static_cast<unsigned long long>(left_queue.dropped_count()),
              static_cast<unsigned long long>(right_queue.dropped_count()),
              decode_fail_count.load(),
              publish_fail_count.load());
  return 0;
}
