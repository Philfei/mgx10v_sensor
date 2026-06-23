#!/usr/bin/env python3
"""Extract IMU samples from recorded sensor chunks into an EuRoC-style CSV.

The chunk records hold a ROS sensor_msgs/Imu protobuf:
    field 1 = header {timestamp{sec,nsec}, frame_id}
    field 4 = angular_velocity   (Vector3, rad/s)
    field 6 = linear_acceleration (Vector3, g)   <-- NOT m/s^2

The accelerometer reports specific force in units of g (rest magnitude ~= 1.0),
so it is multiplied by g0 to produce m/s^2 for ORB-SLAM3 / EuRoC consumers.
Output columns: timestamp[ns], wx, wy, wz, ax, ay, az
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

DEFAULT_DATA_DIR = Path("data/data_2026-06-20-10-47-02")
DEFAULT_OUTPUT = Path("data/imu_260620_1.csv")
STANDARD_GRAVITY = 9.80665  # m/s^2 per g
MAX_HEADER_SIZE = 16 * 1024 * 1024
MAX_RAW_SIZE = 512 * 1024 * 1024


def _read_u32_le(stream) -> int | None:
    data = stream.read(4)
    if not data:
        return None
    if len(data) != 4:
        raise ValueError("incomplete uint32 field")
    return struct.unpack("<I", data)[0]


def _read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(data):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("varint too large")
    raise ValueError("truncated varint")


def _skip_field(data: bytes, offset: int, wire_type: int) -> int:
    if wire_type == 0:
        _, offset = _read_varint(data, offset)
        return offset
    if wire_type == 1:
        return offset + 8
    if wire_type == 2:
        size, offset = _read_varint(data, offset)
        return offset + size
    if wire_type == 5:
        return offset + 4
    raise ValueError(f"unsupported protobuf wire type: {wire_type}")


def _parse_vector3(data: bytes) -> tuple[float, float, float]:
    x = y = z = 0.0
    offset = 0
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field, wire = key >> 3, key & 0x07
        if wire == 1:
            val = struct.unpack_from("<d", data, offset)[0]
            offset += 8
            if field == 1:
                x = val
            elif field == 2:
                y = val
            elif field == 3:
                z = val
        else:
            offset = _skip_field(data, offset, wire)
    return x, y, z


def _parse_timestamp_ns(data: bytes) -> int | None:
    """header submessage -> field 1 = timestamp{sec(1), nsec(2)}."""
    offset = 0
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field, wire = key >> 3, key & 0x07
        if field == 1 and wire == 2:
            size, offset = _read_varint(data, offset)
            sub, end = data[offset:offset + size], offset + size
            sec = nsec = 0
            o = 0
            while o < len(sub):
                k, o = _read_varint(sub, o)
                f, w = k >> 3, k & 0x07
                if f == 1 and w == 0:
                    sec, o = _read_varint(sub, o)
                elif f == 2 and w == 0:
                    nsec, o = _read_varint(sub, o)
                else:
                    o = _skip_field(sub, o, w)
            return sec * 1_000_000_000 + nsec
        offset = _skip_field(data, offset, wire)
    return None


def parse_imu_message(data: bytes) -> tuple[int, tuple[float, float, float], tuple[float, float, float]] | None:
    timestamp_ns = None
    gyro = (0.0, 0.0, 0.0)
    acc = (0.0, 0.0, 0.0)
    offset = 0
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field, wire = key >> 3, key & 0x07
        if wire == 2:
            size, offset = _read_varint(data, offset)
            sub = data[offset:offset + size]
            offset += size
            if field == 1:
                timestamp_ns = _parse_timestamp_ns(sub)
            elif field == 4:
                gyro = _parse_vector3(sub)
            elif field == 6:
                acc = _parse_vector3(sub)
        else:
            offset = _skip_field(data, offset, wire)
    if timestamp_ns is None:
        return None
    return timestamp_ns, gyro, acc


def _iter_records(chunk_path: Path):
    with chunk_path.open("rb") as stream:
        while True:
            header_size = _read_u32_le(stream)
            if header_size is None or header_size == 0:
                break
            if header_size > MAX_HEADER_SIZE:
                raise ValueError(f"{chunk_path}: header too large")
            header_bytes = stream.read(header_size)
            if len(header_bytes) != header_size:
                raise ValueError(f"{chunk_path}: incomplete header")
            raw_size = _read_u32_le(stream)
            if raw_size is None:
                raise ValueError(f"{chunk_path}: missing raw payload size")
            if raw_size > MAX_RAW_SIZE:
                raise ValueError(f"{chunk_path}: raw payload too large")
            payload = stream.read(raw_size)
            if len(payload) != raw_size:
                raise ValueError(f"{chunk_path}: incomplete raw payload")
            yield header_bytes


def _resolve_imu_dir(data_dir: Path) -> Path:
    return data_dir if data_dir.name == "imu" else data_dir / "imu"


def extract_imu(data_dir: Path, output: Path, accel_scale: float) -> dict:
    imu_dir = _resolve_imu_dir(Path(data_dir))
    if not imu_dir.is_dir():
        raise FileNotFoundError(f"imu directory not found: {imu_dir}")

    samples: list[tuple[int, tuple[float, float, float], tuple[float, float, float]]] = []
    chunks = sorted(imu_dir.glob("*_chunk.dat"))
    if not chunks:
        raise FileNotFoundError(f"no *_chunk.dat files in {imu_dir}")
    for chunk in chunks:
        for record in _iter_records(chunk):
            parsed = parse_imu_message(record)
            if parsed is not None:
                samples.append(parsed)

    samples.sort(key=lambda s: s[0])

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as f:
        f.write("#timestamp [ns],w_RS_S_x [rad s^-1],w_RS_S_y [rad s^-1],"
                "w_RS_S_z [rad s^-1],a_RS_S_x [m s^-2],a_RS_S_y [m s^-2],a_RS_S_z [m s^-2]\n")
        for ts, (gx, gy, gz), (ax, ay, az) in samples:
            f.write(f"{ts},{gx:.9f},{gy:.9f},{gz:.9f},"
                    f"{ax * accel_scale:.9f},{ay * accel_scale:.9f},{az * accel_scale:.9f}\n")

    # Stats to sanity-check the accelerometer unit: mean |a| should be ~9.81 m/s^2.
    n = len(samples)
    mean_acc_mag = 0.0
    rate = 0.0
    if n:
        mean_acc_mag = sum(
            (ax * accel_scale) ** 2 + (ay * accel_scale) ** 2 + (az * accel_scale) ** 2
            for _, _, (ax, ay, az) in samples
        )
        mean_acc_mag = (mean_acc_mag / n) ** 0.5  # RMS magnitude
    if n > 1:
        span = (samples[-1][0] - samples[0][0]) * 1e-9
        rate = (n - 1) / span if span > 0 else 0.0
    return {"count": n, "rate_hz": rate, "rms_acc_mag": mean_acc_mag, "output": output}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Extract IMU chunks to EuRoC-style CSV.")
    parser.add_argument("data_dir", nargs="?", type=Path, default=DEFAULT_DATA_DIR,
                        help=f"dataset dir or imu dir (default: {DEFAULT_DATA_DIR})")
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT,
                        help=f"output CSV path (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--accel-unit", choices=["g", "ms2"], default="g",
                        help="raw accelerometer unit (default: g -> scaled to m/s^2)")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    scale = STANDARD_GRAVITY if args.accel_unit == "g" else 1.0
    stats = extract_imu(args.data_dir, args.output, scale)
    print(f"wrote {stats['count']} IMU samples (~{stats['rate_hz']:.1f} Hz) to {stats['output']}")
    print(f"RMS |acc| = {stats['rms_acc_mag']:.4f} m/s^2 (expect ~9.81 at rest -> unit '{args.accel_unit}' OK)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
