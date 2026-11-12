#ifndef _WINCFG_H_
#define _WINCFG_H_

#include <linux/types.h>

/* 刷入器在 insmod 时一次性决定; 加载后不改。
 * 全部默认开启 —— 用户态不再提供任何开关入口。 */
int  windcfg_init(void);
void windcfg_exit(void);

/* 目标进程自动绑定: 由内核自己扫 task 列表,
 * 不依赖用户态 ioctl, 也不要求 insmod 时游戏已经在跑。 */
pid_t windcfg_target_tgid(void);

#endif /* _WINCFG_H_ */
