#ifndef _WINCFG_H_
#define _WINCFG_H_

#include <linux/types.h>

/* 刷入期配置: 默认全开, 加载后不改, 用户态不提供任何开关入口 */
int  windcfg_init(void);
void windcfg_exit(void);

/* 目标进程由用户态传进来 —— 驱动不猜。
 * 用户态 OP_BIND 成功之后调用, 这里做 ACE 绑定 + 进程隐藏 + GPU 隐藏。 */
void windcfg_bind_pid(pid_t tgid);

bool windcfg_gyro_on(void);

#endif /* _WINCFG_H_ */
