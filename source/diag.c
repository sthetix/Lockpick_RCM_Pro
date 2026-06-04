/*
 * Lightweight diagnostic breadcrumbs for field builds.
 */

#include "config.h"
#include "diag.h"

#ifdef LP_DIAG_LOG

#include "gfx/gfx.h"
#include <libs/fatfs/ff.h>
#include <storage/nx_sd.h>
#include <utils/sprintf.h>
#include <utils/util.h>

extern hekate_config h_cfg;

static bool diag_try_mount_sd()
{
	return sd_mount();
}

void diag_log(const char *msg)
{
	if (!h_cfg.diagnostic_log)
		return;

	if (!diag_try_mount_sd())
		return;

	f_mkdir("sd:/switch");

	FIL fp;
	if (f_open(&fp, "sd:/switch/lockpick_diag.log", FA_OPEN_APPEND | FA_WRITE) == FR_OK)
	{
		char line[256];
		u32 len = s_printf(line, "[%d ms] %s\n", get_tmr_ms(), msg);
		f_write(&fp, line, len, NULL);
		f_close(&fp);
	}
}

void diag_log_u32(const char *name, u32 value)
{
	if (!h_cfg.diagnostic_log)
		return;

	char msg[192];
	s_printf(msg, "%s=%d", name, value);

	diag_log(msg);
}

void diag_log_u32_pair(const char *name, u32 value_a, u32 value_b)
{
	if (!h_cfg.diagnostic_log)
		return;

	char msg[192];
	s_printf(msg, "%s=%d/%d", name, value_a, value_b);

	diag_log(msg);
}

void diag_screen_step(u32 step, const char *msg)
{
	if (!h_cfg.diagnostic_log)
		return;

	gfx_printf("%kDBG %02d: %s\n", COLOR_YELLOW, step, msg);
	char log_msg[192];
	s_printf(log_msg, "DBG %d: %s", step, msg);
	diag_log(log_msg);
}

#endif
