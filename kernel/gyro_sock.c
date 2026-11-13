/* 陀螺仪: 在 recvmsg 返回后按 iovec 改写传感器事件 (对齐 paradise 的 gyro 实现)
 *
 * paradise 的做法:
 *   [paradise] gyro: hooked %d sockets in system_server (pid %d)
 *   [paradise] gyro: no SEQPACKET sockets found in system_server
 *   [paradise] gyro: failed to resolve iterator symbols
 * 它遍历 system_server 的 socket 链表挑 SOCK_SEQPACKET, 再去动 iov_iter。
 *
 * 这条路不这么做, 原因有两条:
 *   1. struct msghdr 从 5.x 起就没有 msg_iov/msg_iovlen 了, 换成 msg_iter;
 *      而 struct iov_iter 的内部布局在 6.1/6.6/6.12/6.18 之间并不一致。
 *      直接摸它等于给自己钉死一个内核版本。
 *   2. struct socket 链表同样不是稳定 ABI。
 *
 * 改挂 __arm64_sys_recvmsg, 参数用 UAPI 的 struct user_msghdr, 十年前的布局
 * 和十年后一样。socket 类型不再判断, 改成看每条记录的 type 字段:
 * 只有 type == SENSOR_TYPE_GYROSCOPE 或 GYRO_UNCAL 的记录才改。
 * 这比判 socket 类型更准 —— 同一条流里的加速度计/磁力计记录原样放过。
 */
#include <linux/kernel.h>

/* 由 entry.c 定义: 卸载中标志 */
extern bool g_wind_unloading;
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/percpu.h>
#include <linux/uaccess.h>
#include <linux/socket.h>
#include <linux/atomic.h>

#include "gyro.h"

/* ASensorEvent 布局 (sizeof == 0x68):
 *   +0x00 int32 version
 *   +0x04 int32 sensor
 *   +0x08 int32 type
 *   +0x0c int32 reserved0
 *   +0x10 int64 timestamp
 *   +0x18 float data[16]
 */
#define ASENSOR_EVENT_SIZE   0x68u
#define ASENSOR_TYPE_OFF     0x08u          /* 实际有效的是 +0x08 起的 int32 */
#define ASENSOR_DATA_OFF     0x18u
#define SENSOR_TYPE_GYRO         4
#define SENSOR_TYPE_GYRO_UNCAL  16
#define GYRO_MSG_MAX         (2u * 1024u * 1024u)
#define GYRO_IOV_MAX         64

struct gyro_recv_ctx {
	struct iovec __user *iov;
	unsigned long iovlen;
	bool valid;
};

static DEFINE_PER_CPU(struct gyro_recv_ctx, gyro_ctx);

static struct kprobe g_gyro_kp;
static bool g_gyro_sock_on;
static pid_t g_gyro_tgid;
static atomic64_t g_gyro_msgs = ATOMIC64_INIT(0);
static atomic64_t g_gyro_rewrites = ATOMIC64_INIT(0);

static u32 g_gyro_x_bits;
static u32 g_gyro_y_bits;

/* 保留 incoming 的尾数, 只换成 target 的指数。
 * 直接写固定值会让上报序列的方差突变; 这样量级还是原读数的量级。 */
static u32 gyro_rescale(u32 incoming, u32 target)
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
			return incoming;
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

/* 在扁平化后的 iovec 上读一个 u32, flat_off 相对整条报文起始 */
static int iov_get_u32(struct iovec *iov, unsigned int n,
		       unsigned long flat_off, u32 *out)
{
	unsigned int i;
	unsigned long acc = 0;

	for (i = 0; i < n; i++) {
		if (flat_off < acc + iov[i].iov_len) {
			unsigned long in = flat_off - acc;

			return get_user(*out,
				(u32 __user *)((char __user *)iov[i].iov_base + in));
		}
		acc += iov[i].iov_len;
	}
	return -EFAULT;
}

static int iov_put_u32(struct iovec *iov, unsigned int n,
		       unsigned long flat_off, u32 val)
{
	unsigned int i;
	unsigned long acc = 0;

	for (i = 0; i < n; i++) {
		if (flat_off < acc + iov[i].iov_len) {
			unsigned long in = flat_off - acc;

			return put_user(val,
				(u32 __user *)((char __user *)iov[i].iov_base + in));
		}
		acc += iov[i].iov_len;
	}
	return -EFAULT;
}

static void gyro_rewrite_iov(struct iovec *iov, unsigned int n, unsigned long total)
{
	unsigned long off;

	for (off = 0; off + ASENSOR_EVENT_SIZE <= total; off += ASENSOR_EVENT_SIZE) {
		u32 type, raw[2], x, y;

		if (iov_get_u32(iov, n, off + ASENSOR_TYPE_OFF, &type))
			continue;
		/* 只动陀螺仪记录; 加速度计/磁力计/压力原样放过 */
		if (type != SENSOR_TYPE_GYRO && type != SENSOR_TYPE_GYRO_UNCAL)
			continue;

		if (iov_get_u32(iov, n, off + ASENSOR_DATA_OFF, &raw[0]))
			continue;
		if (iov_get_u32(iov, n, off + ASENSOR_DATA_OFF + 4, &raw[1]))
			continue;

		x = gyro_rescale(raw[0], READ_ONCE(g_gyro_x_bits));
		y = gyro_rescale(raw[1], READ_ONCE(g_gyro_y_bits));

		if (iov_put_u32(iov, n, off + ASENSOR_DATA_OFF, x))
			continue;
		if (iov_put_u32(iov, n, off + ASENSOR_DATA_OFF + 4, y))
			continue;

		atomic64_inc(&g_gyro_rewrites);
	}
}

/* kprobe 落在 __arm64_sys_* 上时, 函数第一个参数是 struct pt_regs *,
 * 真正的 fd/msg/flags 在它指向的用户现场里 —— 要再解一层。
 * 以前直接拿 regs->regs[1] 当 msghdr 指针, 拿到的是内核栈上的 pt_regs 地址,
 * copy_from_user 必然失败, 于是 msgs 计数一直不涨, 改写一次都没发生过。
 * 落在 __sys_recvmsg / ksys_recvmsg 这类直接函数上时 x0=fd, x1=msghdr。
 */
static bool g_gyro_kp_wrapper;
static const char *g_gyro_kp_sym;

static struct pt_regs *gyro_user_regs(struct pt_regs *regs)
{
	if (g_gyro_kp_wrapper) {
		struct pt_regs *ur = (struct pt_regs *)regs->regs[0];

		if (!ur || !user_mode(ur))
			return NULL;
		return ur;
	}
	return regs;
}

static int gyro_recv_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* 模块正在卸载: 立刻退出, 不要碰到即将被释放的状态 */
	if (READ_ONCE(g_wind_unloading))
		return 0;
	struct gyro_recv_ctx *ctx = this_cpu_ptr(&gyro_ctx);
	struct user_msghdr umh;
	struct pt_regs *ur;

	ctx->valid = false;

	if (!READ_ONCE(g_gyro_sock_on))
		return 0;
	if (!g_gyro_tgid || current->tgid != g_gyro_tgid)
		return 0;
	if (!READ_ONCE(g_gyro_x_bits) && !READ_ONCE(g_gyro_y_bits))
		return 0;

	ur = gyro_user_regs(regs);
	if (!ur)
		return 0;
	if (copy_from_user(&umh, (void __user *)ur->regs[1], sizeof(umh)))
		return 0;
	if (!umh.msg_iov || !umh.msg_iovlen || umh.msg_iovlen > GYRO_IOV_MAX)
		return 0;

	ctx->iov = umh.msg_iov;
	ctx->iovlen = umh.msg_iovlen;
	ctx->valid = true;
	atomic64_inc(&g_gyro_msgs);
	return 0;
}

/* 返回后数据已在用户 iovec 里 */
static void gyro_recv_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	struct gyro_recv_ctx *ctx = this_cpu_ptr(&gyro_ctx);
	struct iovec kiov[GYRO_IOV_MAX];
	unsigned long total = 0;
	unsigned int n, i;

	if (!ctx->valid)
		return;
	ctx->valid = false;

	n = (unsigned int)ctx->iovlen;
	if (!n || n > GYRO_IOV_MAX)
		return;
	if (copy_from_user(kiov, ctx->iov, n * sizeof(struct iovec)))
		return;

	for (i = 0; i < n; i++) {
		total += kiov[i].iov_len;
		if (total > GYRO_MSG_MAX)
			return;
	}
	if (total < ASENSOR_EVENT_SIZE || (total % ASENSOR_EVENT_SIZE) != 0)
		return;

	gyro_rewrite_iov(kiov, n, total);
}

/* 传感器接收路径在不同 ROM / 内核上落点不同, 全试一遍。
 * wrapper=true 表示符号是 __arm64_sys_* 形态, 第一个参数是 pt_regs*。 */
static const struct {
	const char *sym;
	bool wrapper;
} g_gyro_cands[] = {
	{ "__arm64_sys_recvmsg",  true  },
	{ "__arm64_sys_recvfrom", true  },
	{ "__sys_recvmsg",        false },
	{ "ksys_recvmsg",         false },
	{ "__se_sys_recvmsg",     false },
};

int gyro_sock_install(pid_t system_server_tgid)
{
	int ret = -EOPNOTSUPP;
	unsigned int i;

	if (g_gyro_kp.addr) {
		WRITE_ONCE(g_gyro_tgid, system_server_tgid);
		WRITE_ONCE(g_gyro_sock_on, true);
		return 0;
	}

	for (i = 0; i < sizeof(g_gyro_cands) / sizeof(g_gyro_cands[0]); i++) {
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
		g_gyro_kp.symbol_name = g_gyro_cands[i].sym;
		g_gyro_kp.pre_handler = gyro_recv_pre;
		g_gyro_kp.post_handler = gyro_recv_post;

		ret = register_kprobe(&g_gyro_kp);
		if (ret == 0) {
			g_gyro_kp_wrapper = g_gyro_cands[i].wrapper;
			g_gyro_kp_sym = g_gyro_cands[i].sym;
			WRITE_ONCE(g_gyro_tgid, system_server_tgid);
			WRITE_ONCE(g_gyro_sock_on, true);
			pr_info("[wind] gyro-sock: 已挂 %s (wrapper=%d), tgid=%d\n",
				g_gyro_kp_sym, (int)g_gyro_kp_wrapper,
				(int)system_server_tgid);
			return 0;
		}
		pr_info("[wind] gyro-sock: %s 挂不上 (%d)\n",
			g_gyro_cands[i].sym, ret);
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
	}

	pr_err("[wind] gyro-sock: 所有接收路径符号都挂不上, 最后错误 %d\n", ret);
	return ret;
}

void gyro_sock_remove(void)
{
	WRITE_ONCE(g_gyro_sock_on, false);
	if (g_gyro_kp.addr || g_gyro_kp.symbol_name) {
		unregister_kprobe(&g_gyro_kp);
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
	}
	pr_info("[wind] gyro-sock: 已卸载 (%s), msgs=%lld rewrites=%lld\n",
		g_gyro_kp_sym ? g_gyro_kp_sym : "-",
		(long long)atomic64_read(&g_gyro_msgs),
		(long long)atomic64_read(&g_gyro_rewrites));
	g_gyro_kp_sym = NULL;
}

void gyro_sock_set(u32 x_bits, u32 y_bits)
{
	WRITE_ONCE(g_gyro_x_bits, x_bits);
	WRITE_ONCE(g_gyro_y_bits, y_bits);
}

bool gyro_sock_active(void)
{
	return READ_ONCE(g_gyro_sock_on) && g_gyro_kp.addr;
}
