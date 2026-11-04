#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include "driver.hpp"

uint64_t get_tick_count64()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000 + ts.tv_nsec / (1000 * 1000));
}

pid_t get_name_pid(char *name)
{
	FILE *fp;
	pid_t pid;
	char cmd[0x100] = "pidof ";

	strcat(cmd, name);
	fp = popen(cmd, "r");
	fscanf(fp, "%d", &pid);
	pclose(fp);
	return pid;
}

int main(int argc, char const *argv[])
{
	uintptr_t base = 0;
	uint64_t result = 0;
	char module_name[0x100] = "libunity.so";
	pid_t pid = get_name_pid((char *)"com.tencent.tmgp.sgame");
	printf("pid = %d\n", pid);

	/* 1. 绑定 pid (缓存 task, 后续读写省 find_get_pid) */
	driver->initialize(pid);

	/* 2. 选择读取模式: MODE_FAST(有缓存,推荐) / MODE_SAFE(无缓存,防侧信道) */
	driver->set_mode(MODE_FAST);

	base = driver->get_module_base(module_name);
	printf("base = %lx\n", base);

	/* 3. 单次读 */
	result = driver->read<uint64_t>(base);
	printf("result = %lx\n", result);

	/* 4. 批量读: 一次 ioctl 读多个离散地址 */
	{
		const int N = 16;
		uintptr_t addrs[N];
		void *bufs[N];
		size_t sizes[N];
		uint64_t vals[N];

		for (int i = 0; i < N; i++)
		{
			addrs[i] = base + i * 8;
			bufs[i] = &vals[i];
			sizes[i] = sizeof(uint64_t);
		}
		int ok = driver->read_batch(addrs, bufs, sizes, N);
		printf("batch read ok=%d/%d\n", ok, N);
		for (int i = 0; i < N; i++)
			printf("  [0x%lx] = 0x%lx\n", addrs[i], vals[i]);
	}

	/* 5. mmap 零 ioctl 直读: 映射 base 起一页, 之后直接指针访问 */
	{
		uintptr_t page_base = base & ~(uintptr_t)0xFFF;
		void *mapped = driver->map(page_base, 0x1000);
		if (mapped != MAP_FAILED)
		{
			uint64_t *p = (uint64_t *)((uintptr_t)mapped + (base - page_base));
			printf("mmap[0x%lx] = 0x%lx\n", base, *p);
			munmap(mapped, 0x1000);
		}
	}

	return 0;
}
