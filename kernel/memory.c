#include "memory.h"
#include "compat.h"
#include "sym.h"
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
#include <linux/vmalloc.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/cacheflush.h>

/* ===================================================================
 * vmap 物理内存映射 (替代 ioremap_cache/iounmap):
 *
 * arm64 的 ioremap_cache 是 static inline, 展开后引用 ioremap_prot
 * (白名单外) 和 __phys_to_virt (引用 memstart_addr/physvirt_offset
 * 数据符号), 均不能在 GKI 下编译期引用。这里改用 vmap 三件套
 * __get_vm_area_caller + ioremap_page_range + free_vm_area, 三者均
 * 白名单外, 由 sym.c 经 register_kprobe 运行时取地址 (wind_* 包装)。
 * PAGE_KERNEL 为 normal cacheable (等价 ioremap_cache 的 PROT_NORMAL)。
 * =================================================================== */
struct wind_phys_map {
	struct vm_struct *area;
	void *addr;
};

static struct wind_phys_map wind_map_phys(phys_addr_t pa, size_t size)
{
	struct wind_phys_map m = { .area = NULL, .addr = NULL };
	unsigned long addr, end;
	int ret;

	m.area = wind_get_vm_area_caller(size, VM_IOREMAP, VMALLOC_START, VMALLOC_END,
					__builtin_return_address(0));
	if (!m.area)
		return m;
	m.addr = m.area->addr;
	addr = (unsigned long)m.addr;
	end = addr + size;
	ret = wind_ioremap_page_range(addr, end, pa, PAGE_KERNEL);
	if (ret) {
		wind_free_vm_area(m.area);
		m.area = NULL;
		m.addr = NULL;
	}
	return m;
}

static void wind_unmap_phys(struct wind_phys_map *m)
{
	if (!m || !m->area)
		return;
	wind_free_vm_area(m->area);
	m->area = NULL;
	m->addr = NULL;
}

/* ===================================================================
 * 页表翻译: 5.4~6.18 统一走 p4d 折叠分支 (arm64 4-level, p4d == pgd)。
 *
 * 这里手动实现各级偏移, 不用 p4d_offset/pud_offset/pmd_offset/
 * pte_offset_kernel 内核宏 —— 它们的 *_page_vaddr 展开会引用 __va,
 * 进而引用 arm64_use_ng_mappings / physvirt_offset 等非白名单符号,
 * GKI 下报 modpost undefined。页表项物理地址用 wind_map_phys 映射
 * (vmap + PAGE_KERNEL, 等价 __va, 不依赖 memstart_addr)。
 * =================================================================== */
phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	phys_addr_t pa;
	struct wind_phys_map m;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	/* 页表项物理地址 -> vmap (cacheable, 等价 __va, 不依赖 memstart_addr) */
	pa = (phys_addr_t)(pgd_val(*pgd) & PHYS_MASK & (s32)PAGE_MASK);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return 0;
	pud = (pud_t *)m.addr + pud_index(va);
	if (pud_none(*pud) || pud_bad(*pud)) {
		wind_unmap_phys(&m);
		return 0;
	}

	pa = (phys_addr_t)(pud_val(*pud) & PHYS_MASK & (s32)PAGE_MASK);
	wind_unmap_phys(&m);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return 0;
	pmd = (pmd_t *)m.addr + pmd_index(va);
	if (pmd_none(*pmd)) {
		wind_unmap_phys(&m);
		return 0;
	}

	pa = (phys_addr_t)(pmd_val(*pmd) & PHYS_MASK & (s32)PAGE_MASK);
	wind_unmap_phys(&m);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return 0;
	pte = (pte_t *)m.addr + pte_index(va);

	if (pte_none(wind_ptep_get(pte))) {
		wind_unmap_phys(&m);
		return 0;
	}
	if (!pte_present(wind_ptep_get(pte))) {
		wind_unmap_phys(&m);
		return 0;
	}

	pa = (phys_addr_t)(pte_pfn(wind_ptep_get(pte)) << PAGE_SHIFT) +
	     (va & (PAGE_SIZE - 1));
	wind_unmap_phys(&m);
	return pa;
}

/* ===================================================================
 * 内核镜像 rodata 写保护绕过。
 * arm64 set_memory_rw 只接受线性映射/vmalloc 地址, 对内核镜像 rodata
 * (null_fops 所在段) 返回 -EINVAL。这里走当前 mm 的内核段页表
 * (swapper_pg_dir 共享), 直接清/置 PTE_RDONLY 后 flush TLB。
 * =================================================================== */
int wind_force_kernel_page_rw(unsigned long va, bool writable)
{
	struct mm_struct *mm = current->active_mm;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	phys_addr_t pa;
	struct wind_phys_map m;
	pteval_t val;

	va &= PAGE_MASK;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return -EINVAL;

	pa = (phys_addr_t)(pgd_val(*pgd) & PHYS_MASK & (s32)PAGE_MASK);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return -EINVAL;
	pud = (pud_t *)m.addr + pud_index(va);
	if (pud_none(*pud) || pud_bad(*pud)) {
		wind_unmap_phys(&m);
		return -EINVAL;
	}

	pa = (phys_addr_t)(pud_val(*pud) & PHYS_MASK & (s32)PAGE_MASK);
	wind_unmap_phys(&m);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return -EINVAL;
	pmd = (pmd_t *)m.addr + pmd_index(va);
	if (pmd_none(*pmd)) {
		wind_unmap_phys(&m);
		return -EINVAL;
	}

	pa = (phys_addr_t)(pmd_val(*pmd) & PHYS_MASK & (s32)PAGE_MASK);
	wind_unmap_phys(&m);
	m = wind_map_phys(pa, PAGE_SIZE);
	if (!m.addr)
		return -EINVAL;
	pte = (pte_t *)m.addr + pte_index(va);

	val = pte_val(wind_ptep_get(pte));
	if (writable)
		val &= ~(pteval_t)PTE_RDONLY;
	else
		val |= (pteval_t)PTE_RDONLY;
	set_pte(pte, __pte(val));
	wind_unmap_phys(&m);

	/* 刷全部内核 TLB (模块加载只执行一次, 保守最稳) */
	asm volatile("dsb ish\n\ttlbi vmalle1is\n\tdsb ish\n\tisb\n" ::: "memory");

	return 0;
}

/* ===================================================================
 * 快速路径: vmap + copy (cacheable, 等价 phys_to_virt 直读)。
 * GKI 拿不到 memstart_addr, 无法重写 phys_to_virt, 改用 wind_map_phys
 * (wind_get_vm_area_caller + wind_ioremap_page_range, PAGE_KERNEL cacheable,
 * 运行时解析)。进程上下文 (ioctl) 可睡眠, vmap 合法。
 * =================================================================== */
static bool read_physical_direct(phys_addr_t pa, void __user *buffer, size_t size)
{
	struct wind_phys_map m = wind_map_phys(pa, size);

	if (!m.addr)
		return false;
	if (copy_to_user(buffer, m.addr, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	wind_unmap_phys(&m);
	return true;
}

static bool write_physical_direct(phys_addr_t pa, const void __user *buffer, size_t size)
{
	struct wind_phys_map m = wind_map_phys(pa, size);

	if (!m.addr)
		return false;
	if (copy_from_user(m.addr, buffer, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	/*
	 * 写回主存并失效对应 cache 行, 保证 GPU/ION/非 cache 映射观察者可见,
	 * 修复 mm cache 别名导致的图形缓冲不一致 (黑屏)。
	 */
	kh_flush_dcache(m.addr, size);
	wind_unmap_phys(&m);
	return true;
}

/* ===================================================================
 * 安全路径: vmap + flush (读前 invalidate 防 stale, 近似无缓存语义)
 * ioremap_nocache (arm64 展开为 ioremap_prot, 白名单外) 已移除, 改用
 * vmap + dc civac 维持一致性; 完整 nocache 属性后续可加 prot 参数恢复。
 * =================================================================== */
static bool read_physical_nocache(phys_addr_t pa, void __user *buffer, size_t size)
{
	struct wind_phys_map m = wind_map_phys(pa, size);

	if (!m.addr)
		return false;
	kh_dcache_clean_inval_poc(m.addr, size); /* 读前 invalidate, 防 stale cache */
	if (copy_to_user(buffer, m.addr, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	wind_unmap_phys(&m);
	return true;
}

static bool write_physical_nocache(phys_addr_t pa, const void __user *buffer, size_t size)
{
	struct wind_phys_map m = wind_map_phys(pa, size);

	if (!m.addr)
		return false;
	if (copy_from_user(m.addr, buffer, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	kh_flush_dcache(m.addr, size);
	wind_unmap_phys(&m);
	return true;
}

/* ---- 模式自适应入口 ---- */
bool read_physical_address(phys_addr_t pa, void __user *buffer, size_t size, int mode)
{
	if (mode == MODE_SAFE)
		return read_physical_nocache(pa, buffer, size);
	return read_physical_direct(pa, buffer, size);
}

bool write_physical_address(phys_addr_t pa, const void __user *buffer, size_t size, int mode)
{
	if (mode == MODE_SAFE)
		return write_physical_nocache(pa, buffer, size);
	return write_physical_direct(pa, buffer, size);
}

/* ===================================================================
 * swap 兜底已移除: get_user_pages_remote / put_page / find_vma 均不在
 * 官方 prebuilt .ko 的 __versions 段 (modpost undefined), 且 swap 页
 * 换入属于辅助路径。核心物理内存读写不依赖换入, 缺页时直接失败。
 * =================================================================== */

/* ===================================================================
 * 进程内存读写 (使用会话缓存 task)
 * =================================================================== */
static struct mm_struct *session_get_mm(struct driver_session *sess)
{
	if (!sess || !sess->task)
		return NULL;
	return kh_get_task_mm(sess->task);
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
			/* 未映射 / swap 页: 不再换入 (辅助符号已移除), 直接失败 */
			break;
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
			break;

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
	wind_mmput(mm);
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
	wind_mmput(mm);
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

	wind_mmput(mm);
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

	wind_mmput(mm);
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
		wind_mmput(mm);
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
		if (wind_remap_pfn_range(vma, off, __phys_to_pfn(pa), PAGE_SIZE, vma->vm_page_prot)) {
			ret = -EAGAIN;
			break;
		}
	}

	wind_mmput(mm);
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
