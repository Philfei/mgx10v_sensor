#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENSORS_CODE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${SENSORS_CODE_DIR}/build/sensor_recorder}"
DEPLOY_DIR=""
DO_DEPLOY=1

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
    --deploy-dir)
      if [[ $# -lt 2 ]]; then
        echo "missing value for --deploy-dir" >&2
        exit 1
      fi
      DEPLOY_DIR="$2"
      shift 2
      ;;
    --no-deploy)
      DO_DEPLOY=0
      shift
      ;;
    *)
      cmake_args+=("$1")
      shift
      ;;
  esac
done

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

if [[ "${DO_DEPLOY}" == "1" ]]; then
  if [[ -z "${DEPLOY_DIR}" ]]; then
    DEPLOY_DIR="${BUILD_DIR}/deploy"
  fi
  cmake -E rm -rf "${DEPLOY_DIR}"
  cmake --install "${BUILD_DIR}" --prefix "${DEPLOY_DIR}"
fi

echo "Build output: ${BUILD_DIR}"
echo "  ${BUILD_DIR}/sensor_recorder"
echo "  ${BUILD_DIR}/config.yaml"
echo "  ${BUILD_DIR}/lib"
if [[ "${DO_DEPLOY}" == "1" ]]; then
  echo "Deploy output: ${DEPLOY_DIR}"
  echo "  Copy ${DEPLOY_DIR}/* to the device runtime directory."
fi
