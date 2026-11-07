#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/namei.h>
#include <linux/fcntl.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <asm/page.h>
#include <asm/sysreg.h>
#include "comm.h"
#include "memory.h"
#include "process.h"
#include "breakpoint.h"
#include "kgsl.h"
#include "gyro.h"
#include "sym.h"
#include "nlcomm.h"

/*
 * entry.c — 通讯通道 (hook /dev/null inode->i_fop, 不注册 dev 节点)
 *
 * 不在 /dev 注册固定节点 (misc_register 会留下 /dev/xxx 可见节点),
 * 改为把 /dev/null inode 的 i_fop 指向本模块内的一份 file_operations
 * 副本 (内容复制自真 null_fops, 只覆盖 unlocked_ioctl / mmap, .open 置空
 * 以阻断 chrdev_open)。用户态 open("/dev/null") + ioctl(WIND_OP_*) 通讯,
 * /dev 下无可疑节点。
 *
 * 为什么改 inode 而不是改 null_fops: null_fops 在 .rodata, 该核 rodata 是
 *   2MB block 映射, 改描述符 AP[2] 会让整块 2MB 短暂可写, 真机实测直接
 *   卡死重启。inode 是 kmalloc 对象, i_fop 指针本就可写, 只写 8 字节。
 *
 * 为什么选 /dev/null: 系统恒定存在、用户态必能打开; 原 null_fops 的
 *   unlocked_ioctl 与 mmap 均为 NULL, 替换后不影响原有语义。
 *
 * 卸载: 重新 filp_open 取当前 inode 并把 i_fop 恢复成保存的原值。
 *   fops.owner = NULL, rmmod 不会被 /dev/null 的持有者卡住。
 *
 * 会话: 改全局单例 g_sess (hook 复用 /dev/null 的 file, 无法用
 *   file->private_data 存会话)。外挂驱动单用户态进程使用, 不额外加锁。
 */

#define HOOK_PATH "/dev/null"

static struct file_operations *g_null_fops;
static long (*g_null_ioctl)(struct file *, unsigned int, unsigned long);
static int  (*g_null_mmap)(struct file *, struct vm_area_struct *);

static struct driver_session *g_sess; /* 全局单例会话 */

/* insmod wind.ko install_hook=0 → 只解析符号, 不动 rodata, 不装 fops hook。
 * 用于设备上排障: 确认 insmod 本身是否安全。 */
static bool install_hook = true;
module_param(install_hook, bool, 0644);
MODULE_PARM_DESC(install_hook, "install /dev/null fops hook at load time");

/* 二分定位用: 0=不装, 1=只把 inode->i_fop 指向副本 (handler 保持真 null_fops),
 * 2=额外覆盖 unlocked_ioctl/mmap (完整通道), 3=保留原 .open (纯诊断, 通道不生效)。 */
static int hook_level = 2;
module_param(hook_level, int, 0644);
MODULE_PARM_DESC(hook_level, "0=off 1=fops redirect only 2=redirect+ioctl/mmap 3=keep orig .open");

/* 通讯通道:
 *   3 = 内核 netlink 私有协议 (默认, 无节点, 无 hook)
 *   2 = 私有 misc 字符设备 (需 mknod)
 *   1 = 重定向 /dev/null 的 inode->i_fop (有卡死重启记录) */
static int transport = 3;
module_param(transport, int, 0644);
MODULE_PARM_DESC(transport, "3=netlink 2=misc chardev 1=/dev/null i_fop");

/* 判断 ioctl cmd 是否属于独家操作码 (高 16 位 == 0x574E) */
static inline bool is_wind_cmd(unsigned int cmd)
{
	return (cmd & 0xFFFF0000u) == (WIND_OP_BASE & 0xFFFF0000u);
}

/* ---- 说明: null_fops 在 .rodata, 该核是 2MB block 映射, 改描述符 AP[2]
 * 会让整块短暂可写并实测触发卡死重启, 因此装载路径完全不碰 rodata。
 * 通讯通道改为写 /dev/null 的 inode->i_fop (kmalloc 对象, 本就可写)。
 */

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

/* netlink 通道与 ioctl 通道共用同一套分发逻辑 */
long wind_dispatch_ioctl(unsigned int cmd, unsigned long arg);

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
	case OP_GYRO_STOP:
		return gyro_stop();
	case OP_GYRO_RESUME:
		return gyro_resume();

	/* ---- UDP 断点 (kprobe 内核 UDP 收发路径) ---- */
	case OP_UDPBP_SET:
	{
		UDPBP_SET_REQUEST req;
		int32_t slot = -1;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		ret = udpbp_set(req.slot_index, &req.point, &slot);
		if (ret)
			return ret;
		if (req.slot_out &&
		    copy_to_user((void __user *)req.slot_out, &slot, sizeof(slot)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_UDPBP_REMOVE:
		return udpbp_remove((int32_t)arg);
	case OP_UDPBP_GET_CONFIG:
	{
		UDPBP_CONFIG cfg;
		int ret = udpbp_get_config(&cfg);

		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_UDPBP_GET_RECORDS:
	{
		UDPBP_RECORDS_REQUEST req;
		UDPBP_RECORD *kbuf;
		uint32_t count = 0;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		if (req.capacity == 0 || req.capacity > 4096)
			return -EINVAL;
		kbuf = kmalloc(req.capacity * sizeof(UDPBP_RECORD), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		ret = udpbp_get_records(req.slot_index, req.start_index, kbuf,
					req.capacity, &count);
		if (ret == 0 && count)
			if (copy_to_user((void __user *)req.records, kbuf,
					 count * sizeof(UDPBP_RECORD)) != 0)
				ret = -EFAULT;
		kfree(kbuf);
		if (ret)
			return ret;
		if (req.count_out && copy_to_user((void __user *)req.count_out,
						  &count, sizeof(count)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_UDPBP_CLEAR_RECORDS:
	{
		UDPBP_CLEAR_REQUEST req;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return udpbp_clear_records(req.slot_index);
	}

	/* ---- 硬件断点 (BRP/WRP) ---- */
	case OP_HWBP_SET:
	{
		HWBP_SET_REQUEST head;
		PHWBP_SET_REQUEST req;
		size_t total;
		int ret;

		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&head, (void __user *)arg, sizeof(head)) != 0)
			return -EFAULT;
		if (head.point_count == 0 || head.point_count > 16)
			return -EINVAL;
		total = sizeof(HWBP_SET_REQUEST) + head.point_count * sizeof(HWBP_POINT_CONFIG);
		req = kmalloc(total, GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		if (copy_from_user(req, (void __user *)arg, total) != 0) {
			kfree(req);
			return -EFAULT;
		}
		ret = hwbp_set(g_sess, req->points, head.point_count);
		kfree(req);
		return ret;
	}
	case OP_HWBP_REMOVE:
		return hwbp_remove();
	case OP_HWBP_GET_CONFIG:
	{
		HWBP_CONFIG cfg;
		int ret = hwbp_get_config(&cfg);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_HWBP_GET_RECORDS:
	{
		HWBP_RECORDS_REQUEST req;
		HWBP_RECORD *kbuf;
		uint32_t count = 0;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		if (req.capacity == 0 || req.capacity > 4096)
			return -EINVAL;
		kbuf = kmalloc(req.capacity * sizeof(HWBP_RECORD), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		ret = hwbp_get_records(req.point_index, req.start_index, kbuf,
				       req.capacity, &count);
		if (ret == 0 && count)
			if (copy_to_user((void __user *)req.records, kbuf,
					 count * sizeof(HWBP_RECORD)) != 0)
				ret = -EFAULT;
		kfree(kbuf);
		if (ret)
			return ret;
		if (req.count_out && copy_to_user((void __user *)req.count_out,
						  &count, sizeof(count)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_HWBP_SET_RECORD:
	{
		HWBP_RECORD_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return hwbp_set_record(req.point_index, req.record_index, &req.record);
	}
	case OP_HWBP_CLEAR_RECORDS:
	{
		HWBP_CLEAR_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return hwbp_clear_records(req.point_index);
	}

	/* ---- PTE 影子页执行断点 ---- */
	case OP_PTEBP_SET:
	{
		PTEBP_SET_REQUEST head;
		PPTEBP_SET_REQUEST req;
		size_t total;
		int ret;

		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&head, (void __user *)arg, sizeof(head)) != 0)
			return -EFAULT;
		if (head.point_count == 0 || head.point_count > 16)
			return -EINVAL;
		total = sizeof(PTEBP_SET_REQUEST) + head.point_count * sizeof(PTEBP_POINT_CONFIG);
		req = kmalloc(total, GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		if (copy_from_user(req, (void __user *)arg, total) != 0) {
			kfree(req);
			return -EFAULT;
		}
		ret = ptebp_set(g_sess, req->points, head.point_count);
		kfree(req);
		return ret;
	}
	case OP_PTEBP_REMOVE:
		return ptebp_remove();
	case OP_PTEBP_GET_CONFIG:
	{
		PTEBP_CONFIG cfg;
		int ret = ptebp_get_config(&cfg);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_PTEBP_GET_RECORDS:
	{
		PTEBP_RECORDS_REQUEST req;
		PTEBP_RECORD *kbuf;
		uint32_t count = 0;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		if (req.capacity == 0 || req.capacity > 4096)
			return -EINVAL;
		kbuf = kmalloc(req.capacity * sizeof(PTEBP_RECORD), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		ret = ptebp_get_records(req.point_index, req.start_index, kbuf,
					req.capacity, &count);
		if (ret == 0 && count)
			if (copy_to_user((void __user *)req.records, kbuf,
					 count * sizeof(PTEBP_RECORD)) != 0)
				ret = -EFAULT;
		kfree(kbuf);
		if (ret)
			return ret;
		if (req.count_out && copy_to_user((void __user *)req.count_out,
						  &count, sizeof(count)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_PTEBP_SET_RECORD:
	{
		PTEBP_RECORD_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return ptebp_set_record(req.point_index, req.record_index, &req.record);
	}
	case OP_PTEBP_CLEAR_RECORDS:
	{
		PTEBP_CLEAR_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return ptebp_clear_records(req.point_index);
	}

	/* ---- BRK 软断点 ---- */
	case OP_BRKBP_SET:
	{
		BRKBP_SET_REQUEST head;
		PBRKBP_SET_REQUEST req;
		size_t total;
		int ret;

		if (!g_sess)
			return -ESRCH;
		if (copy_from_user(&head, (void __user *)arg, sizeof(head)) != 0)
			return -EFAULT;
		if (head.point_count == 0 || head.point_count > 16)
			return -EINVAL;
		total = sizeof(BRKBP_SET_REQUEST) + head.point_count * sizeof(BRKBP_POINT_CONFIG);
		req = kmalloc(total, GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		if (copy_from_user(req, (void __user *)arg, total) != 0) {
			kfree(req);
			return -EFAULT;
		}
		ret = brkbp_set(g_sess, req->points, head.point_count);
		kfree(req);
		return ret;
	}
	case OP_BRKBP_REMOVE:
		return brkbp_remove();
	case OP_BRKBP_GET_CONFIG:
	{
		BRKBP_CONFIG cfg;
		int ret = brkbp_get_config(&cfg);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_BRKBP_GET_RECORDS:
	{
		BRKBP_RECORDS_REQUEST req;
		BRKBP_RECORD *kbuf;
		uint32_t count = 0;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		if (req.capacity == 0 || req.capacity > 4096)
			return -EINVAL;
		kbuf = kmalloc(req.capacity * sizeof(BRKBP_RECORD), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		ret = brkbp_get_records(req.point_index, req.start_index, kbuf,
					req.capacity, &count);
		if (ret == 0 && count)
			if (copy_to_user((void __user *)req.records, kbuf,
					 count * sizeof(BRKBP_RECORD)) != 0)
				ret = -EFAULT;
		kfree(kbuf);
		if (ret)
			return ret;
		if (req.count_out && copy_to_user((void __user *)req.count_out,
						  &count, sizeof(count)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_BRKBP_SET_RECORD:
	{
		BRKBP_RECORD_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return brkbp_set_record(req.point_index, req.record_index, &req.record);
	}
	case OP_BRKBP_CLEAR_RECORDS:
	{
		BRKBP_CLEAR_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return brkbp_clear_records(req.point_index);
	}

	/* ---- ETM/PTM 分支追踪 ---- */
	case OP_ETMBP_SET:
	{
		ETMBP_SET_REQUEST head;
		PETMBP_SET_REQUEST req;
		size_t total;
		int ret;

		if (copy_from_user(&head, (void __user *)arg, sizeof(head)) != 0)
			return -EFAULT;
		if (head.point_count == 0 || head.point_count > 16)
			return -EINVAL;
		total = sizeof(ETMBP_SET_REQUEST) + head.point_count * sizeof(ETMBP_POINT_CONFIG);
		req = kmalloc(total, GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		if (copy_from_user(req, (void __user *)arg, total) != 0) {
			kfree(req);
			return -EFAULT;
		}
		ret = etmbp_set(req->points, head.point_count);
		kfree(req);
		return ret;
	}
	case OP_ETMBP_REMOVE:
		return etmbp_remove();
	case OP_ETMBP_GET_CONFIG:
	{
		ETMBP_CONFIG cfg;
		int ret = etmbp_get_config(&cfg);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &cfg, sizeof(cfg)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_ETMBP_GET_RECORDS:
	{
		ETMBP_RECORDS_REQUEST req;
		ETMBP_RECORD *kbuf;
		uint32_t count = 0;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		if (req.capacity == 0 || req.capacity > 4096)
			return -EINVAL;
		kbuf = kmalloc(req.capacity * sizeof(ETMBP_RECORD), GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
		ret = etmbp_get_records(req.point_index, req.start_index, kbuf,
					req.capacity, &count);
		if (ret == 0 && count)
			if (copy_to_user((void __user *)req.records, kbuf,
					 count * sizeof(ETMBP_RECORD)) != 0)
				ret = -EFAULT;
		kfree(kbuf);
		if (ret)
			return ret;
		if (req.count_out && copy_to_user((void __user *)req.count_out,
						  &count, sizeof(count)) != 0)
			return -EFAULT;
		return 0;
	}
	case OP_ETMBP_SET_RECORD:
	{
		ETMBP_RECORD_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return etmbp_set_record(req.point_index, req.record_index, &req.record);
	}
	case OP_ETMBP_CLEAR_RECORDS:
	{
		ETMBP_CLEAR_REQUEST req;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0)
			return -EFAULT;
		return etmbp_clear_records(req.point_index);
	}
	default:
		return -ENOTTY;
	}
}

/* netlink 通道入口: 协议号校验由 nlcomm.c 完成, 这里只做转发 */
long wind_dispatch_ioctl(unsigned int cmd, unsigned long arg)
{
	return dispatch_ioctl(cmd, arg);
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

/* ---- transport=2: 私有 misc 字符设备, 不碰 /dev/null 也不碰 rodata ---- */
static const struct file_operations g_wind_dev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = hooked_ioctl,
	.mmap = hooked_mmap,
};

static struct miscdevice g_wind_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "windc",
	.fops = &g_wind_dev_fops,
	.mode = 0600,
};

static int wind_misc_install(void)
{
	int ret = wind_misc_register(&g_wind_misc);

	if (ret) {
		printk(KERN_ERR "wind: misc_register 失败 %d\n", ret);
		return ret;
	}
	printk(KERN_INFO "wind: misc 设备已注册 name=%s minor=%u, 用户态 mknod PATH c 10 %u\n",
	       g_wind_misc.name, g_wind_misc.minor, g_wind_misc.minor);
	return 0;
}

static void wind_misc_remove(void)
{
	if (!g_wind_misc.this_device)
		return;
	wind_misc_deregister(&g_wind_misc);
	printk(KERN_INFO "wind: misc 设备已注销\n");
}

/* ------------------------------------------------------------------
 * 通讯通道: 改 /dev/null 的 inode->i_fop 指向本模块内的 fops 副本。
 *
 * 为什么不改 null_fops 本身: null_fops 在 .rodata, 该核 rodata 是 2MB
 * block 映射, 改 block 描述符 AP[2] 会让整块 2MB 短暂可写, 实测触发
 * 卡死重启。inode 与 struct cdev 都是 kmalloc 对象, 字段本就可写。
 *
 * 字符设备 inode->i_fop 本来是 def_chr_fops (只有 .open = chrdev_open),
 * open 时 chrdev_open 用 inode->i_cdev->ops (= memory_fops) 的 .open
 * 调 memory_open, 再 replace_fops 换成真正的 null_fops。
 * 所以副本必须:
 *   1. 内容取 filp_open 之后的 f->f_op (此时已是真 null_fops 的 read/write/llseek)
 *   2. .open 置 NULL → chrdev_open 不被调用, 不会被换回真 null_fops
 *   3. 只覆盖 unlocked_ioctl / mmap
 * 全程只写 inode->i_fop 一个指针与本模块 .data, 不碰页表、不碰 rodata。
 * ------------------------------------------------------------------ */
static struct file_operations g_wind_fops;
static const struct file_operations *g_inode_fop_orig; /* 原 inode->i_fop, 卸载恢复 */
static struct inode *g_null_inode;

static void hook_remove(void);

static int hook_install(void)
{
	const struct file_operations *real_fops;
	struct inode *inode;
	struct file *f;
	int ret = 0;

	f = wind_filp_open(HOOK_PATH, O_RDONLY, 0);
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		printk(KERN_ERR "wind: filp_open %s failed %d\n", HOOK_PATH, ret);
		return ret;
	}

	inode = f->f_inode;
	/* 打开过的 /dev/null: f_op 已经过 memory_open 换成真正的 null_fops */
	real_fops = f->f_op;
	if (!inode || !real_fops) {
		printk(KERN_ERR "wind: 取不到 %s 的 fops/inode\n", HOOK_PATH);
		filp_close(f, NULL);
		return 0;
	}

	g_null_fops = (struct file_operations *)real_fops;
	g_null_ioctl = real_fops->unlocked_ioctl;
	g_null_mmap  = real_fops->mmap;

	memcpy(&g_wind_fops, real_fops, sizeof(g_wind_fops));
	/* hook_level 3 = 纯诊断档: 保留原 .open, 让 chrdev_open 照常跑
	 * (fops 会被换回真 null_fops, 通道不生效), 只留 inode 指向我们的副本。
	 * 用来判断 '卡死重启' 是 .open 被跳过造成的, 还是重定向本身造成的。 */
	if (hook_level != 3)
		g_wind_fops.open = NULL; /* 阻断 chrdev_open, 保住本 fops */
	if (hook_level >= 2) {
		g_wind_fops.unlocked_ioctl = hooked_ioctl;
		g_wind_fops.mmap = hooked_mmap;
	} else if (hook_level == 1) {
		printk(KERN_INFO "wind: hook_level=1, 只重定向 fops, 不覆盖 ioctl/mmap\n");
	} else if (hook_level == 3) {
		printk(KERN_INFO "wind: hook_level=3, 保留原 .open, 通道不生效(仅诊断)\n");
	}
	/* owner 必须为 NULL: 置 THIS_MODULE 的话, 系统里任何 exec 到 /dev/null
	 * 的进程都会持一个模块引用, rmmod 永远 EBUSY。
	 * 代价: 卸载后, 已存在的 /dev/null fd 若再发 ioctl 会跳进已释放内存。
	 * read/write 指向内核真函数, 不受影响; 实际只有本 loader 会发 ioctl,
	 * 卸载即退出, 不触发。 */
	g_wind_fops.owner = NULL;

	g_inode_fop_orig = inode->i_fop;
	g_null_inode = inode;

	/* 单指针赋值, arm64 8 字节对齐写原子 */
	WRITE_ONCE(inode->i_fop, &g_wind_fops);

	printk(KERN_INFO "wind: i_fop hooked inode=%px orig=%px real=%px new=%px\n",
	       inode, g_inode_fop_orig, real_fops, &g_wind_fops);

	filp_close(f, NULL);

	if (hook_level == 3) {
		printk(KERN_INFO "wind: hook_level=3 诊断档, 跳过自检 (通道本就不生效)\n");
		return 0;
	}

	/* 自检: 重新打开 /dev/null, 必须落到我们的 fops 上; 否则立刻回滚,
	 * 绝不把错误状态留在系统里 (这是上一轮留下野 f_op 的教训)。 */
	f = wind_filp_open(HOOK_PATH, O_RDONLY, 0);
	if (IS_ERR(f)) {
		printk(KERN_ERR "wind: 自检 filp_open 失败 %ld, 回滚\n", PTR_ERR(f));
		hook_remove();
		return 0;
	}
	if (f->f_op != &g_wind_fops) {
		printk(KERN_ERR "wind: 自检未命中 (f_op=%px), 回滚\n",
		       (void *)f->f_op);
		filp_close(f, NULL);
		hook_remove();
		return 0;
	}
	filp_close(f, NULL);
	printk(KERN_INFO "wind: 自检通过, /dev/null 已指向 wind fops\n");
	return 0;
}

static void hook_remove(void)
{
	struct file *f;

	if (!g_null_fops || !g_inode_fop_orig)
		return;

	/* 重新打开取当前 inode, 不依赖保存的 inode 指针 (可能被回收) */
	f = wind_filp_open(HOOK_PATH, O_RDONLY, 0);
	if (IS_ERR(f)) {
		printk(KERN_ERR "wind: 恢复失败 filp_open %ld —— 卸载前必须重启\n",
		       PTR_ERR(f));
		return; /* 保留 g_inode_fop_orig, 不置空, 让下次卸载再试 */
	}

	if (f->f_inode)
		WRITE_ONCE(f->f_inode->i_fop, g_inode_fop_orig);
	filp_close(f, NULL);

	printk(KERN_INFO "wind: i_fop restored\n");
	g_null_inode = NULL;
	g_inode_fop_orig = NULL;
	g_null_fops = NULL;
}

static int __init driver_entry(void)
{
	int ret;

	printk("[+] wind driver_entry");
	ret = wind_sym_init();
	if (ret)
		return ret;

	if (!install_hook || hook_level <= 0 || transport == 0) {
		printk(KERN_INFO "wind: 跳过通讯通道 (install_hook=%d hook_level=%d transport=%d), 仅符号解析\n",
		       (int)install_hook, hook_level, transport);
		return 0;
	}

	if (transport == 3) {
		ret = wind_nl_install();
		if (!ret)
			return 0;
		printk(KERN_WARNING "wind: netlink 通道失败 %d, 回退 transport=2 (misc)\n", ret);
		transport = 2;
	}

	if (transport == 2) {
		ret = wind_misc_install();
		if (!ret)
			return 0;
		printk(KERN_WARNING "wind: misc 通道失败 %d, 回退 transport=1 (/dev/null)\n", ret);
		transport = 1;
	}

	if (transport == 1) {
		ret = hook_install();
		if (ret) {
			wind_sym_exit();
			return ret;
		}
		if (!g_null_fops)
			printk(KERN_WARNING "wind: 已加载但通讯通道未建立, 见上方报错\n");
	}

	return 0;
}

static void __exit driver_unload(void)
{
	printk("[+] wind driver_unload");
	gyro_remove();
	kgsl_hook_remove();
	wind_bp_exit();
	wind_nl_remove();
	wind_misc_remove();
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
