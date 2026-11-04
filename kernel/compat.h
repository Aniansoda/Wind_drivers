#ifndef _KH_COMPAT_H_
#define _KH_COMPAT_H_

/*
 * compat.h — 内核版本兼容层
 * 目标: 5.4 / 5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18 (Android GKI)
 *
 * 集中收敛版本差异, 避免各 .c 散落 #ifdef。所有边界基于内核主线 API
 * 引入版本, 见每段注释。
 */

#include <linux/version.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/io.h>
#include <asm/cacheflush.h>

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

/* ---- p4d ----
 * 5-level 页表通用 API (p4d_offset/p4d_none/p4d_bad) 自 4.17 起可用,
 * 5.4 及以上全有。arm64 4-level 下 p4d 折叠 (p4d_t 即 pgd_t), p4d_offset
 * 是 no-op, 因此统一走 p4d 分支对 5.4~6.18 均正确。
 */
#define KH_HAVE_P4D 1

/* ---- VMA 遍历 ----
 * 6.1 起引入 maple tree, 提供 vma_iterator / vma_iter_init / for_each_vma。
 * 6.1 之前用 mm->mmap 链表。
 */
#define KH_HAVE_VMA_ITER (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))

/* ---- get_user_pages_remote ----
 * 6.5 起移除 vmas 参数 (commit "mm/gup: remove vmas parameter"):
 *   5.4~6.1: (mm, start, nr, flags, pages, vmas, locked)
 *   6.5~6.18: (mm, start, nr, flags, pages, locked)
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
#define kh_get_user_pages_remote(mm, start, nr, flags, pages) \
	get_user_pages_remote(mm, start, nr, flags, pages, NULL)
#else
#define kh_get_user_pages_remote(mm, start, nr, flags, pages) \
	get_user_pages_remote(mm, start, nr, flags, pages, NULL, NULL)
#endif

/* ---- arm64 D-cache 维护 ----
 * 5.11 起 arm64 重命名 cache 维护函数:
 *   __flush_dcache_area(addr, size) -> dcache_clean_inval_poc(start, end)
 * 语义一致: clean + invalidate D-cache 到 Point of Coherency。
 * 5.4/5.10 用旧名, 5.15+ 用新名。目标版本里 5.10 为旧 API。
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0))
#define kh_flush_dcache(addr, size) \
	dcache_clean_inval_poc((unsigned long)(addr), (unsigned long)(addr) + (size))
#else
#define kh_flush_dcache(addr, size) \
	__flush_dcache_area((addr), (size))
#endif

/* ---- ioremap_nocache ----
 * arm64 的 ioremap 默认即 non-cacheable device memory (PROT_DEVICE_nGnRE),
 * 没有 ioremap_nocache 变体 (那是 x86 的 API)。统一映射到 ioremap。
 */
#define kh_ioremap_nocache(addr, size) ioremap((addr), (size))

#endif /* _KH_COMPAT_H_ */
