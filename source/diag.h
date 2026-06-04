/*
 * Lightweight diagnostic breadcrumbs for field builds.
 */

#ifndef _DIAG_H_
#define _DIAG_H_

#include <utils/types.h>

void diag_log(const char *msg);
void diag_log_u32(const char *name, u32 value);
void diag_log_u32_pair(const char *name, u32 value_a, u32 value_b);
void diag_screen_step(u32 step, const char *msg);

#endif /* _DIAG_H_ */
