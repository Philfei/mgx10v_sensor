# sensor_replay

`sensor_replay` reads data saved by `sensor_recorder` and publishes it with
the same ZMQ multipart layout as `sensor_data`.

Default published streams:

- `cam_left`: `ipc:///tmp/cam_left`, topic `cam_left_topic`
- `cam_right`: `ipc:///tmp/cam_right`, topic `cam_right_topic`
- `imu`: `ipc:///tmp/imu_data`, topic `imu_topic`
- `gnss`: `ipc:///tmp/gnss_data`, topic `gnss_topic`

Camera messages are sent as:

1. topic
2. serialized `RawImage`
3. raw image bytes

IMU and GNSS messages are sent as:

1. topic
2. serialized protobuf message

## Build

Host build for unit tests:

```bash
cmake -S receiver_test/senser_receiver/sensor_replay \
  -B /tmp/sensor_replay_tests \
  -DSENSOR_REPLAY_BUILD_APP=OFF \
  -DSENSOR_REPLAY_BUILD_TESTS=ON
cmake --build /tmp/sensor_replay_tests -j"$(nproc)"
ctest --test-dir /tmp/sensor_replay_tests --output-on-failure
```

Target build:

```bash
receiver_test/senser_receiver/sensor_replay/build.sh \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=/usr/bin/aarch64-linux-gnu-g++-9
```

The deploy bundle is written to:

```text
receiver_test/sensor_replay_build/deploy
```

## Run

```bash
./sensor_replay --dir /path/to/data_2026-06-17-18-43-49
```

Useful options:

- `--config config.yaml`: use an explicit stream config.
- `--speed 1`: replay by recorded timestamps in realtime.
- `--speed 0`: publish as fast as possible.
- `--loop`: repeat until interrupted.
- `--quiet`: reduce logs.

The input directory is expected to contain per-stream subdirectories such as
`cam_left`, `cam_right`, `imu`, and `gnss`, each holding `*_chunk.dat` files.
Missing stream directories are skipped.
