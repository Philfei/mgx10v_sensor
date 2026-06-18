#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/_build/host"
INSTALL_PREFIX="${ROOT_DIR}/install/host"

# shellcheck source=/dev/null
source "${ROOT_DIR}/versions.env"

PROTOBUF_SRC="${ROOT_DIR}/src/protobuf-${PROTOBUF_VERSION}"
if [[ ! -d "${PROTOBUF_SRC}" ]]; then
  echo "error: ${PROTOBUF_SRC} not found" >&2
  echo "run: ${ROOT_DIR}/scripts/fetch_sources.sh" >&2
  exit 1
fi

if [[ -f "${PROTOBUF_SRC}/cmake/CMakeLists.txt" ]]; then
  PROTOBUF_CMAKE_SRC="${PROTOBUF_SRC}/cmake"
else
  PROTOBUF_CMAKE_SRC="${PROTOBUF_SRC}"
fi

cmake -S "${PROTOBUF_CMAKE_SRC}" -B "${BUILD_ROOT}/protobuf" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=OFF \
  -Dprotobuf_BUILD_LIBPROTOC=ON \
  -Dprotobuf_BUILD_PROTOC_BINARIES=ON

cmake --build "${BUILD_ROOT}/protobuf" -j"$(nproc)"
cmake --install "${BUILD_ROOT}/protobuf"

echo "Host protoc: ${INSTALL_PREFIX}/bin/protoc"

