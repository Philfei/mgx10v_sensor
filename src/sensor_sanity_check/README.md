# sensor_sanity_check

`sensor_sanity_check` checks data folders written by `sensor_recorder`.

It understands chunk records in this format:

```text
uint32_le protobuf_header_size
protobuf_header_bytes
uint32_le raw_payload_size
raw_payload_bytes
```

The checker parses only the protobuf header and seeks over raw image bytes, so
large camera chunks can be scanned without loading full frames into memory.

## Checks

- Dataset and sensor directories exist.
- `_chunk.dat` files are readable and complete.
- Protobuf headers contain timestamps.
- Timestamps are monotonic after sorting.
- Frequency, duration, longest interval, and gaps are reported.
- Camera records with `raw_payload_size == 0` are counted.

Supported sensor directories:

- `cam_left`
- `cam_right`
- `imu`
- `gnss`

## Build

```bash
cd /home/lrs/mgx10v
source toolchain/mgx10v_sysroot/env.sh
receiver_test/senser_receiver/sensor_sanity_check/build.sh \
  -DCMAKE_C_COMPILER=/usr/bin/aarch64-linux-gnu-gcc-9 \
  -DCMAKE_CXX_COMPILER=/usr/bin/aarch64-linux-gnu-g++-9
```

Default output:

```text
receiver_test/sensor_sanity_check_build/
  sensor_sanity_check
  config.yaml
  deploy/
```

## Run

Check one dataset:

```bash
./sensor_sanity_check --dir /root/sensor_receiver/record_data/data_2026-06-17-18-43-49
```

Check all datasets under a root and print every gap:

```bash
./sensor_sanity_check --dir /root/sensor_receiver/record_data --detailed
```

Gap thresholds are configured in `config.yaml`.
