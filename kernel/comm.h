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
	OP_BRK_SET      = WIND_OP_BASE + 0x0C, /* 设置硬件断点 */
	OP_BRK_CLEAR    = WIND_OP_BASE + 0x0D, /* 清除硬件断点 */
	OP_KGSL_HIDE    = WIND_OP_BASE + 0x0E, /* kgsl 隐藏 pid */
	OP_KGSL_HOOK    = WIND_OP_BASE + 0x0F, /* 安装 kgsl hook */
	OP_GYRO_INSTALL = WIND_OP_BASE + 0x10, /* 安装陀螺仪 hook (pid, 对齐 hookmod 控制方式) */
	OP_GYRO_REMOVE  = WIND_OP_BASE + 0x11, /* 卸载陀螺仪 hook */
	OP_GYRO_SET     = WIND_OP_BASE + 0x12, /* 设置目标角速度 (rad/s) */
	OP_GYRO_ENABLE  = WIND_OP_BASE + 0x13, /* 开关陀螺仪篡改 */

	/* ---- 硬件断点 (BRP/WRP debug 寄存器, 最多 16 点) ---- */
	OP_HWBP_SET          = WIND_OP_BASE + 0x14, /* 设置断点/观察点组 */
	OP_HWBP_REMOVE       = WIND_OP_BASE + 0x15, /* 清除全部并清理 debug 寄存器 */
	OP_HWBP_GET_CONFIG   = WIND_OP_BASE + 0x16, /* 读当前配置 + BRP/WRP 数量 */
	OP_HWBP_GET_RECORDS  = WIND_OP_BASE + 0x17, /* 分段读触发 PC 聚合记录 */
	OP_HWBP_SET_RECORD   = WIND_OP_BASE + 0x18, /* 写回记录 + 2-bit 寄存器掩码 */
	OP_HWBP_CLEAR_RECORDS= WIND_OP_BASE + 0x19, /* 清空点位记录 (-1 全部) */

	/* ---- PTE 执行断点 (改 PTE 触发 fault, 最多 16 点, 不用 debug 寄存器) ---- */
	OP_PTEBP_SET          = WIND_OP_BASE + 0x1A,
	OP_PTEBP_REMOVE       = WIND_OP_BASE + 0x1B,
	OP_PTEBP_GET_CONFIG   = WIND_OP_BASE + 0x1C,
	OP_PTEBP_GET_RECORDS  = WIND_OP_BASE + 0x1D,
	OP_PTEBP_SET_RECORD   = WIND_OP_BASE + 0x1E,
	OP_PTEBP_CLEAR_RECORDS= WIND_OP_BASE + 0x1F,

	/* ---- BRK 软断点 (改写目标指令为 BRK, hook 异常) ---- */
	OP_BRKBP_SET          = WIND_OP_BASE + 0x20,
	OP_BRKBP_REMOVE       = WIND_OP_BASE + 0x21,
	OP_BRKBP_GET_CONFIG   = WIND_OP_BASE + 0x22,
	OP_BRKBP_GET_RECORDS  = WIND_OP_BASE + 0x23,
	OP_BRKBP_SET_RECORD   = WIND_OP_BASE + 0x24,
	OP_BRKBP_CLEAR_RECORDS= WIND_OP_BASE + 0x25,

	/* ---- ETM/PTM 分支追踪断点 (Coresight, 硬件级执行流观测) ---- */
	OP_ETMBP_SET          = WIND_OP_BASE + 0x26,
	OP_ETMBP_REMOVE       = WIND_OP_BASE + 0x27,
	OP_ETMBP_GET_CONFIG   = WIND_OP_BASE + 0x28,
	OP_ETMBP_GET_RECORDS  = WIND_OP_BASE + 0x29,
	OP_ETMBP_SET_RECORD   = WIND_OP_BASE + 0x2A,
	OP_ETMBP_CLEAR_RECORDS= WIND_OP_BASE + 0x2B,

	/* ---- 陀螺仪停止/恢复 (暂停/恢复篡改, 保留 hook) ---- */
	OP_GYRO_STOP         = WIND_OP_BASE + 0x2C,
	OP_GYRO_RESUME       = WIND_OP_BASE + 0x2D,

	/* ---- UDP 断点 (内核 UDP 收发路径打点) ---- */
	OP_UDPBP_SET          = WIND_OP_BASE + 0x2E, /* 装/更新一个 UDP 断点槽 */
	OP_UDPBP_REMOVE       = WIND_OP_BASE + 0x2F, /* 拆一个槽 (-1 = 全部) */
	OP_UDPBP_GET_CONFIG   = WIND_OP_BASE + 0x30, /* 读当前槽位表 */
	OP_UDPBP_GET_RECORDS  = WIND_OP_BASE + 0x31, /* 分段读命中记录 */
	OP_UDPBP_CLEAR_RECORDS= WIND_OP_BASE + 0x32, /* 清记录 (-1 = 全部槽) */

	/* ---- 读取路径统计 (页映射缓存命中率, 调优用) ---- */
	OP_STATS              = WIND_OP_BASE + 0x33,

	/* ---- 硬件断点命中时改写 FP 寄存器 (对齐 DUJIA/GTV 的 v3/v4/v5 语义) ---- */
	OP_HWBP_SET_REGS      = WIND_OP_BASE + 0x34,
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

/* ---- 硬件断点 (兼容旧 brk) ---- */
typedef struct _BRK_REQUEST
{
	pid_t pid;
	uintptr_t addr;
	uint32_t type;  /* HW_BREAKPOINT_X / _R / _W / _RW */
} BRK_REQUEST, *PBRK_REQUEST;

/* ---- 断点通用类型 / 长度 ---- */
enum BP_TYPE
{
	BP_TYPE_EXEC  = 1, /* 执行 (BRP) */
	BP_TYPE_READ  = 2, /* 读观察 (WRP) */
	BP_TYPE_WRITE = 3, /* 写观察 (WRP) */
	BP_TYPE_RW    = 4, /* 读写观察 (WRP) */
};

enum BP_LEN
{
	BP_LEN_1 = 1,
	BP_LEN_2 = 2,
	BP_LEN_4 = 4,
	BP_LEN_8 = 8,
};

/*
 * ---- 硬件断点 (BRP/WRP debug 寄存器) ----
 * 底层走 perf hw_breakpoint。执行断点占 BRP, 读写观察点占 WRP。
 * 单点触发记录, 每条记录带 2-bit 寄存器操作掩码 (op_mask)。
 */
typedef struct _HWBP_POINT_CONFIG
{
	uint64_t addr;    /* 目标虚拟地址 */
	uint32_t type;    /* BP_TYPE_* */
	uint32_t length;  /* BP_LEN_* (执行断点恒 4 或 8) */
} HWBP_POINT_CONFIG, *PHWBP_POINT_CONFIG;

typedef struct _HWBP_CONFIG
{
	uint32_t num_brps;      /* 硬件支持断点寄存器数 */
	uint32_t num_wrps;      /* 硬件支持观察点寄存器数 */
	uint32_t active_count;  /* 当前启用点数 */
	uint32_t reserved;
} HWBP_CONFIG, *PHWBP_CONFIG;

typedef struct _HWBP_RECORD
{
	uint64_t trigger_pc; /* 命中时 PC */
	uint64_t hit_addr;   /* 命中地址 */
	uint32_t op_mask;    /* 2-bit 寄存器操作掩码 (bit0=读, bit1=写) */
	uint32_t reserved;
} HWBP_RECORD, *PHWBP_RECORD;

/* 设置请求: 头部 + 变长 points 数组 */
typedef struct _HWBP_SET_REQUEST
{
	uint32_t point_count;
	uint32_t reserved;
	HWBP_POINT_CONFIG points[]; /* 变长 */
} HWBP_SET_REQUEST, *PHWBP_SET_REQUEST;

/* 记录读取请求: 内核回填 records + count_out */
typedef struct _HWBP_RECORDS_REQUEST
{
	uint32_t point_index; /* 目标点位 */
	uint32_t start_index; /* 起始记录下标 */
	uint32_t capacity;    /* records 数组容量 */
	uint32_t reserved;
	HWBP_RECORD *records; /* 用户态数组指针 */
	uint32_t *count_out;  /* 用户态实际条数输出指针 */
} HWBP_RECORDS_REQUEST, *PHWBP_RECORDS_REQUEST;

/* 单条记录写回 */
typedef struct _HWBP_RECORD_REQUEST
{
	uint32_t point_index;
	uint32_t record_index;
	uint32_t reserved;
	HWBP_RECORD record;
} HWBP_RECORD_REQUEST, *PHWBP_RECORD_REQUEST;

/* 清空记录请求: point_index = -1 清全部 */
typedef struct _HWBP_CLEAR_REQUEST
{
	int32_t point_index;
	uint32_t reserved;
} HWBP_CLEAR_REQUEST, *PHWBP_CLEAR_REQUEST;

/*
 * ---- PTE 执行断点 ----
 * 改目标页 PTE 清 PTE_AF (access flag) 触发 fault, 在 fault handler
 * 识别命中并记录 PC, 再恢复 AF + 单步跳过。不用 debug 寄存器, 隐藏性更高。
 */
typedef struct _PTEBP_POINT_CONFIG
{
	uint64_t addr;    /* 目标虚拟地址 */
	uint32_t type;    /* 恒 BP_TYPE_EXEC */
	uint32_t length;  /* 恒 4 */
} PTEBP_POINT_CONFIG, *PPTEBP_POINT_CONFIG;

typedef struct _PTEBP_CONFIG
{
	uint32_t active_count;
	uint32_t reserved;
	/* num_brps / num_wrps 恒 0 (PTE 断点不占 debug 寄存器) */
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

/*
 * ---- BRK 软断点 ----
 * 目标地址原指令暂存后改写为 BRK #imm, 触发 brk 异常, 在 handler 识别命中。
 * 数量无限, 但会改代码字节, 可被完整性校验检测。
 */
typedef struct _BRKBP_POINT_CONFIG
{
	uint64_t addr;
	uint32_t type;    /* 恒 BP_TYPE_EXEC */
	uint32_t length;  /* 恒 4 */
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

/*
 * ---- ETM/PTM 分支追踪断点 ----
 * 通过 Coresight ETM 硬件追踪执行流 (分支/指令), 不改 debug 寄存器、
 * 不改代码、不改页表, 纯被动观测, 隐藏性最高。
 * 地址过滤: 只在 [start,end) 区间内记录执行流。
 */
typedef struct _ETMBP_POINT_CONFIG
{
	uint64_t start;   /* 追踪区间起始地址 */
	uint64_t end;     /* 追踪区间结束地址 (开区间) */
	uint32_t flags;   /* 0=分支追踪 1=指令追踪 */
	uint32_t reserved;
} ETMBP_POINT_CONFIG, *PETMBP_POINT_CONFIG;

typedef struct _ETMBP_CONFIG
{
	uint32_t active_count;
	uint32_t trace_id;   /* 本 trace 会话 ID (sink 关联) */
	uint32_t reserved;
} ETMBP_CONFIG, *PETMBP_CONFIG;

typedef struct _ETMBP_RECORD
{
	uint64_t addr;       /* 追踪到的地址 (分支源/目标) */
	uint64_t timestamp;  /* 相对时间戳 */
	uint32_t flags;      /* bit0=分支 1=异常 2=返回 */
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

/* ---- 陀螺仪 ---- */
typedef struct _GYRO_REQUEST
{
	u32 rx, ry, rz;  /* 目标三轴角速度, float 位模式 (IEEE 754)。内核态禁用 FP 寄存器 */
} GYRO_REQUEST, *PGYRO_REQUEST;

/*
 * ---- 硬件断点命中改写请求 ----
 * 命中执行断点时把 v3/v4/v5 的低 32 位改成给定 float 位模式, 再让内核在
 * 返回用户态时重载 FP 状态。point_index = -1 表示全部点位。
 * enable = 0 时只清改写配置 (断点保留)。
 */
typedef struct _HWBP_REG_REQUEST
{
	int32_t  point_index;
	uint32_t enable;
	uint32_t rx, ry, rz;   /* float 位模式 */
	uint32_t reserved;
} HWBP_REG_REQUEST, *PHWBP_REG_REQUEST;

/*
 * ---- UDP 断点 ----
 * 在 UDP 收发路径上打点 (kprobe udp_send_skb / udp_queue_rcv_skb),
 * 按 pid / 端口 / 方向过滤, 命中即记录或就地改写载荷。
 * 不改 debug 寄存器、不改页表, 只挂内核探针, 对目标进程完全不可见。
 *
 *   dir : 0 = 发送 (TX), 1 = 接收 (RX), 2 = 双向
 *   act : 0 = 只记录, 1 = 记录 + 改写载荷首 patch_len 字节
 *   port: 0 = 任意。TX 比目的端口, RX 比源端口。
 *   pid : 0 = 任意进程 (按 tgid 匹配)
 */
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

#define UDPBP_MAX_SLOTS      8
#define UDPBP_PATCH_MAX      64
#define UDPBP_PREVIEW_MAX    32
#define UDPBP_RECORD_MAX     256

typedef struct _UDPBP_POINT_CONFIG
{
	uint32_t pid;          /* 0 = 任意 (tgid) */
	uint16_t port;         /* 0 = 任意 (主机序) */
	uint16_t dir;          /* UDPBP_DIR_* */
	uint16_t action;       /* UDPBP_ACTION_* */
	uint16_t patch_len;    /* <= UDPBP_PATCH_MAX */
	uint8_t  patch[UDPBP_PATCH_MAX];
} UDPBP_POINT_CONFIG, *PUDPBP_POINT_CONFIG;

typedef struct _UDPBP_RECORD
{
	uint64_t ts_ns;        /* 命中时刻 (ktime_get_ns) */
	uint32_t pid;          /* 命中进程 tgid */
	uint32_t saddr;        /* 网络序 IPv4 */
	uint32_t daddr;
	uint16_t sport;        /* 主机序 */
	uint16_t dport;
	uint16_t dir;          /* UDPBP_DIR_TX / RX */
	uint16_t action;       /* 实际执行的动作 */
	uint32_t len;          /* 载荷长度 */
	uint32_t preview_len;  /* 实际抓到的预览字节数 */
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
	int32_t slot_index;    /* 入: -1 = 自动选空槽; 出: 实际槽号 */
	uint32_t reserved;
	int32_t *slot_out;     /* 可选: 内核写回实际槽号 */
	UDPBP_POINT_CONFIG point;
} UDPBP_SET_REQUEST, *PUDPBP_SET_REQUEST;

typedef struct _UDPBP_RECORDS_REQUEST
{
	int32_t slot_index;    /* -1 = 所有槽合并按时间取 */
	uint32_t start_index;
	uint32_t capacity;
	uint32_t reserved;
	UDPBP_RECORD *records;
	uint32_t *count_out;
} UDPBP_RECORDS_REQUEST, *PUDPBP_RECORDS_REQUEST;

typedef struct _UDPBP_CLEAR_REQUEST
{
	int32_t slot_index;    /* -1 = 全部 */
	uint32_t reserved;
} UDPBP_CLEAR_REQUEST, *PUDPBP_CLEAR_REQUEST;

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

#endif /* _COMM_H_ */
