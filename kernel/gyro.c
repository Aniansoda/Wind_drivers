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
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/mman.h>
#include <asm/ptrace.h>
#include <asm/page.h>
#include <asm/memory.h>
#include "sym.h"
#include "gyro.h"
#include "memory.h"
#include "compat.h"

/* ---- sensors_event_t 布局 (Android HAL, arm64, 未验证) ---- */
#define SENSOR_EVENT_SIZE       0x68   /* sizeof(sensors_event_t) */
#define SENSOR_EVENT_TYPE_OFF   0x08   /* int32_t type */
#define SENSOR_EVENT_DATA_OFF   0x18   /* float data[16]; gyro x/y/z = data[0..2] */
#define SENSOR_TYPE_GYROSCOPE   4
/* HAL: SENSOR_TYPE_GYROSCOPE_UNCALIBRATED = 16。UE/Unity 的陀螺仪插件
 * 多半读未校准事件 (data[0..2] 是 x/y/z, data[3..5] 是 bias), 只认 4
 * 会漏掉一半目标。两个 type 的 x/y/z 都在 data[0..2], 改写方式一致。 */
#define SENSOR_TYPE_GYRO_UNCAL  16

/* 单次 sendObjects 最多处理的事件数 (防御越界) */
#define GYRO_MAX_EVENTS         1024

/* ---- 全局状态 ---- */
static struct perf_event * __percpu *gyro_bp; /* per-CPU 执行断点 (wide) */
static bool gyro_bp_wide = true;              /* false = 退回单任务断点 */
static struct perf_event *gyro_single_bp;     /* 退回路径的单任务断点 */
static atomic64_t gyro_hits = ATOMIC64_INIT(0); /* 命中计数, 诊断用 */
static pid_t gyro_pid;                  /* 目标进程 */
static unsigned long gyro_addr;         /* sendObjects 运行时地址 */
static bool gyro_active;
static bool gyro_enabled;
static u32 gyro_rx, gyro_ry, gyro_rz; /* 目标角速度, float 位模式 (rad/s) */

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
static int gyro_resolve_symbol(unsigned long *out_offset)
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

	f = wind_filp_open(GYRO_LIBSENSOR_PATH, O_RDONLY, 0);
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
				base = vma->vm_start;
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
static void gyro_breakpoint_handler(struct perf_event *bp,
                                    struct perf_sample_data *data,
                                    struct pt_regs *regs)
{
	struct mm_struct *mm;
	unsigned long events, count;
	unsigned int i;

	(void)bp;
	(void)data;

	if (!READ_ONCE(gyro_active) || !READ_ONCE(gyro_enabled))
		return;

	/* wide 断点挂在 per-CPU 上: 同一个 VA 在别的进程里也可能被映射到同址,
	 * 只处理目标 tgid, 否则会改到无关进程的 sensor 数据。 */
	if (gyro_pid && current->tgid != gyro_pid)
		return;
	atomic64_inc(&gyro_hits);

	mm = current->mm; /* 断点在目标进程上下文触发 */
	if (!mm)
		return;

#if defined(CONFIG_ARM64)
	events = regs->regs[1];
	count = regs->regs[2];
#elif defined(CONFIG_X86_64)
	events = regs->rsi;
	count = regs->rdx;
#else
	return;
#endif

	if (!events || !count || count > GYRO_MAX_EVENTS)
		return;

	for (i = 0; i < count; i++)
	{
		uintptr_t ev = events + (uintptr_t)i * SENSOR_EVENT_SIZE;
		int32_t type;
		u32 v[3];

		if (!gyro_read_mem(mm, ev + SENSOR_EVENT_TYPE_OFF, &type, sizeof(type)))
			continue;
		if (type != SENSOR_TYPE_GYROSCOPE && type != SENSOR_TYPE_GYRO_UNCAL)
			continue;

		/* 篡改 gyro x/y/z = data[0..2] (float 位模式, 不做 FP 运算) */
		v[0] = gyro_rx;
		v[1] = gyro_ry;
		v[2] = gyro_rz;
		gyro_write_mem(mm, ev + SENSOR_EVENT_DATA_OFF, v, sizeof(v));
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
static int gyro_bind(struct task_struct *task)
{
	unsigned long offset = 0, base = 0;
	struct perf_event_attr attr;
	int ret;

	ret = gyro_resolve_symbol(&offset);
	if (ret)
	{
		printk(KERN_ERR "gyro: symbol %s not found (%d)\n", GYRO_SENDOBJECTS_SYM, ret);
		return ret;
	}
	printk(KERN_INFO "gyro: target offset 0x%lx\n", offset);

	base = gyro_find_module_base(task, GYRO_LIBSENSOR_NAME);
	if (!base)
	{
		printk(KERN_ERR "gyro: pid %d 没有加载 %s: hook 必须装在真正执行 "
		       "BitTube::sendObjects 的进程 (system_server)\n",
		       gyro_pid, GYRO_LIBSENSOR_NAME);
		return -ENOENT;
	}

	gyro_addr = base + offset;

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
int gyro_install(pid_t pid)
{
	struct task_struct *task;
	int ret;

	if (gyro_active)
		return -EBUSY;
	if (pid <= 0)
	{
		printk(KERN_ERR "gyro: 需要显式传入 system_server 的 pid (不是游戏 pid)\n");
		return -EINVAL;
	}

	gyro_pid = pid;
	task = gyro_get_task();
	if (!task)
	{
		gyro_pid = 0;
		return -ESRCH;
	}

	ret = gyro_bind(task);
	if (ret)
	{
		gyro_pid = 0;
		gyro_addr = 0;
		return ret;
	}

	gyro_active = true;
	atomic64_set(&gyro_hits, 0);
	WRITE_ONCE(gyro_enabled, true);
	printk(KERN_INFO "gyro: registered successfully (pid=%d addr=0x%lx wide=%d hits=%lld)\n",
	       pid, gyro_addr, (int)gyro_bp_wide, (long long)atomic64_read(&gyro_hits));
	return 0;
}

void gyro_remove(void)
{
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
	gyro_bp_wide = true;
	gyro_active = false;
	WRITE_ONCE(gyro_enabled, false);
	gyro_pid = 0;
	gyro_addr = 0;
	printk(KERN_INFO "gyro: unregistered\n");
}

void gyro_set_velocity(u32 rx, u32 ry, u32 rz)
{
	gyro_rx = rx;
	gyro_ry = ry;
	gyro_rz = rz;
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

	if (!gyro_bp && !gyro_single_bp)
	{
		ret = gyro_bind(task);
		if (ret)
			return ret;
	}

	WRITE_ONCE(gyro_enabled, true);
	printk(KERN_INFO "gyro: resumed (pid=%d addr=0x%lx)\n", gyro_pid, gyro_addr);
	return 0;
}
