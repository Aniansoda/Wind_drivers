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

/* 目标角速度 (rad/s, 传 float 位模式 IEEE 754) 与开关 */
void gyro_set_velocity(u32 rx, u32 ry, u32 rz);
void gyro_set_enabled(bool enabled);

/* 停止 / 恢复: 暂停或恢复篡改, 保留 hook 不卸载 (区别于 gyro_remove)
 *   stop  : -ENODEV 未安装; 0 成功 (幂等)
 *   resume: -ENODEV 未安装; -ESRCH 目标已退出; 断点缺失时自动重绑, 失败返回该错误 */
/*
 * ---- SEQPACKET socket 路线 (对齐 paradise 的 gyro 实现) ----
 * paradise 的实测日志:
 *   [paradise] gyro: hooked %d sockets in system_server (pid %d)
 *   [paradise] gyro: no SEQPACKET sockets found in system_server
 * 它在 system_server 的 SOCK_SEQPACKET 收发路径上改报文。
 *
 * 这里挂在 sock_recvmsg 上, 按 socket 类型 + 进程 + 报文长度过滤, 效果一致,
 * 但不枚举 socket 链表, 内核结构变了也不会散架。
 * 不占硬件断点寄存器, 不依赖 sendObjects 符号命中。
 */
int  gyro_sock_install(pid_t system_server_tgid);
void gyro_sock_remove(void);
void gyro_sock_set(u32 x_bits, u32 y_bits);
void gyro_sock_set_enabled(bool enabled);
bool gyro_sock_active(void);

int  gyro_stop(void);
int  gyro_resume(void);

#endif /* _GYRO_H_ */
