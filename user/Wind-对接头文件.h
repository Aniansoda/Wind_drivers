#ifndef WIND_DRIVER_USER_H
#define WIND_DRIVER_USER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <atomic>
#include <thread>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#ifndef WIND_NL_DEFAULT_UNIT
#define WIND_NL_DEFAULT_UNIT 29
#define WIND_NL_UNIT_MIN     29
#define WIND_NL_UNIT_MAX     35
#endif

#define WIND_OP_BASE 0x574E0000

#define WIND_NL_MAGIC 0x574E4C31u

#define WIND_OP_FIRST (WIND_OP_BASE + 0x01)
#define WIND_OP_LAST  (WIND_OP_BASE + 0x3F)

#define WIND_DEV_MISC  "/dev/windc"
#define WIND_DEV_NULL  "/dev/null"
#define WIND_DEV_LOCAL "/data/local/tmp/windc"

#define MMAP_PID_SHIFT 32
#define MMAP_OFF_MASK  0xFFFFFFFFULL

enum OPERATIONS
{
	OP_BIND          = WIND_OP_BASE + 0x01,
	OP_UNBIND        = WIND_OP_BASE + 0x02,
	OP_READ_MEM      = WIND_OP_BASE + 0x03,
	OP_WRITE_MEM     = WIND_OP_BASE + 0x04,
	OP_READ_BATCH    = WIND_OP_BASE + 0x05,
	OP_WRITE_BATCH   = WIND_OP_BASE + 0x06,
	OP_SET_MODE      = WIND_OP_BASE + 0x07,
	OP_MODULE_BASE   = WIND_OP_BASE + 0x08,
	OP_HIDE_FILE     = WIND_OP_BASE + 0x09,
	OP_HIDE_PROCESS  = WIND_OP_BASE + 0x0A,
	OP_UNHIDE_ALL    = WIND_OP_BASE + 0x0B,
	OP_KGSL_HIDE     = WIND_OP_BASE + 0x0E,
	OP_KGSL_HOOK     = WIND_OP_BASE + 0x0F,
	OP_GYRO_INSTALL  = WIND_OP_BASE + 0x10,
	OP_GYRO_REMOVE   = WIND_OP_BASE + 0x11,
	OP_GYRO_SET      = WIND_OP_BASE + 0x12,
	OP_GYRO_ENABLE   = WIND_OP_BASE + 0x13,

	OP_GYRO_STOP   = WIND_OP_BASE + 0x2C,
	OP_GYRO_RESUME = WIND_OP_BASE + 0x2D,

	OP_STATS = WIND_OP_BASE + 0x33,

	OP_CLONEBP_SET          = WIND_OP_BASE + 0x40,
	OP_CLONEBP_REMOVE       = WIND_OP_BASE + 0x41,
	OP_CLONEBP_GET_CONFIG   = WIND_OP_BASE + 0x42,
	OP_CLONEBP_GET_RECORDS  = WIND_OP_BASE + 0x43,
	OP_CLONEBP_SET_RECORD   = WIND_OP_BASE + 0x44,
	OP_CLONEBP_CLEAR_RECORDS= WIND_OP_BASE + 0x45,
	OP_CLONEBP_REFRESH      = WIND_OP_BASE + 0x46,
	OP_CLONEBP_TIMING       = WIND_OP_BASE + 0x47,

	OP_ACE_SET              = WIND_OP_BASE + 0x50,
	OP_ACE_GET_STAT         = WIND_OP_BASE + 0x51,
	OP_ACE_VERBOSE          = WIND_OP_BASE + 0x52,
	OP_ACE_INTE             = WIND_OP_BASE + 0x53,
	OP_ACE_SET_FEATURES     = WIND_OP_BASE + 0x54,
	OP_ACE_GET_FEATURES     = WIND_OP_BASE + 0x55,
};

enum READ_MODE
{
	MODE_FAST = 0,
	MODE_SAFE = 1,
};

typedef struct _WIND_NL_MSG
{
	uint32_t magic;
	uint32_t op;
	uint64_t arg;
	int64_t  result;
} WIND_NL_MSG, *PWIND_NL_MSG;

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
	char *name;
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
	BATCH_ITEM items[];
} BATCH_REQUEST, *PBATCH_REQUEST;

typedef struct _MODE_REQUEST
{
	uint32_t mode;
} MODE_REQUEST, *PMODE_REQUEST;

typedef struct _GYRO_REQUEST
{
	uint32_t rx, ry, rz;
} GYRO_REQUEST, *PGYRO_REQUEST;

typedef struct _WIND_MAP_STATS
{
	uint64_t reads;
	uint64_t hits;
	uint64_t misses;
	uint64_t evicts;
	uint64_t cached;
	uint64_t tr_hits;
	uint64_t tr_misses;
} WIND_MAP_STATS, *PWIND_MAP_STATS;

#define CLONEBP_MAX_POINTS   32
#define CLONEBP_RECORD_MAX   256
#define CLONEBP_MAX_PAGES    64
#define CLONEBP_PATCH_MAX    16
#define CLONEBP_REG_MAX      8
#define CLONEBP_FREG_MAX     8   /* 浮点槽位数: bit0..bit7 -> freg_base+0..7 */
#define CLONEBP_BRK_IMM_BASE 0x1000

#define CLONEBP_MODE_LOG     0
#define CLONEBP_MODE_REWRITE 1

#define CLONEBP_F_ONESHOT    0x1
#define CLONEBP_F_KEEP_ORIG  0x2
#define CLONEBP_F_NO_FLUSH   0x4

enum CLONEBP_STAT_IDX
{
	CLONEBP_ST_IFAULT_IN = 0,
	CLONEBP_ST_DFAULT_IN = 1,
	CLONEBP_ST_BRK_HIT   = 2,
	CLONEBP_ST_TLB_FLUSH = 3,
	CLONEBP_ST_CLONE_OK  = 4,
	CLONEBP_ST_CLONE_FAIL= 5,
	CLONEBP_ST_MAX       = 6,
};

typedef struct _CLONEBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t mode;
	uint32_t reg_base;
	uint32_t reg_mask;
	uint32_t flags;
	uint64_t reg_val[CLONEBP_REG_MAX];
	uint32_t imm;
	uint32_t patch_len;
	uint8_t  patch[CLONEBP_PATCH_MAX];

	/* ---- 浮点寄存器写回 (arm64 S0..S31) ----
	 * reg_base/reg_mask/reg_val 只能写 X0..X30。要改 float 参数
	 * (比如 UE4 里通过 S0..S7 传的 FVector / 弹道参数) 就得走这里。
	 *
	 *   S3 = vregs[3] 的低 32 位, 所以 freg_base=3 就是 S3。
	 *   freg_mask bit0..bit7 -> freg_base+0 .. freg_base+7。
	 *   freg_val[k] 是 float 的位模式 (不是 double, 高位补 0)。
	 *
	 * 写 S3/S4/S5 三个 float: freg_base=3, freg_mask=0x7,
	 *   freg_val[0..2] = {x,y,z} 的位模式。
	 */
	uint32_t freg_base;               /* 起始 S 寄存器号 0..31 */
	uint32_t freg_mask;               /* bit0..bit7 对应 freg_base+0..7, 0=不写浮点 */
	uint32_t freg_val[CLONEBP_FREG_MAX]; /* float 位模式 */
} CLONEBP_POINT_CONFIG, *PCLONEBP_POINT_CONFIG;

typedef struct _CLONEBP_SET_REQUEST
{
	uint32_t pid;
	uint32_t point_count;
	CLONEBP_POINT_CONFIG points[];
} CLONEBP_SET_REQUEST, *PCLONEBP_SET_REQUEST;

typedef struct _CLONEBP_RECORD
{
	uint64_t trigger_pc;
	uint64_t hit_addr;
	uint32_t hits;
	uint32_t reserved;
} CLONEBP_RECORD, *PCLONEBP_RECORD;

/* ---- 改单点配置 (OP_CLONEBP_SET_RECORD = 0x44) ----
 * 已经装好的点就地改寄存器写回值, 不用先拆再装。
 * point_index 是 clonebp_set 时的下标; point 里只有
 * reg_* / freg_* / mode / flags 会被采纳, addr / patch 忽略。
 */
typedef struct _CLONEBP_SET_RECORD_REQUEST
{
	uint32_t pid;                     /* 目标进程, 0 = 用已绑定的 mm */
	uint32_t point_index;             /* 第几个点 */
	CLONEBP_POINT_CONFIG point;       /* 新配置 */
} CLONEBP_SET_RECORD_REQUEST, *PCLONEBP_SET_RECORD_REQUEST;

typedef struct _CLONEBP_CONFIG
{
	uint32_t active_count;
	uint32_t shadow_pages;
	uint64_t faults_in_i;
	uint64_t faults_in_d;
	uint64_t brk_hits;
	uint64_t tlb_flushes;
	uint64_t clone_ok;
	uint64_t clone_fail;
	uint32_t reserved[2];
} CLONEBP_CONFIG, *PCLONEBP_CONFIG;

typedef struct _CLONEBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	CLONEBP_RECORD *records;
	uint32_t *count_out;
} CLONEBP_RECORDS_REQUEST, *PCLONEBP_RECORDS_REQUEST;

#define CLONEBP_TIMING_BALANCE  (1u << 0)
#define CLONEBP_TIMING_JITTER   (1u << 1)
#define CLONEBP_TIMING_TARGET_MAX 200000u
#define CLONEBP_TIMING_BAILOUT_MIN 1u
#define CLONEBP_TIMING_BAILOUT_MAX 4096u

typedef struct _CLONEBP_TIMING_CONFIG
{
	uint32_t mode;
	uint32_t target_ns;
	uint32_t jitter_ns;
	uint32_t bailout_max;
	uint64_t cost_ns;
	uint64_t pad_ns;
	uint64_t evt_cnt;
	uint32_t reserved[2];
} CLONEBP_TIMING_CONFIG, *PCLONEBP_TIMING_CONFIG;

#define ACE_F_MAPS     (1u << 0)
#define ACE_F_MODS     (1u << 1)
#define ACE_F_KSYM     (1u << 2)
#define ACE_F_TRACERP  (1u << 3)
#define ACE_F_FS       (1u << 4)
#define ACE_F_COMM     (1u << 5)
#define ACE_F_PORT     (1u << 6)
#define ACE_F_PROP     (1u << 7)
#define ACE_F_FD       (1u << 8)
#define ACE_F_SMAPS    (1u << 9)
#define ACE_F_MASK     0x3FFu
#define ACE_F_NONE     0u

typedef struct _ACE_STAT
{
	uint32_t tgid;
	uint32_t features;
	uint64_t maps_drop;
	uint64_t mod_drop;
	uint64_t ksym_drop;
	uint64_t tracerp_hide;
	uint64_t fs_drop;
	uint64_t comm_drop;
	uint64_t port_drop;
	uint64_t smaps_drop;
	uint64_t fd_drop;
	uint64_t prop_hit;
} ACE_STAT, *PACE_STAT;

class c_wind
{
public:

	int clonebp_set(const CLONEBP_SET_REQUEST *req) { return xfer(OP_CLONEBP_SET, (uintptr_t)req); }
	int clonebp_remove() { return xfer(OP_CLONEBP_REMOVE, 0); }
	int clonebp_get_config(CLONEBP_CONFIG *out) { return out ? xfer(OP_CLONEBP_GET_CONFIG, (uintptr_t)out) : -1; }
	int clonebp_get_records(uint32_t idx, uint32_t start, CLONEBP_RECORD *recs, uint32_t cap, uint32_t *cnt)
	{
		CLONEBP_RECORDS_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.point_index = idx; r.start_index = start; r.capacity = cap;
		r.records = recs; r.count_out = cnt;
		return xfer(OP_CLONEBP_GET_RECORDS, (uintptr_t)&r);
	}
	int clonebp_clear_records(int32_t idx) { int32_t a[2] = { idx, 0 }; return xfer(OP_CLONEBP_CLEAR_RECORDS, (uintptr_t)a); }
	int clonebp_refresh() { return xfer(OP_CLONEBP_REFRESH, 0); }

	int clonebp_set_timing(CLONEBP_TIMING_CONFIG *io) { return io ? xfer(OP_CLONEBP_TIMING, (uintptr_t)io) : -1; }


	int ace_set(uint32_t tgid) { return xfer(OP_ACE_SET, (uintptr_t)tgid); }
	int ace_get_stat(ACE_STAT *out) { return out ? xfer(OP_ACE_GET_STAT, (uintptr_t)out) : -1; }
	int ace_verbose(int on) { return xfer(OP_ACE_VERBOSE, (uintptr_t)on); }
	int ace_inte(int mode) { return xfer(OP_ACE_INTE, (uintptr_t)mode); }
	int ace_set_features(uint32_t mask) { return xfer(OP_ACE_SET_FEATURES, (uintptr_t)mask); }
	int ace_get_features(uint32_t *out) { return out ? xfer(OP_ACE_GET_FEATURES, (uintptr_t)out) : -1; }

	int ace_enable_feature(uint32_t bit, bool on)
	{
		uint32_t cur = 0;
		uintptr_t ret = xfer(OP_ACE_GET_FEATURES, (uintptr_t)&cur);
		if (ret < 0) return (int)ret;
		cur = on ? (cur | bit) : (cur & ~bit);
		return xfer(OP_ACE_SET_FEATURES, (uintptr_t)cur);
	}

private:
	int fd;
	int nl_fd;
	int nl_unit;
	uint32_t nl_seq;
	pid_t pid;
	bool bound;
	pthread_mutex_t mtx;

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
public:
	template <typename T>
	long xfer(unsigned int op, T arg)
	{
		return xfer_impl(op, (uintptr_t)arg);
	}

	explicit c_wind(int unit = WIND_NL_DEFAULT_UNIT)
	{
		fd = -1;
		nl_unit = -1;
		bound = false;
		nl_seq = 0;
		pid = 0;
		pthread_mutex_init(&mtx, NULL);
		frame_.attach(this);
		snap_.attach(this);

		if (unit <= 0)
		{
			for (int u = WIND_NL_UNIT_MIN; u <= WIND_NL_UNIT_MAX; ++u)
			{
				if (try_unit(u))
					break;
			}
			if (nl_fd < 0)
				printf("[-] netlink 探测 %d..%d 全部失败, 请确认已加载\n",
				       WIND_NL_UNIT_MIN, WIND_NL_UNIT_MAX);
			return;
		}
		if (!try_unit(unit))
			printf("[-] netlink unit=%d 不可用: %s\n", unit, strerror(errno));
	}

	bool try_unit(int unit)
	{
		struct sockaddr_nl sa;

		int fd = socket(AF_NETLINK, SOCK_RAW, unit);

		if (fd < 0)
			return false;
		memset(&sa, 0, sizeof(sa));
		sa.nl_family = AF_NETLINK;
		sa.nl_pid = (uint32_t)getpid();
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 &&
		    errno != EADDRINUSE)
		{
			close(fd);
			return false;
		}
		nl_fd = fd;
		nl_unit = unit;
		return true;
	}

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

	template <typename C>
	int read_batch(const C &items)
	{
		uint32_t n = (uint32_t)items.size();
		uintptr_t *a;
		void **b;
		size_t *s;
		int ret;

		if (n == 0 || n > 1024)
			return -1;
		a = (uintptr_t *)malloc(sizeof(uintptr_t) * n);
		b = (void **)malloc(sizeof(void *) * n);
		s = (size_t *)malloc(sizeof(size_t) * n);
		if (!a || !b || !s) {
			free(a); free(b); free(s);
			return -1;
		}
		for (uint32_t i = 0; i < n; ++i) {
			a[i] = items[i].addr;
			b[i] = items[i].buffer;
			s[i] = items[i].size;
		}
		ret = read_batch(a, b, s, n);
		free(a); free(b); free(s);
		return ret;
	}

	template <typename C>
	int write_batch(const C &items)
	{
		uint32_t n = (uint32_t)items.size();
		uintptr_t *a;
		void **b;
		size_t *s;
		int ret;

		if (n == 0 || n > 1024)
			return -1;
		a = (uintptr_t *)malloc(sizeof(uintptr_t) * n);
		b = (void **)malloc(sizeof(void *) * n);
		s = (size_t *)malloc(sizeof(size_t) * n);
		if (!a || !b || !s) {
			free(a); free(b); free(s);
			return -1;
		}
		for (uint32_t i = 0; i < n; ++i) {
			a[i] = items[i].addr;
			b[i] = items[i].buffer;
			s[i] = items[i].size;
		}
		ret = write_batch(a, b, s, n);
		free(a); free(b); free(s);
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

	int hide_file(const char *path) { return (int)xfer_impl(OP_HIDE_FILE, (uintptr_t)path); }
	int hide_process(pid_t target) { return (int)xfer_impl(OP_HIDE_PROCESS, (uintptr_t)target); }
	int unhide_all() { return (int)xfer_impl(OP_UNHIDE_ALL, 0); }

	int kgsl_hide(pid_t target) { return (int)xfer_impl(OP_KGSL_HIDE, (uintptr_t)target); }
	int kgsl_hook() { return (int)xfer_impl(OP_KGSL_HOOK, 0); }

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

#ifdef WIND_ENABLE_LEGACY_GLOBAL_DRIVER
static c_wind *driver = &c_wind::instance();
#endif

#endif
