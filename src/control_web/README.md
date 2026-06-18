# control_web

`control_web` is a small device-side web server for controlling sensor tools.
It uses only Python standard library modules.

## Run on the device

Copy this directory to:

```text
/userdata/app/control_web
```

Start:

```bash
cd /userdata/app/control_web
./start_control_web.sh
```

Open:

```text
http://<device-ip>:8080
```

## Controlled programs

- `/userdata/app/sensor_data/start_sensor_data.sh`
- `/userdata/app/sensor_data/stop_sensor_data.sh`
- `/userdata/app/ic_gvins/run_zmq.sh`
- `/userdata/app/sensor_recorder/sensor_recorder --config /userdata/app/sensor_recorder/config.yaml`

The service only exposes fixed actions. It does not execute arbitrary shell
commands from the browser.

## Data actions

The page scans:

```text
/userdata/app/sensor_recorder/data/data_*
```

Each row supports:

- `Sanity Check`: runs `/userdata/app/sensor_sanity_check/sensor_sanity_check --dir <dataset>`.
- `Delete`: removes only a direct `data_*` directory under the configured data root.

Logs are written under:

```text
/userdata/app/control_web/logs
```
