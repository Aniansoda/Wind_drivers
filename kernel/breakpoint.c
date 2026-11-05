/*
 * breakpoint.c — brk 硬件断点框架 (内核 perf hw_breakpoint API)
 *
 * 用途: 在目标进程指定虚拟地址设置硬件执行/读写断点, 命中时回调,
 * 用于追踪函数调用 (如定位反作弊检测点) 或数据访问。
 *
 * 依赖内核 CONFIG_HAVE_HW_BREAKPOINT + CONFIG_PERF_EVENTS (GKI 默认开)。
 * ARM64 硬件断点数量有限 (通常 6 个), 需控制并发。
 *
 * 未验证项:
 *   1. register_user_hw_breakpoint 在 5.x/6.x 签名一致, 但触发回调的
 *      perf_sample_data 布局随版本微调, 读取命中地址需按版本核对。
 *   2. 执行断点 (HW_BREAKPOINT_X) 在 ARM64 上依赖 debug 寄存器, 受
 *      hw_breakpoint 全局资源池限制。
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
#include <linux/ptrace.h>
#include <asm/ptrace.h>
#include "breakpoint.h"
#include "sym.h"

struct brk_entry
{
	struct perf_event *bp;
	pid_t pid;
	uintptr_t addr;
	int type;
	bool active;
};

static struct brk_entry brk_slots[6]; /* ARM64 硬件断点上限 */

static void brk_triggered(struct perf_event *bp, struct perf_sample_data *data,
                          struct pt_regs *regs)
{
	int i;
	uintptr_t hit = 0;

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
	{
		if (brk_slots[i].bp == bp)
		{
			hit = brk_slots[i].addr;
			break;
		}
	}

	/* 命中回调: 记录或发信号给用户态, 这里仅打印 */
	printk(KERN_INFO "[brk] hit addr=0x%lx pc=0x%lx\n",
	       (unsigned long)hit, instruction_pointer(regs));
}

/* 设置硬件断点: type = HW_BREAKPOINT_X / _R / _W / _RW */
int brk_set(pid_t pid, uintptr_t addr, int type)
{
	struct perf_event_attr attr;
	struct brk_entry *slot = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(brk_slots); i++)
	{
		if (!brk_slots[i].active)
		{
			slot = &brk_slots[i];
			break;
		}
	}
	if (!slot)
		return -EBUSY;

	hw_breakpoint_init(&attr);
	attr.bp_addr = addr;
	attr.bp_len = HW_BREAKPOINT_LEN_8; /* x64/arm64 执行断点用 8 */
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
	{
		if (brk_slots[i].active && brk_slots[i].pid == pid &&
		    brk_slots[i].addr == addr)
		{
			wind_unregister_hw_breakpoint(brk_slots[i].bp);
			memset(&brk_slots[i], 0, sizeof(brk_slots[i]));
			return 0;
		}
	}
	return -ENOENT;
}
EXPORT_SYMBOL(brk_clear);
