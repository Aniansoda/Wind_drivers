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

#define UDPBP_MAX_SLOTS   8
#define UDPBP_PATCH_MAX   64
#define UDPBP_PREVIEW_MAX 32
#define UDPBP_RECORD_MAX  256

#define WIND_BP_MAX_POINTS     16

#define WIND_BP_RECORDS_MAX    4096
#define WIND_WRPBP_RECORDS_MAX 256

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
	OP_BRK_SET       = WIND_OP_BASE + 0x0C, /* arg = BRK_REQUEST*，老接口，等价 BRKBP 单点安装 */
	OP_BRK_CLEAR     = WIND_OP_BASE + 0x0D, /* arg = BRK_REQUEST*，按 addr 摘除对应点 */
	OP_KGSL_HIDE     = WIND_OP_BASE + 0x0E,
	OP_KGSL_HOOK     = WIND_OP_BASE + 0x0F,
	OP_GYRO_INSTALL  = WIND_OP_BASE + 0x10,
	OP_GYRO_REMOVE   = WIND_OP_BASE + 0x11,
	OP_GYRO_SET      = WIND_OP_BASE + 0x12,
	OP_GYRO_ENABLE   = WIND_OP_BASE + 0x13,

	OP_HWBP_SET           = WIND_OP_BASE + 0x14,
	OP_HWBP_REMOVE        = WIND_OP_BASE + 0x15,
	OP_HWBP_GET_CONFIG    = WIND_OP_BASE + 0x16,
	OP_HWBP_GET_RECORDS   = WIND_OP_BASE + 0x17,
	OP_HWBP_SET_RECORD    = WIND_OP_BASE + 0x18,
	OP_HWBP_CLEAR_RECORDS = WIND_OP_BASE + 0x19,

	OP_PTEBP_SET           = WIND_OP_BASE + 0x1A,
	OP_PTEBP_REMOVE        = WIND_OP_BASE + 0x1B,
	OP_PTEBP_GET_CONFIG    = WIND_OP_BASE + 0x1C,
	OP_PTEBP_GET_RECORDS   = WIND_OP_BASE + 0x1D,
	OP_PTEBP_SET_RECORD    = WIND_OP_BASE + 0x1E,
	OP_PTEBP_CLEAR_RECORDS = WIND_OP_BASE + 0x1F,

	OP_BRKBP_SET           = WIND_OP_BASE + 0x20,
	OP_BRKBP_REMOVE        = WIND_OP_BASE + 0x21,
	OP_BRKBP_GET_CONFIG    = WIND_OP_BASE + 0x22,
	OP_BRKBP_GET_RECORDS   = WIND_OP_BASE + 0x23,
	OP_BRKBP_SET_RECORD    = WIND_OP_BASE + 0x24,
	OP_BRKBP_CLEAR_RECORDS = WIND_OP_BASE + 0x25,

	OP_WRPBP_SET           = WIND_OP_BASE + 0x26,
	OP_WRPBP_REMOVE        = WIND_OP_BASE + 0x27,
	OP_WRPBP_GET_CONFIG    = WIND_OP_BASE + 0x28,
	OP_WRPBP_GET_RECORDS   = WIND_OP_BASE + 0x29,
	OP_WRPBP_SET_VEC       = WIND_OP_BASE + 0x2A,
	OP_WRPBP_CLEAR_RECORDS = WIND_OP_BASE + 0x2B,

	OP_GYRO_STOP   = WIND_OP_BASE + 0x2C,
	OP_GYRO_RESUME = WIND_OP_BASE + 0x2D,

	OP_UDPBP_SET           = WIND_OP_BASE + 0x2E,
	OP_UDPBP_REMOVE        = WIND_OP_BASE + 0x2F,
	OP_UDPBP_GET_CONFIG    = WIND_OP_BASE + 0x30,
	OP_UDPBP_GET_RECORDS   = WIND_OP_BASE + 0x31,
	OP_UDPBP_CLEAR_RECORDS = WIND_OP_BASE + 0x32,

	OP_STATS = WIND_OP_BASE + 0x33,

	OP_HWBP_SET_REGS = WIND_OP_BASE + 0x34,

	OP_PTEBP_SET_REGS = WIND_OP_BASE + 0x35,

	OP_PTEBP_REFRESH = WIND_OP_BASE + 0x36,

	OP_HWBP_REFRESH = WIND_OP_BASE + 0x37,

	OP_BP_DIAG = WIND_OP_BASE + 0x3E,

	OP_BP_ALL_OFF = WIND_OP_BASE + 0x3F,

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

/* ---- 老接口 BRK 软断点（0x0C/0x0D）----
 * 只保留单点安装/摘除，等价 BRKBP 家族的单点形式。
 * 新代码优先用 brkbp_* 系列；这两个为兼容旧工具保留。
 */
typedef struct _BRK_REQUEST
{
	pid_t pid;
	uintptr_t addr;
	uint32_t type;	/* HW_BREAKPOINT_X / _R / _W / _RW */
} BRK_REQUEST, *PBRK_REQUEST;

typedef struct _HWBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;
	uint32_t length;
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
	uint32_t op_mask;
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
	int32_t point_index;
	uint32_t reserved;
} HWBP_CLEAR_REQUEST, *PHWBP_CLEAR_REQUEST;

typedef struct _HWBP_REG_REQUEST
{
	int32_t  point_index;
	uint32_t enable;
	uint32_t rx, ry, rz;
	uint32_t reserved;
} HWBP_REG_REQUEST, *PHWBP_REG_REQUEST;

typedef struct _PTEBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;
	uint32_t length;
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
	uint32_t type;
	uint32_t length;
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

typedef struct _WRPBP_POINT_CONFIG
{
	uint64_t watch_addr;
	uint32_t watch_len;
	uint32_t type;
	uint32_t components;
	uint32_t reserved;
} WRPBP_POINT_CONFIG, *PWRPBP_POINT_CONFIG;

typedef struct _WRPBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	WRPBP_POINT_CONFIG points[];
} WRPBP_SET_REQUEST, *PWRPBP_SET_REQUEST;

typedef struct _WRPBP_VEC_REQUEST
{
	int32_t  point_index;
	uint32_t enable;
	uint32_t x, y, z;
	uint32_t mask;
} WRPBP_VEC_REQUEST, *PWRPBP_VEC_REQUEST;

typedef struct _WRPBP_CONFIG
{
	uint32_t active_count;
	uint32_t watch_addr_lo;
	uint32_t watch_addr_hi;
	uint32_t reserved;
} WRPBP_CONFIG, *PWRPBP_CONFIG;

typedef struct _WIND_BP_DIAG
{
	uint64_t last_hit_pc;
	uint32_t hwbp_armed;
	uint32_t hwbp_hits;
	uint32_t hwbp_writes;
	uint32_t hwbp_write_fails;
	uint32_t ptebp_armed;
	uint32_t ptebp_reloads;
	uint32_t ptebp_heal_ok;
	uint32_t ptebp_heal_fail;
	uint32_t wrpbp_armed;
	uint32_t wrpbp_hits;
	uint32_t wrpbp_writes;
	uint32_t wrpbp_skips;
	uint32_t wrpbp_fails;
	uint32_t last_rx;
	uint32_t last_ry;
	uint32_t last_rz;
	uint32_t reserved;
} WIND_BP_DIAG, *PWIND_BP_DIAG;

typedef struct _GYRO_REQUEST
{
	uint32_t rx, ry, rz;
} GYRO_REQUEST, *PGYRO_REQUEST;

typedef struct _UDPBP_POINT_CONFIG
{
	uint32_t pid;
	uint16_t port;
	uint16_t dir;
	uint16_t action;
	uint16_t patch_len;
	uint8_t  patch[UDPBP_PATCH_MAX];
} UDPBP_POINT_CONFIG, *PUDPBP_POINT_CONFIG;

typedef struct _UDPBP_RECORD
{
	uint64_t ts_ns;
	uint32_t pid;
	uint32_t saddr;
	uint32_t daddr;
	uint16_t sport;
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

	int brk_set(const BRK_REQUEST *req) { return req ? xfer(OP_BRK_SET, (uintptr_t)req) : -1; }
	int brk_clear(const BRK_REQUEST *req) { return req ? xfer(OP_BRK_CLEAR, (uintptr_t)req) : -1; }

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

public:

	int hwbp_refresh() { return xfer(OP_HWBP_REFRESH, 0); }

	int hwbp_set_regs(int32_t point_index, bool enable, float x, float y, float z, uint32_t mask = 0x7)
	{
		HWBP_REG_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.enable = enable ? 1u : 0u;
		memcpy(&r.rx, &x, 4);
		memcpy(&r.ry, &y, 4);
		memcpy(&r.rz, &z, 4);
		r.reserved = mask;

		return xfer(OP_HWBP_SET_REGS, (uintptr_t)&r);
	}

	int ptebp_set_regs(int32_t point_index, bool enable, float x, float y, float z, uint32_t mask = 0x7)
	{
		HWBP_REG_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.enable = enable ? 1u : 0u;
		memcpy(&r.rx, &x, 4);
		memcpy(&r.ry, &y, 4);
		memcpy(&r.rz, &z, 4);
		r.reserved = mask;

		return xfer(OP_PTEBP_SET_REGS, (uintptr_t)&r);
	}
	int ptebp_refresh() { return xfer(OP_PTEBP_REFRESH, 0); }

	int wrpbp_set(const WRPBP_POINT_CONFIG *pts, uint32_t count)
	{
		if (!pts || count == 0)
			return -1;
		return bp_set_raw(OP_WRPBP_SET, pts, count, sizeof(WRPBP_POINT_CONFIG));
	}
	int wrpbp_remove() { return xfer(OP_WRPBP_REMOVE, 0); }
	int wrpbp_get_config(WRPBP_CONFIG *out)
	{
		return out ? xfer(OP_WRPBP_GET_CONFIG, (uintptr_t)out) : -1;
	}
	int wrpbp_get_records(uint32_t point_index, uint32_t start, HWBP_RECORD *recs,
	                      uint32_t cap, uint32_t *count_out)
	{
		return bp_get_records_raw(OP_WRPBP_GET_RECORDS, point_index, start,
		                          recs, cap, count_out);
	}
	int wrpbp_clear_records(int32_t point_index)
	{
		return bp_clear_records_raw(OP_WRPBP_CLEAR_RECORDS, point_index);
	}

	int wrpbp_set_vec(int32_t point_index, bool enable, float x, float y, float z, uint32_t mask = 0x7)
	{
		WRPBP_VEC_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.enable = enable ? 1u : 0u;
		memcpy(&r.x, &x, 4);
		memcpy(&r.y, &y, 4);
		memcpy(&r.z, &z, 4);
		r.mask = mask;

		return xfer(OP_WRPBP_SET_VEC, (uintptr_t)&r);
	}

	int bp_all_off() { return xfer(OP_BP_ALL_OFF, 0); }
	int bp_diag(WIND_BP_DIAG *out)
	{
		return out ? xfer(OP_BP_DIAG, (uintptr_t)out) : -1;
	}
};

#ifdef WIND_ENABLE_LEGACY_GLOBAL_DRIVER
static c_wind *driver = &c_wind::instance();
#endif

#endif
