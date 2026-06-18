#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="${SENSOR_DATA_LOG_DIR:-${SCRIPT_DIR}/logs}"
PID_DIR="${SENSOR_DATA_PID_DIR:-${SCRIPT_DIR}/run}"

mkdir -p "${LOG_DIR}" "${PID_DIR}"

export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${SCRIPT_DIR}/lib/rockchip:${LD_LIBRARY_PATH:-}"

START_VENDOR_PWM="${START_VENDOR_PWM:-1}"
PWM_FREQ="${PWM_FREQ:-20}"
CAM_ARGS="${CAM_ARGS:-}"
IMU_ARGS="${IMU_ARGS:-}"
GNSS_ARGS="${GNSS_ARGS:-}"
STARTUP_CHECK_DELAY="${STARTUP_CHECK_DELAY:-1}"

STARTED_NAMES=()
STARTED_PIDS=()
STARTED_LOGS=()

check_runtime_libs() {
  local missing=0
  local lib
  for lib in \
    "${SCRIPT_DIR}/lib/libprotobuf.so.32" \
    "${SCRIPT_DIR}/lib/libzmq.so.5"; do
    if [[ ! -e "${lib}" ]]; then
      echo "[ERROR] missing runtime library: ${lib}" >&2
      missing=1
    fi
  done

  if [[ "${START_CAM}" == "1" ]]; then
    for lib in \
      "${SCRIPT_DIR}/lib/librga.so.2" \
      "${SCRIPT_DIR}/lib/rockchip/librockit.so" \
      "${SCRIPT_DIR}/lib/rockchip/librkaiq.so" \
      "${SCRIPT_DIR}/lib/rockchip/librockchip_mpp.so"; do
      if [[ ! -e "${lib}" ]]; then
        echo "[ERROR] missing camera runtime library: ${lib}" >&2
        missing=1
      fi
    done
  fi

  if [[ "${missing}" != "0" ]]; then
    echo "[ERROR] deployment is incomplete. Copy the contents of the build deploy/ directory to ${SCRIPT_DIR}" >&2
    echo "[ERROR] expected layout: ${SCRIPT_DIR}/cam_sensor_data and ${SCRIPT_DIR}/lib/..." >&2
    return 1
  fi
}

usage() {
  cat <<EOF
Usage: $(basename "$0") [cam] [imu] [gnss]

Start selected sensor_data publishers. If no sensor is specified, all
publishers are started.

Examples:
  ./start_sensor_data.sh
  ./start_sensor_data.sh cam
  ./start_sensor_data.sh imu gnss
EOF
}

START_CAM=0
START_IMU=0
START_GNSS=0

if [[ $# -eq 0 ]]; then
  START_CAM=1
  START_IMU=1
  START_GNSS=1
else
  for sensor in "$@"; do
    case "${sensor}" in
      cam)
        START_CAM=1
        ;;
      imu)
        START_IMU=1
        ;;
      gnss)
        START_GNSS=1
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "unknown sensor: ${sensor}" >&2
        usage >&2
        exit 2
        ;;
    esac
  done
fi

check_runtime_libs

print_log_tail() {
  local log_file="$1"
  if [[ -f "${log_file}" ]]; then
    echo "---- ${log_file} tail ----" >&2
    tail -n 40 "${log_file}" >&2 || true
    echo "---- end ${log_file} ----" >&2
  else
    echo "log file not found: ${log_file}" >&2
  fi
}

start_one() {
  local name="$1"
  shift
  local exe="${SCRIPT_DIR}/${name}"
  local pid_file="${PID_DIR}/${name}.pid"
  local log_file="${LOG_DIR}/${name}.log"

  if [[ ! -x "${exe}" ]]; then
    echo "[ERROR] ${name} start failed: missing executable: ${exe}" >&2
    return 1
  fi

  if [[ -f "${pid_file}" ]]; then
    local old_pid
    old_pid="$(cat "${pid_file}" 2>/dev/null || true)"
    if [[ -n "${old_pid}" ]] && kill -0 "${old_pid}" 2>/dev/null; then
      echo "${name} already running: pid=${old_pid}"
      return 0
    fi
    rm -f "${pid_file}"
  fi

  nohup "${exe}" "$@" >"${log_file}" 2>&1 &
  local pid="$!"
  echo "${pid}" >"${pid_file}"
  echo "started ${name}: pid=${pid} log=${log_file}"
  STARTED_NAMES+=("${name}")
  STARTED_PIDS+=("${pid}")
  STARTED_LOGS+=("${log_file}")
}

check_started_programs() {
  local failed=0
  local count="${#STARTED_NAMES[@]}"
  if (( count == 0 )); then
    return 0
  fi

  sleep "${STARTUP_CHECK_DELAY}"

  local i
  for ((i = 0; i < count; ++i)); do
    local name="${STARTED_NAMES[$i]}"
    local pid="${STARTED_PIDS[$i]}"
    local log_file="${STARTED_LOGS[$i]}"
    local pid_file="${PID_DIR}/${name}.pid"

    if kill -0 "${pid}" 2>/dev/null; then
      echo "${name} startup check ok: pid=${pid}"
      continue
    fi

    local status="unknown"
    if wait "${pid}" 2>/dev/null; then
      status="0"
    else
      status="$?"
    fi
    echo "[ERROR] ${name} failed to stay running: pid=${pid} exit=${status} log=${log_file}" >&2
    print_log_tail "${log_file}"
    rm -f "${pid_file}"
    failed=1
  done

  return "${failed}"
}

if [[ "${START_CAM}" == "1" && "${START_VENDOR_PWM}" == "1" && -x "${SCRIPT_DIR}/start_vendor_pwm.sh" ]]; then
  echo "starting vendor pwm: FREQ=${PWM_FREQ}"
  if ! (cd "${SCRIPT_DIR}" && FREQ="${PWM_FREQ}" ./start_vendor_pwm.sh); then
    echo "[ERROR] start_vendor_pwm.sh failed" >&2
    exit 1
  fi
elif [[ "${START_CAM}" == "1" && "${START_VENDOR_PWM}" == "1" ]]; then
  echo "start_vendor_pwm.sh not found, skip vendor pwm start" >&2
fi

# Environment argument strings are intentionally simple whitespace-separated
# values for quick device-side use.
# shellcheck disable=SC2206
CAM_ARGV=(${CAM_ARGS})
# shellcheck disable=SC2206
IMU_ARGV=(${IMU_ARGS})
# shellcheck disable=SC2206
GNSS_ARGV=(${GNSS_ARGS})

if [[ "${START_CAM}" == "1" ]]; then
  start_one cam_sensor_data "${CAM_ARGV[@]}"
fi
if [[ "${START_IMU}" == "1" ]]; then
  start_one imu_sensor_data "${IMU_ARGV[@]}"
fi
if [[ "${START_GNSS}" == "1" ]]; then
  start_one gnss_sensor_data "${GNSS_ARGV[@]}"
fi

if ! check_started_programs; then
  echo "[ERROR] one or more sensor_data programs failed to start" >&2
  exit 1
fi

echo "sensor_data programs started"
