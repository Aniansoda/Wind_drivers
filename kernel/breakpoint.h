#ifndef _BREAKPOINT_H_
#define _BREAKPOINT_H_

#include <linux/kernel.h>

int brk_set(pid_t pid, uintptr_t addr, int type);
int brk_clear(pid_t pid, uintptr_t addr);

#endif /* _BREAKPOINT_H_ */
