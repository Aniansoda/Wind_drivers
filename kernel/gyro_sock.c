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
#include <linux/string.h>

/* 由 entry.c 定义: 卸载中标志 */
extern bool g_wind_unloading;
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <asm/ptrace.h>	/* user_mode() */
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
	/* read(fd, buf, count) 这种平铺形状: 没有 msghdr, 返回后直接编 buf */
	void __user *flat_buf;
	unsigned long flat_len;
	bool flat;
};

/* 不用模块 per-cpu: 模块 .data..percpu 段在 Android GKI 上分配失败会让整个
 * insmod 失败 (Could not allocate 72 bytes percpu data)。
 * 这里改成按线程号散列的一张小表, pre 和 post 拿到同一个槽 —— 比 per-cpu
 * 更准: 系统调用中途被迁移到别的 CPU 也不会丢上下文。 */
#define GYRO_CTX_SLOTS 32
struct gyro_ctx_slot {
	pid_t tid;
	struct gyro_recv_ctx c;
};
static struct gyro_ctx_slot g_gyro_ctx_tab[GYRO_CTX_SLOTS];

static struct gyro_recv_ctx *gyro_ctx_get(void)
{
	pid_t tid = task_pid_nr(current);
	struct gyro_ctx_slot *s = &g_gyro_ctx_tab[(unsigned int)tid % GYRO_CTX_SLOTS];

	if (READ_ONCE(s->tid) != tid) {
		s->tid = tid;
		memset(&s->c, 0, sizeof(s->c));
	}
	return &s->c;
}

/* 传感器读口在不同 ROM 上落点不同: 可能是 recvmsg / recvfrom / read。
 * 以前只挂第一个注册成功的符号, 挂上 recvmsg 就不再试别的 —— 如果
 * sensorservice 实际走的是 read(), 那就是零命中, 表现"装上了但没效果"。
 * 现在把能挂上的全部挂上。 */
#define GYRO_KP_MAX 6
static struct kprobe g_gyro_kps[GYRO_KP_MAX];
static unsigned char g_gyro_kp_kind[GYRO_KP_MAX];   /* 0=msghdr 1=平铺 */
static bool g_gyro_kp_wrapper[GYRO_KP_MAX];
static int g_gyro_kp_cnt;
static int g_gyro_kp_cur;                            /* 注册回调用 */
static bool g_gyro_sock_on;
static bool g_gyro_sock_enabled;                     /* 由 gyro_set_enabled 控制 */
static pid_t g_gyro_tgid;
static bool g_gyro_tgid_warned;
static atomic64_t g_gyro_msgs = ATOMIC64_INIT(0);
static atomic64_t g_gyro_rewrites = ATOMIC64_INIT(0);

static bool g_gyro_type_dumped;
static unsigned int g_gyro_type_seen;
static unsigned long g_gyro_kp_hits[GYRO_KP_MAX];
static atomic64_t g_gyro_scanned = ATOMIC64_INIT(0);
static unsigned int g_gyro_type_hist[64];

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
		atomic64_inc(&g_gyro_scanned);
		if (type < 64)
			g_gyro_type_hist[type]++;
		/* 只动陀螺仪记录; 加速度计/磁力计/压力原样放过 */
		if (type != SENSOR_TYPE_GYRO && type != SENSOR_TYPE_GYRO_UNCAL) {
			/* 一次改写都没发生时, 把看到的前几个类型打出来:
			 * rewrites 一直是 0 就靠这行判断是"没命中"还是"类型不是 4/16"。 */
			if (!g_gyro_type_dumped &&
			    atomic64_read(&g_gyro_rewrites) == 0 &&
			    g_gyro_type_seen < 8) {
				g_gyro_type_seen++;
				pr_info("[wind] gyro-sock: 事件 type=%u off=%lu (期望 4 或 16)\n",
					type, off);
				if (g_gyro_type_seen >= 8)
					g_gyro_type_dumped = true;
			}
			continue;
		}

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


static int gyro_kprobe_idx(struct kprobe *p)
{
	int i;

	for (i = 0; i < g_gyro_kp_cnt; i++)
		if (p == &g_gyro_kps[i])
			return i;
	return -1;
}

/* 0 = msghdr 形状(recvmsg), 1 = 平铺缓冲(read/recvfrom) */
static int gyro_kprobe_kind(struct kprobe *p)
{
	int i = gyro_kprobe_idx(p);

	return i >= 0 ? g_gyro_kp_kind[i] : 0;
}

static bool gyro_kp_wrapper_of(struct kprobe *p)
{
	int i;

	for (i = 0; i < g_gyro_kp_cnt; i++)
		if (p == &g_gyro_kps[i])
			return g_gyro_kp_wrapper[i];
	return false;
}

static struct pt_regs *gyro_user_regs(struct kprobe *p, struct pt_regs *regs)
{
	if (gyro_kp_wrapper_of(p)) {
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
	struct gyro_recv_ctx *ctx = gyro_ctx_get();
	struct user_msghdr umh;
	struct pt_regs *ur;
	int idx;

	ctx->valid = false;

	if (!READ_ONCE(g_gyro_sock_on))
		return 0;
	/* 热开关: gyro_set_enabled(false) / gyro_stop 之后不再改写 */
	if (!READ_ONCE(g_gyro_sock_enabled))
		return 0;

	ur = gyro_user_regs(p, regs);
	if (!ur)
		return 0;

	if (gyro_kprobe_kind(p)) {
		/* read(fd, buf, count): 平铺缓冲。
		 * 形状闸门: 长度必须正好是 0x68 的整数倍 —— 传感器队列读的就是
		 * ASensorEvent 数组。以前不判, system_server 每秒几千次普通 read
		 * 全被当成"传感器消息"计数 (msgs 一分钟涨到十几万), 真正的
		 * 陀螺仪事件被淹在噪声里。 */
		ctx->flat = true;
		ctx->flat_buf = (void __user *)ur->regs[1];
		ctx->flat_len = (unsigned long)ur->regs[2];
		ctx->iov = NULL;
		ctx->iovlen = 0;
		if (!ctx->flat_buf || ctx->flat_len < ASENSOR_EVENT_SIZE)
			return 0;
		if (ctx->flat_len % ASENSOR_EVENT_SIZE)
			return 0;
		idx = gyro_kprobe_idx(p);
		if (idx >= 0)
			g_gyro_kp_hits[idx]++;
		ctx->valid = true;
		return 0;
	}

	ctx->flat = false;
	if (copy_from_user(&umh, (void __user *)ur->regs[1], sizeof(umh)))
		return 0;
	if (!umh.msg_iov || !umh.msg_iovlen || umh.msg_iovlen > GYRO_IOV_MAX)
		return 0;

	idx = gyro_kprobe_idx(p);
	if (idx >= 0)
		g_gyro_kp_hits[idx]++;
	ctx->iov = umh.msg_iov;
	ctx->iovlen = umh.msg_iovlen;
	ctx->valid = true;
	return 0;
}

/* 返回后数据已在用户缓冲里 */
static void gyro_recv_post(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
	struct gyro_recv_ctx *ctx = gyro_ctx_get();
	struct iovec kiov[GYRO_IOV_MAX];
	struct iovec one;
	unsigned long total = 0;
	unsigned int n, i;

	(void)p;
	(void)flags;

	if (!ctx->valid)
		return;
	ctx->valid = false;

	if (ctx->flat) {
		/* x0 是 read() 的返回值, 即实际读到的字节数 */
		unsigned long ret = (unsigned long)regs->regs[0];

		if (ret < ASENSOR_EVENT_SIZE || ret > ctx->flat_len)
			return;
		if (ret % ASENSOR_EVENT_SIZE)
			return;
		one.iov_base = ctx->flat_buf;
		one.iov_len = ret;
		atomic64_inc(&g_gyro_msgs);
		gyro_rewrite_iov(&one, 1, ret);
		return;
	}

	n = (unsigned int)ctx->iovlen;
	if (!n || n > GYRO_IOV_MAX)
		return;
	if (copy_from_user(kiov, ctx->iov, n * sizeof(struct iovec)))
		return;

	for (i = 0; i < n; i++)
		total += kiov[i].iov_len;
	if (total < ASENSOR_EVENT_SIZE || total % ASENSOR_EVENT_SIZE)
		return;

	atomic64_inc(&g_gyro_msgs);
	gyro_rewrite_iov(kiov, n, total);
}

/* 给 gyro 心跳用: 哪条路径在命中、扫过多少条记录、类型分布 */
void gyro_sock_dump(void)
{
	int i, printed = 0;

	pr_info("[wind] gyro-dump: scan=%lld msgs=%lld rewrites=%lld\n",
		(long long)atomic64_read(&g_gyro_scanned),
		(long long)atomic64_read(&g_gyro_msgs),
		(long long)atomic64_read(&g_gyro_rewrites));
	for (i = 0; i < g_gyro_kp_cnt; i++)
		pr_info("[wind] gyro-dump: kp%d %s hits=%lu\n", i,
			g_gyro_kps[i].symbol_name, g_gyro_kp_hits[i]);
	for (i = 0; i < 64 && printed < 8; i++) {
		if (!g_gyro_type_hist[i])
			continue;
		pr_info("[wind] gyro-dump: type=%d count=%u\n", i,
			g_gyro_type_hist[i]);
		printed++;
	}
}

/* 传感器接收路径在不同 ROM / 内核上落点不同, 能挂的全挂上。
 * wrapper=true: 符号是 __arm64_sys_* 形态, 第一个参数是 pt_regs*。
 * kind=1: 平铺缓冲(read/recvfrom), 其余是 msghdr(recvmsg)。
 * recvfrom(fd,buf,len,flags,...) 的 buf 是平铺缓冲, 不是 msghdr ——
 * 游戏侧 SensorEventQueue 用 recv()(bionic 里 recv 就是 recvfrom) 读传感器
 * 事件, 之前把 recvfrom 当 msghdr 解析, 形同漏挂, rewrites 一直是 0。 */
static const struct {
	const char *sym;
	bool wrapper;
	unsigned char kind;
} g_gyro_cands[] = {
	{ "__arm64_sys_recvmsg",  true,  0 },
	{ "__arm64_sys_recvfrom", true,  1 },
	{ "__arm64_sys_read",     true,  1 },
	{ "__sys_recvmsg",        false, 0 },
	{ "ksys_recvmsg",         false, 0 },
};

static void gyro_sock_reset_stats(void)
{
	int i;

	atomic64_set(&g_gyro_msgs, 0);
	atomic64_set(&g_gyro_rewrites, 0);
	atomic64_set(&g_gyro_scanned, 0);
	for (i = 0; i < GYRO_KP_MAX; i++)
		g_gyro_kp_hits[i] = 0;
	for (i = 0; i < 64; i++)
		g_gyro_type_hist[i] = 0;
}

int gyro_sock_install(pid_t system_server_tgid)
{
	/* 每次安装从 0 开始计, 这样一次测试的 msgs/rewrites 就是这次的数据 */
	gyro_sock_reset_stats();
	unsigned int i;
	int ok = 0;

	if (g_gyro_kp_cnt > 0) {
		WRITE_ONCE(g_gyro_tgid, system_server_tgid);
		WRITE_ONCE(g_gyro_sock_on, true);
		return 0;
	}

	for (i = 0; i < sizeof(g_gyro_cands) / sizeof(g_gyro_cands[0]); i++) {
		struct kprobe *kp;

		if (g_gyro_kp_cnt >= GYRO_KP_MAX)
			break;
		kp = &g_gyro_kps[g_gyro_kp_cnt];
		memset(kp, 0, sizeof(*kp));
		kp->symbol_name = g_gyro_cands[i].sym;
		kp->pre_handler = gyro_recv_pre;
		kp->post_handler = gyro_recv_post;

		if (register_kprobe(kp) == 0) {
			g_gyro_kp_kind[g_gyro_kp_cnt] = g_gyro_cands[i].kind;
			g_gyro_kp_wrapper[g_gyro_kp_cnt] = g_gyro_cands[i].wrapper;
			g_gyro_kp_cnt++;
			ok++;
			pr_info("[wind] gyro-sock: 已挂 %s (wrapper=%d kind=%u)\n",
				g_gyro_cands[i].sym, (int)g_gyro_cands[i].wrapper,
				(unsigned)g_gyro_cands[i].kind);
		} else {
			pr_info("[wind] gyro-sock: %s 挂不上\n",
				g_gyro_cands[i].sym);
			memset(kp, 0, sizeof(*kp));
		}
	}

	if (!ok) {
		pr_err("[wind] gyro-sock: 所有接收路径符号都挂不上\n");
		return -EOPNOTSUPP;
	}

	WRITE_ONCE(g_gyro_tgid, system_server_tgid);
	WRITE_ONCE(g_gyro_sock_on, true);
	pr_info("[wind] gyro-sock: 共挂 %d 个入口, tgid=%d\n", ok,
		(int)system_server_tgid);
	return 0;
}

void gyro_sock_remove(void)
{
	int i;

	WRITE_ONCE(g_gyro_sock_on, false);
	WRITE_ONCE(g_gyro_sock_enabled, false);
	for (i = 0; i < g_gyro_kp_cnt; i++) {
		unregister_kprobe(&g_gyro_kps[i]);
		memset(&g_gyro_kps[i], 0, sizeof(g_gyro_kps[i]));
	}
	g_gyro_kp_cnt = 0;
	pr_info("[wind] gyro-sock: 已卸载, msgs=%lld rewrites=%lld\n",
		(long long)atomic64_read(&g_gyro_msgs),
		(long long)atomic64_read(&g_gyro_rewrites));
}

void gyro_sock_set(u32 x_bits, u32 y_bits)
{
	WRITE_ONCE(g_gyro_x_bits, x_bits);
	WRITE_ONCE(g_gyro_y_bits, y_bits);
}

void gyro_sock_set_enabled(bool enabled)
{
	WRITE_ONCE(g_gyro_sock_enabled, enabled);
}

void gyro_sock_stats(long long *msgs, long long *rewrites)
{
	*msgs = atomic64_read(&g_gyro_msgs);
	*rewrites = atomic64_read(&g_gyro_rewrites);
}

bool gyro_sock_active(void)
{
	return READ_ONCE(g_gyro_sock_on) && g_gyro_kp_cnt > 0;
}
