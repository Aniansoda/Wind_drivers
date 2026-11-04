#ifndef _GYRO_H_
#define _GYRO_H_

#include <linux/kernel.h>
#include <linux/types.h>

/*
 * gyro.h — 陀螺仪控制 (内核态 uprobe hook, 对齐 hookmod 参考驱动)
 *
 * 控制方式: 不再走用户态 /dev/uinput 注入, 改为内核态 hook
 *   /system/lib64/libsensor.so 的 android::BitTube::sendObjects,
 *   在 sensor 数据 (sensors_event_t) 经 BitTube 传输时拦截并篡改
 *   陀螺仪事件 (SENSOR_TYPE_GYROSCOPE) 的 x/y/z 角速度。
 *
 * 与 hookmod 参考驱动一致的关键点:
 *   - 目标 .so : /system/lib64/libsensor.so
 *   - 目标符号 : _ZN7android7BitTube11sendObjectsERKNS_2spIS0_EEPKvmm
 *   - 定位方式 : filp_open + kernel_read 读 ELF, 解析 .dynsym 取偏移,
 *               加目标进程 libsensor.so 加载基址 = 运行时地址
 *   - hook 方式 : register_user_hw_breakpoint 执行断点 (uprobe)
 */

#define GYRO_LIBSENSOR_PATH   "/system/lib64/libsensor.so"
#define GYRO_LIBSENSOR_NAME   "libsensor.so"
#define GYRO_SENDOBJECTS_SYM  "_ZN7android7BitTube11sendObjectsERKNS_2spIS0_EEPKvmm"

/* 安装/卸载陀螺仪 hook (pid = 读 sensor 的目标进程) */
int  gyro_install(pid_t pid);
void gyro_remove(void);

/* 目标角速度 (rad/s) 与开关 */
void gyro_set_velocity(float rx, float ry, float rz);
void gyro_set_enabled(bool enabled);

#endif /* _GYRO_H_ */
