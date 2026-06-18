# sensor_data ZMQ/protobuf publishers

该目录提供三个发布程序，把设备内传感器数据编码成
`receiver_test/protobuf` 中定义的 protobuf 消息，并通过 ZMQ `PUB` socket 在设备
内传输。

## 程序与 endpoint

| 程序 | 消息 | endpoint | topic |
| --- | --- | --- | --- |
| `cam_sensor_data` | `mgx10v.proto.RawImage` | `ipc:///tmp/cam_left` | `cam_left_topic` |
| `cam_sensor_data` | `mgx10v.proto.RawImage` | `ipc:///tmp/cam_right` | `cam_right_topic` |
| `imu_sensor_data` | `mgx10v.proto.ImuMsg` | `ipc:///tmp/imu_data` | `imu_topic` |
| `gnss_sensor_data` | `mgx10v.proto.GnssMsg` | `ipc:///tmp/gnss_data` | `gnss_topic` |

`imu_sensor_data` 中 `ImuMsg.linear_acceleration` 保持 MEMS 原始协议单位 `g`，
`ImuMsg.angular_velocity` 单位为 `rad/s`。

IMU/GNSS ZMQ 消息使用 multipart：

```text
part 0: topic 字符串
part 1: protobuf 序列化后的 bytes
```

图像 ZMQ 消息使用三段 multipart，避免把大图像 buffer 放进 protobuf：

```text
part 0: topic 字符串
part 1: RawImage protobuf header（timestamp/frame_id/width/height/encoding/step）
part 2: 原始图像 bytes
```

相机图像使用三段 ZMQ multipart 直接传输整帧大图，不使用共享内存。

## Proto 定义

本目录的 `proto/` 保存当前程序使用的消息定义：
`Common.proto`、`RawImageMsg.proto`、`ImuMsg.proto`、`GnssMsg.proto`。构建时从
该目录生成 C++ protobuf 代码，构建输出也会复制 `proto/`，便于部署后查看消息
结构。

`cam_sensor_data` 默认采集 `1920x1080` 原始 `nv12` 数据，在发布前使用
Rockchip RGA 硬件转换为 `bgr8`，再通过三段 ZMQ 发布。采集端会优先使用
V4L2 `VIDIOC_EXPBUF` 导出的 DMA-BUF fd 作为 RGA 输入源；若设备不支持导出，
才回退到 virtual-address buffer。RGA 输出端会优先从 dma-heap 分配 DMA-BUF
并复用 RGA handle，避免每帧使用目标 virtual-address buffer；如果设备没有可用
dma-heap，自动回退到 virtual-address 输出。

## 编译

```bash
cd /home/lrs/mgx10v
source toolchain/mgx10v_sysroot/env.sh
receiver_test/senser_receiver/sensor_data/build.sh
```

默认编译输出在：

```text
receiver_test/sensor_data_build/
  cam_sensor_data
  imu_sensor_data
  gnss_sensor_data
  proto/*.proto
  lib/
    libzmq.so*
    libprotobuf.so*
    librga.so*
    rockchip/*.so*
```

同时会生成干净的部署包：

```text
receiver_test/sensor_data_build/deploy/
```

`cam_sensor_data` 使用 Rockchip RGA 做 `nv12 -> bgr8` 硬件颜色转换，然后只负责
通过 ZMQ 发布原始图像 bytes；本模块不保存图像，也不链接图像编解码或视觉处理库。

## 部署

推荐只拷贝 `deploy/` 目录内容到设备：

```bash
ssh root@10.42.0.42 "mkdir -p /userdata/app/sensor_data"
scp -r /home/lrs/mgx10v/receiver_test/sensor_data_build/deploy/* \
  root@10.42.0.42:/userdata/app/sensor_data/
```

## 运行

设备上：

```bash
cd /userdata/app/sensor_data
./start_sensor_data.sh
./stop_sensor_data.sh
```

`start_sensor_data.sh` 可选择只启动部分发布程序；不传参数时启动全部：

```bash
./start_sensor_data.sh           # cam + imu + gnss
./start_sensor_data.sh cam       # 只启动相机发布
./start_sensor_data.sh imu       # 只启动 IMU 发布
./start_sensor_data.sh gnss      # 只启动 GNSS 发布
./start_sensor_data.sh imu gnss  # 同时启动 IMU 和 GNSS
```

常用参数：

```bash
./cam_sensor_data --duration 10 --left /dev/video33 --right /dev/video42
./cam_sensor_data --duration 10 --color-format bgra8
./cam_sensor_data --duration 10 --w 1280 --h 720 --fps 20
./imu_sensor_data --duration 10 --port /dev/ttyS4
./gnss_sensor_data --duration 10 --port /dev/ttyS8
```

快速启动脚本会把日志写入 `logs/`，PID 写入 `run/`。如果本次启动包含 `cam` 且
部署目录中存在 `start_vendor_pwm.sh`，启动脚本默认会先开启厂家相机同步触发；
如果存在 `stop_vendor_pwm.sh`，停止脚本默认会关闭该触发。常用环境变量：

```bash
PWM_FREQ=20 ./start_sensor_data.sh
CAM_ARGS="--w 1280 --h 720 --fps 20" ./start_sensor_data.sh
IMU_ARGS="--port /dev/ttyS4" GNSS_ARGS="--port /dev/ttyS8" ./start_sensor_data.sh
START_VENDOR_PWM=0 ./start_sensor_data.sh
STOP_VENDOR_PWM=0 ./stop_sensor_data.sh
```

相机分辨率通过 `CAM_ARGS="--w <width> --h <height>"` 控制。详细说明见
`CAMERA_RESOLUTION.md`。

也可以单独控制厂家 PWM：

```bash
FREQ=20 ./start_vendor_pwm.sh
./stop_vendor_pwm.sh
```

`start_vendor_pwm.sh` 默认执行 `insmod /userdata/ko/cam_sync.ko freq=$FREQ`。
`stop_vendor_pwm.sh` 默认按下面的顺序清理：

```bash
echo 0 > /sys/class/pwm/pwmchip1/pwm0/enable
echo 0 > /sys/class/pwm/pwmchip1/unexport
rmmod cam_sync
```

如果设备路径不同，可用 `CAM_SYNC_KO`、`PWM_CHIP`、`PWM_INDEX` 覆盖默认值。

`cam_sensor_data` 默认请求 `1920x1080` 原始图像分辨率，并在启动日志中输出
MPI 实际捕获到的左右目尺寸。需要指定其他分辨率时使用 `--w <n> --h <n>`。
默认发布 `bgr8`；需要对比 RGA 32bit 对齐输出时可使用 `--color-format bgra8`。
统计输出中的 `rga_ms` 为 RGA 同步颜色转换耗时，`publish_ms` 为 ZMQ 发布 raw
payload 的耗时。相机采集线程会读取 `/dev/cam_sync` 触发时间作为绝对时间锚点，
后续用 `frame.stVFrame.u64PTS` 的帧间 delta 推进时间戳，避免因读取到最新
`cam_sync` 触发时间而产生 0.1s 假间隔。再按 sensor subdev 的
`V4L2_CID_EXPOSURE` 和行时间计算曝光时长，将发布时间戳补偿为曝光中点：
`cam_sync_anchor + pts_delta + exposure_us / 2`。默认左目曝光 subdev 为
`/dev/v4l-subdev10`，右目为 `/dev/v4l-subdev5`，可用 `--left-subdev`、
`--right-subdev` 覆盖。
采集线程会在读取时间戳后把 VPSS buffer 交给异步发布队列，`queue_drop`
表示发布队列满时丢弃的旧帧数量。

图像频率由两部分共同决定：

- `start_vendor_pwm.sh` 的 `FREQ` 环境变量控制厂家 `cam_sync.ko` 外触发频率。
- `cam_sensor_data --fps <n>` 作为期望触发频率写入启动日志，实际采集频率以
  外触发和运行时统计输出为准。

实际采集时这两个值应保持一致，例如：

```bash
FREQ=20 ./start_vendor_pwm.sh
./cam_sensor_data --fps 20
```

`start_vendor_pwm.sh` 不控制图像分辨率；分辨率由 `cam_sensor_data --w <n> --h <n>`
请求，最终以启动日志中的 `actual capture` 为准。

相机采集仍需要设备侧外触发/PWM 已开启。
