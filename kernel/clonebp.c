// SPDX-License-Identifier: GPL-2.0
/*
 * 克隆页断点 (Clone-Page Breakpoint) 实现
 * 同 VA 双物理页切换: 原始页只读不可执行, 影子页用户不可读可执行。
 * 不占用 BRP/WRP 硬件断点寄存器, 不改原始页字节。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mmu_context.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/pid.h>
#include <linux/pgtable.h>
#include <linux/version.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/insn.h>
#include <linux/ktime.h>
#include <linux/prandom.h>

#include "clonebp.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))
#define cb_kmap(pg)      kmap_atomic(pg)
#define cb_kunmap(pg)    kunmap_atomic(pg)
#else
#define cb_kmap(pg)      kmap_local_page(pg)
#define cb_kunmap(pg)    kunmap_local(pg)
#endif

#define CB_MAX_PAGES_TOTAL  CLONEBP_MAX_PAGES

static DEFINE_MUTEX(g_cb_lock);
static struct clonebp_page g_cb_pages[CB_MAX_PAGES_TOTAL];
static int g_cb_page_cnt;
static atomic64_t g_cb_stat[CLONEBP_ST_MAX];

/* 全局当前挂载的 kprobe 上下文 */
static struct mm_struct *g_cb_mm;
static struct clonebp_point *g_cb_points;
static int g_cb_point_cnt;

static inline void cb_stat_inc(int idx)
{
	if (idx >= 0 && idx < CLONEBP_ST_MAX)
		atomic64_inc(&g_cb_stat[idx]);
}

/* ============================================================
 * 一、页克隆与 PTE 切换
 * ============================================================ */

/* 取目标地址的 pte 指针(需持 mmap_read_lock) */
static pte_t *cb_pte_get(struct mm_struct *mm, unsigned long addr, pmd_t **pmdp_out)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return NULL;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
	/* 5 级页表折叠后 p4d_offset 已处理, 这里只做空检查 */
#endif
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;
	if (pmdp_out)
		*pmdp_out = pmd;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
	/* __pte_offset_map 在 6.5+ 需要 pmd 且返回前要 pte_unmap */
	return pte_offset_map(pmd, addr);
#else
	return pte_offset_kernel(pmd, addr);
#endif
}

/* 分配影子页并复制原始内容, 在影子页内写入 BRK */
static struct page *cb_clone_page(struct page *orig, u32 offset, u32 imm, const u8 *patch, u32 patch_len)
{
	struct page *shadow;
	void *dst, *src;

	shadow = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!shadow)
		return NULL;

	dst = cb_kmap(shadow);
	src = cb_kmap(orig);
	memcpy(dst, src, PAGE_SIZE);

	/* 在 shadow 页的目标偏移处写入 BRK #imm (ARM64: 0xD4200000 | imm<<5) */
	if (patch && patch_len) {
		memcpy((u8 *)dst + offset, patch, min_t(u32, patch_len, CLONEBP_PATCH_MAX));
	} else {
		u32 insn = 0xD4200000u | ((imm & 0xFFFF) << 5);
		memcpy((u8 *)dst + offset, &insn, 4);
	}
	cb_kunmap(src);
	cb_kunmap(dst);
	cb_stat_inc(CLONEBP_ST_CLONE_OK);
	return shadow;
}

/*
 * 切换页状态
 *   to_shadow=1: 装上影子页(用户不可读, 可执行), 供 CPU 取指
 *   to_shadow=0: 装回原始页(用户可读, 不可执行), 供用户读取
 * 只改 PTE, 不动 VMA, /proc/self/maps 保持原样。
 */
#define CLONEBP_STATE_ORIG    0
#define CLONEBP_STATE_SHADOW  1
#define CLONEBP_STATE_BAILOUT 2
#define CLONEBP_BAILOUT_MAX   16

/* ---- 时序对抗: 异常路径耗时归一化, 消除「读执行交替」可测量方差 ----
 * 反作弊常对同页做 N 次读/执行交替并统计耗时与方差:
 *   原始页访问 ~ns 级, 每次切页 ~us 级, 均值与方差都显著偏离。
 * BALANCE 把单次异常路径补到固定 target_ns, 方差压平;
 * JITTER 叠加均匀抖动, 让分布伪装成 cache miss 而非恒定异常。
 * bailout_max 同时下调可让检测方连打几十次就读到静默页(放弃 hook 保命)。
 */
#define CB_TM_BALANCE   1
#define CB_TM_JITTER    2
#define CB_TM_PAD_MAX   200000U

struct cb_timing {
	u32 mode;
	u32 target_ns;
	u32 jitter_ns;
	u32 bailout_max;
	atomic64_t cost_ns;
	atomic64_t pad_ns;
	atomic64_t evt_cnt;
};

static struct cb_timing g_cb_tm = {
	.mode = 0,
	.target_ns = 0,
	.jitter_ns = 0,
	.bailout_max = CLONEBP_BAILOUT_MAX,
	.cost_ns = ATOMIC64_INIT(0),
	.pad_ns = ATOMIC64_INIT(0),
	.evt_cnt = ATOMIC64_INIT(0),
};

static inline u64 cb_tm_begin(void)
{
	if (!READ_ONCE(g_cb_tm.mode))
		return 0;
	return ktime_get_mono_fast_ns();
}

static inline void cb_tm_end(u64 t0)
{
	u32 mode = READ_ONCE(g_cb_tm.mode);
	u64 cost, pad = 0;

	if (!mode || !t0)
		return;
	cost = ktime_get_mono_fast_ns() - t0;
	atomic64_add(cost, &g_cb_tm.cost_ns);
	atomic64_inc(&g_cb_tm.evt_cnt);
	if (mode & CB_TM_BALANCE) {
		u64 target = READ_ONCE(g_cb_tm.target_ns);

		if (target > cost)
			pad = target - cost;
	}
	if (mode & CB_TM_JITTER) {
		u32 span = READ_ONCE(g_cb_tm.jitter_ns);

		if (span)
			pad += prandom_u32() % span;
	}
	if (pad) {
		if (pad > CB_TM_PAD_MAX)
			pad = CB_TM_PAD_MAX;
		ndelay((unsigned long)pad);
		atomic64_add(pad, &g_cb_tm.pad_ns);
	}
}


/* 同页自读会让「切原页->重试读->取指 UXN 切影子->重试读」形成无限异常环,
 * 参考态实测会拖垮内核。这里按页计数, 超阈值就放宽成可读可执行的原始页,
 * hook 失效但系统存活。 */
static atomic_t cb_bailout_cnt[CLONEBP_MAX_PAGES];
static void cb_flush_va_local(unsigned long va)
{
	dsb(ishst);
	__tlbi(vae1is, va >> PAGE_SHIFT);
	dsb(ish);
	isb();
}
static void flush_tlb_page_range_local(struct mm_struct *mm, unsigned long va);
static int cb_page_index(struct clonebp_page *cp);
static int cb_try_bailout(struct clonebp_page *cp);

static int cb_switch_page(struct mm_struct *mm, struct clonebp_page *cp, int to_shadow)
{
	pte_t *ptep;
	pte_t pte;
	unsigned long flags;

	if (!mm || !cp || !cp->inited)
		return -EINVAL;
	if (atomic_read(&cp->state) == to_shadow)
		return 0;

	mmap_read_lock(mm);
	ptep = cb_pte_get(mm, cp->user_va, NULL);
	if (!ptep) {
		mmap_read_unlock(mm);
		return -EFAULT;
	}
	local_irq_save(flags);
	if (to_shadow == CLONEBP_STATE_SHADOW) {
		/* 影子页: 用户不可读(无 PTE_USER), 可执行 */
		pte = mk_pte(cp->shadow_page, __pgprot(PTE_PXN));
	} else if (to_shadow == CLONEBP_STATE_BAILOUT) {
		/* 兜底: 原始页放宽为可读可执行(去 UXN), 打破异常环 */
		pte = mk_pte(cp->orig_page[0], __pgprot(PTE_USER | PTE_RDONLY | PTE_PXN));
	} else {
		/* 原始页: 用户可读, 不可执行 */
		pte = mk_pte(cp->orig_page[0],
			     __pgprot(PTE_USER | PTE_RDONLY | PTE_UXN | PTE_PXN));
	}
	WRITE_ONCE(*ptep, pte);
	local_irq_restore(flags);
	mmap_read_unlock(mm);

	atomic_set(&cp->state, to_shadow);
	if (to_shadow != CLONEBP_STATE_BAILOUT) {
		int _i = cb_page_index(cp);

		if (_i >= 0)
			atomic_set(&cb_bailout_cnt[_i], 0);
	}
	flush_tlb_page_range_local(mm, cp->user_va);
	cb_stat_inc(CLONEBP_ST_TLB_FLUSH);
	return 0;
}

/* ============================================================
 * 二、异常接管: 执行/读取双向切页 + BRK 命中改写
 * ============================================================ */

/* per-CPU 局部刷: 只冲刷本 CPU 上该 VA 的 TLB, 避免全局 shootdown 带来的
 * 「读执行交替耗时」膨胀(参考态 §读执行耗时 检测点)。 */
static void flush_tlb_page_range_local(struct mm_struct *mm, unsigned long va)
{
	int cpu;

	if (!mm)
		return;
	preempt_disable();
	cpu = smp_processor_id();
	if (mm_cpumask(mm) && cpumask_test_cpu(cpu, mm_cpumask(mm)))
		cb_flush_va_local(va);
	preempt_enable();
}

static int cb_page_index(struct clonebp_page *cp)
{
	if (!cp)
		return -1;
	if (cp < g_cb_pages || cp >= g_cb_pages + CLONEBP_MAX_PAGES)
		return -1;
	return (int)(cp - g_cb_pages);
}

/* 返回 1 表示已进入兜底态, 调用方直接放行异常返回 */
static int cb_try_bailout(struct clonebp_page *cp)
{
	int idx = cb_page_index(cp);

	if (idx < 0)
		return 0;
	if (atomic_inc_return(&cb_bailout_cnt[idx]) < (int)g_cb_tm.bailout_max)
		return 0;
	cb_switch_page(g_cb_mm, cp, CLONEBP_STATE_BAILOUT);
	cb_stat_inc(CLONEBP_ST_BRK_HIT);
	pr_warn("[clonebp] va=%lx 自读异常环, 已放宽为可读可执行\n",
		(unsigned long)cp->user_va);
	return 1;
}

/* 按 VA 查点 */
static struct clonebp_point *cb_find_point(uint64_t pc)
{
	int i;

	for (i = 0; i < g_cb_point_cnt; ++i) {
		struct clonebp_point *p = &g_cb_points[i];

		if (!p->active)
			continue;
		if (p->user_addr == pc)
			return p;
	}
	return NULL;
}

/* 按页基址查页 */
static struct clonebp_page *cb_find_page(uint64_t page_base)
{
	int i;

	for (i = 0; i < g_cb_page_cnt; ++i) {
		if (g_cb_pages[i].inited && g_cb_pages[i].va_page == page_base)
			return &g_cb_pages[i];
	}
	return NULL;
}

/* ESR 判定 */
static inline int cb_esr_is_iabt(unsigned int esr)
{
	return ((esr >> 26) & 0x3F) == 0x21 || ((esr >> 26) & 0x3F) == 0x20;
}
static inline int cb_esr_is_dabt(unsigned int esr)
{
	unsigned int ec = (esr >> 26) & 0x3F;

	return ec == 0x24 || ec == 0x25;
}

/* do_mem_abort pre_handler: 双向切页后放行异常返回, 由 CPU 重试 */
static int cb_mem_abort_do(struct pt_regs *regs)
{
	unsigned long addr = regs->regs[0];       /* x0 = far */
	unsigned int esr = (unsigned int)regs->regs[1];
	struct clonebp_page *cp;

	if (!g_cb_mm || g_cb_point_cnt == 0)
		return 0;

	cp = cb_find_page(addr & PAGE_MASK);
	if (!cp && !(cb_esr_is_iabt(esr) || cb_esr_is_dabt(esr)))
		return 0;

	if (cb_esr_is_iabt(esr)) {
		/* CPU 取指命中原始页(UXN) -> 换影子页 -> 重试取指 */
		if (cp) {
			if (atomic_read(&cp->state) == CLONEBP_STATE_BAILOUT)
				return 0;
			if (cb_try_bailout(cp))
				return 1;
			cb_switch_page(g_cb_mm, cp, CLONEBP_STATE_SHADOW);
			cb_stat_inc(CLONEBP_ST_IFAULT_IN);
			return 1;
		}
		return 0;
	}
	if (cb_esr_is_dabt(esr)) {
		/* 用户读取命中影子页(无 USER 位) -> 换回原始页 -> 重试访问 */
		if (cp && atomic_read(&cp->state) == CLONEBP_STATE_SHADOW) {
			if (cb_try_bailout(cp))
				return 1;
			cb_switch_page(g_cb_mm, cp, CLONEBP_STATE_ORIG);
			cb_stat_inc(CLONEBP_ST_DFAULT_IN);
			return 1;
		}
		return 0;
	}
	return 0;
}

static int cb_mem_abort_pre(struct kprobe *kpr, struct pt_regs *regs)
{
	int ret;
	u64 t0 = cb_tm_begin();

	(void)kpr;
	ret = cb_mem_abort_do(regs);
	cb_tm_end(t0);
	return ret;
}


/* brk_handler pre_handler: 影子页 BRK 命中 -> 记录/改写 -> 跳过 SIGTRAP */
static int cb_brk_do(struct pt_regs *regs)
{
	struct clonebp_point *pt;
	uint64_t pc = regs->pc;
	u32 insn = 0;
	u32 imm;
	int i;

	if (!g_cb_point_cnt)
		return 0;

	/* 读 faulting 指令, 校验确为 BRK */
	if (copy_from_kernel_nofault(&insn, (void __force *)pc, sizeof(insn)))
		return 0;
	if ((insn & 0xFFE0001Fu) != 0xD4200000u)
		return 0;

	imm = (insn >> 5) & 0xFFFF;
	for (i = 0; i < g_cb_point_cnt; ++i) {
		pt = &g_cb_points[i];
		if (!pt->active || pt->imm != imm)
			continue;

		pt->trigger_pc = pc;
		pt->hit_addr = pc & PAGE_MASK;
		pt->hits++;
		pt->last_jiffies = jiffies;
		cb_stat_inc(CLONEBP_ST_BRK_HIT);

		if (pt->mode == CLONEBP_MODE_REWRITE) {
			int k;

			for (k = 0; k < CLONEBP_REG_MAX; ++k) {
				int rn;

				if (!(pt->reg_mask & (1u << k)))
					continue;
				rn = (int)pt->reg_base + k;
				if (rn >= 0 && rn <= 30)
					regs->regs[rn] = pt->reg_val[k];
			}
		}
		if (pt->flags & CLONEBP_F_ONESHOT)
			pt->active = 0;

		regs->pc = pc + 4;   /* 跳过 BRK, 继续后续指令 */
		return 1;            /* 拦截原 handler, 不产生 SIGTRAP */
	}
	return 0;
}

static int cb_brk_pre(struct kprobe *kpr, struct pt_regs *regs)
{
	int ret;
	u64 t0 = cb_tm_begin();

	(void)kpr;
	ret = cb_brk_do(regs);
	cb_tm_end(t0);
	return ret;
}



/* mprotect 会按 VMA 重建 PTE。若此刻挂的是影子页, 新 PTE 会保留影子 PFN
 * 并把权限改成用户可读 -> 影子页里的补丁字节被直接读出来。
 * 在权限重建前把落在范围内的点先切回原始页。 */
static int cb_mprotect_pre(struct kprobe *kpr, struct pt_regs *regs)
{
	unsigned long start = regs->regs[0];
	unsigned long len   = regs->regs[1];
	unsigned long va;
	struct clonebp_page *cp;

	(void)kpr;
	if (!g_cb_mm || !g_cb_point_cnt || !start || !len)
		return 0;
	for (va = start & PAGE_MASK; va < start + len; va += PAGE_SIZE) {
		cp = cb_find_page(va);
		if (!cp || !cp->inited)
			continue;
		if (atomic_read(&cp->state) == CLONEBP_STATE_SHADOW)
			cb_switch_page(g_cb_mm, cp, CLONEBP_STATE_ORIG);
	}
	return 0;
}

static struct kprobe cb_mem_abort_kp = {
	.symbol_name = "do_mem_abort",
	.pre_handler = cb_mem_abort_pre,
};
static struct kprobe cb_brk_kp = {
	.symbol_name = "brk_handler",
	.pre_handler = cb_brk_pre,
};
static struct kprobe cb_mprotect_kp = {
	.symbol_name = "do_mprotect_pkey",
	.pre_handler = cb_mprotect_pre,
};
static int cb_kp_mem_reg;
static int cb_kp_brk_reg;
static int cb_kp_mprot_reg;

static int cb_install_hooks(void)
{
	int ret;

	if (!cb_kp_mem_reg) {
		ret = register_kprobe(&cb_mem_abort_kp);
		if (ret < 0) {
			pr_err("[clonebp] register do_mem_abort failed %d\n", ret);
			return ret;
		}
		cb_kp_mem_reg = 1;
	}
	if (!cb_kp_brk_reg) {
		ret = register_kprobe(&cb_brk_kp);
		if (ret < 0) {
			pr_err("[clonebp] register brk_handler failed %d\n", ret);
			unregister_kprobe(&cb_mem_abort_kp);
			cb_kp_mem_reg = 0;
			return ret;
		}
		cb_kp_brk_reg = 1;
	}
	if (!cb_kp_mprot_reg) {
		ret = register_kprobe(&cb_mprotect_kp);
		if (ret < 0)
			pr_warn("[clonebp] do_mprotect_pkey unavailable %d, "
				"mprotect 会暴露影子页\n", ret);
		else
			cb_kp_mprot_reg = 1;
	}
	return 0;
}

static void cb_remove_hooks(void)
{
	if (cb_kp_mprot_reg) {
		unregister_kprobe(&cb_mprotect_kp);
		cb_kp_mprot_reg = 0;
	}
	if (cb_kp_brk_reg) {
		unregister_kprobe(&cb_brk_kp);
		cb_kp_brk_reg = 0;
	}
	if (cb_kp_mem_reg) {
		unregister_kprobe(&cb_mem_abort_kp);
		cb_kp_mem_reg = 0;
	}
}

/* ============================================================
 * 三、接口层
 * ============================================================ */

static struct clonebp_point *cb_alloc_point(void)
{
	int i;

	for (i = 0; i < CLONEBP_MAX_POINTS; ++i) {
		if (!g_cb_points[i].active && g_cb_points[i].user_addr == 0) {
			memset(&g_cb_points[i], 0, sizeof(g_cb_points[i]));
			return &g_cb_points[i];
		}
	}
	return NULL;
}

static struct clonebp_page *cb_alloc_page(void)
{
	int i;

	for (i = 0; i < CB_MAX_PAGES_TOTAL; ++i) {
		if (!g_cb_pages[i].inited) {
			memset(&g_cb_pages[i], 0, sizeof(g_cb_pages[i]));
			spin_lock_init(&g_cb_pages[i].lock);
			atomic_set(&g_cb_pages[i].state, 0);
			return &g_cb_pages[i];
		}
	}
	return NULL;
}

/* 建立(或复用)页克隆 */
static struct clonebp_page *cb_page_get(struct mm_struct *mm, uint64_t addr,
					u32 offset, u32 imm, const u8 *patch, u32 patch_len)
{
	uint64_t page_base = addr & PAGE_MASK;
	struct clonebp_page *cp = cb_find_page(page_base);
	pmd_t *pmd = NULL;
	pte_t *ptep;
	pte_t pte;
	struct page *orig;

	if (cp) {
		cp->refcnt++;
		return cp;
	}

	cp = cb_alloc_page();
	if (!cp)
		return NULL;

	mmap_read_lock(mm);
	ptep = cb_pte_get(mm, page_base, &pmd);
	if (!ptep) {
		mmap_read_unlock(mm);
		return NULL;
	}
	pte = *ptep;
	if (!pte_present(pte)) {
		mmap_read_unlock(mm);
		return NULL;
	}
	orig = pte_page(pte);
	if (!orig) {
		mmap_read_unlock(mm);
		return NULL;
	}
	get_page(orig);
	mmap_read_unlock(mm);

	cp->va_page = page_base;
	cp->user_va = page_base;
	cp->orig_page[0] = orig;
	cp->orig_page[1] = NULL;
	cp->shadow_page = cb_clone_page(orig, offset, imm, patch, patch_len);
	if (!cp->shadow_page) {
		put_page(orig);
		cb_stat_inc(CLONEBP_ST_CLONE_FAIL);
		return NULL;
	}
	cp->refcnt = 1;
	cp->inited = 1;
	g_cb_page_cnt++;
	return cp;
}

int clonebp_set(struct task_struct *task, const void __user *uconf, uint32_t count)
{
	struct mm_struct *mm;
	int i, ok = 0;

	if (!task || !uconf || count == 0 || count > CLONEBP_MAX_POINTS)
		return -EINVAL;

	mm = get_task_mm(task);
	if (!mm)
		return -ESRCH;

	if (cb_install_hooks() < 0) {
		mmput(mm);
		return -ENODEV;
	}

	mutex_lock(&g_cb_lock);
	if (g_cb_mm && g_cb_mm != mm) {
		/* 只允许单目标 mm, 避免异常路径歧义 */
		mutex_unlock(&g_cb_lock);
		mmput(mm);
		return -EBUSY;
	}
	g_cb_mm = mm;

	for (i = 0; i < (int)count; ++i) {
		struct clonebp_point *pt;
		CLONEBP_POINT_CONFIG cfg;
		struct clonebp_page *cp;
		u32 offset;

		if (copy_from_user(&cfg, (const void __user *)uconf + i * sizeof(cfg), sizeof(cfg)))
			break;
		if (!cfg.addr)
			continue;

		pt = cb_alloc_point();
		if (!pt)
			break;
		offset = (u32)(cfg.addr & ~PAGE_MASK);
		pt->user_addr = cfg.addr;
		pt->page_base = cfg.addr & PAGE_MASK;
		pt->mode = cfg.mode;
		pt->reg_base = cfg.reg_base;
		pt->reg_mask = cfg.reg_mask;
		pt->flags = cfg.flags;
		pt->imm = cfg.imm ? cfg.imm : (u32)(CLONEBP_BRK_IMM_BASE + i);
		memcpy(pt->reg_val, cfg.reg_val, sizeof(pt->reg_val));

		cp = cb_page_get(mm, cfg.addr, offset, pt->imm, cfg.patch, cfg.patch_len);
		if (!cp) {
			memset(pt, 0, sizeof(*pt));
			break;
		}
		pt->page = cp;
		pt->active = 1;
		g_cb_point_cnt++;
		ok++;
	}
	/* 初始把目标页设为"原始页可读不可执行", 等待执行触发切换 */
	for (i = 0; i < g_cb_page_cnt; ++i) {
		if (g_cb_pages[i].inited)
			cb_switch_page(mm, &g_cb_pages[i], 0);
	}
	mutex_unlock(&g_cb_lock);
	return ok > 0 ? ok : -EFAULT;
}

static void cb_page_release(struct clonebp_page *cp)
{
	if (!cp || !cp->inited)
		return;
	if (--cp->refcnt > 0)
		return;
	if (cp->shadow_page) {
		__free_page(cp->shadow_page);
		cp->shadow_page = NULL;
	}
	if (cp->orig_page[0]) {
		put_page(cp->orig_page[0]);
		cp->orig_page[0] = NULL;
	}
	cp->inited = 0;
	cp->ptep = NULL;
	if (g_cb_page_cnt > 0)
		g_cb_page_cnt--;
}

int clonebp_remove_all(struct mm_struct *mm)
{
	int i;

	if (!mm)
		mm = g_cb_mm;

	mutex_lock(&g_cb_lock);
	for (i = 0; i < CLONEBP_MAX_POINTS; ++i) {
		if (g_cb_points[i].page) {
			cb_switch_page(mm, g_cb_points[i].page, 0);
			cb_page_release(g_cb_points[i].page);
		}
		memset(&g_cb_points[i], 0, sizeof(g_cb_points[i]));
	}
	g_cb_point_cnt = 0;
	for (i = 0; i < CB_MAX_PAGES_TOTAL; ++i) {
		if (g_cb_pages[i].inited)
			cb_page_release(&g_cb_pages[i]);
	}
	g_cb_page_cnt = 0;
	g_cb_mm = NULL;
	mutex_unlock(&g_cb_lock);
	cb_remove_hooks();
	return 0;
}

int clonebp_clear_records(struct mm_struct *mm, int32_t point_index)
{
	int i;

	(void)mm;
	mutex_lock(&g_cb_lock);
	for (i = 0; i < CLONEBP_MAX_POINTS; ++i) {
		if (point_index >= 0 && i != point_index)
			continue;
		g_cb_points[i].hits = 0;
		g_cb_points[i].trigger_pc = 0;
		g_cb_points[i].hit_addr = 0;
	}
	mutex_unlock(&g_cb_lock);
	return 0;
}

/* 自愈: 页被重新映射/回收后重新克隆并重装影子页 */
int clonebp_refresh(struct mm_struct *mm)
{
	int i, n = 0;

	if (!mm)
		mm = g_cb_mm;
	if (!mm)
		return -ESRCH;

	mutex_lock(&g_cb_lock);
	for (i = 0; i < CLONEBP_MAX_POINTS; ++i) {
		struct clonebp_point *pt = &g_cb_points[i];
		struct clonebp_page *cp;
		pmd_t *pmd = NULL;
		pte_t *ptep;

		if (!pt->active)
			continue;
		mmap_read_lock(mm);
		ptep = cb_pte_get(mm, pt->page_base, &pmd);
		if (!ptep || !pte_present(*ptep)) {
			mmap_read_unlock(mm);
			continue;
		}
		mmap_read_unlock(mm);
		cp = pt->page;
		if (!cp)
			continue;
		if (cp->shadow_page)
			__free_page(cp->shadow_page);
		cp->shadow_page = cb_clone_page(cp->orig_page[0],
					       (u32)(pt->user_addr & ~PAGE_MASK),
					       pt->imm, NULL, 0);
		if (cp->shadow_page) {
			atomic_set(&cp->state, -1);
			cb_switch_page(mm, cp, 0);
			n++;
		}
	}
	mutex_unlock(&g_cb_lock);
	return n;
}

/* fork: 复制页表前暂停影子映射, 复制后只恢复父进程 */
void clonebp_fork_pause(struct mm_struct *mm)
{
	int i;

	mutex_lock(&g_cb_lock);
	for (i = 0; i < CB_MAX_PAGES_TOTAL; ++i)
		if (g_cb_pages[i].inited)
			cb_switch_page(mm, &g_cb_pages[i], 0);
	mutex_unlock(&g_cb_lock);
}

void clonebp_fork_resume(struct mm_struct *mm)
{
	int i;

	mutex_lock(&g_cb_lock);
	for (i = 0; i < CB_MAX_PAGES_TOTAL; ++i)
		if (g_cb_pages[i].inited && g_cb_pages[i].shadow_page) {
			atomic_set(&g_cb_pages[i].state, -1);
			cb_switch_page(mm, &g_cb_pages[i], 1);
		}
	mutex_unlock(&g_cb_lock);
}

void clonebp_release_mm(struct mm_struct *mm)
{
	if (g_cb_mm == mm)
		clonebp_remove_all(mm);
}

int clonebp_module_init(void)
{
	g_cb_points = kzalloc(sizeof(struct clonebp_point) * CLONEBP_MAX_POINTS,
			      GFP_KERNEL);
	if (!g_cb_points)
		return -ENOMEM;
	g_cb_point_cnt = 0;
	g_cb_page_cnt = 0;
	g_cb_mm = NULL;
	return 0;
}

void clonebp_module_exit(void)
{
	cb_remove_hooks();
	if (g_cb_mm)
		clonebp_remove_all(g_cb_mm);
	kfree(g_cb_points);
	g_cb_points = NULL;
}

/* 按 pid 安装: req = { uint32 pid; uint32 point_count; CLONEBP_POINT_CONFIG points[]; } */
int clonebp_set_pid(uint32_t pid_arg, const void __user *ureq)
{
	struct task_struct *task;
	struct mm_struct *mm;
	uint32_t count = 0;
	uint32_t pid = pid_arg;
	const void __user *points;
	int ret;

	if (!ureq)
		return -EINVAL;
	if (copy_from_user(&pid, (const void __user *)ureq, sizeof(pid)))
		return -EFAULT;
	if (copy_from_user(&count, (const void __user *)((uintptr_t)ureq + 4), sizeof(count)))
		return -EFAULT;
	points = (const void __user *)((uintptr_t)ureq + 8);
	if (count == 0 || count > CLONEBP_MAX_POINTS)
		return -EINVAL;

	rcu_read_lock();
	task = find_task_by_vpid((pid_t)pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;
	mmput(mm);

	/* clonebp_set 内部再次 get_task_mm, 这里只做存在性校验 */
	rcu_read_lock();
	task = find_task_by_vpid((pid_t)pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;
	ret = clonebp_set(task, points, count);
	put_task_struct(task);
	return ret;
}

int clonebp_get_config(void *uout)
{
	CLONEBP_CONFIG cfg;

	if (!uout)
		return -EINVAL;
	memset(&cfg, 0, sizeof(cfg));
	mutex_lock(&g_cb_lock);
	cfg.active_count = (uint32_t)g_cb_point_cnt;
	cfg.shadow_pages = (uint32_t)g_cb_page_cnt;
	mutex_unlock(&g_cb_lock);
	cfg.faults_in_i = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_IFAULT_IN]);
	cfg.faults_in_d = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_DFAULT_IN]);
	cfg.brk_hits = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_BRK_HIT]);
	cfg.tlb_flushes = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_TLB_FLUSH]);
	cfg.clone_ok = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_CLONE_OK]);
	cfg.clone_fail = (uint64_t)atomic64_read(&g_cb_stat[CLONEBP_ST_CLONE_FAIL]);
	memcpy(uout, &cfg, sizeof(cfg));
	return 0;
}

int clonebp_set_timing(CLONEBP_TIMING_CONFIG *cfg)
{
	if (!cfg)
		return -EINVAL;
	if (cfg->mode == 0xFFFFFFFFU)   /* 只读哨兵: 不改配置 */
		return 0;
	if (cfg->mode & ~(CB_TM_BALANCE | CB_TM_JITTER))
		return -EINVAL;
	if (cfg->target_ns > CB_TM_PAD_MAX || cfg->jitter_ns > CB_TM_PAD_MAX)
		return -EINVAL;
	if (cfg->bailout_max == 0 || cfg->bailout_max > 4096)
		return -EINVAL;
	WRITE_ONCE(g_cb_tm.target_ns, cfg->target_ns);
	WRITE_ONCE(g_cb_tm.jitter_ns, cfg->jitter_ns);
	WRITE_ONCE(g_cb_tm.bailout_max, cfg->bailout_max);
	WRITE_ONCE(g_cb_tm.mode, cfg->mode);
	return 0;
}

int clonebp_get_timing(CLONEBP_TIMING_CONFIG *cfg)
{
	if (!cfg)
		return -EINVAL;
	memset(cfg, 0, sizeof(*cfg));
	cfg->mode = READ_ONCE(g_cb_tm.mode);
	cfg->target_ns = READ_ONCE(g_cb_tm.target_ns);
	cfg->jitter_ns = READ_ONCE(g_cb_tm.jitter_ns);
	cfg->bailout_max = READ_ONCE(g_cb_tm.bailout_max);
	cfg->cost_ns = (uint64_t)atomic64_read(&g_cb_tm.cost_ns);
	cfg->pad_ns = (uint64_t)atomic64_read(&g_cb_tm.pad_ns);
	cfg->evt_cnt = (uint64_t)atomic64_read(&g_cb_tm.evt_cnt);
	return 0;
}

int clonebp_get_records(uint32_t point_index, uint32_t start,
			CLONEBP_RECORD *recs, uint32_t cap, uint32_t *count_out)
{
	CLONEBP_RECORD r;
	int i;

	if (!recs || cap == 0 || !count_out)
		return -EINVAL;
	*count_out = 0;
	mutex_lock(&g_cb_lock);
	for (i = 0; i < CLONEBP_MAX_POINTS; ++i) {
		struct clonebp_point *pt = &g_cb_points[i];

		if (!pt->user_addr)
			continue;
		if (point_index != (uint32_t)-1 && (uint32_t)i != point_index)
			continue;
		if (*count_out < start)
			continue;
		if (*count_out - start >= cap)
			break;
		memset(&r, 0, sizeof(r));
		r.trigger_pc = pt->trigger_pc;
		r.hit_addr = pt->user_addr;
		r.hits = (uint32_t)pt->hits;
		recs[*count_out - start] = r;
		(*count_out)++;
	}
	mutex_unlock(&g_cb_lock);
	return 0;
}
