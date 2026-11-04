#pragma once
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#define DEVICE_NAME "/dev/JiangNight"

enum OPERATIONS
{
	OP_BIND         = 0x800,
	OP_UNBIND       = 0x801,
	OP_READ_MEM     = 0x802,
	OP_WRITE_MEM    = 0x803,
	OP_READ_BATCH   = 0x804,
	OP_WRITE_BATCH  = 0x805,
	OP_SET_MODE     = 0x806,
	OP_MODULE_BASE  = 0x807,
	OP_HIDE_FILE    = 0x808,
	OP_HIDE_PROCESS = 0x809,
	OP_UNHIDE_ALL   = 0x80A,
};

enum READ_MODE
{
	MODE_FAST = 0, /* 有缓存, 高性能低发热 */
	MODE_SAFE = 1, /* 无缓存, 防侧信道, 发热 */
};

typedef struct _COPY_MEMORY
{
	pid_t pid;
	uintptr_t addr;
	void *buffer;
	size_t size;
} COPY_MEMORY;

typedef struct _MODULE_BASE
{
	pid_t pid;
	char *name;
	uintptr_t base;
} MODULE_BASE;

typedef struct _BATCH_ITEM
{
	uintptr_t addr;
	size_t size;
	void *buffer;
} BATCH_ITEM;

typedef struct _BATCH_REQUEST
{
	uint32_t count;
	uint32_t reserved;
	BATCH_ITEM items[];
} BATCH_REQUEST;

typedef struct _MODE_REQUEST
{
	uint32_t mode;
} MODE_REQUEST;

class c_driver
{
private:
	int fd;
	pid_t pid;
	bool bound;

public:
	c_driver()
	{
		fd = open(DEVICE_NAME, O_RDWR);
		bound = false;
		if (fd == -1)
		{
			printf("[-] open driver failed\n");
		}
	}

	~c_driver()
	{
		if (fd > 0)
			close(fd);
	}

	bool initialize(pid_t pid)
	{
		this->pid = pid;
		if (ioctl(fd, OP_BIND, (unsigned long)pid) != 0)
		{
			printf("[-] bind pid %d failed\n", pid);
			return false;
		}
		bound = true;
		return true;
	}

	void set_mode(int mode)
	{
		MODE_REQUEST mr;
		mr.mode = mode;
		ioctl(fd, OP_SET_MODE, &mr);
	}

	bool read(uintptr_t addr, void *buffer, size_t size)
	{
		COPY_MEMORY cm;
		cm.pid = this->pid;
		cm.addr = addr;
		cm.buffer = buffer;
		cm.size = size;
		return ioctl(fd, OP_READ_MEM, &cm) == 0;
	}

	bool write(uintptr_t addr, void *buffer, size_t size)
	{
		COPY_MEMORY cm;
		cm.pid = this->pid;
		cm.addr = addr;
		cm.buffer = buffer;
		cm.size = size;
		return ioctl(fd, OP_WRITE_MEM, &cm) == 0;
	}

	template <typename T>
	T read(uintptr_t addr)
	{
		T res;
		if (this->read(addr, &res, sizeof(T)))
			return res;
		return {};
	}

	template <typename T>
	bool write(uintptr_t addr, T value)
	{
		return this->write(addr, &value, sizeof(T));
	}

	/*
	 * 批量读: addrs 与 sizes 各 N 项, buffers 指向 N 个用户态缓冲。
	 * 返回成功读取的条目数。一次 ioctl 完成, 减少 syscall 开销。
	 */
	int read_batch(const uintptr_t *addrs, void **buffers, const size_t *sizes, uint32_t count)
	{
		if (count == 0 || count > 1024)
			return -1;
		size_t total = sizeof(BATCH_REQUEST) + count * sizeof(BATCH_ITEM);
		BATCH_REQUEST *req = (BATCH_REQUEST *)malloc(total);
		if (!req)
			return -1;
		req->count = count;
		req->reserved = 0;
		for (uint32_t i = 0; i < count; i++)
		{
			req->items[i].addr = addrs[i];
			req->items[i].size = sizes[i];
			req->items[i].buffer = buffers[i];
		}
		int ret = (int)ioctl(fd, OP_READ_BATCH, req);
		free(req);
		return ret;
	}

	int write_batch(const uintptr_t *addrs, void **buffers, const size_t *sizes, uint32_t count)
	{
		if (count == 0 || count > 1024)
			return -1;
		size_t total = sizeof(BATCH_REQUEST) + count * sizeof(BATCH_ITEM);
		BATCH_REQUEST *req = (BATCH_REQUEST *)malloc(total);
		if (!req)
			return -1;
		req->count = count;
		req->reserved = 0;
		for (uint32_t i = 0; i < count; i++)
		{
			req->items[i].addr = addrs[i];
			req->items[i].size = sizes[i];
			req->items[i].buffer = buffers[i];
		}
		int ret = (int)ioctl(fd, OP_WRITE_BATCH, req);
		free(req);
		return ret;
	}

	uintptr_t get_module_base(char *name)
	{
		MODULE_BASE mb;
		char buf[0x100];
		strcpy(buf, name);
		mb.pid = this->pid;
		mb.name = buf;
		if (ioctl(fd, OP_MODULE_BASE, &mb) != 0)
			return 0;
		return mb.base;
	}

	/* 隐藏接口 */
	int hide_file(const char *path)
	{
		return ioctl(fd, OP_HIDE_FILE, path);
	}

	int hide_process(pid_t pid)
	{
		return ioctl(fd, OP_HIDE_PROCESS, (unsigned long)pid);
	}

	int unhide_all()
	{
		return ioctl(fd, OP_UNHIDE_ALL, 0);
	}

	/*
	 * mmap 一段目标虚拟地址 (页对齐), 之后直接指针读写, 零 ioctl。
	 * 返回映射到本进程的虚拟地址, 失败返回 MAP_FAILED。
	 */
	void *map(uintptr_t target_va, size_t length)
	{
		void *p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
		               fd, (off_t)target_va);
		return p;
	}
};

static c_driver *driver = new c_driver();
