#ifndef _COMM_H_
#define _COMM_H_

#include <linux/types.h>

/*
 * Wind_driver 通讯协议 v2
 * 目标: 减少 ioctl 调用次数, 支持批量读写与会话缓存, 双模式读取
 */

#define DEVICE_NAME "JiangNight"

/* ---- 操作码 ---- */
enum OPERATIONS
{
	OP_BIND         = 0x800, /* 绑定目标 pid, 缓存 task/mm (open 后先调用一次) */
	OP_UNBIND       = 0x801, /* 解绑并释放缓存 */
	OP_READ_MEM     = 0x802, /* 单次读 (兼容旧协议) */
	OP_WRITE_MEM    = 0x803, /* 单次写 (兼容旧协议) */
	OP_READ_BATCH   = 0x804, /* 批量读, 一次 ioctl 读多个离散地址 */
	OP_WRITE_BATCH  = 0x805, /* 批量写 */
	OP_SET_MODE     = 0x806, /* 切换读取模式 (安全/快速) */
	OP_MODULE_BASE  = 0x807, /* 取模块基址 */
	OP_HIDE_FILE    = 0x808, /* 隐藏文件 (按 basename) */
	OP_HIDE_PROCESS = 0x809, /* 隐藏进程 */
	OP_UNHIDE_ALL   = 0x80A, /* 撤销所有隐藏 */
	OP_BRK_SET      = 0x80B, /* 设置硬件断点 */
	OP_BRK_CLEAR    = 0x80C, /* 清除硬件断点 */
	OP_KGSL_HIDE    = 0x80D, /* kgsl 隐藏 pid */
	OP_KGSL_HOOK    = 0x80E, /* 安装 kgsl hook */
};

/* ---- 读取模式 ---- */
enum READ_MODE
{
	MODE_FAST = 0, /* 有缓存 (ioremap_cache): 高性能, 低发热, 但可能被 cache 侧信道检测 */
	MODE_SAFE = 1, /* 无缓存 (ioremap_nocache + flush): 防侧信道, 代价是发热/变慢 */
};

/* ---- 单次读写 (兼容旧 COPY_MEMORY 布局) ---- */
typedef struct _COPY_MEMORY
{
	pid_t pid;
	uintptr_t addr;
	void *buffer;   /* 用户态缓冲 */
	size_t size;
} COPY_MEMORY, *PCOPY_MEMORY;

/* ---- 模块基址 ---- */
typedef struct _MODULE_BASE
{
	pid_t pid;
	char *name;     /* 用户态模块名字符串指针 */
	uintptr_t base;
} MODULE_BASE, *PMODULE_BASE;

/*
 * ---- 批量读/写 ----
 * 一次 ioctl 传一个 BATCH_REQUEST 头部 + count 个 BATCH_ITEM。
 * 用户态:
 *   BATCH_ITEM items[N];
 *   for i: items[i].addr / items[i].size / items[i].buffer 指向用户态缓冲
 * 内核逐个 translate + 读/写 + copy_to/from_user。
 * 相比 N 次单点 ioctl, 省去 N-1 次 syscall 与 mm 定位。
 */
typedef struct _BATCH_ITEM
{
	uintptr_t addr;
	size_t size;
	void *buffer;   /* 用户态缓冲 */
} BATCH_ITEM, *PBATCH_ITEM;

typedef struct _BATCH_REQUEST
{
	uint32_t count;
	uint32_t reserved;
	BATCH_ITEM items[]; /* 变长数组 */
} BATCH_REQUEST, *PBATCH_REQUEST;

/* ---- 模式设置 ---- */
typedef struct _MODE_REQUEST
{
	uint32_t mode;  /* MODE_FAST / MODE_SAFE */
} MODE_REQUEST, *PMODE_REQUEST;

/* ---- 硬件断点 ---- */
typedef struct _BRK_REQUEST
{
	pid_t pid;
	uintptr_t addr;
	uint32_t type;  /* HW_BREAKPOINT_X / _R / _W / _RW */
} BRK_REQUEST, *PBRK_REQUEST;

/*
 * ---- mmap 映射约定 ----
 * 通过 mmap(fd, ...) 将目标进程的一段虚拟地址直接映射到用户态,
 * 之后直接指针读写, 零 ioctl 开销。
 * offset 高 32 位存 pid, 低 32 位存目标虚拟地址页对齐偏移。
 * 快速映射 (MODE_FAST) 用 remap_pfn_range + ioremap_cache 语义;
 * 安全映射 (MODE_SAFE) 用 ioremap_nocache, 避免 cache 侧信道。
 * 注意: 映射长度必须页对齐, 且目标虚拟地址必须落在已映射物理页内。
 */
#define MMAP_PID_SHIFT   32
#define MMAP_OFF_MASK    0xFFFFFFFFULL

#endif /* _COMM_H_ */
