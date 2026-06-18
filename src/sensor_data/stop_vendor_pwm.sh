#!/usr/bin/env bash
set -euo pipefail

PWM_CHIP="${PWM_CHIP:-/sys/class/pwm/pwmchip1}"
PWM_INDEX="${PWM_INDEX:-0}"
PWM_DIR="${PWM_CHIP}/pwm${PWM_INDEX}"

if [[ -e "${PWM_DIR}/enable" ]]; then
  echo 0 >"${PWM_DIR}/enable" || true
fi

if [[ -e "${PWM_CHIP}/unexport" ]]; then
  echo "${PWM_INDEX}" >"${PWM_CHIP}/unexport" || true
fi

if lsmod 2>/dev/null | awk '{print $1}' | grep -qx cam_sync; then
  echo "Unloading cam_sync"
  rmmod cam_sync
else
  echo "cam_sync is not loaded"
fi

echo "cam_sync stopped"
