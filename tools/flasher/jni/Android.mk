# wind 驱动刷入器 — 可执行 ELF, 不是 .so
#
# AIDE: 项目根目录下就是 jni/, 直接 Build 即可
#       产物 libs/arm64-v8a/wind_flash
# 命令行: ndk-build NDK_PROJECT_PATH=.. APP_BUILD_SCRIPT=Android.mk \
#                   NDK_APPLICATION_MK=Application.mk

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := wind_flash
LOCAL_SRC_FILES := main.c
LOCAL_CFLAGS    := -Os -Wall -Wextra -Wno-unused-parameter -std=gnu11
LOCAL_LDFLAGS   := -Wl,--gc-sections
LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)
