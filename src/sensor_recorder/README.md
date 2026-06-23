# sensor_recorder

`sensor_recorder` subscribes to the ZMQ/protobuf streams published by
`sensor_data` and saves the original records into chunk files.

## Configuration

The recorder reads `config.yaml` from the executable directory, the current
directory, or the path passed with `--config`.

```yaml
data_root: /root/sensor_receiver/data
chunk_duration_s: 5

topics:
  - name: cam_left
    type: raw_image
    endpoint: ipc:///tmp/cam_left
    topic: cam_left_topic
    enabled: true
```

Supported `type` values:

- `raw_image`
- `imu`
- `gnss`

Set `enabled: false` to keep a topic in the file without recording it.

## Output

Each run creates one session folder:

```text
<data_root>/data_YYYY-mm-dd-HH-MM-SS/
  cam_left/
  cam_right/
  imu/
  gnss/
```

Chunk files are named:

```text
<seconds>_<nanoseconds>_<chunk_duration>s_chunk.dat
```

Each record inside a chunk is length-prefixed:

```text
uint32_le protobuf_header_size
protobuf_header_bytes
uint32_le raw_payload_size
raw_payload_bytes
```

For IMU and GNSS, `raw_payload_size` is `0`. For camera streams, raw ZMQ
multipart image bytes are saved directly.

## Build

```bash
cd /home/lrs/mgx10v
source toolchain/mgx10v_sysroot/env.sh
receiver_test/senser_receiver/sensor_recorder/build.sh
```

Default output:

```text
receiver_test/sensor_recorder_build/
  sensor_recorder
  config.yaml
  proto/*.proto
  lib/
    libzmq.so*
    libprotobuf.so*
```

## Run

Start `sensor_data` first, then run:

```bash
cd /root/sensor_receiver/sensor_recorder
export LD_LIBRARY_PATH=$PWD/lib:${LD_LIBRARY_PATH:-}
./sensor_recorder --config config.yaml
```

Short test run:

```bash
./sensor_recorder --config config.yaml --duration 10
```
