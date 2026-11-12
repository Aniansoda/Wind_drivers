/* 陀螺仪: SEQPACKET 接收路径改写 (对齐 paradise 的 gyro 实现)
 *
 * paradise 的做法:
 *   [paradise] gyro: hooked %d sockets in system_server (pid %d)
 *   [paradise] gyro: no SEQPACKET sockets found in system_server
 *   [paradise] gyro: failed to resolve iterator symbols
 *
 * 它遍历 system_server 的 socket 链表挑 SOCK_SEQPACKET, 在收发路径上改报文。
 * 这里不枚举 socket: 在 sock_recvmsg 上判 socket 类型 + 进程 + 报文长度,
 * 效果一样但不依赖任何链表结构, 内核版本变了也不会散架。
 *
 * 为什么改 system_server 而不是游戏:
 *   传感器 HAL 把 ASensorEvent 批量送给 system_server, 由它转发给各个 app。
 *   在 ingest 侧改一次, 下游所有客户端拿到的就是改过的值。
 *
 * 不做指令级改写, 不占硬件断点寄存器, 不依赖任何符号命中。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/atomic.h>
#include <net/sock.h>
#include <linux/iovec.h>

#include "gyro.h"

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

#define ASENSOR_EVENT_SIZE   0x68u          /* sizeof(ASensorEvent) */
#define ASENSOR_DATA_OFF     0x18u          /* data[0], data[1] 在 +0x18/+0x1c */
#define GYRO_MSG_MAX         (2u * 1024u * 1024u)
#define GYRO_IOV_MAX         64

struct gyro_recv_ctx {
	void __user *iov;      /* msg->msg_iov, 用户态指针 */
	unsigned long iovlen;
	unsigned long size;
	bool valid;
};

static DEFINE_PER_CPU(struct gyro_recv_ctx, gyro_ctx);

static struct kprobe g_gyro_kp;
static bool g_gyro_sock_on;
static pid_t g_gyro_tgid;          /* system_server 的 tgid */
static atomic64_t g_gyro_msgs = ATOMIC64_INIT(0);
static atomic64_t g_gyro_rewrites = ATOMIC64_INIT(0);

/* 目标角速度 (float 位模式) */
static u32 g_gyro_x_bits;
static u32 g_gyro_y_bits;

/* 保留 incoming 的尾数, 只换成 target 的指数。
 * 直接写固定值, 上报序列的方差会突变; 这样量级还是原读数的量级。 */
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

/* 在扁平化后的 iovec 里读写一个 u32。
 * flat_off 是相对整条报文起始的字节偏移。 */
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

static void gyro_rewrite_iov(struct iovec *iov, unsigned int n, unsigned long total)
{
	unsigned long off;

	for (off = 0; off + ASENSOR_EVENT_SIZE <= total; off += ASENSOR_EVENT_SIZE) {
		u32 raw[2], x, y;

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

/* sock_recvmsg(struct socket *sock, struct msghdr *msg, size_t size, int flags) */
static int gyro_recv_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct gyro_recv_ctx *ctx = this_cpu_ptr(&gyro_ctx);
	struct socket *sock = (struct socket *)regs->regs[0];
	struct msghdr *msg = (struct msghdr *)regs->regs[1];
	unsigned long size = regs->regs[2];

	ctx->valid = false;

	if (!READ_ONCE(g_gyro_sock_on) || !sock || !msg)
		return 0;
	if (!g_gyro_tgid || current->tgid != g_gyro_tgid)
		return 0;
	if (!READ_ONCE(g_gyro_x_bits) && !READ_ONCE(g_gyro_y_bits))
		return 0;
	if (!size || (size % ASENSOR_EVENT_SIZE) != 0 || size > GYRO_MSG_MAX)
		return 0;
	if (!sock->sk || sock->sk->sk_family != AF_UNIX)
		return 0;
	/* 传感器走 SOCK_SEQPACKET; SOCK_DGRAM 是别的用途, 不碰 */
	if (sock->type != SOCK_SEQPACKET)
		return 0;
	if (!msg->msg_iov || !msg->msg_iovlen || msg->msg_iovlen > GYRO_IOV_MAX)
		return 0;

	ctx->iov = msg->msg_iov;
	ctx->iovlen = msg->msg_iovlen;
	ctx->size = size;
	ctx->valid = true;
	atomic64_inc(&g_gyro_msgs);
	return 0;
}

/* 返回之后数据已经在用户 iovec 里了, 这时候改 */
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
	if (n > GYRO_IOV_MAX)
		return;
	if (copy_from_user(kiov, ctx->iov, n * sizeof(struct iovec)))
		return;

	for (i = 0; i < n; i++) {
		total += kiov[i].iov_len;
		if (total > GYRO_MSG_MAX)
			return;
	}
	if (total < ASENSOR_EVENT_SIZE)
		return;

	gyro_rewrite_iov(kiov, n, total);
}

int gyro_sock_install(pid_t system_server_tgid)
{
	int ret;

	if (g_gyro_kp.addr) {
		WRITE_ONCE(g_gyro_tgid, system_server_tgid);
		WRITE_ONCE(g_gyro_sock_on, true);
		return 0;
	}

	memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
	g_gyro_kp.symbol_name = "sock_recvmsg";
	g_gyro_kp.pre_handler = gyro_recv_pre;
	g_gyro_kp.post_handler = gyro_recv_post;

	ret = register_kprobe(&g_gyro_kp);
	if (ret) {
		pr_err("[wind] gyro-sock: sock_recvmsg 挂不上 (%d)\n", ret);
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
		g_gyro_kp.symbol_name = "unix_dgram_recvmsg";
		g_gyro_kp.pre_handler = gyro_recv_pre;
		g_gyro_kp.post_handler = gyro_recv_post;
		ret = register_kprobe(&g_gyro_kp);
	}
	if (ret) {
		pr_err("[wind] gyro-sock: 回落 unix_dgram_recvmsg 也失败 (%d)\n", ret);
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
		return ret;
	}

	WRITE_ONCE(g_gyro_tgid, system_server_tgid);
	WRITE_ONCE(g_gyro_sock_on, true);
	pr_info("[wind] gyro-sock: 已挂 %s, tgid=%d\n",
		g_gyro_kp.symbol_name, system_server_tgid);
	return 0;
}

void gyro_sock_remove(void)
{
	WRITE_ONCE(g_gyro_sock_on, false);
	if (g_gyro_kp.addr || g_gyro_kp.symbol_name) {
		unregister_kprobe(&g_gyro_kp);
		memset(&g_gyro_kp, 0, sizeof(g_gyro_kp));
	}
	pr_info("[wind] gyro-sock: 已卸载, msgs=%lld rewrites=%lld\n",
		(long long)atomic64_read(&g_gyro_msgs),
		(long long)atomic64_read(&g_gyro_rewrites));
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
