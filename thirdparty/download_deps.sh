#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${ROOT_DIR}/src"
ARCHIVE_DIR="${SRC_DIR}/_archives"
VERSIONS_FILE="${ROOT_DIR}/versions.env"

# shellcheck source=/dev/null
source "${VERSIONS_FILE}"

WRITE_VERSIONS=1
ZEROMQ_VERSION_ARG="${ZEROMQ_VERSION}"
CPPZMQ_VERSION_ARG="${CPPZMQ_VERSION}"
PROTOBUF_VERSION_ARG="${PROTOBUF_VERSION}"
PROTOBUF_RELEASE_TAG_ARG="${PROTOBUF_RELEASE_TAG}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [opts]

Download and extract receiver_test thirdparty source archives.

Options:
  --zeromq VERSION        ZeroMQ version, default ${ZEROMQ_VERSION}
  --cppzmq VERSION        cppzmq version, default ${CPPZMQ_VERSION}
  --protobuf VERSION      protobuf C++ version, default ${PROTOBUF_VERSION}
  --protobuf-tag TAG      protobuf GitHub release tag, default ${PROTOBUF_RELEASE_TAG}
  --no-write-versions     do not update versions.env
  -h, --help              show this help

Example:
  ./receiver_test/thirdparty/download_deps.sh \\
    --zeromq 4.3.5 --cppzmq 4.10.0 --protobuf 3.21.12 --protobuf-tag v21.12
EOF
}

derive_protobuf_tag() {
  local version="$1"
  case "${version}" in
    3.21.*)
      echo "v${version#3.}"
      ;;
    *)
      echo "v${version}"
      ;;
  esac
}

protobuf_version_changed=0
protobuf_tag_explicit=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --zeromq)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        exit 2
      fi
      ZEROMQ_VERSION_ARG="$2"
      shift 2
      ;;
    --cppzmq)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        exit 2
      fi
      CPPZMQ_VERSION_ARG="$2"
      shift 2
      ;;
    --protobuf)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        exit 2
      fi
      PROTOBUF_VERSION_ARG="$2"
      protobuf_version_changed=1
      shift 2
      ;;
    --protobuf-tag)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        exit 2
      fi
      PROTOBUF_RELEASE_TAG_ARG="$2"
      protobuf_tag_explicit=1
      shift 2
      ;;
    --no-write-versions)
      WRITE_VERSIONS=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${protobuf_version_changed}" == "1" && "${protobuf_tag_explicit}" == "0" ]]; then
  PROTOBUF_RELEASE_TAG_ARG="$(derive_protobuf_tag "${PROTOBUF_VERSION_ARG}")"
fi

if [[ "${WRITE_VERSIONS}" == "1" ]]; then
  cat >"${VERSIONS_FILE}" <<EOF
# Dependency versions used by receiver_test thirdparty source builds.
#
# ZeroMQ 4.3.x is stable and has a small dependency surface when CURVE/libsodium
# support is disabled.
ZEROMQ_VERSION=${ZEROMQ_VERSION_ARG}

# cppzmq is header-only and provides zmq.hpp / zmq_addon.hpp.
CPPZMQ_VERSION=${CPPZMQ_VERSION_ARG}

# protobuf 3.21.x keeps the C++ runtime simple compared with newer releases
# that pull in abseil as an additional dependency.
PROTOBUF_VERSION=${PROTOBUF_VERSION_ARG}
PROTOBUF_RELEASE_TAG=${PROTOBUF_RELEASE_TAG_ARG}
EOF
fi

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

ZEROMQ_ARCHIVE="${ARCHIVE_DIR}/zeromq-${ZEROMQ_VERSION_ARG}.tar.gz"
CPPZMQ_ARCHIVE="${ARCHIVE_DIR}/cppzmq-${CPPZMQ_VERSION_ARG}.tar.gz"
PROTOBUF_ARCHIVE="${ARCHIVE_DIR}/protobuf-cpp-${PROTOBUF_VERSION_ARG}.tar.gz"

download "https://github.com/zeromq/libzmq/releases/download/v${ZEROMQ_VERSION_ARG}/zeromq-${ZEROMQ_VERSION_ARG}.tar.gz" "${ZEROMQ_ARCHIVE}"
download "https://github.com/zeromq/cppzmq/archive/refs/tags/v${CPPZMQ_VERSION_ARG}.tar.gz" "${CPPZMQ_ARCHIVE}"
download "https://github.com/protocolbuffers/protobuf/releases/download/${PROTOBUF_RELEASE_TAG_ARG}/protobuf-cpp-${PROTOBUF_VERSION_ARG}.tar.gz" "${PROTOBUF_ARCHIVE}"

extract "${ZEROMQ_ARCHIVE}" "${SRC_DIR}/zeromq-${ZEROMQ_VERSION_ARG}"
extract "${CPPZMQ_ARCHIVE}" "${SRC_DIR}/cppzmq-${CPPZMQ_VERSION_ARG}"
extract "${PROTOBUF_ARCHIVE}" "${SRC_DIR}/protobuf-${PROTOBUF_VERSION_ARG}"

echo
echo "Thirdparty sources are ready:"
echo "  ${SRC_DIR}/zeromq-${ZEROMQ_VERSION_ARG}"
echo "  ${SRC_DIR}/cppzmq-${CPPZMQ_VERSION_ARG}"
echo "  ${SRC_DIR}/protobuf-${PROTOBUF_VERSION_ARG}"
echo "Version file: ${VERSIONS_FILE}"
