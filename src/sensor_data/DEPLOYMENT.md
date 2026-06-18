# sensor_data Deployment Bundle

After building, the self-contained deployment bundle is generated under:

```text
receiver_test/sensor_data_build/deploy/
```

Copy the contents of this directory to the device:

```bash
ssh root@10.42.0.42 "mkdir -p /userdata/app/sensor_data"
scp -r receiver_test/sensor_data_build/deploy/* \
  root@10.42.0.42:/userdata/app/sensor_data/
```

The bundle contains:

```text
cam_sensor_data
imu_sensor_data
gnss_sensor_data
start_sensor_data.sh
stop_sensor_data.sh
start_vendor_pwm.sh
stop_vendor_pwm.sh
README.md
CAMERA_RESOLUTION.md
DEPLOYMENT.md
proto/
lib/
  libzmq.so*
  libprotobuf.so*
  librga.so*
  rockchip/*.so*
```

Run on the device:

```bash
cd /userdata/app/sensor_data
./start_sensor_data.sh
```

Start only selected publishers:

```bash
./start_sensor_data.sh cam
./start_sensor_data.sh imu gnss
```

The launch script sets `LD_LIBRARY_PATH` to the bundle-local `lib/` and
`lib/rockchip/` directories.
