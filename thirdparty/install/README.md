This directory stores local prebuilt dependencies for receiver_test.

It is intentionally not tracked by git, except this README and `.gitkeep`.
Do not delete the generated contents if you want to build without downloading
and prebuilding third-party libraries again.

Expected local contents include:

```text
host/bin/protoc
aarch64-linux-gnu/include/
aarch64-linux-gnu/lib/libzmq.so*
aarch64-linux-gnu/lib/libprotobuf.so*
```

Regenerate with:

```bash
receiver_test/thirdparty/download_deps.sh
receiver_test/thirdparty/prebuild_deps.sh --skip-download
```
