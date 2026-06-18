#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CAM_SYNC_KO="${CAM_SYNC_KO:-/userdata/ko/cam_sync.ko}"
FREQ="${FREQ:-20}"

if [[ ! -f "${CAM_SYNC_KO}" ]]; then
  echo "cam_sync kernel module not found: ${CAM_SYNC_KO}" >&2
  exit 1
fi

if [[ -x "${SCRIPT_DIR}/stop_vendor_pwm.sh" ]]; then
  "${SCRIPT_DIR}/stop_vendor_pwm.sh" >/dev/null 2>&1 || true
fi

echo "Loading cam_sync: ${CAM_SYNC_KO} freq=${FREQ}"
insmod "${CAM_SYNC_KO}" "freq=${FREQ}"
echo "cam_sync started"
