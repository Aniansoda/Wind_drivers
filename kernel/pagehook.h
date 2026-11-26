/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PAGEHOOK_H_
#define _PAGEHOOK_H_

#include <linux/types.h>
#include <linux/mm.h>
#include "comm.h"   /* PAGEHOOK_STAT / PAGE_HOOK_VIEW_DATA / PAGE_HOOK_PATCH_MAX */

#define PH_MAX_POINTS      64
#define PH_PATCH_MAX       PAGE_HOOK_PATCH_MAX
#define PH_VIEW_DATA       PAGE_HOOK_VIEW_DATA

int  pagehook_module_init(void);
void pagehook_module_exit(void);
int  pagehook_install(struct mm_struct *mm, u64 addr, const u8 *patch, u32 patch_len);
int  pagehook_remove(struct mm_struct *mm, u64 addr);
int  pagehook_set_view(struct mm_struct *mm, u64 addr, u32 view);
void pagehook_get_stat(PAGEHOOK_STAT *out);

#endif /* _PAGEHOOK_H_ */
