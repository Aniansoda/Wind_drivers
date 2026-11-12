# wind 驱动刷入器（AIDE / ndk-build）

可执行 ELF，不是 .so。走 `BUILD_EXECUTABLE`。

## AIDE

1. AIDE 新建「C/C++ 原生项目」，把整个 `flasher/` 目录当项目根。
2. 目录结构必须是：

```
flasher/
└── jni/
    ├── Android.mk
    ├── Application.mk
    └── main.c
```

3. 直接 Build。产物：

```
flasher/libs/arm64-v8a/wind_flash
```

推送：

```sh
adb push libs/arm64-v8a/wind_flash /data/local/tmp/
adb shell su -c "chmod 755 /data/local/tmp/wind_flash"
adb shell su -c "/data/local/tmp/wind_flash /data/local/tmp/wind.ko"
```

## 命令行 ndk-build

```sh
cd flasher
$NDK/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk \
               NDK_APPLICATION_MK=jni/Application.mk
```

## 设备上直接 clang（AIDE 的 NDK 就行）

```sh
NDK=/data/data/com.aide.ui3.max/no_backup/ndksupport-1710240003/android-ndk-aide
B=$NDK/toolchains/llvm/prebuilt/linux-aarch64/bin
$B/clang --target=aarch64-linux-android21 \
    --sysroot=$NDK/toolchains/llvm/prebuilt/linux-aarch64/sysroot \
    -O2 -std=gnu11 -o wind_flash main.c
```

## 参数

```
wind_flash [ko路径]       默认 /data/local/tmp/wind.ko
```

菜单一次决定，配置存 `/data/local/tmp/wind.cfg`，下次问是否复用。

进程名写 `/data/local/tmp/wind_proc_name`，Keep 的 `DrawInit()`
启动时读它做 `prctl(PR_SET_NAME)`。

## 和 wind_flash.sh 的关系

`../wind_flash.sh` 是同一套逻辑的 shell 版，不编译也能用；
这个 C 版不依赖 sh 和 printf/read，适合直接 `su -c` 调用。
