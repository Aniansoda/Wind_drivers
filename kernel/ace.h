#ifndef _ACE_H_
#define _ACE_H_

#include <linux/types.h>

int  ace_init(void);
void ace_exit(void);
int  ace_set_pid(uint32_t tgid);
int  ace_clear_pid(void);
int  ace_get_stat(void *uout);

#endif
