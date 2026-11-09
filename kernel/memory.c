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
#include <linux/jiffies.h>
#include <linux/atomic.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/cacheflush.h>
#include <asm/sysreg.h>

struct wind_phys_map {
	struct vm_struct *area;
	void *addr;
};

static struct wind_phys_map wind_map_phys(phys_addr_t pa, size_t size)
{
	struct wind_phys_map m = { .area = NULL, .addr = NULL };
	unsigned long addr, end;
	int ret;

	/*
	 * ioremap_page_range -> vmap_range_noflush 对 phys_addr / size 做
	 * BUG_ON 页对齐断言 (mm/vmalloc.c:1101), 非对齐参数直接 panic。
	 * translate_linear_address 返回的 pa 带页内偏移, 此处强制对齐兜底。
	 */
	pa &= PAGE_MASK;
	size = PAGE_ALIGN(size);
	if (!size)
		return m;

	m.area = wind_get_vm_area_caller(size, VM_IOREMAP, VMALLOC_START, VMALLOC_END,
					__builtin_return_address(0));
	if (!m.area)
		return m;
	m.addr = m.area->addr;
	addr = (unsigned long)m.addr;
	end = addr + size;
	ret = wind_ioremap_page_range(addr, end, pa, PAGE_KERNEL);
	if (ret) {
		wind_vunmap(m.addr, m.area);
		m.area = NULL;
		m.addr = NULL;
	}
	return m;
}

static void wind_unmap_phys(struct wind_phys_map *m)
{
	if (!m || !m->area)
		return;

	wind_vunmap(m->addr, m->area);
	m->area = NULL;
	m->addr = NULL;
}

#if defined(CONFIG_PGTABLE_LEVELS) && (CONFIG_PGTABLE_LEVELS > 0)
#define WIND_PGTABLE_LEVELS CONFIG_PGTABLE_LEVELS
#else
#define WIND_PGTABLE_LEVELS 4
#endif

#define WIND_PT_BITS	(PAGE_SHIFT - 3)
#define WIND_PT_IDXMASK	((1UL << WIND_PT_BITS) - 1)
#define WIND_PTE_AF	((pteval_t)1 << 10)

static inline unsigned long wind_lidx(unsigned long va, int lv)
{
	return (va >> (PAGE_SHIFT + (unsigned int)lv * WIND_PT_BITS)) & WIND_PT_IDXMASK;
}

static int wind_pgtable_levels_cached;

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

	pgd = mm->pgd + wind_lidx(va, levels - 1);
	val = pgd_val(*pgd);
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

static pte_t *wind_mm_pte_locate(struct mm_struct *mm, unsigned long va,
				 struct wind_phys_map *out_m)
{
	return wind_mm_pte_locate_at(mm, va, wind_pgtable_levels(), out_m);
}

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

static inline void wind_flush_tlb_va(unsigned long va)
{
	asm volatile("dsb ishst\n"
		     "\ttlbi vaae1is, %0\n"
		     "\tdsb ish\n"
		     "\tisb\n"
		     : : "r" (va) : "memory");
}

static bool allow_block_patch;
module_param(allow_block_patch, bool, 0644);
MODULE_PARM_DESC(allow_block_patch, "force allow AP[2] patch on executable block leaf");

static int wind_kpage_validate(unsigned long va, int levels, bool *leaf_block,
			       pteval_t *leaf_val)
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
			*leaf_val = val;
			return 0;
		}

		if (type == 0x1) {
			if (!(val & WIND_PTE_AF)) {
				pr_err("wind: force_rw lv%d block AF=0 va=%lx val=%llx\n",
				       lv, va, (unsigned long long)val);
				return -EINVAL;
			}
			*leaf_block = true;
			*leaf_val = val;
			return 0;
		}

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

static int wind_kpage_patch(unsigned long va, bool writable, int levels)
{
	const pteval_t AP_RO = (pteval_t)1 << 7;
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
	const pteval_t PXN = (pteval_t)1 << 53;
	const pteval_t UXN = (pteval_t)1 << 54;
	bool leaf_block = false;
	pteval_t leaf_val = 0;
	int ret, levels;

	va &= PAGE_MASK;

	levels = wind_pgtable_levels();

	ret = wind_kpage_validate(va, levels, &leaf_block, &leaf_val);
	if (ret)
		return ret;

	if (leaf_block) {
		bool nx = (leaf_val & PXN) && (leaf_val & UXN);

		if (!nx && !allow_block_patch) {
			pr_err("wind: block 叶子是可执行段, 拒绝改写 (val=%llx)\n",
			       (unsigned long long)leaf_val);
			return -EINVAL;
		}
		if (!allow_block_patch)
			pr_info("wind: block 叶子 NX 数据段, 整块 2MB 临时开放 (val=%llx)\n",
				(unsigned long long)leaf_val);
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

int wind_kernel_page_ro_state(unsigned long va, bool *is_ro)
{
	const pteval_t AP_RO = (pteval_t)1 << 7;
	struct wind_phys_map m;
	phys_addr_t pa;
	pteval_t val, leaf_val = 0;
	bool leaf_block = false;
	int lv, levels, ret;

	if (!is_ro)
		return -EINVAL;

	va &= PAGE_MASK;
	levels = wind_pgtable_levels();

	ret = wind_kpage_validate(va, levels, &leaf_block, &leaf_val);
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

		pa &= PAGE_MASK;
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

#define WIND_MAP_SLOTS 256
#define WIND_MAP_TTL   (5 * HZ)

struct wind_map_slot {
	phys_addr_t pa;
	struct vm_struct *area;
	void *addr;
	unsigned long stamp;
};

/* map_cache=0 → 完全退回旧的「每次读都 vmap/vunmap」路径。
 * 刷入就重启时用 insmod wind.ko map_cache=0 一条命令就能判定是不是这段引起的,
 * 不需要重新编译。 */
static bool map_cache = true;
module_param(map_cache, bool, 0644);
MODULE_PARM_DESC(map_cache, "1=复用单页映射(默认, 快) 0=每次读重新映射(旧行为, 慢)");

static struct wind_map_slot g_map_slots[WIND_MAP_SLOTS];
static DEFINE_MUTEX(g_map_mutex);
static atomic64_t g_stat_reads = ATOMIC64_INIT(0);
static atomic64_t g_stat_hits = ATOMIC64_INIT(0);
static atomic64_t g_stat_misses = ATOMIC64_INIT(0);
static atomic64_t g_stat_evicts = ATOMIC64_INIT(0);

static void wind_slot_free(struct wind_map_slot *s)
{
	if (s->area)
		wind_vunmap(s->addr, s->area);
	memset(s, 0, sizeof(*s));
}

void wind_map_cache_purge(void)
{
	int i;

	mutex_lock(&g_map_mutex);
	for (i = 0; i < WIND_MAP_SLOTS; i++)
		wind_slot_free(&g_map_slots[i]);
	mutex_unlock(&g_map_mutex);
}

void wind_map_cache_get_stats(struct wind_map_stats *out)
{
	int i, used = 0;

	if (!out)
		return;
	mutex_lock(&g_map_mutex);
	for (i = 0; i < WIND_MAP_SLOTS; i++)
		if (g_map_slots[i].area)
			used++;
	mutex_unlock(&g_map_mutex);

	out->reads = (uint64_t)atomic64_read(&g_stat_reads);
	out->hits = (uint64_t)atomic64_read(&g_stat_hits);
	out->misses = (uint64_t)atomic64_read(&g_stat_misses);
	out->evicts = (uint64_t)atomic64_read(&g_stat_evicts);
	out->cached = (uint64_t)used;
}

static void *wind_slot_lookup_locked(phys_addr_t pa_page)
{
	unsigned long now = jiffies;
	unsigned long oldest = ~0UL;
	int i, free_slot = -1, lru = 0;
	struct wind_map_slot *s;
	struct wind_phys_map m;

	for (i = 0; i < WIND_MAP_SLOTS; i++) {
		s = &g_map_slots[i];
		if (!s->area) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (time_after(now, s->stamp + WIND_MAP_TTL)) {
			wind_slot_free(s);
			atomic64_inc(&g_stat_evicts);
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (s->pa == pa_page) {
			s->stamp = now;
			atomic64_inc(&g_stat_hits);
			return s->addr;
		}
		if (s->stamp < oldest) {
			oldest = s->stamp;
			lru = i;
		}
	}

	if (free_slot < 0) {
		free_slot = lru;
		wind_slot_free(&g_map_slots[lru]);
		atomic64_inc(&g_stat_evicts);
	}

	m = wind_map_phys(pa_page, PAGE_SIZE);
	atomic64_inc(&g_stat_misses);
	if (!m.addr)
		return NULL;

	s = &g_map_slots[free_slot];
	s->pa = pa_page;
	s->area = m.area;
	s->addr = m.addr;
	s->stamp = now;
	return s->addr;
}

static bool wind_read_cached_page(phys_addr_t pa_page, size_t off,
				  void __user *buffer, size_t size, bool safe)
{
	void *kva;
	bool ok = false;

	/* mutex 不能在原子上下文/关中断里拿。断点、trace、netfilter 这些回调
	 * 里如果走到物理读, 必须退回一次性映射, 否则就是
	 * "BUG: sleeping function called from invalid context" → panic。 */
	if (!map_cache || in_atomic() || irqs_disabled())
		return false;

	mutex_lock(&g_map_mutex);
	kva = wind_slot_lookup_locked(pa_page);
	if (kva) {
		if (safe)
			kh_dcache_clean_inval_poc((char *)kva + off, size);
		ok = copy_to_user(buffer, (char *)kva + off, size) == 0;
	}
	mutex_unlock(&g_map_mutex);
	return ok;
}

static bool read_physical_direct(phys_addr_t pa, void __user *buffer, size_t size)
{
	phys_addr_t base = pa & PAGE_MASK;
	size_t off = (size_t)(pa - base);
	struct wind_phys_map m;

	atomic64_inc(&g_stat_reads);
	/* 页内读走缓存; 缓存被关/在原子上下文/映射失败时回落一次性映射 */
	if (off + size <= PAGE_SIZE && wind_read_cached_page(base, off, buffer, size, false))
		return true;

	m = wind_map_phys(base, PAGE_ALIGN(off + size));
	if (!m.addr)
		return false;
	if (copy_to_user(buffer, (char *)m.addr + off, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	wind_unmap_phys(&m);
	return true;
}

static bool write_physical_direct(phys_addr_t pa, const void __user *buffer, size_t size)
{
	phys_addr_t base = pa & PAGE_MASK;
	size_t off = (size_t)(pa - base);
	struct wind_phys_map m = wind_map_phys(base, PAGE_ALIGN(off + size));

	if (!m.addr)
		return false;
	if (copy_from_user((char *)m.addr + off, buffer, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	kh_flush_dcache((char *)m.addr + off, size);
	wind_unmap_phys(&m);
	return true;
}

static bool read_physical_nocache(phys_addr_t pa, void __user *buffer, size_t size)
{
	phys_addr_t base = pa & PAGE_MASK;
	size_t off = (size_t)(pa - base);
	struct wind_phys_map m;

	atomic64_inc(&g_stat_reads);
	if (off + size <= PAGE_SIZE && wind_read_cached_page(base, off, buffer, size, true))
		return true;

	m = wind_map_phys(base, PAGE_ALIGN(off + size));
	if (!m.addr)
		return false;
	kh_dcache_clean_inval_poc((char *)m.addr + off, size); /* 读前 invalidate, 防 stale cache */
	if (copy_to_user(buffer, (char *)m.addr + off, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	wind_unmap_phys(&m);
	return true;
}

static bool write_physical_nocache(phys_addr_t pa, const void __user *buffer, size_t size)
{
	phys_addr_t base = pa & PAGE_MASK;
	size_t off = (size_t)(pa - base);
	struct wind_phys_map m = wind_map_phys(base, PAGE_ALIGN(off + size));

	if (!m.addr)
		return false;
	if (copy_from_user((char *)m.addr + off, buffer, size)) {
		wind_unmap_phys(&m);
		return false;
	}
	kh_flush_dcache((char *)m.addr + off, size);
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

static struct mm_struct *session_get_mm(struct driver_session *sess)
{
	if (!sess || !sess->task)
		return NULL;
	return kh_get_task_mm(sess->task);
}

static bool __read_mm(struct mm_struct *mm, uintptr_t addr, void __user *buffer, size_t size, int mode)
{
	while (size > 0)
	{
		phys_addr_t pa;
		size_t chunk;

		pa = translate_linear_address(mm, addr);
		if (!pa)
		{
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

	if (sess && sess->task && sess->task->mm == current->mm) {
		void *tmp;

		if (!size || size > PAGE_SIZE * 4)
			return false;
		tmp = kmalloc(size, GFP_KERNEL);
		if (!tmp)
			return false;
		if (copy_from_user(tmp, (void __user *)addr, size) != 0) {
			kfree(tmp);
			return false;
		}
		if (copy_to_user(buffer, tmp, size) != 0) {
			kfree(tmp);
			return false;
		}
		kfree(tmp);
		return true;
	}

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

	if (sess && sess->task && sess->task->mm == current->mm) {
		void *tmp;

		if (!size || size > PAGE_SIZE * 4)
			return false;
		tmp = kmalloc(size, GFP_KERNEL);
		if (!tmp)
			return false;
		if (copy_from_user(tmp, buffer, size) != 0) {
			kfree(tmp);
			return false;
		}
		if (copy_to_user((void __user *)addr, tmp, size) != 0) {
			kfree(tmp);
			return false;
		}
		kfree(tmp);
		return true;
	}

	mm = session_get_mm(sess);
	if (!mm)
		return false;

	result = __write_mm(mm, addr, buffer, size, sess->read_mode);
	wind_mmput(mm);
	return result;
}

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
		return -EOPNOTSUPP;
	}

	for (va = target_va & PAGE_MASK, off = vma->vm_start;
	     va < target_va + length;
	     va += PAGE_SIZE, off += PAGE_SIZE) {

		pa = translate_linear_address(mm, va);
		if (!pa)
			break;
		pa &= PAGE_MASK;
		if (wind_remap_pfn_range(vma, off, __phys_to_pfn(pa), PAGE_SIZE, vma->vm_page_prot)) {
			ret = -EAGAIN;
			break;
		}
	}

	wind_mmput(mm);
	return (off > vma->vm_start) ? 0 : ret;
}

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

	wind_map_cache_purge();

	if (sess->task) {
		put_task_struct(sess->task);
		sess->task = NULL;
	}
	sess->bound = false;

	pid_struct = find_get_pid(pid);
	if (!pid_struct)
		return;
	task = pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task)
		return;

	if (!task->mm)
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
	wind_map_cache_purge();
	kfree(sess);
}
