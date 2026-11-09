#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/vmalloc.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include "comm.h"

/*
 * 会话结构: open 后绑定, close 释放。
 * 缓存 task 引用避免每次 find_get_pid + get_pid_task;
 * mm 每次 get_task_mm (轻量, 且规避 mm 生命周期与 exec 换 mm 的野指针问题)。
 */
/* vmap/ioremap 出来的物理页映射, 定义放这里因为 wind_pte_pin 内嵌它 */
struct wind_phys_map {
	struct vm_struct *area;
	void *addr;
};

struct driver_session
{
	struct task_struct *task; /* get_task_struct 持有引用 */
	int read_mode;            /* MODE_FAST / MODE_SAFE */
	bool bound;
	spinlock_t lock;          /* 保护后续加入的页缓存/批量状态 */
};

/* ---- 页映射缓存统计 (OP_STATS 返回) ---- */
/* 这个宏跟着结构体一起出现在头文件里。
 * memory.c 用它判断「头文件是不是同一版」: 仓库里 memory.c 更新而 memory.h
 * 没同步时, 编译不会因为多了两个字段而挂, 只是统计里少两项。 */
#define WIND_HAVE_TR_STATS 1

struct wind_map_stats
{
	uint64_t reads;   /* 走物理读的总次数 */
	uint64_t hits;    /* 缓存命中 */
	uint64_t misses;  /* 新建映射 */
	uint64_t evicts;  /* 淘汰/过期回收 */
	uint64_t cached;  /* 当前驻留条目数 */
	uint64_t tr_hits;   /* VA->PA 翻译缓存命中 */
	uint64_t tr_misses; /* 翻译缓存未命中(需遍历页表) */
};

void wind_map_cache_purge(void);
void wind_map_cache_get_stats(struct wind_map_stats *out);
void wind_tr_cache_purge(void);

/* ---- 页表翻译 ---- */
phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va);

/* ---- 内核镜像 rodata 写保护绕过 (set_memory_rw 不接受内核镜像地址) ---- */
int wind_force_kernel_page_rw(unsigned long va, bool writable);
/* 只读探测: 校验整条页表路径后读叶子 AP[2], 不写页表 */
int wind_kernel_page_ro_state(unsigned long va, bool *is_ro);

/* ---- 影子页操作 (直接改目标进程 PTE 权限位, 无痕断点) ----
 * mask/val 为 pteval_t 位域 (PTE_UXN / PTE_PXN / PTE_RDONLY / PTE_VALID / PTE_AF)。
 * 遍历目标 mm 页表定位 PTE, 按 (old & ~mask) | (val & mask) 改写后刷 TLB。
 * get 返回当前 PTE 值, err 置 0/负 errno。
 */
int wind_shadow_pte_set(struct mm_struct *mm, unsigned long va, pteval_t mask, pteval_t val);
pteval_t wind_shadow_pte_get(struct mm_struct *mm, unsigned long va, int *err);

/* 页表项钉住: 装断点时把 PTE 所在页 vmap 一次并留着, 之后断层上下文里
 * 只做一次 WRITE_ONCE + TLBI, 不再 get_vm_area/ioremap (会睡眠)。
 * mm 必须活到 unpin; 页表页在 mm 存活期间不会被释放。 */
struct wind_pte_pin {
	struct wind_phys_map map;   /* 必须放在第一个, 便于整体清零 */
	pteval_t *pte;
	bool valid;
};
int wind_pte_pin(struct mm_struct *mm, unsigned long va, struct wind_pte_pin *pin);
void wind_pte_unpin(struct wind_pte_pin *pin);
pteval_t wind_pte_pin_get(struct wind_pte_pin *pin);
void wind_pte_pin_write(struct wind_pte_pin *pin, unsigned long va,
			pteval_t mask, pteval_t val);

/* ---- 内核态跨进程读写 (非 __user, 供 BRK 断点改指令用) ---- */
bool wind_read_process_va(struct mm_struct *mm, uintptr_t va, void *buf, size_t size);
bool wind_write_process_va(struct mm_struct *mm, uintptr_t va, const void *buf, size_t size);

/* 代码页改写: GUP(FOLL_WRITE) 做出私有副本后改字节 + 刷 I/D cache。
 * 内联 hook 的唯一写入口, 进程上下文调用。 */
int wind_patch_process_code(struct mm_struct *mm, unsigned long va,
			    const void *buf, size_t len);
/* 在目标 mm 里拿/还一页 RWX 跳板 (工作队列里 kthread_use_mm + vm_mmap) */
unsigned long wind_map_exec_page_in(struct mm_struct *mm, unsigned long hint);
int wind_unmap_in(struct mm_struct *mm, unsigned long addr, unsigned long len);

/* 改完字节后刷 I/D cache (影子页 / 跳板页) */
void wind_flush_code(void *kaddr, size_t len);

/* ---- 影子页: 私有副本映射到目标 VA, 原页与文件不动 ---- */
struct wind_shadow {
	struct page *page;   /* 影子页 (我们自己 alloc 的) */
	struct page *orig;   /* 原页 (pin 住, 卸载时映射回去) */
	struct wind_pte_pin pin;
	pteval_t saved;      /* 原始 PTE 值 */
	void *kva;           /* 影子页内核 VA */
	bool remapped;
	bool exec_ok;        /* 原 PTE 允许 EL0 取指 (UXN=0), 跳板才能搬进来 */
};
int wind_shadow_attach(struct mm_struct *mm, unsigned long va, struct wind_shadow *sh);
int wind_shadow_detach(struct mm_struct *mm, unsigned long va, struct wind_shadow *sh);

/* ---- 物理读写 (模式自适应) ---- */
bool read_physical_address(phys_addr_t pa, void __user *buffer, size_t size, int mode);
bool write_physical_address(phys_addr_t pa, const void __user *buffer, size_t size, int mode);

/* ---- 进程内存读写 ---- */
bool read_process_memory(struct driver_session *sess, uintptr_t addr, void __user *buffer, size_t size);
bool write_process_memory(struct driver_session *sess, uintptr_t addr, const void __user *buffer, size_t size);

/* ---- 批量读写 ---- */
long read_process_memory_batch(struct driver_session *sess, PBATCH_REQUEST req);
long write_process_memory_batch(struct driver_session *sess, PBATCH_REQUEST req);

/* ---- mmap 映射一段目标虚拟地址到用户态 ---- */
int mmap_process_memory(struct driver_session *sess, struct vm_area_struct *vma,
                        uintptr_t target_va, size_t length);

/* ---- 会话管理 ---- */
struct driver_session *session_create(void);
void session_bind(struct driver_session *sess, pid_t pid);
void session_destroy(struct driver_session *sess);

#endif /* _MEMORY_H_ */
