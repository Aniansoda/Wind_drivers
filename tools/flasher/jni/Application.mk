# 驱动只跑 64 位, 只出 arm64-v8a
APP_ABI      := arm64-v8a
APP_PLATFORM := android-21
APP_OPTIM    := release
APP_CFLAGS   := -Os -fno-unwind-tables -fno-asynchronous-unwind-tables
