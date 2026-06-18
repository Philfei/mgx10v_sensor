#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/_build/aarch64-linux-gnu"
INSTALL_PREFIX="${ROOT_DIR}/install/aarch64-linux-gnu"
SYSROOT_ENV="${REPO_ROOT}/toolchain/mgx10v_sysroot/env.sh"
TOOLCHAIN_FILE="${REPO_ROOT}/toolchain/mgx10v_sysroot/cmake/aarch64-linux-gnu.cmake"

# shellcheck source=/dev/null
source "${ROOT_DIR}/versions.env"

ZEROMQ_SRC="${ROOT_DIR}/src/zeromq-${ZEROMQ_VERSION}"
CPPZMQ_SRC="${ROOT_DIR}/src/cppzmq-${CPPZMQ_VERSION}"
PROTOBUF_SRC="${ROOT_DIR}/src/protobuf-${PROTOBUF_VERSION}"

for path in "${ZEROMQ_SRC}" "${CPPZMQ_SRC}" "${PROTOBUF_SRC}"; do
  if [[ ! -d "${path}" ]]; then
    echo "error: ${path} not found" >&2
    echo "run: ${ROOT_DIR}/scripts/fetch_sources.sh" >&2
    exit 1
  fi
done

if [[ -f "${SYSROOT_ENV}" ]]; then
  # shellcheck source=/dev/null
  source "${SYSROOT_ENV}"
fi

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "error: toolchain file not found: ${TOOLCHAIN_FILE}" >&2
  exit 1
fi

COMMON_CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}"
  -DCMAKE_SKIP_RPATH=ON
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
)

cmake -S "${ZEROMQ_SRC}" -B "${BUILD_ROOT}/zeromq" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DBUILD_SHARED=ON \
  -DBUILD_STATIC=ON \
  -DZMQ_BUILD_TESTS=OFF \
  -DBUILD_TESTS=OFF \
  -DWITH_DOCS=OFF \
  -DWITH_PERF_TOOL=OFF \
  -DWITH_LIBSODIUM=OFF \
  -DENABLE_CURVE=OFF \
  -DENABLE_DRAFTS=OFF

cmake --build "${BUILD_ROOT}/zeromq" -j"$(nproc)"
cmake --install "${BUILD_ROOT}/zeromq"

PKG_CONFIG_PATH="${INSTALL_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
cmake -S "${CPPZMQ_SRC}" -B "${BUILD_ROOT}/cppzmq" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DCMAKE_PREFIX_PATH="${INSTALL_PREFIX}" \
  -DZeroMQ_DIR="${INSTALL_PREFIX}/lib/cmake/ZeroMQ" \
  -DCPPZMQ_BUILD_TESTS=OFF

cmake --build "${BUILD_ROOT}/cppzmq" -j"$(nproc)"
cmake --install "${BUILD_ROOT}/cppzmq"

if [[ -f "${PROTOBUF_SRC}/cmake/CMakeLists.txt" ]]; then
  PROTOBUF_CMAKE_SRC="${PROTOBUF_SRC}/cmake"
else
  PROTOBUF_CMAKE_SRC="${PROTOBUF_SRC}"
fi

cmake -S "${PROTOBUF_CMAKE_SRC}" -B "${BUILD_ROOT}/protobuf" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Dprotobuf_BUILD_TESTS=OFF \
  -Dprotobuf_BUILD_SHARED_LIBS=ON \
  -Dprotobuf_BUILD_LIBPROTOC=OFF \
  -Dprotobuf_BUILD_PROTOC_BINARIES=OFF

cmake --build "${BUILD_ROOT}/protobuf" -j"$(nproc)"
cmake --install "${BUILD_ROOT}/protobuf"

echo
echo "MGX10V thirdparty install prefix:"
echo "  ${INSTALL_PREFIX}"
echo
echo "Use with CMake:"
echo "  -DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
