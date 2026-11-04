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
 * 不能睡眠, 因此数据篡改走 translate_linear_address 页表翻译 +
 * phys_to_virt 直读直写 (非睡眠路径), 复用 memory.c 的翻译函数。
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

/* 单次 sendObjects 最多处理的事件数 (防御越界) */
#define GYRO_MAX_EVENTS         1024

/* ---- 全局状态 ---- */
static struct perf_event *gyro_bp;      /* 硬件执行断点 */
static pid_t gyro_pid;                  /* 目标进程 */
static unsigned long gyro_addr;         /* sendObjects 运行时地址 */
static bool gyro_active;
static bool gyro_enabled;
static u32 gyro_rx, gyro_ry, gyro_rz; /* 目标角速度, float 位模式 (rad/s) */

/* ===================================================================
 * 非睡眠内存读写: 页表翻译 + phys_to_virt 直读直写。
 * 逐页分段, 支持跨页读写。中断上下文安全 (不睡眠)。
 * =================================================================== */
static bool gyro_read_mem(struct mm_struct *mm, uintptr_t va, void *buf, size_t size)
{
	uint8_t *dst = buf;

	while (size > 0)
	{
		phys_addr_t pa = translate_linear_address(mm, va);
		void *kva;
		size_t chunk;

		if (!pa)
			return false;
		chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
		if (chunk > size)
			chunk = size;
		kva = wind_phys_to_virt(pa);
		if (!kva)
			return false;
		memcpy(dst, kva, chunk);
		dst += chunk;
		va += chunk;
		size -= chunk;
	}
	return true;
}

static bool gyro_write_mem(struct mm_struct *mm, uintptr_t va, const void *buf, size_t size)
{
	const uint8_t *src = buf;

	while (size > 0)
	{
		phys_addr_t pa = translate_linear_address(mm, va);
		void *kva;
		size_t chunk;

		if (!pa)
			return false;
		chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
		if (chunk > size)
			chunk = size;
		kva = wind_phys_to_virt(pa);
		if (!kva)
			return false;
		memcpy(kva, src, chunk);
		kh_flush_dcache(kva, chunk);
		src += chunk;
		va += chunk;
		size -= chunk;
	}
	return true;
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

	f = filp_open(GYRO_LIBSENSOR_PATH, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	pos = 0;
	if (kernel_read(f, &ehdr, sizeof(ehdr), &pos) != sizeof(ehdr))
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
	if (kernel_read(f, shdrs, ehdr.e_shnum * ehdr.e_shentsize, &pos) !=
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
		if (kernel_read(f, shstrtab, sh->sh_size, &pos) != (ssize_t)sh->sh_size)
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
	if (kernel_read(f, dynstr, dynstr_size, &pos) != (ssize_t)dynstr_size)
	{
		ret = -EIO;
		goto out;
	}
	pos = dynsym_off;
	if (kernel_read(f, dynsym, dynsym_size, &pos) != (ssize_t)dynsym_size)
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

	mm = get_task_mm(task);
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
			char *p = file_path(vma->vm_file, buf, sizeof(buf) - 1);

			if (!strcmp(kbasename(p), name))
			{
				base = vma->vm_start;
				break;
			}
		}
	}
	kh_mmap_read_unlock(mm);
	mmput(mm);
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

	if (!gyro_active || !gyro_enabled)
		return;

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
		if (type != SENSOR_TYPE_GYROSCOPE)
			continue;

		/* 篡改 gyro x/y/z = data[0..2] (float 位模式, 不做 FP 运算) */
		v[0] = gyro_rx;
		v[1] = gyro_ry;
		v[2] = gyro_rz;
		gyro_write_mem(mm, ev + SENSOR_EVENT_DATA_OFF, v, sizeof(v));
	}
}

/* ===================================================================
 * 安装 hook: 解析符号 -> 找基址 -> 设执行断点。
 * =================================================================== */
int gyro_install(pid_t pid)
{
	struct pid *pid_struct;
	struct task_struct *task;
	unsigned long offset = 0, base = 0;
	struct perf_event_attr attr;
	int ret;

	if (gyro_active)
		return -EBUSY;

	ret = gyro_resolve_symbol(&offset);
	if (ret)
	{
		printk(KERN_ERR "gyro: symbol %s not found (%d)\n", GYRO_SENDOBJECTS_SYM, ret);
		return ret;
	}
	printk(KERN_INFO "gyro: target offset 0x%lx\n", offset);

	pid_struct = find_get_pid(pid);
	if (!pid_struct)
		return -ESRCH;
	task = pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task)
		return -ESRCH;

	base = gyro_find_module_base(task, GYRO_LIBSENSOR_NAME);
	if (!base)
	{
		printk(KERN_ERR "gyro: failed to grab target inode\n");
		return -ENOENT;
	}

	gyro_addr = base + offset;

	hw_breakpoint_init(&attr);
	attr.bp_addr = gyro_addr;
	attr.bp_len = HW_BREAKPOINT_LEN_8; /* arm64 执行断点 */
	attr.bp_type = HW_BREAKPOINT_X;

	gyro_bp = register_user_hw_breakpoint(&attr, gyro_breakpoint_handler, NULL, task);
	if (IS_ERR(gyro_bp))
	{
		ret = PTR_ERR(gyro_bp);
		gyro_bp = NULL;
		printk(KERN_ERR "gyro: uprobe register failed %d\n", ret);
		return ret;
	}

	gyro_pid = pid;
	gyro_active = true;
	gyro_enabled = true;
	printk(KERN_INFO "gyro: registered successfully (pid=%d addr=0x%lx)\n", pid, gyro_addr);
	return 0;
}

void gyro_remove(void)
{
	if (gyro_bp)
	{
		unregister_hw_breakpoint(gyro_bp);
		gyro_bp = NULL;
	}
	gyro_active = false;
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
	gyro_enabled = enabled;
}
