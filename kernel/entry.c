#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/fcntl.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <asm/page.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "breakpoint.h"
#include "kgsl.h"
#include "gyro.h"
#include "sym.h"

/*
 * entry.c — 通讯通道 (hook /dev/null fops, 不注册 dev 节点)
 *
 * 不在 /dev 注册固定节点 (misc_register 会留下 /dev/xxx 可见节点),
 * 改为 hook 系统自带 /dev/null 的 file_operations->unlocked_ioctl / mmap。
 * 用户态 open("/dev/null") + ioctl(WIND_OP_*) 通讯, /dev 下无可疑节点。
 *
 * 为什么选 /dev/null:
 *   系统恒定存在、用户态必能打开; null_fops 的 unlocked_ioctl 与 mmap
 *   均为 NULL, 替换后不影响原有语义 (非 magic ioctl 返回 -ENOTTY)。
 *
 * 写保护: null_fops 位于内核 rodata, 用 set_memory_rw/set_memory_ro 临时
 *   放开页权限再恢复。若目标内核未导出 set_memory_rw (GPL), 需手动改
 *   PTE 清 PTE_WRITE (未验证兜底)。
 *
 * 会话: 改全局单例 g_sess (hook 复用 /dev/null 的 file, 无法用
 *   file->private_data 存会话)。外挂驱动单用户态进程使用, 不额外加锁。
 */

#define HOOK_PATH "/dev/null"

static struct file_operations *g_null_fops;
static long (*g_null_ioctl)(struct file *, unsigned int, unsigned long);
static int  (*g_null_mmap)(struct file *, struct vm_area_struct *);

static struct driver_session *g_sess; /* 全局单例会话 */

/* 判断 ioctl cmd 是否属于独家操作码 (高 16 位 == 0x574E) */
static inline bool is_wind_cmd(unsigned int cmd)
{
	return (cmd & 0xFFFF0000u) == (WIND_OP_BASE & 0xFFFF0000u);
}

/* ---- fops 写保护绕过 (null_fops 在 rodata) ---- */
static int fops_unlock(void)
{
	unsigned long page = (unsigned long)g_null_fops & PAGE_MASK;
	return wind_set_memory_rw(page, 1);
}

static void fops_relock(void)
{
	unsigned long page = (unsigned long)g_null_fops & PAGE_MASK;
	if (wind_set_memory_ro(page, 1))
		printk(KERN_WARNING "wind: set_memory_ro failed (page left rw)\n");
}

/*
 * 批量请求: 用户态传 BATCH_REQUEST 头 + count 个 BATCH_ITEM。
 * 先读头取 count, 再整体复制到内核缓冲后处理。
 */
static long handle_batch(struct driver_session *sess, unsigned long arg, bool is_write)
{
	BATCH_REQUEST head;
	PBATCH_REQUEST req = NULL;
	size_t total;
	long ret = -EINVAL;

	if (copy_from_user(&head, (void __user *)arg, sizeof(head)) != 0)
		return -EFAULT;

	if (head.count == 0 || head.count > 1024)
		return -EINVAL;

	total = sizeof(BATCH_REQUEST) + head.count * sizeof(BATCH_ITEM);
	req = kmalloc(total, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	if (copy_from_user(req, (void __user *)arg, total) != 0) {
		ret = -EFAULT;
		goto out;
	}

	ret = is_write
		? write_process_memory_batch(sess, req)
		: read_process_memory_batch(sess, req);
out:
	kfree(req);
	return ret;
}

static long dispatch_ioctl(unsigned int cmd, unsigned long arg)
{
	COPY_MEMORY cm;
	MODULE_BASE mb;
	MODE_REQUEST mode_req;
	char name[0x100] = {0};

	switch (cmd)
	{
	case OP_BIND:
	{
		pid_t pid = (pid_t)arg;
		if (!g_sess) {
			g_sess = session_create();
			if (!g_sess)
				return -ENOMEM;
		}
		session_bind(g_sess, pid);
		if (!g_sess->bound)
			return -ESRCH;
		return 0;
	}
	case OP_UNBIND:
	{
		if (g_sess) {
			session_destroy(g_sess);
			g_sess = NULL;
		}
		return 0;
	}
	case OP_SET_MODE:
	{
		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&mode_req, (void __user *)arg, sizeof(mode_req)) != 0)
			return -EFAULT;
		if (mode_req.mode > MODE_SAFE)
			return -EINVAL;
		g_sess->read_mode = mode_req.mode;
		return 0;
	}
	case OP_READ_MEM:
	{
		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		if (read_process_memory(g_sess, cm.addr, (void __user *)cm.buffer, cm.size) == false)
			return -EFAULT;
		return 0;
	}
	case OP_WRITE_MEM:
	{
		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		if (write_process_memory(g_sess, cm.addr, (const void __user *)cm.buffer, cm.size) == false)
			return -EFAULT;
		return 0;
	}
	case OP_READ_BATCH:
		if (!g_sess)
			return -ESRCH;
		return handle_batch(g_sess, arg, false);
	case OP_WRITE_BATCH:
		if (!g_sess)
			return -ESRCH;
		return handle_batch(g_sess, arg, true);
	case OP_MODULE_BASE:
	{
		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)) != 0 ||
		    copy_from_user(name, (void __user *)mb.name, sizeof(name) - 1) != 0)
			return -EFAULT;
		mb.base = get_module_base(g_sess, name);
		if (copy_to_user((void __user *)arg, &mb, sizeof(mb)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_HIDE_FILE:
	{
		char hide_path[256] = {0};
		if (copy_from_user(hide_path, (void __user *)arg, sizeof(hide_path) - 1) != 0)
			return -EFAULT;
		return hide_file(hide_path);
	}
	case OP_HIDE_PROCESS:
		return hide_process((pid_t)arg);
	case OP_UNHIDE_ALL:
		return unhide_all();
	case OP_BRK_SET:
	{
		BRK_REQUEST br;
		if (copy_from_user(&br, (void __user *)arg, sizeof(br)) != 0)
			return -EFAULT;
		return brk_set(br.pid, br.addr, br.type);
	}
	case OP_BRK_CLEAR:
	{
		BRK_REQUEST br;
		if (copy_from_user(&br, (void __user *)arg, sizeof(br)) != 0)
			return -EFAULT;
		return brk_clear(br.pid, br.addr);
	}
	case OP_KGSL_HIDE:
		return kgsl_hide_pid((pid_t)arg);
	case OP_KGSL_HOOK:
		return kgsl_hook_install();
	case OP_GYRO_INSTALL:
		return gyro_install((pid_t)arg);
	case OP_GYRO_REMOVE:
		gyro_remove();
		return 0;
	case OP_GYRO_SET:
	{
		GYRO_REQUEST gr;
		if (copy_from_user(&gr, (void __user *)arg, sizeof(gr)) != 0)
			return -EFAULT;
		gyro_set_velocity(gr.rx, gr.ry, gr.rz);
		return 0;
	}
	case OP_GYRO_ENABLE:
		gyro_set_enabled((bool)arg);
		return 0;
	default:
		return -ENOTTY;
	}
}

/* hook 后的 ioctl: 独家 opcode 走 dispatch, 其余走 null 原行为 */
static long hooked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (!is_wind_cmd(cmd)) {
		if (g_null_ioctl)
			return g_null_ioctl(file, cmd, arg);
		return -ENOTTY;
	}
	return dispatch_ioctl(cmd, arg);
}

static int hooked_mmap(struct file *file, struct vm_area_struct *vma)
{
	uintptr_t target_va;

	(void)file;
	if (!g_sess || !g_sess->bound)
		return -ESRCH;

	target_va = (uintptr_t)(vma->vm_pgoff << PAGE_SHIFT);
	return mmap_process_memory(g_sess, vma, target_va, vma->vm_end - vma->vm_start);
}

static int hook_install(void)
{
	struct file *f;
	int ret;

	f = filp_open(HOOK_PATH, O_RDONLY, 0);
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		printk(KERN_ERR "wind: filp_open %s failed %d\n", HOOK_PATH, ret);
		return ret;
	}

	g_null_fops = (struct file_operations *)f->f_op;
	g_null_ioctl = g_null_fops->unlocked_ioctl;
	g_null_mmap  = g_null_fops->mmap;
	filp_close(f, NULL);

	ret = fops_unlock();
	if (ret) {
		printk(KERN_ERR "wind: set_memory_rw failed %d (need manual PTE bypass)\n", ret);
		g_null_fops = NULL;
		return ret;
	}
	g_null_fops->unlocked_ioctl = hooked_ioctl;
	g_null_fops->mmap = hooked_mmap;
	fops_relock();

	printk(KERN_INFO "wind: fops hook installed (fops=%p)\n", g_null_fops);
	return 0;
}

static void hook_remove(void)
{
	if (!g_null_fops)
		return;

	fops_unlock();
	g_null_fops->unlocked_ioctl = g_null_ioctl;
	g_null_fops->mmap = g_null_mmap;
	fops_relock();

	printk(KERN_INFO "wind: fops hook removed\n");
	g_null_fops = NULL;
}

static int __init driver_entry(void)
{
	int ret;

	printk("[+] wind driver_entry");
	ret = wind_sym_init();
	if (ret)
		return ret;
	ret = hook_install();
	if (ret) {
		wind_sym_exit();
		return ret;
	}
	return 0;
}

static void __exit driver_unload(void)
{
	printk("[+] wind driver_unload");
	gyro_remove();
	kgsl_hook_remove();
	hook_remove();
	if (g_sess) {
		session_destroy(g_sess);
		g_sess = NULL;
	}
	wind_sym_exit();
}

module_init(driver_entry);
module_exit(driver_unload);

MODULE_DESCRIPTION("Wind Linux driver - kernel driver for all devices.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("TomieNian-阿念");
