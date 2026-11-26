// SPDX-License-Identifier: GPL-2.0
/*
 * sharemem.c — 目标进程与调用者进程共享同一批物理页
 * 对齐 Satan ALLOCATE_MEMORY。
 *
 * 做法:
 *   1. alloc_pages 拿一批页(物理连续不必需, 逐页映射)
 *   2. 在目标进程 mm 里建一段匿名 VMA(kthread_use_mm + vm_mmap),
 *      再用 remap_pfn_range 把我们的页顶掉匿名页
 *   3. 在调用者进程 mm 里同样建 VMA 并 remap 同一批页
 *   两个 VA 指向同一物理页, 任一侧写入另一侧立即可见。
 */
#include <linux/module.h>

/* 由 entry.c 定义: 卸载中标志 */
extern bool g_wind_unloading;
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mman.h>   /* PROT_* / MAP_* / vm_mmap */
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/highmem.h>

#include "sharemem.h"
#include "comm.h"

#define SM_MAX_REGIONS   SHAREMEM_MAX_REGIONS

struct sm_region {
	struct mm_struct *target_mm;
	u64    target_va;
	u64    caller_va;
	u64    size;
	struct page **pages;
	unsigned int nr_pages;
	int    inited;
};

static DEFINE_MUTEX(g_sm_lock);
static struct sm_region g_sm_regions[SM_MAX_REGIONS];

/* ---- 用 kthread 借目标 mm 建 VMA ----
 * 进程上下文的 current->mm 不能直接换成目标 mm (会破坏 mm_users 计数,
 * 且 vm_mmap 内部还要拿 mmap_write_lock)。放独立 kthread 里 use_mm 最稳。
 */
struct sm_mmap_work {
	struct mm_struct *mm;
	unsigned long len;
	unsigned long prot;
	unsigned long addr;
	int ret;
	struct completion done;
};

static int sm_mmap_thread(void *arg)
{
	struct sm_mmap_work *w = arg;

	kthread_use_mm(w->mm);
	w->addr = vm_mmap(NULL, 0, w->len, w->prot,
			  MAP_PRIVATE | MAP_ANONYMOUS, 0);
	kthread_unuse_mm(w->mm);
	w->ret = IS_ERR_VALUE(w->addr) ? -ENOMEM : 0;
	if (w->ret)
		w->addr = 0;
	complete(&w->done);
	return 0;
}

/* 在 mm 里建一段 len 字节匿名 VMA, 返回 VA */
static unsigned long sm_mmap_in(struct mm_struct *mm, unsigned long len,
				unsigned long prot)
{
	struct sm_mmap_work w;
	struct task_struct *t;
	long wait;

	if (!mm || !len)
		return 0;

	memset(&w, 0, sizeof(w));
	w.mm = mm;
	w.len = len;
	w.prot = prot;
	init_completion(&w.done);

	t = kthread_run(sm_mmap_thread, &w, "wind-smmap");
	if (IS_ERR(t))
		return 0;

	wait = wait_for_completion_killable(&w.done);
	if (wait < 0) {
		/* 被信号打断: 线程还会跑完, 但不能等太久 */
		wait_for_completion(&w.done);
	}
	return w.ret ? 0 : w.addr;
}

/* 把 mm 里 [va, va+len) 的匿名页替换成我们持有的物理页 */
static int sm_remap_in(struct mm_struct *mm, unsigned long va,
		       struct page **pages, unsigned int nr)
{
	struct vm_area_struct *vma;
	unsigned int i;
	int ret = 0;

	if (mmap_write_lock_killable(mm))
		return -EINTR;

	vma = find_vma(mm, va);
	if (!vma || vma->vm_start > va || vma->vm_end < va + nr * PAGE_SIZE) {
		mmap_write_unlock(mm);
		return -EFAULT;
	}

	/* 直接 remap 覆盖匿名页 PTE。zap_page_range_single 不导出给模块,
	 * 这里不做显式 zap: remap_pfn_range 会重写该范围 PTE, 被覆盖的
	 * 匿名页随 vma 回收释放。 */
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_pgoff = 0;

	for (i = 0; i < nr; ++i) {
		ret = remap_pfn_range(vma, va + i * PAGE_SIZE,
				      page_to_pfn(pages[i]), PAGE_SIZE,
				      vma->vm_page_prot);
		if (ret)
			break;
	}

	mmap_write_unlock(mm);
	return ret;
}

/* ---- 对外接口 ---- */
int sharemem_alloc(struct mm_struct *target_mm, struct mm_struct *caller_mm,
		   u64 size, u32 prot, u64 *target_va_out, u64 *caller_va_out,
		   u64 *size_out)
{
	struct sm_region *r = NULL;
	struct page **pages;
	unsigned long len, tva, cva;
	unsigned int nr, i;
	int ret = 0;

	if (!target_mm || !caller_mm || !size)
		return -EINVAL;
	if (size > SHAREMEM_MAX_SIZE)
		return -EINVAL;

	len = PAGE_ALIGN(size);
	if (!len)
		return -EINVAL;
	nr = len >> PAGE_SHIFT;

	pages = kvcalloc(nr, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < nr; ++i) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto free_pages;
		}
	}

	/* 目标进程侧 */
	tva = sm_mmap_in(target_mm, len, prot ? prot : (PROT_READ | PROT_WRITE));
	if (!tva) {
		ret = -ENOMEM;
		goto free_pages;
	}
	ret = sm_remap_in(target_mm, tva, pages, nr);
	if (ret)
		goto free_pages;

	/* 调用者进程侧 */
	cva = sm_mmap_in(caller_mm, len, prot ? prot : (PROT_READ | PROT_WRITE));
	if (!cva) {
		ret = -ENOMEM;
		goto free_pages;
	}
	ret = sm_remap_in(caller_mm, cva, pages, nr);
	if (ret)
		goto free_pages;

	mutex_lock(&g_sm_lock);
	for (i = 0; i < SM_MAX_REGIONS; ++i) {
		if (!g_sm_regions[i].inited) {
			r = &g_sm_regions[i];
			break;
		}
	}
	if (!r) {
		mutex_unlock(&g_sm_lock);
		ret = -ENOSPC;
		goto free_pages;
	}
	r->target_mm = target_mm;
	r->target_va = tva;
	r->caller_va = cva;
	r->size = len;
	r->pages = pages;
	r->nr_pages = nr;
	r->inited = 1;
	mutex_unlock(&g_sm_lock);

	if (target_va_out)
		*target_va_out = tva;
	if (caller_va_out)
		*caller_va_out = cva;
	if (size_out)
		*size_out = len;

	pr_info("[sharemem] alloc target=%llx caller=%llx size=%lu\n",
		(unsigned long long)tva, (unsigned long long)cva, len);
	return 0;

free_pages:
	for (i = 0; i < nr; ++i) {
		if (pages[i])
			__free_page(pages[i]);
	}
	kvfree(pages);
	return ret;
}

int sharemem_free(struct mm_struct *target_mm, u64 target_va)
{
	struct sm_region *r = NULL;
	struct mm_struct *tmm = NULL;
	unsigned long tva = 0;
	unsigned int nr = 0, i;
	struct page **pages = NULL;
	int found = 0;

	mutex_lock(&g_sm_lock);
	for (i = 0; i < SM_MAX_REGIONS; ++i) {
		if (g_sm_regions[i].inited &&
		    g_sm_regions[i].target_va == (target_va & PAGE_MASK) &&
		    (!target_mm || g_sm_regions[i].target_mm == target_mm)) {
			r = &g_sm_regions[i];
			found = 1;
			break;
		}
	}
	if (!found) {
		mutex_unlock(&g_sm_lock);
		return -ENOENT;
	}
	/* 复制出来再释放, 避免持锁做 munmap */
	tmm = r->target_mm;
	tva = r->target_va;
	pages = r->pages;
	nr = r->nr_pages;
	memset(r, 0, sizeof(*r));
	mutex_unlock(&g_sm_lock);

	/* VMA 不在这里撤: vm_munmap/do_munmap 都不导出给模块。
	 * 目标进程自行 munmap 或退出时回收。我们只释放自己持有的页引用,
	 * 但必须保证释放后没人再访问 —— 所以页在 region 存续期内始终有效,
	 * free 只做记账清除。 */
	for (i = 0; i < nr; ++i) {
		if (pages && pages[i])
			__free_page(pages[i]);
	}
	if (pages)
		kvfree(pages);

	pr_info("[sharemem] free va=%llx pages=%u\n",
		(unsigned long long)tva, nr);
	return 0;
}

void sharemem_release_mm(struct mm_struct *mm)
{
	int i;

	if (!mm)
		return;

	mutex_lock(&g_sm_lock);
	for (i = 0; i < SM_MAX_REGIONS; ++i) {
		struct sm_region *r = &g_sm_regions[i];
		unsigned int k;

		if (!r->inited)
			continue;
		if (r->target_mm != mm && r->caller_va == 0)
			continue;
		for (k = 0; k < r->nr_pages; ++k) {
			if (r->pages && r->pages[k])
				__free_page(r->pages[k]);
		}
		if (r->pages)
			kvfree(r->pages);
		memset(r, 0, sizeof(*r));
	}
	mutex_unlock(&g_sm_lock);
}

void sharemem_module_exit(void)
{
	int i;

	mutex_lock(&g_sm_lock);
	for (i = 0; i < SM_MAX_REGIONS; ++i) {
		struct sm_region *r = &g_sm_regions[i];
		unsigned int k;

		if (!r->inited)
			continue;
		for (k = 0; k < r->nr_pages; ++k) {
			if (r->pages && r->pages[k])
				__free_page(r->pages[k]);
		}
		if (r->pages)
			kvfree(r->pages);
		memset(r, 0, sizeof(*r));
	}
	mutex_unlock(&g_sm_lock);
}

int sharemem_module_init(void)
{
	memset(g_sm_regions, 0, sizeof(g_sm_regions));
	return 0;
}
