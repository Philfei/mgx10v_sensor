# receiver_test thirdparty

该目录用于准备 `sensor_data` / `sensor_decode` 所需的第三方依赖。仓库只跟踪
脚本、版本文件、README 和占位文件；下载源码、预编译产物、厂家二进制库不进
git。

不要随意删除以下本地目录内容，`sensor_data` 编译或部署会用到它们：

```text
receiver_test/thirdparty/install/
receiver_test/thirdparty/rockchip/
```

如果这些目录内容缺失，需要重新运行下载/预编译脚本，或从已配置好的机器复制同名
目录。

## 下载指定版本源码

```bash
cd /home/lrs/mgx10v

receiver_test/thirdparty/download_deps.sh \
  --zeromq 4.3.5 \
  --cppzmq 4.10.0 \
  --protobuf 3.21.12 \
  --protobuf-tag v21.12
```

不传参数时使用 `versions.env` 中的默认版本。

下载和解压输出：

```text
receiver_test/thirdparty/src/
receiver_test/thirdparty/src/_archives/
```

脚本默认会同步更新 `versions.env`，保证后续预编译使用同一套版本。

## 预编译依赖

```bash
cd /home/lrs/mgx10v
receiver_test/thirdparty/prebuild_deps.sh --skip-download
```

如果希望下载并编译一步完成：

```bash
receiver_test/thirdparty/prebuild_deps.sh \
  --zeromq 4.3.5 \
  --cppzmq 4.10.0 \
  --protobuf 3.21.12 \
  --protobuf-tag v21.12
```

输出：

```text
receiver_test/thirdparty/install/host/bin/protoc
receiver_test/thirdparty/install/aarch64-linux-gnu/include/
receiver_test/thirdparty/install/aarch64-linux-gnu/lib/
```

其中：

- host `protoc` 用于在主机生成 `.pb.cc/.pb.h`
- `aarch64-linux-gnu` 下的头文件和库用于 MGX10V 交叉编译

这些文件是本地预编译产物，不跟踪到 git。

## Rockchip 厂家库

`sensor_data` 的相机程序还需要 Rockchip 厂家运行库：

```text
receiver_test/thirdparty/rockchip/aarch64-linux-gnu/lib/
```

该目录下应包含 `librockit.so`、`librkaiq.so`、`librockchip_mpp.so` 等文件。它们
来自厂家 SDK 或已配置好的本地环境，是二进制依赖，不跟踪到 git，但不能随意删除。

## 版本文件

当前默认版本在：

```text
receiver_test/thirdparty/versions.env
```

当前推荐：

```text
zeromq   4.3.5
cppzmq   4.10.0
protobuf 3.21.12, release tag v21.12
```

## 底层脚本

顶层脚本会调用：

```text
receiver_test/thirdparty/scripts/build_host_protoc.sh
receiver_test/thirdparty/scripts/build_for_mgx10v.sh
```

一般不需要直接调用这些脚本。
