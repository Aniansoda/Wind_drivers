#include "memory.h"
#include "compat.h"
#include <linux/tty.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/mman.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/cacheflush.h>

/* ===================================================================
 * 页表翻译: 5.4~6.18 统一走 p4d 分支。
 * p4d_offset 自 4.17 起可用, arm64 4-level 下 p4d 折叠 (p4d_t 即 pgd_t),
 * p4d_offset 是 no-op, 因此对所有目标内核版本均正确。
 * =================================================================== */
phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;
	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;
	pud = pud_offset(p4d, va);
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;
	pmd = pmd_offset(pud, va);
	if (pmd_none(*pmd))
		return 0;
	pte = pte_offset_kernel(pmd, va);
	if (pte_none(kh_ptep_get(pte)))
		return 0;
	if (!pte_present(kh_ptep_get(pte)))
		return 0;

	page_addr = (phys_addr_t)(pte_pfn(kh_ptep_get(pte)) << PAGE_SHIFT);
	page_offset = va & (PAGE_SIZE - 1);
	return page_addr + page_offset;
}

/* ===================================================================
 * 物理地址校验: 判断是否落在 kernel 线性映射区 (可 phys_to_virt 直接访问)。
 * arm64 用 pfn_valid 判断物理页是否有效。valid_phys_addr_range 是 x86 专用
 * API (arm64 无此导出符号), 不引入。普通 RAM 走 phys_to_virt 直读, 与进程
 * 页表同为 WB cache, PIPT 物理索引, 天然 coherent, 无 ioremap 别名。
 * =================================================================== */
static inline bool is_direct_map(phys_addr_t pa, size_t size)
{
	(void)size;
	return pfn_valid(__phys_to_pfn(pa));
}

/* ===================================================================
 * 快速路径: phys_to_virt + memcpy (cache coherent, 高性能低发热)
 * =================================================================== */
static bool read_physical_direct(phys_addr_t pa, void __user *buffer, size_t size)
{
	void *src = phys_to_virt(pa);

	if (copy_to_user(buffer, src, size))
		return false;
	return true;
}

static bool write_physical_direct(phys_addr_t pa, const void __user *buffer, size_t size)
{
	void *dst = phys_to_virt(pa);

	if (copy_from_user(dst, buffer, size))
		return false;
	/*
	 * 写回主存并失效对应 cache 行, 保证 GPU/ION/非 cache 映射观察者可见,
	 * 修复 mm cache 别名导致的图形缓冲不一致 (黑屏)。
	 */
	kh_flush_dcache(dst, size);
	return true;
}

/* ===================================================================
 * 安全路径: ioremap_nocache + flush (无缓存, 防 cache 侧信道, 发热)
 * =================================================================== */
static bool read_physical_nocache(phys_addr_t pa, void __user *buffer, size_t size)
{
	void *mapped = kh_ioremap_nocache(pa, size);
	if (!mapped)
		return false;
	if (copy_to_user(buffer, mapped, size)) {
		iounmap(mapped);
		return false;
	}
	iounmap(mapped);
	return true;
}

static bool write_physical_nocache(phys_addr_t pa, const void __user *buffer, size_t size)
{
	void *mapped = kh_ioremap_nocache(pa, size);
	if (!mapped)
		return false;
	if (copy_from_user(mapped, buffer, size)) {
		iounmap(mapped);
		return false;
	}
	kh_flush_dcache(mapped, size);
	iounmap(mapped);
	return true;
}

/* ---- 模式自适应入口 ---- */
bool read_physical_address(phys_addr_t pa, void __user *buffer, size_t size, int mode)
{
	if (mode == MODE_SAFE)
		return read_physical_nocache(pa, buffer, size);

	if (is_direct_map(pa, size))
		return read_physical_direct(pa, buffer, size);
	return read_physical_nocache(pa, buffer, size);
}

bool write_physical_address(phys_addr_t pa, const void __user *buffer, size_t size, int mode)
{
	if (mode == MODE_SAFE)
		return write_physical_nocache(pa, buffer, size);

	if (is_direct_map(pa, size))
		return write_physical_direct(pa, buffer, size);
	return write_physical_nocache(pa, buffer, size);
}

/* ===================================================================
 * swap 兜底: 页表翻译失败 (swap entry / 未映射) 时用 get_user_pages 换入
 * 解决 swap 页漏物资问题。返回 true 表示已换入。
 * =================================================================== */
static bool fault_in_page(struct mm_struct *mm, uintptr_t addr)
{
	struct vm_area_struct *vma;
	struct page *page = NULL;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start)
		return false;

	/*
	 * 触发缺页换入 (swap entry -> 物理页)。用 FOLL_GET 做读 fault,
	 * 对只读映射(代码段/只读数据)同样有效, 避免 FOLL_WRITE 的 COW 副作用。
	 * vmas 参数在 6.5 起移除, 由 compat.h 的 kh_get_user_pages_remote 统一。
	 */
	if (kh_get_user_pages_remote(mm, addr, 1, FOLL_GET, &page) < 0)
		return false;

	if (page)
		put_page(page);
	return true;
}

/* ===================================================================
 * 进程内存读写 (使用会话缓存 task)
 * =================================================================== */
static struct mm_struct *session_get_mm(struct driver_session *sess)
{
	if (!sess || !sess->task)
		return NULL;
	return get_task_mm(sess->task);
}

/*
 * 内部实现: 复用已获取的 mm, 不负责 mm 生命周期。
 * 逐页翻译 + 读写, 正确处理跨页 (VA 连续 ≠ PA 连续)。
 * 批量读写用这一层, 一次 get_task_mm 服务 N 个地址。
 */
static bool __read_mm(struct mm_struct *mm, uintptr_t addr, void __user *buffer, size_t size, int mode)
{
	while (size > 0)
	{
		phys_addr_t pa;
		size_t chunk;

		pa = translate_linear_address(mm, addr);
		if (!pa)
		{
			/* swap / 未映射: 尝试换入后重译 */
			if (!fault_in_page(mm, addr))
				break;
			pa = translate_linear_address(mm, addr);
			if (!pa)
			{
				/* 纯匿名未 fault 页: 返回零页内容 */
				if (!find_vma(mm, addr))
					break;
				chunk = min(size, (size_t)(PAGE_SIZE - (addr & (PAGE_SIZE - 1))));
				if (clear_user(buffer, chunk) != 0)
					break;
				addr += chunk;
				buffer += chunk;
				size -= chunk;
				continue;
			}
		}

		chunk = min(size, (size_t)(PAGE_SIZE - (addr & (PAGE_SIZE - 1))));
		if (!read_physical_address(pa, buffer, chunk, mode))
			break;

		addr += chunk;
		buffer += chunk;
		size -= chunk;
	}
	return size == 0;
}

static bool __write_mm(struct mm_struct *mm, uintptr_t addr, const void __user *buffer, size_t size, int mode)
{
	while (size > 0)
	{
		phys_addr_t pa;
		size_t chunk;

		pa = translate_linear_address(mm, addr);
		if (!pa)
		{
			if (!fault_in_page(mm, addr))
				break;
			pa = translate_linear_address(mm, addr);
			if (!pa)
				break;
		}

		chunk = min(size, (size_t)(PAGE_SIZE - (addr & (PAGE_SIZE - 1))));
		if (!write_physical_address(pa, buffer, chunk, mode))
			break;

		addr += chunk;
		buffer += chunk;
		size -= chunk;
	}
	return size == 0;
}

bool read_process_memory(struct driver_session *sess, uintptr_t addr, void __user *buffer, size_t size)
{
	struct mm_struct *mm;
	bool result;

	mm = session_get_mm(sess);
	if (!mm)
		return false;

	result = __read_mm(mm, addr, buffer, size, sess->read_mode);
	mmput(mm);
	return result;
}

bool write_process_memory(struct driver_session *sess, uintptr_t addr, const void __user *buffer, size_t size)
{
	struct mm_struct *mm;
	bool result;

	mm = session_get_mm(sess);
	if (!mm)
		return false;

	result = __write_mm(mm, addr, buffer, size, sess->read_mode);
	mmput(mm);
	return result;
}

/* ===================================================================
 * 批量读写: 一次 ioctl 处理多个离散地址, 且只取一次 mm
 * =================================================================== */
long read_process_memory_batch(struct driver_session *sess, PBATCH_REQUEST req)
{
	struct mm_struct *mm;
	uint32_t i;
	long done = 0;

	mm = session_get_mm(sess);
	if (!mm)
		return -ESRCH;

	for (i = 0; i < req->count; i++) {
		PBATCH_ITEM it = &req->items[i];
		if (it->size == 0 || it->buffer == NULL)
			continue;
		if (__read_mm(mm, it->addr, (void __user *)it->buffer, it->size, sess->read_mode))
			done++;
	}

	mmput(mm);
	return done;
}

long write_process_memory_batch(struct driver_session *sess, PBATCH_REQUEST req)
{
	struct mm_struct *mm;
	uint32_t i;
	long done = 0;

	mm = session_get_mm(sess);
	if (!mm)
		return -ESRCH;

	for (i = 0; i < req->count; i++) {
		PBATCH_ITEM it = &req->items[i];
		if (it->size == 0 || it->buffer == NULL)
			continue;
		if (__write_mm(mm, it->addr, (const void __user *)it->buffer, it->size, sess->read_mode))
			done++;
	}

	mmput(mm);
	return done;
}

/* ===================================================================
 * mmap: 把目标进程一段虚拟地址映射到用户态, 零 ioctl 直接指针读写
 * =================================================================== */
int mmap_process_memory(struct driver_session *sess, struct vm_area_struct *vma,
                        uintptr_t target_va, size_t length)
{
	struct mm_struct *mm;
	phys_addr_t pa;
	unsigned long off;
	uintptr_t va;
	int ret = 0;

	mm = session_get_mm(sess);
	if (!mm)
		return -ESRCH;

	if (sess->read_mode == MODE_SAFE) {
		mmput(mm);
		/* 安全映射不缓存, 逐页 ioremap_nocache 语义, 通过缺页回调处理 */
		return -EOPNOTSUPP;
	}

	/* 逐页 remap_pfn_range, 用目标物理页 */
	for (va = target_va, off = vma->vm_start;
	     va < target_va + length;
	     va += PAGE_SIZE, off += PAGE_SIZE) {

		pa = translate_linear_address(mm, va);
		if (!pa)
			break;
		if (remap_pfn_range(vma, off, __phys_to_pfn(pa), PAGE_SIZE, vma->vm_page_prot)) {
			ret = -EAGAIN;
			break;
		}
	}

	mmput(mm);
	return (off > vma->vm_start) ? 0 : ret;
}

/* ===================================================================
 * 会话管理
 * =================================================================== */
struct driver_session *session_create(void)
{
	struct driver_session *sess;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return NULL;
	spin_lock_init(&sess->lock);
	sess->read_mode = MODE_FAST;
	return sess;
}

void session_bind(struct driver_session *sess, pid_t pid)
{
	struct pid *pid_struct;
	struct task_struct *task;

	if (!sess)
		return;

	/* 解绑旧的 */
	if (sess->task) {
		put_task_struct(sess->task);
		sess->task = NULL;
	}

	pid_struct = find_get_pid(pid);
	if (!pid_struct)
		return;
	task = pid_task(pid_struct, PIDTYPE_PID); /* 不持引用, 下面显式 get_task_struct */
	put_pid(pid_struct);
	if (!task)
		return;

	get_task_struct(task);
	sess->task = task;
	sess->bound = true;
}

void session_destroy(struct driver_session *sess)
{
	if (!sess)
		return;
	if (sess->task) {
		put_task_struct(sess->task);
		sess->task = NULL;
	}
	sess->bound = false;
	kfree(sess);
}
