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

#endif
