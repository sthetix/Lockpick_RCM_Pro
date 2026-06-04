/*
 * Copyright (c) 2018-2020 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>

#include "config.h"
#include <utils/ini.h>
#include <gfx_utils.h>
#include "gfx/tui.h"
#include <libs/fatfs/ff.h>
#include <soc/fuse.h>
#include <soc/hw_init.h>
#include <soc/t210.h>
#include <storage/nx_sd.h>
#include <storage/sdmmc.h>
#include <utils/btn.h>
#include <utils/list.h>
#include <utils/util.h>

extern hekate_config h_cfg;

void set_default_configuration()
{
	h_cfg.autoboot = 0;
	h_cfg.autoboot_list = 0;
	h_cfg.bootwait = 3;
	h_cfg.backlight = 100;
	h_cfg.autohosoff = 0;
	h_cfg.autonogc = 1;
	h_cfg.updater2p = 0;
	h_cfg.bootprotect = 0;
	h_cfg.errors = 0;
	h_cfg.aes_slots_new = false;
	h_cfg.rcm_patched = fuse_check_patched_rcm();
	h_cfg.emummc_force_disable = false;
	h_cfg.t210b01 = hw_get_chip_id() == GP_HIDREV_MAJOR_T210B01;
	h_cfg.verbose_errors = false;
	h_cfg.diagnostic_log = false;
}

void load_lockpick_configuration()
{
	LIST_INIT(ini_sections);
	if (!ini_parse(&ini_sections, "config/lockpick_rcm_pro/config.ini", false))
		return;

	LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
	{
		if (ini_sec->type != INI_CHOICE || strcmp(ini_sec->name, "debug"))
			continue;

		LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
		{
			if (!strcmp("verbose_errors", kv->key))
				h_cfg.verbose_errors = atoi(kv->val) != 0;
			else if (!strcmp("diagnostic_log", kv->key))
				h_cfg.diagnostic_log = atoi(kv->val) != 0;
		}
		break;
	}
}
