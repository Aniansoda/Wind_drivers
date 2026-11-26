/*
 * wrpbp.c — WRP 数据断点追踪 (观察点 + 命中即写回)
 *
 * 最隐蔽的一族: 不改代码字节, 不动页表映射, 不占 BRP 执行断点, 也不产生
 * 异常信号。只开 WRP 观察点盯住「弹道/朝向向量」那块内存, 游戏每写一次
 * (每帧重算准心), 立刻把它改回目标向量; 可选再走 fpsimd 写 S 寄存器。
 *
 * 隐蔽面: 代码段 / smaps / 页表全干净, 只剩数据值本身与游戏计算不一致。
 *
 * 两个必须处理的坑:
 *   1) 我们自己写回也会命中观察点 -> 递归。per-CPU busy 计数 + 只在
 *      user_mode 命中时写回, 双保险。
 *   2) 12 字节向量一个 WRP 覆盖不全 (arm64 WRP 最大 LEN_8) -> 每个点
 *      只铺 8 字节主事件 (覆盖 x/y), 写回时整段 12 字节一起改。
 *
 * 依赖: CONFIG_HAVE_HW_BREAKPOINT + CONFIG_PERF_EVENTS (GKI 默认开)。
 * 白名单外符号 (register_user_hw_breakpoint / unregister_hw_breakpoint /
 * fpsimd_preserve_current_state / copy_*_user_nofault) 全走 sym.c 运行时解析。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/thread_info.h>
#include <linux/compat.h>
#include <asm/ptrace.h>
#include <asm/fpsimd.h>

#include "comm.h"
#include "sym.h"
#include "wrpbp.h"

/* 由 entry.c 定义: 卸载中标志 */
extern bool g_wind_unloading;
/* gyro.c 导出: 陀螺仪是否占着硬件断点寄存器 */
extern bool gyro_holds_breakpoints(void);
/* 浮点写回总开关: 定义在 clonebp.c (任务②把 breakpoint.c 的浮点写回迁过去时
 * 一起迁过去了), 这里只引用, 不能重复定义。 */
extern bool g_wind_fp_writeback;

#define BP_SLOT_MAX       16
#define BP_REC_PER_SLOT   64
#define WRPBP_VEC_BYTES   12
#define WRPBP_EV_PER_SLOT 1
#define WRPBP_MAX_POINTS  2
#define HWBP_MASK_ALL     0x7U
#define WRPBP_RATE_MAX    800

/* ---- 卡写修复开关 ----
 * arm64 的 WRP 在 store 提交前 trap。handler 写完目标地址后返回, CPU 会
 * 重新执行那条 store, 把我们写的值覆盖掉, 并再次触发同一 watchpoint ->
 * store 永远执行不完, 目标线程卡死, 后续 netlink 命令(含 bind)再也下发不出去。
 * 修法: 写回成功后把 pc 推进到下一条指令, 由驱动独占这次写入, 不让 CPU 重试。
 * insmod wind.ko wind_wrp_skip_store=0 可关掉(回到旧的覆盖行为)。
 */
static bool g_wrp_skip_store = true;
module_param_named(wind_wrp_skip_store, g_wrp_skip_store, bool, 0644);
MODULE_PARM_DESC(wind_wrp_skip_store,
		 "WRP 写回后跳过触发的 store 指令, 默认 1 (修复卡写)");

#define WRPBP_SKIP_MAX    800   /* 每秒最多跳过这么多 store, 超了退回旧行为 */

/* 跳过节流: 页面被当寄存器狂写时, 无节制跳过会把指令流跳乱 */
static unsigned long wrpbp_skip_pc[BP_SLOT_MAX];   /* 最近一次的 store pc, 诊断用 */
static unsigned long wrpbp_skip_last[BP_SLOT_MAX];
static u32 wrpbp_skip_cnt[BP_SLOT_MAX];

/* ===================================================================
 * 环形记录
 * =================================================================== */
static void bp_rec_push(void *recs, size_t rec_size, u32 *head, u32 *count,
			const void *rec)
{
	char *base = recs;
	char *dst = base + (*head) * rec_size;

	memcpy(dst, rec, rec_size);
	*head = (*head + 1) % BP_REC_PER_SLOT;
	if (*count < BP_REC_PER_SLOT)
		(*count)++;
}

static u32 bp_rec_read(const void *recs, size_t rec_size, u32 head, u32 count,
		       u32 start_index, void *out, u32 max)
{
	char *base = (char *)recs;
	u32 i, n = 0;

	if (start_index >= count || max == 0)
		return 0;

	for (i = start_index; i < count && n < max; i++) {
		u32 oldest = (head - count + BP_REC_PER_SLOT) % BP_REC_PER_SLOT;
		u32 idx = (oldest + i) % BP_REC_PER_SLOT;

		memcpy((char *)out + n * rec_size, base + idx * rec_size, rec_size);
		n++;
	}
	return n;
}

/* 类型/长度映射 */
static int bp_type_to_hw(int type)
{
	switch (type) {
	case BP_TYPE_EXEC:   return HW_BREAKPOINT_X;
	case BP_TYPE_READ:   return HW_BREAKPOINT_R;
	case BP_TYPE_WRITE:  return HW_BREAKPOINT_W;
	case BP_TYPE_RW:     return HW_BREAKPOINT_RW;
	default:             return -1;
	}
}

static int bp_len_to_hw(int len)
{
	switch (len) {
	case 1: return HW_BREAKPOINT_LEN_1;
	case 2: return HW_BREAKPOINT_LEN_2;
	case 4: return HW_BREAKPOINT_LEN_4;
	case 8: return HW_BREAKPOINT_LEN_8;
	default: return -1;
	}
}

struct wrpbp_slot {
	uint64_t watch_addr;
	uint32_t watch_len;
	uint32_t type;
	uint32_t components;
	uint32_t freg_base;         /* 起始 S 寄存器号 */
	uint32_t freg_mask;         /* bit0..bit7 -> freg_base+0..7 */
	struct perf_event *ev[3];   /* 覆盖 12 字节: 8 + 4 */
	uint64_t ev_addr[3];
	uint32_t ev_len[3];
	bool wide[3];
	bool active;
	bool ro;            /* 目标地址所在 VMA 不可写: 只观察, 不回写内存 */
};

struct wrpbp_vecmod {
	bool enabled;
	u32 mask;
	u32 x, y, z;
	u32 freg_mask;                     /* 本次要不要写浮点寄存器 */
	u32 freg_val[CLONEBP_FREG_MAX];    /* float 位模式 */
};

static struct wrpbp_slot wrpbp_slots[BP_SLOT_MAX];
static struct wrpbp_vecmod wrpbp_vec[BP_SLOT_MAX];
static HWBP_RECORD wrpbp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 wrpbp_head[BP_SLOT_MAX];
static u32 wrpbp_count[BP_SLOT_MAX];
static pid_t wrpbp_tgid;
static struct task_struct *wrpbp_task;   /* 持有引用, 观察点存活期有效 */
static DEFINE_PER_CPU(int, wrpbp_busy);

/* 诊断计数 (只读诊断, 竞态无害) */
static u32 g_wrpbp_hits, g_wrpbp_writes, g_wrpbp_skips, g_wrpbp_fails;
static u32 g_wrpbp_skipped;   /* 因卡写修复被跳过的 store 条数 */
static u64 g_wrpbp_last_hit_pc;
static u32 g_wrpbp_last_rx, g_wrpbp_last_ry, g_wrpbp_last_rz;

static void bpc_note_regs(u32 x, u32 y, u32 z)
{
	g_wrpbp_last_rx = x;
	g_wrpbp_last_ry = y;
	g_wrpbp_last_rz = z;
}

/* 命中后把目标向量写回: 只走 nofault uaccess, 断点上下文里不能睡眠。
 * 返回 1 表示本次确实向目标地址写入了新值(调用方据此决定是否跳过 store),
 * 返回 0 表示没写(只读 VMA / 读失败 / 已是目标值)。
 */
static int wrpbp_apply_vec(int slot)
{
	struct wrpbp_vecmod *v = &wrpbp_vec[slot];
	struct wrpbp_slot *sl = &wrpbp_slots[slot];
	u32 buf[3];
	u32 mask = v->mask ? v->mask : HWBP_MASK_ALL;
	int ret = -EFAULT;

	if (!mask)
		return 0;
	/* 只读 VMA: 写回去必然失败, 直接跳过内存通道(寄存器通道仍可用) */
	if (sl->ro)
		return 0;

	if (wind_copy_from_user_nofault(buf, (void __user *)sl->watch_addr,
					WRPBP_VEC_BYTES) != 0)
		goto out;

	/* 已经是目标值就不写: 少一次写回就少一次被观察点抓到的机会 */
	if ((!(mask & 0x1) || buf[0] == v->x) &&
	    (!(mask & 0x2) || buf[1] == v->y) &&
	    (!(mask & 0x4) || buf[2] == v->z)) {
		g_wrpbp_skips++;
		return 0;
	}

	if (mask & 0x1)
		buf[0] = v->x;
	if (mask & 0x2)
		buf[1] = v->y;
	if (mask & 0x4)
		buf[2] = v->z;

	ret = (int)wind_copy_to_user_nofault((void __user *)sl->watch_addr, buf,
					     WRPBP_VEC_BYTES);
out:
	if (ret == 0) {
		g_wrpbp_writes++;
		bpc_note_regs(v->x, v->y, v->z);
		return 1;
	}
	g_wrpbp_fails++;
	return 0;
}

/* 浮点寄存器写回: 内存改写对"先搬进 S 寄存器再算"的代码无效, 这里补一条
 * 寄存器通道。与 clonebp 走同一套 fpsimd 机制:
 *   先 preserve 把硬件状态落到 thread.uw.fpsimd_state,
 *   再改 vregs[n] 低 32 位, 最后置 TIF_FOREIGN_FPSTATE 让返回用户态时重载。 */
static void wrpbp_apply_fregs(int slot)
{
	struct wrpbp_vecmod *v = &wrpbp_vec[slot];
	struct wrpbp_slot *sl = &wrpbp_slots[slot];
	unsigned long fl;
	int k;

	/* 只认运行时的 v->freg_mask: 安装期的 sl->freg_mask 只是"允许有这个通道",
	 * 真正写哪些槽位必须以每次 set_fregs 传的为准。 */
	if (!v->freg_mask)
		return;
	if (!g_wind_fp_writeback)
		return;

	/* perf 溢出回调里 fpsimd_preserve_current_state() 内部有
	 * WARN_ON(preemptible()), Android 默认 panic_on_warn, 一命中就重启。
	 * 先把抢占关掉再进。 */
	preempt_disable();
	if (!test_thread_flag(TIF_FOREIGN_FPSTATE)) {
		if (!wind_fpsimd_preserve_current_state()) {
			preempt_enable();
			return;
		}
	}

	local_irq_save(fl);
	for (k = 0; k < CLONEBP_FREG_MAX; ++k) {
		int rn;
		u32 *p;

		if (!(v->freg_mask & (1u << k)))
			continue;
		rn = (int)sl->freg_base + k;
		if (rn < 0 || rn > 31)
			continue;
		p = (u32 *)&current->thread.uw.fpsimd_state.vregs[rn];
		p[0] = v->freg_val[k];
	}
	set_thread_flag(TIF_FOREIGN_FPSTATE);
	local_irq_restore(fl);
	preempt_enable();
}

/* 每秒最多写回这么多次, 超了就暂停该槽位的写回 */
static unsigned long wrpbp_rate_last[BP_SLOT_MAX];
static atomic_t wrpbp_rate_cnt[BP_SLOT_MAX];
static bool wrpbp_rate_warned[BP_SLOT_MAX];

static void wrpbp_triggered(struct perf_event *bp, struct perf_sample_data *data,
			    struct pt_regs *regs)
{
	int i, j;
	HWBP_RECORD rec;

	if (READ_ONCE(g_wind_unloading))
		return;
	if (wrpbp_tgid && current->tgid != wrpbp_tgid)
		return;
	/* 内核上下文里的写基本就是我们自己的写回, 直接放过, 不递归 */
	if (!user_mode(regs))
		return;
	if (this_cpu_read(wrpbp_busy))
		return;

	for (i = 0; i < BP_SLOT_MAX; i++) {
		if (!wrpbp_slots[i].active)
			continue;
		for (j = 0; j < 3; j++) {
			if (wrpbp_slots[i].ev[j] != bp)
				continue;

			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = instruction_pointer(regs);
			rec.hit_addr = data ? data->addr : wrpbp_slots[i].watch_addr;
			rec.op_mask = 0x2; /* 写命中 */
			bp_rec_push(wrpbp_recs[i], sizeof(HWBP_RECORD),
				    &wrpbp_head[i], &wrpbp_count[i], &rec);
			g_wrpbp_hits++;
			g_wrpbp_last_hit_pc = rec.trigger_pc;

			if (wrpbp_vec[i].enabled) {
				unsigned long now = jiffies;
				int wrote = 0;

				if (!wrpbp_rate_last[i] ||
				    time_after(now, wrpbp_rate_last[i] + HZ)) {
					wrpbp_rate_last[i] = now;
					atomic_set(&wrpbp_rate_cnt[i], 0);
				}
				if (atomic_inc_return(&wrpbp_rate_cnt[i]) >
				    WRPBP_RATE_MAX) {
					wrpbp_vec[i].enabled = false;
					wrpbp_vec[i].mask = 0;
					if (!wrpbp_rate_warned[i]) {
						wrpbp_rate_warned[i] = true;
						pr_warn("wrpbp: slot %d 命中频率超限(%d/s), "
							"已关闭该槽写回 地址=%llx\n",
							i, WRPBP_RATE_MAX,
							(unsigned long long)
							wrpbp_slots[i].watch_addr);
					}
				} else {
					this_cpu_inc(wrpbp_busy);
					wrote = wrpbp_apply_vec(i);
					wrpbp_apply_fregs(i);
					this_cpu_dec(wrpbp_busy);
				}

				/* 卡写修复: 写回成功后跳过触发的 store。
				 * 否则 CPU 返回后会重试这条 store, 把刚写的值覆盖掉,
				 * 并再次命中同一 watchpoint, 无限循环把线程卡死。
				 * A64 指令定长 4 字节, 推进 pc 即跳过;
				 * 32 位 compat 任务是 2/4 字节混排, 不推进(退回旧行为)。 */
				if (wrote && g_wrp_skip_store && !is_compat_task()) {
					unsigned long pc = instruction_pointer(regs);

					if (wrpbp_skip_last[i] &&
					    time_after(jiffies,
						       wrpbp_skip_last[i] + HZ)) {
						wrpbp_skip_last[i] = jiffies;
						wrpbp_skip_cnt[i] = 0;
					}
					wrpbp_skip_last[i] = jiffies;

					if (++wrpbp_skip_cnt[i] <= WRPBP_SKIP_MAX) {
						wrpbp_skip_pc[i] = pc;
						instruction_pointer_set(regs, pc + 4);
						g_wrpbp_skipped++;
					} else if (!wrpbp_rate_warned[i]) {
						/* 跳得过密说明页面被当寄存器狂写,
						 * 退回旧行为比把指令流跳乱安全 */
						wrpbp_rate_warned[i] = true;
						pr_warn("wrpbp: slot %d 跳过率超限(%d/s), "
							"停止跳 store 地址=%llx\n",
							i, WRPBP_SKIP_MAX,
							(unsigned long long)
							wrpbp_slots[i].watch_addr);
					}
				}
			}
			return;
		}
	}
}

static void wrpbp_clear_slot(int i)
{
	int j;

	for (j = 0; j < 3; j++) {
		if (!wrpbp_slots[i].ev[j])
			continue;
		if (wrpbp_slots[i].wide[j])
			wind_unregister_wide_hw_breakpoint(
				(struct perf_event * __percpu *)wrpbp_slots[i].ev[j]);
		else
			wind_unregister_hw_breakpoint(wrpbp_slots[i].ev[j]);
		wrpbp_slots[i].ev[j] = NULL;
	}
	wrpbp_slots[i].active = false;
	wrpbp_slots[i].ro = false;
}

/* 返回 1 = 地址整段落在某个 VMA 里, 0 = 根本没映射。
 * writable 带回该 VMA 是否可写 —— 不可写时只能观察不能回写。 */
static int wrpbp_addr_probe(struct mm_struct *mm, uint64_t addr, u32 len,
			    bool *writable, unsigned long *vstart,
			    unsigned long *vend, unsigned long *vflags)
{
	struct vm_area_struct *vma;

	*writable = false;
	*vstart = *vend = *vflags = 0;

	if (!mm || !len)
		return 0;
	if (addr < 0x1000ULL || addr >= 0x0001000000000000ULL)
		return 0;
	if (addr + len < addr)
		return 0;

	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)addr);
	if (vma && (unsigned long)addr >= vma->vm_start &&
	    (unsigned long)addr + len <= vma->vm_end) {
		*writable = !!(vma->vm_flags & VM_WRITE);
		*vstart = vma->vm_start;
		*vend = vma->vm_end;
		*vflags = vma->vm_flags;
		mmap_read_unlock(mm);
		return 1;
	}
	mmap_read_unlock(mm);
	return 0;
}

int wrpbp_set(struct task_struct *task, const WRPBP_POINT_CONFIG *points,
	      uint32_t point_count)
{
	struct mm_struct *mm = NULL;
	int i, ret = 0;

	if (!task || !points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;
	if (point_count > WRPBP_MAX_POINTS) {
		pr_err("wrpbp: 一次最多 %d 个观察点 (arm64 只有 4 个断点寄存器), "
		       "本次请求 %u 个, 只装前 %d 个\n",
		       WRPBP_MAX_POINTS, point_count, WRPBP_MAX_POINTS);
		point_count = WRPBP_MAX_POINTS;
	}

	/* 陀螺仪占着调试寄存器时就别再装追踪点 */
	if (gyro_holds_breakpoints()) {
		pr_err("wrpbp: 陀螺仪已占用硬件断点, 拒绝安装追踪点 (先关陀螺仪自瞄)\n");
		return -EBUSY;
	}

	wrpbp_remove();
	wrpbp_tgid = task->tgid;
	wrpbp_task = get_task_struct(task);

	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto fail;
	}

	for (i = 0; i < (int)point_count; i++) {
		struct wrpbp_slot *sl = &wrpbp_slots[i];
		u32 hwtype = bp_type_to_hw(points[i].type ? points[i].type
							 : BP_TYPE_WRITE);
		u32 len = points[i].watch_len ? points[i].watch_len : 4;
		u32 off;
		int j, n_ev = 0;

		if (hwtype != HW_BREAKPOINT_W && hwtype != HW_BREAKPOINT_RW) {
			ret = -EINVAL;
			goto fail;
		}
		if (bp_len_to_hw(len) < 0) {
			ret = -EINVAL;
			goto fail;
		}
		{
			bool w = false, mapped;
			unsigned long vs = 0, ve = 0, vf = 0;

			mapped = wrpbp_addr_probe(mm, points[i].watch_addr,
						  WRPBP_VEC_BYTES, &w,
						  &vs, &ve, &vf);
			if (!mapped) {
				pr_err("wrpbp: point %d addr=%llx 在 pid=%d(%s) "
				       "里没有映射, 拒绝安装\n", i,
				       (unsigned long long)points[i].watch_addr,
				       task->tgid, task->comm);
				ret = -EFAULT;
				goto fail;
			}
			if (!w) {
				sl->ro = true;
				pr_warn("wrpbp: point %d addr=%llx 落在只读 VMA "
				       "[%lx-%lx) flags=%lx, 只观察不回写\n", i,
				       (unsigned long long)points[i].watch_addr,
				       vs, ve, vf);
			}
		}

		for (j = 0, off = 0; j < WRPBP_EV_PER_SLOT && off < WRPBP_VEC_BYTES; j++) {
			struct perf_event_attr attr;
			uint64_t addr;
			u32 cur_len = 0;
			void *ev;
			bool wide = false;
			u32 rest = WRPBP_VEC_BYTES - off;

			addr = points[i].watch_addr + off;
			if (rest >= 8 && j == 0 && len >= 8)
				cur_len = 8;
			else if (rest >= 4)
				cur_len = 4;
			else
				break;

			hw_breakpoint_init(&attr);
			attr.bp_addr = addr;
			attr.bp_len = bp_len_to_hw(cur_len);
			attr.bp_type = hwtype;

			/* 必须按任务注册, 不退回 wide 版 (wide 是每核每任务
			 * 都进异常, 用户地址会拖死整机)。 */
			ev = wind_register_user_hw_breakpoint(&attr,
							      wrpbp_triggered,
							      NULL, task);
			if (IS_ERR(ev)) {
				ret = PTR_ERR(ev);
				if (ret == -EOPNOTSUPP)
					pr_err("wrpbp: register_user_hw_breakpoint "
					       "解析不到 (内核未导出该符号)\n");
				else
					pr_err("wrpbp: point %d ev%d 注册失败 %d\n",
					       i, j, ret);
				goto fail;
			}

			sl->ev[j] = ev;
			sl->wide[j] = wide;
			sl->ev_addr[j] = addr;
			sl->ev_len[j] = cur_len;
			n_ev++;
			off += cur_len;
		}

		sl->watch_addr = points[i].watch_addr;
		sl->watch_len = len;
		sl->type = points[i].type ? points[i].type : BP_TYPE_WRITE;
		sl->components = points[i].components ? points[i].components : HWBP_MASK_ALL;
		sl->freg_base = points[i].freg_base;
		sl->freg_mask = points[i].freg_mask;
		sl->active = true;
		wrpbp_head[i] = wrpbp_count[i] = 0;

		pr_info("wrpbp: armed slot=%d addr=%llx len=%u 观察点=%d tgid=%d\n",
		       i, (unsigned long long)sl->watch_addr, len, n_ev,
		       (int)wrpbp_tgid);
	}
	mmput(mm);
	return 0;

fail:
	if (mm)
		mmput(mm);
	wrpbp_remove();
	return ret;
}

int wrpbp_set_vec(int32_t point_index, u32 enable, u32 x, u32 y, u32 z, u32 mask)
{
	int i, lo = 0, hi = BP_SLOT_MAX - 1;

	if (point_index >= BP_SLOT_MAX)
		return -EINVAL;
	if (point_index >= 0)
		lo = hi = point_index;

	for (i = lo; i <= hi; i++) {
		wrpbp_vec[i].enabled = enable != 0;
		wrpbp_vec[i].mask = mask ? mask : HWBP_MASK_ALL;
		wrpbp_vec[i].x = x;
		wrpbp_vec[i].y = y;
		wrpbp_vec[i].z = z;
		wrpbp_rate_warned[i] = false;
		wrpbp_rate_last[i] = 0;
		wrpbp_skip_pc[i] = 0;
		atomic_set(&wrpbp_rate_cnt[i], 0);
	}
	return 0;
}

int wrpbp_set_fregs(int32_t point_index, u32 freg_base, u32 freg_mask,
		    const u32 *vals, u32 count)
{
	int i, lo = 0, hi = BP_SLOT_MAX - 1;

	if (point_index >= BP_SLOT_MAX)
		return -EINVAL;
	if (count > CLONEBP_FREG_MAX)
		count = CLONEBP_FREG_MAX;
	if (point_index >= 0)
		lo = hi = point_index;

	for (i = lo; i <= hi; i++) {
		u32 k;

		wrpbp_slots[i].freg_base = freg_base;
		wrpbp_vec[i].freg_mask = freg_mask;
		for (k = 0; k < count; ++k)
			wrpbp_vec[i].freg_val[k] = vals ? vals[k] : 0;
	}
	return 0;
}

int wrpbp_get_config(WRPBP_CONFIG *out)
{
	int i;
	u32 active = 0;

	if (!out)
		return -EINVAL;
	for (i = 0; i < BP_SLOT_MAX; i++)
		if (wrpbp_slots[i].active)
			active++;

	memset(out, 0, sizeof(*out));
	out->active_count = active;
	for (i = 0; i < BP_SLOT_MAX; i++) {
		if (!wrpbp_slots[i].active)
			continue;
		out->watch_addr_lo = (u32)(wrpbp_slots[i].watch_addr & 0xffffffffU);
		out->watch_addr_hi = (u32)(wrpbp_slots[i].watch_addr >> 32);
		break;
	}
	out->skipped = g_wrpbp_skipped;
	out->writes  = g_wrpbp_writes;
	out->fails   = g_wrpbp_fails;
	return 0;
}

int wrpbp_get_records(uint32_t point_index, uint32_t start_index,
		      HWBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n;

	if (point_index >= BP_SLOT_MAX || !records || !count_out)
		return -EINVAL;
	n = bp_rec_read(wrpbp_recs[point_index], sizeof(HWBP_RECORD),
			wrpbp_head[point_index], wrpbp_count[point_index],
			start_index, records, capacity);
	*count_out = n;
	return 0;
}

int wrpbp_clear_records(int32_t point_index)
{
	int i, lo = 0, hi = BP_SLOT_MAX - 1;

	if (point_index >= BP_SLOT_MAX)
		return -EINVAL;
	if (point_index >= 0)
		lo = hi = point_index;

	for (i = lo; i <= hi; i++) {
		wrpbp_head[i] = 0;
		wrpbp_count[i] = 0;
	}
	return 0;
}

int wrpbp_remove(void)
{
	int i;

	for (i = 0; i < BP_SLOT_MAX; i++) {
		wrpbp_clear_slot(i);
		memset(&wrpbp_vec[i], 0, sizeof(wrpbp_vec[i]));
		wrpbp_rate_warned[i] = false;
		wrpbp_rate_last[i] = 0;
		atomic_set(&wrpbp_rate_cnt[i], 0);
	}
	wrpbp_tgid = 0;
	if (wrpbp_task) {
		put_task_struct(wrpbp_task);
		wrpbp_task = NULL;
	}
	return 0;
}

int wrpbp_set_pid(uint32_t pid_arg, const void __user *ureq)
{
	struct task_struct *task;
	WRPBP_POINT_CONFIG *kpts;
	uint32_t pid = pid_arg;
	uint32_t count;
	const void __user *points;
	int ret;

	if (!ureq)
		return -EINVAL;
	if (copy_from_user(&pid, (const void __user *)ureq, sizeof(pid)))
		return -EFAULT;
	if (copy_from_user(&count, (const void __user *)((uintptr_t)ureq + 4),
			   sizeof(count)))
		return -EFAULT;
	points = (const void __user *)((uintptr_t)ureq + 8);
	if (count == 0 || count > WRPBP_MAX_POINTS)
		return -EINVAL;

	kpts = kcalloc(count, sizeof(*kpts), GFP_KERNEL);
	if (!kpts)
		return -ENOMEM;
	if (copy_from_user(kpts, points, count * sizeof(*kpts))) {
		kfree(kpts);
		return -EFAULT;
	}

	rcu_read_lock();
	task = find_task_by_vpid((pid_t)pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task) {
		kfree(kpts);
		return -ESRCH;
	}

	ret = wrpbp_set(task, kpts, count);
	put_task_struct(task);
	kfree(kpts);
	return ret;
}

int wrpbp_module_init(void)
{
	memset(wrpbp_slots, 0, sizeof(wrpbp_slots));
	memset(wrpbp_vec, 0, sizeof(wrpbp_vec));
	wrpbp_tgid = 0;
	wrpbp_task = NULL;
	return 0;
}

void wrpbp_module_exit(void)
{
	if (READ_ONCE(g_wind_unloading))
		wrpbp_remove();
}
