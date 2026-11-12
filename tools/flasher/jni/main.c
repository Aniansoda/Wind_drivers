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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#define CFG_PATH       "/data/local/tmp/wind.cfg"
#define PROCNAME_PATH  "/data/local/tmp/wind_proc_name"
#define KO_DEFAULT     "/data/local/tmp/wind.ko"
#define MOD_NAME       "wind"

struct cfg {
	int  hide_driver;
	int  hide_pid;
	int  hide_gpu;
	int  oppo_backdoor;
	int  force_load;
	int  gyro;
	char proc_name[64];
	char target_comm[64];
};

static void cfg_defaults(struct cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->hide_driver   = 1;
	c->hide_pid      = 1;
	c->hide_gpu      = 1;
	c->oppo_backdoor = 1;
	c->force_load    = 1;
	c->gyro          = 1;
	strcpy(c->proc_name, "");
	strcpy(c->target_comm, "com.tencent.tmgp.pubgmhd");
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

	printf("\n %s [0-关/1-开] [默认:%d]\n 请选择: ", title, def);
	fflush(stdout);
	if (read_line(line, sizeof(line)) < 0)
		return def;
	if (!line[0])
		return def;
	v = atoi(line);
	return (v == 0 || v == 1) ? v : def;
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
		else if (!strcmp(line, "target_comm"))   snprintf(c->target_comm, sizeof(c->target_comm), "%s", v);
	}
	fclose(f);
}

static void save_cfg(const struct cfg *c)
{
	FILE *f = fopen(CFG_PATH, "w");

	if (!f) {
		printf("[!] 写 %s 失败: %s\n", CFG_PATH, strerror(errno));
		return;
	}
	fprintf(f, "hide_driver=%d\n",   c->hide_driver);
	fprintf(f, "hide_pid=%d\n",      c->hide_pid);
	fprintf(f, "hide_gpu=%d\n",      c->hide_gpu);
	fprintf(f, "oppo_backdoor=%d\n", c->oppo_backdoor);
	fprintf(f, "force_load=%d\n",    c->force_load);
	fprintf(f, "gyro=%d\n",          c->gyro);
	fprintf(f, "proc_name=%s\n",     c->proc_name);
	fprintf(f, "target_comm=%s\n",   c->target_comm);
	fclose(f);
	printf("\n[+] 配置已存 %s\n", CFG_PATH);
}

/* 跑一条命令, 返回退出码; 失败返回 -1 */
static int run(char *const argv[])
{
	pid_t pid;
	int st;

	pid = fork();
	if (pid < 0) {
		printf("[x] fork 失败: %s\n", strerror(errno));
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
	char *argv[] = { "lsmod", NULL };
	FILE *p;
	char line[256];
	int found = 0;

	/* lsmod 不一定有, 用 /proc/modules 更稳 */
	p = fopen("/proc/modules", "r");
	if (!p) {
		(void)argv;
		return 0;
	}
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
		printf("[*] 进程名伪装: 关闭\n");
		return;
	}
	f = fopen(PROCNAME_PATH, "w");
	if (!f) {
		printf("[!] 写 %s 失败: %s\n", PROCNAME_PATH, strerror(errno));
		return;
	}
	fputs(name, f);
	fclose(f);
	chmod(PROCNAME_PATH, 0644);
	printf("[*] 进程名伪装: %s\n", name);
}

static void dump_dmesg(void)
{
	char *argv[] = { "sh", "-c",
		"dmesg | grep -E 'wind-cfg|wind-hide|ace|gyro-sock|kgsl' | tail -12",
		NULL };

	printf("\n-------------------\n内核日志:\n");
	run(argv);
}

int main(int argc, char **argv)
{
	const char *ko = (argc > 1) ? argv[1] : KO_DEFAULT;
	struct cfg c;
	char line[64];

	/* 非 root 自举: 用 su -c 重新执行自己 */
	if (getuid() != 0) {
		char self[512];
		char cmd[1024];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

		if (n <= 0) {
			printf("[x] 取自身路径失败, 请用 su 执行\n");
			return 1;
		}
		self[n] = 0;
		snprintf(cmd, sizeof(cmd), "%s %s", self, ko);
		printf("[!] 需要 root, 尝试 su\n");
		fflush(stdout);
		execlp("su", "su", "-c", cmd, (char *)NULL);
		printf("[x] su 不可用\n");
		return 1;
	}

	if (access(ko, R_OK) != 0) {
		printf("[x] 找不到 %s (%s)\n", ko, strerror(errno));
		return 1;
	}

	printf("wind 驱动刷入器\n目标: %s\n-------------------\n", ko);

	cfg_defaults(&c);

	/* 上次配置 */
	if (access(CFG_PATH, R_OK) == 0) {
		struct cfg old;

		cfg_defaults(&old);
		load_cfg(&old);
		printf("检测到上次配置：\n");
		printf("  隐藏驱动=%d  pid隐藏=%d  GPU隐藏=%d\n",
		       old.hide_driver, old.hide_pid, old.hide_gpu);
		printf("  oppo后门=%d  强制加载=%d  陀螺仪=%d\n",
		       old.oppo_backdoor, old.force_load, old.gyro);
		printf("  进程名=%s  目标=%s\n",
		       old.proc_name[0] ? old.proc_name : "无", old.target_comm);
		printf("\n 使用上次配置 [1-是/2-否]\n 请选择: ");
		fflush(stdout);
		if (read_line(line, sizeof(line)) == 0 && line[0] != '2') {
			c = old;
			goto do_load;
		}
	}

	/* 是否加载 */
	printf("\n 是否加载 Wind 驱动 [1-是/2-否]\n 请选择: ");
	fflush(stdout);
	if (read_line(line, sizeof(line)) == 0 && line[0] == '2') {
		printf("已取消\n");
		return 0;
	}

	c.hide_driver   = ask01("隐藏 Wind 驱动", 1);
	c.oppo_backdoor = ask01("屏蔽一加/Oppo proc 后门", 1);
	c.force_load    = ask01("强制加载", 1);

	printf("\n ---- 以下三项与游戏进程相关, 可单独关闭 ----\n");

	c.gyro     = ask01("陀螺仪控制", 1);
	c.hide_pid = ask01("pid 隐藏", 1);
	c.hide_gpu = ask01("GPU 隐藏", 1);

	/* 进程名伪装 */
	printf("\n 进程名伪装（回车跳过）\n 输入: ");
	fflush(stdout);
	if (read_line(c.proc_name, sizeof(c.proc_name)) < 0)
		c.proc_name[0] = 0;

	/* 目标进程 */
	printf("\n 目标进程 comm（回车用 com.tencent.tmgp.pubgmhd）\n 输入: ");
	fflush(stdout);
	{
		char t[64];
		if (read_line(t, sizeof(t)) == 0 && t[0])
			snprintf(c.target_comm, sizeof(c.target_comm), "%s", t);
	}

	save_cfg(&c);

do_load:
	if (!c.target_comm[0])
		snprintf(c.target_comm, sizeof(c.target_comm), "%s", "com.tencent.tmgp.pubgmhd");

	/* 卸载旧的 */
	if (module_loaded()) {
		char *rm[] = { "rmmod", MOD_NAME, NULL };
		printf("[*] 卸载已加载的 %s\n", MOD_NAME);
		if (run(rm) != 0)
			printf("[!] rmmod 失败, 可能仍在使用, 继续尝试加载\n");
	}

	write_proc_name(c.proc_name);

	/* 加载 */
	{
		char a_hd[32], a_hp[32], a_hg[32], a_ob[32], a_fl[32], a_gy[32];
		char a_tc[160], a_kp[320];
		char *ins[11];
		int rc;

		snprintf(a_hd, sizeof(a_hd), "hide_driver=%d",   c.hide_driver);
		snprintf(a_hp, sizeof(a_hp), "hide_pid=%d",      c.hide_pid);
		snprintf(a_hg, sizeof(a_hg), "hide_gpu=%d",      c.hide_gpu);
		snprintf(a_ob, sizeof(a_ob), "oppo_backdoor=%d", c.oppo_backdoor);
		snprintf(a_fl, sizeof(a_fl), "force_load=%d",    c.force_load);
		snprintf(a_gy, sizeof(a_gy), "gyro=%d",          c.gyro);
		snprintf(a_tc, sizeof(a_tc), "target_comm=%s",   c.target_comm);
		snprintf(a_kp, sizeof(a_kp), "ko_path=%s",       ko);

		ins[0]  = "insmod";
		ins[1]  = (char *)ko;
		ins[2]  = a_hd;
		ins[3]  = a_hp;
		ins[4]  = a_hg;
		ins[5]  = a_ob;
		ins[6]  = a_fl;
		ins[7]  = a_gy;
		ins[8]  = a_tc;
		ins[9]  = a_kp;
		ins[10] = NULL;

		printf("\n[*] insmod ...\n");
		fflush(stdout);
		rc = run(ins);
		if (rc != 0) {
			printf("[x] insmod 失败 rc=%d\n", rc);
			dump_dmesg();
			return rc;
		}
	}

	printf("[+] insmod 成功\n");
	sleep(1);

	if (module_loaded())
		printf("[+] 模块在线\n");
	else
		printf("[!] 不在 /proc/modules 里（hide_driver 生效时属正常）\n");

	dump_dmesg();

	printf("\n-------------------\n");
	printf("自动绑定目标: %s\n", c.target_comm);
	printf("游戏未启动时内核每 5 秒扫一次, 最多 5 分钟。\n");
	printf("验证: dmesg | grep wind-cfg | tail -5\n");
	return 0;
}
