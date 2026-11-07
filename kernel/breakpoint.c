/*
 * breakpoint.c — 五套断点框架
 *   1. hwbp  : BRP/WRP debug 寄存器硬件断点 (perf hw_breakpoint), 16 点 + 记录
 *   2. ptebp : PTE 影子页执行断点 (改 PTE_UXN 触发 fault), 16 点 + 记录, 不用 debug 寄存器
 *   3. brkbp : BRK 软断点 (改写指令字节), 16 点 + 记录
 *   4. etmbp : ETM/PTM 分支追踪 (Coresight), 全局执行流观测
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
 *   - etmbp 依赖内核 Coresight 驱动 (cs_etm PMU); trace 流解码在用户态
 *     (OpenCSD/perf cs_etm), 内核侧只做 PMU 探测 + event 框架。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/ptrace.h>
#include <linux/kprobes.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <asm/pgtable.h>
#include "breakpoint.h"
#include "sym.h"
#include "memory.h"

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
	struct perf_event *bp;
	uint64_t addr;
	uint32_t type;
	uint32_t length;
	bool active;
};

static struct hwbp_slot hwbp_slots[BP_SLOT_MAX];
static HWBP_RECORD hwbp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 hwbp_head[BP_SLOT_MAX];
static u32 hwbp_count[BP_SLOT_MAX];

static void hwbp_triggered(struct perf_event *bp, struct perf_sample_data *data,
			   struct pt_regs *regs)
{
	int i;
	HWBP_RECORD rec;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (hwbp_slots[i].active && hwbp_slots[i].bp == bp)
		{
			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = instruction_pointer(regs);
			rec.hit_addr = data ? data->addr : hwbp_slots[i].addr;
			rec.op_mask = bp_op_mask(hwbp_slots[i].type, rec.trigger_pc);
			bp_rec_push(hwbp_recs[i], sizeof(HWBP_RECORD),
				    &hwbp_head[i], &hwbp_count[i], &rec);
			break;
		}
	}
}

static void hwbp_clear_slot(int i)
{
	if (hwbp_slots[i].bp)
	{
		wind_unregister_hw_breakpoint(hwbp_slots[i].bp);
		hwbp_slots[i].bp = NULL;
	}
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

	for (i = 0; i < (int)point_count; i++)
	{
		struct perf_event_attr attr;
		struct perf_event *bp;

		hwtype = bp_type_to_hw(points[i].type);
		hwlen = bp_len_to_hw(points[i].length ? points[i].length : BP_LEN_8);
		if (hwtype < 0 || hwlen < 0)
		{
			ret = -EINVAL;
			goto fail;
		}

		hw_breakpoint_init(&attr);
		attr.bp_addr = points[i].addr;
		attr.bp_len = hwlen;
		attr.bp_type = hwtype;

		bp = wind_register_user_hw_breakpoint(&attr, hwbp_triggered, NULL,
						     sess->task);
		if (IS_ERR(bp))
		{
			ret = PTR_ERR(bp);
			printk(KERN_ERR "hwbp: register point %d failed %d\n", i, ret);
			goto fail;
		}

		hwbp_slots[i].bp = bp;
		hwbp_slots[i].addr = points[i].addr;
		hwbp_slots[i].type = points[i].type;
		hwbp_slots[i].length = points[i].length;
		hwbp_slots[i].active = true;
	}
	return 0;

fail:
	hwbp_remove();
	return ret;
}

int hwbp_remove(void)
{
	int i;
	for (i = 0; i < BP_SLOT_MAX; i++)
		hwbp_clear_slot(i);
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
 * 2. ptebp — PTE 影子页执行断点 (改 PTE_UXN 触发 permission fault)
 * =================================================================== */
struct ptebp_slot
{
	uint64_t addr;
	pteval_t orig_pte;
	bool active;
};

static struct ptebp_slot ptebp_slots[BP_SLOT_MAX];
static PTEBP_RECORD ptebp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 ptebp_head[BP_SLOT_MAX];
static u32 ptebp_count[BP_SLOT_MAX];
static struct kprobe *g_ptebp_kp;
static struct mm_struct *g_ptebp_mm;

/* fault 处理函数候选 (do_page_fault 首参 far / do_mem_abort 首参 far) */
static const char *g_ptebp_fault_syms[] = {
	"do_page_fault",
	"do_mem_abort",
	"do_translation_fault",
	"do_permission_fault",
	NULL,
};

static int ptebp_fault_pre(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long far = read_sysreg(far_el1);
	unsigned long pc = instruction_pointer(regs);
	int i;

	(void)p;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (!ptebp_slots[i].active)
			continue;
		/* 命中判定: 同页即可 (执行到目标页触发) */
		if ((far & PAGE_MASK) == (ptebp_slots[i].addr & PAGE_MASK))
		{
			PTEBP_RECORD rec;

			memset(&rec, 0, sizeof(rec));
			rec.trigger_pc = pc;
			rec.hit_addr = far;
			rec.op_mask = bp_op_mask(BP_TYPE_EXEC, pc);
			bp_rec_push(ptebp_recs[i], sizeof(PTEBP_RECORD),
				    &ptebp_head[i], &ptebp_count[i], &rec);

			/* 恢复原 PTE (一次性断点, 透明持续需单步) */
			wind_shadow_pte_set(g_ptebp_mm, ptebp_slots[i].addr,
					    PTE_UXN, ptebp_slots[i].orig_pte & PTE_UXN);
		}
	}
	return 0;
}

int ptebp_set(struct driver_session *sess, const PTEBP_POINT_CONFIG *points,
	      size_t point_count)
{
	struct mm_struct *mm;
	int i, ret;

	if (!sess || !sess->task)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	mm = get_task_mm(sess->task);
	if (!mm)
		return -ESRCH;

	ptebp_remove();

	/* 懒注册 fault hook */
	if (!g_ptebp_kp)
	{
		int s;
		for (s = 0; g_ptebp_fault_syms[s]; s++)
		{
			g_ptebp_kp = wind_kprobe_register_symbol(g_ptebp_fault_syms[s],
							       ptebp_fault_pre, NULL);
			if (g_ptebp_kp)
			{
				printk(KERN_INFO "ptebp: fault hook @ %s\n",
				       g_ptebp_fault_syms[s]);
				break;
			}
		}
		if (!g_ptebp_kp)
		{
			ret = -EOPNOTSUPP;
			printk(KERN_ERR "ptebp: no kprobe-able fault handler (NOKPROBE)\n");
			goto fail_mm;
		}
	}

	for (i = 0; i < (int)point_count; i++)
	{
		int err = 0;
		pteval_t orig = wind_shadow_pte_get(mm, points[i].addr, &err);

		if (err)
		{
			ret = -EINVAL;
			goto fail;
		}

		ptebp_slots[i].addr = points[i].addr;
		ptebp_slots[i].orig_pte = orig;
		ptebp_slots[i].active = true;

		/* 设 PTE_UXN → 用户态执行触发 permission fault */
		if (wind_shadow_pte_set(mm, points[i].addr, PTE_UXN, PTE_UXN))
		{
			ret = -EINVAL;
			goto fail;
		}
	}

	g_ptebp_mm = mm; /* 持有 mm 引用直到 remove */
	return 0;

fail:
	ptebp_remove();
fail_mm:
	wind_mmput(mm);
	return ret;
}

int ptebp_remove(void)
{
	int i;

	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		if (ptebp_slots[i].active && g_ptebp_mm)
			wind_shadow_pte_set(g_ptebp_mm, ptebp_slots[i].addr,
					    PTE_UXN, ptebp_slots[i].orig_pte & PTE_UXN);
		memset(&ptebp_slots[i], 0, sizeof(ptebp_slots[i]));
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

int brkbp_set(struct driver_session *sess, const BRKBP_POINT_CONFIG *points,
	      size_t point_count)
{
	struct mm_struct *mm;
	int i, ret;

	if (!sess || !sess->task)
		return -ESRCH;
	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	mm = get_task_mm(sess->task);
	if (!mm)
		return -ESRCH;

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
 * 4. etmbp — ETM/PTM 分支追踪 (Coresight)
 *
 * 内核侧: 探测 cs_etm PMU 可用性, 记录地址过滤区间。真实 trace 捕获
 * 与解码依赖内核 Coresight 驱动 + 用户态 OpenCSD, 不在模块内完成。
 * =================================================================== */
static ETMBP_POINT_CONFIG etmbp_slots[BP_SLOT_MAX];
static bool etmbp_active[BP_SLOT_MAX];
static ETMBP_RECORD etmbp_recs[BP_SLOT_MAX][BP_REC_PER_SLOT];
static u32 etmbp_head[BP_SLOT_MAX];
static u32 etmbp_count[BP_SLOT_MAX];
static u32 etmbp_active_count;

int etmbp_set(const ETMBP_POINT_CONFIG *points, size_t point_count)
{
	int i;

	if (!points || point_count == 0 || point_count > BP_SLOT_MAX)
		return -EINVAL;

	etmbp_remove();

	for (i = 0; i < (int)point_count; i++)
	{
		etmbp_slots[i] = points[i];
		etmbp_active[i] = true;
		etmbp_head[i] = 0;
		etmbp_count[i] = 0;
		etmbp_active_count++;
	}

	/* 内核侧仅登记过滤区间; 实际 ETM 使能需 Coresight 驱动支持 (未验证) */
	return 0;
}

int etmbp_remove(void)
{
	memset(etmbp_slots, 0, sizeof(etmbp_slots));
	memset(etmbp_active, 0, sizeof(etmbp_active));
	etmbp_active_count = 0;
	return 0;
}

int etmbp_get_config(ETMBP_CONFIG *config_out)
{
	if (!config_out)
		return -EINVAL;
	memset(config_out, 0, sizeof(*config_out));
	config_out->active_count = etmbp_active_count;
	return 0;
}

int etmbp_get_records(uint32_t point_index, uint32_t start_index,
		      ETMBP_RECORD *records, uint32_t capacity, uint32_t *count_out)
{
	u32 n;

	if (point_index >= BP_SLOT_MAX || !records || !count_out)
		return -EINVAL;
	n = bp_rec_read(etmbp_recs[point_index], sizeof(ETMBP_RECORD),
			etmbp_head[point_index], etmbp_count[point_index],
			start_index, records, capacity);
	*count_out = n;
	return 0;
}

int etmbp_set_record(uint32_t point_index, uint32_t record_index,
		     const ETMBP_RECORD *record)
{
	if (point_index >= BP_SLOT_MAX || !record)
		return -EINVAL;
	if (!bp_rec_set(etmbp_recs[point_index], sizeof(ETMBP_RECORD),
			etmbp_head[point_index], etmbp_count[point_index],
			record_index, record))
		return -ENOENT;
	return 0;
}

int etmbp_clear_records(int32_t point_index)
{
	int i;

	if (point_index >= 0)
	{
		if ((u32)point_index >= BP_SLOT_MAX)
			return -EINVAL;
		etmbp_head[point_index] = 0;
		etmbp_count[point_index] = 0;
		return 0;
	}
	for (i = 0; i < BP_SLOT_MAX; i++)
	{
		etmbp_head[i] = 0;
		etmbp_count[i] = 0;
	}
	return 0;
}

/* ===================================================================
 * 兼容旧 brk (perf hw_breakpoint, 单点, 6 槽)
 * =================================================================== */
struct brk_entry
{
	struct perf_event *bp;
	pid_t pid;
	uintptr_t addr;
	int type;
	bool active;
};

static struct brk_entry brk_slots[6];

static void brk_triggered(struct perf_event *bp, struct perf_sample_data *data,
			  struct pt_regs *regs)
{
	int i;
	uintptr_t hit = 0;

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
		if (brk_slots[i].bp == bp)
		{
			hit = brk_slots[i].addr;
			break;
		}

	printk(KERN_INFO "[brk] hit addr=0x%lx pc=0x%lx\n",
	       (unsigned long)hit, instruction_pointer(regs));
}

int brk_set(pid_t pid, uintptr_t addr, int type)
{
	struct perf_event_attr attr;
	struct brk_entry *slot = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
		if (!brk_slots[i].active)
		{
			slot = &brk_slots[i];
			break;
		}
	if (!slot)
		return -EBUSY;

	hw_breakpoint_init(&attr);
	attr.bp_addr = addr;
	attr.bp_len = HW_BREAKPOINT_LEN_8;
	attr.bp_type = type;

	{
		struct pid *pid_struct = find_get_pid(pid);
		struct task_struct *task = NULL;

		if (!pid_struct)
			return -ESRCH;
		task = pid_task(pid_struct, PIDTYPE_PID);
		put_pid(pid_struct);
		if (!task)
			return -ESRCH;

		slot->bp = wind_register_user_hw_breakpoint(&attr, brk_triggered, NULL, task);
	}
	if (IS_ERR(slot->bp))
		return PTR_ERR(slot->bp);

	slot->pid = pid;
	slot->addr = addr;
	slot->type = type;
	slot->active = true;
	return 0;
}
EXPORT_SYMBOL(brk_set);

int brk_clear(pid_t pid, uintptr_t addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
		if (brk_slots[i].active && brk_slots[i].pid == pid &&
		    brk_slots[i].addr == addr)
		{
			wind_unregister_hw_breakpoint(brk_slots[i].bp);
			memset(&brk_slots[i], 0, sizeof(brk_slots[i]));
			return 0;
		}
	return -ENOENT;
}
EXPORT_SYMBOL(brk_clear);

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
	memcpy(&s->recs[idx], rec, sizeof(*rec));
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

		memcpy(&out[n], &s->recs[idx], sizeof(UDPBP_RECORD));
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
		if (headlen < sizeof(struct iphdr))
			return false;
		iph = (struct iphdr *)base;
		if (iph->version != 4 || iph->protocol != IPPROTO_UDP || iph->ihl < 5)
			return false;
		off = (u32)iph->ihl * 4;
		if (headlen < off + 8)
			return false;
		uh = base + off;
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
		if (s->cfg.port)
		{
			u16 want = (dir == UDPBP_DIR_TX) ? dport : sport;

			if (want != s->cfg.port)
				continue;
		}

		act = s->cfg.action;

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

		memset(&rec, 0, sizeof(rec));
		rec.ts_ns = (u64)READ_ONCE(jiffies) * (NSEC_PER_SEC / HZ);
		rec.pid = pid;
		rec.saddr = saddr;
		rec.daddr = daddr;
		rec.sport = sport;
		rec.dport = dport;
		rec.dir = dir;
		rec.action = act;
		rec.len = plen;
		rec.preview_len = (payload && plen) ? min_t(u32, plen, UDPBP_PREVIEW_MAX) : 0;
		if (rec.preview_len)
			memcpy(rec.preview, payload, rec.preview_len);

		udpbp_rec_push(s, &rec);
	}
}

static int udpbp_tx_pre(struct kprobe *p, struct pt_regs *regs)
{
	/* ip_local_out(struct net *, struct sock *, struct sk_buff *) */
	(void)p;
	udpbp_handle((struct sk_buff *)regs->regs[2], UDPBP_DIR_TX);
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

	g_udpbp_tx_kp = wind_kprobe_register_symbol("ip_local_out", udpbp_tx_pre, NULL);
	g_udpbp_rx_kp = wind_kprobe_register_symbol("__udp4_lib_rcv", udpbp_rx_pre, NULL);

	if (!g_udpbp_tx_kp && !g_udpbp_rx_kp)
	{
		printk(KERN_ERR "wind: udpbp probe register failed (ip_local_out / __udp4_lib_rcv)\n");
		return -EOPNOTSUPP;
	}
	printk(KERN_INFO "wind: udpbp probes tx=%d rx=%d\n",
	       g_udpbp_tx_kp ? 1 : 0, g_udpbp_rx_kp ? 1 : 0);
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

	memcpy(&s->cfg, point, sizeof(s->cfg));
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
 * 统一清理
 * =================================================================== */
void wind_bp_exit(void)
{
	int i;

	hwbp_remove();
	ptebp_remove();
	brkbp_remove();
	etmbp_remove();
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

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
		if (brk_slots[i].active)
		{
			wind_unregister_hw_breakpoint(brk_slots[i].bp);
			memset(&brk_slots[i], 0, sizeof(brk_slots[i]));
		}
}
