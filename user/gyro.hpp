#pragma once
/*
 * gyro.hpp — 陀螺仪控制 (uinput 注入)
 *
 * 原理: 通过 /dev/uinput 创建虚拟陀螺仪 input 设备, 上报 EV_ABS
 * (ABS_RX/RY/RZ = 三轴角速度, 单位 rad/s), 驱动游戏视角转动。
 *
 * 局限 (Android):
 *   多数 Android 游戏通过 NDK ASensorManager 读 sensor HAL, 而非直接读
 *   input 设备。此时 uinput 注入不会被游戏感知, 需改为 hook sensor HAL
 *   (libsensorservice / 目标 so 注入) 或篡改 sensor 数据。本类适用于
 *   「游戏直接读 /dev/input 陀螺仪」的场景, 或作为 hook 层的输入源。
 *
 * 用法:
 *   c_gyro g;
 *   g.open();
 *   g.set_angular_velocity(0.0, 0.05, 0.0);  // 绕 y 轴 0.05 rad/s
 */

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <cstdio>

class c_gyro
{
private:
	int fd;

	bool emit(int type, int code, int value)
	{
		struct input_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = type;
		ev.code = code;
		ev.value = value;
		return write(fd, &ev, sizeof(ev)) == sizeof(ev);
	}

	void sync()
	{
		struct input_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = EV_SYN;
		ev.code = SYN_REPORT;
		ev.value = 0;
		write(fd, &ev, sizeof(ev));
	}

public:
	c_gyro() : fd(-1) {}

	~c_gyro()
	{
		if (fd > 0)
		{
			ioctl(fd, UI_DEV_DESTROY);
			close(fd);
		}
	}

	bool open()
	{
		struct uinput_setup usetup;

		fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if (fd < 0)
			return false;

		/* 声明支持的轴: 陀螺仪三轴角速度 */
		ioctl(fd, UI_SET_EVBIT, EV_ABS);
		ioctl(fd, UI_SET_ABSBIT, ABS_RX);
		ioctl(fd, UI_SET_ABSBIT, ABS_RY);
		ioctl(fd, UI_SET_ABSBIT, ABS_RZ);

		/* 轴范围: -100..100 rad/s, 分辨率 1/1000 rad/s */
		struct uinput_abs_setup absx = {};
		absx.code = ABS_RX;
		absx.absinfo.maximum = 100000;
		absx.absinfo.minimum = -100000;
		absx.absinfo.resolution = 1000;
		ioctl(fd, UI_ABS_SETUP, &absx);

		struct uinput_abs_setup absy = {};
		absy.code = ABS_RY;
		absy.absinfo.maximum = 100000;
		absy.absinfo.minimum = -100000;
		absy.absinfo.resolution = 1000;
		ioctl(fd, UI_ABS_SETUP, &absy);

		struct uinput_abs_setup absz = {};
		absz.code = ABS_RZ;
		absz.absinfo.maximum = 100000;
		absz.absinfo.minimum = -100000;
		absz.absinfo.resolution = 1000;
		ioctl(fd, UI_ABS_SETUP, &absz);

		memset(&usetup, 0, sizeof(usetup));
		usetup.id.bustype = BUS_USB;
		usetup.id.vendor = 0x1234;
		usetup.id.product = 0x5678;
		strncpy(usetup.name, "Virtual Gyro", UINPUT_MAX_NAME_SIZE - 1);

		if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0)
			return false;
		if (ioctl(fd, UI_DEV_CREATE) < 0)
			return false;

		return true;
	}

	/* 注入三轴角速度 (rad/s) */
	bool set_angular_velocity(float rx, float ry, float rz)
	{
		if (fd < 0)
			return false;

		emit(EV_ABS, ABS_RX, (int)(rx * 1000.0f));
		emit(EV_ABS, ABS_RY, (int)(ry * 1000.0f));
		emit(EV_ABS, ABS_RZ, (int)(rz * 1000.0f));
		sync();
		return true;
	}

	/* 屏幕坐标误差 -> 角速度: 水平/垂直像素差按灵敏度映射 */
	bool aim(float dx_px, float dy_px, float sens)
	{
		/* 简单线性映射: 像素差 * 灵敏度 = 角速度 (rad/s) */
		return set_angular_velocity(-dx_px * sens, dy_px * sens, 0.0f);
	}
};
