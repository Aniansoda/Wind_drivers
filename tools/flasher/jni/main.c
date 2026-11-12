/* wind 驱动刷入器 — AIDE / ndk-build 编译为 arm64 可执行文件
 *
 * 和 keep 一样走 Android.mk, 但用 BUILD_EXECUTABLE:
 *   AIDE 里直接 Build -> libs/arm64-v8a/wind_flash
 *
 * 菜单一次决定驱动加载参数, 加载后内核不再暴露运行时开关。
 * 配置持久化到 /data/local/tmp/wind.cfg, 下次可复用。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "loader.h"
#include "payload.h"

#define CFG_PATH       "/data/local/tmp/wind.cfg"
#define PROCNAME_PATH  "/data/local/tmp/wind_proc_name"
#define KO_DEFAULT     "/data/local/tmp/wind.ko"
#define MOD_NAME       "wind"

/* 终端配色 */
#define K_RST "\033[0m"
#define K_BLD "\033[1m"
#define K_DIM "\033[2m"
#define K_CYA "\033[36m"
#define K_GRN "\033[32m"
#define K_YEL "\033[33m"
#define K_RED "\033[31m"
#define K_MAG "\033[35m"

#define TAG K_CYA "[+]" K_RST

static void banner(void)
{
	fputs(K_CYA K_BLD
	      "\n"
	      "██╗    ██╗██╗███╗   ██╗██████╗ \n"
	      "██║    ██║██║████╗  ██║██╔══██╗\n"
	      "██║ █╗ ██║██║██╔██╗ ██║██║  ██║\n"
	      "██║███╗██║██║██║╚██╗██║██║  ██║\n"
	      "╚███╔███╔╝██║██║ ╚████║██████╔╝\n"
	      " ╚══╝╚══╝ ╚═╝╚═╝  ╚═══╝╚═════╝ \n"
	      K_RST, stdout);
	fputs(TAG " " K_MAG "[Wind-channel] 驱动更新频道@Winddrivers" K_RST "\n\n", stdout);
	fflush(stdout);
}

/* 统一输出: 每行都以 [+] 开头, 颜色区分轻重 */
static void klog(const char *color, const char *fmt, ...)
{
	va_list ap;

	fputs(TAG " ", stdout);
	fputs(color, stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fputs(K_RST "\n", stdout);
	fflush(stdout);
}

#define k_ok(...)   klog(K_GRN, __VA_ARGS__)
#define k_info(...) klog(K_CYA, __VA_ARGS__)
#define k_warn(...) klog(K_YEL, __VA_ARGS__)
#define k_err(...)  klog(K_RED, __VA_ARGS__)
#define k_dim(...)  klog(K_DIM, __VA_ARGS__)

struct cfg {
	int  hide_driver;
	int  hide_pid;
	int  hide_gpu;
	int  oppo_backdoor;
	int  force_load;
	int  gyro;
	char proc_name[64];
};

static void cfg_defaults(struct cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->hide_driver   = 1;
	c->hide_pid      = 1;
	c->hide_gpu      = 1;
	c->oppo_backdoor = 1;
	c->force_load    = 1;
	c->gyro          = 0;   /* 陀螺仪通道已撤, 自瞄走断点写回 */
	strcpy(c->proc_name, "");
}

/* 读一行, 去掉行尾空白。返回 0 表示读到内容 */
static int read_line(char *buf, size_t cap)
{
	size_t n;

	if (!fgets(buf, (int)cap, stdin))
		return -1;
	n = strlen(buf);
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
		     buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = 0;
	return 0;
}

static int ask01(const char *title, int def)
{
	char line[32];
	int v;

	printf(TAG " " K_YEL "%s" K_RST " " K_DIM "[0-关/1-开] 默认:%d" K_RST "\n", title, def);
	printf(TAG " 请选择: ");
	fflush(stdout);
	if (read_line(line, sizeof(line)) < 0)
		return def;
	if (!line[0])
		return def;
	v = atoi(line);
	return (v == 0 || v == 1) ? v : def;
}

/* 常见系统进程名, 让用户有得选 */
static const char *kProcNames[] = {
	"",                 /* 1  不伪装 */
	"nvpmgr",           /* 2 */
	"installer",        /* 3 */
	"dumpsys",          /* 4 */
	"media.codec",      /* 5 */
	"android.hardware", /* 6 */
};
#define PROC_NAME_CNT ((int)(sizeof(kProcNames) / sizeof(kProcNames[0])))

static void choose_proc_name(struct cfg *c)
{
	char line[64];
	int i, v;

	printf(TAG " " K_YEL "进程名伪装" K_RST "\n");
	for (i = 0; i < PROC_NAME_CNT; i++)
		printf(TAG "  %d  %s\n", i + 1,
		       kProcNames[i][0] ? kProcNames[i] : "不伪装");
	printf(TAG "  %d  自定义\n", PROC_NAME_CNT + 1);
	printf(TAG " 请选择 [1-%d] 默认:1: ", PROC_NAME_CNT + 1);
	fflush(stdout);

	if (read_line(line, sizeof(line)) < 0 || !line[0]) {
		c->proc_name[0] = 0;
		return;
	}
	v = atoi(line);
	if (v == PROC_NAME_CNT + 1) {
		printf(TAG " 输入进程名: ");
		fflush(stdout);
		if (read_line(c->proc_name, sizeof(c->proc_name)) < 0)
			c->proc_name[0] = 0;
		return;
	}
	if (v >= 1 && v <= PROC_NAME_CNT)
		snprintf(c->proc_name, sizeof(c->proc_name), "%s", kProcNames[v - 1]);
	else
		c->proc_name[0] = 0;
}

static void load_cfg(struct cfg *c)
{
	FILE *f = fopen(CFG_PATH, "r");
	char line[256];

	if (!f)
		return;
	while (fgets(line, (int)sizeof(line), f)) {
		char *eq = strchr(line, '=');
		char *v;
		size_t n;

		if (!eq)
			continue;
		*eq = 0;
		v = eq + 1;
		n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == '\r'))
			v[--n] = 0;

		if      (!strcmp(line, "hide_driver"))   c->hide_driver   = atoi(v) != 0;
		else if (!strcmp(line, "hide_pid"))      c->hide_pid      = atoi(v) != 0;
		else if (!strcmp(line, "hide_gpu"))      c->hide_gpu      = atoi(v) != 0;
		else if (!strcmp(line, "oppo_backdoor")) c->oppo_backdoor = atoi(v) != 0;
		else if (!strcmp(line, "force_load"))    c->force_load    = atoi(v) != 0;
		else if (!strcmp(line, "gyro"))          c->gyro          = atoi(v) != 0;
		else if (!strcmp(line, "proc_name"))     snprintf(c->proc_name, sizeof(c->proc_name), "%s", v);
	}
	fclose(f);
}

static void save_cfg(const struct cfg *c)
{
	FILE *f = fopen(CFG_PATH, "w");

	if (!f) {
		k_err("写 %s 失败: %s", CFG_PATH, strerror(errno));
		return;
	}
	fprintf(f, "hide_driver=%d\n",   c->hide_driver);
	fprintf(f, "hide_pid=%d\n",      c->hide_pid);
	fprintf(f, "hide_gpu=%d\n",      c->hide_gpu);
	fprintf(f, "oppo_backdoor=%d\n", c->oppo_backdoor);
	fprintf(f, "force_load=%d\n",    c->force_load);
	fprintf(f, "gyro=%d\n",          c->gyro);
	fprintf(f, "proc_name=%s\n",     c->proc_name);
	fclose(f);
	k_ok("配置已存 %s", CFG_PATH);
}

/* 跑一条命令, 返回退出码; 失败返回 -1 */
static int run(char *const argv[])
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid < 0) {
		k_err("fork 失败: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &st, 0) < 0)
		return -1;
	if (!WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static int module_loaded(void)
{
	FILE *p;
	char line[256];
	int found = 0;

	/* lsmod 不一定有, 用 /proc/modules 更稳 */
	p = fopen("/proc/modules", "r");
	if (!p)
		return 0;
	while (fgets(line, (int)sizeof(line), p)) {
		if (!strncmp(line, MOD_NAME " ", sizeof(MOD_NAME))) {
			found = 1;
			break;
		}
	}
	fclose(p);
	return found;
}

static void write_proc_name(const char *name)
{
	FILE *f;

	if (!name || !name[0]) {
		remove(PROCNAME_PATH);
		k_dim("进程名伪装: 关闭");
		return;
	}
	f = fopen(PROCNAME_PATH, "w");
	if (!f) {
		k_err("写 %s 失败: %s", PROCNAME_PATH, strerror(errno));
		return;
	}
	fputs(name, f);
	fclose(f);
	chmod(PROCNAME_PATH, 0644);
	k_ok("进程名伪装: %s", name);
}

static void dump_dmesg(void)
{
	char *argv[] = { "sh", "-c",
		"dmesg | grep -E 'wind-cfg|wind-hide|ace|gyro-sock|kgsl|version magic|invalid module|Unknown symbol' | tail -16",
		NULL };

	printf(TAG " " K_DIM "内核日志:" K_RST "\n");
	run(argv);
}

int main(int argc, char **argv)
{
	const char *ko = (argc > 1) ? argv[1] : KO_DEFAULT;
	struct cfg c;
	char line[64];

	banner();

	/* 非 root 自举: 用 su -c 重新执行自己 */
	if (getuid() != 0) {
		char self[512];
		char cmd[1024];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

		if (n <= 0) {
			k_err("取自身路径失败, 请用 su 执行");
			return 1;
		}
		self[n] = 0;
		snprintf(cmd, sizeof(cmd), "%s %s", self, ko);
		k_warn("需要 root, 尝试 su");
		execlp("su", "su", "-c", cmd, (char *)NULL);
		k_err("su 不可用");
		return 1;
	}

	/* 没给 ko 路径: 先从内嵌载荷里解一个出来。
	 * 表是空的 (仓库里的占位版) 就退回读文件。 */
	if (argc <= 1 && wind_payload_cnt > 0) {
		int prc = wind_payload_extract(ko);

		if (prc != WPE_OK && prc != WPE_NO_PAYLOAD)
			k_warn("内嵌载荷没解出来 rc=%d, 试现成文件", prc);
	}

	if (access(ko, R_OK) != 0) {
		k_err("找不到 %s (%s)", ko, strerror(errno));
		return 1;
	}
	k_info("目标: %s", ko);

	cfg_defaults(&c);

	/* 上次配置 */
	if (access(CFG_PATH, R_OK) == 0) {
		struct cfg old;

		cfg_defaults(&old);
		load_cfg(&old);
		k_dim("上次: 驱动隐藏=%d pid隐藏=%d GPU隐藏=%d oppo后门=%d 强制加载=%d 陀螺仪=%d 进程名=%s",
		      old.hide_driver, old.hide_pid, old.hide_gpu,
		      old.oppo_backdoor, old.force_load, old.gyro,
		      old.proc_name[0] ? old.proc_name : "无");
		printf(TAG " 使用上次配置 " K_DIM "[1-是/2-否]" K_RST " 请选择: ");
		fflush(stdout);
		if (read_line(line, sizeof(line)) == 0 && line[0] != '2') {
			c = old;
			goto do_load;
		}
	}

	/* 是否加载 */
	printf(TAG " " K_YEL "是否加载 Wind 驱动" K_RST " " K_DIM "[1-是/2-否]" K_RST " 请选择: ");
	fflush(stdout);
	if (read_line(line, sizeof(line)) == 0 && line[0] == '2') {
		k_dim("已取消");
		return 0;
	}

	c.hide_driver   = ask01("隐藏驱动", 1);
	c.oppo_backdoor = ask01("屏蔽一加后门", 1);
	c.force_load    = ask01("强制加载", 1);

	printf(TAG " " K_DIM "以下三项与游戏进程相关, 可单独关闭" K_RST "\n");

	/* 陀螺仪通道撤了: 它占 arm64 仅有的 4 个硬件断点寄存器,
	 * 和自瞄要用的 WRP 观察点互斥, 开着就自瞄无效或整机重启。
	 * 这里不再问, 固定关。 */
	c.gyro     = 0;
	c.hide_pid = ask01("pid 隐藏", 1);
	c.hide_gpu = ask01("GPU 隐藏", 1);

	/* 进程名伪装 */
	choose_proc_name(&c);

	save_cfg(&c);

do_load:
	/* 卸载旧的 */
	if (module_loaded()) {
		char *rm[] = { "rmmod", MOD_NAME, NULL };
		k_info("卸载已加载的 %s", MOD_NAME);
		if (run(rm) != 0)
			k_warn("rmmod 失败, 可能仍在使用, 继续加载");
	}

	write_proc_name(c.proc_name);

	/* 加载 */
	{
		char a_hd[32], a_hp[32], a_hg[32], a_ob[32], a_fl[32], a_gy[32];
		char a_kp[320];
		char *ins[10];
		int rc;

		snprintf(a_hd, sizeof(a_hd), "hide_driver=%d",   c.hide_driver);
		snprintf(a_hp, sizeof(a_hp), "hide_pid=%d",      c.hide_pid);
		snprintf(a_hg, sizeof(a_hg), "hide_gpu=%d",      c.hide_gpu);
		snprintf(a_ob, sizeof(a_ob), "oppo_backdoor=%d", c.oppo_backdoor);
		snprintf(a_fl, sizeof(a_fl), "force_load=%d",    c.force_load);
		snprintf(a_gy, sizeof(a_gy), "gyro=%d",          c.gyro);
		snprintf(a_kp, sizeof(a_kp), "ko_path=%s",       ko);

		ins[0]  = "insmod";
		ins[1]  = (char *)ko;
		ins[2]  = a_hd;
		ins[3]  = a_hp;
		ins[4]  = a_hg;
		ins[5]  = a_ob;
		ins[6]  = a_fl;
		ins[7]  = a_gy;
		ins[8]  = a_kp;
		ins[9]  = NULL;

		k_info("insmod 中 ...");
		rc = run(ins);
		if (rc != 0) {
			/* vermagic 真改不动时, 内核只在 CONFIG_MODULE_FORCE_LOAD=y
			 * 的情况下吃 -f。试一次, 不行再报。 */
			char *insf[11];
			int i;

			k_warn("insmod 失败 rc=%d, 试 -f", rc);
			insf[0] = "insmod";
			insf[1] = "-f";
			for (i = 1; i <= 8; i++)
				insf[i + 1] = ins[i];
			insf[10] = NULL;
			rc = run(insf);
			if (rc != 0) {
				k_err("insmod -f 也失败 rc=%d", rc);
				dump_dmesg();
				return rc;
			}
		}
	}

	k_ok("insmod 成功");
	sleep(1);

	if (module_loaded())
		k_ok("模块在线");
	else
		k_warn("/proc/modules 里没有 (hide_driver 生效时正常)");

	dump_dmesg();

	k_dim("目标 pid 由 Keep 启动时读到, 走 OP_BIND 交给驱动");
	k_dim("验证: dmesg | grep wind-cfg | tail -5");
	return 0;
}
