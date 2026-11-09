#ifndef _BREAKPOINT_H_
#define _BREAKPOINT_H_

#include <linux/kernel.h>
#include "comm.h"

struct driver_session;
struct kprobe;
struct pt_regs;

/* 异常/fault handler kprobe hook (见 sym.c 实现, 白名单外符号运行时注册) */
struct kprobe *wind_kprobe_register_symbol(const char *name,
					  int (*pre)(struct kprobe *, struct pt_regs *),
					  void (*post)(struct kprobe *, struct pt_regs *, unsigned long));
void wind_kprobe_unregister_symbol(struct kprobe *kp);

/* ---- 旧 brk (perf hw_breakpoint, 单点, 兼容) ---- */
int brk_set(pid_t pid, uintptr_t addr, int type);
int brk_clear(pid_t pid, uintptr_t addr);

/* ---- 硬件断点 (BRP/WRP debug 寄存器, 16 点 + 记录) ---- */
int hwbp_set(struct driver_session *sess, const HWBP_POINT_CONFIG *points, size_t point_count);
int hwbp_set_reg_modify(int point_index, u32 enable, u32 rx, u32 ry, u32 rz,
			u32 mask);
int hwbp_refresh(void);
int hwbp_remove(void);
int hwbp_get_config(HWBP_CONFIG *config_out);
int hwbp_get_records(uint32_t point_index, uint32_t start_index,
		     HWBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int hwbp_set_record(uint32_t point_index, uint32_t record_index,
		    const HWBP_RECORD *record);
int hwbp_clear_records(int32_t point_index);

/* ---- PTE 影子页执行断点 (16 点 + 记录, 不用 debug 寄存器) ---- */
int ptebp_set(struct driver_session *sess, const PTEBP_POINT_CONFIG *points, size_t point_count);
int ptebp_set_regs(int point_index, u32 enable, u32 rx, u32 ry, u32 rz,
		   u32 mask);
int ptebp_refresh(void);
int ptebp_remove(void);
int ptebp_get_config(PTEBP_CONFIG *config_out);
int ptebp_get_records(uint32_t point_index, uint32_t start_index,
		      PTEBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int ptebp_set_record(uint32_t point_index, uint32_t record_index,
		     const PTEBP_RECORD *record);
int ptebp_clear_records(int32_t point_index);

/* ---- BRK 软断点 (16 点 + 记录, 改写指令字节) ---- */
int brkbp_set(struct driver_session *sess, const BRKBP_POINT_CONFIG *points, size_t point_count);
int brkbp_remove(void);
int brkbp_get_config(BRKBP_CONFIG *config_out);
int brkbp_get_records(uint32_t point_index, uint32_t start_index,
		      BRKBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int brkbp_set_record(uint32_t point_index, uint32_t record_index,
		     const BRKBP_RECORD *record);
int brkbp_clear_records(int32_t point_index);

/* ---- WRP 数据断点追踪 (观察点 + 命中写回, 最隐蔽的一族) ---- */
int wrpbp_set(struct driver_session *sess, const WRPBP_POINT_CONFIG *points,
	      size_t point_count);
int wrpbp_set_vec(int32_t point_index, u32 enable, u32 x, u32 y, u32 z, u32 mask);
int wrpbp_get_config(WRPBP_CONFIG *out);
int wrpbp_get_records(uint32_t point_index, uint32_t start_index,
		      HWBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int wrpbp_clear_records(int32_t point_index);
int wrpbp_remove(void);

/* ---- 三族诊断快照 ---- */
int bp_diag_get(WIND_BP_DIAG *out);

/* ---- 三族一次性全关 (拆除 + 关改写 + 清记录), 换追踪方式时用 ---- */
int bp_all_off(void);


/* ---- UDP 断点 (kprobe udp_send_skb / udp_queue_rcv_skb, 按 pid/端口/方向过滤) ---- */
int udpbp_set(int32_t slot_index, const UDPBP_POINT_CONFIG *point, int32_t *slot_out);
int udpbp_remove(int32_t slot_index);
int udpbp_get_config(UDPBP_CONFIG *config_out);
int udpbp_get_records(int32_t slot_index, uint32_t start_index,
		      UDPBP_RECORD *records, uint32_t capacity, uint32_t *count_out);
int udpbp_clear_records(int32_t slot_index);

/* 卸载时清理全部断点框架 */
void wind_bp_exit(void);

#endif /* _BREAKPOINT_H_ */
