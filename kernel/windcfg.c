/* 刷入期配置: 默认全开, 加载后不再改, 用户态不提供任何开关
 *
 * 设计前提: 反 ACE 的每一项都是「装上就该一直开着」的东西, 让用户在 UI 里
 * 逐个开关只会制造「关了忘开」的漏点。所以全部收敛到 insmod 参数:
 *
 *   insmod wind.ko hide_driver=1 hide_pid=1 hide_gpu=1 oppo_backdoor=1 \
 *                  force_load=1 gyro=1 target_comm=com.tencent.tmgp.pubgmhd
 *
 * 目标进程由内核自己扫 task 列表绑定, 不经过任何 ioctl:
 *   insmod 时游戏通常还没起来, 所以用 delayed_work 重试, 找到即绑。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/limits.h>

#include "windcfg.h"
#include "ace.h"
#include "kgsl.h"

/* hide.c 里没有头文件, 这里按外部声明接 */
extern int hide_file(const char *path);
extern int hide_process(pid_t pid);

/* ---- insmod 参数, 默认全开 ---- */
static bool cfg_hide_driver = true;
static bool cfg_hide_pid = true;
static bool cfg_hide_gpu = true;
static bool cfg_oppo_backdoor = true;
static bool cfg_force_load = true;
static bool cfg_gyro = true;

static char *cfg_target_comm = "com.tencent.tmgp.pubgmhd";
static char *cfg_ko_path = "/data/local/tmp/wind.ko";
/* ColorOS/OxygenOS 暴露给 ACE 的 proc 后门, 逗号分隔; 换机型可覆盖 */
static char *cfg_oppo_procs = "oplus_mem,oplus_nvme,oplus_health,oplus_vooc,"
			      "hmbird_pid,hmbird_tgid,hbp_pid,onepower";

module_param_named(hide_driver, cfg_hide_driver, bool, 0444);
module_param_named(hide_pid, cfg_hide_pid, bool, 0444);
module_param_named(hide_gpu, cfg_hide_gpu, bool, 0444);
module_param_named(oppo_backdoor, cfg_oppo_backdoor, bool, 0444);
module_param_named(force_load, cfg_force_load, bool, 0444);
module_param_named(gyro, cfg_gyro, bool, 0444);
module_param_named(target_comm, cfg_target_comm, charp, 0444);
module_param_named(ko_path, cfg_ko_path, charp, 0444);
module_param_named(oppo_procs, cfg_oppo_procs, charp, 0444);

/* ---- 自动绑定 ---- */
#define WINCFG_RETRY_MAX 60      /* 5s x 60 = 5 分钟 */
#define WINCFG_RETRY_MS  5000

static struct delayed_work g_bind_work;
static pid_t g_bound_tgid;
static int  g_retry_left;
static bool g_gpu_hooked;

pid_t windcfg_target_tgid(void)
{
	return READ_ONCE(g_bound_tgid);
}

/* comm 只有 16 字节, 比 target_comm 短, 按短的那边截 */
static bool windcfg_comm_match(const char *comm)
{
	size_t n;

	if (!comm || !comm[0])
		return false;
	n = strnlen(cfg_target_comm, TASK_COMM_LEN - 1);
	if (!n)
		return false;
	return strncmp(comm, cfg_target_comm, n) == 0;
}

static pid_t windcfg_scan(void)
{
	struct task_struct *p;
	pid_t found = 0;

	rcu_read_lock();
	for_each_process(p) {
		if (windcfg_comm_match(p->comm)) {
			found = p->tgid;
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

static void windcfg_bind_work_fn(struct work_struct *w)
{
	pid_t tgid;
	int ret;

	tgid = windcfg_scan();
	if (!tgid) {
		if (--g_retry_left > 0) {
			schedule_delayed_work(&g_bind_work,
					      msecs_to_jiffies(WINCFG_RETRY_MS));
		} else {
			pr_info("[wind-cfg] 未找到 %s, 停止自动绑定 (用户态仍可手动绑)\n",
				cfg_target_comm);
		}
		return;
	}

	if (tgid == g_bound_tgid)
		return;
	g_bound_tgid = tgid;

	ret = ace_set_pid((u32)tgid);
	pr_info("[wind-cfg] 自动绑定 tgid=%d (comm=%s) ace=%d\n",
		tgid, cfg_target_comm, ret);

	if (cfg_hide_pid) {
		ret = hide_process(tgid);
		pr_info("[wind-cfg] hide_process(%d)=%d\n", tgid, ret);
	}

	if (cfg_hide_gpu) {
		if (!g_gpu_hooked) {
			/* 同一个 hook 只装一次, 失败也不重复装 */
			ret = kgsl_hook_install();
			g_gpu_hooked = (ret == 0);
			pr_info("[wind-cfg] kgsl_hook_install=%d\n", ret);
		}
		if (g_gpu_hooked) {
			ret = kgsl_hide_pid(tgid);
			pr_info("[wind-cfg] kgsl_hide_pid(%d)=%d\n", tgid, ret);
		}
	}

	/* 绑上之后继续盯着: 游戏重启会换 tgid, 这里每 5s 复查一次 */
	g_retry_left = INT_MAX;
	schedule_delayed_work(&g_bind_work, msecs_to_jiffies(WINCFG_RETRY_MS));
}

/* ---- 一加/Oppo proc 后门: 从 getdents 里抹掉 ---- */
static void windcfg_hide_oppo_procs(void)
{
	char *buf, *p, *tok;
	int cnt = 0;

	if (!cfg_oppo_backdoor || !cfg_oppo_procs || !cfg_oppo_procs[0])
		return;

	buf = kstrdup(cfg_oppo_procs, GFP_KERNEL);
	if (!buf) {
		pr_warn("[wind-cfg] oppo proc 名单分配失败\n");
		return;
	}

	p = buf;
	while ((tok = strsep(&p, ",")) != NULL) {
		char path[128];
		int n;

		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (!tok[0])
			continue;
		n = snprintf(path, sizeof(path), "/proc/%s", tok);
		if (n <= 0 || n >= (int)sizeof(path))
			continue;
		if (hide_file(path) == 0)
			cnt++;
	}
	kfree(buf);
	pr_info("[wind-cfg] oppo proc 后门已抹掉 %d 项\n", cnt);
}

int windcfg_init(void)
{
	pr_info("[wind-cfg] hide_driver=%d hide_pid=%d hide_gpu=%d "
		"oppo_backdoor=%d force_load=%d gyro=%d target=%s\n",
		(int)cfg_hide_driver, (int)cfg_hide_pid, (int)cfg_hide_gpu,
		(int)cfg_oppo_backdoor, (int)cfg_force_load, (int)cfg_gyro,
		cfg_target_comm);

	if (cfg_hide_driver && cfg_ko_path && cfg_ko_path[0]) {
		int ret = hide_file(cfg_ko_path);

		pr_info("[wind-cfg] hide_file(%s)=%d\n", cfg_ko_path, ret);
	}

	windcfg_hide_oppo_procs();

	g_retry_left = WINCFG_RETRY_MAX;
	INIT_DELAYED_WORK(&g_bind_work, windcfg_bind_work_fn);
	schedule_delayed_work(&g_bind_work, msecs_to_jiffies(1000));
	return 0;
}

void windcfg_exit(void)
{
	cancel_delayed_work_sync(&g_bind_work);
	g_bound_tgid = 0;
}

/* 供 entry.c / 其它模块查参数 */
bool windcfg_gyro_on(void)
{
	return cfg_gyro;
}
