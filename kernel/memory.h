#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/spinlock.h>
#include "comm.h"

/*
 * 会话结构: open 后绑定, close 释放。
 * 缓存 task 引用避免每次 find_get_pid + get_pid_task;
 * mm 每次 get_task_mm (轻量, 且规避 mm 生命周期与 exec 换 mm 的野指针问题)。
 */
struct driver_session
{
	struct task_struct *task; /* get_task_struct 持有引用 */
	int read_mode;            /* MODE_FAST / MODE_SAFE */
	bool bound;
	spinlock_t lock;          /* 保护后续加入的页缓存/批量状态 */
};

/* ---- 页表翻译 ---- */
phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va);

/* ---- 内核镜像 rodata 写保护绕过 (set_memory_rw 不接受内核镜像地址) ---- */
int wind_force_kernel_page_rw(unsigned long va, bool writable);

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
