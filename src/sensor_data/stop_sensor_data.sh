#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_DIR="${SENSOR_DATA_PID_DIR:-${SCRIPT_DIR}/run}"
STOP_VENDOR_PWM="${STOP_VENDOR_PWM:-1}"
STOP_TIMEOUT_SEC="${STOP_TIMEOUT_SEC:-3}"

stop_pid() {
  local name="$1"
  local pid="$2"

  if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
    return 0
  fi

  echo "stopping ${name}: pid=${pid}"
  kill "${pid}" 2>/dev/null || true

  local waited=0
  while kill -0 "${pid}" 2>/dev/null; do
    if (( waited >= STOP_TIMEOUT_SEC )); then
      echo "force stopping ${name}: pid=${pid}"
      kill -KILL "${pid}" 2>/dev/null || true
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
}

stop_one() {
  local name="$1"
  local pid_file="${PID_DIR}/${name}.pid"

  if [[ -f "${pid_file}" ]]; then
    local pid
    pid="$(cat "${pid_file}" 2>/dev/null || true)"
    stop_pid "${name}" "${pid}"
    rm -f "${pid_file}"
  fi

  if command -v pidof >/dev/null 2>&1; then
    local pids
    pids="$(pidof "${name}" 2>/dev/null || true)"
    if [[ -n "${pids}" ]]; then
      local pid
      for pid in ${pids}; do
        stop_pid "${name}" "${pid}"
      done
    fi
  fi
}

stop_one cam_sensor_data
stop_one imu_sensor_data
stop_one gnss_sensor_data

if [[ "${STOP_VENDOR_PWM}" == "1" && -x "${SCRIPT_DIR}/stop_vendor_pwm.sh" ]]; then
  echo "stopping vendor pwm"
  (cd "${SCRIPT_DIR}" && ./stop_vendor_pwm.sh)
elif [[ "${STOP_VENDOR_PWM}" == "1" ]]; then
  echo "stop_vendor_pwm.sh not found, skip vendor pwm stop" >&2
fi

echo "sensor_data programs stopped"
