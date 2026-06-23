#!/usr/bin/env python3
"""Extract recorded camera image chunks into image files.

The recorder stores each frame verbatim as sensor_data published it, so the
payload encoding varies by capture: `nv12` (current sensor_data native format),
`jpg`, `bgr8`/`rgb8`/`bgra8`, or `mono8`. This tool auto-detects the encoding
from each record's RawImage header and decodes to BGR. Output defaults to
lossless PNG so a lossless source (nv12/bgr8/mono8) is not re-compressed; pass
`--format jpg` for smaller (lossy) files.
"""

from __future__ import annotations

import argparse
import dataclasses
import struct
import sys
from pathlib import Path

try:
    import cv2
    import numpy as np
except ImportError as exc:  # pragma: no cover - environment guard
    raise SystemExit(
        "This script requires OpenCV and NumPy for Python. "
        "Install python3-opencv/numpy or run in the project Python environment."
    ) from exc


DEFAULT_INPUT_DIR = Path("data/data_2026-06-21-16-41-33")
DEFAULT_OUTPUT_DIR = Path("data/photos")
MAX_HEADER_SIZE = 16 * 1024 * 1024
MAX_RAW_SIZE = 512 * 1024 * 1024


@dataclasses.dataclass(frozen=True)
class RawImageHeader:
    seconds: int = 0
    nanos: int = 0
    width: int = 0
    height: int = 0
    encoding: str = ""
    step: int = 0
    data_size: int = 0
    sequence: int = 0


@dataclasses.dataclass
class ExtractStats:
    chunks_seen: int = 0
    records_seen: int = 0
    frames_written: int = 0
    skipped_empty: int = 0


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


def _parse_timestamp(data: bytes) -> tuple[int, int]:
    offset = 0
    seconds = 0
    nanos = 0
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


def parse_raw_image_header(data: bytes) -> RawImageHeader:
    offset = 0
    seconds = 0
    nanos = 0
    width = 0
    height = 0
    encoding = ""
    step = 0
    data_size = 0
    sequence = 0

    while offset < len(data):
        key, offset = _read_varint(data, offset)
        field_number = key >> 3
        wire_type = key & 0x07

        if field_number == 1 and wire_type == 2:
            size, offset = _read_varint(data, offset)
            seconds, nanos = _parse_timestamp(data[offset : offset + size])
            offset += size
        elif field_number == 2 and wire_type == 5:
            width = struct.unpack_from("<I", data, offset)[0]
            offset += 4
        elif field_number == 3 and wire_type == 5:
            height = struct.unpack_from("<I", data, offset)[0]
            offset += 4
        elif field_number == 4 and wire_type == 2:
            size, offset = _read_varint(data, offset)
            encoding = data[offset : offset + size].decode("utf-8", errors="replace")
            offset += size
        elif field_number == 5 and wire_type == 5:
            step = struct.unpack_from("<I", data, offset)[0]
            offset += 4
        elif field_number == 10 and wire_type == 1:
            data_size = struct.unpack_from("<Q", data, offset)[0]
            offset += 8
        elif field_number == 11 and wire_type == 1:
            sequence = struct.unpack_from("<Q", data, offset)[0]
            offset += 8
        else:
            offset = _skip_field(data, offset, wire_type)

        if offset > len(data):
            raise ValueError("protobuf field extends past header")

    return RawImageHeader(
        seconds=seconds,
        nanos=nanos,
        width=width,
        height=height,
        encoding=encoding,
        step=step,
        data_size=data_size,
        sequence=sequence,
    )


def _image_from_payload(header: RawImageHeader, payload: bytes) -> np.ndarray:
    if header.width <= 0 or header.height <= 0:
        raise ValueError("image header is missing width or height")

    encoding = header.encoding.lower()
    step = header.step
    if encoding in {"jpg", "jpeg", "png"}:
        # Compressed payload (e.g. sensor_data hardware-JPEG): decode to BGR.
        image = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError(f"failed to decode {encoding} payload")
        return image
    if encoding in {"nv12", "nv21"}:
        # Packed YUV420 semi-planar (sensor_data native): Y plane (h rows) +
        # interleaved UV (h/2 rows), row stride = step (defaults to width).
        stride = step or header.width
        _validate_payload_size_rows(header, payload, stride, header.height * 3 // 2)
        yuv = np.frombuffer(payload, dtype=np.uint8).reshape(
            (header.height * 3 // 2, stride)
        )[:, : header.width]
        code = cv2.COLOR_YUV2BGR_NV12 if encoding == "nv12" else cv2.COLOR_YUV2BGR_NV21
        return cv2.cvtColor(np.ascontiguousarray(yuv), code)
    if encoding == "bgr8":
        channels = 3
        step = step or header.width * channels
        _validate_payload_size(header, payload, step)
        image = np.frombuffer(payload, dtype=np.uint8).reshape((header.height, step))[
            :, : header.width * channels
        ]
        return image.reshape((header.height, header.width, channels))
    if encoding == "rgb8":
        channels = 3
        step = step or header.width * channels
        _validate_payload_size(header, payload, step)
        image = np.frombuffer(payload, dtype=np.uint8).reshape((header.height, step))[
            :, : header.width * channels
        ]
        image = image.reshape((header.height, header.width, channels))
        return cv2.cvtColor(image, cv2.COLOR_RGB2BGR)
    if encoding == "bgra8":
        channels = 4
        step = step or header.width * channels
        _validate_payload_size(header, payload, step)
        image = np.frombuffer(payload, dtype=np.uint8).reshape((header.height, step))[
            :, : header.width * channels
        ]
        image = image.reshape((header.height, header.width, channels))
        return cv2.cvtColor(image, cv2.COLOR_BGRA2BGR)
    if encoding in {"mono8", "gray8", "grey8"}:
        step = step or header.width
        _validate_payload_size(header, payload, step)
        image = np.frombuffer(payload, dtype=np.uint8).reshape((header.height, step))[
            :, : header.width
        ]
        return image

    raise ValueError(f"unsupported image encoding: {header.encoding!r}")


def _validate_payload_size(
    header: RawImageHeader, payload: bytes, step: int
) -> None:
    _validate_payload_size_rows(header, payload, step, header.height)


def _validate_payload_size_rows(
    header: RawImageHeader, payload: bytes, step: int, rows: int
) -> None:
    expected_size = rows * step
    if len(payload) != expected_size:
        raise ValueError(
            "raw payload size does not match image header: "
            f"got {len(payload)}, expected {expected_size}"
        )


def _output_name(header: RawImageHeader, frame_index: int, image_format: str) -> str:
    timestamp_ns = header.seconds * 1_000_000_000 + header.nanos
    if timestamp_ns > 0:
        stem = f"{timestamp_ns:019d}"
    else:
        stem = f"frame_{frame_index:06d}"
    if header.sequence:
        stem += f"_seq{header.sequence}"
    return f"{stem}.{image_format}"


def _chunk_paths(input_dir: Path) -> list[Path]:
    return sorted(input_dir.glob("*_chunk.dat"))


def _resolve_camera_dirs(input_dir: Path) -> list[tuple[Path, str]]:
    """Return the directories that actually hold image chunks, as (dir, label).

    Accepts either a camera chunk directory (``.../cam_left``) or a dataset root
    (``.../data_xxx`` containing ``cam_left/``, ``cam_right/``). Only descends
    into the known camera streams -- never ``imu/``/``gnss/`` (those chunks are
    not RawImage and would mis-parse).
    """
    input_dir = Path(input_dir)
    if _chunk_paths(input_dir):
        return [(input_dir, "")]  # chunks directly in this dir
    found: list[tuple[Path, str]] = []
    for name in ("cam_left", "cam_right"):
        sub = input_dir / name
        if sub.is_dir() and _chunk_paths(sub):
            found.append((sub, name))
    return found


def extract_photos(
    input_dir: Path,
    output_dir: Path,
    *,
    image_format: str = "png",
    jpeg_quality: int = 95,
    limit: int | None = None,
    overwrite: bool = False,
) -> ExtractStats:
    input_dir = Path(input_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not input_dir.is_dir():
        raise FileNotFoundError(f"input directory not found: {input_dir}")

    image_format = image_format.lower().lstrip(".")
    if image_format not in {"jpg", "jpeg", "png"}:
        raise ValueError("image format must be jpg, jpeg, or png")

    stats = ExtractStats()
    frame_index = 0
    for chunk_path in _chunk_paths(input_dir):
        stats.chunks_seen += 1
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

                payload = stream.read(raw_size)
                if len(payload) != raw_size:
                    raise ValueError(f"{chunk_path}: incomplete raw payload")

                stats.records_seen += 1
                if raw_size == 0:
                    stats.skipped_empty += 1
                    continue

                header = parse_raw_image_header(header_bytes)
                image = _image_from_payload(header, payload)
                output_path = output_dir / _output_name(
                    header, frame_index, image_format
                )
                if output_path.exists() and not overwrite:
                    output_path = output_dir / (
                        f"{output_path.stem}_{frame_index:06d}{output_path.suffix}"
                    )

                params: list[int] = []
                if image_format in {"jpg", "jpeg"}:
                    params = [int(cv2.IMWRITE_JPEG_QUALITY), int(jpeg_quality)]
                if not cv2.imwrite(str(output_path), image, params):
                    raise RuntimeError(f"failed to write image: {output_path}")

                frame_index += 1
                stats.frames_written += 1
                if limit is not None and stats.frames_written >= limit:
                    return stats

    return stats


def extract_dataset(
    input_dir: Path,
    output_dir: Path,
    *,
    image_format: str = "png",
    jpeg_quality: int = 95,
    limit: int | None = None,
    overwrite: bool = False,
) -> ExtractStats:
    """Extract a camera chunk dir, or a whole dataset root (cam_left + cam_right).

    Per-camera output goes to ``output_dir/<cam>/`` when a root is given; a
    single camera dir extracts directly into ``output_dir``.
    """
    input_dir = Path(input_dir)
    if not input_dir.is_dir():
        raise FileNotFoundError(f"input directory not found: {input_dir}")

    cam_dirs = _resolve_camera_dirs(input_dir)
    if not cam_dirs:
        raise FileNotFoundError(
            f"no image chunks (*_chunk.dat) found in {input_dir} or its "
            "cam_left/ or cam_right/ subdirectories"
        )

    total = ExtractStats()
    for chunk_dir, label in cam_dirs:
        out = Path(output_dir) / label if label else Path(output_dir)
        st = extract_photos(
            chunk_dir,
            out,
            image_format=image_format,
            jpeg_quality=jpeg_quality,
            limit=limit,
            overwrite=overwrite,
        )
        for field in ("chunks_seen", "records_seen", "frames_written", "skipped_empty"):
            setattr(total, field, getattr(total, field) + getattr(st, field))
        print(
            f"  [{label or chunk_dir.name}] {st.frames_written} images "
            f"from {st.records_seen} records -> {out}"
        )
    return total


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract recorded camera image chunks to PNG/JPEG. Accepts a "
        "dataset root (extracts cam_left + cam_right) or a single camera chunk dir."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=DEFAULT_INPUT_DIR,
        help=f"dataset root or a camera chunk dir (default: {DEFAULT_INPUT_DIR})",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help=f"photo output directory (default: {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--format",
        choices=["jpg", "jpeg", "png"],
        default="png",
        help="output image format (default: png, lossless; use jpg for smaller files)",
    )
    parser.add_argument(
        "--jpeg-quality",
        type=int,
        default=95,
        help="JPEG quality when --format is jpg/jpeg (default: 95)",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=None,
        help="extract at most this many frames",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="overwrite existing output files with the same name",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    stats = extract_dataset(
        args.input_dir,
        args.output_dir,
        image_format=args.format,
        jpeg_quality=args.jpeg_quality,
        limit=args.limit,
        overwrite=args.overwrite,
    )
    print(
        "extracted "
        f"{stats.frames_written} images from {stats.records_seen} records "
        f"across {stats.chunks_seen} chunks to {args.output_dir}"
    )
    if stats.skipped_empty:
        print(f"skipped {stats.skipped_empty} records with empty raw payload")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
