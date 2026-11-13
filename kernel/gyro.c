/*
 * gyro.c — 陀螺仪控制 (内核态 uprobe hook libsensor.so)
 *
 * 对齐 hookmod 参考驱动的控制方式:
 *   1. filp_open("/system/lib64/libsensor.so") + kernel_read 读 ELF
 *   2. 解析 .dynsym/.dynstr, 定位 android::BitTube::sendObjects 符号偏移
 *   3. 加目标进程 libsensor.so 加载基址 = 运行时地址
 *   4. register_user_hw_breakpoint 设置执行断点 (uprobe)
 *   5. 断点命中时拦截 sensors_event_t 数组, 篡改陀螺仪角速度
 *
 * 断点回调运行在目标进程上下文 (perf overflow handler, 中断上下文),
 * current = 目标进程, 不能睡眠。用户态地址 events 直接用 copy_*_user_nofault
 * 读写 (nofault 缺页安全, 中断上下文可用), 不依赖 phys_to_virt。
 *
 * 未验证项 (版本相关, 需按目标 Android 版本核对):
 *   1. sensors_event_t 布局: type@0x08, data@0x18 (gyro x/y/z = data[0..2])。
 *      该布局为 Android HAL sensors.h 标准布局, 但 BitTube 实际传输的
 *      结构体可能随 AOSP 版本微调, 偏移需在真机 dump 后确认。
 *   2. sendObjects 参数 (arm64): x1 = events, x2 = count。符号为 static
 *      成员函数, x0 = tube(sp<BitTube> const&), 不参与数据定位。
 *   3. SENSOR_TYPE_GYROSCOPE = 4 (HAL 常量, 长期稳定)。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/elf.h>
#include <linux/string_helpers.h>
#include <linux/percpu.h>
#include <linux/jiffies.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/mman.h>
#include <asm/ptrace.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include "windcfg.h"	/* windcfg_gyro_on() */


#include <asm/page.h>
#include <asm/memory.h>
#include "sym.h"
#include "gyro.h"
#include "memory.h"
#include "compat.h"

/* ---- sensors_event_t 布局 (Android HAL, arm64) ----
 * HAL 版结构 (BitTube 传的就是它):
 *   0x00 int32 version | 0x04 int32 sensor | 0x08 int32 type | 0x0C int32 reserved0
 *   0x10 int64 timestamp | 0x18 float data[16] | 0x58 uint32 flags
 * 注意 NDK 的 ASensorEvent 把 version/sensor 并成 union, type 落在 0x04;
 * 两种都可能遇到, 所以下面用「先 0x08, 找不到 gyro 再试 0x04」自动挑。
 * x/y/z 在 data[0..2], z 在 0x20。 */
#define SENSOR_EVENT_SIZE       0x68   /* sizeof(sensors_event_t) */
#define SENSOR_EVENT_TYPE_OFF   0x08   /* int32_t type (HAL 布局) */
#define SENSOR_EVENT_TYPE_OFF_NDK 0x04 /* int32_t type (NDK ASensorEvent 布局) */
#define SENSOR_EVENT_DATA_OFF   0x18   /* float data[16]; gyro x/y/z = data[0..2] */
#define SENSOR_TYPE_GYROSCOPE   4
/* HAL: SENSOR_TYPE_GYROSCOPE_UNCALIBRATED = 16。UE/Unity 的陀螺仪插件
 * 多半读未校准事件 (data[0..2] 是 x/y/z, data[3..5] 是 bias), 只认 4
 * 会漏掉一半目标。两个 type 的 x/y/z 都在 data[0..2], 改写方式一致。 */
#define SENSOR_TYPE_GYRO_UNCAL  16

/* 单次 sendObjects 最多处理的事件数 (防御越界) */
#define GYRO_MAX_EVENTS         1024

/* BitTube 的实现在 libui.so 里, 不在 libsensor.so。老实现只认 libsensor.so,
 * 在原生 AOSP 上直接 -ENOENT 装不上 —— 这里按候选表逐个试 */
struct gyro_lib_cand
{
	const char *name;
	const char *path;
};

static const struct gyro_lib_cand gyro_libs[] = {
	{ "libui.so",            "/system/lib64/libui.so" },
	{ "libsensor.so",        "/system/lib64/libsensor.so" },
	{ "libsensorservice.so", "/system/lib64/libsensorservice.so" },
	{ "libgui.so",           "/system/lib64/libgui.so" },
};
#define GYRO_LIB_COUNT ((int)ARRAY_SIZE(gyro_libs))

/* ---- 全局状态 ---- */
static struct perf_event * __percpu *gyro_bp; /* per-CPU 执行断点 (wide) */
static bool gyro_bp_wide = true;              /* false = 退回单任务断点 */
static struct perf_event *gyro_single_bp;     /* 退回路径的单任务断点 */
static atomic64_t gyro_hits = ATOMIC64_INIT(0); /* 命中计数, 诊断用 */
static pid_t gyro_pid;                  /* 目标进程: 只用来定位 hook 地址 */
/* 命中过滤开关。true = 全局生效, 不检查 current->tgid;
 * false = 只对 gyro_pid 那个进程生效。
 * 设为全局是因为 sendObjects 的篡改结果落在共享内存里,
 * 任何读这块 buffer 的进程都该看到同一个值,
 * 只认 system_server 会让游戏侧读到未改写的旧数据。
 */
static bool gyro_global = true;
static unsigned long gyro_addr;         /* sendObjects 运行时地址 */
static bool gyro_active;
static bool gyro_enabled;
static u32 gyro_rx, gyro_ry; /* 目标角速度 x/y, float 位模式 (rad/s); z 恒不动 */

extern void gyro_sock_stats(long long *msgs, long long *rewrites);
extern void gyro_sock_dump(void);
extern bool gyro_sock_active(void);
extern bool g_wind_unloading;

/* 用户态一帧都没推的时候, 以前 gyro-diag 一行都不会出, 看起来像"驱动没反应"。
 * 改成独立心跳: 装上之后每 2 秒一条, 不管用户态推没推。 */
static struct delayed_work gyro_diag_work;
static unsigned int gyro_diag_dumps;

static void gyro_diag_tick(struct work_struct *w)
{
	long long sm = 0, sr = 0;

	(void)w;
	if (READ_ONCE(g_wind_unloading))
		return;
	gyro_sock_stats(&sm, &sr);
	pr_info("[wind] gyro-tick: sock_on=%d msgs=%lld rewrites=%lld "
		"x=%08x y=%08x active=%d\n",
		(int)gyro_sock_active(), sm, sr,
		gyro_rx, gyro_ry, (int)gyro_active);
	/* 改写数还是 0 的时候, 前几次心跳顺带把"哪条路径在命中 / 类型分布"
	 * 打出来 —— 这是区分"没接上"和"类型不对"的唯一依据。 */
	if (!sr && gyro_diag_dumps < 4) {
		gyro_diag_dumps++;
		gyro_sock_dump();
	}
	if (gyro_active)
		schedule_delayed_work(&gyro_diag_work, 2 * HZ);
}
static u32 gyro_rz_unused;   /* 兼容旧 set 接口, 不再写进事件 */

/* ---- WRP 数据断点: 学到事件缓冲地址后, 只盯 x/y 那 8 字节 ----
 * 这一步把执行断点从 system_server 里彻底摘掉: 稳态只剩一个写观察点,
 * 不改任何代码, 不占 BRP, 也不在 system_server 的指令流里留下 perf 事件 */
static struct perf_event * __percpu *gyro_wrp[2];
static struct perf_event *gyro_wrp_single[2];
static bool gyro_wrp_wide[2];
static uint64_t gyro_wrp_addr[2];       /* 各观察点盯住的用户 VA */
static int gyro_wrp_count;
static bool gyro_wrp_ready;
static uint64_t gyro_wrp_pending;       /* 待转换的地址 (中断上下文只记, 不注册) */
static atomic64_t gyro_wrp_hits = ATOMIC64_INIT(0);
static atomic64_t gyro_writes = ATOMIC64_INIT(0);
static unsigned long gyro_wrp_last_seen;
static unsigned long gyro_diag_next;
static int gyro_wrp_busy_cpu[NR_CPUS];
#define gyro_wrp_busy_now() (gyro_wrp_busy_cpu[raw_smp_processor_id()])
static const char *gyro_lib_used;
static int gyro_mode;   /* 0=学到后自动转 WRP 1=只用执行断点 2=已转 WRP */
module_param(gyro_mode, int, 0644);
MODULE_PARM_DESC(gyro_mode, "0=自动(默认): 执行断点学一次地址后转 WRP 数据断点; 1=只用执行断点");

/* ===================================================================
 * 非睡眠内存读写: 断点回调运行在目标进程上下文 (current = 目标进程),
 * 用户态地址 events 直接 copy_*_user_nofault, 无需页表遍历, 也不依赖
 * phys_to_virt (GKI 拿不到 memstart_addr)。nofault 版本在缺页时返回
 * 错误而非崩溃 (中断上下文安全)。
 * =================================================================== */
static bool gyro_read_mem(struct mm_struct *mm, uintptr_t va, void *buf, size_t size)
{
	(void)mm;
	return wind_copy_from_user_nofault(buf, (void __user *)va, size) == 0;
}

static bool gyro_write_mem(struct mm_struct *mm, uintptr_t va, const void *buf, size_t size)
{
	(void)mm;
	return wind_copy_to_user_nofault((void __user *)va, buf, size) == 0;
}

/* ===================================================================
 * ELF 符号解析: 读 libsensor.so 的 .dynsym/.dynstr, 返回 sendObjects
 * 的 st_value (PIE 下即文件内偏移)。
 * =================================================================== */
static int gyro_resolve_symbol(const char *path, unsigned long *out_offset)
{
	struct file *f;
	Elf64_Ehdr ehdr;
	Elf64_Shdr *shdrs = NULL;
	char *shstrtab = NULL, *dynstr = NULL;
	Elf64_Sym *dynsym = NULL;
	unsigned long dynsym_off = 0, dynsym_size = 0;
	unsigned long dynstr_off = 0, dynstr_size = 0;
	loff_t pos;
	int ret = -ENOENT;
	int i;

	f = wind_filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	pos = 0;
	if (wind_kernel_read(f, &ehdr, sizeof(ehdr), &pos) != sizeof(ehdr))
	{
		ret = -EIO;
		goto out;
	}
	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
	    ehdr.e_ident[EI_CLASS] != ELFCLASS64)
	{
		ret = -ENOEXEC;
		goto out;
	}

	shdrs = kmalloc_array(ehdr.e_shnum, ehdr.e_shentsize, GFP_KERNEL);
	if (!shdrs)
	{
		ret = -ENOMEM;
		goto out;
	}
	pos = ehdr.e_shoff;
	if (wind_kernel_read(f, shdrs, ehdr.e_shnum * ehdr.e_shentsize, &pos) !=
	    (ssize_t)(ehdr.e_shnum * ehdr.e_shentsize))
	{
		ret = -EIO;
		goto out;
	}

	/* 读 .shstrtab, 用于按名定位 .dynstr */
	{
		Elf64_Shdr *sh = &shdrs[ehdr.e_shstrndx];
		shstrtab = kmalloc(sh->sh_size, GFP_KERNEL);
		if (!shstrtab)
		{
			ret = -ENOMEM;
			goto out;
		}
		pos = sh->sh_offset;
		if (wind_kernel_read(f, shstrtab, sh->sh_size, &pos) != (ssize_t)sh->sh_size)
		{
			ret = -EIO;
			goto out;
		}
	}

	for (i = 0; i < ehdr.e_shnum; i++)
	{
		const char *name = shstrtab + shdrs[i].sh_name;

		if (shdrs[i].sh_type == SHT_DYNSYM)
		{
			dynsym_off = shdrs[i].sh_offset;
			dynsym_size = shdrs[i].sh_size;
		}
		else if (shdrs[i].sh_type == SHT_STRTAB && !strcmp(name, ".dynstr"))
		{
			dynstr_off = shdrs[i].sh_offset;
			dynstr_size = shdrs[i].sh_size;
		}
	}
	if (!dynsym_off || !dynstr_off)
	{
		ret = -ENOENT;
		goto out;
	}

	dynstr = kmalloc(dynstr_size, GFP_KERNEL);
	dynsym = kmalloc(dynsym_size, GFP_KERNEL);
	if (!dynstr || !dynsym)
	{
		ret = -ENOMEM;
		goto out;
	}
	pos = dynstr_off;
	if (wind_kernel_read(f, dynstr, dynstr_size, &pos) != (ssize_t)dynstr_size)
	{
		ret = -EIO;
		goto out;
	}
	pos = dynsym_off;
	if (wind_kernel_read(f, dynsym, dynsym_size, &pos) != (ssize_t)dynsym_size)
	{
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < (int)(dynsym_size / sizeof(Elf64_Sym)); i++)
	{
		const char *name = dynstr + dynsym[i].st_name;

		if (!strcmp(name, GYRO_SENDOBJECTS_SYM))
		{
			*out_offset = (unsigned long)dynsym[i].st_value;
			ret = 0;
			break;
		}
	}

out:
	kfree(dynsym);
	kfree(dynstr);
	kfree(shstrtab);
	kfree(shdrs);
	filp_close(f, NULL);
	return ret;
}

/* ===================================================================
 * 目标进程 libsensor.so 加载基址: 遍历 VMA, 按 basename 匹配。
 * =================================================================== */
static unsigned long gyro_find_module_base(struct task_struct *task, const char *name)
{
	struct mm_struct *mm;
	unsigned long base = 0;
#if KH_HAVE_VMA_ITER
	struct vma_iterator vmi;
#endif
	struct vm_area_struct *vma;

	mm = kh_get_task_mm(task);
	if (!mm)
		return 0;

	kh_mmap_read_lock(mm);
#if KH_HAVE_VMA_ITER
	vma_iter_init(&vmi, mm, 0);
	for_each_vma(vmi, vma)
#else
	for (vma = mm->mmap; vma; vma = vma->vm_next)
#endif
	{
		if (vma->vm_file)
		{
			char buf[256];
			char *p = wind_file_path(vma->vm_file, buf, sizeof(buf) - 1);

			if (!IS_ERR(p) && !strcmp(kbasename(p), name))
			{
				/* 同 process.c: 必须减掉 vm_pgoff 才是 load bias,
				 * 否则 sendObjects 的运行时地址整体错位, 断点装了不响 */
				base = vma->vm_start -
				       (unsigned long)(vma->vm_pgoff << PAGE_SHIFT);
				break;
			}
		}
	}
	kh_mmap_read_unlock(mm);
	wind_mmput(mm);
	return base;
}

/* ===================================================================
 * 断点回调: 拦截 sendObjects 的 sensor 数据, 篡改陀螺仪角速度。
 * arm64: x1 = events (void const*), x2 = count (unsigned long)。
 * =================================================================== */
static struct task_struct *gyro_get_task(void);

/* ---- WRP 观察点命中: 只写 x/y 两个分量, z 恒不动 ---- */
static void gyro_wrp_handler(struct perf_event *bp, struct perf_sample_data *data,
			     struct pt_regs *regs)
{
	struct mm_struct *mm;
	u32 v[2];

	(void)bp;
	(void)data;

	if (!READ_ONCE(gyro_active) || !READ_ONCE(gyro_enabled))
		return;
	if (!gyro_global && gyro_pid && current->tgid != gyro_pid)
		return;
	/* 我们自己写回也会命中这个观察点: 内核上下文 + per-CPU busy 双闸 */
	if (!user_mode(regs))
		return;
	if (gyro_wrp_busy_now())
		return;

	mm = current->mm;
	if (!mm)
		return;

	atomic64_inc(&gyro_wrp_hits);
	gyro_wrp_last_seen = jiffies;

	v[0] = gyro_rx;
	v[1] = gyro_ry;
	/* 观察窗口就是 x/y 那 8 字节: 写回长度 8, z 所在 0x20 一个字节都不碰 */
	gyro_wrp_busy_now()++;
	if (gyro_write_mem(mm, (uintptr_t)gyro_wrp_addr[0], v, sizeof(v)))
		atomic64_inc(&gyro_writes);
	gyro_wrp_busy_now()--;

	if (atomic64_read(&gyro_wrp_hits) <= 3)
		printk(KERN_INFO "gyro-wrp: hit #%lld addr=%llx x=%08x y=%08x\n",
		       (long long)atomic64_read(&gyro_wrp_hits),
		       (unsigned long long)gyro_wrp_addr[0], gyro_rx, gyro_ry);
}

/* 把一个观察点挂上去 (重试逻辑与执行断点一致) */
static int gyro_wrp_arm_one(int idx, struct task_struct *task)
{
	struct perf_event_attr attr;
	void *ev;
	bool wide = true;

	hw_breakpoint_init(&attr);
	attr.bp_addr = gyro_wrp_addr[idx];
	attr.bp_len = HW_BREAKPOINT_LEN_8;   /* x + y 一起盯, z 在下一格 */
	attr.bp_type = HW_BREAKPOINT_W;

	/* 按任务优先: 用户地址用 wide 会在一堆无关进程的同 VA 写入上误命中,
	 * 每次误命中都进一次异常, 帧率就是这么掉下去的。 */
	ev = wind_register_user_hw_breakpoint(&attr, gyro_wrp_handler, NULL, task);
	if (IS_ERR(ev) && PTR_ERR(ev) == -EOPNOTSUPP)
	{
		wide = true;
		ev = wind_register_wide_hw_breakpoint(&attr, gyro_wrp_handler, NULL);
	}
	else if (!IS_ERR(ev))
	{
		wide = false;
	}
	if (IS_ERR(ev))
	{
		printk(KERN_ERR "gyro-wrp: 观察点注册失败 %ld\n", PTR_ERR(ev));
		return (int)PTR_ERR(ev);
	}

	gyro_wrp[idx] = wide ? ev : NULL;
	gyro_wrp_single[idx] = wide ? NULL : ev;
	gyro_wrp_wide[idx] = wide;
	printk(KERN_INFO "gyro-wrp: 观察点 %d armed addr=%llx len=8 wide=%d\n",
	       idx, (unsigned long long)gyro_wrp_addr[idx], (int)wide);
	return 0;
}

static void gyro_wrp_disarm_all(void)
{
	int i;

	for (i = 0; i < 2; i++)
	{
		if (gyro_wrp[i])
		{
			wind_unregister_wide_hw_breakpoint(gyro_wrp[i]);
			gyro_wrp[i] = NULL;
		}
		if (gyro_wrp_single[i])
		{
			wind_unregister_hw_breakpoint(gyro_wrp_single[i]);
			gyro_wrp_single[i] = NULL;
		}
	}
	gyro_wrp_count = 0;
	gyro_wrp_ready = false;
}

/* 执行断点命中 → 记下事件缓冲地址 → 交给进程上下文去完成转换
 * (不能在 perf 回调里注销自己的事件) */
static void gyro_learn(uintptr_t event_va)
{
	gyro_wrp_pending = (uint64_t)(event_va + SENSOR_EVENT_DATA_OFF);
}

/* 进程上下文: 把「学到地址」翻译成「摘掉执行断点, 换成写观察点」 */
static void gyro_try_switch(void)
{
	struct task_struct *task;
	int ret;

	if (!gyro_wrp_pending || gyro_wrp_ready || gyro_mode == 1)
		return;

	task = gyro_get_task();
	if (!task)
		return;

	gyro_wrp_addr[0] = gyro_wrp_pending;
	ret = gyro_wrp_arm_one(0, task);
	if (ret)
	{
		/* 占不到观察点寄存器就继续用执行断点, 至少还有一条路 */
		printk(KERN_WARNING "gyro: 转 WRP 失败(%d), 继续用执行断点\n", ret);
		gyro_wrp_pending = 0;
		return;
	}

	gyro_wrp_count = 1;
	gyro_wrp_ready = true;
	gyro_wrp_last_seen = jiffies;

	if (gyro_bp)
	{
		wind_unregister_wide_hw_breakpoint(gyro_bp);
		gyro_bp = NULL;
	}
	if (gyro_single_bp)
	{
		wind_unregister_hw_breakpoint(gyro_single_bp);
		gyro_single_bp = NULL;
	}

	gyro_mode = 2;
	printk(KERN_INFO "gyro: 已转为 WRP 数据断点 addr=%llx, "
			 "system_server 里不再有执行断点 (代码/BRP 全干净)\n",
	       (unsigned long long)gyro_wrp_addr[0]);
}

static void gyro_breakpoint_handler(struct perf_event *bp,
				    struct perf_sample_data *data,
				    struct pt_regs *regs)
{
	struct mm_struct *mm;
	unsigned long events, count, stride;
	unsigned int i;
	int type_off, pass;

	(void)bp;
	(void)data;

	if (!READ_ONCE(gyro_active) || !READ_ONCE(gyro_enabled))
		return;

	/* wide 断点挂在 per-CPU 上, 同一个 VA 在别的进程里也可能被映射到同址。
	 * gyro_global 为真时不筛 tgid: 篡改结果写进共享内存,
	 * 所有读取方都应拿到同一个值, 这是全局语义。 */
	if (!gyro_global && gyro_pid && current->tgid != gyro_pid)
		return;
	atomic64_inc(&gyro_hits);

	mm = current->mm; /* 断点在目标进程上下文触发 */
	if (!mm)
		return;

#if defined(CONFIG_ARM64)
	events = regs->regs[1];
	count = regs->regs[2];
	stride = regs->regs[3];   /* sendObjects 的第 4 个参数就是 objSize */
#elif defined(CONFIG_X86_64)
	events = regs->rsi;
	count = regs->rdx;
	stride = regs->rcx;
#else
	return;
#endif

	if (!events || !count || count > GYRO_MAX_EVENTS)
		return;
	/* 第 4 参为空就按 HAL 的 sizeof(sensors_event_t) 兜底 */
	if (stride < 0x20 || stride > 0x400)
		stride = SENSOR_EVENT_SIZE;

	/* 头 5 次命中记一行: 没有这行分不清「断点没响」和「响了但改不到」 */
	if (atomic64_read(&gyro_hits) <= 5)
		printk(KERN_INFO "gyro: hit #%lld pc=%lx events=%lx count=%lu stride=%lu tgid=%d\n",
		       (long long)atomic64_read(&gyro_hits),
		       (unsigned long)instruction_pointer(regs),
		       events, count, stride, (int)current->tgid);

	/* 先按 HAL 布局 (type@0x08) 找, 一件 gyro 都没有就按 NDK 布局
	 * (type@0x04) 再来一遍 —— 两种拼法在不同 Android 版本上都出现过 */
	for (pass = 0; pass < 2; pass++)
	{
		type_off = pass ? SENSOR_EVENT_TYPE_OFF_NDK : SENSOR_EVENT_TYPE_OFF;

		for (i = 0; i < count; i++)
		{
			uintptr_t ev = events + (uintptr_t)i * stride;
			int32_t type;
			u32 v[2];

			if (!gyro_read_mem(mm, ev + type_off, &type, sizeof(type)))
				continue;
			if (type != SENSOR_TYPE_GYROSCOPE &&
			    type != SENSOR_TYPE_GYRO_UNCAL)
				continue;

			/* 只写 data[0]/data[1] = x/y, z (data[2], 偏移 0x20) 原样不动 */
			v[0] = gyro_rx;
			v[1] = gyro_ry;
			if (gyro_write_mem(mm, ev + SENSOR_EVENT_DATA_OFF, v, sizeof(v)))
				atomic64_inc(&gyro_writes);

			/* 第一次找到就把这块缓冲记下来, 之后转成写观察点 */
			if (!gyro_wrp_pending && !gyro_wrp_ready)
				gyro_learn(ev);
			break;
		}
		if (i < count)
			break;   /* 这一种布局里找到了 gyro 事件 */
	}
}

/* 取目标进程 task (不持引用, 与 install 时一致) */
static struct task_struct *gyro_get_task(void)
{
	struct pid *pid_struct;
	struct task_struct *task;

	if (!gyro_pid)
		return NULL;

	pid_struct = find_get_pid(gyro_pid);
	if (!pid_struct)
		return NULL;

	task = pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	return task;
}

/* 解析符号 + 取基址 + 注册执行断点 (install / resume 共用) */
static int __maybe_unused gyro_bind(struct task_struct *task)
{
	unsigned long offset = 0, base = 0;
	struct perf_event_attr attr;
	int ret, i;

	/* 逐个候选库试: 先看文件里有没有这个符号, 再看进程里有没有映射它 */
	base = 0;
	offset = 0;
	for (i = 0; i < GYRO_LIB_COUNT; i++)
	{
		unsigned long off = 0, b;

		if (gyro_resolve_symbol(gyro_libs[i].path, &off) != 0)
			continue;
		b = gyro_find_module_base(task, gyro_libs[i].name);
		if (!b)
			continue;
		base = b;
		offset = off;
		gyro_lib_used = gyro_libs[i].name;
		break;
	}
	if (!base)
	{
		printk(KERN_ERR "gyro: %s 在候选库里都没找到 (libui/libsensor/"
		       "libsensorservice/libgui): hook 必须装在真正执行 "
		       "BitTube::sendObjects 的进程 (system_server)\n",
		       GYRO_SENDOBJECTS_SYM);
		return -ENOENT;
	}
	printk(KERN_INFO "gyro: 命中 %s off=0x%lx base=0x%lx\n",
	       gyro_lib_used, offset, base);

	gyro_addr = base + offset;
	printk(KERN_INFO "gyro: base=0x%lx off=0x%lx addr=0x%lx\n",
	       base, offset, gyro_addr);

	hw_breakpoint_init(&attr);
	attr.bp_addr = gyro_addr;
	/* arm64 的 arch_build_bp_info 只接受 LEN_4 的执行断点, 写 LEN_8
	 * 会直接 -EINVAL (旧版 gyro 就死在这一步, dmesg 里是 register failed -22)。 */
#if defined(CONFIG_ARM64)
	attr.bp_len = HW_BREAKPOINT_LEN_4;
#else
	attr.bp_len = HW_BREAKPOINT_LEN_1;
#endif
	attr.bp_type = HW_BREAKPOINT_X;

	/* 必须用 per-CPU 全域断点: sendObjects 由 system_server 的 binder 线程
	 * 执行, 只绑一个 task 的断点永远不命中。命中侧按 tgid 过滤。 */
	gyro_bp_wide = true;
	gyro_bp = wind_register_wide_hw_breakpoint(&attr, gyro_breakpoint_handler, NULL);
	if (IS_ERR(gyro_bp) && PTR_ERR(gyro_bp) == -EOPNOTSUPP)
	{
		/* 解析不到 register_wide_hw_breakpoint: 退回绑单个 task 的断点。
		 * 这条路径只覆盖被绑线程, sensor 投递在 binder 线程上, 大概率零命中。 */
		gyro_bp_wide = false;
		gyro_bp = NULL;
		gyro_single_bp = wind_register_user_hw_breakpoint(
			&attr, gyro_breakpoint_handler, NULL, task);
		if (IS_ERR(gyro_single_bp))
		{
			ret = PTR_ERR(gyro_single_bp);
			gyro_single_bp = NULL;
			printk(KERN_ERR "gyro: single-task bp register failed %d\n", ret);
			return ret;
		}
		printk(KERN_WARNING "gyro: wide bp 不可用, 退回单任务断点 (可能零命中)\n");
		return 0;
	}
	if (IS_ERR(gyro_bp))
	{
		ret = PTR_ERR(gyro_bp);
		gyro_bp = NULL;
		printk(KERN_ERR "gyro: wide breakpoint register failed %d\n", ret);
		return ret;
	}
	printk(KERN_INFO "gyro: wide bp armed addr=0x%lx tgid=%d\n", gyro_addr, gyro_pid);
	return 0;
}

/* ===================================================================
 * 安装 hook: 解析符号 -> 找基址 -> 设执行断点。
 * =================================================================== */
/* 陀螺仪通道总开关。默认开。
 * 主路走 Paradise 那套: 挂 system_server 的 __arm64_sys_recvmsg,
 * 直接改 ASensorEvent 里陀螺仪记录的 data[0]/data[1], 下游客户端拿到的
 * 就是改过的值。这条路不占 arm64 的 4 个硬件断点寄存器, 也不依赖
 * sendObjects 命中, 所以和 WRP 追踪点不冲突。
 * 只有主路挂不上、回落到执行断点时才占寄存器 —— 那时 wrpbp_set 会回
 * -EBUSY, 日志里看得见。insmod wind.ko wind_gyro=0 可整体关掉。
 */
bool g_wind_gyro_enable = true;
module_param_named(wind_gyro, g_wind_gyro_enable, bool, 0444);
MODULE_PARM_DESC(wind_gyro, "陀螺仪改写通道, 1=开(默认, Paradise 同路 socket 改写)");

int gyro_install(pid_t pid)
{
	int ret;

	/* 两个开关都要看:
	 *   wind_gyro  —— gyro.c 自己的总闸
	 *   gyro       —— 刷入器传的那个 (windcfg), 用户关掉时它才是 0
	 * 以前只认 wind_gyro, 刷入器选"关闭"只改了 cfg_gyro, 用户以为关了其实
	 * 没关; 反过来也容易让人以为驱动没开陀螺仪。两边一致才算开。 */
	if (!g_wind_gyro_enable || !windcfg_gyro_on())
	{
		printk(KERN_INFO "gyro: 通道已关闭 (wind_gyro=%d gyro=%d)\n",
		       (int)g_wind_gyro_enable, (int)windcfg_gyro_on());
		return -EOPNOTSUPP;
	}
	if (gyro_active)
		return -EBUSY;
	if (pid <= 0)
	{
		printk(KERN_ERR "gyro: 需要显式传入 system_server 的 pid (不是游戏 pid)\n");
		return -EINVAL;
	}

	gyro_pid = pid;
	gyro_global = true;   /* 默认全局, 不绑单一进程 */

	/* 主路: SEQPACKET socket 改写。
	 * 不占调试寄存器, 不依赖 sendObjects 命中, 也不受帧率影响。
	 * 挂上了就不必再装执行断点。 */
	ret = gyro_sock_install(pid);
	if (ret == 0) {
		gyro_active = true;
		printk(KERN_INFO "gyro: socket 路线已启用 tgid=%d\n", pid);
		gyro_diag_dumps = 0;
		INIT_DELAYED_WORK(&gyro_diag_work, gyro_diag_tick);
		schedule_delayed_work(&gyro_diag_work, 2 * HZ);
		return 0;
	}

	/* 只保留 Paradise 这条 (改 system_server 收到的 ASensorEvent)。
	 * 不再回落到执行断点: 那条路要占 arm64 仅有的 4 个硬件断点寄存器,
	 * 和子弹追踪的观察点直接抢, 一回落就是"自瞄没反应 + 追踪装不上"同时出现,
	 * 而且断点异常风暴本身就会把机器拖死。
	 * 这里原样返回 socket 的错误码, 用户态看 dmesg 就知道挂的是哪个符号。 */
	printk(KERN_ERR "gyro: socket 路线失败 %d, 不再回落到执行断点\n", ret);
	gyro_pid = 0;
	return ret;
}

void gyro_remove(void)
{
	/* 先停心跳, 否则卸载过程中还会有一条 tick 打到已经拆掉的 kprobe 上 */
	cancel_delayed_work_sync(&gyro_diag_work);
	gyro_sock_remove();
	if (gyro_bp)
	{
		wind_unregister_wide_hw_breakpoint(gyro_bp);
		gyro_bp = NULL;
	}
	if (gyro_single_bp)
	{
		wind_unregister_hw_breakpoint(gyro_single_bp);
		gyro_single_bp = NULL;
	}
	gyro_wrp_disarm_all();
	gyro_wrp_pending = 0;
	gyro_bp_wide = true;
	gyro_active = false;
	WRITE_ONCE(gyro_enabled, false);
	gyro_pid = 0;
	gyro_addr = 0;
	gyro_mode = 0;
	printk(KERN_INFO "gyro: unregistered\n");
}

/* 陀螺仪占着调试寄存器时, 子弹追踪那套观察点再装就超配额:
 * arm64 一共 4 个, 陀螺仪 2 个 + 每个追踪点最多 3 个。超了 register 会失败,
 * 更糟的是两个子系统互相踩, 一路异常把机器拖死。
 * 这里给 wrpbp 一个只读查询, 让它自己拒装。 */
bool gyro_holds_breakpoints(void)
{
	return gyro_active && (gyro_bp || gyro_single_bp || gyro_wrp_ready ||
			       gyro_wrp[0] || gyro_wrp_single[0]);
}
EXPORT_SYMBOL(gyro_holds_breakpoints);

void gyro_set_velocity(u32 rx, u32 ry, u32 rz)
{
	/* socket 路线只认 x/y; z 从不使用, 保持原样 */
	if (gyro_sock_active())
		gyro_sock_set(rx, ry);

	struct task_struct *task;

	gyro_rx = rx;
	gyro_ry = ry;
	gyro_rz_unused = rz;   /* z 不动: 存下来只为了兼容旧接口 */

	/* 每 2 秒打一行, 用来判断卡在哪一步:
	 *   pending!=0 且 ready=0  -> 学到了地址但观察点注册不上
	 *   exec_hits 一直是 0     -> sendObjects 执行断点根本没命中, 地址/符号错
	 *   wrp_hits 是 0 而 exec_hits 不走 -> 已经转到观察点但一次没命中
	 * 这行放在下面 task 判空之前, 目标进程没了也要能看见最后状态。 */
	if (time_after(jiffies, gyro_diag_next)) {
		gyro_diag_next = jiffies + 2 * HZ;
		long long sm = 0, sr = 0;

		gyro_sock_stats(&sm, &sr);
		printk(KERN_INFO "gyro-diag: sock_active=%d sock_msgs=%lld sock_rewrites=%lld "
				 "mode=%d pending=%llx ready=%d bp=%d "
				 "single=%d wrp=%d exec_hits=%lld wrp_hits=%lld writes=%lld\n",
			       (int)gyro_sock_active(), sm, sr,
				       gyro_mode, (unsigned long long)gyro_wrp_pending,
		       gyro_wrp_ready ? 1 : 0, gyro_bp ? 1 : 0,
		       gyro_single_bp ? 1 : 0, gyro_wrp[0] ? 1 : 0,
		       (long long)atomic64_read(&gyro_hits),
		       (long long)atomic64_read(&gyro_wrp_hits),
		       (long long)atomic64_read(&gyro_writes));
	}

	/* 这里有进程上下文: 完成「执行断点 → WRP 观察点」的切换,
	 * 以及观察点漂了之后的重新学习 */
	task = gyro_get_task();
	if (!task)
		return;

	if (gyro_wrp_pending && !gyro_wrp_ready)
		gyro_try_switch();

	/* 事件缓冲被回收/重映射时观察点会静默失效: 3 秒零命中就退回执行断点
	 * 重新学一次地址 */
	if (gyro_wrp_ready && gyro_mode == 2 &&
	    time_after(jiffies, gyro_wrp_last_seen + 3 * HZ))
	{
		printk(KERN_INFO "gyro: 观察点 3 秒无命中, 重新学习事件缓冲地址\n");
		gyro_wrp_disarm_all();
		gyro_wrp_pending = 0;
		gyro_mode = 0;
		if (!gyro_bp && !gyro_single_bp)
			gyro_bind(task);
	}
}

void gyro_set_enabled(bool enabled)
{
	WRITE_ONCE(gyro_enabled, enabled);
}

/* 停止: 只关篡改, hook 保留 (热开关, 无重新解析开销) */
int gyro_stop(void)
{
	if (!gyro_active)
		return -ENODEV;

	WRITE_ONCE(gyro_enabled, false);
	printk(KERN_INFO "gyro: stopped (hook kept)\n");
	return 0;
}

/* 恢复: 校验目标仍存活; 断点被回收则重新绑定后再开篡改 */
int gyro_resume(void)
{
	struct task_struct *task;
	int ret;

	if (!gyro_active)
		return -ENODEV;

	if (READ_ONCE(gyro_enabled))
		return 0;

	task = gyro_get_task();
	if (!task)
		return -ESRCH;

	if (!gyro_bp && !gyro_single_bp && !gyro_wrp_ready)
	{
		ret = gyro_bind(task);
		if (ret)
			return ret;
	}
	if (gyro_wrp_ready)
		gyro_wrp_last_seen = jiffies;

	WRITE_ONCE(gyro_enabled, true);
	printk(KERN_INFO "gyro: resumed (pid=%d addr=0x%lx)\n", gyro_pid, gyro_addr);
	return 0;
}
