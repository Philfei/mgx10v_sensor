#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${HOST:-0.0.0.0}"
PORT="${PORT:-8080}"
LOG_DIR="${CONTROL_WEB_LOG_DIR:-/userdata/app/control_web/logs}"
PID_DIR="${CONTROL_WEB_PID_DIR:-/userdata/app/control_web/run}"

mkdir -p "${LOG_DIR}" "${PID_DIR}"

PID_FILE="${PID_DIR}/control_web.pid"
LOG_FILE="${LOG_DIR}/control_web.log"

if [[ -f "${PID_FILE}" ]]; then
  old_pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
  if [[ -n "${old_pid}" ]] && kill -0 "${old_pid}" 2>/dev/null; then
    echo "control_web already running: pid=${old_pid}"
    exit 0
  fi
  rm -f "${PID_FILE}"
fi

nohup python3 "${SCRIPT_DIR}/control_server.py" \
  --host "${HOST}" \
  --port "${PORT}" \
  --log-dir "${LOG_DIR}" \
  >"${LOG_FILE}" 2>&1 &

pid="$!"
echo "${pid}" > "${PID_FILE}"
echo "started control_web: pid=${pid} url=http://${HOST}:${PORT} log=${LOG_FILE}"
