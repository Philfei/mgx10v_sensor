#!/usr/bin/env python3
"""Extract GNSS trajectory files from recorded sensor data chunks."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import math
import os
import struct
import sys
import tempfile
from pathlib import Path


DEFAULT_DATASET_DIR = Path("data/data_2021-12-07-15-28-01")
DEFAULT_OUTPUT_ROOT = Path("gnss_trajs")
MAX_HEADER_SIZE = 16 * 1024 * 1024
MAX_RAW_SIZE = 64 * 1024 * 1024

WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)
GPS_UNIX_EPOCH_SECONDS = 315964800.0


@dataclasses.dataclass(frozen=True)
class GnssSample:
    timestamp: float | None = None
    timestamp_ns: int | None = None
    gps_week: int | None = None
    gps_tow: float | None = None
    pos_type: int = 0
    pos_sol_status: int = 0
    blh: tuple[float, ...] = ()
    blh_std: tuple[float, ...] = ()
    vel: tuple[float, ...] = ()
    blh_cov: tuple[float, ...] = ()
    diff_age: float | None = None
    sol_age: float | None = None
    soln_svs: int = 0
    hrms: float | None = None
    vrms: float | None = None
    hdop: float | None = None
    vdop: float | None = None


@dataclasses.dataclass(frozen=True)
class TrajectoryPoint:
    sample: GnssSample
    east: float
    north: float
    up: float


@dataclasses.dataclass(frozen=True)
class ProcessResult:
    dataset_dir: Path
    gnss_dir: Path
    output_dir: Path
    chunks_read: int
    records_read: int
    valid_points: int
    fixed_points: int
    plot_files: tuple[Path, ...]


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
        offset += 8
    elif wire_type == 2:
        size, offset = _read_varint(data, offset)
        offset += size
    elif wire_type == 5:
        offset += 4
    else:
        raise ValueError(f"unsupported protobuf wire type: {wire_type}")

    if offset > len(data):
        raise ValueError("protobuf field extends past message")
    return offset


def _read_double(data: bytes, offset: int) -> tuple[float, int]:
    if offset + 8 > len(data):
        raise ValueError("truncated double field")
    return struct.unpack_from("<d", data, offset)[0], offset + 8


def _read_float(data: bytes, offset: int) -> tuple[float, int]:
    if offset + 4 > len(data):
        raise ValueError("truncated float field")
    return struct.unpack_from("<f", data, offset)[0], offset + 4


def _read_repeated_double(
    data: bytes, offset: int, wire_type: int
) -> tuple[list[float], int]:
    if wire_type == 1:
        value, offset = _read_double(data, offset)
        return [value], offset
    if wire_type != 2:
        raise ValueError(f"unexpected repeated double wire type: {wire_type}")

    size, offset = _read_varint(data, offset)
    end = offset + size
    if end > len(data):
        raise ValueError("packed double field extends past message")
    if size % 8 != 0:
        raise ValueError("packed double field size is not a multiple of 8")

    values = []
    while offset < end:
        value, offset = _read_double(data, offset)
        values.append(value)
    return values, offset


def _parse_timestamp(data: bytes) -> tuple[int | None, int | None]:
    offset = 0
    seconds = None
    nanos = None
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 0x07
        if field_number == 1 and wire_type == 0:
            seconds, offset = _read_varint(data, offset)
        elif field_number == 2 and wire_type == 0:
            nanos, offset = _read_varint(data, offset)
        else:
            offset = _skip_field(data, offset, wire_type)
    return seconds, nanos


def _parse_header(data: bytes) -> tuple[int | None, int | None]:
    offset = 0
    seconds = None
    nanos = None
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 0x07
        if field_number == 1 and wire_type == 2:
            size, offset = _read_varint(data, offset)
            end = offset + size
            if end > len(data):
                raise ValueError("timestamp field extends past header")
            seconds, nanos = _parse_timestamp(data[offset:end])
            offset = end
        else:
            offset = _skip_field(data, offset, wire_type)
    return seconds, nanos


def _parse_gps_time(data: bytes) -> tuple[int | None, float | None]:
    offset = 0
    week = None
    tow = None
    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 0x07
        if field_number == 1 and wire_type == 0:
            week, offset = _read_varint(data, offset)
        elif field_number == 2 and wire_type == 1:
            tow, offset = _read_double(data, offset)
        else:
            offset = _skip_field(data, offset, wire_type)
    return week, tow


def parse_gnss_message(data: bytes) -> GnssSample:
    offset = 0
    timestamp = None
    timestamp_ns = None
    gps_week = None
    gps_tow = None
    pos_type = 0
    pos_sol_status = 0
    blh: list[float] = []
    blh_std: list[float] = []
    vel: list[float] = []
    blh_cov: list[float] = []
    diff_age = None
    sol_age = None
    soln_svs = 0
    hrms = None
    vrms = None
    hdop = None
    vdop = None

    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 0x07

        if field_number == 1 and wire_type == 2:
            size, offset = _read_varint(data, offset)
            end = offset + size
            if end > len(data):
                raise ValueError("gps time field extends past message")
            gps_week, gps_tow = _parse_gps_time(data[offset:end])
            offset = end
        elif field_number == 2 and wire_type == 0:
            pos_type, offset = _read_varint(data, offset)
        elif field_number == 3:
            values, offset = _read_repeated_double(data, offset, wire_type)
            blh.extend(values)
        elif field_number == 4:
            values, offset = _read_repeated_double(data, offset, wire_type)
            blh_std.extend(values)
        elif field_number == 5:
            values, offset = _read_repeated_double(data, offset, wire_type)
            vel.extend(values)
        elif field_number == 6 and wire_type == 0:
            pos_sol_status, offset = _read_varint(data, offset)
        elif field_number == 7 and wire_type == 1:
            diff_age, offset = _read_double(data, offset)
        elif field_number == 8 and wire_type == 1:
            sol_age, offset = _read_double(data, offset)
        elif field_number == 9 and wire_type == 0:
            soln_svs, offset = _read_varint(data, offset)
        elif field_number == 10 and wire_type == 2:
            size, offset = _read_varint(data, offset)
            end = offset + size
            if end > len(data):
                raise ValueError("header field extends past message")
            seconds, nanos = _parse_header(data[offset:end])
            if seconds is not None:
                nanos = nanos or 0
                timestamp_ns = seconds * 1_000_000_000 + nanos
                timestamp = timestamp_ns / 1_000_000_000.0
            offset = end
        elif field_number == 11:
            values, offset = _read_repeated_double(data, offset, wire_type)
            blh_cov.extend(values)
        elif field_number == 12 and wire_type == 5:
            hrms, offset = _read_float(data, offset)
        elif field_number == 13 and wire_type == 5:
            vrms, offset = _read_float(data, offset)
        elif field_number == 14 and wire_type == 5:
            hdop, offset = _read_float(data, offset)
        elif field_number == 15 and wire_type == 5:
            vdop, offset = _read_float(data, offset)
        else:
            offset = _skip_field(data, offset, wire_type)

    if timestamp is None and gps_week is not None and gps_tow is not None:
        timestamp = GPS_UNIX_EPOCH_SECONDS + gps_week * 604800.0 + gps_tow

    return GnssSample(
        timestamp=timestamp,
        timestamp_ns=timestamp_ns,
        gps_week=gps_week,
        gps_tow=gps_tow,
        pos_type=pos_type,
        pos_sol_status=pos_sol_status,
        blh=tuple(blh),
        blh_std=tuple(blh_std),
        vel=tuple(vel),
        blh_cov=tuple(blh_cov),
        diff_age=diff_age,
        sol_age=sol_age,
        soln_svs=soln_svs,
        hrms=hrms,
        vrms=vrms,
        hdop=hdop,
        vdop=vdop,
    )


def _iter_chunk_messages(chunk_path: Path):
    with chunk_path.open("rb") as stream:
        while True:
            header_size = _read_u32_le(stream)
            if header_size is None:
                break
            if header_size == 0:
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

            raw_payload = stream.read(raw_size)
            if len(raw_payload) != raw_size:
                raise ValueError(f"{chunk_path}: incomplete raw payload")

            yield header_bytes


def _iter_pb_messages(pb_path: Path):
    data = pb_path.read_bytes()
    if data:
        yield data


def _chunk_paths(gnss_dir: Path) -> list[Path]:
    return sorted(gnss_dir.glob("*_chunk.dat"))


def _pb_paths(gnss_dir: Path) -> list[Path]:
    return sorted(gnss_dir.glob("*.pb"))


def _resolve_dataset(data_dir: Path) -> tuple[Path, Path]:
    data_dir = Path(data_dir)
    if data_dir.name == "gnss":
        return data_dir.parent, data_dir
    return data_dir, data_dir / "gnss"


def _is_valid_trajectory_sample(sample: GnssSample) -> bool:
    return (
        sample.timestamp is not None
        and sample.pos_sol_status == 0
        and len(sample.blh) >= 3
    )


def _is_fixed_solution_sample(sample: GnssSample) -> bool:
    return _is_valid_trajectory_sample(sample) and sample.pos_type == 4


def _blh_to_ecef(
    lat_deg: float, lon_deg: float, height_m: float
) -> tuple[float, float, float]:
    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    n = WGS84_A / math.sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat)
    x = (n + height_m) * cos_lat * math.cos(lon)
    y = (n + height_m) * cos_lat * math.sin(lon)
    z = (n * (1.0 - WGS84_E2) + height_m) * sin_lat
    return x, y, z


def _ecef_to_enu(
    ecef: tuple[float, float, float],
    ref_ecef: tuple[float, float, float],
    ref_lat_deg: float,
    ref_lon_deg: float,
) -> tuple[float, float, float]:
    lat = math.radians(ref_lat_deg)
    lon = math.radians(ref_lon_deg)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    sin_lon = math.sin(lon)
    cos_lon = math.cos(lon)
    dx = ecef[0] - ref_ecef[0]
    dy = ecef[1] - ref_ecef[1]
    dz = ecef[2] - ref_ecef[2]

    east = -sin_lon * dx + cos_lon * dy
    north = -sin_lat * cos_lon * dx - sin_lat * sin_lon * dy + cos_lat * dz
    up = cos_lat * cos_lon * dx + cos_lat * sin_lon * dy + sin_lat * dz
    return east, north, up


def _trajectory_points(samples: list[GnssSample]) -> list[TrajectoryPoint]:
    if not samples:
        return []

    ref_lat, ref_lon, ref_height = samples[0].blh[:3]
    ref_ecef = _blh_to_ecef(ref_lat, ref_lon, ref_height)

    points = []
    for sample in samples:
        lat, lon, height = sample.blh[:3]
        ecef = _blh_to_ecef(lat, lon, height)
        east, north, up = _ecef_to_enu(ecef, ref_ecef, ref_lat, ref_lon)
        points.append(TrajectoryPoint(sample=sample, east=east, north=north, up=up))
    return points


def _timestamp_text(sample: GnssSample) -> str:
    if sample.timestamp_ns is not None:
        seconds, nanos = divmod(sample.timestamp_ns, 1_000_000_000)
        return f"{seconds}.{nanos:09d}"
    if sample.timestamp is None:
        return ""
    return f"{sample.timestamp:.9f}"


def _format_float(value: float | None, digits: int = 9) -> str:
    if value is None:
        return ""
    return f"{value:.{digits}f}"


def _write_tum(path: Path, points: list[TrajectoryPoint]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        handle.write("# timestamp tx ty tz qx qy qz qw\n")
        for point in points:
            handle.write(
                f"{_timestamp_text(point.sample)} "
                f"{point.east:.9f} {point.north:.9f} {point.up:.9f} "
                "0.000000000 0.000000000 0.000000000 1.000000000\n"
            )


def _write_metrics(path: Path, points: list[TrajectoryPoint]) -> None:
    fieldnames = [
        "timestamp",
        "latitude_deg",
        "longitude_deg",
        "height_m",
        "east_m",
        "north_m",
        "up_m",
        "pos_type",
        "pos_sol_status",
        "solnSVs",
        "hrms",
        "vrms",
        "hdop",
        "vdop",
        "diff_age",
        "sol_age",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for point in points:
            sample = point.sample
            lat, lon, height = sample.blh[:3]
            writer.writerow(
                {
                    "timestamp": _timestamp_text(sample),
                    "latitude_deg": f"{lat:.12f}",
                    "longitude_deg": f"{lon:.12f}",
                    "height_m": f"{height:.4f}",
                    "east_m": f"{point.east:.6f}",
                    "north_m": f"{point.north:.6f}",
                    "up_m": f"{point.up:.6f}",
                    "pos_type": str(sample.pos_type),
                    "pos_sol_status": str(sample.pos_sol_status),
                    "solnSVs": str(sample.soln_svs),
                    "hrms": _format_float(sample.hrms, 6),
                    "vrms": _format_float(sample.vrms, 6),
                    "hdop": _format_float(sample.hdop, 6),
                    "vdop": _format_float(sample.vdop, 6),
                    "diff_age": _format_float(sample.diff_age, 3),
                    "sol_age": _format_float(sample.sol_age, 3),
                }
            )


def _relative_times(points: list[TrajectoryPoint]) -> list[float]:
    if not points or points[0].sample.timestamp is None:
        return []
    start = points[0].sample.timestamp
    return [(point.sample.timestamp or start) - start for point in points]


def _make_plots(output_dir: Path, points: list[TrajectoryPoint]) -> tuple[Path, ...]:
    if not points:
        return ()

    try:
        cache_dir = Path(tempfile.gettempdir()) / "matplotlib-cache"
        cache_dir.mkdir(parents=True, exist_ok=True)
        os.environ.setdefault("MPLCONFIGDIR", str(cache_dir))

        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return ()

    times = _relative_times(points)
    if not times:
        return ()

    plot_files: list[Path] = []

    xyz_path = output_dir / "trajectory_xyz.png"
    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(10, 7))
    axes[0].plot(times, [point.east for point in points], linewidth=1.2)
    axes[0].set_ylabel("east (m)")
    axes[1].plot(times, [point.north for point in points], linewidth=1.2)
    axes[1].set_ylabel("north (m)")
    axes[2].plot(times, [point.up for point in points], linewidth=1.2)
    axes[2].set_ylabel("up (m)")
    axes[2].set_xlabel("time (s)")
    fig.tight_layout()
    fig.savefig(xyz_path, dpi=150)
    plt.close(fig)
    plot_files.append(xyz_path)

    status_path = output_dir / "gnss_solution_status.png"
    fig, axes = plt.subplots(2, 1, sharex=True, figsize=(10, 5))
    axes[0].plot(times, [point.sample.pos_type for point in points], linewidth=1.0)
    axes[0].set_ylabel("pos_type")
    axes[1].plot(
        times, [point.sample.pos_sol_status for point in points], linewidth=1.0
    )
    axes[1].set_ylabel("status")
    axes[1].set_xlabel("time (s)")
    fig.tight_layout()
    fig.savefig(status_path, dpi=150)
    plt.close(fig)
    plot_files.append(status_path)

    accuracy_path = output_dir / "gnss_accuracy_dop.png"
    fig, ax = plt.subplots(figsize=(10, 5))
    plotted_accuracy = False
    for label, values in (
        ("hrms", [point.sample.hrms for point in points]),
        ("vrms", [point.sample.vrms for point in points]),
        ("hdop", [point.sample.hdop for point in points]),
        ("vdop", [point.sample.vdop for point in points]),
    ):
        if any(value is not None for value in values):
            plotted_accuracy = True
            ax.plot(
                times,
                [float("nan") if value is None else value for value in values],
                label=label,
                linewidth=1.0,
            )
    ax.set_xlabel("time (s)")
    if plotted_accuracy:
        ax.legend(loc="best")
    else:
        ax.text(0.5, 0.5, "no accuracy/dop data", ha="center", va="center")
    fig.tight_layout()
    fig.savefig(accuracy_path, dpi=150)
    plt.close(fig)
    plot_files.append(accuracy_path)

    return tuple(plot_files)


def process_dataset(
    data_dir: Path,
    output_root: Path = DEFAULT_OUTPUT_ROOT,
    *,
    make_plots: bool = True,
) -> ProcessResult:
    dataset_dir, gnss_dir = _resolve_dataset(data_dir)
    if not gnss_dir.is_dir():
        raise FileNotFoundError(f"gnss directory not found: {gnss_dir}")

    output_dir = Path(output_root) / dataset_dir.name
    output_dir.mkdir(parents=True, exist_ok=True)

    chunk_paths = _chunk_paths(gnss_dir)
    pb_paths = [] if chunk_paths else _pb_paths(gnss_dir)
    if not chunk_paths and not pb_paths:
        raise FileNotFoundError(f"no *_chunk.dat or *.pb files found in {gnss_dir}")

    records_read = 0
    samples: list[GnssSample] = []

    for chunk_path in chunk_paths:
        for message in _iter_chunk_messages(chunk_path):
            records_read += 1
            sample = parse_gnss_message(message)
            if _is_valid_trajectory_sample(sample):
                samples.append(sample)

    for pb_path in pb_paths:
        for message in _iter_pb_messages(pb_path):
            records_read += 1
            sample = parse_gnss_message(message)
            if _is_valid_trajectory_sample(sample):
                samples.append(sample)

    points = _trajectory_points(samples)
    fixed_points = [
        point for point in points if _is_fixed_solution_sample(point.sample)
    ]

    _write_tum(output_dir / "traj_gt.txt", points)
    _write_tum(output_dir / "traj_gt_fixed.txt", fixed_points)
    _write_metrics(output_dir / "gnss_metrics.csv", points)
    plot_files = _make_plots(output_dir, points) if make_plots else ()

    return ProcessResult(
        dataset_dir=dataset_dir,
        gnss_dir=gnss_dir,
        output_dir=output_dir,
        chunks_read=len(chunk_paths),
        records_read=records_read,
        valid_points=len(points),
        fixed_points=len(fixed_points),
        plot_files=plot_files,
    )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract GNSS trajectory and metrics from recorded chunk data."
    )
    parser.add_argument(
        "data_dir",
        nargs="?",
        type=Path,
        default=DEFAULT_DATASET_DIR,
        help=f"dataset directory or gnss directory (default: {DEFAULT_DATASET_DIR})",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=DEFAULT_OUTPUT_ROOT,
        help=f"output root directory (default: {DEFAULT_OUTPUT_ROOT})",
    )
    parser.add_argument(
        "--no-plots",
        action="store_true",
        help="skip trajectory/status/accuracy plot generation",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    result = process_dataset(
        args.data_dir,
        args.output_root,
        make_plots=not args.no_plots,
    )
    print(
        f"read {result.records_read} GNSS records from {result.chunks_read} chunks; "
        f"wrote {result.valid_points} trajectory points and "
        f"{result.fixed_points} fixed points to {result.output_dir}"
    )
    if result.plot_files:
        print("plots: " + ", ".join(str(path) for path in result.plot_files))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
