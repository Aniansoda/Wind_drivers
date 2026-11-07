#include "memory.h"
#include "compat.h"
#include "sym.h"
#include <linux/kconfig.h>
#include <linux/tty.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/mman.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/cacheflush.h>
#include <asm/sysreg.h>

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

/* 页表层数: GKI arm64 4KB 页下 VA_BITS=39 是 3 级 (pgd -> pmd -> pte),
 * VA_BITS=48 是 4 级 (pgd -> pud -> pmd -> pte)。3 级时没有 pud 层,
 * 顶层项的下一级表直接就是 pmd 表, 必须用 pmd_index(shift 21) 索引;
 * 若按 pud_index(shift 30) 去索引这张 pmd 表会落到空项 (表现为 pud none)。 */
#if defined(CONFIG_PGTABLE_LEVELS) && (CONFIG_PGTABLE_LEVELS > 0)
#define WIND_PGTABLE_LEVELS CONFIG_PGTABLE_LEVELS
#else
#define WIND_PGTABLE_LEVELS 4
#endif

/* 每级索引位数 = PAGE_SHIFT - 3 (4KB 页 9 位, 16KB 页 11 位, 64KB 页 13 位);
 * 第 lv 级 (0=pte) 的移位 = PAGE_SHIFT + lv * bits。与内核 VA_BITS 无关,
 * 因此 3/4/5 级页表共用这一套索引。 */
#define WIND_PT_BITS	(PAGE_SHIFT - 3)
#define WIND_PT_IDXMASK	((1UL << WIND_PT_BITS) - 1)
#define WIND_PTE_AF	((pteval_t)1 << 10) /* arm64 末级(page/block) descriptor 的 AF */

static inline unsigned long wind_lidx(unsigned long va, int lv)
{
	return (va >> (PAGE_SHIFT + (unsigned int)lv * WIND_PT_BITS)) & WIND_PT_IDXMASK;
}

/* 运行时确定内核页表层数。
 * 首选 TCR_EL1.T1SZ (硬件真实配置: 64 - T1SZ = TTBR1 的 VA 位宽),
 * 取不到才退回 task_size。VA_BITS=39 -> 3 级, 48 -> 4 级, 52 -> 5 级。 */
static int wind_pgtable_levels_cached;

/* TCR_EL1.T1SZ = bits[21:16] */
static int wind_ttbr1_vabits(void)
{
	u64 tcr = read_sysreg(tcr_el1);
	u32 t1sz = (u32)((tcr >> 16) & 0x3fUL);
	int vabits = 64 - (int)t1sz;

	if (vabits < 32 || vabits > 52)
		return 0;
	return vabits;
}

static int wind_pgtable_levels(void)
{
	int lv = READ_ONCE(wind_pgtable_levels_cached);
	int vabits, n;

	if (lv)
		return lv;

	vabits = wind_ttbr1_vabits();
	if (!vabits && current && current->mm) {
		unsigned long ts = current->mm->task_size;

		if (ts >= (1UL << 32))
			vabits = 63 - __builtin_clzll((unsigned long long)ts);
	}
	if (vabits < 25)
		vabits = 39; /* 兜底: 按 3 级处理 */

	n = (vabits - PAGE_SHIFT + WIND_PT_BITS - 1) / WIND_PT_BITS;
	if (n < 3 || n > 5)
		n = 3;

	WRITE_ONCE(wind_pgtable_levels_cached, n);
	pr_info("wind: vabits=%d pgtables=%d level(s)\n", vabits, n);
	return n;
}

/* 按指定层数走目标 mm 的页表, 到 pte 级返回该项指针。
 * 成功且 out_m 非空时, *out_m 持有该 pte 表的 vmap, 由调用方 unmap。 */
static pte_t *wind_mm_pte_locate_at(struct mm_struct *mm, unsigned long va,
				    int levels, struct wind_phys_map *out_m)
{
	pgd_t *pgd;
	phys_addr_t pa;
	struct wind_phys_map m;
	pteval_t val, type;
	int lv;

	if (!mm || levels < 3 || levels > 5)
		return NULL;

	va &= PAGE_MASK;

	/* 顶层直接用 mm->pgd (已在直接映射区, 无需 vmap) */
	pgd = mm->pgd + wind_lidx(va, levels - 1);
	val = pgd_val(*pgd);
	/* 顶层是表描述符: bit10 不是 AF, 只校验类型 */
	if (!val || (val & 0x3) != 0x3)
		return NULL;

	pa = (phys_addr_t)(val & PHYS_MASK & (s32)PAGE_MASK);
	if (!pa || !IS_ALIGNED(pa, PAGE_SIZE))
		return NULL;

	for (lv = levels - 2; lv >= 0; lv--) {
		pteval_t *entry;

		m = wind_map_phys(pa, PAGE_SIZE);
		if (!m.addr)
			return NULL;

		entry = (pteval_t *)m.addr + wind_lidx(va, lv);
		val = READ_ONCE(*entry);
		if (!val) {
			wind_unmap_phys(&m);
			return NULL;
		}
		type = val & 0x3;

		if (lv == 0) {
			/* pte 级: 0b11 = 有效页 */
			if (type != 0x3) {
				wind_unmap_phys(&m);
				return NULL;
			}
			if (out_m)
				*out_m = m;
			else
				wind_unmap_phys(&m);
			return (pte_t *)entry;
		}

		if (type != 0x3) {
			wind_unmap_phys(&m);
			return NULL;
		}

		pa = (phys_addr_t)(val & PHYS_MASK & (s32)PAGE_MASK);
		wind_unmap_phys(&m);
		if (!pa || !IS_ALIGNED(pa, PAGE_SIZE))
			return NULL;
	}

	return NULL;
}

/* 按运行时确定的层数走一次。 */
static pte_t *wind_mm_pte_locate(struct mm_struct *mm, unsigned long va,
				 struct wind_phys_map *out_m)
{
	return wind_mm_pte_locate_at(mm, va, wind_pgtable_levels(), out_m);
}

/* ===================================================================
 * 页表翻译: 按 3/4/5 级自适应。
 *
 * 这里手动实现各级偏移, 不用 p4d_offset/pud_offset/pmd_offset/
 * pte_offset_kernel 内核宏 —— 它们的 *_page_vaddr 展开会引用 __va,
 * 进而引用 arm64_use_ng_mappings / physvirt_offset 等非白名单符号,
 * GKI 下报 modpost undefined。页表项物理地址用 wind_map_phys 映射
 * (vmap + PAGE_KERNEL, 等价 __va, 不依赖 memstart_addr)。
 * =================================================================== */
phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
	struct wind_phys_map m = { .area = NULL, .addr = NULL };
	pte_t *pte;
	phys_addr_t pa;

	if (!mm)
		return 0;

	pte = wind_mm_pte_locate(mm, va, &m);
	if (!pte)
		return 0;

	if (pte_none(wind_ptep_get(pte)) || !pte_present(wind_ptep_get(pte))) {
		wind_unmap_phys(&m);
		return 0;
	}

	pa = (phys_addr_t)(pte_pfn(wind_ptep_get(pte)) << PAGE_SHIFT) +
	     (va & (PAGE_SIZE - 1));
	wind_unmap_phys(&m);
	return pa;
}

/* 单地址刷 leaf + walk cache。全刷 vmalle1is 破坏 KPTI 全局 TLB; 只刷
 * vae1is 漏 walk cache 残留旧只读项。 */
static inline void wind_flush_tlb_va(unsigned long va)
{
	asm volatile("dsb ishst\n"
		     "\ttlbi vaae1is, %0\n"
		     "\tdsb ish\n"
		     "\tisb\n"
		     : : "r" (va) : "memory");
}

/* ===================================================================
 * 内核镜像 rodata 写保护绕过。
 * arm64 set_memory_rw 只接受线性映射/vmalloc 地址, 对内核镜像 rodata
 * (null_fops 所在段) 返回 -EINVAL。这里直接改内核页表 (TTBR1) 的
 * AP[2] (bit7, 所有 level 同一位) 切 RO/RW。
 *
 * 安全策略: 先做一次 "只读校验遍历" —— 每级必须非零、非叶子级必须是表项
 * (0b11) 且下一级表 PA 页对齐; 叶子必须是 4KB 页 (0b11) 或 block (0b01)
 * 且 AF=1。全部通过才走第二遍真正改写。层数错了会在校验阶段就退出,
 * 不会按错误的 shift 去索引别的区域的表, 避免写坏无关 PTE 导致卡死/重启。
 * 注意: arm64 表描述符的 bit10 不是 AF (内核置 0), 只在末级校验 AF。
 * =================================================================== */

/* 叶子是 block (2MB/1GB) 映射时, 改 AP[2] 会把整块改成可写。
 * 默认拒绝: 只允许 4K 页叶子 (rodata=full 内核)。需要时显式开启。 */
static bool allow_block_patch;
module_param(allow_block_patch, bool, 0644);
MODULE_PARM_DESC(allow_block_patch, "allow patching AP[2] on block-mapped rodata");

/* 只读校验: 返回 0 表示整条路径合法; *leaf_block 说明叶子是 block */
static int wind_kpage_validate(unsigned long va, int levels, bool *leaf_block)
{
	struct wind_phys_map m;
	phys_addr_t pa;
	pteval_t val, type;
	int lv;

	if (levels < 3 || levels > 5)
		return -EINVAL;
	if (!(va >> 63)) {
		pr_err("wind: force_rw va=%lx 不在内核高半区\n", va);
		return -EINVAL;
	}

	pa = (phys_addr_t)(read_sysreg(ttbr1_el1) & PHYS_MASK & (s32)PAGE_MASK);
	if (!pa) {
		pr_err("wind: ttbr1 root pa = 0\n");
		return -EINVAL;
	}

	for (lv = levels - 1; lv >= 0; lv--) {
		pteval_t *entry;

		m = wind_map_phys(pa, PAGE_SIZE);
		if (!m.addr) {
			pr_err("wind: force_rw lv%d map %pa failed\n", lv, &pa);
			return -EINVAL;
		}

		entry = (pteval_t *)m.addr + wind_lidx(va, lv);
		val = READ_ONCE(*entry);
		wind_unmap_phys(&m);

		type = val & 0x3;

		if (!val) {
			pr_err("wind: force_rw lv%d none va=%lx idx=%lu (levels=%d)\n",
			       lv, va, wind_lidx(va, lv), levels);
			return -EINVAL;
		}

		if (lv == 0) {
			/* pte 级: 0b11 = 4KB 页, 叶子 AF 必须置位 */
			if (type != 0x3) {
				pr_err("wind: force_rw pte bad va=%lx val=%llx\n",
				       va, (unsigned long long)val);
				return -EINVAL;
			}
			if (!(val & WIND_PTE_AF)) {
				pr_err("wind: force_rw pte AF=0 va=%lx val=%llx\n",
				       va, (unsigned long long)val);
				return -EINVAL;
			}
			*leaf_block = false;
			return 0;
		}

		if (type == 0x1) {
			/* block 叶子 (2MB/1GB): 末级描述符, bit10 是 AF */
			if (!(val & WIND_PTE_AF)) {
				pr_err("wind: force_rw lv%d block AF=0 va=%lx val=%llx\n",
				       lv, va, (unsigned long long)val);
				return -EINVAL;
			}
			*leaf_block = true;
			return 0;
		}

		/* 表项 (0b11): arm64 表描述符 bit10 不是 AF (内核置 0), 不校验 */
		if (type != 0x3) {
			pr_err("wind: force_rw lv%d bad va=%lx val=%llx\n",
			       lv, va, (unsigned long long)val);
			return -EINVAL;
		}

		pa = (phys_addr_t)(val & PHYS_MASK & (s32)PAGE_MASK);
		if (!IS_ALIGNED(pa, PAGE_SIZE) || !pa) {
			pr_err("wind: force_rw lv%d table pa bad %llx\n",
			       lv, (unsigned long long)pa);
			return -EINVAL;
		}
	}

	return -EINVAL;
}

/* 校验通过后再走一遍, 只改叶子项的 AP[2] */
static int wind_kpage_patch(unsigned long va, bool writable, int levels)
{
	const pteval_t AP_RO = (pteval_t)1 << 7; /* AP[2]: 1=只读 0=可写 */
	struct wind_phys_map m;
	phys_addr_t pa;
	pteval_t val;
	int lv;

	pa = (phys_addr_t)(read_sysreg(ttbr1_el1) & PHYS_MASK & (s32)PAGE_MASK);

	for (lv = levels - 1; lv >= 0; lv--) {
		pteval_t *entry;
		pteval_t type;

		m = wind_map_phys(pa, PAGE_SIZE);
		if (!m.addr)
			return -EINVAL;

		entry = (pteval_t *)m.addr + wind_lidx(va, lv);
		val = READ_ONCE(*entry);
		type = val & 0x3;

		if (lv == 0 || type == 0x1) {
			pteval_t new = writable ? (val & ~AP_RO) : (val | AP_RO);

			WRITE_ONCE(*entry, new);
			wind_unmap_phys(&m);
			wind_flush_tlb_va(va);
			return 0;
		}

		if (type != 0x3) {
			wind_unmap_phys(&m);
			return -EINVAL;
		}

		pa = (phys_addr_t)(val & PHYS_MASK & (s32)PAGE_MASK);
		wind_unmap_phys(&m);
	}

	return -EINVAL;
}

int wind_force_kernel_page_rw(unsigned long va, bool writable)
{
	bool leaf_block = false;
	int ret, levels;

	va &= PAGE_MASK;

	levels = wind_pgtable_levels();

	ret = wind_kpage_validate(va, levels, &leaf_block);
	if (ret)
		return ret;

	if (leaf_block && !allow_block_patch) {
		pr_err("wind: rodata 叶子是 block 映射, 拒绝改写 (需 rodata=full 或 allow_block_patch=1)\n");
		return -EINVAL;
	}

	ret = wind_kpage_patch(va, writable, levels);
	if (ret == 0) {
		static bool wind_lv_ok;

		if (!wind_lv_ok) {
			wind_lv_ok = true;
			pr_info("wind: kernel pgtables = %d level(s), leaf %s\n",
				levels, leaf_block ? "block" : "page");
		}
	}
	return ret;
}

/* 只读探测: 整条路径校验通过后读叶子 AP[2], 不写任何东西。
 * *is_ro = true 表示叶子当前只读。 */
int wind_kernel_page_ro_state(unsigned long va, bool *is_ro)
{
	const pteval_t AP_RO = (pteval_t)1 << 7;
	struct wind_phys_map m;
	phys_addr_t pa;
	pteval_t val;
	bool leaf_block = false;
	int lv, levels, ret;

	if (!is_ro)
		return -EINVAL;

	va &= PAGE_MASK;
	levels = wind_pgtable_levels();

	ret = wind_kpage_validate(va, levels, &leaf_block);
	if (ret)
		return ret;

	pa = (phys_addr_t)(read_sysreg(ttbr1_el1) & PHYS_MASK & (s32)PAGE_MASK);
	for (lv = levels - 1; lv >= 0; lv--) {
		pteval_t *entry;
		pteval_t type;

		m = wind_map_phys(pa, PAGE_SIZE);
		if (!m.addr)
			return -EINVAL;
		entry = (pteval_t *)m.addr + wind_lidx(va, lv);
		val = READ_ONCE(*entry);
		wind_unmap_phys(&m);

		type = val & 0x3;
		if (lv == 0 || type == 0x1) {
			*is_ro = !!(val & AP_RO);
			return 0;
		}
		pa = (phys_addr_t)(val & PHYS_MASK & (s32)PAGE_MASK);
	}
	return -EINVAL;
}

/* ===================================================================
 * 影子页操作: 直接改目标进程页表中目标地址的 PTE 权限位。
 * 与 wind_force_kernel_page_rw 同构, 但目标是传入的 mm (用户态地址,
 * TTBR0), 且按 mask/val 位域改写 (UXN/PXN/RDONLY/VALID/AF 等)。
 * flush 用 vaae1is 刷所有 ASID 的目标地址 (断点进程非 current 时也生效)。
 * =================================================================== */
pteval_t wind_shadow_pte_get(struct mm_struct *mm, unsigned long va, int *err)
{
	struct wind_phys_map m = { .area = NULL, .addr = NULL };
	pte_t *pte;
	pteval_t val = 0;

	if (err)
		*err = -EINVAL;
	if (!mm)
		return 0;

	pte = wind_mm_pte_locate(mm, va, &m);
	if (!pte)
		return 0;

	val = pte_val(wind_ptep_get(pte));
	wind_unmap_phys(&m);
	if (err)
		*err = 0;
	return val;
}

int wind_shadow_pte_set(struct mm_struct *mm, unsigned long va, pteval_t mask, pteval_t val)
{
	struct wind_phys_map m = { .area = NULL, .addr = NULL };
	pte_t *pte;
	pteval_t old, new;

	if (!mm)
		return -EINVAL;

	pte = wind_mm_pte_locate(mm, va, &m);
	if (!pte)
		return -EINVAL;

	old = pte_val(wind_ptep_get(pte));
	new = (old & ~mask) | (val & mask);

	/* 直接写 PTE 值, 写后单地址刷 leaf + walk cache (同 wind_force_kernel_page_rw) */
	WRITE_ONCE(*(pteval_t *)pte, new);
	asm volatile("dsb ishst\n"
		     "\ttlbi vaae1is, %0\n"
		     "\ttlbi vale1is, %0\n"
		     "\tdsb ish\n"
		     "\tisb\n"
		     : : "r" (va & PAGE_MASK) : "memory");

	wind_unmap_phys(&m);
	return 0;
}

/* ===================================================================
 * 内核态跨进程读写 (非 __user buffer)。按页翻译物理地址, vmap 后
 * memcpy。BRK 软断点改目标进程指令字节用。
 * =================================================================== */
static bool wind_process_va_xfer(struct mm_struct *mm, uintptr_t va,
				 void *buf, size_t size, bool write)
{
	uintptr_t cur = va;
	char *b = buf;
	size_t left = size;

	while (left)
	{
		phys_addr_t pa;
		size_t off, chunk;
		struct wind_phys_map m;

		pa = translate_linear_address(mm, cur);
		if (!pa)
			return false;

		off = cur & (PAGE_SIZE - 1);
		chunk = PAGE_SIZE - off;
		if (chunk > left)
			chunk = left;

		m = wind_map_phys(pa, PAGE_SIZE);
		if (!m.addr)
			return false;

		if (write)
			memcpy((char *)m.addr + off, b, chunk);
		else
			memcpy(b, (char *)m.addr + off, chunk);

		wind_unmap_phys(&m);
		b += chunk;
		cur += chunk;
		left -= chunk;
	}
	return true;
}

bool wind_read_process_va(struct mm_struct *mm, uintptr_t va, void *buf, size_t size)
{
	if (!mm || !buf)
		return false;
	return wind_process_va_xfer(mm, va, buf, size, false);
}

bool wind_write_process_va(struct mm_struct *mm, uintptr_t va, const void *buf, size_t size)
{
	if (!mm || !buf)
		return false;
	return wind_process_va_xfer(mm, va, (void *)buf, size, true);
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
