#ifndef _PROCESS_H_
#define _PROCESS_H_

#include <linux/kernel.h>
#include "memory.h"

uintptr_t get_module_base(struct driver_session *sess, char *name);

/* 隐藏接口 (见 process.c 实现, 依赖内核版本, 未验证项见注释) */
int hide_file(const char *path);
int hide_process(pid_t pid);
int unhide_all(void);

#endif /* _PROCESS_H_ */
