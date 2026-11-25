#ifndef _ACE_H_
#define _ACE_H_

#include <linux/types.h>

int  ace_init(void);
void ace_exit(void);
int  ace_set_pid(uint32_t tgid);
int  ace_clear_pid(void);
int  ace_get_stat(void *uout);
int  ace_set_verbose(int on);
int  ace_inte_takeover(int remove_only);
int  ace_set_features(uint32_t mask);
uint32_t ace_get_features(void);

/* 统一设置 (OP_ACE_SET) */
int  ace_ctrl_set(const void __user *uarg);
/* 统一查询 (OP_ACE_QUERY): stat/features/pkgname */
int  ace_ctrl_query(void __user *uarg);
/* 包名获取: 读 pid 进程 cmdline argv[0] */
int  ace_get_pkgname(uint32_t pid, char *buf, size_t len);

#endif
