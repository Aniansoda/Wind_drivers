#include "process.h"
#include "compat.h"
#include "sym.h"
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/sched/mm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/string.h>

#define ARC_PATH_MAX 256

uintptr_t get_module_base(struct driver_session *sess, char *name)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	uintptr_t base_addr = 0;
#if KH_HAVE_VMA_ITER
	struct vma_iterator vmi;
#endif

	if (!sess || !sess->task)
		return 0;

	mm = kh_get_task_mm(sess->task);
	if (!mm)
		return 0;

	/* 遍历 VMA 需持 mmap 读锁 (vma_iterator 与链表遍历均要求) */
	kh_mmap_read_lock(mm);
#if KH_HAVE_VMA_ITER
	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
#else
	for (vma = mm->mmap; vma; vma = vma->vm_next)
#endif
	{
		char buf[ARC_PATH_MAX];
		char *path_nm = "";

		if (vma->vm_file)
		{
			path_nm = wind_file_path(vma->vm_file, buf, ARC_PATH_MAX - 1);
			if (!IS_ERR(path_nm) && !strcmp(kbasename(path_nm), name))
			{
				/* 加载基址 ≠ 这个 VMA 的起点: bionic 按 PT_LOAD 分段映射,
				 * 每段的起点是 base + (p_vaddr & PAGE_MASK), vm_pgoff 记录
				 * 该段在文件里的偏移。不减掉它, 命中重定位段时算出的
				 * 「基址」偏出去一整段, 断点地址跟着错位 -> 永远零命中。 */
				base_addr = vma->vm_start -
					    (uintptr_t)(vma->vm_pgoff << PAGE_SHIFT);
				break;
			}
		}
	}
	kh_mmap_read_unlock(mm);

	wind_mmput(mm);
	return base_addr;
}
