#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${ROOT_DIR}/src"
ARCHIVE_DIR="${SRC_DIR}/_archives"

# shellcheck source=/dev/null
source "${ROOT_DIR}/versions.env"

mkdir -p "${ARCHIVE_DIR}"

download() {
  local url="$1"
  local out="$2"
  if [[ -f "${out}" ]]; then
    echo "Using existing archive: ${out}"
    return
  fi

  echo "Downloading ${url}"
  if command -v curl >/dev/null 2>&1; then
    curl -L --fail --retry 3 -o "${out}" "${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "${out}" "${url}"
  else
    echo "error: curl or wget is required" >&2
    exit 1
  fi
}

extract() {
  local archive="$1"
  local dst="$2"
  if [[ -d "${dst}" ]]; then
    echo "Using existing source directory: ${dst}"
    return
  fi

  echo "Extracting ${archive}"
  tar -xf "${archive}" -C "${SRC_DIR}"
}

ZEROMQ_ARCHIVE="${ARCHIVE_DIR}/zeromq-${ZEROMQ_VERSION}.tar.gz"
CPPZMQ_ARCHIVE="${ARCHIVE_DIR}/cppzmq-${CPPZMQ_VERSION}.tar.gz"
PROTOBUF_ARCHIVE="${ARCHIVE_DIR}/protobuf-cpp-${PROTOBUF_VERSION}.tar.gz"

download "https://github.com/zeromq/libzmq/releases/download/v${ZEROMQ_VERSION}/zeromq-${ZEROMQ_VERSION}.tar.gz" "${ZEROMQ_ARCHIVE}"
download "https://github.com/zeromq/cppzmq/archive/refs/tags/v${CPPZMQ_VERSION}.tar.gz" "${CPPZMQ_ARCHIVE}"
download "https://github.com/protocolbuffers/protobuf/releases/download/${PROTOBUF_RELEASE_TAG}/protobuf-cpp-${PROTOBUF_VERSION}.tar.gz" "${PROTOBUF_ARCHIVE}"

extract "${ZEROMQ_ARCHIVE}" "${SRC_DIR}/zeromq-${ZEROMQ_VERSION}"
extract "${CPPZMQ_ARCHIVE}" "${SRC_DIR}/cppzmq-${CPPZMQ_VERSION}"
extract "${PROTOBUF_ARCHIVE}" "${SRC_DIR}/protobuf-${PROTOBUF_VERSION}"

echo
echo "Sources are ready under ${SRC_DIR}:"
echo "  zeromq-${ZEROMQ_VERSION}"
echo "  cppzmq-${CPPZMQ_VERSION}"
echo "  protobuf-${PROTOBUF_VERSION}"
