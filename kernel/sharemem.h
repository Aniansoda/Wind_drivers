/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SHAREMEM_H_
#define _SHAREMEM_H_

#include <linux/types.h>
#include <linux/mm.h>
#include "comm.h"

#define SHAREMEM_MAX_REGIONS  8
#define SHAREMEM_MAX_SIZE     (64ULL * 1024 * 1024)

int  sharemem_module_init(void);
void sharemem_module_exit(void);
int  sharemem_alloc(struct mm_struct *target_mm, struct mm_struct *caller_mm,
		    u64 size, u32 prot, u64 *target_va_out, u64 *caller_va_out,
		    u64 *size_out);
int  sharemem_free(struct mm_struct *target_mm, u64 target_va);
void sharemem_release_mm(struct mm_struct *mm);

#endif /* _SHAREMEM_H_ */
