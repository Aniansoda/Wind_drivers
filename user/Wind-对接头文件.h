#pragma once
/*
 * wind.h — wind 驱动用户态完整对接头文件 (作者: 川上富江Tomie)
 *
 * 一个头文件包含全部内容:
 *   - 传输层: 内核 netlink 私有协议 (默认, 无设备节点) / 文件 + ioctl (兼容)
 *   - 全部操作码 (与 kernel/comm.h 一一对应)
 *   - 全部请求/响应结构 (布局与内核逐字节一致)
 *   - c_wind: 读写 / 批量 / 隐藏 / kgsl / 陀螺仪 /
 *               hwbp / ptebp / brkbp / etmbp / udpbp 全功能封装
 *
 * 用法 (全部能力都在 c_wind 一个类里, 没有第二个类要管):
 *     #include "wind.hpp"
 *     c_wind &d = c_wind::instance(target_pid);   // 进程内单例, 默认 netlink unit 31
 *     int v = d.read<int>(base + 0x10);
 *     d.write<int>(base + 0x10, 123);
 *
 *     d.frame_begin();                 // 一帧内同地址只读一次
 *     uint64_t hp = 0; d.frame_read(base + 0x100, hp);
 *
 *     d.snap_start(base + 0x200, 60);  // 后台 60Hz 刷新
 *     uint32_t cur = d.snap_value<uint32_t>();
 *     d.snap_stop();
 *
 * 需要多个通道实例时自己构造, 都是 c_wind 的实例:
 *     c_wind d2(30);                 // netlink, 指定协议号
 *     c_wind d3("/dev/null");        // 文件模式 transport=1
 *     c_wind d4("/data/local/tmp/windc");  // 文件模式 transport=2
 *
 * 内存占用: sizeof(c_wind) 约 48 KB, 其中 47 KB 是 frame_ 的帧缓存表 (2048 槽)
 *           单例与静态实例不占栈; 要在函数里建局部实例, 建议 new 到堆上
 *
 * 传输选择:
 *     c_wind()               → netlink (驱动 transport=3, 默认)
 *     c_wind(30)             → netlink, 指定协议号
 *     c_wind("/dev/null")    → 文件模式 (驱动 transport=1)
 *     c_wind("/data/local/tmp/windc") → 文件模式 (驱动 transport=2, 自己 mknod)
 *
 */

#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

#define WIND_NL_DEFAULT_UNIT 31
#define WIND_NL_MAGIC        0x574E4C31u /* "WNL1" */

#define HOOK_DEVICE "/dev/null"

typedef struct _WIND_NL_MSG
{
	uint32_t magic;  /* WIND_NL_MAGIC */
	uint32_t op;     /* OPERATIONS 之一 */
	uint64_t arg;    /* 用户指针/值, 语义同 ioctl 第三参数 */
	int64_t  result; /* 内核回填 */
} WIND_NL_MSG;

#define WIND_OP_BASE 0x574E0000

enum OPERATIONS
{
	/* 核心 */
	OP_BIND          = WIND_OP_BASE + 0x01, /* arg = pid */
	OP_UNBIND        = WIND_OP_BASE + 0x02, /* arg = 0 */
	OP_READ_MEM      = WIND_OP_BASE + 0x03, /* arg = COPY_MEMORY* */
	OP_WRITE_MEM     = WIND_OP_BASE + 0x04, /* arg = COPY_MEMORY* */
	OP_READ_BATCH    = WIND_OP_BASE + 0x05, /* arg = BATCH_REQUEST* */
	OP_WRITE_BATCH   = WIND_OP_BASE + 0x06, /* arg = BATCH_REQUEST* */
	OP_SET_MODE      = WIND_OP_BASE + 0x07, /* arg = MODE_REQUEST* */
	OP_MODULE_BASE   = WIND_OP_BASE + 0x08, /* arg = MODULE_BASE* */

	/* 隐藏 */
	OP_HIDE_FILE     = WIND_OP_BASE + 0x09, /* arg = char* 路径 */
	OP_HIDE_PROCESS  = WIND_OP_BASE + 0x0A, /* arg = pid */
	OP_UNHIDE_ALL    = WIND_OP_BASE + 0x0B, /* arg = 0 */

	/* 旧版断点 (perf hw_breakpoint 直通) */
	OP_BRK_SET       = WIND_OP_BASE + 0x0C, /* arg = BRK_REQUEST* */
	OP_BRK_CLEAR     = WIND_OP_BASE + 0x0D, /* arg = BRK_REQUEST* */

	/* kgsl (GPU 侧隐藏) */
	OP_KGSL_HIDE     = WIND_OP_BASE + 0x0E, /* arg = pid */
	OP_KGSL_HOOK     = WIND_OP_BASE + 0x0F, /* arg = 0 */

	/* 陀螺仪 */
	OP_GYRO_INSTALL  = WIND_OP_BASE + 0x10, /* arg = pid */
	OP_GYRO_REMOVE   = WIND_OP_BASE + 0x11, /* arg = 0 */
	OP_GYRO_SET      = WIND_OP_BASE + 0x12, /* arg = GYRO_REQUEST* */
	OP_GYRO_ENABLE   = WIND_OP_BASE + 0x13, /* arg = 0/1 */
	OP_GYRO_STOP     = WIND_OP_BASE + 0x2C, /* arg = 0 */
	OP_GYRO_RESUME   = WIND_OP_BASE + 0x2D, /* arg = 0 */

	/* 硬件断点 (BRP/WRP debug 寄存器, 最多 16 点) */
	OP_HWBP_SET           = WIND_OP_BASE + 0x14,
	OP_HWBP_REMOVE        = WIND_OP_BASE + 0x15,
	OP_HWBP_GET_CONFIG    = WIND_OP_BASE + 0x16,
	OP_HWBP_GET_RECORDS   = WIND_OP_BASE + 0x17,
	OP_HWBP_SET_RECORD    = WIND_OP_BASE + 0x18,
	OP_HWBP_CLEAR_RECORDS = WIND_OP_BASE + 0x19,

	/* PTE 影子页执行断点 (改权限位触发 fault, 不占 debug 寄存器) */
	OP_PTEBP_SET           = WIND_OP_BASE + 0x1A,
	OP_PTEBP_REMOVE        = WIND_OP_BASE + 0x1B,
	OP_PTEBP_GET_CONFIG    = WIND_OP_BASE + 0x1C,
	OP_PTEBP_GET_RECORDS   = WIND_OP_BASE + 0x1D,
	OP_PTEBP_SET_RECORD    = WIND_OP_BASE + 0x1E,
	OP_PTEBP_CLEAR_RECORDS = WIND_OP_BASE + 0x1F,

	/* BRK 软断点 (改写指令为 BRK, 数量无限, 有完整性暴露面) */
	OP_BRKBP_SET           = WIND_OP_BASE + 0x20,
	OP_BRKBP_REMOVE        = WIND_OP_BASE + 0x21,
	OP_BRKBP_GET_CONFIG    = WIND_OP_BASE + 0x22,
	OP_BRKBP_GET_RECORDS   = WIND_OP_BASE + 0x23,
	OP_BRKBP_SET_RECORD    = WIND_OP_BASE + 0x24,
	OP_BRKBP_CLEAR_RECORDS = WIND_OP_BASE + 0x25,

	/* ETM 硬件分支追踪 (Coresight, 不改代码不改页表) */
	OP_ETMBP_SET           = WIND_OP_BASE + 0x26,
	OP_ETMBP_REMOVE        = WIND_OP_BASE + 0x27,
	OP_ETMBP_GET_CONFIG    = WIND_OP_BASE + 0x28,
	OP_ETMBP_GET_RECORDS   = WIND_OP_BASE + 0x29,
	OP_ETMBP_SET_RECORD    = WIND_OP_BASE + 0x2A,
	OP_ETMBP_CLEAR_RECORDS = WIND_OP_BASE + 0x2B,

	/* UDP 断点 (内核 UDP 收发路径 kprobe) */
	OP_UDPBP_SET           = WIND_OP_BASE + 0x2E,
	OP_UDPBP_REMOVE        = WIND_OP_BASE + 0x2F,
	OP_UDPBP_GET_CONFIG    = WIND_OP_BASE + 0x30,
	OP_UDPBP_GET_RECORDS   = WIND_OP_BASE + 0x31,
	OP_UDPBP_CLEAR_RECORDS = WIND_OP_BASE + 0x32,
};

/* 读取模式 */
enum READ_MODE
{
	MODE_FAST = 0, /* ioremap_cache: 快, 低发热, 可能被 cache 侧信道发现 */
	MODE_SAFE = 1, /* ioremap_nocache + flush: 慢, 防侧信道 */
};

typedef struct _COPY_MEMORY
{
	pid_t pid;
	uintptr_t addr;
	void *buffer;
	size_t size;
} COPY_MEMORY, *PCOPY_MEMORY;

typedef struct _MODULE_BASE
{
	pid_t pid;
	char *name; /* 模块名, 内核按 basename 匹配 */
	uintptr_t base;
} MODULE_BASE, *PMODULE_BASE;

typedef struct _BATCH_ITEM
{
	uintptr_t addr;
	size_t size;
	void *buffer;
} BATCH_ITEM, *PBATCH_ITEM;

typedef struct _BATCH_REQUEST
{
	uint32_t count;
	uint32_t reserved;
	BATCH_ITEM items[]; /* 变长, 最多 1024 */
} BATCH_REQUEST, *PBATCH_REQUEST;

typedef struct _MODE_REQUEST
{
	uint32_t mode; /* MODE_FAST / MODE_SAFE */
} MODE_REQUEST, *PMODE_REQUEST;

typedef struct _BRK_REQUEST
{
	pid_t pid;
	uintptr_t addr;
	uint32_t type;
} BRK_REQUEST, *PBRK_REQUEST;

/* 断点类型 / 长度 */
enum BP_TYPE
{
	BP_TYPE_EXEC  = 1,
	BP_TYPE_READ  = 2,
	BP_TYPE_WRITE = 3,
	BP_TYPE_RW    = 4,
};

enum BP_LEN
{
	BP_LEN_1 = 1,
	BP_LEN_2 = 2,
	BP_LEN_4 = 4,
	BP_LEN_8 = 8,
};

#define MMAP_PID_SHIFT 32
#define MMAP_OFF_MASK  0xFFFFFFFFULL

typedef struct _HWBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;   /* BP_TYPE_* */
	uint32_t length; /* BP_LEN_* */
} HWBP_POINT_CONFIG, *PHWBP_POINT_CONFIG;

typedef struct _HWBP_CONFIG
{
	uint32_t num_brps;
	uint32_t num_wrps;
	uint32_t active_count;
	uint32_t reserved;
} HWBP_CONFIG, *PHWBP_CONFIG;

typedef struct _HWBP_RECORD
{
	uint64_t trigger_pc;
	uint64_t hit_addr;
	uint32_t op_mask; /* bit0=读 bit1=写 */
	uint32_t reserved;
} HWBP_RECORD, *PHWBP_RECORD;

typedef struct _HWBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	HWBP_POINT_CONFIG points[];
} HWBP_SET_REQUEST, *PHWBP_SET_REQUEST;

typedef struct _HWBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	HWBP_RECORD *records;
	uint32_t *count_out;
} HWBP_RECORDS_REQUEST, *PHWBP_RECORDS_REQUEST;

typedef struct _HWBP_RECORD_REQUEST
{
	uint32_t point_index;
	uint32_t record_index;
	uint32_t reserved;
	HWBP_RECORD record;
} HWBP_RECORD_REQUEST, *PHWBP_RECORD_REQUEST;

typedef struct _HWBP_CLEAR_REQUEST
{
	int32_t point_index; /* -1 = 全部 */
	uint32_t reserved;
} HWBP_CLEAR_REQUEST, *PHWBP_CLEAR_REQUEST;

typedef struct _PTEBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;   /* 恒 BP_TYPE_EXEC */
	uint32_t length; /* 恒 4 */
} PTEBP_POINT_CONFIG, *PPTEBP_POINT_CONFIG;

typedef struct _PTEBP_CONFIG
{
	uint32_t active_count;
	uint32_t reserved;
} PTEBP_CONFIG, *PPTEBP_CONFIG;

typedef struct _PTEBP_RECORD
{
	uint64_t trigger_pc;
	uint64_t hit_addr;
	uint32_t op_mask;
	uint32_t reserved;
} PTEBP_RECORD, *PPTEBP_RECORD;

typedef struct _PTEBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	PTEBP_POINT_CONFIG points[];
} PTEBP_SET_REQUEST, *PPTEBP_SET_REQUEST;

typedef struct _PTEBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	PTEBP_RECORD *records;
	uint32_t *count_out;
} PTEBP_RECORDS_REQUEST, *PPTEBP_RECORDS_REQUEST;

typedef struct _PTEBP_RECORD_REQUEST
{
	uint32_t point_index;
	uint32_t record_index;
	uint32_t reserved;
	PTEBP_RECORD record;
} PTEBP_RECORD_REQUEST, *PPTEBP_RECORD_REQUEST;

typedef struct _PTEBP_CLEAR_REQUEST
{
	int32_t point_index;
	uint32_t reserved;
} PTEBP_CLEAR_REQUEST, *PPTEBP_CLEAR_REQUEST;

typedef struct _BRKBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;   /* 恒 BP_TYPE_EXEC */
	uint32_t length; /* 恒 4 */
} BRKBP_POINT_CONFIG, *PBRKBP_POINT_CONFIG;

typedef struct _BRKBP_CONFIG
{
	uint32_t active_count;
	uint32_t reserved;
} BRKBP_CONFIG, *PBRKBP_CONFIG;

typedef struct _BRKBP_RECORD
{
	uint64_t trigger_pc;
	uint64_t hit_addr;
	uint32_t op_mask;
	uint32_t reserved;
} BRKBP_RECORD, *PBRKBP_RECORD;

typedef struct _BRKBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	BRKBP_POINT_CONFIG points[];
} BRKBP_SET_REQUEST, *PBRKBP_SET_REQUEST;

typedef struct _BRKBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	BRKBP_RECORD *records;
	uint32_t *count_out;
} BRKBP_RECORDS_REQUEST, *PBRKBP_RECORDS_REQUEST;

typedef struct _BRKBP_RECORD_REQUEST
{
	uint32_t point_index;
	uint32_t record_index;
	uint32_t reserved;
	BRKBP_RECORD record;
} BRKBP_RECORD_REQUEST, *PBRKBP_RECORD_REQUEST;

typedef struct _BRKBP_CLEAR_REQUEST
{
	int32_t point_index;
	uint32_t reserved;
} BRKBP_CLEAR_REQUEST, *PBRKBP_CLEAR_REQUEST;

typedef struct _ETMBP_POINT_CONFIG
{
	uint64_t start; /* 区间起始 */
	uint64_t end;   /* 区间结束 (开区间) */
	uint32_t flags; /* 0=分支追踪 1=指令追踪 */
	uint32_t reserved;
} ETMBP_POINT_CONFIG, *PETMBP_POINT_CONFIG;

typedef struct _ETMBP_CONFIG
{
	uint32_t active_count;
	uint32_t trace_id;
	uint32_t reserved;
} ETMBP_CONFIG, *PETMBP_CONFIG;

typedef struct _ETMBP_RECORD
{
	uint64_t addr;
	uint64_t timestamp;
	uint32_t flags; /* bit0=分支 1=异常 2=返回 */
	uint32_t reserved;
} ETMBP_RECORD, *PETMBP_RECORD;

typedef struct _ETMBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	ETMBP_POINT_CONFIG points[];
} ETMBP_SET_REQUEST, *PETMBP_SET_REQUEST;

typedef struct _ETMBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	ETMBP_RECORD *records;
	uint32_t *count_out;
} ETMBP_RECORDS_REQUEST, *PETMBP_RECORDS_REQUEST;

typedef struct _ETMBP_RECORD_REQUEST
{
	uint32_t point_index;
	uint32_t record_index;
	uint32_t reserved;
	ETMBP_RECORD record;
} ETMBP_RECORD_REQUEST, *PETMBP_RECORD_REQUEST;

typedef struct _ETMBP_CLEAR_REQUEST
{
	int32_t point_index;
	uint32_t reserved;
} ETMBP_CLEAR_REQUEST, *PETMBP_CLEAR_REQUEST;

typedef struct _GYRO_REQUEST
{
	uint32_t rx, ry, rz;
} GYRO_REQUEST, *PGYRO_REQUEST;

enum UDPBP_DIR
{
	UDPBP_DIR_TX   = 0,
	UDPBP_DIR_RX   = 1,
	UDPBP_DIR_BOTH = 2,
};

enum UDPBP_ACTION
{
	UDPBP_ACT_LOG     = 0,
	UDPBP_ACT_REWRITE = 1,
};

#define UDPBP_MAX_SLOTS   8
#define UDPBP_PATCH_MAX   64
#define UDPBP_PREVIEW_MAX 32
#define UDPBP_RECORD_MAX  256

typedef struct _UDPBP_POINT_CONFIG
{
	uint32_t pid;       /* 0 = 任意 tgid */
	uint16_t port;      /* 0 = 任意 (主机序; TX 比目的端口, RX 比源端口) */
	uint16_t dir;       /* UDPBP_DIR_* */
	uint16_t action;    /* UDPBP_ACTION_* */
	uint16_t patch_len; /* <= UDPBP_PATCH_MAX */
	uint8_t  patch[UDPBP_PATCH_MAX];
} UDPBP_POINT_CONFIG, *PUDPBP_POINT_CONFIG;

typedef struct _UDPBP_RECORD
{
	uint64_t ts_ns;
	uint32_t pid;
	uint32_t saddr; /* 网络序 IPv4 */
	uint32_t daddr;
	uint16_t sport; /* 主机序 */
	uint16_t dport;
	uint16_t dir;
	uint16_t action;
	uint32_t len;
	uint32_t preview_len;
	uint8_t  preview[UDPBP_PREVIEW_MAX];
	uint32_t reserved;
} UDPBP_RECORD, *PUDPBP_RECORD;

typedef struct _UDPBP_CONFIG
{
	uint32_t slot_count;
	uint32_t reserved;
	UDPBP_POINT_CONFIG slots[UDPBP_MAX_SLOTS];
} UDPBP_CONFIG, *PUDPBP_CONFIG;

typedef struct _UDPBP_SET_REQUEST
{
	int32_t slot_index;
	uint32_t reserved;
	int32_t *slot_out;
	UDPBP_POINT_CONFIG point;
} UDPBP_SET_REQUEST, *PUDPBP_SET_REQUEST;

typedef struct _UDPBP_RECORDS_REQUEST
{
	int32_t slot_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	UDPBP_RECORD *records;
	uint32_t *count_out;
} UDPBP_RECORDS_REQUEST, *PUDPBP_RECORDS_REQUEST;

typedef struct _UDPBP_CLEAR_REQUEST
{
	int32_t slot_index;
	uint32_t reserved;
} UDPBP_CLEAR_REQUEST, *PUDPBP_CLEAR_REQUEST;

class c_wind
{
private:
	int fd;
	int nl_fd;
	int nl_unit;
	uint32_t nl_seq;
	pid_t pid;
	bool bound;
	pthread_mutex_t mtx;   /* 后台快照线程与绘制线程共用一条通道, 必须串行 */

	long xfer_impl(unsigned int op, uintptr_t arg)
	{
		long ret;

		pthread_mutex_lock(&mtx);
		ret = xfer_locked(op, arg);
		pthread_mutex_unlock(&mtx);
		return ret;
	}

	long xfer_locked(unsigned int op, uintptr_t arg)
	{
		if (nl_fd >= 0)
		{
			unsigned char sbuf[NLMSG_HDRLEN + sizeof(WIND_NL_MSG)];
			unsigned char rbuf[NLMSG_HDRLEN + sizeof(WIND_NL_MSG)];
			struct nlmsghdr *nlh, *rnlh;
			struct sockaddr_nl sa;
			struct iovec iov;
			struct msghdr msg;
			WIND_NL_MSG *m;
			ssize_t n;

			memset(sbuf, 0, sizeof(sbuf));
			nlh = (struct nlmsghdr *)sbuf;
			nlh->nlmsg_len = NLMSG_LENGTH(sizeof(WIND_NL_MSG));
			nlh->nlmsg_seq = ++nl_seq;
			nlh->nlmsg_pid = 0;

			m = (WIND_NL_MSG *)NLMSG_DATA(nlh);
			m->magic = WIND_NL_MAGIC;
			m->op = op;
			m->arg = (uint64_t)arg;
			m->result = 0;

			memset(&sa, 0, sizeof(sa));
			sa.nl_family = AF_NETLINK;
			iov.iov_base = sbuf;
			iov.iov_len = nlh->nlmsg_len;
			memset(&msg, 0, sizeof(msg));
			msg.msg_name = &sa;
			msg.msg_namelen = sizeof(sa);
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;

			if (sendmsg(nl_fd, &msg, 0) < 0)
				return -errno;

			n = recv(nl_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
				for (int attempt = 0; attempt < 2; attempt++)
				{
					struct pollfd pfd;
					int pr;

					pfd.fd = nl_fd;
					pfd.events = POLLIN;
					pfd.revents = 0;
					pr = poll(&pfd, 1, 50);
					if (pr > 0)
					{
						n = recv(nl_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT);
						break;
					}
					if (pr < 0 && errno != EINTR)
						return -errno;
				}
			}
			if (n < (ssize_t)(NLMSG_HDRLEN + sizeof(WIND_NL_MSG)))
				return (n < 0) ? -errno : -ETIMEDOUT;

			rnlh = (struct nlmsghdr *)rbuf;
			if (rnlh->nlmsg_len < NLMSG_LENGTH(sizeof(WIND_NL_MSG)))
				return -EBADMSG;
			m = (WIND_NL_MSG *)NLMSG_DATA(rnlh);
			if (m->magic != WIND_NL_MAGIC)
				return -EBADMSG;
			return (long)m->result;
		}
		return ioctl(fd, op, arg);
	}

	long bp_set_raw(unsigned int op, const void *points, uint32_t count, size_t point_size)
	{
		uint8_t *buf;
		size_t total;
		long ret;

		if (count == 0 || count > 16 || !points)
			return -1;
		total = 8 + (size_t)count * point_size;
		buf = (uint8_t *)malloc(total);
		if (!buf)
			return -1;
		memcpy(buf, &count, 4);
		uint32_t zero = 0;
		memcpy(buf + 4, &zero, 4);
		memcpy(buf + 8, points, (size_t)count * point_size);
		ret = xfer_impl(op, (uintptr_t)buf);
		free(buf);
		return ret;
	}

	struct bp_records_req
	{
		uint32_t point_index;
		uint32_t start_index;
		uint32_t capacity;
		uint32_t reserved;
		void *records;
		uint32_t *count_out;
	};

	long bp_get_records_raw(unsigned int op, uint32_t point_index, uint32_t start,
	                        void *recs, uint32_t cap, uint32_t *count_out)
	{
		struct bp_records_req req;

		if (!recs || cap == 0)
			return -1;
		memset(&req, 0, sizeof(req));
		req.point_index = point_index;
		req.start_index = start;
		req.capacity = cap;
		req.records = recs;
		req.count_out = count_out;
		return xfer_impl(op, (uintptr_t)&req);
	}

	long bp_set_record_raw(unsigned int op, uint32_t point_index, uint32_t record_index,
	                       const void *record, size_t record_size)
	{
		uint8_t buf[16 + 24];

		if (!record || record_size > 24)
			return -1;
		memset(buf, 0, sizeof(buf));
		memcpy(buf, &point_index, 4);
		memcpy(buf + 4, &record_index, 4);
		memcpy(buf + 16, record, record_size);
		return xfer_impl(op, (uintptr_t)buf);
	}

	long bp_clear_records_raw(unsigned int op, int32_t point_index)
	{
		int32_t buf[2];

		buf[0] = point_index;
		buf[1] = 0;
		return xfer_impl(op, (uintptr_t)buf);
	}

public:
	template <typename T>
	long xfer(unsigned int op, T arg)
	{
		return xfer_impl(op, (uintptr_t)arg);
	}

	/* 默认 netlink (驱动 transport=3), 无设备节点 */
	explicit c_wind(int unit = WIND_NL_DEFAULT_UNIT)
	{
		struct sockaddr_nl sa;

		fd = -1;
		nl_unit = -1;
		bound = false;
		nl_seq = 0;
		pid = 0;
		pthread_mutex_init(&mtx, NULL);
		frame_.attach(this);
		snap_.attach(this);

		nl_fd = socket(AF_NETLINK, SOCK_RAW, unit);
		if (nl_fd < 0)
		{
			printf("[-] netlink socket(unit=%d) 失败: %s\n", unit, strerror(errno));
			return;
		}
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = (uint32_t)getpid();
		if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		{
			printf("[-] netlink bind 失败: %s\n", strerror(errno));
			close(nl_fd);
			nl_fd = -1;
			return;
		}
		nl_unit = unit;
	}

	/* 文件模式: transport=1 用 /dev/null, transport=2 用自己 mknod 的节点 */
	explicit c_wind(const char *dev)
	{
		fd = -1;
		nl_fd = -1;
		nl_unit = -1;
		bound = false;
		nl_seq = 0;
		pid = 0;
		pthread_mutex_init(&mtx, NULL);
		frame_.attach(this);
		snap_.attach(this);

		fd = open(dev, O_RDWR);
		if (fd < 0)
			printf("[-] open %s 失败: %s\n", dev, strerror(errno));
	}

	~c_wind()
	{
		snap_.stop();
		if (bound)
			xfer_locked(OP_UNBIND, 0);
		if (fd >= 0)
			close(fd);
		if (nl_fd >= 0)
			close(nl_fd);
		pthread_mutex_destroy(&mtx);
	}

	bool ok() const { return fd > 0 || nl_fd > 0; }
	bool is_bound() const { return bound; }
	pid_t target_pid() const { return pid; }
	int netlink_unit() const { return nl_unit; }

	/* ---- 会话 ---- */

	bool initialize(pid_t target)
	{
		if (xfer_impl(OP_BIND, (uintptr_t)target) != 0)
		{
			printf("[-] bind pid %d 失败\n", target);
			return false;
		}
		pid = target;
		bound = true;
		return true;
	}

	int unbind()
	{
		int ret = (int)xfer_impl(OP_UNBIND, 0);

		bound = false;
		pid = 0;
		return ret;
	}

	int set_mode(int mode)
	{
		MODE_REQUEST mr;

		mr.mode = (uint32_t)mode;
		return (int)xfer_impl(OP_SET_MODE, (uintptr_t)&mr);
	}

	int set_fast() { return set_mode(MODE_FAST); }
	int set_safe() { return set_mode(MODE_SAFE); }

	/* ---- 内存读写 ---- */

	bool read(uintptr_t addr, void *buffer, size_t size)
	{
		COPY_MEMORY cm;

		cm.pid = pid;
		cm.addr = addr;
		cm.buffer = buffer;
		cm.size = size;
		return xfer_impl(OP_READ_MEM, (uintptr_t)&cm) == 0;
	}

	bool write(uintptr_t addr, const void *buffer, size_t size)
	{
		COPY_MEMORY cm;

		cm.pid = pid;
		cm.addr = addr;
		cm.buffer = (void *)buffer;
		cm.size = size;
		return xfer_impl(OP_WRITE_MEM, (uintptr_t)&cm) == 0;
	}

	template <typename T>
	T read(uintptr_t addr)
	{
		T res;

		memset(&res, 0, sizeof(T));
		read(addr, &res, sizeof(T));
		return res;
	}

	template <typename T>
	bool write(uintptr_t addr, T value)
	{
		return write(addr, &value, sizeof(T));
	}

	int read_batch(const uintptr_t *addrs, void **buffers, const size_t *sizes, uint32_t count)
	{
		BATCH_REQUEST *req;
		size_t total;
		int ret;

		if (count == 0 || count > 1024 || !addrs || !buffers || !sizes)
			return -1;
		total = sizeof(BATCH_REQUEST) + (size_t)count * sizeof(BATCH_ITEM);
		req = (BATCH_REQUEST *)malloc(total);
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
		ret = (int)xfer_impl(OP_READ_BATCH, (uintptr_t)req);
		free(req);
		return ret;
	}

	int write_batch(const uintptr_t *addrs, void **buffers, const size_t *sizes, uint32_t count)
	{
		BATCH_REQUEST *req;
		size_t total;
		int ret;

		if (count == 0 || count > 1024 || !addrs || !buffers || !sizes)
			return -1;
		total = sizeof(BATCH_REQUEST) + (size_t)count * sizeof(BATCH_ITEM);
		req = (BATCH_REQUEST *)malloc(total);
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
		ret = (int)xfer_impl(OP_WRITE_BATCH, (uintptr_t)req);
		free(req);
		return ret;
	}

	struct FieldRef
	{
		size_t off;
		size_t size;
		void *dst;
	};

	int read_fields(uintptr_t base, const FieldRef *fields, int count)
	{
		BATCH_REQUEST *req;
		size_t total;
		int ret;

		if (!fields || count <= 0)
			return -1;
		if (count > 256)
			count = 256;
		total = sizeof(BATCH_REQUEST) + (size_t)count * sizeof(BATCH_ITEM);
		req = (BATCH_REQUEST *)malloc(total);
		if (!req)
			return -1;
		req->count = (uint32_t)count;
		req->reserved = 0;
		for (int i = 0; i < count; i++)
		{
			req->items[i].addr = base + fields[i].off;
			req->items[i].size = fields[i].size;
			req->items[i].buffer = fields[i].dst;
		}
		ret = (int)xfer_impl(OP_READ_BATCH, (uintptr_t)req);
		free(req);
		return ret;
	}

	/* ---- 模块基址 ---- */

	uintptr_t get_module_base(const char *name)
	{
		MODULE_BASE mb;
		char buf[0x100] = {0};

		if (!name)
			return 0;
		strncpy(buf, name, sizeof(buf) - 1);
		mb.pid = pid;
		mb.name = buf;
		mb.base = 0;
		if (xfer_impl(OP_MODULE_BASE, (uintptr_t)&mb) != 0)
			return 0;
		return mb.base;
	}

	uintptr_t base_plus(const char *name, uintptr_t off)
	{
		uintptr_t b = get_module_base(name);

		return b ? b + off : 0;
	}

	/* ---- mmap 直读 (仅文件模式有效) ---- */

	void *map(uintptr_t target_va, size_t length)
	{
		if (fd < 0)
			return MAP_FAILED;
		return mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		            (off_t)(target_va & ~(uintptr_t)(getpagesize() - 1)));
	}

	static void unmap(void *p, size_t length)
	{
		if (p && p != MAP_FAILED)
			munmap(p, length);
	}

	/* 按目标虚拟地址直接映射, 返回值已经加上页内偏移, 读它就是一条 load。
	 * 只读映射, 页表被改后看到的是旧页 —— 要绝对新鲜就走 read()。 */
	void *map_target(uintptr_t va, size_t length)
	{
		uintptr_t page = va & ~(uintptr_t)(getpagesize() - 1);
		size_t span = length + (size_t)(va - page);
		void *p;

		if (fd < 0)
			return NULL;
		p = mmap(NULL, span, PROT_READ, MAP_SHARED, fd, (off_t)page);
		if (p == MAP_FAILED)
			return NULL;
		return (char *)p + (va - page);
	}

	void unmap_target(void *p, uintptr_t va, size_t length)
	{
		uintptr_t page = va & ~(uintptr_t)(getpagesize() - 1);
		size_t span = length + (size_t)(va - page);

		if (p)
			munmap((char *)p - (va - page), span);
	}

	/* ---- 隐藏 ---- */

	int hide_file(const char *path) { return (int)xfer_impl(OP_HIDE_FILE, (uintptr_t)path); }
	int hide_process(pid_t target) { return (int)xfer_impl(OP_HIDE_PROCESS, (uintptr_t)target); }
	int unhide_all() { return (int)xfer_impl(OP_UNHIDE_ALL, 0); }

	/* ---- kgsl (GPU 侧 pid 隐藏) ---- */

	int kgsl_hide(pid_t target) { return (int)xfer_impl(OP_KGSL_HIDE, (uintptr_t)target); }
	int kgsl_hook() { return (int)xfer_impl(OP_KGSL_HOOK, 0); }

	/* ---- 旧版断点 (perf 直通) ---- */

	int brk_set(pid_t target, uintptr_t addr, uint32_t type)
	{
		BRK_REQUEST br;

		br.pid = target;
		br.addr = addr;
		br.type = type;
		return (int)xfer_impl(OP_BRK_SET, (uintptr_t)&br);
	}

	int brk_clear(pid_t target, uintptr_t addr)
	{
		BRK_REQUEST br;

		br.pid = target;
		br.addr = addr;
		br.type = 0;
		return (int)xfer_impl(OP_BRK_CLEAR, (uintptr_t)&br);
	}

	/* ---- 陀螺仪 ---- */

	int gyro_install(pid_t target) { return (int)xfer_impl(OP_GYRO_INSTALL, (uintptr_t)target); }
	int gyro_remove() { return (int)xfer_impl(OP_GYRO_REMOVE, 0); }

	int gyro_set(float rx, float ry, float rz)
	{
		GYRO_REQUEST gr;
		union { float f; uint32_t u; } c;

		c.f = rx; gr.rx = c.u;
		c.f = ry; gr.ry = c.u;
		c.f = rz; gr.rz = c.u;
		return (int)xfer_impl(OP_GYRO_SET, (uintptr_t)&gr);
	}

	int gyro_enable(bool enabled) { return (int)xfer_impl(OP_GYRO_ENABLE, (uintptr_t)(enabled ? 1 : 0)); }
	int gyro_stop() { return (int)xfer_impl(OP_GYRO_STOP, 0); }
	int gyro_resume() { return (int)xfer_impl(OP_GYRO_RESUME, 0); }

	/* ---- 硬件断点 HWBP ---- */

	int hwbp_set(const HWBP_POINT_CONFIG *pts, uint32_t count)
	{
		return (int)bp_set_raw(OP_HWBP_SET, pts, count, sizeof(HWBP_POINT_CONFIG));
	}

	int hwbp_exec(uint64_t addr)
	{
		HWBP_POINT_CONFIG p = {addr, BP_TYPE_EXEC, BP_LEN_4};

		return hwbp_set(&p, 1);
	}

	int hwbp_watch(uint64_t addr, uint32_t type, uint32_t len)
	{
		HWBP_POINT_CONFIG p = {addr, type, len};

		return hwbp_set(&p, 1);
	}

	int hwbp_remove() { return (int)xfer_impl(OP_HWBP_REMOVE, 0); }

	int hwbp_get_config(HWBP_CONFIG *out)
	{
		return out ? (int)xfer_impl(OP_HWBP_GET_CONFIG, (uintptr_t)out) : -1;
	}

	int hwbp_get_records(uint32_t point_index, uint32_t start, HWBP_RECORD *recs,
	                     uint32_t cap, uint32_t *count_out)
	{
		return (int)bp_get_records_raw(OP_HWBP_GET_RECORDS, point_index, start,
		                               recs, cap, count_out);
	}

	int hwbp_set_record(uint32_t point_index, uint32_t record_index, const HWBP_RECORD &rec)
	{
		return (int)bp_set_record_raw(OP_HWBP_SET_RECORD, point_index, record_index,
		                              &rec, sizeof(rec));
	}

	int hwbp_clear_records(int32_t point_index)
	{
		return (int)bp_clear_records_raw(OP_HWBP_CLEAR_RECORDS, point_index);
	}

	/* ---- PTE 影子页断点 PTEBP ---- */

	int ptebp_set(const PTEBP_POINT_CONFIG *pts, uint32_t count)
	{
		return (int)bp_set_raw(OP_PTEBP_SET, pts, count, sizeof(PTEBP_POINT_CONFIG));
	}

	int ptebp_exec(uint64_t addr)
	{
		PTEBP_POINT_CONFIG p = {addr, BP_TYPE_EXEC, BP_LEN_4};

		return ptebp_set(&p, 1);
	}

	int ptebp_remove() { return (int)xfer_impl(OP_PTEBP_REMOVE, 0); }

	int ptebp_get_config(PTEBP_CONFIG *out)
	{
		return out ? (int)xfer_impl(OP_PTEBP_GET_CONFIG, (uintptr_t)out) : -1;
	}

	int ptebp_get_records(uint32_t point_index, uint32_t start, PTEBP_RECORD *recs,
	                      uint32_t cap, uint32_t *count_out)
	{
		return (int)bp_get_records_raw(OP_PTEBP_GET_RECORDS, point_index, start,
		                               recs, cap, count_out);
	}

	int ptebp_set_record(uint32_t point_index, uint32_t record_index, const PTEBP_RECORD &rec)
	{
		return (int)bp_set_record_raw(OP_PTEBP_SET_RECORD, point_index, record_index,
		                              &rec, sizeof(rec));
	}

	int ptebp_clear_records(int32_t point_index)
	{
		return (int)bp_clear_records_raw(OP_PTEBP_CLEAR_RECORDS, point_index);
	}

	/* ---- BRK 软断点 BRKBP ---- */

	int brkbp_set(const BRKBP_POINT_CONFIG *pts, uint32_t count)
	{
		return (int)bp_set_raw(OP_BRKBP_SET, pts, count, sizeof(BRKBP_POINT_CONFIG));
	}

	int brkbp_exec(uint64_t addr)
	{
		BRKBP_POINT_CONFIG p = {addr, BP_TYPE_EXEC, BP_LEN_4};

		return brkbp_set(&p, 1);
	}

	int brkbp_remove() { return (int)xfer_impl(OP_BRKBP_REMOVE, 0); }

	int brkbp_get_config(BRKBP_CONFIG *out)
	{
		return out ? (int)xfer_impl(OP_BRKBP_GET_CONFIG, (uintptr_t)out) : -1;
	}

	int brkbp_get_records(uint32_t point_index, uint32_t start, BRKBP_RECORD *recs,
	                      uint32_t cap, uint32_t *count_out)
	{
		return (int)bp_get_records_raw(OP_BRKBP_GET_RECORDS, point_index, start,
		                               recs, cap, count_out);
	}

	int brkbp_set_record(uint32_t point_index, uint32_t record_index, const BRKBP_RECORD &rec)
	{
		return (int)bp_set_record_raw(OP_BRKBP_SET_RECORD, point_index, record_index,
		                              &rec, sizeof(rec));
	}

	int brkbp_clear_records(int32_t point_index)
	{
		return (int)bp_clear_records_raw(OP_BRKBP_CLEAR_RECORDS, point_index);
	}

	/* ---- ETM/PTM 分支追踪 ETMBP ---- */

	int etmbp_set(const ETMBP_POINT_CONFIG *pts, uint32_t count)
	{
		return (int)bp_set_raw(OP_ETMBP_SET, pts, count, sizeof(ETMBP_POINT_CONFIG));
	}

	int etmbp_trace(uint64_t start, uint64_t end, uint32_t flags = 0)
	{
		ETMBP_POINT_CONFIG p = {start, end, flags, 0};

		return etmbp_set(&p, 1);
	}

	int etmbp_remove() { return (int)xfer_impl(OP_ETMBP_REMOVE, 0); }

	int etmbp_get_config(ETMBP_CONFIG *out)
	{
		return out ? (int)xfer_impl(OP_ETMBP_GET_CONFIG, (uintptr_t)out) : -1;
	}

	int etmbp_get_records(uint32_t point_index, uint32_t start, ETMBP_RECORD *recs,
	                      uint32_t cap, uint32_t *count_out)
	{
		return (int)bp_get_records_raw(OP_ETMBP_GET_RECORDS, point_index, start,
		                               recs, cap, count_out);
	}

	int etmbp_set_record(uint32_t point_index, uint32_t record_index, const ETMBP_RECORD &rec)
	{
		return (int)bp_set_record_raw(OP_ETMBP_SET_RECORD, point_index, record_index,
		                              &rec, sizeof(rec));
	}

	int etmbp_clear_records(int32_t point_index)
	{
		return (int)bp_clear_records_raw(OP_ETMBP_CLEAR_RECORDS, point_index);
	}

	/* ---- UDP 断点 ---- */

	int udpbp_set(const UDPBP_POINT_CONFIG &point, int32_t *slot_out = NULL)
	{
		UDPBP_SET_REQUEST req;

		memset(&req, 0, sizeof(req));
		req.slot_index = -1;
		req.slot_out = slot_out;
		req.point = point;
		return (int)xfer_impl(OP_UDPBP_SET, (uintptr_t)&req);
	}

	int udpbp_update(int32_t slot, const UDPBP_POINT_CONFIG &point)
	{
		UDPBP_SET_REQUEST req;

		memset(&req, 0, sizeof(req));
		req.slot_index = slot;
		req.point = point;
		return (int)xfer_impl(OP_UDPBP_SET, (uintptr_t)&req);
	}

	int udpbp_remove(int32_t slot) { return (int)xfer_impl(OP_UDPBP_REMOVE, (uintptr_t)(intptr_t)slot); }

	int udpbp_get_config(UDPBP_CONFIG *cfg)
	{
		return cfg ? (int)xfer_impl(OP_UDPBP_GET_CONFIG, (uintptr_t)cfg) : -1;
	}

	int udpbp_get_records(int32_t slot, uint32_t start, UDPBP_RECORD *recs,
	                      uint32_t cap, uint32_t *count_out)
	{
		UDPBP_RECORDS_REQUEST req;

		if (!recs || cap == 0)
			return -1;
		memset(&req, 0, sizeof(req));
		req.slot_index = slot;
		req.start_index = start;
		req.capacity = cap;
		req.records = recs;
		req.count_out = count_out;
		return (int)xfer_impl(OP_UDPBP_GET_RECORDS, (uintptr_t)&req);
	}

	int udpbp_clear_records(int32_t slot)
	{
		UDPBP_CLEAR_REQUEST req;

		req.slot_index = slot;
		req.reserved = 0;
		return (int)xfer_impl(OP_UDPBP_CLEAR_RECORDS, (uintptr_t)&req);
	}

	int udpbp_log_port(uint16_t port, uint16_t dir, uint32_t target_pid = 0,
	                   int32_t *slot_out = NULL)
	{
		UDPBP_POINT_CONFIG p;

		memset(&p, 0, sizeof(p));
		p.pid = target_pid;
		p.port = port;
		p.dir = dir;
		p.action = UDPBP_ACT_LOG;
		return udpbp_set(p, slot_out);
	}

	int udpbp_rewrite_port(uint16_t port, uint16_t dir, uint32_t target_pid,
	                       const void *patch, uint16_t len, int32_t *slot_out = NULL)
	{
		UDPBP_POINT_CONFIG p;

		if (!patch || len == 0 || len > UDPBP_PATCH_MAX)
			return -1;
		memset(&p, 0, sizeof(p));
		p.pid = target_pid;
		p.port = port;
		p.dir = dir;
		p.action = UDPBP_ACT_REWRITE;
		p.patch_len = len;
		memcpy(p.patch, patch, len);
		return udpbp_set(p, slot_out);
	}
	
	class Frame
	{
	public:
		Frame() : owner_(NULL), gen_(1)
		{
			memset(tab_, 0, sizeof(tab_));
		}

		explicit Frame(c_wind *owner) : owner_(owner), gen_(1)
		{
			memset(tab_, 0, sizeof(tab_));
		}

		void attach(c_wind *owner) { owner_ = owner; }

		void begin()
		{
			if (++gen_ == 0)
			{
				gen_ = 1;
				memset(tab_, 0, sizeof(tab_));
			}
		}

		bool read(uintptr_t addr, uint64_t &out)
		{
			Slot &s = tab_[(addr >> 3) & (kSlots - 1)];

			if (s.gen == gen_ && s.addr == addr)
			{
				out = s.val;
				++hits_;
				return true;
			}
			if (!owner_ || !owner_->read(addr, &out, sizeof(out)))
				return false;
			s.addr = addr;
			s.val = out;
			s.gen = gen_;
			return true;
		}

		int hit_stats() const { return hits_; }
		void reset_stats() { hits_ = 0; }

	private:
		enum { kSlots = 2048 };
		struct Slot
		{
			uintptr_t addr;
			uint64_t val;
			uint32_t gen;
			uint32_t pad;
		};

		c_wind *owner_;
		Slot tab_[kSlots];
		uint32_t gen_;
		int hits_;
	};

	template <typename T>
	class Snapshot
	{
	public:
		Snapshot() : owner_(NULL), v_(0), ok_(false), addr_(0), hz_(60), run_(false) {}
		explicit Snapshot(c_wind *owner) : owner_(owner), v_(0), ok_(false), addr_(0), hz_(60), run_(false) {}
		~Snapshot() { stop(); }

		void attach(c_wind *owner) { owner_ = owner; }

		void start(uintptr_t addr, int hz = 60)
		{
			stop();
			addr_ = addr;
			hz_ = (hz < 1) ? 1 : (hz > 240 ? 240 : hz);
			run_.store(true);
			th_ = std::thread(&Snapshot::loop, this);
		}

		void start(c_wind *owner, uintptr_t addr, int hz = 60)
		{
			attach(owner);
			start(addr, hz);
		}

		void stop()
		{
			run_.store(false);
			if (th_.joinable())
				th_.join();
		}

		T value() const { return (T)v_.load(std::memory_order_relaxed); }
		bool fresh() const { return ok_.load(std::memory_order_relaxed); }

	private:
		void loop()
		{
			const long period_us = 1000000L / hz_;

			while (run_.load(std::memory_order_relaxed))
			{
				uint64_t tmp = 0;

				if (owner_ && owner_->read(addr_, &tmp, sizeof(tmp)))
				{
					v_.store(tmp, std::memory_order_relaxed);
					ok_.store(true, std::memory_order_relaxed);
				}
				usleep((useconds_t)period_us);
			}
		}

		c_wind *owner_;
		std::atomic<uint64_t> v_;
		std::atomic<bool> ok_;
		uintptr_t addr_;
		int hz_;
		std::thread th_;
		std::atomic<bool> run_;
	};

	static c_wind &instance(pid_t target = 0, int unit = WIND_NL_DEFAULT_UNIT)
	{
		static c_wind inst(unit);

		if (target > 0 && !inst.is_bound())
			inst.initialize(target);
		return inst;
	}

	void frame_begin() { frame_.begin(); }
	bool frame_read(uintptr_t addr, uint64_t &out) { return frame_.read(addr, out); }
	template <typename T>
	bool frame_read(uintptr_t addr, T &out)
	{
		uint64_t v = 0;

		if (!frame_.read(addr, v))
			return false;
		out = (T)v;
		return true;
	}
	int frame_hits() const { return frame_.hit_stats(); }
	void frame_reset_stats() { frame_.reset_stats(); }

	bool snap_start(uintptr_t addr, int hz = 60) { snap_.start(addr, hz); return true; }
	void snap_stop() { snap_.stop(); }
	uint64_t snap_u64() const { return (uint64_t)snap_.value(); }
	template <typename T>
	T snap_value() const { return (T)snap_.value(); }
	bool snap_fresh() const { return snap_.fresh(); }

private:
	Frame frame_;
	Snapshot<uint64_t> snap_;
};

/* 需要全局 driver 指针时, 在包含本头文件前定义这个宏 */
#ifdef WIND_ENABLE_LEGACY_GLOBAL_DRIVER
static c_wind *driver = &c_wind::instance();
#endif
