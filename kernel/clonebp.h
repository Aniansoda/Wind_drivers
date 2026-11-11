#ifndef _CLONEBP_H_
#define _CLONEBP_H_

#include <linux/types.h>
#include "comm.h"   /* 协议常量 / 结构 / 统计索引统一来自 comm.h */

/*
 * 克隆页断点 (Clone-Page Breakpoint)
 * 同 VA 双物理页:
 *   原始页 PTE_USER|PTE_RDONLY|PTE_UXN -> 用户读看到干净指令, 不可执行
 *   影子页 无 PTE_USER, 可执行        -> CPU 取指走这里, 内含 BRK
 * 读 -> Data Abort -> 切原始页; 执行 -> Instruction Abort -> 切影子页。
 * 不占 BRP/WRP 寄存器, 不改原始页字节。
 */

#ifdef __KERNEL__

#include <linux/mm.h>        /* pte_t / struct page / struct mm_struct 前置声明 */
#include <linux/spinlock.h>  /* spinlock_t */
#include <linux/sched.h>     /* struct task_struct */

struct task_struct;
struct mm_struct;
struct pt_regs;

struct clonebp_page {
	uint64_t va_page;              /* 页对齐起始虚拟地址 */
	unsigned long user_va;         /* 用户虚拟地址(页对齐) */
	struct page *orig_page[2];     /* 原始页(compat: 两槽) */
	struct page *shadow_page;      /* 克隆出的影子页 */
	pte_t *ptep;                   /* 页表项指针 */
	spinlock_t lock;               /* 该页状态切换锁 */
	atomic_t state;                /* 0=orig 1=shadow */
	unsigned int refcnt;           /* 引用该页的点数 */
	int inited;
};

struct clonebp_point {
	uint64_t user_addr;            /* 目标函数入口 VA */
	uint64_t page_base;            /* 所属页基址 */
	struct clonebp_page *page;     /* 页句柄 */
	uint32_t mode;
	uint32_t reg_base;
	uint32_t reg_mask;
	uint64_t reg_val[CLONEBP_REG_MAX];
	uint32_t imm;                  /* BRK 立即数, 用于区分点 */
	uint32_t flags;
	uint64_t trigger_pc;           /* 最近命中 PC */
	uint64_t hit_addr;
	uint64_t hits;
	uint64_t last_jiffies;
	int active;
};

/* ---- 生命周期 ---- */
int  clonebp_module_init(void);
void clonebp_module_exit(void);

/* ---- 对外接口 ---- */
int  clonebp_set_pid(uint32_t pid, const void __user *ureq);
int  clonebp_remove_all(struct mm_struct *mm);
int  clonebp_clear_records(struct mm_struct *mm, int32_t point_index);
int  clonebp_refresh(struct mm_struct *mm);
int  clonebp_get_config(void *uout);
int  clonebp_get_records(uint32_t point_index, uint32_t start,
			 CLONEBP_RECORD *recs, uint32_t cap, uint32_t *count_out);

/* fork 保护: 复制页表前暂停影子映射, 复制后只恢复父进程 */
void clonebp_fork_pause(struct mm_struct *mm);
void clonebp_fork_resume(struct mm_struct *mm);

/* 卸载/进程退出清理钩子 */
void clonebp_release_mm(struct mm_struct *mm);

#endif /* __KERNEL__ */
#endif /* _CLONEBP_H_ */
