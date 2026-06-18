#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKIP_DOWNLOAD=0
download_args=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [opts]

Build receiver_test thirdparty dependencies:
  1. host protoc -> receiver_test/thirdparty/install/host
  2. aarch64 ZeroMQ/cppzmq/protobuf -> receiver_test/thirdparty/install/aarch64-linux-gnu

Options:
  --skip-download         do not run download_deps.sh first
  --zeromq VERSION        version forwarded to download_deps.sh
  --cppzmq VERSION        version forwarded to download_deps.sh
  --protobuf VERSION      version forwarded to download_deps.sh
  --protobuf-tag TAG      release tag forwarded to download_deps.sh
  -h, --help              show this help

Example:
  ./receiver_test/thirdparty/prebuild_deps.sh \\
    --zeromq 4.3.5 --cppzmq 4.10.0 --protobuf 3.21.12 --protobuf-tag v21.12
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-download)
      SKIP_DOWNLOAD=1
      shift
      ;;
    --zeromq|--cppzmq|--protobuf|--protobuf-tag)
      if [[ $# -lt 2 ]]; then
        echo "missing value for $1" >&2
        exit 2
      fi
      download_args+=("$1" "$2")
      shift 2
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

if [[ "${SKIP_DOWNLOAD}" == "0" ]]; then
  "${ROOT_DIR}/download_deps.sh" "${download_args[@]}"
elif [[ "${#download_args[@]}" -gt 0 ]]; then
  echo "error: version options require download_deps.sh; remove --skip-download" >&2
  exit 2
fi

"${ROOT_DIR}/scripts/build_host_protoc.sh"
"${ROOT_DIR}/scripts/build_for_mgx10v.sh"

echo
echo "Thirdparty prebuild complete:"
echo "  ${ROOT_DIR}/install/host/bin/protoc"
echo "  ${ROOT_DIR}/install/aarch64-linux-gnu"
