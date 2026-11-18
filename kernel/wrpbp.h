#ifndef _WRPBP_H_
#define _WRPBP_H_

#include <linux/types.h>
#include "comm.h"   /* WRPBP_* 协议结构 / 常量 / CLONEBP_FREG_MAX */

/*
 * WRP 数据断点 (Watchpoint Register Pair 观察点 + 浮点寄存器写回)
 *
 * 盯住「弹道/朝向向量」那块内存 (12 字节: x/y/z 三个 float), 游戏每帧重算
 * 准心写一次, 命中后立刻把目标向量写回, 并可选地同步写 S 寄存器 (fpsimd)。
 * 不碰代码字节、不动页表、不占 BRP 执行断点、不产生异常信号。
 *
 * 只占 arm64 WRP 硬件观察点寄存器 (整机 4 个), 每个追踪点最多 1 个事件。
 */

#ifdef __KERNEL__

struct task_struct;

/* ---- 生命周期 ---- */
int  wrpbp_module_init(void);
void wrpbp_module_exit(void);

/* ---- 对外接口 ---- */
/* OP_WRPBP_SET: ureq 是 WRPBP_SET_REQUEST {pid, point_count, points[]} */
int  wrpbp_set_pid(uint32_t pid_arg, const void __user *ureq);
int  wrpbp_remove(void);
/* OP_WRPBP_SET_VEC: 设内存写回向量 (point_index=-1 全部) */
int  wrpbp_set_vec(int32_t point_index, u32 enable, u32 x, u32 y, u32 z, u32 mask);
/* OP_WRPBP_SET_FREGS: 设浮点寄存器写回 (point_index=-1 全部) */
int  wrpbp_set_fregs(int32_t point_index, u32 freg_base, u32 freg_mask,
		     const u32 *vals, u32 count);
int  wrpbp_get_config(WRPBP_CONFIG *out);
int  wrpbp_get_records(uint32_t point_index, uint32_t start_index,
		       HWBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int  wrpbp_clear_records(int32_t point_index);

#endif /* __KERNEL__ */
#endif /* _WRPBP_H_ */
