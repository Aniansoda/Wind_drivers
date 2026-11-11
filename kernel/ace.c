#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/percpu.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/seq_file.h>
#include <linux/pid.h>
#include <linux/list.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/proc_fs.h>

#include "ace.h"
#include "comm.h"

#define ACE_PFX "[wind-ace] "

static uint32_t g_ace_tgid;
/* 单项开关位掩码, 默认全开。高 3 位留给后续功能。 */
#define ACE_F_MAPS     (1u << 0)
#define ACE_F_MODS     (1u << 1)
#define ACE_F_KSYM     (1u << 2)
#define ACE_F_TRACERP  (1u << 3)
#define ACE_F_FS       (1u << 4)
#define ACE_F_COMM     (1u << 5)
#define ACE_F_PORT     (1u << 6)
#define ACE_F_MASK     0x7Fu

static u32 g_ace_feat = ACE_F_MASK;
static int ace_verbose;

static inline bool ace_feat(u32 bit)
{
	return READ_ONCE(g_ace_feat) & bit;
}

#define ACE_LOG(fmt, ...)						\
	do {								\
		if (READ_ONCE(ace_verbose))				\
			pr_info(ACE_PFX fmt, ##__VA_ARGS__);		\
	} while (0)
static atomic64_t g_ace_maps_drop;
static atomic64_t g_ace_mod_drop;
static atomic64_t g_ace_ksym_drop;
static atomic64_t g_ace_tp_hide;
static atomic64_t g_ace_fs_drop;
static atomic64_t g_ace_comm_drop;
static atomic64_t g_ace_port_drop;

static const char *const ace_deny[] = {
	"libwind", "wind.ko", "keep.sh", "libkeep",
	"gadget", "libfrida", "frida-agent", "substrate",
	"xposed", "lsposed", "riru", "zygisk",
	"dobby", "shadowhook", "whale", "sandhook",
	"libmem", "gameguardian",
};

/* fs 层用全路径前缀匹配: "su" 这类短词做 strstr 会误伤 status/issue 等 */
static const char *const ace_path_deny[] = {
	"/system/bin/su", "/system/xbin/su", "/sbin/su",
	"/su/bin/su", "/debug_ramdisk/su", "/data/local/tmp/su",
	"/data/adb/magisk", "/data/adb/ksu", "/data/adb/ap",
	"/data/adb/modules", "/sbin/magisk", "/cache/magisk",
	"/system/app/Superuser.apk", "/system/bin/frida-server",
	"/data/local/tmp/frida-server",
};

static bool ace_fs_deny(const char *p)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(ace_path_deny); ++i) {
		size_t n = strlen(ace_path_deny[i]);

		if (strncmp(p, ace_path_deny[i], n) == 0)
			return true;
	}
	return false;
}

static bool ace_deny_name(const char *p)
{
	int i;

	if (!p || !*p)
		return false;
	for (i = 0; i < (int)ARRAY_SIZE(ace_deny); ++i)
		if (strstr(p, ace_deny[i]))
			return true;
	return false;
}

static inline bool ace_is_target(pid_t tgid)
{
	uint32_t t = READ_ONCE(g_ace_tgid);

	return t && tgid > 0 && (uint32_t)tgid == t;
}

static inline bool ace_cur_is_target(void)
{
	return ace_is_target(current->tgid);
}

/* ---- B1/B2/B3: /proc/<pid>/maps 行级过滤 ---- */

static noinline int ace_maps_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct vm_area_struct *vma = (struct vm_area_struct *)regs->regs[1];
	char *buf, *p;
	bool deny;

	(void)kp;
	if (!vma)
		return 0;
	if (!vma->vm_file)
		return 0;
	if (!ace_feat(ACE_F_MAPS))
		return 0;
	if (!ace_cur_is_target())
		return 0;
	buf = kmalloc(PATH_MAX, GFP_ATOMIC);
	if (!buf)
		return 0;
	p = d_path(&vma->vm_file->f_path, buf, PATH_MAX);
	deny = !IS_ERR(p) && ace_deny_name(p);
	kfree(buf);
	if (!deny)
		return 0;
	atomic64_inc(&g_ace_maps_drop);
	regs->pc = regs->regs[30];
	return 1;
}

static struct kprobe ace_maps_kp = {
	.symbol_name = "show_map_vma",
	.pre_handler = ace_maps_pre,
};

/* ---- D1: /proc/modules 行级过滤 ---- */

static noinline int ace_mods_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct module *mod = (struct module *)regs->regs[1];

	(void)kp;
	if (!mod || !mod->name)
		return 0;
	if (!ace_feat(ACE_F_MODS))
		return 0;
	if (!ace_cur_is_target())
		return 0;
	if (!ace_deny_name(mod->name))
		return 0;
	atomic64_inc(&g_ace_mod_drop);
	regs->pc = regs->regs[30];
	return 1;
}

static struct kprobe ace_mods_kp = {
	.symbol_name = "m_show",
	.pre_handler = ace_mods_pre,
};

/* ---- D2: /proc/kallsyms 符号名清除 ----
 * kallsyms_expand_symbol(off, result[, maxlen]) 的 result 是输出缓冲,
 * 在 kretprobe 里清空即可让该行显示为空名。
 * 权衡: 空名行比留 wind_* 安全, 逐行解析 kallsyms 的代价 ACE 通常不付。
 */

static noinline int ace_ksym_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	char *r;

	(void)ri;
	if (!READ_ONCE(g_ace_tgid))
		return 0;
	if (!ace_feat(ACE_F_KSYM))
		return 0;
	r = (char *)regs->regs[1];
	if (!r || !*r)
		return 0;
	if (!ace_deny_name(r))
		return 0;
	r[0] = '\0';
	atomic64_inc(&g_ace_ksym_drop);
	return 0;
}

static struct kretprobe ace_ksym_krp = {
	.kp.symbol_name = "kallsyms_expand_symbol",
	.handler = ace_ksym_ret,
	.maxactive = 32,
};

/* ---- C1: TracerPid 清零 ----
 * proc_pid_status 的 task 参数在 5.10~6.18 都是第 4 个 (regs[3])。
 * pre 清 task->ptrace, kretprobe 恢复; 保存到 per-CPU 避免并发串台。
 */

struct ace_tp_slot {
	struct task_struct *task;
	unsigned int ptrace;
};

static DEFINE_PER_CPU(struct ace_tp_slot, ace_tp_slot);

static noinline int ace_status_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct task_struct *task = (struct task_struct *)regs->regs[3];
	struct ace_tp_slot *s = this_cpu_ptr(&ace_tp_slot);

	(void)kp;
	s->task = NULL;
	if (!task)
		return 0;
	if (!ace_feat(ACE_F_TRACERP))
		return 0;
	if (!ace_is_target(task->tgid))
		return 0;
	if (!task->ptrace)
		return 0;
	s->task = task;
	s->ptrace = task->ptrace;
	task->ptrace = 0;
	atomic64_inc(&g_ace_tp_hide);
	return 0;
}

static noinline int ace_status_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ace_tp_slot *s = this_cpu_ptr(&ace_tp_slot);

	(void)ri;
	(void)regs;
	if (!s->task)
		return 0;
	s->task->ptrace = s->ptrace;
	s->task = NULL;
	return 0;
}

static struct kprobe ace_status_kp = {
	.symbol_name = "proc_pid_status",
	.pre_handler = ace_status_pre,
};

static struct kretprobe ace_status_krp = {
	.kp.symbol_name = "proc_pid_status",
	.handler = ace_status_ret,
	.maxactive = 32,
};


/* ---- E1/E2/E3: 路径类 syscall 过滤 ----
 * 三个入口都只做一件事: 把原来的用户态路径指针置 NULL,
 * 让 getname_flags 直接返回 -EFAULT。不改内核栈, 不改返回值寄存器,
 * 不依赖 prologue 是否已压栈, 所以不会崩。
 * 覆盖: open/openat -> do_sys_openat2; access/faccessat -> do_faccessat;
 *       stat/lstat/newfstatat -> vfs_statx
 */

static noinline bool ace_fs_hit(const struct pt_regs *regs)
{
	char *p;
	bool hit;

	if (!ace_feat(ACE_F_FS))
		return false;
	if (!ace_cur_is_target())
		return false;
	p = (char *)regs->regs[1];
	if (!p)
		return false;
	/* 只读前 128 字节, 避免用户给超长路径时白跑 */
	{
		char buf[128];
		long n = strncpy_from_user(buf, p, sizeof(buf) - 1);

		if (n <= 0)
			return false;
		buf[n] = '\0';
		hit = ace_fs_deny(buf);
	}
	return hit;
}

static noinline int ace_open_pre(struct kprobe *kp, struct pt_regs *regs)
{
	(void)kp;
	if (!ace_fs_hit(regs))
		return 0;

	regs->regs[1] = 0;
	atomic64_inc(&g_ace_fs_drop);
	return 0;
}

static noinline int ace_faccess_pre(struct kprobe *kp, struct pt_regs *regs)
{
	(void)kp;
	if (!ace_fs_hit(regs))
		return 0;

	regs->regs[1] = 0;
	atomic64_inc(&g_ace_fs_drop);
	return 0;
}

static noinline int ace_statx_pre(struct kprobe *kp, struct pt_regs *regs)
{
	(void)kp;
	if (!ace_fs_hit(regs))
		return 0;

	regs->regs[1] = 0;
	atomic64_inc(&g_ace_fs_drop);
	return 0;
}

static struct kprobe ace_open_kp = {
	.symbol_name = "do_sys_openat2",
	.pre_handler = ace_open_pre,
};

static struct kprobe ace_faccess_kp = {
	.symbol_name = "do_faccessat",
	.pre_handler = ace_faccess_pre,
};

static struct kprobe ace_statx_kp = {
	.symbol_name = "vfs_statx",
	.pre_handler = ace_statx_pre,
};


/* ---- A3: 线程名过滤 ----
 * /proc/<pid>/task/<tid>/comm 由 proc_task_name 输出, 命中则跳过整行。
 */

static const char *const ace_comm_deny[] = {
	"gum-js-loop", "gmain", "gdbus", "pool-frida", "frida",
	"gadget", "xposed", "substrate", "riru", "zygisk",
};

static bool ace_comm_hit(const char *comm)
{
	int i;

	if (!comm || !*comm)
		return false;
	for (i = 0; i < (int)ARRAY_SIZE(ace_comm_deny); ++i)
		if (strstr(comm, ace_comm_deny[i]))
			return true;
	return false;
}

static noinline int ace_comm_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct task_struct *task = (struct task_struct *)regs->regs[3];

	(void)kp;
	if (!task || !ace_cur_is_target())
		return 0;
	if (!ace_feat(ACE_F_COMM))
		return 0;
	if (!ace_comm_hit(task->comm))
		return 0;
	atomic64_inc(&g_ace_comm_drop);
	regs->pc = regs->regs[30];
	return 1;
}

static struct kprobe ace_comm_kp = {
	.symbol_name = "proc_task_name",
	.pre_handler = ace_comm_pre,
};

/* ---- G1: 调试端口阻断 ----
 * __sys_connect(fd, sockaddr __user *, addrlen) 第 2 参是用户态 sockaddr。
 * 只读前 8 字节 (family + port + 部分 addr), 命中黑名单端口就把指针置 NULL
 * 让 getname 等价的 copy_from_user 失败返回 -EFAULT, 不改栈不改返回值寄存器。
 */

static const unsigned short ace_deny_port[] = {
	27042, 27043, 23946, 5037, 8000, 8080, 31337,
};

static noinline int ace_connect_pre(struct kprobe *kp, struct pt_regs *regs)
{
	unsigned char buf[8];
	const void __user *sa = (const void __user *)regs->regs[1];
	unsigned short port;
	int i;

	(void)kp;
	if (!sa || !ace_cur_is_target())
		return 0;
	if (!ace_feat(ACE_F_PORT))
		return 0;
	if (copy_from_user(buf, sa, sizeof(buf)))
		return 0;
	if ((buf[0] | (buf[1] << 8)) != 2)
		return 0;
	port = (unsigned short)((buf[2] << 8) | buf[3]);
	for (i = 0; i < (int)ARRAY_SIZE(ace_deny_port); ++i) {
		if (port != ace_deny_port[i])
			continue;
		regs->regs[1] = 0;
		atomic64_inc(&g_ace_port_drop);
		return 0;
	}
	return 0;
}

static struct kprobe ace_connect_kp = {
	.symbol_name = "__sys_connect",
	.pre_handler = ace_connect_pre,
};


/* ---- 一加 inte 节点接管 ----
 * /sys/module/inte 是 built-in (不在 /proc/modules), 不能 rmmod,
 * 三个 /proc 节点由它在初始化时创建。做法:
 *   1) remove_proc_entry 摘掉原节点, 底层完整性检查的回调随之失联
 *   2) 用相同 mode 的 proc_create 重建同名节点, 挂本驱动的 proc_ops
 * 节点仍在(一加系统服务不会因为节点消失告警), 但读永远返回空、
 * 写永远假装成功, inte 的模块表/systbl 检查永不真正执行。
 * 需要"彻底消失"时把 arg 传 1, 只摘不重建。
 */

static ssize_t ace_inte_read(struct file *f, char __user *b, size_t n, loff_t *p)
{
	(void)f;
	(void)b;
	(void)n;
	(void)p;
	return 0;
}

static ssize_t ace_inte_write(struct file *f, const char __user *b, size_t n,
			      loff_t *p)
{
	(void)f;
	(void)b;
	(void)p;
	return (ssize_t)n;
}

static const struct proc_ops ace_inte_ops = {
	.proc_read = ace_inte_read,
	.proc_write = ace_inte_write,
};

static const struct {
	const char *name;
	umode_t mode;
} ace_inte_tbl[] = {
	{ "inte_ko",     0664 },
	{ "inte_systbl", 0664 },
	{ "inte_status", 0660 },
};

int ace_inte_takeover(int remove_only)
{
	int i, done = 0;

	for (i = 0; i < (int)ARRAY_SIZE(ace_inte_tbl); ++i) {
		struct proc_dir_entry *pde;

		remove_proc_entry(ace_inte_tbl[i].name, NULL);
		if (remove_only) {
			++done;
			continue;
		}
		pde = proc_create(ace_inte_tbl[i].name,
				  ace_inte_tbl[i].mode, NULL, &ace_inte_ops);
		if (!pde) {
			pr_warn(ACE_PFX "rebuild %s failed\n",
				ace_inte_tbl[i].name);
			continue;
		}
		++done;
	}
	pr_info(ACE_PFX "inte takeover done=%d remove_only=%d\n",
		done, remove_only);
	return done;
}

/* ---- 对外接口 ---- */




int ace_set_pid(uint32_t tgid)
{
	if (!tgid)
		return -EINVAL;
	WRITE_ONCE(g_ace_tgid, tgid);
	ACE_LOG("target tgid=%u\n", tgid);
	return 0;
}

int ace_set_features(u32 mask)
{
	WRITE_ONCE(g_ace_feat, mask & ACE_F_MASK);
	pr_info(ACE_PFX "features=0x%x\n", READ_ONCE(g_ace_feat));
	return 0;
}

u32 ace_get_features(void)
{
	return READ_ONCE(g_ace_feat);
}

int ace_set_verbose(int on)
{
	WRITE_ONCE(ace_verbose, on ? 1 : 0);
	return 0;
}

int ace_clear_pid(void)
{
	WRITE_ONCE(g_ace_tgid, 0);
	return 0;
}

int ace_get_stat(void *uout)
{
	ACE_STAT st;

	if (!uout)
		return -EINVAL;
	memset(&st, 0, sizeof(st));
	st.tgid = READ_ONCE(g_ace_tgid);
	st.features = READ_ONCE(g_ace_feat);
	st.maps_drop = (uint64_t)atomic64_read(&g_ace_maps_drop);
	st.mod_drop = (uint64_t)atomic64_read(&g_ace_mod_drop);
	st.ksym_drop = (uint64_t)atomic64_read(&g_ace_ksym_drop);
	st.tracerp_hide = (uint64_t)atomic64_read(&g_ace_tp_hide);
	st.fs_drop = (uint64_t)atomic64_read(&g_ace_fs_drop);
	st.comm_drop = (uint64_t)atomic64_read(&g_ace_comm_drop);
	st.port_drop = (uint64_t)atomic64_read(&g_ace_port_drop);
	memcpy(uout, &st, sizeof(st));
	return 0;
}

int ace_init(void)
{
	int r;

	r = register_kprobe(&ace_maps_kp);
	if (r)
		pr_warn(ACE_PFX "show_map_vma probe failed: %d\n", r);
	r = register_kprobe(&ace_mods_kp);
	if (r)
		pr_warn(ACE_PFX "m_show probe failed: %d\n", r);
	r = register_kretprobe(&ace_ksym_krp);
	if (r)
		pr_warn(ACE_PFX "kallsyms_expand_symbol probe failed: %d\n", r);
	r = register_kprobe(&ace_comm_kp);
	if (r)
		pr_warn(ACE_PFX "proc_task_name probe failed: %d\n", r);
	r = register_kprobe(&ace_connect_kp);
	if (r)
		pr_warn(ACE_PFX "__sys_connect probe failed: %d\n", r);
	r = register_kprobe(&ace_open_kp);
	if (r)
		pr_warn(ACE_PFX "do_sys_openat2 probe failed: %d\n", r);
	r = register_kprobe(&ace_faccess_kp);
	if (r)
		pr_warn(ACE_PFX "do_faccessat probe failed: %d\n", r);
	r = register_kprobe(&ace_statx_kp);
	if (r)
		pr_warn(ACE_PFX "vfs_statx probe failed: %d\n", r);
	r = register_kprobe(&ace_status_kp);
	if (r)
		pr_warn(ACE_PFX "proc_pid_status probe failed: %d\n", r);
	r = register_kretprobe(&ace_status_krp);
	if (r)
		pr_warn(ACE_PFX "proc_pid_status retprobe failed: %d\n", r);
	ACE_LOG("ready\n");
	return 0;
}

void ace_exit(void)
{
	unregister_kretprobe(&ace_status_krp);
	unregister_kprobe(&ace_connect_kp);
	unregister_kprobe(&ace_comm_kp);
	unregister_kprobe(&ace_statx_kp);
	unregister_kprobe(&ace_faccess_kp);
	unregister_kprobe(&ace_open_kp);
	unregister_kprobe(&ace_status_kp);
	unregister_kretprobe(&ace_ksym_krp);
	unregister_kprobe(&ace_mods_kp);
	unregister_kprobe(&ace_maps_kp);
	WRITE_ONCE(g_ace_tgid, 0);
	ACE_LOG("gone\n");
}
