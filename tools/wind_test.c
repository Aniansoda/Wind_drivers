/*
 * wind_test.c — Wind_driver 用户态完整功能测试程序
 *
 * 通过 netlink (默认 unit 29) 与 wind.ko 通信, 覆盖驱动全部 OP。
 * 纯 C / POSIX, 无 Android 依赖; 可交叉编译成 arm64 ELF 直接 push 到设备跑。
 *
 * 用法:
 *   wind_test selftest <pid>                完整自检流程
 *   wind_test <command> [args...]           单条命令
 *
 * 所有地址 / 浮点均支持十六进制位模式 (0x 前缀) 与十进制。
 * 浮点字段: 传十进制字符串按 float 解析, 传 0x 前缀按 IEEE754 位模式直通。
 *
 * 编译 (任选其一):
 *   gcc -O2 -Wall -o wind_test wind_test.c
 *   aarch64-linux-gnu-gcc -O2 -Wall -static -o wind_test wind_test.c
 *   NDK clang: aarch64-linux-android-clang -O2 -o wind_test wind_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/netlink.h>

/* ===================== 协议常量 (与 kernel/comm.h 对齐) ===================== */

#define WIND_OP_BASE    0x574E0000u
#define WIND_NL_MAGIC   0x574E4C31u
#define WIND_NL_UNIT    29

enum {
    OP_BIND            = WIND_OP_BASE + 0x01,
    OP_UNBIND          = WIND_OP_BASE + 0x02,
    OP_READ_MEM        = WIND_OP_BASE + 0x03,
    OP_WRITE_MEM       = WIND_OP_BASE + 0x04,
    OP_READ_BATCH      = WIND_OP_BASE + 0x05,
    OP_WRITE_BATCH     = WIND_OP_BASE + 0x06,
    OP_SET_MODE        = WIND_OP_BASE + 0x07,
    OP_MODULE_BASE     = WIND_OP_BASE + 0x08,
    OP_HIDE_FILE       = WIND_OP_BASE + 0x09,
    OP_HIDE_PROCESS    = WIND_OP_BASE + 0x0A,
    OP_UNHIDE_ALL      = WIND_OP_BASE + 0x0B,
    OP_KGSL_HIDE       = WIND_OP_BASE + 0x0E,
    OP_KGSL_HOOK       = WIND_OP_BASE + 0x0F,
    OP_GYRO_INSTALL    = WIND_OP_BASE + 0x10,
    OP_GYRO_REMOVE     = WIND_OP_BASE + 0x11,
    OP_GYRO_SET        = WIND_OP_BASE + 0x12,
    OP_GYRO_ENABLE     = WIND_OP_BASE + 0x13,

    OP_WRPBP_SET           = WIND_OP_BASE + 0x26,
    OP_WRPBP_REMOVE        = WIND_OP_BASE + 0x27,
    OP_WRPBP_GET_CONFIG    = WIND_OP_BASE + 0x28,
    OP_WRPBP_GET_RECORDS   = WIND_OP_BASE + 0x29,
    OP_WRPBP_SET_VEC       = WIND_OP_BASE + 0x2A,
    OP_WRPBP_CLEAR_RECORDS = WIND_OP_BASE + 0x2B,
    OP_WRPBP_SET_FREGS     = WIND_OP_BASE + 0x56,

    OP_GYRO_STOP   = WIND_OP_BASE + 0x2C,
    OP_GYRO_RESUME = WIND_OP_BASE + 0x2D,

    OP_STATS = WIND_OP_BASE + 0x33,

    OP_CLONEBP_SET           = WIND_OP_BASE + 0x40,
    OP_CLONEBP_REMOVE        = WIND_OP_BASE + 0x41,
    OP_CLONEBP_GET_CONFIG    = WIND_OP_BASE + 0x42,
    OP_CLONEBP_GET_RECORDS   = WIND_OP_BASE + 0x43,
    OP_CLONEBP_SET_RECORD    = WIND_OP_BASE + 0x44,
    OP_CLONEBP_CLEAR_RECORDS = WIND_OP_BASE + 0x45,
    OP_CLONEBP_REFRESH       = WIND_OP_BASE + 0x46,
    OP_CLONEBP_TIMING        = WIND_OP_BASE + 0x47,

    OP_ACE_SET          = WIND_OP_BASE + 0x50,
    OP_ACE_QUERY        = WIND_OP_BASE + 0x51,
};

enum { MODE_FAST = 0, MODE_SAFE = 1 };

#define CLONEBP_MAX_POINTS 32
#define CLONEBP_RECORD_MAX 256
#define CLONEBP_MAX_PAGES  64
#define CLONEBP_PATCH_MAX  16
#define CLONEBP_REG_MAX    8
#define CLONEBP_FREG_MAX   8

#define CLONEBP_MODE_LOG     0
#define CLONEBP_MODE_REWRITE 1
#define CLONEBP_F_ONESHOT    0x1
#define CLONEBP_F_KEEP_ORIG  0x2
#define CLONEBP_F_NO_FLUSH   0x4

#define WRPBP_MAX_POINTS 2
#define WRPBP_RECORD_MAX 256
#define HWBP_MASK_ALL    0x7U

enum { BP_TYPE_EXEC = 1, BP_TYPE_READ = 2, BP_TYPE_WRITE = 3, BP_TYPE_RW = 4 };
enum { BP_LEN_1 = 1, BP_LEN_2 = 2, BP_LEN_4 = 4, BP_LEN_8 = 8 };

#define ACE_F_MAPS   (1u << 0)
#define ACE_F_MODS   (1u << 1)
#define ACE_F_KSYM   (1u << 2)
#define ACE_F_TRACER (1u << 3)
#define ACE_F_FS     (1u << 4)
#define ACE_F_COMM   (1u << 5)
#define ACE_F_PORT   (1u << 6)
#define ACE_F_PROP   (1u << 7)
#define ACE_F_FD     (1u << 8)
#define ACE_F_SMAPS  (1u << 9)
#define ACE_F_MASK   0x3FFu

/* ===================== 协议结构体 (与 kernel/comm.h 对齐) ===================== */

typedef struct _WIND_NL_MSG {
    uint32_t magic;
    uint32_t op;
    uint64_t arg;
    int64_t  result;
} WIND_NL_MSG;

typedef struct _COPY_MEMORY {
    int32_t    pid;
    uintptr_t  addr;
    void      *buffer;
    size_t     size;
} COPY_MEMORY;

typedef struct _MODULE_BASE {
    int32_t    pid;
    char      *name;
    uintptr_t  base;
} MODULE_BASE;

typedef struct _BATCH_ITEM {
    uintptr_t addr;
    size_t    size;
    void     *buffer;
} BATCH_ITEM;

typedef struct _BATCH_REQUEST {
    uint32_t count;
    uint32_t reserved;
    BATCH_ITEM items[];
} BATCH_REQUEST;

typedef struct _MODE_REQUEST { uint32_t mode; } MODE_REQUEST;

typedef struct _GYRO_REQUEST {
    uint32_t rx, ry, rz;
} GYRO_REQUEST;

typedef struct _CLONEBP_TIMING_CONFIG {
    uint32_t mode;
    uint32_t target_ns;
    uint32_t jitter_ns;
    uint32_t bailout_max;
    uint64_t cost_ns;
    uint64_t pad_ns;
    uint64_t evt_cnt;
    uint32_t reserved[2];
} CLONEBP_TIMING_CONFIG;

typedef struct _CLONEBP_POINT_CONFIG {
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
    uint32_t freg_val[CLONEBP_FREG_MAX];
} CLONEBP_POINT_CONFIG;

typedef struct _CLONEBP_SET_REQUEST {
    uint32_t pid;
    uint32_t point_count;
    CLONEBP_POINT_CONFIG points[];
} CLONEBP_SET_REQUEST;

typedef struct _CLONEBP_RECORD {
    uint64_t trigger_pc;
    uint64_t hit_addr;
    uint32_t hits;
    uint32_t reserved;
} CLONEBP_RECORD;

typedef struct _CLONEBP_SET_RECORD_REQUEST {
    uint32_t pid;
    uint32_t point_index;
    CLONEBP_POINT_CONFIG point;
} CLONEBP_SET_RECORD_REQUEST;

typedef struct _CLONEBP_CONFIG {
    uint32_t active_count;
    uint32_t shadow_pages;
    uint64_t faults_in_i;
    uint64_t faults_in_d;
    uint64_t brk_hits;
    uint64_t tlb_flushes;
    uint64_t clone_ok;
    uint64_t clone_fail;
    uint32_t reserved[2];
} CLONEBP_CONFIG;

typedef struct _CLONEBP_RECORDS_REQUEST {
    uint32_t point_index;
    uint32_t start_index;
    uint32_t capacity;
    uint32_t reserved;
    CLONEBP_RECORD *records;
    uint32_t *count_out;
} CLONEBP_RECORDS_REQUEST;

typedef struct _HWBP_RECORD {
    uint64_t trigger_pc;
    uint64_t hit_addr;
    uint32_t op_mask;
    uint32_t reserved;
} HWBP_RECORD;

typedef struct _WRPBP_POINT_CONFIG {
    uint64_t watch_addr;
    uint32_t watch_len;
    uint32_t type;
    uint32_t components;
    uint32_t freg_base;
    uint32_t freg_mask;
} WRPBP_POINT_CONFIG;

typedef struct _WRPBP_SET_REQUEST {
    uint32_t pid;
    uint32_t point_count;
    WRPBP_POINT_CONFIG points[];
} WRPBP_SET_REQUEST;

typedef struct _WRPBP_VEC_REQUEST {
    int32_t  point_index;
    uint32_t enable;
    uint32_t x, y, z;
    uint32_t mask;
} WRPBP_VEC_REQUEST;

typedef struct _WRPBP_FREGS_REQUEST {
    int32_t  point_index;
    uint32_t freg_base;
    uint32_t freg_mask;
    uint32_t freg_val[CLONEBP_FREG_MAX];
} WRPBP_FREGS_REQUEST;

typedef struct _WRPBP_CONFIG {
    uint32_t active_count;
    uint32_t watch_addr_lo;
    uint32_t watch_addr_hi;
    uint32_t reserved;
} WRPBP_CONFIG;

typedef struct _WRPBP_RECORDS_REQUEST {
    uint32_t point_index;
    uint32_t start_index;
    uint32_t capacity;
    uint32_t reserved;
    HWBP_RECORD *records;
    uint32_t *count_out;
} WRPBP_RECORDS_REQUEST;

typedef struct _ACE_STAT {
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
} ACE_STAT;

typedef struct _ACE_SET_REQUEST {
    uint32_t tgid;
    uint32_t features;
    uint32_t verbose;
    uint32_t inte;
} ACE_SET_REQUEST;

#define ACE_QUERY_STAT     1
#define ACE_QUERY_FEATURES 2
#define ACE_QUERY_PKGNAME  3
#define ACE_PKGNAME_MAX    256

typedef struct _ACE_QUERY_REQUEST {
    uint32_t cmd;
    uint32_t pid;
    uint32_t reserved0;
    uint32_t reserved1;
    ACE_STAT stat;
    uint32_t features;
    char     pkgname[ACE_PKGNAME_MAX];
} ACE_QUERY_REQUEST;

typedef struct _WIND_MAP_STATS {
    uint64_t reads;
    uint64_t hits;
    uint64_t misses;
    uint64_t evicts;
    uint64_t cached;
    uint64_t tr_hits;
    uint64_t tr_misses;
} WIND_MAP_STATS;

/* ===================== netlink 通道 ===================== */

static int nl_fd = -1;
static uint32_t nl_seq = 1;

static int nl_open(void)
{
    struct sockaddr_nl sa;

    nl_fd = socket(AF_NETLINK, SOCK_RAW, WIND_NL_UNIT);
    if (nl_fd < 0) {
        fprintf(stderr, "socket(AF_NETLINK,%d) 失败: %s\n", WIND_NL_UNIT, strerror(errno));
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid    = getpid();
    if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "bind netlink 失败: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* 发送一条 OP, 返回内核回填的 result (dispatch 返回值) */
static long nl_call(uint32_t op, uint64_t arg)
{
    struct {
        struct nlmsghdr nlh;
        WIND_NL_MSG      m;
    } req;
    char rbuf[512];
    struct sockaddr_nl dst;
    ssize_t n;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len  = NLMSG_LENGTH(sizeof(WIND_NL_MSG));
    req.nlh.nlmsg_type = 0;
    req.nlh.nlmsg_flags= 0;
    req.nlh.nlmsg_seq  = nl_seq++;
    req.nlh.nlmsg_pid  = getpid();
    req.m.magic = WIND_NL_MAGIC;
    req.m.op    = op;
    req.m.arg   = arg;
    req.m.result= -1;

    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;

    if (sendto(nl_fd, &req, req.nlh.nlmsg_len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        fprintf(stderr, "  sendto op=0x%08x 失败: %s\n", op, strerror(errno));
        return -errno;
    }

    n = recv(nl_fd, rbuf, sizeof(rbuf), 0);
    if (n < (ssize_t)NLMSG_LENGTH(sizeof(WIND_NL_MSG))) {
        fprintf(stderr, "  recv op=0x%08x 失败/超短: n=%zd (%s)\n", op, n, strerror(errno));
        return -errno;
    }

    return ((WIND_NL_MSG *)NLMSG_DATA((struct nlmsghdr *)rbuf))->result;
}

/* ===================== 参数解析 helper ===================== */

static uint64_t parse_u64(const char *s)
{
    return strtoull(s, NULL, 0);   /* 0x 前缀按十六进制, 否则十进制 */
}

/* float 字符串 -> IEEE754 位模式; 0x 前缀按位模式直通 */
static uint32_t parse_fbits(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint32_t)strtoul(s + 2, NULL, 16);
    float f = strtof(s, NULL);
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static void print_ret(const char *what, long r)
{
    printf("%-30s -> ret=%ld%s\n", what, r,
           r < 0 ? " (负=错误, 对照 dmesg)" : "");
}

/* ===================== 各功能命令实现 ===================== */

static int cmd_bind(const char *pidstr)
{
    pid_t pid = (pid_t)atoi(pidstr);
    return (int)nl_call(OP_BIND, (uint64_t)(long)pid);
}

static int cmd_stats(void)
{
    WIND_MAP_STATS st;
    long r = nl_call(OP_STATS, (uint64_t)&st);
    if (r == 0) {
        printf("  reads=%llu hits=%llu misses=%llu evicts=%llu cached=%llu tr_hits=%llu tr_misses=%llu\n",
               (unsigned long long)st.reads, (unsigned long long)st.hits,
               (unsigned long long)st.misses, (unsigned long long)st.evicts,
               (unsigned long long)st.cached, (unsigned long long)st.tr_hits,
               (unsigned long long)st.tr_misses);
    }
    return (int)r;
}

static int cmd_module_base(const char *pidstr, const char *name)
{
    MODULE_BASE mb;
    char nbuf[256];
    memset(&mb, 0, sizeof(mb));
    memset(nbuf, 0, sizeof(nbuf));
    strncpy(nbuf, name, sizeof(nbuf) - 1);
    mb.pid  = (int32_t)atoi(pidstr);
    mb.name = nbuf;
    long r = nl_call(OP_MODULE_BASE, (uint64_t)&mb);
    if (r == 0)
        printf("  base=0x%llx\n", (unsigned long long)mb.base);
    return (int)r;
}

static int cmd_read(const char *addrstr, const char *sizestr)
{
    COPY_MEMORY cm;
    uint8_t buf[64];
    size_t size = (size_t)strtoull(sizestr, NULL, 0);
    if (size > sizeof(buf)) size = sizeof(buf);

    memset(&cm, 0, sizeof(cm));
    memset(buf, 0, sizeof(buf));
    cm.addr   = (uintptr_t)parse_u64(addrstr);
    cm.buffer = buf;
    cm.size   = size;
    long r = nl_call(OP_READ_MEM, (uint64_t)&cm);
    if (r == 0) {
        printf("  [");
        for (size_t i = 0; i < size; i++) printf("%02x ", buf[i]);
        printf("]\n");
    }
    return (int)r;
}

static int cmd_write(const char *addrstr, const char *hexbytes)
{
    COPY_MEMORY cm;
    uint8_t buf[64];
    size_t n = 0;
    char *tok, *save = NULL, *tmp;

    tmp = strdup(hexbytes);
    for (tok = strtok_r(tmp, ",", &save); tok && n < sizeof(buf); tok = strtok_r(NULL, ",", &save))
        buf[n++] = (uint8_t)strtoul(tok, NULL, 16);
    free(tmp);

    memset(&cm, 0, sizeof(cm));
    cm.addr   = (uintptr_t)parse_u64(addrstr);
    cm.buffer = buf;
    cm.size   = n;
    return (int)nl_call(OP_WRITE_MEM, (uint64_t)&cm);
}

static int cmd_hide_file(const char *path)
{
    char p[256];
    memset(p, 0, sizeof(p));
    strncpy(p, path, sizeof(p) - 1);
    return (int)nl_call(OP_HIDE_FILE, (uint64_t)p);
}

static int cmd_wrpbp_set(const char *pidstr, const char *addrstr, const char *lenstr,
                         const char *typestr, const char *compstr,
                         const char *fregbasestr, const char *fregmaskstr)
{
    struct { WRPBP_SET_REQUEST h; WRPBP_POINT_CONFIG p; } req;
    memset(&req, 0, sizeof(req));
    req.h.pid         = (uint32_t)atoi(pidstr);
    req.h.point_count = 1;
    req.p.watch_addr  = parse_u64(addrstr);
    req.p.watch_len   = (uint32_t)atoi(lenstr);
    req.p.type        = (uint32_t)atoi(typestr);
    req.p.components  = (uint32_t)atoi(compstr);
    req.p.freg_base   = (uint32_t)atoi(fregbasestr);
    req.p.freg_mask   = (uint32_t)strtoul(fregmaskstr, NULL, 0);
    return (int)nl_call(OP_WRPBP_SET, (uint64_t)&req);
}

static int cmd_wrpbp_config(void)
{
    WRPBP_CONFIG cfg;
    long r = nl_call(OP_WRPBP_GET_CONFIG, (uint64_t)&cfg);
    if (r == 0)
        printf("  active=%u watch_addr=0x%08x%08x\n", cfg.active_count,
               cfg.watch_addr_hi, cfg.watch_addr_lo);
    return (int)r;
}

static int cmd_wrpbp_records(const char *idxstr)
{
    WRPBP_RECORDS_REQUEST req;
    HWBP_RECORD recs[WRPBP_RECORD_MAX];
    uint32_t cnt = 0;
    memset(&req, 0, sizeof(req));
    memset(recs, 0, sizeof(recs));
    req.point_index = (uint32_t)atoi(idxstr);
    req.start_index = 0;
    req.capacity    = WRPBP_RECORD_MAX;
    req.records     = recs;
    req.count_out   = &cnt;
    long r = nl_call(OP_WRPBP_GET_RECORDS, (uint64_t)&req);
    if (r == 0) {
        printf("  count=%u\n", cnt);
        for (uint32_t i = 0; i < cnt && i < 16; i++)
            printf("    pc=0x%llx addr=0x%llx op=%u\n",
                   (unsigned long long)recs[i].trigger_pc,
                   (unsigned long long)recs[i].hit_addr, recs[i].op_mask);
    }
    return (int)r;
}

static int cmd_wrpbp_set_vec(const char *idx, const char *enable,
                             const char *x, const char *y, const char *z, const char *mask)
{
    WRPBP_VEC_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.point_index = (int32_t)atoi(idx);
    req.enable      = (uint32_t)atoi(enable);
    req.x = parse_fbits(x);
    req.y = parse_fbits(y);
    req.z = parse_fbits(z);
    req.mask = (uint32_t)strtoul(mask, NULL, 0);
    return (int)nl_call(OP_WRPBP_SET_VEC, (uint64_t)&req);
}

static int cmd_wrpbp_set_fregs(const char *idx, const char *fregbase, const char *fregmask,
                               int argc, char **argv)
{
    WRPBP_FREGS_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.point_index = (int32_t)atoi(idx);
    req.freg_base   = (uint32_t)atoi(fregbase);
    req.freg_mask   = (uint32_t)strtoul(fregmask, NULL, 0);
    for (int i = 0; i < argc && i < CLONEBP_FREG_MAX; i++)
        req.freg_val[i] = parse_fbits(argv[i]);
    return (int)nl_call(OP_WRPBP_SET_FREGS, (uint64_t)&req);
}

static int cmd_clonebp_set(const char *pidstr, const char *addrstr, const char *modestr,
                           const char *regbase, const char *regmask, const char *immstr)
{
    struct { CLONEBP_SET_REQUEST h; CLONEBP_POINT_CONFIG p; } req;
    memset(&req, 0, sizeof(req));
    req.h.pid         = (uint32_t)atoi(pidstr);
    req.h.point_count = 1;
    req.p.addr        = parse_u64(addrstr);
    req.p.mode        = (uint32_t)atoi(modestr);
    req.p.reg_base    = (uint32_t)atoi(regbase);
    req.p.reg_mask    = (uint32_t)strtoul(regmask, NULL, 0);
    req.p.imm         = (uint32_t)strtoul(immstr, NULL, 0);
    return (int)nl_call(OP_CLONEBP_SET, (uint64_t)&req);
}

static int cmd_clonebp_config(void)
{
    CLONEBP_CONFIG cfg;
    long r = nl_call(OP_CLONEBP_GET_CONFIG, (uint64_t)&cfg);
    if (r == 0)
        printf("  active=%u shadow=%u brk=%llu faults_i=%llu faults_d=%llu clone_ok=%llu fail=%llu\n",
               cfg.active_count, cfg.shadow_pages,
               (unsigned long long)cfg.brk_hits, (unsigned long long)cfg.faults_in_i,
               (unsigned long long)cfg.faults_in_d, (unsigned long long)cfg.clone_ok,
               (unsigned long long)cfg.clone_fail);
    return (int)r;
}

static int cmd_clonebp_records(const char *idxstr)
{
    CLONEBP_RECORDS_REQUEST req;
    CLONEBP_RECORD recs[CLONEBP_RECORD_MAX];
    uint32_t cnt = 0;
    memset(&req, 0, sizeof(req));
    memset(recs, 0, sizeof(recs));
    req.point_index = (uint32_t)atoi(idxstr);
    req.start_index = 0;
    req.capacity    = CLONEBP_RECORD_MAX;
    req.records     = recs;
    req.count_out   = &cnt;
    long r = nl_call(OP_CLONEBP_GET_RECORDS, (uint64_t)&req);
    if (r == 0) {
        printf("  count=%u\n", cnt);
        for (uint32_t i = 0; i < cnt && i < 16; i++)
            printf("    pc=0x%llx addr=0x%llx hits=%u\n",
                   (unsigned long long)recs[i].trigger_pc,
                   (unsigned long long)recs[i].hit_addr, recs[i].hits);
    }
    return (int)r;
}

static int cmd_clonebp_timing(const char *mode, const char *target, const char *jitter, const char *bailout)
{
    CLONEBP_TIMING_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode        = (uint32_t)strtoul(mode, NULL, 0);
    cfg.target_ns   = (uint32_t)atoi(target);
    cfg.jitter_ns   = (uint32_t)atoi(jitter);
    cfg.bailout_max = (uint32_t)atoi(bailout);
    long r = nl_call(OP_CLONEBP_TIMING, (uint64_t)&cfg);
    if (r == 0)
        printf("  cost=%llu pad=%llu evt=%llu\n",
               (unsigned long long)cfg.cost_ns, (unsigned long long)cfg.pad_ns,
               (unsigned long long)cfg.evt_cnt);
    return (int)r;
}

static int cmd_ace_stat(void)
{
    ACE_QUERY_REQUEST q;
    memset(&q, 0, sizeof(q));
    q.cmd = ACE_QUERY_STAT;
    long r = nl_call(OP_ACE_QUERY, (uint64_t)&q);
    if (r == 0) {
        ACE_STAT *st = &q.stat;
        printf("  tgid=%u features=0x%x maps=%llu mods=%llu ksym=%llu tracer=%llu fs=%llu comm=%llu port=%llu smaps=%llu fd=%llu prop=%llu\n",
               st->tgid, st->features,
               (unsigned long long)st->maps_drop, (unsigned long long)st->mod_drop,
               (unsigned long long)st->ksym_drop, (unsigned long long)st->tracerp_hide,
               (unsigned long long)st->fs_drop, (unsigned long long)st->comm_drop,
               (unsigned long long)st->port_drop, (unsigned long long)st->smaps_drop,
               (unsigned long long)st->fd_drop, (unsigned long long)st->prop_hit);
    }
    return (int)r;
}

static int cmd_ace_get_features(void)
{
    ACE_QUERY_REQUEST q;
    memset(&q, 0, sizeof(q));
    q.cmd = ACE_QUERY_FEATURES;
    long r = nl_call(OP_ACE_QUERY, (uint64_t)&q);
    if (r == 0)
        printf("  features=0x%08x\n", q.features);
    return (int)r;
}

static int cmd_ace_pkgname(const char *pidstr)
{
    ACE_QUERY_REQUEST q;
    memset(&q, 0, sizeof(q));
    q.cmd = ACE_QUERY_PKGNAME;
    q.pid = (uint32_t)atoi(pidstr);
    long r = nl_call(OP_ACE_QUERY, (uint64_t)&q);
    if (r == 0)
        printf("  pkgname=%s\n", q.pkgname);
    return (int)r;
}

/* ===================== 自检流程 ===================== */

static int selftest(const char *pidstr)
{
    pid_t pid = (pid_t)atoi(pidstr);
    long r;

    printf("== wind_test selftest pid=%d ==\n", pid);

    r = nl_call(OP_STATS, (uint64_t)0);
    /* OP_STATS 需要用户态缓冲, 但先探一下通道是否活着 */
    (void)r;

    r = cmd_bind(pidstr);                 print_ret("bind", r);
    r = cmd_stats();                      print_ret("stats", r);

    r = nl_call(OP_SET_MODE, (uint64_t)0);
    {
        MODE_REQUEST mr; mr.mode = MODE_FAST;
        r = nl_call(OP_SET_MODE, (uint64_t)&mr);
    }
    print_ret("set-mode FAST", r);

    r = cmd_module_base(pidstr, "libUE4.so");  print_ret("module-base libUE4.so", r);

    r = cmd_read("0x0", "8");             print_ret("read-mem 0x0", r);

    r = cmd_hide_file("/data/local/tmp/wind.ko"); print_ret("hide-file", r);
    r = nl_call(OP_UNHIDE_ALL, 0);        print_ret("unhide-all", r);

    r = nl_call(OP_KGSL_HOOK, 0);         print_ret("kgsl-hook", r);
    r = nl_call(OP_KGSL_HIDE, (uint64_t)(long)pid); print_ret("kgsl-hide", r);

    r = nl_call(OP_GYRO_INSTALL, (uint64_t)(long)pid); print_ret("gyro-install", r);
    {
        GYRO_REQUEST gr;
        gr.rx = parse_fbits("0.0");
        gr.ry = parse_fbits("0.0");
        gr.rz = parse_fbits("0.0");
        r = nl_call(OP_GYRO_SET, (uint64_t)&gr);
    }
    print_ret("gyro-set", r);
    r = nl_call(OP_GYRO_ENABLE, 1);       print_ret("gyro-enable 1", r);
    r = nl_call(OP_GYRO_STOP, 0);         print_ret("gyro-stop", r);
    r = nl_call(OP_GYRO_RESUME, 0);       print_ret("gyro-resume", r);
    r = nl_call(OP_GYRO_REMOVE, 0);       print_ret("gyro-remove", r);

    /* WRP 子弹追踪 (占位地址, 验证通道与返回值) */
    r = cmd_wrpbp_set("0", "0x7000000000", "8", "3", "7", "0", "0");
    print_ret("wrpbp-set", r);
    r = cmd_wrpbp_config();               print_ret("wrpbp-config", r);
    r = cmd_wrpbp_set_vec("-1", "1", "0.0", "0.0", "0.0", "7");
    print_ret("wrpbp-set-vec", r);
    r = cmd_wrpbp_set_fregs("-1", "0", "7", 3,
                            (char *[]){"0.0", "0.0", "0.0"});
    print_ret("wrpbp-set-fregs", r);
    r = cmd_wrpbp_records("0");           print_ret("wrpbp-records", r);
    r = nl_call(OP_WRPBP_CLEAR_RECORDS, (uint64_t)(long)-1); print_ret("wrpbp-clear-records", r);
    r = nl_call(OP_WRPBP_REMOVE, 0);      print_ret("wrpbp-remove", r);

    /* 克隆页断点 (占位地址) */
    r = cmd_clonebp_set("0", "0x7100000000", "1", "0", "0", "0");
    print_ret("clonebp-set", r);
    r = cmd_clonebp_config();             print_ret("clonebp-config", r);
    r = cmd_clonebp_records("0");         print_ret("clonebp-records", r);
    r = nl_call(OP_CLONEBP_REFRESH, 0);   print_ret("clonebp-refresh", r);
    r = cmd_clonebp_timing("1", "1000", "500", "256");
    print_ret("clonebp-timing", r);
    r = nl_call(OP_CLONEBP_REMOVE, 0);    print_ret("clonebp-remove", r);

    /* ACE (统一设置/查询 + 包名获取) */
    {
        ACE_SET_REQUEST s;
        memset(&s, 0, sizeof(s));
        s.tgid = (uint32_t)pid;
        s.features = 0xFFFFFFFFu;
        s.verbose = 2;
        s.inte = 2;
        r = nl_call(OP_ACE_SET, (uint64_t)&s); print_ret("ace-set", r);
    }
    r = cmd_ace_get_features();           print_ret("ace-get-features", r);
    r = cmd_ace_stat();                   print_ret("ace-stat", r);
    r = cmd_ace_pkgname(pidstr);          print_ret("ace-pkgname", r);

    r = nl_call(OP_UNBIND, 0);            print_ret("unbind", r);

    printf("== selftest done ==\n");
    return 0;
}

/* ===================== 命令路由 ===================== */

static void usage(const char *argv0)
{
    printf(
"Wind_driver 完整功能测试程序\n"
"\n"
"用法: %s selftest <pid>           跑完整自检\n"
"      %s <command> [args...]      单条命令\n"
"\n"
"命令:\n"
"  bind <pid>                     绑定目标进程 (先决条件)\n"
"  unbind                         解绑\n"
"  stats                          读页映射缓存统计\n"
"  set-mode <0|1>                 0=FAST 1=SAFE\n"
"  module-base <pid> <name>       取模块基址\n"
"  read <addr> <size>             读目标进程内存 (hex)\n"
"  write <addr> <byte[,byte..]>   写目标进程内存\n"
"  hide-file <path>               隐藏文件 (需先 bind)\n"
"  hide-process <pid>             隐藏进程\n"
"  unhide-all                     撤销所有隐藏\n"
"  kgsl-hook                      安装 kgsl 隐藏 hook\n"
"  kgsl-hide <pid>                登记 kgsl 隐藏 pid\n"
"  gyro-install <pid>             安装陀螺仪 hook\n"
"  gyro-remove                    卸载陀螺仪 hook\n"
"  gyro-set <rx> <ry> <rz>        设三轴角速度 (float 或 0x 位模式)\n"
"  gyro-enable <0|1>              开关陀螺仪篡改\n"
"  gyro-stop / gyro-resume        暂停/恢复\n"
"  wrpbp-set <pid> <addr> <len> <type> <comp> <fregbase> <fregmask>\n"
"  wrpbp-remove / wrpbp-config / wrpbp-records <idx> / wrpbp-clear-records\n"
"  wrpbp-set-vec <idx> <enable> <x> <y> <z> <mask>\n"
"  wrpbp-set-fregs <idx> <fregbase> <fregmask> <v0..v7 float>\n"
"  clonebp-set <pid> <addr> <mode> <regbase> <regmask> <imm>\n"
"  clonebp-remove / clonebp-config / clonebp-records <idx> / clonebp-refresh\n"
"  clonebp-timing <mode> <target_ns> <jitter_ns> <bailout>\n"
"  ace-set <tgid>                 设 ACE 目标\n"
"  ace-stat                       ACE 命中统计\n"
"  ace-verbose <0|1>              驱动日志开关\n"
"  ace-set-features <mask>        设伪装开关位掩码\n"
"  ace-get-features               读当前位掩码\n"
"  ace-inte <0|1>                 inte 节点接管/摘除\n"
"  ace-pkgname <pid>              取进程包名 (cmdline argv[0])\n"
"\n"
"浮点字段: 十进制按 float 解析, 0x 前缀按 IEEE754 位模式直通。\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
    long r;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (nl_open() < 0)
        return 1;

    const char *cmd = argv[1];

    if (!strcmp(cmd, "selftest")) {
        if (argc < 3) { fprintf(stderr, "selftest 需要 pid\n"); return 1; }
        return selftest(argv[2]);
    }

    if (!strcmp(cmd, "bind"))        r = cmd_bind(argv[2]);
    else if (!strcmp(cmd, "unbind")) r = nl_call(OP_UNBIND, 0);
    else if (!strcmp(cmd, "stats"))  r = cmd_stats();
    else if (!strcmp(cmd, "set-mode")) r = ({ MODE_REQUEST mr; mr.mode=(uint32_t)atoi(argv[2]); nl_call(OP_SET_MODE,(uint64_t)&mr); });
    else if (!strcmp(cmd, "module-base")) r = cmd_module_base(argv[2], argv[3]);
    else if (!strcmp(cmd, "read"))   r = cmd_read(argv[2], argv[3]);
    else if (!strcmp(cmd, "write"))  r = cmd_write(argv[2], argv[3]);
    else if (!strcmp(cmd, "hide-file")) r = cmd_hide_file(argv[2]);
    else if (!strcmp(cmd, "hide-process")) r = nl_call(OP_HIDE_PROCESS, (uint64_t)(long)atoi(argv[2]));
    else if (!strcmp(cmd, "unhide-all")) r = nl_call(OP_UNHIDE_ALL, 0);
    else if (!strcmp(cmd, "kgsl-hook")) r = nl_call(OP_KGSL_HOOK, 0);
    else if (!strcmp(cmd, "kgsl-hide")) r = nl_call(OP_KGSL_HIDE, (uint64_t)(long)atoi(argv[2]));
    else if (!strcmp(cmd, "gyro-install")) r = nl_call(OP_GYRO_INSTALL, (uint64_t)(long)atoi(argv[2]));
    else if (!strcmp(cmd, "gyro-remove")) r = nl_call(OP_GYRO_REMOVE, 0);
    else if (!strcmp(cmd, "gyro-set")) {
        GYRO_REQUEST gr;
        gr.rx = parse_fbits(argv[2]); gr.ry = parse_fbits(argv[3]); gr.rz = parse_fbits(argv[4]);
        r = nl_call(OP_GYRO_SET, (uint64_t)&gr);
    }
    else if (!strcmp(cmd, "gyro-enable")) r = nl_call(OP_GYRO_ENABLE, (uint64_t)atoi(argv[2]));
    else if (!strcmp(cmd, "gyro-stop")) r = nl_call(OP_GYRO_STOP, 0);
    else if (!strcmp(cmd, "gyro-resume")) r = nl_call(OP_GYRO_RESUME, 0);
    else if (!strcmp(cmd, "wrpbp-set")) r = cmd_wrpbp_set(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7],argv[8]);
    else if (!strcmp(cmd, "wrpbp-remove")) r = nl_call(OP_WRPBP_REMOVE, 0);
    else if (!strcmp(cmd, "wrpbp-config")) r = cmd_wrpbp_config();
    else if (!strcmp(cmd, "wrpbp-records")) r = cmd_wrpbp_records(argv[2]);
    else if (!strcmp(cmd, "wrpbp-clear-records")) r = nl_call(OP_WRPBP_CLEAR_RECORDS, (uint64_t)(long)atoi(argv[2]));
    else if (!strcmp(cmd, "wrpbp-set-vec")) r = cmd_wrpbp_set_vec(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);
    else if (!strcmp(cmd, "wrpbp-set-fregs")) r = cmd_wrpbp_set_fregs(argv[2],argv[3],argv[4],argc-5,argv+5);
    else if (!strcmp(cmd, "clonebp-set")) r = cmd_clonebp_set(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);
    else if (!strcmp(cmd, "clonebp-remove")) r = nl_call(OP_CLONEBP_REMOVE, 0);
    else if (!strcmp(cmd, "clonebp-config")) r = cmd_clonebp_config();
    else if (!strcmp(cmd, "clonebp-records")) r = cmd_clonebp_records(argv[2]);
    else if (!strcmp(cmd, "clonebp-clear-records")) r = nl_call(OP_CLONEBP_CLEAR_RECORDS, (uint64_t)(long)atoi(argv[2]));
    else if (!strcmp(cmd, "clonebp-refresh")) r = nl_call(OP_CLONEBP_REFRESH, 0);
    else if (!strcmp(cmd, "clonebp-timing")) r = cmd_clonebp_timing(argv[2],argv[3],argv[4],argv[5]);
    else if (!strcmp(cmd, "ace-set")) {
        ACE_SET_REQUEST s;
        memset(&s, 0, sizeof(s));
        s.tgid = (uint32_t)atoi(argv[2]);
        s.features = 0xFFFFFFFFu;
        s.verbose = 2;
        s.inte = 2;
        r = nl_call(OP_ACE_SET, (uint64_t)&s);
    }
    else if (!strcmp(cmd, "ace-stat")) r = cmd_ace_stat();
    else if (!strcmp(cmd, "ace-pkgname")) r = cmd_ace_pkgname(argv[2]);
    else if (!strcmp(cmd, "ace-verbose")) {
        ACE_SET_REQUEST s;
        memset(&s, 0, sizeof(s));
        s.features = 0xFFFFFFFFu;
        s.verbose = (uint32_t)atoi(argv[2]);
        s.inte = 2;
        r = nl_call(OP_ACE_SET, (uint64_t)&s);
    }
    else if (!strcmp(cmd, "ace-set-features")) {
        ACE_SET_REQUEST s;
        memset(&s, 0, sizeof(s));
        s.features = (uint32_t)strtoul(argv[2], NULL, 0);
        s.verbose = 2;
        s.inte = 2;
        r = nl_call(OP_ACE_SET, (uint64_t)&s);
    }
    else if (!strcmp(cmd, "ace-get-features")) r = cmd_ace_get_features();
    else if (!strcmp(cmd, "ace-inte")) {
        ACE_SET_REQUEST s;
        memset(&s, 0, sizeof(s));
        s.features = 0xFFFFFFFFu;
        s.verbose = 2;
        s.inte = (uint32_t)atoi(argv[2]);
        r = nl_call(OP_ACE_SET, (uint64_t)&s);
    }
    else {
        fprintf(stderr, "未知命令: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }

    print_ret(cmd, r);
    return (r < 0) ? 1 : 0;
}
