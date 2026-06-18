#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="${SCRIPT_DIR}/image_tcp_bridge"

if [[ ! -x "${APP}" ]]; then
  echo "image_tcp_bridge executable not found or not executable: ${APP}" >&2
  exit 1
fi

OUTPUT_PATH="${OUTPUT_PATH:-/root/sensor_receiver/snapshot}"
IMAGE_PUB="${IMAGE_PUB:-tcp://*:5560}"
CONTROL="${CONTROL:-tcp://*:5561}"
OUT_W="${OUT_W:-1280}"
OUT_H="${OUT_H:-720}"
JPEG_QUALITY="${JPEG_QUALITY:-80}"

exec "${APP}" \
  --output-path "${OUTPUT_PATH}" \
  --image-pub "${IMAGE_PUB}" \
  --control "${CONTROL}" \
  --out-w "${OUT_W}" \
  --out-h "${OUT_H}" \
  --jpeg-quality "${JPEG_QUALITY}" \
  "$@"
