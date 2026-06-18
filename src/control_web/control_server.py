#!/usr/bin/env python3
"""Small device-side web control server for sensor tools."""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import shutil
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse


SENSOR_NAMES = ("cam_left", "cam_right", "imu", "gnss")


class BadRequest(Exception):
    pass


class NotFound(Exception):
    pass


@dataclass(frozen=True)
class ControlConfig:
    data_root: Path = Path("/userdata/app/sensor_recorder/data")
    sensor_data_dir: Path = Path("/userdata/app/sensor_data")
    ic_gvins_dir: Path = Path("/userdata/app/ic_gvins")
    recorder_dir: Path = Path("/userdata/app/sensor_recorder")
    sanity_dir: Path = Path("/userdata/app/sensor_sanity_check")
    log_dir: Path = Path("/userdata/app/control_web/logs")
    static_dir: Path = Path(__file__).resolve().parent / "static"
    sanity_timeout_s: int = 120
    stop_timeout_s: float = 5.0


class ControlService:
    def __init__(self, config: ControlConfig):
        self.config = config
        self._children: dict[str, subprocess.Popen[Any]] = {}
        self._child_lock = threading.Lock()

    def status(self) -> dict[str, Any]:
        return {
            "processes": {
                "sensor_data": self._group_status(
                    {
                        "cam_sensor_data": "cam_sensor_data",
                        "imu_sensor_data": "imu_sensor_data",
                        "gnss_sensor_data": "gnss_sensor_data",
                    }
                ),
                "ic_gvins_zmq": self._process_status("ic_gvins_zmq"),
                "sensor_recorder": self._process_status_by_pid_file(
                    "sensor_recorder",
                    str(self.config.recorder_dir / "sensor_recorder"),
                ),
            },
            "disk": self._disk_status(self.config.data_root),
            "paths": {
                "data_root": str(self.config.data_root),
                "log_dir": str(self.config.log_dir),
            },
        }

    def list_datasets(self) -> list[dict[str, Any]]:
        if not self.config.data_root.exists():
            return []
        rows = []
        for entry in self.config.data_root.iterdir():
            if not entry.is_dir() or not entry.name.startswith("data_"):
                continue
            stat = entry.stat()
            rows.append(
                {
                    "name": entry.name,
                    "path": str(entry),
                    "mtime": int(stat.st_mtime),
                    "mtime_text": time.strftime(
                        "%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)
                    ),
                    "size_bytes": self._directory_size(entry),
                    "sensors": {
                        name: (entry / name).is_dir() for name in SENSOR_NAMES
                    },
                }
            )
        rows.sort(key=lambda row: row["mtime"], reverse=True)
        return rows

    def run_action(self, action: str) -> dict[str, Any]:
        actions = {
            "start_sensor_data": self._start_sensor_data,
            "stop_sensor_data": self._stop_sensor_data,
            "start_ic_gvins_zmq": self._start_ic_gvins_zmq,
            "stop_ic_gvins_zmq": self._stop_ic_gvins_zmq,
            "start_sensor_recorder": self._start_sensor_recorder,
            "stop_sensor_recorder": self._stop_sensor_recorder,
        }
        handler = actions.get(action)
        if handler is None:
            raise BadRequest(f"unknown action: {action}")
        result = handler()
        result["action"] = action
        return result

    def delete_dataset(self, name: str) -> dict[str, Any]:
        path = self._dataset_path(name, must_exist=True)
        shutil.rmtree(path)
        return {"deleted": path.name}

    def sanity_check(self, name: str) -> dict[str, Any]:
        dataset = self._dataset_path(name, must_exist=True)
        exe = self.config.sanity_dir / "sensor_sanity_check"
        if not exe.exists():
            raise NotFound(f"sensor_sanity_check not found: {exe}")
        proc = subprocess.run(
            [str(exe), "--dir", str(dataset)],
            cwd=str(self.config.sanity_dir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=self.config.sanity_timeout_s,
            check=False,
        )
        return {
            "dataset": dataset.name,
            "ok": proc.returncode == 0,
            "returncode": proc.returncode,
            "output": proc.stdout,
        }

    def read_log_tail(self, name: str, max_bytes: int = 20000) -> dict[str, Any]:
        allowed = {
            "control": self.config.log_dir / "control_web.log",
            "sensor_data": self.config.sensor_data_dir / "logs" / "cam_sensor_data.log",
            "imu": self.config.sensor_data_dir / "logs" / "imu_sensor_data.log",
            "gnss": self.config.sensor_data_dir / "logs" / "gnss_sensor_data.log",
            "recorder": self.config.log_dir / "sensor_recorder.log",
            "ic_gvins": self.config.log_dir / "ic_gvins_zmq.log",
        }
        path = allowed.get(name)
        if path is None:
            raise BadRequest(f"unknown log: {name}")
        if not path.exists():
            return {"name": name, "path": str(path), "text": ""}
        with path.open("rb") as handle:
            if path.stat().st_size > max_bytes:
                handle.seek(-max_bytes, os.SEEK_END)
            text = handle.read().decode("utf-8", errors="replace")
        return {"name": name, "path": str(path), "text": text}

    def _start_sensor_data(self) -> dict[str, Any]:
        return self._start_process(
            [str(self.config.sensor_data_dir / "start_sensor_data.sh")],
            cwd=self.config.sensor_data_dir,
            log_name="start_sensor_data.log",
        )

    def _stop_sensor_data(self) -> dict[str, Any]:
        return self._run_sync(
            [str(self.config.sensor_data_dir / "stop_sensor_data.sh")],
            cwd=self.config.sensor_data_dir,
        )

    def _start_ic_gvins_zmq(self) -> dict[str, Any]:
        return self._start_process(
            [str(self.config.ic_gvins_dir / "run_zmq.sh")],
            cwd=self.config.ic_gvins_dir,
            log_name="ic_gvins_zmq.log",
        )

    def _stop_ic_gvins_zmq(self) -> dict[str, Any]:
        return self._stop_by_pattern("ic_gvins_zmq")

    def _start_sensor_recorder(self) -> dict[str, Any]:
        return self._start_process(
            [
                str(self.config.recorder_dir / "sensor_recorder"),
                "--config",
                str(self.config.recorder_dir / "config.yaml"),
            ],
            cwd=self.config.recorder_dir,
            log_name="sensor_recorder.log",
            process_name="sensor_recorder",
        )

    def _stop_sensor_recorder(self) -> dict[str, Any]:
        return self._stop_by_pid_file(
            "sensor_recorder",
            str(self.config.recorder_dir / "sensor_recorder"),
        )

    def _start_process(
        self,
        argv: list[str],
        cwd: Path,
        log_name: str,
        process_name: str | None = None,
    ) -> dict[str, Any]:
        if not Path(argv[0]).exists():
            raise NotFound(f"executable not found: {argv[0]}")
        self.config.log_dir.mkdir(parents=True, exist_ok=True)
        log_path = self.config.log_dir / log_name
        env = os.environ.copy()
        existing_ld = env.get("LD_LIBRARY_PATH", "")
        lib_paths = [
            str(cwd),
            str(cwd / "lib"),
            str(cwd / "lib" / "rockchip"),
        ]
        env["LD_LIBRARY_PATH"] = ":".join(
            [p for p in lib_paths + [existing_ld] if p]
        )
        log = log_path.open("ab")
        proc = subprocess.Popen(
            argv,
            cwd=str(cwd),
            stdout=log,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
        if process_name:
            self._remember_child(process_name, proc)
            self._write_pid_file(process_name, proc.pid)
        return {"started": True, "pid": proc.pid, "log": str(log_path)}

    def _run_sync(self, argv: list[str], cwd: Path) -> dict[str, Any]:
        if not Path(argv[0]).exists():
            raise NotFound(f"executable not found: {argv[0]}")
        proc = subprocess.run(
            argv,
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        return {
            "ok": proc.returncode == 0,
            "returncode": proc.returncode,
            "output": proc.stdout,
        }

    def _stop_by_pattern(self, pattern: str) -> dict[str, Any]:
        pids = self._pgrep(pattern)
        return self._stop_pids(pids)

    def _stop_by_pid_file(self, name: str, fallback_pattern: str) -> dict[str, Any]:
        pids = []
        pid = self._read_pid_file(name)
        if pid is not None and self._pid_running(pid):
            pids.append(pid)
        if not pids:
            pids.extend(self._pgrep(fallback_pattern))
        result = self._stop_pids(pids)
        self._reap_child(name)
        if not result["remaining_pids"]:
            self._remove_pid_file(name)
        return result

    def _stop_pids(self, pids: list[int]) -> dict[str, Any]:
        stopped = []
        seen = set()
        for pid in pids:
            if pid in seen:
                continue
            seen.add(pid)
            if self._send_signal(pid, signal.SIGTERM):
                stopped.append(pid)

        remaining = self._wait_for_exit(stopped, self.config.stop_timeout_s)
        killed = []
        for pid in remaining:
            if self._send_signal(pid, signal.SIGKILL):
                killed.append(pid)
        remaining = self._wait_for_exit(remaining, 1.0)
        return {
            "ok": not remaining,
            "stopped_pids": stopped,
            "force_killed_pids": killed,
            "remaining_pids": remaining,
        }

    def _dataset_path(self, name: str, must_exist: bool) -> Path:
        if "/" in name or "\\" in name or not name.startswith("data_"):
            raise BadRequest("dataset name must be a data_* directory name")
        root = self.config.data_root.resolve()
        path = (self.config.data_root / name).resolve()
        if path.parent != root:
            raise BadRequest("dataset path escapes data root")
        if must_exist and not path.is_dir():
            raise NotFound(f"dataset not found: {name}")
        return path

    def _group_status(self, names: dict[str, str]) -> dict[str, Any]:
        children = {key: self._process_status(pattern) for key, pattern in names.items()}
        return {
            "running": all(child["running"] for child in children.values()),
            "children": children,
        }

    def _process_status(self, pattern: str) -> dict[str, Any]:
        pids = self._pgrep(pattern)
        return {"running": bool(pids), "pids": pids}

    def _pgrep(self, pattern: str) -> list[int]:
        proc = subprocess.run(
            ["pgrep", "-f", pattern],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        pids = []
        for line in proc.stdout.splitlines():
            try:
                pid = int(line.strip())
            except ValueError:
                continue
            if pid != os.getpid() and self._pid_running(pid):
                pids.append(pid)
        return pids

    def _send_signal(self, pid: int, sig: signal.Signals) -> bool:
        try:
            pgid = os.getpgid(pid)
            if pgid == pid:
                os.killpg(pgid, sig)
            else:
                os.kill(pid, sig)
            return True
        except ProcessLookupError:
            return False

    def _pid_running(self, pid: int) -> bool:
        state = self._proc_state(pid)
        if state == "Z":
            return False
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False

    def _wait_for_exit(self, pids: list[int], timeout_s: float) -> list[int]:
        deadline = time.monotonic() + max(0.0, timeout_s)
        remaining = set(pids)
        while remaining and time.monotonic() < deadline:
            remaining = {pid for pid in remaining if self._pid_running(pid)}
            if remaining:
                time.sleep(0.1)
        return sorted(remaining)

    @staticmethod
    def _proc_state(pid: int) -> str | None:
        try:
            text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        except FileNotFoundError:
            return None
        except OSError:
            return None
        parts = text.split()
        return parts[2] if len(parts) > 2 else None

    def _pid_file(self, name: str) -> Path:
        return self.config.log_dir / f"{name}.pid"

    def _write_pid_file(self, name: str, pid: int) -> None:
        self.config.log_dir.mkdir(parents=True, exist_ok=True)
        self._pid_file(name).write_text(f"{pid}\n", encoding="utf-8")

    def _read_pid_file(self, name: str) -> int | None:
        try:
            text = self._pid_file(name).read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            return None
        try:
            return int(text)
        except ValueError:
            return None

    def _remove_pid_file(self, name: str) -> None:
        try:
            self._pid_file(name).unlink()
        except FileNotFoundError:
            pass

    def _remember_child(self, name: str, proc: subprocess.Popen[Any]) -> None:
        with self._child_lock:
            old = self._children.get(name)
            if old is not None:
                old.poll()
            self._children[name] = proc

    def _reap_child(self, name: str) -> None:
        with self._child_lock:
            proc = self._children.get(name)
            if proc is not None and proc.poll() is not None:
                self._children.pop(name, None)

    def _process_status_by_pid_file(
        self, name: str, fallback_pattern: str
    ) -> dict[str, Any]:
        self._reap_child(name)
        pid = self._read_pid_file(name)
        if pid is not None:
            if self._pid_running(pid):
                return {"running": True, "pids": [pid]}
            self._remove_pid_file(name)
        return self._process_status(fallback_pattern)

    @staticmethod
    def _directory_size(path: Path) -> int:
        total = 0
        for entry in path.rglob("*"):
            if entry.is_file():
                try:
                    total += entry.stat().st_size
                except FileNotFoundError:
                    continue
        return total

    @staticmethod
    def _disk_status(path: Path) -> dict[str, int]:
        target = path if path.exists() else path.parent
        usage = shutil.disk_usage(target)
        return {"total": usage.total, "used": usage.used, "free": usage.free}


class ControlHandler(BaseHTTPRequestHandler):
    service: ControlService

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/status":
            self._send_json(self.service.status())
            return
        if parsed.path == "/api/datasets":
            self._send_json({"datasets": self.service.list_datasets()})
            return
        if parsed.path.startswith("/api/logs/"):
            name = unquote(parsed.path.removeprefix("/api/logs/"))
            self._handle_json_call(lambda: self.service.read_log_tail(name))
            return
        self._serve_static(parsed.path)

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/actions":
            payload = self._read_json()
            self._handle_json_call(
                lambda: self.service.run_action(str(payload.get("action", "")))
            )
            return
        if parsed.path.startswith("/api/datasets/") and parsed.path.endswith(
            "/sanity"
        ):
            name = unquote(
                parsed.path.removeprefix("/api/datasets/").removesuffix("/sanity")
            ).strip("/")
            self._handle_json_call(lambda: self.service.sanity_check(name))
            return
        self._send_error(HTTPStatus.NOT_FOUND, "not found")

    def do_DELETE(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path.startswith("/api/datasets/"):
            name = unquote(parsed.path.removeprefix("/api/datasets/")).strip("/")
            self._handle_json_call(lambda: self.service.delete_dataset(name))
            return
        self._send_error(HTTPStatus.NOT_FOUND, "not found")

    def log_message(self, fmt: str, *args: Any) -> None:
        self.service.config.log_dir.mkdir(parents=True, exist_ok=True)
        with (self.service.config.log_dir / "control_web.log").open(
            "a", encoding="utf-8"
        ) as handle:
            handle.write("[%s] " % time.strftime("%Y-%m-%d %H:%M:%S"))
            handle.write(fmt % args)
            handle.write("\n")

    def _serve_static(self, path: str) -> None:
        if path == "/":
            path = "/index.html"
        relative = Path(path.lstrip("/"))
        static_root = self.service.config.static_dir.resolve()
        target = (static_root / relative).resolve()
        if static_root not in target.parents and target != static_root:
            self._send_error(HTTPStatus.BAD_REQUEST, "invalid static path")
            return
        if not target.is_file():
            self._send_error(HTTPStatus.NOT_FOUND, "not found")
            return
        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        data = target.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        data = self.rfile.read(length)
        try:
            payload = json.loads(data.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise BadRequest(f"invalid JSON: {exc}") from exc
        if not isinstance(payload, dict):
            raise BadRequest("JSON body must be an object")
        return payload

    def _handle_json_call(self, func: Any) -> None:
        try:
            result = func()
            self._send_json(result)
        except BadRequest as exc:
            self._send_error(HTTPStatus.BAD_REQUEST, str(exc))
        except NotFound as exc:
            self._send_error(HTTPStatus.NOT_FOUND, str(exc))
        except subprocess.TimeoutExpired as exc:
            self._send_error(HTTPStatus.GATEWAY_TIMEOUT, f"command timed out: {exc}")
        except Exception as exc:  # pragma: no cover - final HTTP safety net
            self._send_error(HTTPStatus.INTERNAL_SERVER_ERROR, str(exc))

    def _send_json(self, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_error(self, status: HTTPStatus, message: str) -> None:
        data = json.dumps({"error": message}, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Sensor control web server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--data-root", type=Path, default=ControlConfig.data_root)
    parser.add_argument(
        "--sensor-data-dir", type=Path, default=ControlConfig.sensor_data_dir
    )
    parser.add_argument("--ic-gvins-dir", type=Path, default=ControlConfig.ic_gvins_dir)
    parser.add_argument("--recorder-dir", type=Path, default=ControlConfig.recorder_dir)
    parser.add_argument("--sanity-dir", type=Path, default=ControlConfig.sanity_dir)
    parser.add_argument("--log-dir", type=Path, default=ControlConfig.log_dir)
    parser.add_argument("--stop-timeout-s", type=float, default=ControlConfig.stop_timeout_s)
    parser.add_argument(
        "--static-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "static",
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    config = ControlConfig(
        data_root=args.data_root,
        sensor_data_dir=args.sensor_data_dir,
        ic_gvins_dir=args.ic_gvins_dir,
        recorder_dir=args.recorder_dir,
        sanity_dir=args.sanity_dir,
        log_dir=args.log_dir,
        static_dir=args.static_dir,
        stop_timeout_s=args.stop_timeout_s,
    )
    config.log_dir.mkdir(parents=True, exist_ok=True)
    service = ControlService(config)

    class Handler(ControlHandler):
        pass

    Handler.service = service
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"control web listening on http://{args.host}:{args.port}")
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
