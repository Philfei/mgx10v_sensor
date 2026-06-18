# image_tcp_bridge

`image_tcp_bridge` subscribes to `sensor_data` IPC streams, converts each
camera frame to BGR, resizes it from the original `1920x1080` frame to
`1280x720`, JPEG-compresses it, and publishes the compressed image over TCP.
It is intended for the current `sensor_data` default `--image-transport raw`
mode.

Default endpoints:

- Input camera: `ipc:///tmp/cam_left`, `ipc:///tmp/cam_right`
- Input IMU: `ipc:///tmp/imu_data`
- Output image TCP PUB: `tcp://*:5560`
- Control TCP REP: `tcp://*:5561`

Build:

```bash
source toolchain/mgx10v_sysroot/env.sh
receiver_test/senser_receiver/image_tcp_bridge/build.sh
```

Deploy the generated `receiver_test/image_tcp_bridge_build/deploy/*` to the
device together with `sensor_data`.

Device side example:

```bash
cd /userdata/app/sensor_data
./start_sensor_data.sh cam imu --w 1920 --h 1080 --fps 20

cd /userdata/app/image_tcp_bridge
./start_image_tcp_bridge.sh
```

The launcher can be configured with environment variables:

```bash
OUTPUT_PATH=/root/sensor_receiver/snapshot \
IMAGE_PUB='tcp://*:5560' \
CONTROL='tcp://*:5561' \
OUT_W=1280 OUT_H=720 JPEG_QUALITY=80 \
./start_image_tcp_bridge.sh
```

Extra CLI arguments are passed to `image_tcp_bridge`.

PC side example:

```bash
python3 -m pip install pyzmq numpy opencv-python
python3 recv_tcp_image.py --host 10.42.0.130
```

Press `s` in the display window to ask `image_tcp_bridge` to save one original
stereo pair and record the following 3 seconds of IMU data. Saved files:

- Images: `1_left.png`, `1_right.png`, `2_left.png`, `2_right.png`, ...
- Image timestamp map: `image_timestamps.txt`
- IMU windows: `1_imu.txt`, `2_imu.txt`, ...

Snapshot paths are:

```text
<output_path>/cam_left/<id>_left.png
<output_path>/cam_right/<id>_right.png
<output_path>/<id>_imu.txt
```

`<output_path>` is configured with `--output-path`, or equivalently
`--save-dir`.

IMU rows are:

```text
ts ax ay az gx gy gz
```

`ax ay az` keep the original MEMS unit `g`; `gx gy gz` are `rad/s`.
