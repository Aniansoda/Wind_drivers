#ifndef _WIND_SYM_H_
#define _WIND_SYM_H_

/*
 * sym.h — GKI 白名单外符号运行时解析层
 *
 * GKI 冻结 KMI, vendor 模块只能引用白名单符号 (abi_symbollist 交集)。
 * 本驱动用到的若干内核函数不在官方 prebuilt .ko 的 __versions 段里
 * (modpost 报 undefined, 真机按冻结 CRC 加载时对不上), 统一改用
 * register_kprobe (EXPORT_SYMBOL_GPL, 白名单内) 按符号名取函数地址,
 * 编译期不产生对这些符号的 CRC 引用。
 *
 * 只适用于函数符号 (KALLSYMS_ALL=y 下所有非 static 函数都在 kallsyms 表)。
 * 数据符号 (memstart_addr / cache_line_size 等) 拿不到, 由 compat.h
 * 用白名单内联指令或硬编码替代, 不依赖数据符号。
 */

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/page.h>
#include <asm/pgtable.h>

struct mm_struct;
struct vm_area_struct;
struct vm_struct;
struct file;
struct perf_event;
struct perf_event_attr;
struct perf_sample_data;
struct pt_regs;
struct task_struct;
struct kprobe;

/* 内核 perf_overflow_handler_t 定义在 linux/perf_event.h, 这里 forward 声明等价签名;
 * sym.c 内部用内核真实类型接收, breakpoint.c/gyro.c 的回调函数签名一致可隐式转换。 */
typedef void (*wind_perf_overflow_handler_t)(struct perf_event *,
					     struct perf_sample_data *,
					     struct pt_regs *);

/* 重写的 arch 内联逻辑 */
pte_t wind_ptep_get(pte_t *ptep);
unsigned long wind_clear_user(void __user *to, unsigned long n);

/* mm 借用 (真机未导出 kthread_use_mm/kthread_unuse_mm, 运行时解析) */
int wind_kthread_use_mm(struct mm_struct *mm);
void wind_kthread_unuse_mm(struct mm_struct *mm);

/* 重写的页表权限 (arm64 set_memory_* 非白名单) */
int wind_set_memory_rw(unsigned long addr, int numpages);
int wind_set_memory_ro(unsigned long addr, int numpages);

/* 运行时解析的内核函数 (白名单外, register_kprobe 取地址) */
void wind_mmput(struct mm_struct *mm);
void wind_iounmap(volatile void __iomem *addr);
int wind_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
			 unsigned long pfn, unsigned long size, pgprot_t prot);

/* vmap 三件套 (白名单外, 运行时解析) */
void wind_free_vm_area(struct vm_struct *area);
void wind_vunmap(const void *addr, struct vm_struct *area);
struct vm_struct *wind_get_vm_area_caller(unsigned long size, unsigned long flags,
					  unsigned long start, unsigned long end,
					  const void *caller);
int wind_ioremap_page_range(unsigned long addr, unsigned long end,
			    phys_addr_t phys_addr, pgprot_t prot);

/* 文件操作 (白名单外, 运行时解析) */
struct file *wind_filp_open(const char *filename, int flags, umode_t mode);
ssize_t wind_kernel_read(struct file *file, void *buf, size_t count, loff_t *pos);
char *wind_file_path(struct file *filp, char *buf, int buflen);

/* 私有字符设备 (白名单外, 运行时解析) */
struct miscdevice;
int wind_misc_register(struct miscdevice *misc);
void wind_misc_deregister(struct miscdevice *misc);

/* netlink 私有通道 (白名单外, 运行时解析; 编译期不产生 CRC 引用) */
struct sock;
struct sk_buff;
struct net;
struct netlink_kernel_cfg;
struct sock *wind_netlink_kernel_create(struct net *net, int unit,
					struct netlink_kernel_cfg *cfg);
void wind_netlink_kernel_release(struct sock *sk);
int wind_netlink_unicast(struct sock *ssk, struct sk_buff *skb, u32 portid, int nonblock);
struct sk_buff *wind_alloc_skb(unsigned int size, gfp_t gfp);
bool wind_netlink_available(void);

/* 硬件断点 (白名单外, 运行时解析) */
struct perf_event *wind_register_user_hw_breakpoint(struct perf_event_attr *attr,
						    wind_perf_overflow_handler_t triggered,
						    void *context, struct task_struct *tsk);
void wind_unregister_hw_breakpoint(struct perf_event *bp);

/* per-CPU 全域硬件断点: 绑进程主线程的 user 断点抓不到工作线程
 * (system_server 的 sensor 投递在 binder 线程上跑), 这类场景必须用
 * 全域断点 + 命中后按 tgid 过滤。返回 __percpu 指针数组, 失败返回 ERR_PTR。 */
struct perf_event * __percpu *wind_register_wide_hw_breakpoint(struct perf_event_attr *attr,
							      wind_perf_overflow_handler_t triggered,
							      void *context);
void wind_unregister_wide_hw_breakpoint(struct perf_event * __percpu *cpu_events);

/* 把当前线程的 FP/SIMD 状态落盘到 task_struct (白名单外, 运行时解析)。
 * 命中改写 v3/v4/v5 前必须先同步, 否则内存里是旧值, 回用户态重载会打乱
 * 其余寄存器。解析不到返回 false, 调用方必须放弃改写。 */
bool wind_fpsimd_preserve_current_state(void);

/* nofault 拷贝 (白名单外, 运行时解析) */
long wind_copy_from_user_nofault(void *dst, const void __user *src, size_t size);
long wind_copy_to_user_nofault(void __user *dst, const void *src, size_t size);

/* 异常/fault handler kprobe hook (白名单外符号, 运行时注册)。
 * 用于影子页断点捕获 fault / BRK 断点捕获异常。
 * 注意: arm64 的 do_mem_abort / do_page_fault / brk_handler 等可能受
 * NOKPROBE 保护, register_kprobe 会失败 (返回 NULL), 调用方需处理降级。
 */
struct kprobe *wind_kprobe_register_symbol(const char *name,
					  int (*pre)(struct kprobe *, struct pt_regs *),
					  void (*post)(struct kprobe *, struct pt_regs *, unsigned long));
void wind_kprobe_unregister_symbol(struct kprobe *kp);

/* 解析任意符号地址（含 NOKPROBE 保护的符号）。
 * kallsyms_lookup_name 的 export 被 GKI 移除, 但其符号本身非 static 非
 * NOKPROBE, 可先用 register_kprobe 解析它的地址, 再调用它解析目标符号。
 * 用于 ftrace hook do_mem_abort(NOKPROBE, kprobe 解析不到)。
 * 返回 0 表示解析失败。 */
unsigned long wind_kallsyms_lookup_name(const char *name);

int wind_sym_init(void);
void wind_sym_exit(void);

#endif /* _WIND_SYM_H_ */
