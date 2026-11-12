/* 陀螺仪: 走 sendto hook, 改写发出去的 ASensorEvent 数组。
 *
 * 依据 lsdriver.ko 的 gyro_sendto_handler (0x2544, 1172 字节):
 *   - 只处理 len % 104 == 0 且 len <= 2 MB 的 sendto
 *   - 每条记录 0x68 字节, data[0] 在 +0x18, data[1] 在 +0x1c
 *   - 不是直接覆盖, 而是保留原值尾数、只换指数(见 rescale_float)
 *
 * 这条路不需要定位传感器缓冲, 不占硬件断点寄存器。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/atomic.h>

#include "comm.h"

#define ASENSOR_EVENT_SIZE   0x68
#define ASENSOR_DATA_OFF     0x18
#define ASENSOR_MAX_LEN      (2u * 1024u * 1024u)

static struct kprobe g_gyro_sendto_kp;
static pid_t g_sendto_tgid;   /* 只改这个进程的报文 */
static atomic64_t g_sendto_hits = ATOMIC64_INIT(0);
static atomic64_t g_sendto_rewrites = ATOMIC64_INIT(0);

/* 陀螺仪目标值, float 位模式; 由 OP_GYRO_SET 写入 */
u32 g_gyro_x_bits;
u32 g_gyro_y_bits;
bool g_gyro_on;

/* 保留 incoming 的尾数, 只把指数换成 target 的指数。
 * 直接覆盖一个固定值, 上报序列的方差会突变; 这样改完量级还是原读数的量级。 */
static u32 rescale_float(u32 incoming, u32 target)
{
	int e_in, e_tgt, d;
	u32 mant;

	if ((incoming & 0x7fffffffu) == 0)
		return target;

	e_in  = (incoming >> 23) & 0xff;
	e_tgt = (target   >> 23) & 0xff;
	mant  = (incoming & 0x7fffffu) | 0x800000u;

	if (e_in > e_tgt) {
		d = e_in - e_tgt;
		if (d > 24)
			return incoming;   /* 差太远, 别乱改 */
		mant >>= d;
	} else {
		d = e_tgt - e_in;
		if (d > 24)
			return target;
		mant <<= d;
	}

	return (mant & 0x7fffffu) | ((u32)e_tgt << 23) |
	       (incoming & 0x80000000u);
}

/* 一条 sendto 的报文里可能有多个事件, 逐个改 */
static void rewrite_events(void __user *buf, unsigned long len)
{
	unsigned long off;

	for (off = 0; off + ASENSOR_EVENT_SIZE <= len; off += ASENSOR_EVENT_SIZE) {
		u32 x, y;

		if (get_user(x, (u32 __user *)((char __user *)buf + off + ASENSOR_DATA_OFF)))
			return;
		if (get_user(y, (u32 __user *)((char __user *)buf + off + ASENSOR_DATA_OFF + 4)))
			return;

		x = rescale_float(x, g_gyro_x_bits);
		y = rescale_float(y, g_gyro_y_bits);

		if (put_user(x, (u32 __user *)((char __user *)buf + off + ASENSOR_DATA_OFF)))
			return;
		if (put_user(y, (u32 __user *)((char __user *)buf + off + ASENSOR_DATA_OFF + 4)))
			return;

		atomic64_inc(&g_sendto_rewrites);
	}
}

/* sys_sendto(fd, buf, len, flags, addr, addrlen)
 * x0..x5 -> regs->regs[0..5]
 * 用 kprobe 的 pre_handler: 改的是即将被 sendto 拿去发的用户缓冲。 */
static int gyro_sendto_pre(struct kprobe *p, struct pt_regs *regs)
{
	void __user *buf = (void __user *)regs->regs[1];
	unsigned long len = regs->regs[2];

	if (!READ_ONCE(g_gyro_on) || !buf || !len)
		return 0;

	if (len > ASENSOR_MAX_LEN || (len % ASENSOR_EVENT_SIZE) != 0)
		return 0;

	/* 只在目标进程里改, 别的进程发的包不碰 */
	if (!g_sendto_tgid || current->tgid != g_sendto_tgid)
		return 0;

	atomic64_inc(&g_sendto_hits);
	rewrite_events(buf, len);
	return 0;
}

int wind_gyro_sendto_install(void)
{
	int ret;

	memset(&g_gyro_sendto_kp, 0, sizeof(g_gyro_sendto_kp));
	g_gyro_sendto_kp.symbol_name = "__arm64_sys_sendto";
	g_gyro_sendto_kp.pre_handler = gyro_sendto_pre;

	ret = register_kprobe(&g_gyro_sendto_kp);
	if (ret) {
		g_gyro_sendto_kp.symbol_name = "sys_sendto";
		ret = register_kprobe(&g_gyro_sendto_kp);
	}
	if (ret) {
		pr_err("[wind] gyro sendto: kprobe 注册失败 %d\n", ret);
		return ret;
	}

	pr_info("[wind] gyro sendto: 已挂 %s\n", g_gyro_sendto_kp.symbol_name);
	return 0;
}

void wind_gyro_sendto_remove(void)
{
	if (g_gyro_sendto_kp.addr || g_gyro_sendto_kp.symbol_name)
		unregister_kprobe(&g_gyro_sendto_kp);
	pr_info("[wind] gyro sendto: 已卸载, hits=%lld rewrites=%lld\n",
		(long long)atomic64_read(&g_sendto_hits),
		(long long)atomic64_read(&g_sendto_rewrites));
}

void wind_gyro_sendto_target(pid_t tgid)
{
	WRITE_ONCE(g_sendto_tgid, tgid);
	pr_info("[wind] gyro sendto: 目标 tgid=%d\n", tgid);
}

void wind_gyro_sendto_set(u32 x_bits, u32 y_bits, bool on)
{
	WRITE_ONCE(g_gyro_x_bits, x_bits);
	WRITE_ONCE(g_gyro_y_bits, y_bits);
	WRITE_ONCE(g_gyro_on, on);
}
