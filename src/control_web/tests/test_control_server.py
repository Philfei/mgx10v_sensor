import json
import signal
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import control_server


class ControlServiceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data_root = self.root / "record_data"
        self.data_root.mkdir()
        self.sensor_data_dir = self.root / "sensor_data"
        self.sensor_data_dir.mkdir()
        self.ic_gvins_dir = self.root / "ic_gvins"
        self.ic_gvins_dir.mkdir()
        self.recorder_dir = self.root / "sensor_recorder"
        self.recorder_dir.mkdir()
        self.sanity_dir = self.root / "sensor_sanity_check"
        self.sanity_dir.mkdir()
        self.log_dir = self.root / "logs"

        for path in [
            self.sensor_data_dir / "start_sensor_data.sh",
            self.sensor_data_dir / "stop_sensor_data.sh",
            self.ic_gvins_dir / "run_zmq.sh",
            self.recorder_dir / "sensor_recorder",
            self.recorder_dir / "config.yaml",
            self.sanity_dir / "sensor_sanity_check",
        ]:
            path.write_text("#!/bin/sh\n", encoding="utf-8")

        self.config = control_server.ControlConfig(
            data_root=self.data_root,
            sensor_data_dir=self.sensor_data_dir,
            ic_gvins_dir=self.ic_gvins_dir,
            recorder_dir=self.recorder_dir,
            sanity_dir=self.sanity_dir,
            log_dir=self.log_dir,
        )
        self.service = control_server.ControlService(self.config)

    def tearDown(self):
        self.tmp.cleanup()

    def test_list_datasets_reports_sensor_dirs_and_size(self):
        dataset = self.data_root / "data_2026-06-18-14-29-01"
        (dataset / "cam_left").mkdir(parents=True)
        (dataset / "cam_right").mkdir()
        (dataset / "imu").mkdir()
        (dataset / "gnss").mkdir()
        (dataset / "imu" / "chunk.dat").write_bytes(b"12345")

        rows = self.service.list_datasets()

        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["name"], dataset.name)
        self.assertEqual(rows[0]["size_bytes"], 5)
        self.assertEqual(rows[0]["sensors"]["cam_left"], True)
        self.assertEqual(rows[0]["sensors"]["cam_right"], True)
        self.assertEqual(rows[0]["sensors"]["imu"], True)
        self.assertEqual(rows[0]["sensors"]["gnss"], True)

    def test_delete_dataset_rejects_path_traversal_and_non_data_names(self):
        with self.assertRaises(control_server.BadRequest):
            self.service.delete_dataset("../data_2026-06-18-14-29-01")
        with self.assertRaises(control_server.BadRequest):
            self.service.delete_dataset("not_a_dataset")

    def test_delete_dataset_removes_only_data_directory(self):
        dataset = self.data_root / "data_2026-06-18-14-29-01"
        dataset.mkdir()
        (dataset / "marker.txt").write_text("x", encoding="utf-8")

        result = self.service.delete_dataset(dataset.name)

        self.assertEqual(result["deleted"], dataset.name)
        self.assertFalse(dataset.exists())

    @mock.patch("subprocess.Popen")
    def test_start_actions_use_fixed_commands(self, popen):
        popen.return_value.pid = 123

        result = self.service.run_action("start_sensor_data")
        self.assertEqual(result["pid"], 123)
        self.assertEqual(
            popen.call_args.args[0],
            [str(self.sensor_data_dir / "start_sensor_data.sh")],
        )

        self.service.run_action("start_ic_gvins_zmq")
        self.assertEqual(
            popen.call_args.args[0],
            [str(self.ic_gvins_dir / "run_zmq.sh")],
        )

        self.service.run_action("start_sensor_recorder")
        self.assertEqual(
            popen.call_args.args[0],
            [
                str(self.recorder_dir / "sensor_recorder"),
                "--config",
                str(self.recorder_dir / "config.yaml"),
            ],
        )

    def test_unknown_action_is_rejected(self):
        with self.assertRaises(control_server.BadRequest):
            self.service.run_action("rm -rf /")

    @mock.patch.object(control_server.ControlService, "_pid_running")
    @mock.patch("control_server.os.killpg")
    @mock.patch("control_server.os.getpgid")
    def test_stop_recorder_waits_for_exit(self, getpgid, killpg, pid_running):
        self.log_dir.mkdir()
        (self.log_dir / "sensor_recorder.pid").write_text("123\n", encoding="utf-8")
        getpgid.return_value = 123
        pid_running.side_effect = [True, False]

        result = self.service.run_action("stop_sensor_recorder")

        self.assertTrue(result["ok"])
        self.assertEqual(result["stopped_pids"], [123])
        self.assertEqual(result["force_killed_pids"], [])
        killpg.assert_called_once_with(123, signal.SIGTERM)
        self.assertFalse((self.log_dir / "sensor_recorder.pid").exists())

    @mock.patch.object(control_server.ControlService, "_pid_running")
    @mock.patch("control_server.os.killpg")
    @mock.patch("control_server.os.getpgid")
    def test_stop_recorder_force_kills_after_timeout(
        self, getpgid, killpg, pid_running
    ):
        config = control_server.ControlConfig(
            data_root=self.data_root,
            sensor_data_dir=self.sensor_data_dir,
            ic_gvins_dir=self.ic_gvins_dir,
            recorder_dir=self.recorder_dir,
            sanity_dir=self.sanity_dir,
            log_dir=self.log_dir,
            stop_timeout_s=0.0,
        )
        service = control_server.ControlService(config)
        self.log_dir.mkdir()
        (self.log_dir / "sensor_recorder.pid").write_text("123\n", encoding="utf-8")
        getpgid.return_value = 123
        pid_running.side_effect = [True, False]

        result = service.run_action("stop_sensor_recorder")

        self.assertTrue(result["ok"])
        self.assertEqual(result["force_killed_pids"], [123])
        self.assertEqual(
            killpg.call_args_list,
            [mock.call(123, signal.SIGTERM), mock.call(123, signal.SIGKILL)],
        )

    @mock.patch("subprocess.run")
    def test_sanity_check_uses_fixed_binary_and_dataset_path(self, run):
        run.return_value = mock.Mock(returncode=0, stdout="ok\n", stderr="")
        dataset = self.data_root / "data_2026-06-18-14-29-01"
        dataset.mkdir()

        result = self.service.sanity_check(dataset.name)

        self.assertTrue(result["ok"])
        self.assertEqual(result["output"], "ok\n")
        self.assertEqual(
            run.call_args.args[0],
            [
                str(self.sanity_dir / "sensor_sanity_check"),
                "--dir",
                str(dataset),
            ],
        )

    def test_status_serializes_to_json_safe_object(self):
        payload = self.service.status()
        json.dumps(payload)
        self.assertIn("processes", payload)


if __name__ == "__main__":
    unittest.main()
