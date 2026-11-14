#ifndef _COMM_H_
#define _COMM_H_

#include <linux/types.h>

/*
 * Wind_driver 通讯协议 v2
 * 目标: 减少 ioctl 调用次数, 支持批量读写与会话缓存, 双模式读取
 */

/* ---- 操作码 ---- */
/* 独家编码: 高 16 位 = 0x574E ("WN" = wind), 低 16 位 = 序号。
 * 不用通用 0x800 系列, 避免与示例驱动 / 其它模块 opcode 撞车。 */
#define WIND_OP_BASE 0x574E0000

enum OPERATIONS
{
	OP_BIND         = WIND_OP_BASE + 0x01, /* 绑定目标 pid, 缓存 task/mm (open 后先调用一次) */
	OP_UNBIND       = WIND_OP_BASE + 0x02, /* 解绑并释放缓存 */
	OP_READ_MEM     = WIND_OP_BASE + 0x03, /* 单次读 (兼容旧协议) */
	OP_WRITE_MEM    = WIND_OP_BASE + 0x04, /* 单次写 (兼容旧协议) */
	OP_READ_BATCH   = WIND_OP_BASE + 0x05, /* 批量读, 一次 ioctl 读多个离散地址 */
	OP_WRITE_BATCH  = WIND_OP_BASE + 0x06, /* 批量写 */
	OP_SET_MODE     = WIND_OP_BASE + 0x07, /* 切换读取模式 (安全/快速) */
	OP_MODULE_BASE  = WIND_OP_BASE + 0x08, /* 取模块基址 */
	OP_HIDE_FILE    = WIND_OP_BASE + 0x09, /* 隐藏文件 (按 basename) */
	OP_HIDE_PROCESS = WIND_OP_BASE + 0x0A, /* 隐藏进程 */
	OP_UNHIDE_ALL   = WIND_OP_BASE + 0x0B, /* 撤销所有隐藏 */
	OP_KGSL_HIDE    = WIND_OP_BASE + 0x0E, /* kgsl 隐藏 pid */
	OP_KGSL_HOOK    = WIND_OP_BASE + 0x0F, /* 安装 kgsl hook */
	OP_GYRO_INSTALL = WIND_OP_BASE + 0x10, /* 安装陀螺仪 hook (pid, 对齐 hookmod 控制方式) */
	OP_GYRO_REMOVE  = WIND_OP_BASE + 0x11, /* 卸载陀螺仪 hook */
	OP_GYRO_SET     = WIND_OP_BASE + 0x12, /* 设置目标角速度 (rad/s) */
	OP_GYRO_ENABLE  = WIND_OP_BASE + 0x13, /* 开关陀螺仪篡改 */

	/* ---- 陀螺仪停止/恢复 (暂停/恢复篡改, 保留 hook) ---- */
	OP_GYRO_STOP         = WIND_OP_BASE + 0x2C,
	OP_GYRO_RESUME       = WIND_OP_BASE + 0x2D,

	/* ---- 读取路径统计 (页映射缓存命中率, 调优用) ---- */
	OP_STATS              = WIND_OP_BASE + 0x33,

	/* ---- 克隆页断点 (Clone-Page): 同 VA 双物理页, 不占 BRP/WRP 寄存器 ----
	 * 原始页只读不可执行(用户 CRC 干净), 影子页用户不可读可执行(内含 BRK)。 */
	OP_CLONEBP_SET          = WIND_OP_BASE + 0x40, /* 安装克隆页断点组 */
	OP_CLONEBP_REMOVE       = WIND_OP_BASE + 0x41, /* 拆除全部并还原页表 */
	OP_CLONEBP_GET_CONFIG   = WIND_OP_BASE + 0x42, /* 读启用点数 + 影子页数 + 故障计数 */
	OP_CLONEBP_GET_RECORDS  = WIND_OP_BASE + 0x43, /* 分段读命中记录 */
	OP_CLONEBP_SET_RECORD   = WIND_OP_BASE + 0x44, /* 改单点改写配置 */
	OP_CLONEBP_CLEAR_RECORDS= WIND_OP_BASE + 0x45, /* 清记录 (-1 全部) */
	OP_CLONEBP_REFRESH      = WIND_OP_BASE + 0x46, /* 重新克隆影子页(页被重映射后自愈) */
	OP_CLONEBP_TIMING       = WIND_OP_BASE + 0x47, /* 时序对抗: 耗时归一化/抖动/兜底阈值 */
	OP_ACE_SET              = WIND_OP_BASE + 0x50, /* 设 ACE 目标 tgid */
	OP_ACE_GET_STAT         = WIND_OP_BASE + 0x51, /* 读 ACE 伪装命中计数 */
	OP_ACE_VERBOSE          = WIND_OP_BASE + 0x52, /* 开关驱动自身日志(默认关) */
	OP_ACE_SET_FEATURES     = WIND_OP_BASE + 0x54, /* 设单项伪装开关位掩码 */
	OP_ACE_GET_FEATURES     = WIND_OP_BASE + 0x55, /* 读当前开关位掩码 */
	OP_ACE_INTE             = WIND_OP_BASE + 0x53, /* 0=接管 inte 三节点(重建假节点) 1=只摘除 */
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

/* ---- 陀螺仪 ---- */
typedef struct _GYRO_REQUEST
{
	u32 rx, ry, rz;  /* 目标三轴角速度, float 位模式 (IEEE 754)。内核态禁用 FP 寄存器 */
} GYRO_REQUEST, *PGYRO_REQUEST;

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

/*
 * ---- netlink 传输 (无节点通道) ----
 * 内核注册一个私有 AF_NETLINK 协议号, 用户态 socket(AF_NETLINK, SOCK_RAW, unit)
 * 发一条 WIND_NL_MSG, 内核在同一发送进程上下文里执行操作并回一条同样结构。
 * arg 是用户态指针, 语义与 ioctl 的 arg 完全一致, 因此 dispatch 逻辑不用改。
 * 不建任何设备节点, 不 hook 任何内核对象。
 */
#define WIND_NL_MAGIC 0x574E4C31u /* "WNL1" */

typedef struct _WIND_NL_MSG
{
	uint32_t magic;  /* WIND_NL_MAGIC */
	uint32_t op;     /* OPERATIONS 之一 */
	uint64_t arg;    /* 用户态指针, 同 ioctl 第三参数 */
	int64_t  result; /* 内核回填的返回值 */
} WIND_NL_MSG, *PWIND_NL_MSG;


/* ---- 克隆页断点协议 ---- */
#define CLONEBP_MAX_POINTS   32
#define CLONEBP_RECORD_MAX   256

/* ---- 克隆页时序对抗配置 (读写同一结构: 写入 mode/target/jitter/bailout, 回读统计) ---- */
typedef struct _CLONEBP_TIMING_CONFIG
{
	uint32_t mode;          /* bit0=BALANCE bit1=JITTER; 0xFFFFFFFF 表示只读 */
	uint32_t target_ns;     /* BALANCE 目标单次异常耗时 (ns), 上限 200000 */
	uint32_t jitter_ns;     /* JITTER 均匀抖动上限 (ns) */
	uint32_t bailout_max;   /* 同页连续异常达到该次数就放宽为静默页 (1~4096) */
	uint64_t cost_ns;       /* 出参: 累计异常处理耗时 */
	uint64_t pad_ns;        /* 出参: 累计补齐耗时 */
	uint64_t evt_cnt;       /* 出参: 累计异常事件数 */
	uint32_t reserved[2];
} CLONEBP_TIMING_CONFIG, *PCLONEBP_TIMING_CONFIG;

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
	CLONEBP_ST_IFAULT_IN = 0,  /* 执行命中原始页 -> 切影子 */
	CLONEBP_ST_DFAULT_IN = 1,  /* 读取命中影子页 -> 切原始 */
	CLONEBP_ST_BRK_HIT   = 2,  /* 影子页 BRK 命中 */
	CLONEBP_ST_TLB_FLUSH = 3,
	CLONEBP_ST_CLONE_OK  = 4,
	CLONEBP_ST_CLONE_FAIL= 5,
	CLONEBP_ST_MAX       = 6,
};

typedef struct _CLONEBP_POINT_CONFIG
{
	uint64_t addr;                    /* 目标函数入口 VA */
	uint32_t mode;                    /* CLONEBP_MODE_* */
	uint32_t reg_base;                /* 写回起始 X 寄存器号 0..30 */
	uint32_t reg_mask;                /* bit0..bit7 对应 reg_base+0..7 */
	uint32_t flags;                   /* CLONEBP_F_* */
	uint64_t reg_val[CLONEBP_REG_MAX];/* 写回值 */
	uint32_t imm;                     /* BRK 立即数, 0=自动分配 */
	uint32_t patch_len;               /* 影子页补丁长度, 0=写 BRK */
	uint8_t  patch[CLONEBP_PATCH_MAX];/* 影子页替换字节 */

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
	uint32_t pid;          /* 目标进程 */
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
	uint64_t faults_in_i;   /* 执行故障次数 */
	uint64_t faults_in_d;   /* 读取故障次数 */
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

/* ---- 反 ACE 检测: 伪装命中统计 ---- */
#define ACE_F_MAPS     (1u << 0)
#define ACE_F_MODS     (1u << 1)
#define ACE_F_KSYM     (1u << 2)
#define ACE_F_TRACERP  (1u << 3)
#define ACE_F_FS       (1u << 4)
#define ACE_F_COMM     (1u << 5)
#define ACE_F_PORT     (1u << 6)
#define ACE_F_PROP     (1u << 7)   /* /dev/__properties__ 共享映射识别 */
#define ACE_F_FD       (1u << 8)   /* /proc/<pid>/fd 目录枚举阻断 */
#define ACE_F_SMAPS    (1u << 9)   /* /proc/<pid>/smaps 行级过滤 */
#define ACE_F_MASK     0x3FFu

typedef struct _ACE_STAT
{
	uint32_t tgid;
	uint32_t features;
	uint64_t maps_drop;      /* maps 被滤掉的行数 */
	uint64_t mod_drop;       /* /proc/modules 被滤掉的行数 */
	uint64_t ksym_drop;      /* kallsyms 被清空的符号数 */
	uint64_t tracerp_hide;   /* TracerPid 被清零的次数 */
	uint64_t fs_drop;        /* 路径类 syscall 被置空的次数 */
	uint64_t comm_drop;      /* 线程名被滤掉的行数 */
	uint64_t port_drop;      /* 调试端口连接被阻断的次数 */
	uint64_t smaps_drop;     /* smaps 被滤掉的 vma 段数 */
	uint64_t fd_drop;        /* /proc/<pid>/fd 枚举被阻断的次数 */
	uint64_t prop_hit;       /* 属性区共享映射被识别的次数 */
} ACE_STAT, *PACE_STAT;

#endif /* _COMM_H_ */
