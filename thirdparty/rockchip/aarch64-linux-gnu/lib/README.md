This directory must contain Rockchip vendor runtime libraries for MGX10V.

Required core libraries:

```text
librockit.so
librkaiq.so
librockchip_mpp.so
```

Keep any accompanying `.so*` files from the same vendor SDK bundle, because
they may be indirect runtime dependencies. These binaries are intentionally not
tracked by git.
