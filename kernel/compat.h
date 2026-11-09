#ifndef _KH_COMPAT_H_
#define _KH_COMPAT_H_

/*
 * compat.h — 内核版本兼容层
 * 目标: 5.4 / 5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18 (Android GKI)
 *
 * 集中收敛版本差异, 避免各 .c 散落 #ifdef。所有边界基于内核主线 API
 * 引入版本, 见每段注释。
 */

/* 6.10 起 include/linux/version.h 被删, LINUX_VERSION_CODE / KERNEL_VERSION 只剩
 * 生成头里这一份。kbuild 的 -I$(objtree)/include 能找到, 5.4~6.18 通用。 */
#include <generated/uapi/linux/version.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <asm/cacheflush.h>
#include <asm/cache.h>

/* ---- mmap 读锁 ----
 * 5.8 起 mmap_sem 重命名为 mmap_lock, 并新增 mmap_read_lock() 辅助函数。
 * 5.4 用 down_read(&mm->mmap_sem)。目标版本中 5.4 走旧 API, 5.10+ 走新 API。
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
#include <linux/mmap_lock.h>
#define kh_mmap_read_lock(mm)   mmap_read_lock(mm)
#define kh_mmap_read_unlock(mm) mmap_read_unlock(mm)
#else
#define kh_mmap_read_lock(mm)   down_read(&(mm)->mmap_sem)
#define kh_mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#endif

/* ---- get_task_mm 内联替代 ----
 * get_task_mm 不在官方 prebuilt .ko 的 __versions 段里 (modpost undefined),
 * 这里内联其等价逻辑 (task_lock + mmget, 均为 inline, 不依赖导出符号)。
 */
static inline struct mm_struct *kh_get_task_mm(struct task_struct *task)
{
	struct mm_struct *mm;

	task_lock(task);
	mm = task->mm;
	if (mm && !(task->flags & PF_KTHREAD))
		mmget(mm);
	else
		mm = NULL;
	task_unlock(task);
	return mm;
}

/* ---- p4d ----
 * 5-level 页表通用 API (p4d_offset/p4d_none/p4d_bad) 自 4.17 起可用,
 * 5.4 及以上全有。arm64 4-level 下 p4d 折叠 (p4d_t 即 pgd_t), p4d_offset
 * 是 no-op, 因此统一走 p4d 分支对 5.4~6.18 均正确。
 */
#define KH_HAVE_P4D 1

/* ---- arm64 ptep_get ----
 * arm64 5.10 引入 PTE 保护并随之上游 ptep_get(ptep); 5.4 无此宏, 直接
 * *ptep 解引用 (5.4 的 pte_t 无保护位, 解引用即真实值)。
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
#define kh_ptep_get(ptep) ptep_get(ptep)
#else
#define kh_ptep_get(ptep) (*(ptep))
#endif

/* ---- VMA 遍历 ----
 * 6.1 起引入 maple tree, 提供 vma_iterator / vma_iter_init / for_each_vma。
 * 6.1 之前用 mm->mmap 链表。
 */
#define KH_HAVE_VMA_ITER (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))

/* ---- get_user_pages_remote ----
 * 签名演变 (mm/gup 系列, 首参不一直是 tsk):
 *   5.4             : (tsk, mm, start, nr, flags, pages, vmas, locked)  8 参数
 *   5.9 起移除 tsk  : (mm, start, nr, flags, pages, vmas, locked)       7 参数
 *   6.5 起移除 vmas : (mm, start, nr, flags, pages, locked)             6 参数
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
#define kh_get_user_pages_remote(mm, start, nr, flags, pages) \
	get_user_pages_remote(mm, start, nr, flags, pages, NULL)
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
#define kh_get_user_pages_remote(mm, start, nr, flags, pages) \
	get_user_pages_remote(mm, start, nr, flags, pages, NULL, NULL)
#else
#define kh_get_user_pages_remote(mm, start, nr, flags, pages) \
	get_user_pages_remote(current, mm, start, nr, flags, pages, NULL, NULL)
#endif

/* ---- arm64 D-cache 维护 ----
 * 5.11 起 __flush_dcache_area 改名 dcache_clean_inval_poc 且不再 EXPORT_SYMBOL,
 * 模块不可直接引用。这里用内联 dc civac 指令统一实现 clean+invalidate 到 PoC,
 * 跨 5.4~6.18 一致, 不依赖任何导出符号。
 *
 * cache_line_size() 是数据符号 (ctr_el0 解析), 不在官方 prebuilt .ko 的
 * __versions 段里 (modpost undefined)。arm64 dc civac 按硬件实际 cache line
 * 操作, 步进取保守值 64 字节即可完整覆盖 (不会漏刷)。
 */
#define KH_DCACHE_LINE 64

static inline void kh_dcache_clean_inval_poc(void *addr, size_t size)
{
	unsigned long start = (unsigned long)addr;
	unsigned long end = start + size;

	/* 向下/向上对齐到 64 字节 cache line 边界, 完整覆盖 [addr, addr+size) */
	start &= ~(KH_DCACHE_LINE - 1);
	end = (end + KH_DCACHE_LINE - 1) & ~(KH_DCACHE_LINE - 1);

	for (; start < end; start += KH_DCACHE_LINE)
		asm volatile("dc civac, %0" : : "r" (start) : "memory");
	asm volatile("dsb sy" : : : "memory");
}

#define kh_flush_dcache(addr, size) kh_dcache_clean_inval_poc((addr), (size))

/* ---- arm64_use_ng_mappings 白名单外数据符号桩 ----
 * 6.1 的 __va/__phys_to_virt 旧路径经页表宏展开引用它 (kpti ng 映射开关),
 * 是数据符号且不在 GKI 冻结白名单, 直接引用会导致模块加载 ENOEXEC。
 * 本驱动只做页表遍历取物理地址, 不依赖 ng 映射判断, 这里重定向为常量 false
 * 消除符号引用。6.6 起该符号已从 __va 路径移除, 6.12 起彻底删除, 此桩无副作用。
 */
#ifdef arm64_use_ng_mappings
#undef arm64_use_ng_mappings
#endif
#define arm64_use_ng_mappings false

/* ---- kmap_local_page / kunmap_local ----
 * 5.11 才有 local 版 (可嵌套、返回地址按 task 记录)。5.10 及更早没有, 退到
 * kmap_atomic/kunmap_atomic: 同样是「关抢占、不可睡眠」的短临界区, 配对关系
 * 不变, 对 4K/64K 页都成立。
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)) && !defined(kmap_local_page)
#define kmap_local_page(page)	kmap_atomic(page)
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)) && !defined(kunmap_local)
#define kunmap_local(addr)	kunmap_atomic(addr)
#endif

/* ---- PTE 里的物理地址位 ----
 * 5.11 删掉 PTE_ADDR_MASK, 只留下 PTE_ADDR_LOW / PTE_ADDR_HIGH (后者仅在
 * CONFIG_ARM64_PA_BITS_52 下存在)。按可用性逐级收敛, 5.4~6.18 用同一份代码:
 *   - 老内核: 直接用 PTE_ADDR_MASK
 *   - 5.11+:  LOW | HIGH
 *   - 无 52 位 PA: LOW
 *   - 万一两个宏都没有: 按 4K 页 + 52 位 PA 的位形自己算, 保留 51 位以上的
 *     软件位 (PTE_SPECIAL 等), 只清物理地址
 */
#if defined(PTE_ADDR_MASK)
#define KH_PTE_PA_MASK	((pteval_t)PTE_ADDR_MASK)
#elif defined(PTE_ADDR_LOW) && defined(PTE_ADDR_HIGH)
#define KH_PTE_PA_MASK	((pteval_t)(PTE_ADDR_LOW | PTE_ADDR_HIGH))
#elif defined(PTE_ADDR_LOW)
#define KH_PTE_PA_MASK	((pteval_t)PTE_ADDR_LOW)
#else
#define KH_PTE_PA_MASK	((pteval_t)(PAGE_MASK & GENMASK_ULL(51, 0)))
#endif

/* ---- 物理地址 → PTE 存放形式 ----
 * 52 位 PA 的机器上 arm64 把物理地址 [51:50] 挪到 PTE 的 [9:8] (4K 页),
 * 直接 `| page_to_phys(p)` 会写错位。这里自己按位形拼 (与内核 __phys_to_pte_val
 * 等价), 只用 PTE_ADDR_HIGH / PTE_ADDR_HIGH_SHIFT 两个宏, 不依赖内核内联函数
 * 叫什么名字, 6.x 各版改名也不受影响。
 */
#if defined(CONFIG_ARM64_PA_BITS_52) && defined(PTE_ADDR_HIGH_SHIFT) && \
	defined(PTE_ADDR_HIGH)
#define KH_PHYS_TO_PTE(phys)						\
	((pteval_t)(((phys) | ((u64)(phys) >> PTE_ADDR_HIGH_SHIFT)) &	\
		    KH_PTE_PA_MASK))
#else
#define KH_PHYS_TO_PTE(phys)	((pteval_t)(phys))
#endif

#endif /* _KH_COMPAT_H_ */
