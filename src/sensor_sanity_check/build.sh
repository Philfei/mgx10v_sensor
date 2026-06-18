#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENSORS_CODE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${SENSORS_CODE_DIR}/build/sensor_sanity_check}"

cmake_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "missing value for --build-dir" >&2
        exit 1
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    *)
      cmake_args+=("$1")
      shift
      ;;
  esac
done

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "Build output: ${BUILD_DIR}"
echo "  ${BUILD_DIR}/sensor_sanity_check"
echo "  ${BUILD_DIR}/deploy"
