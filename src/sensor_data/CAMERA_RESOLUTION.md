# Camera Resolution Control

`start_sensor_data.sh` controls the camera resolution through `CAM_ARGS`.
The script passes `CAM_ARGS` directly to `cam_sensor_data`, and
`cam_sensor_data` uses `--w <width> --h <height>` to request the V4L2 capture
resolution.

## Start All Sensors With A Camera Resolution

```bash
cd /userdata/app/sensor_data
CAM_ARGS="--w 1280 --h 720 --fps 20" ./start_sensor_data.sh
```

This starts `cam_sensor_data`, `imu_sensor_data`, and `gnss_sensor_data`.
Only `cam_sensor_data` receives `CAM_ARGS`.

## Start Only The Camera

```bash
cd /userdata/app/sensor_data
CAM_ARGS="--w 1280 --h 720 --fps 20" ./start_sensor_data.sh cam
```

## Default Resolution

If `CAM_ARGS` is not set, `cam_sensor_data` uses its built-in defaults:

```text
1920x1080, fps 20
```

## Confirm The Actual Resolution

The requested resolution can be rejected or adjusted by the device driver.
Always check the `cam_sensor_data` log after startup:

```bash
tail -n 50 logs/cam_sensor_data.log
```

Look for:

```text
actual capture: L=<width>x<height> R=<width>x<height>
```

That line is the actual V4L2 negotiated resolution.

## PWM Frequency

`PWM_FREQ` controls the external camera trigger frequency. It does not control
image resolution. Keep it consistent with `--fps`:

```bash
PWM_FREQ=20 CAM_ARGS="--w 1280 --h 720 --fps 20" ./start_sensor_data.sh cam
```
