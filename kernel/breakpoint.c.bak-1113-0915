/*
 * breakpoint.c — 五套断点框架
 *   1. hwbp  : BRP/WRP debug 寄存器硬件断点 (perf hw_breakpoint), 16 点 + 记录
 *   2. ptebp : PTE 影子页执行断点 (改 PTE_UXN 触发 fault), 16 点 + 记录, 不用 debug 寄存器
 *   3. brkbp : BRK 软断点 (改写指令字节), 16 点 + 记录
 *   5. udpbp : UDP 断点 (kprobe ip_local_out / __udp4_lib_rcv), 按 pid/端口/方向过滤,
 *              可选改写载荷。全部走 kprobe, 不动 debug 寄存器/页表/目标指令。
 *
 * 依赖: CONFIG_HAVE_HW_BREAKPOINT + CONFIG_PERF_EVENTS (GKI 默认开)。
 * 白名单外符号 (register_user_hw_breakpoint / unregister_hw_breakpoint /
 * do_mem_abort / do_page_fault / brk_handler 等) 一律经 sym.c 运行时解析,
 * 编译期不产生 CRC 引用。
 *
 * 未验证项 (版本相关, 需真机核对):
 *   - ptebp/brkbp 的 fault/异常捕获经 kprobe hook 内核 fault 处理函数,
 *     这些函数可能受 NOKPROBE 保护, register_kprobe 会失败 → set 返回
 *     -EOPNOTSUPP 并打印候选符号; 可用替代是 VBAR_EL1 向量表改写(未实现)。
 *   - ptebp 命中后恢复 PTE 属一次性断点; 透明持续命中需单步(MDSCR_EL1.SS
 *     + hook 单步异常), 受 NOKPROBE 限制, 未完整实现。
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
#include <linux/kprobes.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/fpsimd.h>
#include <linux/thread_info.h>
#include "breakpoint.h"
#include "sym.h"
#include "memory.h"
#include "compat.h"
#include "clonebp.h"   /* clonebp_get_diag: 填 clonebp_* 诊断字段 */

#define BP_SLOT_MAX       16
#define BP_REC_PER_SLOT   64

/* ===================================================================
 * 通用环形记录
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

/* 从环形读 [start_index, start_index+max) 条, 返回实际条数 */
static u32 bp_rec_read(const void *recs, size_t rec_size, u32 head, u32 count,
		       u32 start_index, void *out, u32 max)
{
	char *base = (char *)recs;
	u32 i, n = 0;

	if (start_index >= count || max == 0)
		return 0;

	for (i = start_index; i < count && n < max; i++)
	{
		/* 最旧一条在 (head - count + BP_REC_PER_SLOT) % BP_REC_PER_SLOT */
		u32 oldest = (head - count + BP_REC_PER_SLOT) % BP_REC_PER_SLOT;
		u32 idx = (oldest + i) % BP_REC_PER_SLOT;
		memcpy((char *)out + n * rec_size, base + idx * rec_size, rec_size);
		n++;
	}
	return n;
}

/* 单条写回: record_index 相对最旧 */
static bool bp_rec_set(void *recs, size_t rec_size, u32 head, u32 count,
		       u32 record_index, const void *rec)
{
	char *base = recs;

	if (record_index >= count)
		return false;
	{
		u32 oldest = (head - count + BP_REC_PER_SLOT) % BP_REC_PER_SLOT;
		u32 idx = (oldest + record_index) % BP_REC_PER_SLOT;
		memcpy(base + idx * rec_size, rec, rec_size);
	}
	return true;
}

/* ===================================================================
 * 三族断点运行期计数 (OP_BP_DIAG 读出去, 用于区分「没装上 / 装上了没命中 /
 * 命中了但写回失败」这三种完全不同的故障)
 * =================================================================== */
struct bp_counters
{
	u32 hwbp_hits, hwbp_writes, hwbp_write_fails;
	u32 ptebp_reloads, ptebp_heal_ok, ptebp_heal_fail;
	u32 wrpbp_hits, wrpbp_writes, wrpbp_skips, wrpbp_fails;
	u64 last_hit_pc;
	u32 last_rx, last_ry, last_rz;
};

static struct bp_counters g_bpc;
static void bpc_note_regs(u32 x, u32 y, u32 z)
{
	g_bpc.last_rx = x;
	g_bpc.last_ry = y;
	g_bpc.last_rz = z;
}

/* 类型/长度映射 */
static int bp_type_to_hw(int type)
{
	switch (type)
	{
	case BP_TYPE_EXEC:   return HW_BREAKPOINT_X;
	case BP_TYPE_READ:   return HW_BREAKPOINT_R;
	case BP_TYPE_WRITE:  return HW_BREAKPOINT_W;
	case BP_TYPE_RW:     return HW_BREAKPOINT_RW;
	default:             return -1;
	}
}

static int bp_len_to_hw(int len)
{
	switch (len)
	{
	case 1: return HW_BREAKPOINT_LEN_1;
	case 2: return HW_BREAKPOINT_LEN_2;
	case 4: return HW_BREAKPOINT_LEN_4;
	case 8: return HW_BREAKPOINT_LEN_8;
	default: return -1;
	}
}

/* 2-bit 寄存器操作掩码: 观察点按读写类型; 执行断点基础解码(完整需反汇编引擎) */
static uint32_t bp_op_mask(uint32_t bp_type, unsigned long pc)
{
	(void)pc;
	switch (bp_type)
	{
	case BP_TYPE_READ:   return 0x1; /* bit0 读 */
	case BP_TYPE_WRITE:  return 0x2; /* bit1 写 */
	case BP_TYPE_RW:     return 0x3;
	default:             return 0;   /* 执行断点: 寄存器级追踪未实现 */
	}
}

/* ===================================================================
 * 1. hwbp — BRP/WRP 硬件断点
 * =================================================================== */
struct hwbp_slot
{
	void *bp;      /* per-CPU 数组 或 单任务 event, 由 wide 区分 */
	bool wide;
	uint64_t addr;
	uint32_t type;
	uint32_t length;
	bool active;
};

/* 命中时改写 v3/v4/v5 (对齐 DUJIA/GTV 语义): rx/ry/rz 为 float 位模式 */
struct hwbp_regmod
{
	bool enabled;
	u32 mask;   /* bit0=s3(x) bit1=s4(y) bit2=s5(z), 0 视为 0x7 */
	u32 rx, ry, rz;
};

#define HWBP_MASK_ALL 0x7U

static struct hwbp_regmod hwbp_regmod[BP_SLOT_MAX];
/* 断点绑的是 VA: per-CPU 全域断点会命中任何映射到同址的进程,
 * 命中侧按 tgid 过滤, 只认 bind 的那个进程。 */
static pid_t hwbp_tgid;

/* 命中改写频率闸门: 老实现按「目标角没变就不写」去重, 那是错的 ——
 * 游戏每帧都会把 s3/s4/s5 重算成自己的准心, 只在角度变化时写一次,
 * 之后每一发用的还是游戏自己的值, 现象就是「改了但弹道没变」。
 * 现在默认每次命中都写 (reg_min_ms=0), 需要省电再调大这个参数。 */
static u32 hwbp_last_regs[BP_SLOT_MAX][3];
static unsigned long hwbp_last_regs_jiffies[BP_SLOT_MAX];
static bool hwbp_fp_warned;

static int reg_min_ms;
module_param(reg_min_ms, int, 0644);
MODULE_PARM_DESC(reg_min_ms, "命中改写最小间隔毫秒, 0=每次命中都写(默认)");

/* 符号解析不到 fpsimd_preserve_current_state 时的直写通道 */
#if defined(CONFIG_ARM64)
/* 断点溢出上下文里 q3/q4/q5 就是用户态当前值: 直接改它们的低 32 位
 * (s3/s4/s5) 等价于 HookPTE 的 v3/v4/v5 改写, 不依赖任何内核符号。
 * 内核按 -mgeneral-regs-only 编译, 汇编器拒收 fp 助记符 (clang 报
 * "instruction requires: fp-armv8"), 所以这里用 .inst 写死编码。
 * 编码由 GNU as 实测得到:
 *   stp q3, q4, [x9] = 0xad001123    str q5, [x9] = 0x3d800125
 *   ldp q3, q4, [x9] = 0xad401123    ldr q5, [x9] = 0x3dc00125 */
static bool hwbp_fp_reg_direct(const struct hwbp_regmod *cfg)
{
	u64 q34[4] __attribute__((aligned(16)));
	u64 q5[2] __attribute__((aligned(16)));
	volatile u64 *v34 = q34;
	volatile u64 *v5 = q5;
	u32 mask = cfg->mask ? cfg->mask : HWBP_MASK_ALL;

	{
		register u64 *p asm("x9") = q34;

		asm volatile(".inst 0xad001123" : "+r"(p) :: "memory"); /* stp q3, q4, [x9] */
	}
	{
		register u64 *p asm("x9") = q5;

		asm volatile(".inst 0x3d800125" : "+r"(p) :: "memory"); /* str q5, [x9] */
	}

	if (mask & 0x1)
		v34[0] = (v34[0] & ~0xffffffffULL) | (u64)cfg->rx;
	if (mask & 0x2)
		v34[2] = (v34[2] & ~0xffffffffULL) | (u64)cfg->ry;
	if (mask & 0x4)
		v5[0] = (v5[0] & ~0xffffffffULL) | (u64)cfg->rz;

	{
		register u64 *p asm("x9") = q34;

		asm volatile(".inst 0xad401123" : "+r"(p) :: "memory"); /* ldp q3, q4, [x9] */
	}
	{
		register u64 *p asm("x9") = q5;

		asm volatile(".inst 0x3dc00125" : "+r"(p) :: "memory"); /* ldr q5, [x9] */
	}
	return true;
}
#else
static bool hwbp_fp_reg_direct(const struct hwbp_regmod *cfg)
{
	(void)cfg;
	return false;
}
#endif

static void hwbp_apply_fp_regs(int slot, const struct hwbp_regmod *cfg)
{
	u32 *v;
	unsigned long flags, now;
	u32 mask = cfg->mask ? cfg->mask : HWBP_MASK_ALL;

	now = jiffies;
	if (reg_min_ms > 0 && hwbp_last_regs_jiffies[slot] &&
	    time_before(now, hwbp_last_regs_jiffies[slot] +
			     msecs_to_jiffies(reg_min_ms)))
		return;

	if (!wind_fpsimd_preserve_current_state()) {
		if (hwbp_fp_reg_direct(cfg)) {
			g_bpc.hwbp_writes++;
			bpc_note_regs(cfg->rx, cfg->ry, cfg->rz);
			hwbp_last_regs_jiffies[slot] = now;
			return;
		}
		g_bpc.hwbp_write_fails++;
		if (!hwbp_fp_warned) {
			hwbp_fp_warned = true;
			printk(KERN_ERR "hwbp: FP 状态既同步不了也直写不了, 准心改写不会生效\n");
		}
		return;
	}

	/* 同步路径: 把当前 FP 状态落进 thread.uw.fpsimd_state, 改 s3/s4/s5,
	 * 标记 FOREIGN, 返回用户态时内核按内存重载 → 改写真正落到寄存器 */
	local_irq_save(flags);
	if (mask & 0x1) {
		v = (u32 *)&current->thread.uw.fpsimd_state.vregs[3];
		v[0] = cfg->rx;
	}
	if (mask & 0x2) {
		v = (u32 *)&current->thread.uw.fpsimd_state.vregs[4];
		v[0] = cfg->ry;
	}
	if (mask & 0x4) {
		v = (u32 *)&current->thread.uw.fpsimd_state.vregs[5];
		v[0] = cfg->rz;
	}
	/* 只改了 FP 状态内存副本, 必须让返回用户态时按内存重载, 否则改的是
	 * 一份没人读的副本 —— 这是「改了但弹道没变」的老坑之一 */
	set_thread_flag(TIF_FOREIGN_FPSTATE);
	local_irq_restore(flags);

	g_bpc.hwbp_writes++;
	bpc_note_regs(cfg->rx, cfg->ry, cfg->rz);
	hwbp_last_regs_jiffies[slot] = now;
}

int hwbp_set_reg_modify(int point_index, u32 enable, u32 rx, u32 ry, u32 rz,
			u32 mask)
{
	int i, lo = 0, hi = BP_SLOT_MAX - 1;

	if (point_index >= BP_SLOT_MAX)
		return -EINVAL;
	if (point_index >= 0)
		lo = hi = point_index;

	for (i = lo; i <= hi; i++)
	{
		hwbp_regmod[i].enabled = (enable != 0);
		hwbp_regmod[i].mask = mask ? mask : HWBP_MASK_ALL;
		hwbp_regmod[i].rx = rx;
		hwbp_regmod[i].ry = ry;
		hwbp_regmod[i].rz = rz;
	}
	return 0;
}

static struct hwbp_slot hwbp_slots[BP_SLOT_MAX];
static HWBP_RECORD hwbp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 hwbp_head[BP_SLOT_MAX];
static u32 hwbp_count[BP_SLOT_MAX];
/* 每个槽位头几次命中打一行 dmesg: 「装了但零命中」和「命中了但数据不对」
 * 是两种完全不同的故障, 没有这行日志只能靠猜 */
static u32 hwbp_prints[BP_SLOT_MAX];

static int bp_dbg = 1;
module_param(bp_dbg, int, 0644);
MODULE_PARM_DESC(bp_dbg, "1=每个断点槽位头 3 次命中打 dmesg(默认) 0=关");

static void hwbp_triggered(struct perf_event *bp, struct perf_sample_data *data,
			   struct pt_regs *regs)
{
	int i;
	HWBP_RECORD rec;

	if (hwbp_tgid && current->tgid != hwbp_tgid)
		return;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		/* wide 断点的回调给的是 per-CPU 事件, 不是注册时拿到的数组指针,
		 * 只能按断点地址认领槽位。 */
		if (hwbp_slots[i].active && hwbp_slots[i].addr == bp->attr.bp_addr)
		{
			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = instruction_pointer(regs);
			rec.hit_addr = data ? data->addr : hwbp_slots[i].addr;
			rec.op_mask = bp_op_mask(hwbp_slots[i].type, rec.trigger_pc);
			bp_rec_push(hwbp_recs[i], sizeof(HWBP_RECORD),
				    &hwbp_head[i], &hwbp_count[i], &rec);
			g_bpc.hwbp_hits++;
			g_bpc.last_hit_pc = rec.trigger_pc;
			if (bp_dbg && hwbp_prints[i] < 3) {
				hwbp_prints[i]++;
				printk(KERN_INFO "hwbp: hit slot=%d pc=%lx addr=%lx n=%u\n",
				       i, (unsigned long)rec.trigger_pc,
				       (unsigned long)hwbp_slots[i].addr,
				       (unsigned)hwbp_count[i]);
			}
			if (hwbp_regmod[i].enabled &&
			    hwbp_slots[i].type == BP_TYPE_EXEC)
				hwbp_apply_fp_regs(i, &hwbp_regmod[i]);
			break;
		}
	}
}

static void hwbp_clear_slot(int i)
{
	if (hwbp_slots[i].bp) {
		if (hwbp_slots[i].wide)
			wind_unregister_wide_hw_breakpoint(
				(struct perf_event * __percpu *)hwbp_slots[i].bp);
		else
			wind_unregister_hw_breakpoint((struct perf_event *)hwbp_slots[i].bp);
		hwbp_slots[i].bp = NULL;
	}
	hwbp_slots[i].wide = false;
	hwbp_slots[i].active = false;
}

int hwbp_set(struct driver_session *sess, const HWBP_POINT_CONFIG *points,
	     size_t point_count)
{
	int i, hwtype, hwlen, ret;

	if (!sess || !sess->task)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	hwbp_remove();
	hwbp_tgid = sess->task->tgid;

	for (i = 0; i < (int)point_count; i++)
	{
		struct perf_event_attr attr;
		void *bp;
		bool wide = true;

		hwtype = bp_type_to_hw(points[i].type);
		hwlen = bp_len_to_hw(points[i].length ? points[i].length : BP_LEN_8);
		if (hwtype < 0 || hwlen < 0)
		{
			ret = -EINVAL;
			goto fail;
		}

		hw_breakpoint_init(&attr);
		attr.bp_addr = points[i].addr;
#if defined(CONFIG_ARM64)
		/* arm64 执行断点只接受 LEN_4, 传 8 直接 -EINVAL */
		if (hwtype == HW_BREAKPOINT_X)
			hwlen = HW_BREAKPOINT_LEN_4;
#endif
		attr.bp_len = hwlen;
		attr.bp_type = hwtype;

		/* 全域断点: 目标进程的准心计算跑在工作线程上, 绑主线程的
		 * 单任务断点基本不命中。命中侧按 tgid 过滤。 */
		bp = wind_register_wide_hw_breakpoint(&attr, hwbp_triggered, NULL);
		if (IS_ERR(bp) && PTR_ERR(bp) == -EOPNOTSUPP)
		{
			/* 内核解析不到 register_wide_hw_breakpoint 时退回单任务断点,
			 * 只能命中被绑线程, 但至少不是完全装不上。 */
			wide = false;
			bp = wind_register_user_hw_breakpoint(&attr, hwbp_triggered, NULL,
							     sess->task);
		}
		if (IS_ERR(bp))
		{
			ret = PTR_ERR(bp);
			printk(KERN_ERR "hwbp: register point %d failed %d\n", i, ret);
			goto fail;
		}

		hwbp_slots[i].bp = bp;
		hwbp_slots[i].wide = wide;
		hwbp_slots[i].addr = points[i].addr;
		hwbp_slots[i].type = points[i].type;
		hwbp_slots[i].length = points[i].length;
		hwbp_slots[i].active = true;
		hwbp_prints[i] = 0;
		printk(KERN_INFO "hwbp: armed slot=%d addr=%lx type=%d len=%d wide=%d tgid=%d\n",
		       i, (unsigned long)points[i].addr, points[i].type,
		       points[i].length, (int)wide, (int)hwbp_tgid);
	}
	return 0;

fail:
	hwbp_remove();
	return ret;
}

/* CPU 热插拔会让 wide 事件在掉线的那颗核上失效; perf 侧也可能因为
 * 别的子系统抢断点寄存器把事件停掉。这里把已装事件重新 enable 一遍,
 * 返回重新点亮的槽位数 —— 客户端每帧调一次都不亏。 */
int hwbp_refresh(void)
{
	int i, cpu, n = 0;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (!hwbp_slots[i].active || !hwbp_slots[i].bp)
			continue;

		if (hwbp_slots[i].wide)
		{
			struct perf_event * __percpu *arr = hwbp_slots[i].bp;

			for_each_possible_cpu(cpu)
			{
				struct perf_event *ev = *per_cpu_ptr(arr, cpu);

				if (ev)
					perf_event_enable(ev);
			}
		}
		else
		{
			perf_event_enable((struct perf_event *)hwbp_slots[i].bp);
		}
		n++;
	}
	return n;
}

int hwbp_remove(void)
{
	int i;
	for (i = 0; i < BP_SLOT_MAX; i++)
		hwbp_clear_slot(i);
	memset(hwbp_regmod, 0, sizeof(hwbp_regmod));
	memset(hwbp_last_regs, 0, sizeof(hwbp_last_regs));
	memset(hwbp_last_regs_jiffies, 0, sizeof(hwbp_last_regs_jiffies));
	hwbp_tgid = 0;
	return 0;
}

int hwbp_get_config(HWBP_CONFIG *config_out)
{
	int i;
	u32 active = 0;

	if (!config_out)
		return -EINVAL;
	for (i = 0; i < BP_SLOT_MAX; i++)
		if (hwbp_slots[i].active)
			active++;

	memset(config_out, 0, sizeof(*config_out));
	config_out->num_brps = 16;
	config_out->num_wrps = 16;
	config_out->active_count = active;
	return 0;
}

int hwbp_get_records(uint32_t point_index, uint32_t start_index,
		     HWBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n;

	if (point_index >= BP_SLOT_MAX || !records || !count_out)
		return -EINVAL;
	n = bp_rec_read(hwbp_recs[point_index], sizeof(HWBP_RECORD),
			hwbp_head[point_index], hwbp_count[point_index],
			start_index, records, capacity);
	*count_out = n;
	return 0;
}

int hwbp_set_record(uint32_t point_index, uint32_t record_index,
		    const HWBP_RECORD *record)
{
	if (point_index >= BP_SLOT_MAX || !record)
		return -EINVAL;
	if (!bp_rec_set(hwbp_recs[point_index], sizeof(HWBP_RECORD),
			hwbp_head[point_index], hwbp_count[point_index],
			record_index, record))
		return -ENOENT;
	return 0;
}

int hwbp_clear_records(int32_t point_index)
{
	int i;

	if (point_index >= 0)
	{
		if ((u32)point_index >= BP_SLOT_MAX)
			return -EINVAL;
		hwbp_head[point_index] = 0;
		hwbp_count[point_index] = 0;
		return 0;
	}
	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		hwbp_head[i] = 0;
		hwbp_count[i] = 0;
	}
	return 0;
}

/* ===================================================================
 * 2. ptebp — PTE 内联执行钩子 (真正能拽弹道的那条路)
 *
 * 老实现是「清 PTE_UXN -> 执行触发 permission fault -> 记录 -> 恢复 PTE」:
 * 命中一次就恢复, 不会再有第二次, 而且整条路没有寄存器写回通道, 所以既
 * 追不了弹道, 记录也只有第一发。
 *
 * 现在的做法与 HookPTE/DUJIA 一致 —— 用 PTE 通道把目标代码页改掉:
 *
 *   钩子点 A : b  T                (4 字节)
 *   跳板   T : ldr s3, [pc,#20]    T+0
 *              ldr s4, [pc,#16]    T+4
 *              ldr s5, [pc,#12]    T+8
 *              <A 处原指令>         T+12
 *              b  A+4              T+16
 *              float x, y, z       T+20..T+31
 *
 * 执行到 A 直接跳 T: 先把 s3/s4/s5 改成目标方向, 再跑原指令, 再回 A+4
 * 没有异常、没有 fault、每发都命中, 所以弹道必然按目标角走; 改目标角只
 * 需要重写 T+20 那 12 字节。
 *
 * 代价 (如实写清):
 *   - A 必须是 4 字节对齐的单条指令, 且不能是 PC 相对指令 (adr/adrp/b/bl/
 *     ldr-literal/cbz/tbz), 否则搬进跳板后语义变了 -> 直接拒绝装载
 *   - 跳板是目标 mm 里现开的一页 RWX (工作队列里 kthread_use_mm + vm_mmap),
 *     落点必须能被 b imm26 够到 (±128MB)
 * =================================================================== */
#define PTEBP_TRAMP_SZ   32
#define PTEBP_LIT_OFF    20
#define PTEBP_NOP        0xD503201FU

struct ptebp_page;

struct ptebp_slot
{
	uint64_t addr;        /* 钩子点 A */
	uint32_t orig_insn;   /* A 处原指令 */
	uint32_t hook_off;    /* A 在页内的偏移 */
	uint64_t tramp_pc;    /* 跳板 T 的用户 VA */
	uint64_t lit_pc;      /* 三个 float 立即数所在地址 */
	bool active;
	bool regmod;
	bool tramp_in_page;   /* 跳板就在影子页里 */
	struct ptebp_page *pg;/* 钩子点所在影子页 (引用计数共享) */
	struct ptebp_page *tpg;/* 跳板所在影子页 (可能同页, 也可能邻页) */
	u32 rx, ry, rz;
	u32 mask;
};

/* 同一页上的多个钩子点必须共用一份影子页: 每个点各 attach 一次的话,
 * 后一个点会影子到「已经含前一个点 b 指令」的页面上, 前一个点立刻失效
 * —— 这是多点多页场景下的真实 bug, 用引用计数共享修掉。 */
#define PTEBP_PAGE_MAX 8

struct ptebp_page
{
	uint64_t va;          /* 页基址 */
	struct wind_shadow sh;
	int refs;
};

static struct ptebp_page ptebp_pages[PTEBP_PAGE_MAX];
static struct ptebp_slot ptebp_slots[BP_SLOT_MAX];
static PTEBP_RECORD ptebp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 ptebp_head[BP_SLOT_MAX];
static u32 ptebp_count[BP_SLOT_MAX];
static struct kprobe *g_ptebp_kp;   /* 内联钩子不用 kprobe, 保留给退出清理 */
static struct mm_struct *g_ptebp_mm;
static int ptebp_shadow = 1;        /* 0=强制走 COW 写, 1=优先影子页 */
module_param(ptebp_shadow, int, 0644);

/* ---- 影子页表: 按页基址共享 ---- */
static struct ptebp_page *ptebp_page_get(struct mm_struct *mm, uint64_t va)
{
	uint64_t base = va & PAGE_MASK;
	int i, slot = -1;

	for (i = 0; i < PTEBP_PAGE_MAX; i++)
	{
		if (ptebp_pages[i].refs && ptebp_pages[i].va == base)
		{
			ptebp_pages[i].refs++;
			return &ptebp_pages[i];
		}
		if (!ptebp_pages[i].refs && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return NULL;

	if (wind_shadow_attach(mm, base, &ptebp_pages[slot].sh) != 0)
		return NULL;

	ptebp_pages[slot].va = base;
	ptebp_pages[slot].refs = 1;
	return &ptebp_pages[slot];
}

static void ptebp_page_put(struct ptebp_page *pg)
{
	if (!pg || !pg->refs)
		return;
	if (--pg->refs > 0)
		return;

	/* 最后一个引用撤掉时才把 PTE 换回原页 */
	if (g_ptebp_mm)
		wind_shadow_detach(g_ptebp_mm, pg->va, &pg->sh);
	memset(pg, 0, sizeof(*pg));
}

static void ptebp_pages_clear(void)
{
	int i;

	for (i = 0; i < PTEBP_PAGE_MAX; i++)
	{
		if (!ptebp_pages[i].refs)
			continue;
		ptebp_pages[i].refs = 1;
		ptebp_page_put(&ptebp_pages[i]);
	}
}

/* 这条指令搬进跳板后语义会变 (PC 相对), 一律拒绝 */
static bool ptebp_insn_pc_rel(u32 insn)
{
	if ((insn & 0x1F000000U) == 0x10000000U)   /* adr / adrp */
		return true;
	if ((insn & 0x7C000000U) == 0x14000000U)   /* b / bl */
		return true;
	if ((insn & 0x3B000000U) == 0x18000000U)   /* ldr (literal) */
		return true;
	if ((insn & 0x7E000000U) == 0x34000000U)   /* cbz / cbnz */
		return true;
	if ((insn & 0x7E000000U) == 0x36000000U)   /* tbz / tbnz */
		return true;
	return false;
}

/* ldr s<rt>, [pc, #delta]  (delta 相对该指令自身 PC) */
static u32 ptebp_ldr_s(u32 rt, u32 delta)
{
	return 0x1C000000U | (((delta >> 2) & 0x7FFFFU) << 5) | (rt & 0x1FU);
}

/* b <to>, 相对该指令自身 PC */
static u32 ptebp_b(u64 from, u64 to)
{
	s64 d = ((s64)to - (s64)from) >> 2;

	return 0x14000000U | ((u32)d & 0x03FFFFFFU);
}

/* 在一页的内核 VA 里找 PTEBP_TRAMP_SZ 字节空隙 (整段 nop 或 0) */
static u32 ptebp_find_cave(const u8 *kva, u32 hook_off, bool check_hook)
{
	u32 i, j;

	for (i = 0; i + PTEBP_TRAMP_SZ <= PAGE_SIZE; i += 4)
	{
		if (check_hook && i <= hook_off && hook_off < i + PTEBP_TRAMP_SZ)
			continue;
		for (j = 0; j < PTEBP_TRAMP_SZ; j += 4)
		{
			u32 w;

			memcpy(&w, kva + i + j, sizeof(w));
			if (w != PTEBP_NOP && w != 0)
				break;
		}
		if (j >= PTEBP_TRAMP_SZ)
			return i;
	}
	return 0xFFFFFFFFU;
}

/* 把三行 ldr + 原指令 + b 回 + 立即数拼成 32 字节跳板 */
static void ptebp_build_tramp(u32 *t, u64 tramp_pc, u64 hook_addr, u32 insn)
{
	memset(t, 0, PTEBP_TRAMP_SZ);
	t[0] = ptebp_ldr_s(3, PTEBP_LIT_OFF);
	t[1] = ptebp_ldr_s(4, PTEBP_LIT_OFF - 4);
	t[2] = ptebp_ldr_s(5, PTEBP_LIT_OFF - 8);
	t[3] = insn;
	t[4] = ptebp_b(tramp_pc + 16, hook_addr + 4);
}

/* 在候选页里给跳板找位置:
 *   0) 钩子点自己那页的空隙 (最不显眼)
 *   1) ±1 页、±2 页 (同 VMA 的可执行页, b 的 ±128MB 覆盖得下)
 *   2) 都不行 -> 目标 mm 里另开一页 RWX (最显眼, 兜底) */
static int ptebp_place_tramp(struct mm_struct *mm, struct ptebp_slot *sl)
{
	static const int delta_order[] = { 0, -1, 1, -2, 2 };
	int k;

	sl->tramp_in_page = false;

	for (k = 0; k < (int)ARRAY_SIZE(delta_order); k++)
	{
		uint64_t page_va = (sl->addr & PAGE_MASK) + (s64)delta_order[k] * PAGE_SIZE;
		struct ptebp_page *pg;
		u32 cave;

		/* 跳过空地址 / 内核态高地址 */
		if (page_va < PAGE_SIZE || page_va >= (1ULL << 47))
			continue;

		pg = ptebp_page_get(mm, page_va);
		if (!pg)
			continue;
		if (!pg->sh.exec_ok)
		{
			ptebp_page_put(pg);
			continue;
		}

		cave = ptebp_find_cave((const u8 *)pg->sh.kva,
				       (u32)(page_va == (sl->addr & PAGE_MASK) ?
					     sl->hook_off : 0xFFFFFFFFU),
				       page_va == (sl->addr & PAGE_MASK));
		if (cave == 0xFFFFFFFFU)
		{
			ptebp_page_put(pg);
			continue;
		}

		sl->tpg = pg;
		sl->tramp_pc = page_va + cave;
		sl->lit_pc = sl->tramp_pc + PTEBP_LIT_OFF;
		sl->tramp_in_page = true;
		return 0;
	}

	sl->tramp_pc = wind_map_exec_page_in(mm, sl->addr);
	if (!sl->tramp_pc)
		return -ENOMEM;
	sl->lit_pc = sl->tramp_pc + PTEBP_LIT_OFF;
	return 0;
}

int ptebp_set(struct driver_session *sess, const PTEBP_POINT_CONFIG *points,
	      size_t point_count)
{
	struct mm_struct *mm;
	int i, ret = 0;

	if (!sess || !sess->task)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	mm = get_task_mm(sess->task);
	if (!mm)
		return -ESRCH;

	ptebp_remove();
	g_ptebp_mm = mm;

	for (i = 0; i < (int)point_count; i++)
	{
		struct ptebp_slot *sl = &ptebp_slots[i];
		u32 insn = 0, tramp[PTEBP_TRAMP_SZ / 4], hook;

		if (points[i].addr & 3U)
		{
			ret = -EINVAL;
			printk(KERN_ERR "ptebp: addr 未 4 字节对齐 %llx\n",
			       (unsigned long long)points[i].addr);
			goto fail;
		}

		if (!wind_read_process_va(mm, points[i].addr, &insn, sizeof(insn)))
		{
			ret = -EFAULT;
			printk(KERN_ERR "ptebp: 读不到钩子点指令 %llx\n",
			       (unsigned long long)points[i].addr);
			goto fail;
		}

		if (ptebp_insn_pc_rel(insn))
		{
			ret = -EOPNOTSUPP;
			printk(KERN_ERR "ptebp: %llx 处 insn=%08x 是 PC 相对指令, "
				       "搬进跳板语义会变, 换个钩子点\n",
			       (unsigned long long)points[i].addr, insn);
			goto fail;
		}

		sl->addr = points[i].addr;
		sl->orig_insn = insn;
		sl->hook_off = (u32)(points[i].addr & (PAGE_SIZE - 1));

		/* 1. 钩子点所在页的影子副本 */
		if (ptebp_shadow)
			sl->pg = ptebp_page_get(mm, points[i].addr);
		if (ptebp_shadow && !sl->pg)
			printk(KERN_INFO "ptebp: %llx 影子页不可用 (CONTPTE/内存), "
					 "退回 COW 写\n",
			       (unsigned long long)points[i].addr);

		sl->tramp_pc = points[i].addr;
		sl->lit_pc = points[i].addr + PTEBP_LIT_OFF;

		/* 2. 跳板位置 */
		if (ptebp_place_tramp(mm, sl) != 0)
		{
			ret = -ENOMEM;
			printk(KERN_ERR "ptebp: 目标 mm 里要不到跳板位置 "
				       "(±2 页无空隙且 ±128MB 内没页)\n");
			goto fail;
		}

		/* 3. 写跳板 + 钩子点 */
		ptebp_build_tramp(tramp, sl->tramp_pc, points[i].addr, insn);

		if (sl->tramp_in_page)
		{
			u32 off = (u32)(sl->tramp_pc & (PAGE_SIZE - 1));

			memcpy((u8 *)sl->tpg->sh.kva + off, tramp, PTEBP_TRAMP_SZ);
			wind_flush_code((u8 *)sl->tpg->sh.kva + off, PTEBP_TRAMP_SZ);
		}
		else if (wind_patch_process_code(mm, sl->tramp_pc, tramp, PTEBP_TRAMP_SZ))
		{
			ret = -EFAULT;
			goto fail;
		}

		hook = ptebp_b(points[i].addr, sl->tramp_pc);

		if (sl->pg)
		{
			memcpy((u8 *)sl->pg->sh.kva + sl->hook_off, &hook, sizeof(hook));
			wind_flush_code((u8 *)sl->pg->sh.kva + sl->hook_off, sizeof(hook));
		}
		else if (wind_patch_process_code(mm, points[i].addr, &hook, sizeof(hook)))
		{
			ret = -EFAULT;
			goto fail;
		}

		sl->active = true;

		printk(KERN_INFO "ptebp: hook armed A=%llx insn=%08x T=%llx %s%s\n",
		       (unsigned long long)points[i].addr, insn,
		       (unsigned long long)sl->tramp_pc,
		       sl->pg ? "[影子页]" : "[COW]",
		       sl->tramp_in_page ? "+[页内跳板]" : "+[外跳板]");
	}

	printk(KERN_INFO "ptebp: %d 个内联钩子就位 (原页未改, 每发生效)\n",
	       (int)point_count);
	return 0;

fail:
	ptebp_remove();
	return ret;
}

int ptebp_set_regs(int point_index, u32 enable, u32 rx, u32 ry, u32 rz, u32 mask)
{
	int i, lo = 0, hi = BP_SLOT_MAX - 1;

	if (point_index >= BP_SLOT_MAX)
		return -EINVAL;
	if (point_index >= 0)
		lo = hi = point_index;

	if (!mask)
		mask = HWBP_MASK_ALL;

	for (i = lo; i <= hi; i++)
	{
		struct ptebp_slot *sl = &ptebp_slots[i];
		u32 cur[3], lit[3];
		u32 off;

		if (!sl->active)
			continue;

		sl->regmod = enable != 0;
		sl->rx = rx;
		sl->ry = ry;
		sl->rz = rz;
		sl->mask = mask;
		if (!enable)
			continue;

		/* 跳板里的三个字是「要写入寄存器的值」. 只要 mask 没全给, 就得
		 * 先读旧值再按位替换 —— 否则被掩掉的分量会被写成 0, 准心直接飞掉 */
		if (mask == HWBP_MASK_ALL)
		{
			lit[0] = rx;
			lit[1] = ry;
			lit[2] = rz;
		}
		else
		{
			if (sl->tramp_in_page)
			{
				off = (u32)(sl->lit_pc & (PAGE_SIZE - 1));
				memcpy(cur, (u8 *)sl->tpg->sh.kva + off, sizeof(cur));
			}
			else if (!g_ptebp_mm ||
				 !wind_read_process_va(g_ptebp_mm, (unsigned long)sl->lit_pc,
						       cur, sizeof(cur)))
				return -EFAULT;

			lit[0] = (mask & 0x1) ? rx : cur[0];
			lit[1] = (mask & 0x2) ? ry : cur[1];
			lit[2] = (mask & 0x4) ? rz : cur[2];
		}

		if (sl->tramp_in_page)
		{
			off = (u32)(sl->lit_pc & (PAGE_SIZE - 1));
			memcpy((u8 *)sl->tpg->sh.kva + off, lit, sizeof(lit));
			wind_flush_code((u8 *)sl->tpg->sh.kva + off, sizeof(lit));
			g_bpc.ptebp_reloads++;
			bpc_note_regs(lit[0], lit[1], lit[2]);
			continue;
		}

		if (!g_ptebp_mm)
			return -ESRCH;
		if (wind_patch_process_code(g_ptebp_mm,
					    (unsigned long)sl->lit_pc, lit, sizeof(lit)))
			return -EFAULT;

		g_bpc.ptebp_reloads++;
		bpc_note_regs(lit[0], lit[1], lit[2]);
	}
	return 0;
}

/* 自检/自愈: 影子页映射被谁改回去、或者钩子字节被谁写回, 都在这儿修。
 * 返回修好的点数, 0 表示本来就全好, 负数是错误码。 */
int ptebp_refresh(void)
{
	int i, fixed = 0;

	if (!g_ptebp_mm)
		return -ESRCH;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		struct ptebp_slot *sl = &ptebp_slots[i];
		u32 hook;
		bool bad = false;

		if (!sl->active)
			continue;

		/* a. 影子页还在不在 PTE 上 */
		if (sl->pg)
		{
			pteval_t cur = wind_pte_pin_get(&sl->pg->sh.pin);
			pteval_t want = (sl->pg->sh.saved & ~KH_PTE_PA_MASK) |
					KH_PHYS_TO_PTE(page_to_phys(sl->pg->sh.page));

			if ((cur & KH_PTE_PA_MASK) != (want & KH_PTE_PA_MASK))
			{
				wind_pte_pin_write(&sl->pg->sh.pin, sl->pg->va,
						   ~(pteval_t)0, want | PTE_AF);
				bad = true;
			}
		}

		/* b. 钩子字节还在不在 */
		hook = ptebp_b(sl->addr, sl->tramp_pc);
		if (sl->pg)
		{
			u32 cur_hook = 0;

			memcpy(&cur_hook, (u8 *)sl->pg->sh.kva + sl->hook_off,
			       sizeof(cur_hook));
			if (cur_hook != hook)
			{
				memcpy((u8 *)sl->pg->sh.kva + sl->hook_off, &hook,
				       sizeof(hook));
				wind_flush_code((u8 *)sl->pg->sh.kva + sl->hook_off,
						sizeof(hook));
				bad = true;
			}
		}
		else
		{
			u32 cur_hook = 0;

			if (!wind_read_process_va(g_ptebp_mm, sl->addr, &cur_hook,
						  sizeof(cur_hook)) ||
			    cur_hook != hook)
			{
				wind_patch_process_code(g_ptebp_mm, sl->addr, &hook,
							sizeof(hook));
				bad = true;
			}
		}

		/* c. 跳板还在不在 */
		if (sl->tramp_in_page)
		{
			u32 t[5];
			u32 off = (u32)(sl->tramp_pc & (PAGE_SIZE - 1));

			ptebp_build_tramp(t, sl->tramp_pc, sl->addr, sl->orig_insn);
			{
				u32 cur[5];

				memcpy(cur, (u8 *)sl->tpg->sh.kva + off, sizeof(cur));
				if (cur[0] != t[0] || cur[3] != t[3] || cur[4] != t[4])
				{
					memcpy((u8 *)sl->tpg->sh.kva + off, t,
					       sizeof(t));
					wind_flush_code((u8 *)sl->tpg->sh.kva + off,
							sizeof(t));
					bad = true;
				}
			}
		}

		if (bad)
			fixed++;
	}

	if (fixed)
		g_bpc.ptebp_heal_ok += fixed;
	return fixed;
}

int ptebp_remove(void)
{
	int i;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		struct ptebp_slot *sl = &ptebp_slots[i];

		/* 半装失败也要清: 影子页可能已经换上去了, 跳板页可能已经开好了 */
		if (!sl->active && !sl->pg && !sl->tramp_pc)
			continue;

		if (sl->pg)
		{
			/* 影子页是共享的: 撤掉自己的引用前必须把自己那个钩子字节写回
			 * 原指令, 否则同页其它点还在时, 这个点会留下一条指向已拆跳板的
			 * b 指令, 一跳就飞 */
			memcpy((u8 *)sl->pg->sh.kva + sl->hook_off, &sl->orig_insn,
			       sizeof(sl->orig_insn));
			wind_flush_code((u8 *)sl->pg->sh.kva + sl->hook_off,
					sizeof(sl->orig_insn));
			/* 最后一个引用撤掉时才真正把 PTE 换回原页 */
			ptebp_page_put(sl->pg);
			sl->pg = NULL;
		}
		else if (sl->active && g_ptebp_mm)
		{
			wind_patch_process_code(g_ptebp_mm, sl->addr,
						&sl->orig_insn,
						sizeof(sl->orig_insn));
		}

		/* 外跳板页: 顺序必须在恢复钩子点之后, 否则目标会在已释放页上取指 */
		if (!sl->tramp_in_page && sl->tramp_pc && g_ptebp_mm &&
		    sl->tramp_pc != sl->addr)
			wind_unmap_in(g_ptebp_mm, sl->tramp_pc, PAGE_SIZE);

		if (sl->tpg)
		{
			ptebp_page_put(sl->tpg);
			sl->tpg = NULL;
		}

		memset(sl, 0, sizeof(*sl));
	}

	ptebp_pages_clear();

	if (g_ptebp_kp)
	{
		wind_kprobe_unregister_symbol(g_ptebp_kp);
		g_ptebp_kp = NULL;
	}
	if (g_ptebp_mm)
	{
		wind_mmput(g_ptebp_mm);
		g_ptebp_mm = NULL;
	}
	return 0;
}

int ptebp_get_config(PTEBP_CONFIG *config_out)
{
	int i;
	u32 active = 0;

	if (!config_out)
		return -EINVAL;
	for (i = 0; i < BP_SLOT_MAX; i++)
		if (ptebp_slots[i].active)
			active++;
	memset(config_out, 0, sizeof(*config_out));
	config_out->active_count = active;
	return 0;
}

int ptebp_get_records(uint32_t point_index, uint32_t start_index,
		      PTEBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n;

	if (point_index >= BP_SLOT_MAX || !records || !count_out)
		return -EINVAL;
	n = bp_rec_read(ptebp_recs[point_index], sizeof(PTEBP_RECORD),
			ptebp_head[point_index], ptebp_count[point_index],
			start_index, records, capacity);
	*count_out = n;
	return 0;
}

int ptebp_set_record(uint32_t point_index, uint32_t record_index,
		     const PTEBP_RECORD *record)
{
	if (point_index >= BP_SLOT_MAX || !record)
		return -EINVAL;
	if (!bp_rec_set(ptebp_recs[point_index], sizeof(PTEBP_RECORD),
			ptebp_head[point_index], ptebp_count[point_index],
			record_index, record))
		return -ENOENT;
	return 0;
}

int ptebp_clear_records(int32_t point_index)
{
	int i;

	if (point_index >= 0)
	{
		if ((u32)point_index >= BP_SLOT_MAX)
			return -EINVAL;
		ptebp_head[point_index] = 0;
		ptebp_count[point_index] = 0;
		return 0;
	}
	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		ptebp_head[i] = 0;
		ptebp_count[i] = 0;
	}
	return 0;
}

/* ===================================================================
 * 3. wrpbp — 数据断点追踪 (WRP 观察点 + 命中即写回)
 *
 * 这是三族里最隐蔽的一族: 不改代码字节, 不动页表映射, 不占 BRP 执行断点,
 * 也不产生任何异常信号。只开一个 WRP 观察点盯住「弹道/朝向向量」那块内存,
 * 游戏每写一次(每帧重算准心), 我们立刻把它改回目标向量。
 *
 * 隐蔽面: 代码段 / smaps / 页表 全干净, 只剩数据值本身与游戏自己的计算
 * 结果不一致, 而这是任何方案都避免不了的。
 *
 * 两个必须处理的坑:
 *   1) 我们自己写回也会命中观察点 -> 递归。用 per-CPU busy 计数 + 只在
 *      user_mode 命中时写回, 双保险。
 *   2) 12 字节向量一个 WRP 覆盖不全 (arm64 WRP 最大 LEN_8) -> 主观察点
 *      + 尾观察点两个事件, 任一命中都写回整段。
 * =================================================================== */
#define WRPBP_VEC_BYTES 12

struct wrpbp_slot
{
	uint64_t watch_addr;
	uint32_t watch_len;
	uint32_t type;
	uint32_t components;
	uint32_t freg_base;         /* 起始 S 寄存器号 */
	uint32_t freg_mask;         /* bit0..bit7 -> freg_base+0..7 */
	struct perf_event *ev[3];   /* 覆盖 12 字节: 8 + 4, 极端情况 4+4+4 */
	uint64_t ev_addr[3];
	uint32_t ev_len[3];
	bool wide[3];
	bool active;
};

struct wrpbp_vecmod
{
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
static DEFINE_PER_CPU(int, wrpbp_busy);

/* 命中后把目标向量写回: 只走 nofault uaccess, 断点上下文里不能睡眠 */
static void wrpbp_apply_vec(int slot)
{
	struct wrpbp_vecmod *v = &wrpbp_vec[slot];
	struct wrpbp_slot *sl = &wrpbp_slots[slot];
	u32 buf[3];
	u32 mask = v->mask ? v->mask : HWBP_MASK_ALL;
	int ret = -EFAULT;

	if (!mask)
		return;

	if (wind_copy_from_user_nofault(buf, (void __user *)sl->watch_addr,
					WRPBP_VEC_BYTES) != 0)
		goto out;

	/* 已经是目标值就不写: 少一次写回就少一次被观察点抓到的机会 */
	if ((!(mask & 0x1) || buf[0] == v->x) &&
	    (!(mask & 0x2) || buf[1] == v->y) &&
	    (!(mask & 0x4) || buf[2] == v->z))
	{
		g_bpc.wrpbp_skips++;
		return;
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
	if (ret == 0)
	{
		g_bpc.wrpbp_writes++;
		bpc_note_regs(v->x, v->y, v->z);
	}
	else
	{
		g_bpc.wrpbp_fails++;
	}
}

/* WRP 的浮点寄存器通道: 内存改写对"先搬进 S 寄存器再算"的代码无效,
 * 这里补一条寄存器写回。与 clonebp 走同一套 fpsimd 机制:
 *   先 preserve 把硬件状态落到 thread.uw.fpsimd_state,
 *   再改 vregs[n] 低 32 位, 最后置 TIF_FOREIGN_FPSTATE 让返回用户态时重载。
 */
/* 浮点写回总开关, 默认开。insmod wind.ko wind_fp_writeback=0 可整体关掉,
 * 用来区分"崩在浮点写回"还是"崩在别处"。
 */
bool g_wind_fp_writeback = true;
module_param_named(wind_fp_writeback, g_wind_fp_writeback, bool, 0644);
MODULE_PARM_DESC(wind_fp_writeback,
		 "浮点寄存器写回总开关, 0=只用内存改写 (默认 1)");

static void wrpbp_apply_fregs(int slot)
{
	struct wrpbp_vecmod *v = &wrpbp_vec[slot];
	struct wrpbp_slot *sl = &wrpbp_slots[slot];
	unsigned long fl;
	int k;

	/* 只认运行时的 v->freg_mask: 安装期的 sl->freg_mask 只是"允许有这个通道",
	 * 真正写哪些槽位必须以每次 set_fregs 传的为准, 否则会在用户还没设值时
	 * 把 S3/S4/S5 一次性清零。 */
	if (!v->freg_mask)
		return;
	if (!g_wind_fp_writeback)
		return;

	/* 这一段是 perf 溢出回调里跑的。fpsimd_preserve_current_state() 内部
	 * 有 WARN_ON(preemptible()), Android 默认 panic_on_warn, 一命中就重启。
	 * 先把抢占关掉再进, 并把整个写回序列包在同一段里。
	 */
	preempt_disable();
	if (!test_thread_flag(TIF_FOREIGN_FPSTATE)) {
		/* 硬件里的活值还没落盘, 先存一次, 否则下面置 FOREIGN 后
		 * 返回用户态会用一份旧内存覆盖全部寄存器。 */
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

extern bool gyro_holds_breakpoints(void);

/* 每秒最多写回这么多次, 超了就暂停该槽位的写回 */
#define WRPBP_RATE_MAX 4000
static unsigned long wrpbp_rate_last[BP_SLOT_MAX];
static atomic_t wrpbp_rate_cnt[BP_SLOT_MAX];
static bool wrpbp_rate_warned[BP_SLOT_MAX];

static void wrpbp_triggered(struct perf_event *bp, struct perf_sample_data *data,
			    struct pt_regs *regs)
{
	int i, j;
	HWBP_RECORD rec;

	if (wrpbp_tgid && current->tgid != wrpbp_tgid)
		return;
	/* 内核上下文里的写基本就是我们自己的写回, 直接放过, 不递归 */
	if (!user_mode(regs))
		return;
	if (this_cpu_read(wrpbp_busy))
		return;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (!wrpbp_slots[i].active)
			continue;
		for (j = 0; j < 3; j++)
		{
			if (!wrpbp_slots[i].ev[j])
				continue;
			if (wrpbp_slots[i].ev_addr[j] != (uint64_t)bp->attr.bp_addr)
				continue;

			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = instruction_pointer(regs);
			rec.hit_addr = data ? data->addr : wrpbp_slots[i].watch_addr;
			rec.op_mask = 0x2; /* 写命中 */
			bp_rec_push(wrpbp_recs[i], sizeof(HWBP_RECORD),
				    &wrpbp_head[i], &wrpbp_count[i], &rec);
			g_bpc.wrpbp_hits++;
			g_bpc.last_hit_pc = rec.trigger_pc;

			if (bp_dbg && wrpbp_count[i] <= 3)
				printk(KERN_INFO "wrpbp: hit slot=%d pc=%lx addr=%llx n=%u\n",
				       i, (unsigned long)rec.trigger_pc,
				       (unsigned long long)wrpbp_slots[i].watch_addr,
				       (unsigned)wrpbp_count[i]);

			if (wrpbp_vec[i].enabled)
			{
				/* 每秒写回次数限流。
				 * 观察点打在一个极热的字段上时, 每次命中都改内存
				 * 等于持续往目标进程里灌数据, 一旦地址或值不对,
				 * 游戏过一会儿就 SIGSEGV。超限就停手, 只保留计数。 */
				unsigned long now = jiffies;

				if (!wrpbp_rate_last[i] ||
				    time_after(now, wrpbp_rate_last[i] + HZ)) {
					wrpbp_rate_last[i] = now;
					atomic_set(&wrpbp_rate_cnt[i], 0);
				}
				if (atomic_inc_return(&wrpbp_rate_cnt[i]) >
				    WRPBP_RATE_MAX) {
					if (!wrpbp_rate_warned[i]) {
						wrpbp_rate_warned[i] = true;
						pr_warn("wrpbp: slot %d 写入频率超限(%d/s), "
							"已暂停写回地址 %llx\n",
							i, WRPBP_RATE_MAX,
							(unsigned long long)
							wrpbp_slots[i].watch_addr);
					}
				}
				else
				{
					this_cpu_inc(wrpbp_busy);
					wrpbp_apply_vec(i);
					wrpbp_apply_fregs(i);
					this_cpu_dec(wrpbp_busy);
				}
			}
			return;
		}
	}
}

static void wrpbp_clear_slot(int i)
{
	int j;

	for (j = 0; j < 3; j++)
	{
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
}

/* watch_addr 必须整个落在目标进程一个可写 VMA 里。
 * 不校验的话, 用户传一个错的/过期的地址, apply_vec 就往别处写 12 字节,
 * 游戏跑一会儿踩到被写坏的数据就 SIGSEGV, 看起来像"读取完数据过一秒崩"。
 */
static bool wrpbp_addr_writable(struct mm_struct *mm, uint64_t addr, u32 len)
{
	struct vm_area_struct *vma;
	bool ok = false;

	if (!mm || !len)
		return false;
	if (addr < 0x1000ULL || addr >= 0x0001000000000000ULL)
		return false;
	if (addr + len < addr)
		return false;

	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)addr);
	if (vma && (unsigned long)addr >= vma->vm_start &&
	    (unsigned long)addr + len <= vma->vm_end &&
	    (vma->vm_flags & VM_WRITE))
		ok = true;
	mmap_read_unlock(mm);
	return ok;
}

int wrpbp_set(struct driver_session *sess, const WRPBP_POINT_CONFIG *points,
	      size_t point_count)
{
	int i, ret = 0;

	if (!sess || !sess->task)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	/* 陀螺仪占着调试寄存器时就别再装追踪点了。
	 * arm64 只有 4 个, 陀螺仪 2 个 + 每个追踪点最多 3 个, 一起装必然失败;
	 * 更糟的是两边互相踩, 异常一路打下来把机器拖死 -> 卡死重启。 */
	if (gyro_holds_breakpoints())
	{
		printk(KERN_ERR "wrpbp: 陀螺仪已占用硬件断点, 拒绝安装追踪点 "
			       "(先关陀螺仪自瞄)\n");
		return -EBUSY;
	}

	wrpbp_remove();
	wrpbp_tgid = sess->task->tgid;

	for (i = 0; i < (int)point_count; i++)
	{
		struct wrpbp_slot *sl = &wrpbp_slots[i];
		u32 hwtype = bp_type_to_hw(points[i].type ? points[i].type
							 : BP_TYPE_WRITE);
		u32 len = points[i].watch_len ? points[i].watch_len : 4;
		u32 off;
		int j, n_ev = 0;

		if (hwtype != HW_BREAKPOINT_W && hwtype != HW_BREAKPOINT_RW)
		{
			ret = -EINVAL;
			goto fail;
		}
		if (bp_len_to_hw(len) < 0)
		{
			ret = -EINVAL;
			goto fail;
		}
		if (!wrpbp_addr_writable(sess->task->mm, points[i].watch_addr,
					 WRPBP_VEC_BYTES))
		{
			printk(KERN_ERR "wrpbp: point %d addr=%llx 不在目标可写 VMA 内, "
			       "拒绝安装\n", i,
			       (unsigned long long)points[i].watch_addr);
			ret = -EFAULT;
			goto fail;
		}

		/* 12 字节向量在 arm64 上最多 LEN_8, 一个观察点盖不住:
		 * 从偏移 0 起按 8 -> 4 (必要时 4/4/4) 连续铺满 [0, WRPBP_VEC_BYTES) */
		for (j = 0, off = 0; j < 3 && off < WRPBP_VEC_BYTES; j++)
		{
			struct perf_event_attr attr;
			uint64_t addr;
			u32 cur_len = 0;
			void *ev;
			bool wide = true;
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

			/* 必须按任务注册。
			 * wide 版是"全 CPU 全任务"的断点: 每个核每次上下文切换都要进
			 * 一次异常, 再靠 wrpbp_tgid 过滤掉。用户地址用 wide 的代价是
			 * 整机每秒多出几万次异常 -> 表现为"用 wind 读取非常卡",
			 * 断点打得多的时候直接把机器拖死重启。
			 * register_user_hw_breakpoint 只在该 task 调度上 CPU 时生效。 */
			/* 按任务优先; 解析不到 user 版时退回 wide, 别把功能整个搞没 */
			ev = wind_register_user_hw_breakpoint(&attr,
							      wrpbp_triggered,
							      NULL, sess->task);
			if (IS_ERR(ev) && PTR_ERR(ev) == -EOPNOTSUPP)
			{
				wide = true;
				ev = wind_register_wide_hw_breakpoint(&attr,
								      wrpbp_triggered, NULL);
			}
			if (IS_ERR(ev))
			{
				ret = PTR_ERR(ev);
				printk(KERN_ERR "wrpbp: point %d ev%d 注册失败 %d\n",
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

		printk(KERN_INFO "wrpbp: armed slot=%d addr=%llx len=%u 观察点=%d tgid=%d\n",
		       i, (unsigned long long)sl->watch_addr, len, n_ev, (int)wrpbp_tgid);
	}
	return 0;

fail:
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

	for (i = lo; i <= hi; i++)
	{
		wrpbp_vec[i].enabled = enable != 0;
		wrpbp_vec[i].mask = mask ? mask : HWBP_MASK_ALL;
		wrpbp_vec[i].x = x;
		wrpbp_vec[i].y = y;
		wrpbp_vec[i].z = z;
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
	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (!wrpbp_slots[i].active)
			continue;
		out->watch_addr_lo = (u32)(wrpbp_slots[i].watch_addr & 0xffffffffU);
		out->watch_addr_hi = (u32)(wrpbp_slots[i].watch_addr >> 32);
		break;
	}
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

	for (i = lo; i <= hi; i++)
	{
		wrpbp_head[i] = 0;
		wrpbp_count[i] = 0;
	}
	return 0;
}

int wrpbp_remove(void)
{
	int i;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		wrpbp_clear_slot(i);
		memset(&wrpbp_vec[i], 0, sizeof(wrpbp_vec[i]));
	}
	wrpbp_tgid = 0;
	return 0;
}

/* ===================================================================
 * 3. brkbp — BRK 软断点 (改写指令字节为 BRK #imm)
 * =================================================================== */
#define BRKBP_IMM         0x5A
#define BRKBP_INSN        (0xD4200000U | ((BRKBP_IMM & 0xFFFF) << 5))

struct brkbp_slot
{
	uint64_t addr;
	u32 orig_insn;
	bool active;
};

static struct brkbp_slot brkbp_slots[BP_SLOT_MAX];
static BRKBP_RECORD brkbp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 brkbp_head[BP_SLOT_MAX];
static u32 brkbp_count[BP_SLOT_MAX];
static struct kprobe *g_brkbp_kp;
static struct mm_struct *g_brkbp_mm;

static const char *g_brkbp_syms[] = {
	"brk_handler",
	"do_debug_exception",
	NULL,
};

static int brkbp_pre(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);
	int i;

	(void)p;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (!brkbp_slots[i].active)
			continue;
		if (pc == brkbp_slots[i].addr)
		{
			BRKBP_RECORD rec;

			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = pc;
			rec.hit_addr = pc;
			rec.op_mask = bp_op_mask(BP_TYPE_EXEC, pc);
			bp_rec_push(brkbp_recs[i], sizeof(BRKBP_RECORD),
				    &brkbp_head[i], &brkbp_count[i], &rec);
		}
	}
	return 0;
}

/* mm 的所有权在本函数手里: 成功时交给 g_brkbp_mm, 失败时自己 mmput */
static int brkbp_set_mm(struct mm_struct *mm,
			const BRKBP_POINT_CONFIG *points, size_t point_count)
{
	int i, ret;

	if (!mm)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
	{
		wind_mmput(mm);
		return -EINVAL;
	}

	brkbp_remove();

	if (!g_brkbp_kp)
	{
		int s;
		for (s = 0; g_brkbp_syms[s]; s++)
		{
			g_brkbp_kp = wind_kprobe_register_symbol(g_brkbp_syms[s],
							       brkbp_pre, NULL);
			if (g_brkbp_kp)
			{
				printk(KERN_INFO "brkbp: hook @ %s\n", g_brkbp_syms[s]);
				break;
			}
		}
		if (!g_brkbp_kp)
		{
			ret = -EOPNOTSUPP;
			printk(KERN_ERR "brkbp: no kprobe-able brk handler (NOKPROBE)\n");
			goto fail_mm;
		}
	}

	for (i = 0; i < (int)point_count; i++)
	{
		u32 insn;

		if (!wind_read_process_va(mm, points[i].addr, &insn, 4))
		{
			ret = -EINVAL;
			goto fail;
		}

		brkbp_slots[i].addr = points[i].addr;
		brkbp_slots[i].orig_insn = insn;
		brkbp_slots[i].active = true;

		{
			u32 brk = BRKBP_INSN;
			if (!wind_write_process_va(mm, points[i].addr, &brk, 4))
			{
				ret = -EINVAL;
				goto fail;
			}
		}
	}

	g_brkbp_mm = mm;
	return 0;

fail:
	brkbp_remove();
fail_mm:
	wind_mmput(mm);
	return ret;
}

int brkbp_set(struct driver_session *sess, const BRKBP_POINT_CONFIG *points,
	      size_t point_count)
{
	if (!sess || !sess->task)
		return -ESRCH;
	return brkbp_set_mm(get_task_mm(sess->task), points, point_count);
}

/* ---- 老接口 OP_BRK_SET / OP_BRK_CLEAR ----
 * 同一个机制: 把目标指令换成 BRK, 命中由 kprobe 接住。这里只做 pid 解析,
 * 之后全部交给 brkbp_set_mm, 不再复制一份断点实现。 */
int brk_set(pid_t pid, uintptr_t addr, int type)
{
	BRKBP_POINT_CONFIG pt;
	struct pid *pidp;
	struct task_struct *task;
	struct mm_struct *mm;

	if (pid <= 1 || !addr)
		return -EINVAL;
	if (type != BP_TYPE_EXEC)
		return -EINVAL;

	pidp = find_get_pid(pid);
	if (!pidp)
		return -ESRCH;
	task = pid_task(pidp, PIDTYPE_PID);
	if (!task)
	{
		put_pid(pidp);
		return -ESRCH;
	}
	get_task_struct(task);
	put_pid(pidp);
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	memset(&pt, 0, sizeof(pt));
	pt.addr = addr;
	pt.type = BP_TYPE_EXEC;
	pt.length = 4;
	return brkbp_set_mm(mm, &pt, 1);
}

int brk_clear(pid_t pid, uintptr_t addr)
{
	int i, hit = 0;

	if (pid <= 1)
		return -EINVAL;

	for (i = 0; i < BP_SLOT_MAX; i++)
		if (brkbp_slots[i].active &&
		    (!addr || brkbp_slots[i].addr == addr))
			hit = 1;
	if (!hit)
		return -ENOENT;
	return brkbp_remove();
}

int brkbp_remove(void)
{
	int i;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (brkbp_slots[i].active && g_brkbp_mm)
			wind_write_process_va(g_brkbp_mm, brkbp_slots[i].addr,
					      &brkbp_slots[i].orig_insn, 4);
		memset(&brkbp_slots[i], 0, sizeof(brkbp_slots[i]));
	}
	if (g_brkbp_mm)
	{
		wind_mmput(g_brkbp_mm);
		g_brkbp_mm = NULL;
	}
	return 0;
}

int brkbp_get_config(BRKBP_CONFIG *config_out)
{
	int i;
	u32 active = 0;

	if (!config_out)
		return -EINVAL;
	for (i = 0; i < BP_SLOT_MAX; i++)
		if (brkbp_slots[i].active)
			active++;
	memset(config_out, 0, sizeof(*config_out));
	config_out->active_count = active;
	return 0;
}

int brkbp_get_records(uint32_t point_index, uint32_t start_index,
		      BRKBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n;

	if (point_index >= BP_SLOT_MAX || !records || !count_out)
		return -EINVAL;
	n = bp_rec_read(brkbp_recs[point_index], sizeof(BRKBP_RECORD),
			brkbp_head[point_index], brkbp_count[point_index],
			start_index, records, capacity);
	*count_out = n;
	return 0;
}

int brkbp_set_record(uint32_t point_index, uint32_t record_index,
		     const BRKBP_RECORD *record)
{
	if (point_index >= BP_SLOT_MAX || !record)
		return -EINVAL;
	if (!bp_rec_set(brkbp_recs[point_index], sizeof(BRKBP_RECORD),
			brkbp_head[point_index], brkbp_count[point_index],
			record_index, record))
		return -ENOENT;
	return 0;
}

int brkbp_clear_records(int32_t point_index)
{
	int i;

	if (point_index >= 0)
	{
		if ((u32)point_index >= BP_SLOT_MAX)
			return -EINVAL;
		brkbp_head[point_index] = 0;
		brkbp_count[point_index] = 0;
		return 0;
	}
	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		brkbp_head[i] = 0;
		brkbp_count[i] = 0;
	}
	return 0;
}

/* ===================================================================
 * 5. udpbp : UDP 断点 (kprobe 内核 UDP 收发路径)
 *
 * 打点位置 (都是非 static 符号, 可 kprobe):
 *   TX : ip_local_out(net, sk, skb)   x2 = skb, data 指向 IPv4 头
 *   RX : __udp4_lib_rcv(skb, table, proto) x0 = skb, data 指向 UDP 头
 *
 * 过滤: pid(tgid) / 端口 / 方向。命中写环形记录, 可选改写载荷首
 * patch_len 字节 (仅在 skb 未被 clone 时改写, 避免污染共享数据)。
 * 不改 debug 寄存器、不改页表、不改目标进程地址空间。
 *
 * 未验证项:
 *   - 仅 IPv4 (__udp4_lib_rcv); IPv6 走 __udp6_lib_rcv, 未接。
 *   - TX 的 skb 已是构造好的 IPv4 包, data 指向 IP 头; 若上游走
 *     GSO/frag 链 (非线性), 预览/改写只作用于线性区。
 *   - RX 若目标内核在 __udp4_lib_rcv 入口前已 pull IP 头, 端口仍从
 *     skb->data 的 UDP 头读; 该假设需真机用 tcpdump 对照一次。
 * =================================================================== */
#define UDPBP_REC_PER_SLOT  UDPBP_RECORD_MAX

struct udpbp_slot
{
	bool used;
	UDPBP_POINT_CONFIG cfg;
	UDPBP_RECORD *recs;
	u32 head;
	u32 count;
	u64 hits;
};

static struct udpbp_slot g_udpbp[UDPBP_MAX_SLOTS];
static struct kprobe *g_udpbp_tx_kp;
static struct kprobe *g_udpbp_rx_kp;
static bool g_udpbp_armed;

static void udpbp_rec_push(struct udpbp_slot *s, const UDPBP_RECORD *rec)
{
	u32 idx;

	if (!s->recs)
		return;

	idx = s->head % UDPBP_REC_PER_SLOT;
	s->recs[idx] = *rec;
	s->head = (s->head + 1) % UDPBP_REC_PER_SLOT;
	if (s->count < UDPBP_REC_PER_SLOT)
		s->count++;
	s->hits++;
}

static u32 udpbp_rec_read(struct udpbp_slot *s, u32 start_index,
			  UDPBP_RECORD *out, u32 max)
{
	u32 i, n = 0, oldest;

	if (!s->recs || start_index >= s->count || max == 0)
		return 0;

	oldest = (s->head - s->count + UDPBP_REC_PER_SLOT) % UDPBP_REC_PER_SLOT;
	for (i = start_index; i < s->count && n < max; i++)
	{
		u32 idx = (oldest + i) % UDPBP_REC_PER_SLOT;

		out[n] = s->recs[idx];
		n++;
	}
	return n;
}

/* 解析 skb 的 UDP 端口/载荷/地址。成功返回 true。 */
static bool udpbp_parse(struct sk_buff *skb, u16 dir, u16 *sport, u16 *dport,
			u8 **payload, u32 *plen, u32 *saddr, u32 *daddr)
{
	u8 *base, *uh;
	u32 headlen, off;
	struct iphdr *iph;

	if (!skb || !skb->data || skb->len < 8)
		return false;

	base = skb->data;
	headlen = skb_headlen(skb);

	if (dir == UDPBP_DIR_RX)
	{
		u32 ip_off = skb->network_header;

		if (ip_off < skb->end && (u32)ip_off + sizeof(struct iphdr) <= (u32)skb->end)
		{
			iph = (struct iphdr *)(skb->head + ip_off);
			if (iph->version == 4 && iph->protocol == IPPROTO_UDP)
			{
				*saddr = (u32)iph->saddr; /* 网络序原样 */
				*daddr = (u32)iph->daddr;
			}
		}
		if (headlen < 8)
			return false;
		uh = base;
		off = 0; /* base 已指向 UDP 头 */
	}
	else
	{
		/* TX: udp_send_skb / ip_send_skb 时 skb->data 一般已指向 IP 头,
		 * 但 GSO/分片链上只保证 network_header 有效, 两条都试。 */
		iph = NULL;
		if (base && headlen >= sizeof(struct iphdr))
		{
			struct iphdr *cand = (struct iphdr *)base;

			if (cand->version == 4 && cand->protocol == IPPROTO_UDP &&
			    cand->ihl >= 5)
				iph = cand;
		}
		if (!iph && (u32)skb->network_header + sizeof(struct iphdr) <=
			     (u32)skb->end)
		{
			struct iphdr *cand =
				(struct iphdr *)(skb->head + skb->network_header);

			if (cand->version == 4 && cand->protocol == IPPROTO_UDP &&
			    cand->ihl >= 5)
				iph = cand;
		}
		if (!iph)
			return false;

		{
			u8 *ip_base = (u8 *)iph;
			u32 ip_off = (u32)(ip_base - base);

			off = ip_off + (u32)iph->ihl * 4;
			if (off < ip_off || headlen < off + 8)
				return false;
			uh = ip_base + (u32)iph->ihl * 4;
		}
		*saddr = (u32)iph->saddr; /* 网络序原样 */
		*daddr = (u32)iph->daddr;
	}

	*sport = ((u16)uh[0] << 8) | uh[1];
	*dport = ((u16)uh[2] << 8) | uh[3];
	*payload = uh + 8;
	*plen = (skb->len > off + 8) ? (skb->len - off - 8) : 0;
	{
		u32 lin = (headlen > off + 8) ? (headlen - off - 8) : 0;

		if (*plen > lin)
			*plen = lin;
	}

	return true;
}

static void udpbp_handle(struct sk_buff *skb, u16 dir)
{
	u16 sport = 0, dport = 0;
	u32 plen = 0, saddr = 0, daddr = 0;
	u8 *payload = NULL;
	u32 pid;
	int i;

	if (!READ_ONCE(g_udpbp_armed) || !skb)
		return;

	pid = (u32)current->tgid;

	if (!udpbp_parse(skb, dir, &sport, &dport, &payload, &plen, &saddr, &daddr))
		return;

	for (i = 0; i < UDPBP_MAX_SLOTS; i++)
	{
		struct udpbp_slot *s = &g_udpbp[i];
		UDPBP_RECORD rec;
		u16 act;

		if (!READ_ONCE(s->used))
			continue;
		if (s->cfg.dir != UDPBP_DIR_BOTH && s->cfg.dir != dir)
			continue;
		if (s->cfg.pid && s->cfg.pid != pid)
			continue;
		/* 原实现 TX 只比目的端口、RX 只比源端口, 按端口抓本地收发都会漏。
		 * 改成源/目的任一命中即算命中, 结构体定义不变。 */
		if (s->cfg.port && s->cfg.port != sport && s->cfg.port != dport)
			continue;

		act = s->cfg.action;

		/* 记录里的 preview 必须是"改写前"的原始载荷: 上层要拿它做离线解密,
		 * 若先改写再拷贝, 记录里就只剩自己写进去的明文, 无法校验。
		 * 先把记录填好, 再做改写。 */
		memset(&rec, 0, sizeof(rec));
		rec.ts_ns = (u64)READ_ONCE(jiffies) * (NSEC_PER_SEC / HZ);
		rec.pid = pid;
		rec.saddr = saddr;
		rec.daddr = daddr;
		rec.sport = sport;
		rec.dport = dport;
		rec.dir = dir;
		rec.len = plen;
		rec.preview_len = (payload && plen) ? min_t(u32, plen, UDPBP_PREVIEW_MAX) : 0;
		if (rec.preview_len)
			memcpy(rec.preview, payload, rec.preview_len);

		/* 改写: 只动线性区且 skb 未被 clone, 否则降级为只记录 */
		if (act == UDPBP_ACT_REWRITE)
		{
			u32 n = s->cfg.patch_len;

			if (!payload || n == 0 || n > plen || skb_cloned(skb))
			{
				act = UDPBP_ACT_LOG;
			}
			else
			{
				memcpy(payload, s->cfg.patch, n);
			}
		}
		rec.action = act;

		udpbp_rec_push(s, &rec);
	}
}

/* TX 探针候选: 名字 / skb 所在参数序号 (arm64 x0..x2)。
 * ip_local_out 在 GKI 的 LTO 构建里被内联, 挂在它的符号地址上的 kprobe
 * 一次都不会触发（真机 ftrace: hits=0）, 所以它只能当兜底。 */
static const char *g_udpbp_tx_sym;
static int g_udpbp_tx_arg = 2;

static int udpbp_tx_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* udp_send_skb(skb, sk) / ip_send_skb(net, skb) /
	 * ip_local_out(net, sk, skb) —— 按 g_udpbp_tx_arg 取参数 */
	(void)p;
	udpbp_handle((struct sk_buff *)regs->regs[g_udpbp_tx_arg], UDPBP_DIR_TX);
	return 0;
}

static int udpbp_rx_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* __udp4_lib_rcv(struct sk_buff *, struct udp_table *, int) */
	(void)p;
	udpbp_handle((struct sk_buff *)regs->regs[0], UDPBP_DIR_RX);
	return 0;
}

/* 首次使用时懒注册探针 */
static int udpbp_probe_ensure(void)
{
	if (g_udpbp_tx_kp || g_udpbp_rx_kp)
		return 0;

	{
		static const struct {
			const char *name;
			int arg;
		} tx_cands[] = {
			{ "udp_send_skb", 0 },  /* 首选: 每个 UDP 报文必走 */
			{ "ip_send_skb",  1 },
			{ "ip_local_out", 2 },  /* GKI 上被内联, 仅兜底 */
		};
		int i;

		for (i = 0; i < (int)(sizeof(tx_cands) / sizeof(tx_cands[0])); i++)
		{
			g_udpbp_tx_arg = tx_cands[i].arg;
			g_udpbp_tx_kp = wind_kprobe_register_symbol(tx_cands[i].name,
								   udpbp_tx_pre, NULL);
			if (g_udpbp_tx_kp)
			{
				g_udpbp_tx_sym = tx_cands[i].name;
				break;
			}
		}
	}
	g_udpbp_rx_kp = wind_kprobe_register_symbol("__udp4_lib_rcv", udpbp_rx_pre, NULL);

	if (!g_udpbp_tx_kp && !g_udpbp_rx_kp)
	{
		printk(KERN_ERR "wind: udpbp probe register failed (tx: udp_send_skb/ip_send_skb/ip_local_out, rx: __udp4_lib_rcv)\n");
		return -EOPNOTSUPP;
	}
	printk(KERN_INFO "wind: udpbp probes tx=%s(arg%d) rx=%d\n",
	       g_udpbp_tx_sym ? g_udpbp_tx_sym : "none", g_udpbp_tx_arg,
	       g_udpbp_rx_kp ? 1 : 0);
	return 0;
}

int udpbp_set(int32_t slot_index, const UDPBP_POINT_CONFIG *point, int32_t *slot_out)
{
	struct udpbp_slot *s = NULL;
	int i, ret;

	if (!point)
		return -EINVAL;
	if (point->dir > UDPBP_DIR_BOTH || point->action > UDPBP_ACT_REWRITE)
		return -EINVAL;
	if (point->patch_len > UDPBP_PATCH_MAX)
		return -EINVAL;
	if (point->action == UDPBP_ACT_REWRITE && point->patch_len == 0)
		return -EINVAL;

	ret = udpbp_probe_ensure();
	if (ret)
		return ret;

	if (slot_index >= 0)
	{
		if (slot_index >= UDPBP_MAX_SLOTS)
			return -EINVAL;
		s = &g_udpbp[slot_index];
	}
	else
	{
		for (i = 0; i < UDPBP_MAX_SLOTS; i++)
			if (!g_udpbp[i].used)
			{
				s = &g_udpbp[i];
				break;
			}
		if (!s)
			return -ENOSPC;
	}

	if (!s->recs)
	{
		s->recs = vzalloc(sizeof(UDPBP_RECORD) * UDPBP_REC_PER_SLOT);
		if (!s->recs)
			return -ENOMEM;
		s->head = 0;
		s->count = 0;
		s->hits = 0;
	}

	s->cfg = *point;
	WRITE_ONCE(s->used, true);
	WRITE_ONCE(g_udpbp_armed, true);

	if (slot_out)
		*slot_out = (int32_t)(s - g_udpbp);

	printk(KERN_INFO "wind: udpbp slot=%d pid=%u port=%u dir=%u act=%u len=%u\n",
	       (int)(s - g_udpbp), point->pid, point->port, point->dir,
	       point->action, point->patch_len);
	return 0;
}

int udpbp_remove(int32_t slot_index)
{
	int i, n = 0;

	for (i = 0; i < UDPBP_MAX_SLOTS; i++)
	{
		struct udpbp_slot *s = &g_udpbp[i];

		if (slot_index >= 0 && i != slot_index)
			continue;
		if (!s->used)
			continue;

		/* 只失活, 不释放记录环: kprobe 可能正在另一 CPU 上读它,
		 * 真正的 vfree 留到 wind_bp_exit (探针已注销之后)。 */
		WRITE_ONCE(s->used, false);
		memset(&s->cfg, 0, sizeof(s->cfg));
		s->head = 0;
		s->count = 0;
		s->hits = 0;
		n++;
		if (slot_index >= 0)
			break;
	}

	{
		bool any = false;

		for (i = 0; i < UDPBP_MAX_SLOTS; i++)
			if (g_udpbp[i].used)
				any = true;
		WRITE_ONCE(g_udpbp_armed, any);
	}

	return n ? 0 : -ENOENT;
}

int udpbp_get_config(UDPBP_CONFIG *config_out)
{
	int i, n = 0;

	if (!config_out)
		return -EINVAL;

	memset(config_out, 0, sizeof(*config_out));
	for (i = 0; i < UDPBP_MAX_SLOTS; i++)
		if (g_udpbp[i].used)
		{
			memcpy(&config_out->slots[n], &g_udpbp[i].cfg,
			       sizeof(UDPBP_POINT_CONFIG));
			n++;
		}
	config_out->slot_count = (uint32_t)n;
	return 0;
}

int udpbp_get_records(int32_t slot_index, uint32_t start_index,
		      UDPBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n = 0;
	int i;

	if (!records || !count_out)
		return -EINVAL;

	if (slot_index >= 0)
	{
		if (slot_index >= UDPBP_MAX_SLOTS)
			return -EINVAL;
		n = udpbp_rec_read(&g_udpbp[slot_index], start_index, records, capacity);
	}
	else
	{
		/* 合并: 逐槽按序追加 (槽号升序, 单槽内按时间) */
		for (i = 0; i < UDPBP_MAX_SLOTS && n < capacity; i++)
		{
			if (!g_udpbp[i].used)
				continue;
			n += udpbp_rec_read(&g_udpbp[i], start_index,
					    records + n, capacity - n);
		}
	}

	*count_out = n;
	return 0;
}

int udpbp_clear_records(int32_t slot_index)
{
	int i, n = 0;

	for (i = 0; i < UDPBP_MAX_SLOTS; i++)
	{
		if (slot_index >= 0 && i != slot_index)
			continue;
		g_udpbp[i].head = 0;
		g_udpbp[i].count = 0;
		g_udpbp[i].hits = 0;
		n++;
		if (slot_index >= 0)
			break;
	}
	return n ? 0 : -ENOENT;
}

/* 探针注销之后才能释放记录环 (handler 已不会再进来) */
static void udpbp_free_all(void)
{
	int i;

	for (i = 0; i < UDPBP_MAX_SLOTS; i++)
		if (g_udpbp[i].recs)
		{
			vfree(g_udpbp[i].recs);
			g_udpbp[i].recs = NULL;
		}
}

/* ===================================================================
 * 三族诊断快照
 * =================================================================== */
/* 换追踪方式的原子动作: 顺序固定, 先拆事件再关改写, 最后清记录。
 * 任何一步失败都继续往下走 —— 半关状态比全关危险得多。 */
int bp_all_off(void)
{
	int i, ret = 0, r;

	r = hwbp_remove();
	if (r < 0 && !ret)
		ret = r;
	r = ptebp_remove();
	if (r < 0 && !ret)
		ret = r;
	r = wrpbp_remove();
	if (r < 0 && !ret)
		ret = r;

	hwbp_set_reg_modify(-1, 0, 0, 0, 0, 0);
	ptebp_set_regs(-1, 0, 0, 0, 0, 0);
	wrpbp_set_vec(-1, 0, 0, 0, 0, 0);

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		hwbp_clear_records(i);
		ptebp_clear_records(i);
		wrpbp_clear_records(i);
	}

	printk(KERN_INFO "bp: all off (三族断点已全部拆除并关闭改写)\n");
	return ret;
}

int bp_diag_get(WIND_BP_DIAG *out)
{
	int i;

	if (!out)
		return -EINVAL;

	memset(out, 0, sizeof(*out));

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (hwbp_slots[i].active)
			out->hwbp_armed++;
		if (ptebp_slots[i].active)
			out->ptebp_armed++;
		if (wrpbp_slots[i].active)
			out->wrpbp_armed++;
	}

	out->last_hit_pc = g_bpc.last_hit_pc;
	out->hwbp_hits = g_bpc.hwbp_hits;
	out->hwbp_writes = g_bpc.hwbp_writes;
	out->hwbp_write_fails = g_bpc.hwbp_write_fails;
	out->ptebp_reloads = g_bpc.ptebp_reloads;
	out->ptebp_heal_ok = g_bpc.ptebp_heal_ok;
	out->ptebp_heal_fail = g_bpc.ptebp_heal_fail;
	out->wrpbp_hits = g_bpc.wrpbp_hits;
	out->wrpbp_writes = g_bpc.wrpbp_writes;
	out->wrpbp_skips = g_bpc.wrpbp_skips;
	out->wrpbp_fails = g_bpc.wrpbp_fails;
	out->last_rx = g_bpc.last_rx;
	out->last_ry = g_bpc.last_ry;
	out->last_rz = g_bpc.last_rz;

	/* 克隆页断点自己维护统计, 这里只是转手。
	 * 之前漏了转手, 客户端读 clonebp_active 拿到的是 0, 误判成"装载被回滚"。 */
	clonebp_get_diag(&out->clonebp_active, &out->clonebp_pages,
			 &out->clonebp_brk_hits, &out->clonebp_tlb_flush);
	return 0;
}

/* ===================================================================
 * 统一清理
 * =================================================================== */
void wind_bp_exit(void)
{
	hwbp_remove();
	ptebp_remove();
	wrpbp_remove();
	brkbp_remove();
	udpbp_remove(-1);

	if (g_udpbp_tx_kp)
	{
		wind_kprobe_unregister_symbol(g_udpbp_tx_kp);
		g_udpbp_tx_kp = NULL;
	}
	if (g_udpbp_rx_kp)
	{
		wind_kprobe_unregister_symbol(g_udpbp_rx_kp);
		g_udpbp_rx_kp = NULL;
	}
	udpbp_free_all();

	if (g_ptebp_kp)
	{
		wind_kprobe_unregister_symbol(g_ptebp_kp);
		g_ptebp_kp = NULL;
	}
	if (g_brkbp_kp)
	{
		wind_kprobe_unregister_symbol(g_brkbp_kp);
		g_brkbp_kp = NULL;
	}

}
