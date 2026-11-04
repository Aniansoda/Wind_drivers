/*
 * kgsl.c — GPU KGSL 隐藏 (kretprobe, CFI 安全)
 *
 * 目标: 游戏通过 kgsl (Adreno) ioctl 查询 GPU 进程列表/使用率,
 * 检测到异常进程占用 GPU (外挂内存扫描) 即判定作弊。
 * 通过 kretprobe 钩 kgsl_ioctl, 当「被隐藏进程」发起「读方向(查询类)」ioctl
 * 时, 篡改返回值为 -EACCES, 使查询失败 → 进程在 GPU 列表中不可见。
 *
 * 为什么 kretprobe:
 *   kprobe pre_handler 无法跳过原函数; kretprobe 在返回时触发, 可改返回值。
 *   且不修改代码段, 天然绕过 CFI。
 *
 * 依赖 (未验证, 需按目标核对):
 *   1. kgsl_ioctl 符号可见性 (vendor 模块, 需 CONFIG_KALLSYMS_ALL)。
 *   2. kgsl_ioctl 签名: long kgsl_ioctl(struct file *, unsigned int cmd,
 *      unsigned long arg)。arm64: x0=file, x1=cmd, x2=arg, 返回值 x0。
 *   3. 只拦 _IOC_READ 方向是保守策略: 放行写方向(正常 GPU 提交), 拦查询类。
 *      若需精确到 KGSL_IOC_QUERY 等具体命令, 需 vendor msm_kgsl.h 的 cmd 常量。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <asm/ioctl.h>
#include <asm/ptrace.h>

#define MAX_HIDDEN_PIDS 64

static pid_t hidden_pids[MAX_HIDDEN_PIDS];
static int hidden_count = 0;

int kgsl_hide_pid(pid_t pid)
{
	int i;

	if (hidden_count >= MAX_HIDDEN_PIDS)
		return -ENOSPC;

	for (i = 0; i < hidden_count; i++)
		if (hidden_pids[i] == pid)
			return 0;

	hidden_pids[hidden_count++] = pid;
	return 0;
}
EXPORT_SYMBOL(kgsl_hide_pid);

bool kgsl_is_hidden(pid_t pid)
{
	int i;

	for (i = 0; i < hidden_count; i++)
		if (hidden_pids[i] == pid)
			return true;
	return false;
}
EXPORT_SYMBOL(kgsl_is_hidden);

/* 每次调用 kretprobe 的私有数据 */
struct kgsl_ctx
{
	bool hidden;
};

static int kgsl_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct kgsl_ctx *ctx = (struct kgsl_ctx *)ri->data;
	unsigned int cmd;

	ctx->hidden = false;
	if (!kgsl_is_hidden(current->pid))
		return 0;

#if defined(CONFIG_ARM64)
	cmd = (unsigned int)regs->regs[1]; /* x1 = cmd */
#elif defined(CONFIG_X86_64)
	cmd = (unsigned int)regs->rsi;
#else
	return 0;
#endif

	/* 只拦读方向(查询类) ioctl, 放行写方向(正常 GPU 提交) */
	if (_IOC_DIR(cmd) & _IOC_READ)
		ctx->hidden = true;
	return 0;
}

static int kgsl_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct kgsl_ctx *ctx = (struct kgsl_ctx *)ri->data;

	if (ctx->hidden)
	{
#if defined(CONFIG_ARM64)
		regs->regs[0] = -EACCES; /* 篡改返回值 */
#elif defined(CONFIG_X86_64)
		regs->rax = -EACCES;
#endif
	}
	return 0;
}

static struct kretprobe krp_kgsl = {
	.kp.symbol_name = "kgsl_ioctl",
	.entry_handler = kgsl_entry_handler,
	.handler = kgsl_ret_handler,
	.data_size = sizeof(struct kgsl_ctx),
	.maxactive = 64,
};

int kgsl_hook_install(void)
{
	int ret;

	if (krp_kgsl.kp.addr)
		return 0;

	ret = register_kretprobe(&krp_kgsl);
	if (ret < 0)
	{
		/* 符号不可见: 需 CONFIG_KALLSYMS_ALL 或用 kallsyms_lookup_name */
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL(kgsl_hook_install);

void kgsl_hook_remove(void)
{
	if (krp_kgsl.kp.addr)
		unregister_kretprobe(&krp_kgsl);
	hidden_count = 0;
}
EXPORT_SYMBOL(kgsl_hook_remove);
