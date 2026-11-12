/* 刷入期配置: 默认全开, 加载后不再改, 用户态不提供任何开关
 *
 *   insmod wind.ko hide_driver=1 hide_pid=1 hide_gpu=1 \
 *                  oppo_backdoor=1 force_load=1 gyro=1
 *
 * 目标进程不在这里识别。用户态 (Keep 的 DrawInit) 自己找到游戏 pid,
 * 走 OP_BIND 传进来, 内核在 windcfg_bind_pid() 里做绑定和隐藏。
 * 驱动侧不认识包名, 也不扫 task 列表 —— 谁传进来就绑谁。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "windcfg.h"
#include "ace.h"
#include "kgsl.h"

/* hide.c 没有头文件, 按外部声明接 */
extern int hide_file(const char *path);
extern int hide_process(pid_t pid);

/* ---- insmod 参数, 默认全开 ---- */
static bool cfg_hide_driver = true;
static bool cfg_hide_pid = true;
static bool cfg_hide_gpu = true;
static bool cfg_oppo_backdoor = true;
static bool cfg_force_load = true;
static bool cfg_gyro = true;

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
module_param_named(ko_path, cfg_ko_path, charp, 0444);
module_param_named(oppo_procs, cfg_oppo_procs, charp, 0444);

static bool  g_gpu_hooked;
static pid_t g_bound_tgid;

bool windcfg_gyro_on(void)
{
	return cfg_gyro;
}

/* 用户态传 pid 进来之后做的事。
 * 重复传同一个 pid 直接返回, 免得每次 attach 重装一次 hook。 */
void windcfg_bind_pid(pid_t tgid)
{
	int ret;

	if (tgid <= 1)
		return;
	if (tgid == g_bound_tgid)
		return;
	g_bound_tgid = tgid;

	ret = ace_set_pid((u32)tgid);
	pr_info("[wind-cfg] bind tgid=%d ace=%d\n", tgid, ret);

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
		"oppo_backdoor=%d force_load=%d gyro=%d\n",
		(int)cfg_hide_driver, (int)cfg_hide_pid, (int)cfg_hide_gpu,
		(int)cfg_oppo_backdoor, (int)cfg_force_load, (int)cfg_gyro);

	if (cfg_hide_driver && cfg_ko_path && cfg_ko_path[0]) {
		int ret = hide_file(cfg_ko_path);

		pr_info("[wind-cfg] hide_file(%s)=%d\n", cfg_ko_path, ret);
	}

	windcfg_hide_oppo_procs();
	return 0;
}

void windcfg_exit(void)
{
	g_bound_tgid = 0;
}
