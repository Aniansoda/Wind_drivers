#ifndef _KGSL_H_
#define _KGSL_H_

#include <linux/kernel.h>

int kgsl_hide_pid(pid_t pid);
bool kgsl_is_hidden(pid_t pid);
int kgsl_hook_install(void);
void kgsl_hook_remove(void);

#endif /* _KGSL_H_ */
