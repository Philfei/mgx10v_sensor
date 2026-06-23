#!/usr/bin/env python3
"""Plot per-record timestamps of every sensor in a recorded dataset.

Reads the recorder's chunk framing (``[u32 hdr_size][hdr][u32 raw_size][raw]``)
for cam_left / cam_right / imu / gnss, extracts each record's timestamp, and
renders two stacked panels sharing a time axis:

  * a raster (one lane per sensor) showing sample coverage and gaps;
  * Δt between consecutive samples (log ms) showing rate stability / dropouts.

Image payloads are skipped with ``seek`` so the multi-GB cam directories stay
fast -- only the small protobuf header of each record is read.

Run ``plot_sensor_timestamps.py --selftest`` to exercise the parser.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import tempfile
from pathlib import Path

DEFAULT_DATASET_DIR = Path("data/data_2026-06-20-10-47-02")

# (sensor dir name, protobuf field path from record header down to the
# google.protobuf.Timestamp). Mirrors sensor_replay/replay_index.cpp:
#   RawImage -> field 1 is the Timestamp itself
#   ImuMsg   -> header(1).stamp(1)
#   GnssMsg  -> header(10).stamp(1)
SENSORS: list[tuple[str, tuple[int, ...]]] = [
    ("cam_left", (1,)),
    ("cam_right", (1,)),
    ("imu", (1, 1)),
    ("gnss", (10, 1)),
]

MAX_HEADER_SIZE = 16 * 1024 * 1024


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


def _field_bytes(blob: bytes, field_number: int) -> bytes | None:
    """Return the length-delimited (wire type 2) value of ``field_number``."""
    offset = 0
    while offset < len(blob):
        key, offset = _read_varint(blob, offset)
        fn = key >> 3
        wt = key & 0x07
        if wt == 0:
            _, offset = _read_varint(blob, offset)
        elif wt == 1:
            offset += 8
        elif wt == 2:
            size, offset = _read_varint(blob, offset)
            if fn == field_number:
                return blob[offset : offset + size]
            offset += size
        elif wt == 5:
            offset += 4
        else:
            raise ValueError(f"unsupported wire type {wt}")
    return None


def _timestamp_ns(ts_blob: bytes) -> int:
    """google.protobuf.Timestamp{seconds=1, nanos=2} -> nanoseconds."""
    seconds = 0
    nanos = 0
    offset = 0
    while offset < len(ts_blob):
        key, offset = _read_varint(ts_blob, offset)
        fn = key >> 3
        wt = key & 0x07
        if fn == 1 and wt == 0:
            seconds, offset = _read_varint(ts_blob, offset)
        elif fn == 2 and wt == 0:
            nanos, offset = _read_varint(ts_blob, offset)
        elif wt == 0:
            _, offset = _read_varint(ts_blob, offset)
        elif wt == 1:
            offset += 8
        elif wt == 2:
            size, offset = _read_varint(ts_blob, offset)
            offset += size
        elif wt == 5:
            offset += 4
        else:
            raise ValueError(f"unsupported wire type {wt}")
    return seconds * 1_000_000_000 + nanos


def _extract_ts(header: bytes, field_path: tuple[int, ...]) -> int | None:
    blob: bytes | None = header
    for fn in field_path:
        blob = _field_bytes(blob, fn)
        if blob is None:
            return None
    return _timestamp_ns(blob)


def read_sensor_timestamps(sensor_dir: Path, field_path: tuple[int, ...]):
    """Yield timestamp_ns for every record across the sensor's chunk files."""
    for chunk in sorted(sensor_dir.glob("*_chunk.dat")):
        with chunk.open("rb") as stream:
            while True:
                head = stream.read(4)
                if len(head) < 4:
                    break
                header_size = struct.unpack("<I", head)[0]
                if header_size == 0 or header_size > MAX_HEADER_SIZE:
                    break
                header = stream.read(header_size)
                if len(header) != header_size:
                    break
                raw_head = stream.read(4)
                if len(raw_head) < 4:
                    break
                raw_size = struct.unpack("<I", raw_head)[0]
                if raw_size:
                    stream.seek(raw_size, os.SEEK_CUR)  # skip payload, don't load
                ts = _extract_ts(header, field_path)
                if ts:
                    yield ts


def collect(dataset_dir: Path):
    """Return [(name, sorted np.ndarray[int64] of ns)] for present sensors."""
    import numpy as np

    series = []
    for name, field_path in SENSORS:
        sensor_dir = dataset_dir / name
        if not sensor_dir.is_dir():
            continue
        ts = np.fromiter(read_sensor_timestamps(sensor_dir, field_path), dtype=np.int64)
        if ts.size:
            ts.sort()
            series.append((name, ts))
        else:
            print(f"warning: no timestamps found for {name}", file=sys.stderr)
    return series


def _summary(series) -> None:
    import numpy as np

    print(f"{'sensor':<10} {'count':>7} {'dur[s]':>8} {'rate[Hz]':>9} "
          f"{'med dt[ms]':>11} {'max dt[ms]':>11}")
    for name, ts in series:
        dur = (ts[-1] - ts[0]) / 1e9
        rate = (ts.size - 1) / dur if dur > 0 else 0.0
        dt_ms = np.diff(ts) / 1e6 if ts.size > 1 else np.array([0.0])
        print(f"{name:<10} {ts.size:>7} {dur:>8.2f} {rate:>9.2f} "
              f"{np.median(dt_ms):>11.2f} {dt_ms.max():>11.2f}")


def plot(series, title: str, output: Path | None) -> None:
    import numpy as np

    os.environ.setdefault(
        "MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "matplotlib-cache")
    )
    import matplotlib

    if output is not None:  # headless save; otherwise keep the interactive backend
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    t0 = min(ts[0] for _, ts in series)
    cmap = plt.get_cmap("tab10")
    colors = {name: cmap(i % 10) for i, (name, _) in enumerate(series)}

    fig, (ax_raster, ax_dt) = plt.subplots(
        2, 1, figsize=(12, 7), sharex=True, height_ratios=[1, 2]
    )

    for i, (name, ts) in enumerate(series):
        rel = (ts - t0) / 1e9
        ax_raster.plot(rel, np.full(ts.size, i), "|", markersize=8,
                       color=colors[name])
        if ts.size > 1:
            dur = (ts[-1] - ts[0]) / 1e9
            rate = (ts.size - 1) / dur if dur > 0 else 0.0
            ax_dt.plot(rel[1:], np.diff(ts) / 1e6, ".", markersize=2,
                       color=colors[name], label=f"{name} (~{rate:.1f} Hz)")

    ax_raster.set_yticks(range(len(series)))
    ax_raster.set_yticklabels([name for name, _ in series])
    ax_raster.set_ylim(-0.5, len(series) - 0.5)
    ax_raster.set_title(f"Sensor sample timestamps — {title}")
    ax_raster.grid(True, axis="x", alpha=0.3)

    ax_dt.set_yscale("log")
    ax_dt.set_ylabel("Δt between consecutive samples [ms]")
    ax_dt.set_xlabel("time since first sample [s]")
    ax_dt.grid(True, which="both", alpha=0.3)
    ax_dt.legend(loc="upper right", markerscale=4)

    fig.tight_layout()
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(output, dpi=150)
        print(f"wrote {output}")
        plt.close(fig)
    else:
        plt.show()


def _selftest() -> int:
    # Build a Timestamp{sec=12, nanos=345}, wrap it as Header.stamp(1),
    # then as ImuMsg.header(1); assert the parser recovers 12e9 + 345.
    def tag(fn, wt):
        return bytes([(fn << 3) | wt])

    def varint(v):
        out = bytearray()
        while True:
            b = v & 0x7F
            v >>= 7
            out.append(b | (0x80 if v else 0))
            if not v:
                return bytes(out)

    def ld(fn, payload):  # length-delimited field
        return tag(fn, 2) + varint(len(payload)) + payload

    ts = tag(1, 0) + varint(12) + tag(2, 0) + varint(345)
    assert _timestamp_ns(ts) == 12 * 1_000_000_000 + 345
    raw_image = ld(1, ts)                       # RawImage.timestamp(1)
    assert _extract_ts(raw_image, (1,)) == 12_000_000_345
    imu = ld(1, ld(1, ts))                      # ImuMsg.header(1).stamp(1)
    assert _extract_ts(imu, (1, 1)) == 12_000_000_345
    gnss = ld(10, ld(1, ts)) + tag(2, 0) + varint(7)  # GnssMsg.header(10).stamp
    assert _extract_ts(gnss, (10, 1)) == 12_000_000_345
    assert _extract_ts(ld(2, ts), (1,)) is None  # missing field -> None
    print("selftest OK")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", nargs="?", type=Path, default=DEFAULT_DATASET_DIR,
                        help=f"dataset directory (default: {DEFAULT_DATASET_DIR})")
    parser.add_argument("-o", "--output", type=Path, default=None,
                        help="save the figure to this path (headless) instead of "
                             "showing an interactive window")
    parser.add_argument("--selftest", action="store_true",
                        help="run the parser self-check and exit")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    if args.selftest:
        return _selftest()

    if not args.dataset.is_dir():
        raise SystemExit(f"dataset directory not found: {args.dataset}")

    series = collect(args.dataset)
    if not series:
        raise SystemExit(f"no sensor timestamps found under {args.dataset}")

    _summary(series)
    plot(series, args.dataset.name, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
