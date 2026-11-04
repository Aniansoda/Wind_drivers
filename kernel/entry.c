#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "breakpoint.h"
#include "kgsl.h"

#define DEVICE_NAME "WindNian"

static struct driver_session *file_session(struct file *file)
{
	return (struct driver_session *)file->private_data;
}

int dispatch_open(struct inode *node, struct file *file)
{
	struct driver_session *sess;

	(void)node;
	sess = session_create();
	if (!sess)
		return -ENOMEM;
	file->private_data = sess;
	return 0;
}

int dispatch_close(struct inode *node, struct file *file)
{
	struct driver_session *sess = file_session(file);

	(void)node;
	if (sess) {
		session_destroy(sess);
		file->private_data = NULL;
	}
	return 0;
}

/* ---- mmap: offset 即目标虚拟地址 (页对齐), pid 已在 open 后绑定 ---- */
static int dispatch_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct driver_session *sess = file_session(file);
	uintptr_t target_va;

	if (!sess || !sess->bound)
		return -ESRCH;

	target_va = (uintptr_t)(vma->vm_pgoff << PAGE_SHIFT);
	return mmap_process_memory(sess, vma, target_va, vma->vm_end - vma->vm_start);
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

long dispatch_ioctl(struct file *const file, unsigned int const cmd, unsigned long const arg)
{
	struct driver_session *sess = file_session(file);
	COPY_MEMORY cm;
	MODULE_BASE mb;
	MODE_REQUEST mode_req;
	char name[0x100] = {0};

	if (!sess)
		return -ESRCH;

	switch (cmd)
	{
	case OP_BIND:
	{
		pid_t pid = (pid_t)arg;
		session_bind(sess, pid);
		if (!sess->bound)
			return -ESRCH;
		return 0;
	}
	case OP_UNBIND:
	{
		session_destroy(sess);
		sess = session_create();
		if (!sess)
			return -ENOMEM;
		file->private_data = sess;
		return 0;
	}
	case OP_SET_MODE:
	{
		if (copy_from_user(&mode_req, (void __user *)arg, sizeof(mode_req)) != 0)
			return -EFAULT;
		if (mode_req.mode > MODE_SAFE)
			return -EINVAL;
		sess->read_mode = mode_req.mode;
		return 0;
	}
	case OP_READ_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		if (read_process_memory(sess, cm.addr, (void __user *)cm.buffer, cm.size) == false)
			return -EFAULT;
		return 0;
	}
	case OP_WRITE_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		if (write_process_memory(sess, cm.addr, (const void __user *)cm.buffer, cm.size) == false)
			return -EFAULT;
		return 0;
	}
	case OP_READ_BATCH:
		return handle_batch(sess, arg, false);
	case OP_WRITE_BATCH:
		return handle_batch(sess, arg, true);
	case OP_MODULE_BASE:
	{
		if (copy_from_user(&mb, (void __user *)arg, sizeof(mb)) != 0 ||
		    copy_from_user(name, (void __user *)mb.name, sizeof(name) - 1) != 0)
			return -EFAULT;
		mb.base = get_module_base(sess, name);
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
	default:
		return -ENOTTY;
	}
}

struct file_operations dispatch_functions = {
	.owner = THIS_MODULE,
	.open = dispatch_open,
	.release = dispatch_close,
	.unlocked_ioctl = dispatch_ioctl,
	.mmap = dispatch_mmap,
};

struct miscdevice misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &dispatch_functions,
};

int __init driver_entry(void)
{
	int ret;
	printk("[+] driver_entry");
	ret = misc_register(&misc);
	return ret;
}

void __exit driver_unload(void)
{
	printk("[+] driver_unload");
	misc_deregister(&misc);
}

module_init(driver_entry);
module_exit(driver_unload);

MODULE_DESCRIPTION("Wind-Driven Linux.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("WindNian");
