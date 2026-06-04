/*
 * Lightweight diagnostic breadcrumbs for field builds.
 */

#ifndef _DIAG_H_
#define _DIAG_H_

#include <utils/types.h>

#ifdef LP_DIAG_LOG
void diag_log(const char *msg);
void diag_log_u32(const char *name, u32 value);
void diag_log_u32_pair(const char *name, u32 value_a, u32 value_b);
void diag_screen_step(u32 step, const char *msg);
#else
#define diag_log(msg) ((void)0)
#define diag_log_u32(name, value) ((void)0)
#define diag_log_u32_pair(name, value_a, value_b) ((void)0)
#define diag_screen_step(step, msg) ((void)0)
#endif

#endif /* _DIAG_H_ */
