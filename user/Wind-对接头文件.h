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
#define WIND_OP_LAST  (WIND_OP_BASE + 0xFF)

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

	OP_WRPBP_SET          = WIND_OP_BASE + 0x26,
	OP_WRPBP_REMOVE       = WIND_OP_BASE + 0x27,
	OP_WRPBP_GET_CONFIG   = WIND_OP_BASE + 0x28,
	OP_WRPBP_GET_RECORDS  = WIND_OP_BASE + 0x29,
	OP_WRPBP_SET_VEC      = WIND_OP_BASE + 0x2A,
	OP_WRPBP_CLEAR_RECORDS= WIND_OP_BASE + 0x2B,

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
	OP_ACE_QUERY            = WIND_OP_BASE + 0x51,
	OP_WRPBP_SET_FREGS      = WIND_OP_BASE + 0x56,
	OP_PAGE_HOOK            = WIND_OP_BASE + 0x57,
	OP_HOOK_VIEW            = WIND_OP_BASE + 0x58,
	OP_PAGEHOOK_STAT        = WIND_OP_BASE + 0x59,
	OP_MEM_ALLOC            = WIND_OP_BASE + 0x5A,
	OP_MEM_FREE             = WIND_OP_BASE + 0x5B,
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
#define CLONEBP_FREG_MAX     8
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
	uint32_t freg_base;
	uint32_t freg_mask;
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

typedef struct _CLONEBP_SET_RECORD_REQUEST
{
	uint32_t pid;
	uint32_t point_index;
	CLONEBP_POINT_CONFIG point;
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

#define WRPBP_MAX_POINTS    2
#define WRPBP_RECORD_MAX    256
#define WRPBP_VEC_BYTES     12
#define HWBP_MASK_ALL       0x7U

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

typedef struct _HWBP_RECORD
{
	uint64_t trigger_pc;
	uint64_t hit_addr;
	uint32_t op_mask;
	uint32_t reserved;
} HWBP_RECORD, *PHWBP_RECORD;

typedef struct _WRPBP_POINT_CONFIG
{
	uint64_t watch_addr;
	uint32_t watch_len;
	uint32_t type;
	uint32_t components;
	uint32_t freg_base;
	uint32_t freg_mask;
} WRPBP_POINT_CONFIG, *PWRPBP_POINT_CONFIG;

typedef struct _WRPBP_SET_REQUEST
{
	uint32_t pid;
	uint32_t point_count;
	WRPBP_POINT_CONFIG points[];
} WRPBP_SET_REQUEST, *PWRPBP_SET_REQUEST;

typedef struct _WRPBP_VEC_REQUEST
{
	int32_t  point_index;
	uint32_t enable;
	uint32_t x, y, z;
	uint32_t mask;
} WRPBP_VEC_REQUEST, *PWRPBP_VEC_REQUEST;

typedef struct _WRPBP_FREGS_REQUEST
{
	int32_t  point_index;
	uint32_t freg_base;
	uint32_t freg_mask;
	uint32_t freg_val[CLONEBP_FREG_MAX];
} WRPBP_FREGS_REQUEST, *PWRPBP_FREGS_REQUEST;

typedef struct _WRPBP_CONFIG
{
	uint32_t active_count;
	uint32_t watch_addr_lo;
	uint32_t watch_addr_hi;
	uint32_t skipped;
	uint32_t writes;
	uint32_t fails;
	uint32_t reserved;
	uint32_t reserved2;
} WRPBP_CONFIG, *PWRPBP_CONFIG;

typedef struct _WRPBP_RECORDS_REQUEST
{
	uint32_t point_index;
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	HWBP_RECORD *records;
	uint32_t *count_out;
} WRPBP_RECORDS_REQUEST, *PWRPBP_RECORDS_REQUEST;

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

typedef struct _ACE_SET_REQUEST
{
	uint32_t tgid;
	uint32_t features;
	uint32_t verbose;
	uint32_t inte;
} ACE_SET_REQUEST, *PACE_SET_REQUEST;

#define ACE_QUERY_STAT       1
#define ACE_QUERY_FEATURES   2
#define ACE_QUERY_PKGNAME    3
#define ACE_PKGNAME_MAX      256

typedef struct _ACE_QUERY_REQUEST
{
	uint32_t cmd;
	uint32_t pid;
	uint32_t reserved0;
	uint32_t reserved1;
	ACE_STAT stat;
	uint32_t features;
	char     pkgname[ACE_PKGNAME_MAX];
} ACE_QUERY_REQUEST, *PACE_QUERY_REQUEST;

#define PAGE_HOOK_REMOVE     0U
#define PAGE_HOOK_INSTALL    1U
#define PAGE_HOOK_PATCH_MAX  256U
#define PAGE_HOOK_VIEW_DATA  0U

typedef struct _PAGE_HOOK_REQUEST
{
	int32_t  pid;
	uint32_t action;
	uint64_t address;
	uint64_t patch_buffer;
	uint32_t patch_size;
	uint32_t flags;
} PAGE_HOOK_REQUEST;

typedef struct _HOOK_VIEW_REQUEST
{
	int32_t  pid;
	uint32_t view;
	uint64_t address;
} HOOK_VIEW_REQUEST;

#define PAGEHOOK_STAT_MAX_PAGES 16

typedef struct _PAGEHOOK_PAGE_INFO
{
	uint64_t va;
	uint32_t state;
	uint32_t pad;
	uint64_t ifault;
	uint64_t dfault;
} PAGEHOOK_PAGE_INFO;

typedef struct _PAGEHOOK_STAT
{
	uint32_t page_cnt;
	uint32_t page_cnt_used;
	uint64_t ifault;
	uint64_t dfault;
	uint64_t switch_cnt;
	uint64_t view_orig;
	PAGEHOOK_PAGE_INFO pages[PAGEHOOK_STAT_MAX_PAGES];
} PAGEHOOK_STAT;

typedef struct _SHAREMEM_REQUEST
{
	int32_t  pid;
	uint32_t protection;
	uint64_t size;
	uint64_t target_address;
	uint64_t caller_address;
	uint64_t allocated_size;
} SHAREMEM_REQUEST;

/* 与内核 comm.h 对拍: 结构对不上会静默读错字段, 用编译期断言挡住。
 * 内核侧 WRPBP_CONFIG 已扩到 32 字节(加 skipped/writes/fails),
 * 老客户端只开到 16 字节时, copy_to_user 会写穿用户栈。 */
static_assert(sizeof(WRPBP_POINT_CONFIG) == 32, "WRPBP_POINT_CONFIG 必须 32 字节");
static_assert(sizeof(WRPBP_CONFIG) == 32, "WRPBP_CONFIG 必须 32 字节");
static_assert(sizeof(WRPBP_VEC_REQUEST) == 24, "WRPBP_VEC_REQUEST 必须 24 字节");
static_assert(sizeof(PAGE_HOOK_REQUEST) == 32, "PAGE_HOOK_REQUEST 必须 32 字节");
static_assert(sizeof(HOOK_VIEW_REQUEST) == 16, "HOOK_VIEW_REQUEST 必须 16 字节");
static_assert(sizeof(SHAREMEM_REQUEST) == 40, "SHAREMEM_REQUEST 必须 40 字节");

/*
 * c_wind —— Wind 驱动用户态封装（净荷零依赖，只需 C++11 与 linux/netlink.h）
 *
 * 通道
 *   netlink AF_NETLINK unit 29（默认，范围 29..35），需 bind nl_pid=getpid()
 *   WIND_NL_MSG{magic, op, arg, result} 塞在 nlmsghdr 之后
 *   所有 op 高 16 位固定 0x574E（"WN"）
 *
 * 能力分组
 *   会话       initialize / unbind / is_bound / target_pid
 *   内存       read / write / read_batch / write_batch / module_base / set_mode
 *   隐藏       hide_file / hide_process / unhide_all / kgsl_hook / kgsl_hide
 *   陀螺仪     gyro_install / gyro_remove / gyro_set / gyro_enable / gyro_stop
 *   克隆页断点 clonebp_set / clonebp_remove / clonebp_get_config / clonebp_get_records
 *   ACE 伪装   ace_set / ace_verbose / ace_inte / ace_set_features /
 *              ace_get_features / ace_get_stat / ace_get_pkgname
 *   页 hook    page_hook / page_unhook / page_set_view_data / page_hook_stat
 *   共享页     share_alloc / share_free
 *   WRP 观察点 wrpbp_set / wrpbp_set_one / wrpbp_set_vec / wrpbp_set_fregs /
 *              wrpbp_get_config / wrpbp_get_records / wrpbp_skip_active
 *
 * ACE 两个 opcode 覆盖原来六个
 *   OP_ACE_SET   (0x50) ACE_SET_REQUEST{tgid,features,verbose,inte}
 *                      0 / 0xFFFFFFFF / 2 / 2 表示该字段不改
 *   OP_ACE_QUERY (0x51) ACE_QUERY_REQUEST{cmd,pid,...}
 *                      cmd=1 取 ACE_STAT，cmd=2 取 features，cmd=3 取 pkgname
 *
 * WRP 观察点（0x26..0x2B / 0x56）
 *   OP_WRPBP_SET (0x26) WRPBP_SET_REQUEST{pid, point_count, points[]}
 *                       pid 由 wrpbp_set() 自动填当前绑定 pid，
 *                       手拼请求漏填 pid 会被内核当成 find_task_by_vpid(0)
 *                       返回 NULL，表现成"绑定了但追踪点没下发"
 *   OP_WRPBP_SET_VEC (0x2A) mask bit0=x bit1=y bit2=z，默认 0x7 三位全写
 *   卡写语义: arm64 的 WRP 在 store 提交前 trap。内核写回目标地址后必须
 *   推进 pc 跳过那条 store（wind_wrp_skip_store=1），否则 CPU 重试会覆盖
 *   写回值，并再次命中同一观察点，把线程卡死在这条指令上——后续 netlink
 *   命令（含 bind）就再也发不出去。用户态用 wrpbp_skip_active() 或
 *   WRPBP_CONFIG.skipped 确认跳过真的在发生。
 *
 * 页级执行补丁 hook（对齐 Satan HOOK_MEMORY / SET_HOOK_VIEW）
 *   OP_PAGE_HOOK      (0x57) PAGE_HOOK_REQUEST{pid,action,address,patch_buffer,patch_size,flags}
 *   OP_HOOK_VIEW      (0x58) HOOK_VIEW_REQUEST{pid,view,address}，view 只支持 0=DATA
 *   OP_PAGEHOOK_STAT  (0x59) PAGEHOOK_STAT
 *   同 VA 双物理页：原始页用户可读不可执行，影子页可执行；取指走影子、读取走原始
 *
 * 跨进程共享物理页（对齐 Satan ALLOCATE_MEMORY）
 *   OP_MEM_ALLOC (0x5A) SHAREMEM_REQUEST，返回 target_address 与 caller_address
 *   OP_MEM_FREE  (0x5B)
 *
 * 注意
 *   sizeof(c_wind) 约 48 KB，其中 47 KB 是帧缓存表（2048 槽），
 *   局部实例建议 new 到堆上，避免爆栈。
 */
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


	int ace_set(uint32_t tgid) { ACE_SET_REQUEST r; memset(&r,0,sizeof(r)); r.tgid=tgid; r.features=0xFFFFFFFFu; r.verbose=2; r.inte=2; return xfer(OP_ACE_SET,(uintptr_t)&r); }
	int ace_verbose(int on) { ACE_SET_REQUEST r; memset(&r,0,sizeof(r)); r.tgid=0; r.features=0xFFFFFFFFu; r.verbose=on; r.inte=2; return xfer(OP_ACE_SET,(uintptr_t)&r); }
	int ace_inte(int mode) { ACE_SET_REQUEST r; memset(&r,0,sizeof(r)); r.tgid=0; r.features=0xFFFFFFFFu; r.verbose=2; r.inte=mode; return xfer(OP_ACE_SET,(uintptr_t)&r); }
	int ace_set_features(uint32_t mask) { ACE_SET_REQUEST r; memset(&r,0,sizeof(r)); r.tgid=0; r.features=mask; r.verbose=2; r.inte=2; return xfer(OP_ACE_SET,(uintptr_t)&r); }

	int ace_get_stat(ACE_STAT *out) { if(!out) return -1; ACE_QUERY_REQUEST r; memset(&r,0,sizeof(r)); r.cmd=ACE_QUERY_STAT; long ret=xfer(OP_ACE_QUERY,(uintptr_t)&r); if(ret==0) *out=r.stat; return (int)ret; }
	int ace_get_features(uint32_t *out) { if(!out) return -1; ACE_QUERY_REQUEST r; memset(&r,0,sizeof(r)); r.cmd=ACE_QUERY_FEATURES; long ret=xfer(OP_ACE_QUERY,(uintptr_t)&r); if(ret==0) *out=r.features; return (int)ret; }
	int ace_get_pkgname(uint32_t pid, char *out, uint32_t cap) { if(!out||!cap) return -1; ACE_QUERY_REQUEST r; memset(&r,0,sizeof(r)); r.cmd=ACE_QUERY_PKGNAME; r.pid=pid; long ret=xfer(OP_ACE_QUERY,(uintptr_t)&r); if(ret==0) { snprintf(out,cap,"%s",r.pkgname); } return (int)ret; }

	/* ---- 页级执行补丁 hook ---- */
	int page_hook(uintptr_t address, const void *patch, uint32_t size)
	{
		PAGE_HOOK_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.pid = 0;
		r.action = PAGE_HOOK_INSTALL;
		r.address = (uint64_t)address;
		r.patch_buffer = (uint64_t)(uintptr_t)patch;
		r.patch_size = size;
		r.flags = 0;
		return (int)xfer(OP_PAGE_HOOK, (uintptr_t)&r);
	}

	int page_unhook(uintptr_t address)
	{
		PAGE_HOOK_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.action = PAGE_HOOK_REMOVE;
		r.address = (uint64_t)address;
		return (int)xfer(OP_PAGE_HOOK, (uintptr_t)&r);
	}

	int page_set_view_data(uintptr_t address)
	{
		HOOK_VIEW_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.view = PAGE_HOOK_VIEW_DATA;
		r.address = (uint64_t)address;
		return (int)xfer(OP_HOOK_VIEW, (uintptr_t)&r);
	}

	int page_hook_stat(PAGEHOOK_STAT *out)
	{
		if (!out) return -1;
		return (int)xfer(OP_PAGEHOOK_STAT, (uintptr_t)out);
	}

	/* ---- 跨进程共享物理页 ----
	 * 返回当前进程可直接访问的地址, target 侧地址写进 target_out。
	 */
	uintptr_t share_alloc(size_t size, uint32_t prot,
			      uintptr_t *target_out = nullptr,
			      size_t *allocated_out = nullptr)
	{
		SHAREMEM_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.size = (uint64_t)size;
		r.protection = prot;
		if (xfer(OP_MEM_ALLOC, (uintptr_t)&r) != 0)
			return 0;
		if (target_out)    *target_out = (uintptr_t)r.target_address;
		if (allocated_out) *allocated_out = (size_t)r.allocated_size;
		return (uintptr_t)r.caller_address;
	}

	int share_free(uintptr_t target_address)
	{
		SHAREMEM_REQUEST r;
		memset(&r, 0, sizeof(r));
		r.target_address = (uint64_t)target_address;
		return (int)xfer(OP_MEM_FREE, (uintptr_t)&r);
	}

	int ace_enable_feature(uint32_t bit, bool on)
	{
		uint32_t cur = 0;
		if (ace_get_features(&cur) < 0) return -1;
		cur = on ? (cur | bit) : (cur & ~bit);
		return ace_set_features(cur);
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
				printf("[ - ] netlink 探测 %d..%d 全部失败, 请确认已加载\n",
				       WIND_NL_UNIT_MIN, WIND_NL_UNIT_MAX);
			return;
		}
		if (!try_unit(unit))
			printf("[ - ] netlink unit=%d 不可用: %s\n", unit, strerror(errno));
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
			printf("[ - ] open %s 失败: %s\n", dev, strerror(errno));
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
			printf("[ - ] bind pid %d 失败\n", target);
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

	/* ---- WRP 观察点 ----
	 * pid 一律从当前绑定会话带下去, 不用调用方再传。
	 * 以前要手工拼 WRPBP_SET_REQUEST 且自己填 pid, 忘记填就是 pid=0,
	 * 内核 find_task_by_vpid(0) 返回 NULL -> -ESRCH,
	 * 表现成"绑定了但追踪点没下发"。
	 */
	int wrpbp_set(const WRPBP_POINT_CONFIG *points, uint32_t count)
	{
		if (!points || !count || !bound || pid <= 0)
			return -1;

		size_t need = sizeof(WRPBP_SET_REQUEST) +
			      (size_t)count * sizeof(WRPBP_POINT_CONFIG);
		WRPBP_SET_REQUEST *req = (WRPBP_SET_REQUEST *)calloc(1, need);
		int ret;

		if (!req)
			return -1;
		req->pid = (uint32_t)pid;      /* 关键: 绑定 pid 随请求下发 */
		req->point_count = count;
		memcpy(req->points, points, (size_t)count * sizeof(*points));
		ret = (int)xfer_impl(OP_WRPBP_SET, (uintptr_t)req);
		free(req);
		return ret;
	}

	/* 单点便捷版: 装一个观察点。
	 * type 取 enum BP_TYPE: WRITE=3 只抓写, RW=4 读写都抓。
	 * 向量追踪一律用 WRITE —— 只抓游戏自己的写入, 少一半异常,
	 * 也不会把我们自己的回读当成命中。 */
	int wrpbp_set_one(uint64_t watch_addr, uint32_t watch_len = 4,
			  uint32_t type = BP_TYPE_WRITE)
	{
		WRPBP_POINT_CONFIG pt;

		memset(&pt, 0, sizeof(pt));
		pt.watch_addr = watch_addr;
		pt.watch_len = watch_len;
		pt.type = type;
		pt.components = HWBP_MASK_ALL;
		return wrpbp_set(&pt, 1);
	}

	int wrpbp_remove() { return (int)xfer_impl(OP_WRPBP_REMOVE, 0); }

	int wrpbp_get_config(WRPBP_CONFIG *out)
	{
		return out ? (int)xfer_impl(OP_WRPBP_GET_CONFIG, (uintptr_t)out) : -1;
	}

	/* 观察点写回: mask bit0=x bit1=y bit2=z。
	 * 默认 0x7 三位全写 —— 向量追踪只改 x/y 会让 z 每帧都像"漂移",
	 * 上层判 dirty 时又会因为 z 变化反复重发, 两边互相踢。
	 * 参数直接收 float, 内部转位模式, 省得调用方自己 memcpy。 */
	int wrpbp_set_vec(int32_t point_index, uint32_t enable,
			  float x, float y, float z, uint32_t mask = HWBP_MASK_ALL)
	{
		WRPBP_VEC_REQUEST r;

		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.enable = enable;
		memcpy(&r.x, &x, sizeof(r.x));
		memcpy(&r.y, &y, sizeof(r.y));
		memcpy(&r.z, &z, sizeof(r.z));
		r.mask = mask;
		return (int)xfer_impl(OP_WRPBP_SET_VEC, (uintptr_t)&r);
	}

	/* 等价的 float 重载, 给已经抱着 float 的调用方 */
	int wrpbp_set_vec_f(int32_t point_index, uint32_t enable,
			    float x, float y, float z,
			    uint32_t mask = HWBP_MASK_ALL)
	{
		return wrpbp_set_vec(point_index, enable, x, y, z, mask);
	}

	/* 位模式版: 调用方手里是 u32 位模式时用这个 */
	int wrpbp_set_vec_bits(int32_t point_index, uint32_t enable,
			       uint32_t xb, uint32_t yb, uint32_t zb,
			       uint32_t mask = HWBP_MASK_ALL)
	{
		WRPBP_VEC_REQUEST r;

		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.enable = enable;
		r.x = xb; r.y = yb; r.z = zb;
		r.mask = mask;
		return (int)xfer_impl(OP_WRPBP_SET_VEC, (uintptr_t)&r);
	}

	/* 停止写回, 观察点保留(页表/调试寄存器都不动) */
	int wrpbp_pause()
	{
		return wrpbp_set_vec(-1, 0, 0.0f, 0.0f, 0.0f, HWBP_MASK_ALL);
	}

	/* 浮点寄存器通道: 命中时改 S[freg_base..+n] 而不是内存 */
	int wrpbp_set_fregs(int32_t point_index, uint32_t freg_base,
			    uint32_t freg_mask, const float *vals, uint32_t count)
	{
		WRPBP_FREGS_REQUEST r;
		uint32_t k, n = count;

		if (n > CLONEBP_FREG_MAX)
			n = CLONEBP_FREG_MAX;
		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.freg_base = freg_base;
		r.freg_mask = freg_mask;
		for (k = 0; k < n; ++k)
			memcpy(&r.freg_val[k], &vals[k], sizeof(r.freg_val[k]));
		return (int)xfer_impl(OP_WRPBP_SET_FREGS, (uintptr_t)&r);
	}

	int wrpbp_get_records(uint32_t point_index, uint32_t start,
			      HWBP_RECORD *recs, uint32_t cap, uint32_t *cnt)
	{
		WRPBP_RECORDS_REQUEST r;

		if (!recs || !cap)
			return -1;
		memset(&r, 0, sizeof(r));
		r.point_index = point_index;
		r.start_index = start;
		r.capacity = cap;
		r.records = recs;
		r.count_out = cnt;
		return (int)xfer_impl(OP_WRPBP_GET_RECORDS, (uintptr_t)&r);
	}

	int wrpbp_clear_records(int32_t point_index = -1)
	{
		return (int)xfer_impl(OP_WRPBP_CLEAR_RECORDS,
				      (uintptr_t)(intptr_t)point_index);
	}

	/* 卡写修复是否真的在生效:
	 *   skipped 增长 -> 内核推进了 pc, store 没有被重试, 写回能留住
	 *   writes 涨而 skipped 不涨 -> 32 位 compat 任务, 或
	 *                                insmod 时 wind_wrp_skip_store=0
	 *   fails 涨 -> 观察点落在只读 VMA, 内存通道写不进去
	 * 返回 1 表示跳过计数在工作, 0 表示没在跳, -1 表示查不到。 */
	int wrpbp_skip_active()
	{
		WRPBP_CONFIG cfg;

		if (wrpbp_get_config(&cfg) != 0)
			return -1;
		return cfg.skipped ? 1 : 0;
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
