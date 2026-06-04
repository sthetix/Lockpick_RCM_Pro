/*
 * Copyright (c) 2018 naehrwert
 *
 * Copyright (c) 2018-2021 CTCaer
 * Copyright (c) 2019-2021 shchmue
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

#include "config.h"
#include "diag.h"
#include <display/di.h>
#include <gfx_utils.h>
#include "gfx/gfx.h"
#include "gfx/tui.h"
#include "hid/hid.h"
#include "keys/keys.h"
#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <mem/minerva.h>
#include <power/bq24193.h>
#include <power/max17050.h>
#include <power/max77620.h>
#include <rtc/max77620-rtc.h>
#include <soc/bpmp.h>
#include <soc/hw_init.h>
#include "storage/emummc.h"
#include "storage/nx_emmc.h"
#include "storage/nx_emmc_bis.h"
#include <storage/nx_sd.h>
#include <storage/sdmmc.h>
#include <utils/btn.h>
#include <input/touch.h>
#include <utils/dirlist.h>
#include <utils/ini.h>
#include <utils/list.h>
#include <utils/sprintf.h>
#include <utils/util.h>

#include "keys/keys.h"

hekate_config h_cfg;
boot_cfg_t __attribute__((section ("._boot_cfg"))) b_cfg;
const volatile ipl_ver_meta_t __attribute__((section ("._ipl_version"))) ipl_ver = {
	.magic = LP_MAGIC,
	.version = (LP_VER_MJ + '0') | ((LP_VER_MN + '0') << 8) | ((LP_VER_BF + '0') << 16),
	.rsvd0 = 0,
	.rsvd1 = 0
};

volatile nyx_storage_t *nyx_str = (nyx_storage_t *)NYX_STORAGE_ADDR;

// This is a safe and unused DRAM region for our payloads.
#define RELOC_META_OFF      0x7C
#define PATCHED_RELOC_SZ    0x94
#define PATCHED_RELOC_STACK 0x40007000
#define PATCHED_RELOC_ENTRY 0x40010000
#define EXT_PAYLOAD_ADDR    0xC0000000
#define RCM_PAYLOAD_ADDR    (EXT_PAYLOAD_ADDR + ALIGN(PATCHED_RELOC_SZ, 0x10))
#define COREBOOT_END_ADDR   0xD0000000
#define COREBOOT_VER_OFF    0x41
#define CBFS_DRAM_EN_ADDR   0x4003e000
#define  CBFS_DRAM_MAGIC    0x4452414D // "DRAM"

static void *coreboot_addr;

void reloc_patcher(u32 payload_dst, u32 payload_src, u32 payload_size)
{
	memcpy((u8 *)payload_src, (u8 *)IPL_LOAD_ADDR, PATCHED_RELOC_SZ);

	volatile reloc_meta_t *relocator = (reloc_meta_t *)(payload_src + RELOC_META_OFF);

	relocator->start = payload_dst - ALIGN(PATCHED_RELOC_SZ, 0x10);
	relocator->stack = PATCHED_RELOC_STACK;
	relocator->end   = payload_dst + payload_size;
	relocator->ep    = payload_dst;

	if (payload_size == 0x7000)
	{
		memcpy((u8 *)(payload_src + ALIGN(PATCHED_RELOC_SZ, 0x10)), coreboot_addr, 0x7000); //Bootblock
		*(vu32 *)CBFS_DRAM_EN_ADDR = CBFS_DRAM_MAGIC;
	}
}

int launch_payload(char *path, bool clear_screen)
{
	if (clear_screen)
		gfx_clear_grey(0x1B);
	gfx_con_setpos(0, 0);
	if (!path)
		return 1;

	if (sd_mount())
	{
		FIL fp;
		if (f_open(&fp, path, FA_READ))
		{
			gfx_con.mute = false;
			EPRINTFARGS("Payload file is missing!\n(%s)", path);

			goto out;
		}

		// Read and copy the payload to our chosen address
		void *buf;
		u32 size = f_size(&fp);

		if (size < 0x30000)
			buf = (void *)RCM_PAYLOAD_ADDR;
		else
		{
			coreboot_addr = (void *)(COREBOOT_END_ADDR - size);
			buf = coreboot_addr;
			if (h_cfg.t210b01)
			{
				f_close(&fp);

				gfx_con.mute = false;
				EPRINTF("Coreboot not allowed on Mariko!");

				goto out;
			}
		}

		if (f_read(&fp, buf, size, NULL))
		{
			f_close(&fp);

			goto out;
		}

		f_close(&fp);

		sd_end();

		if (size < 0x30000)
		{
			reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, ALIGN(size, 0x10));

			hw_reinit_workaround(false, byte_swap_32(*(u32 *)(buf + size - sizeof(u32))));
		}
		else
		{
			reloc_patcher(PATCHED_RELOC_ENTRY, EXT_PAYLOAD_ADDR, 0x7000);

			// Get coreboot seamless display magic.
			u32 magic = 0;
			char *magic_ptr = buf + COREBOOT_VER_OFF;
			memcpy(&magic, magic_ptr + strlen(magic_ptr) - 4, 4);
			hw_reinit_workaround(true, magic);
		}

		// Some cards (Sandisk U1), do not like a fast power cycle. Wait min 100ms.
		sdmmc_storage_init_wait_sd();

		void (*ext_payload_ptr)() = (void *)EXT_PAYLOAD_ADDR;

		// Launch our payload.
		(*ext_payload_ptr)();
	}

out:
	sd_end();
	return 1;
}

void launch_tools()
{
	u8 max_entries = 61;
	char *filelist = NULL;
	char *file_sec = NULL;
	char *dir = NULL;

	ment_t *ments = (ment_t *)malloc(sizeof(ment_t) * (max_entries + 3));

	gfx_clear_grey(0x1B);
	gfx_con_setpos(0, 0);

	if (sd_mount())
	{
		dir = (char *)malloc(256);

		memcpy(dir, "sd:/bootloader/payloads", 24);

		filelist = dirlist(dir, NULL, false, false);

		u32 i = 0;
		u32 i_off = 2;

		if (filelist)
		{
			// Build configuration menu.
			u32 color_idx = 0;

			ments[0].type = MENT_BACK;
			ments[0].caption = "Back";
			ments[0].color = colors[(color_idx++) % 6];
			ments[1].type = MENT_CHGLINE;
			ments[1].color = colors[(color_idx++) % 6];
			if (!f_stat("sd:/atmosphere/reboot_payload.bin", NULL))
			{
				ments[i_off].type = INI_CHOICE;
				ments[i_off].caption = "reboot_payload.bin";
				ments[i_off].color = colors[(color_idx++) % 6];
				ments[i_off].data = "sd:/atmosphere/reboot_payload.bin";
				i_off++;
			}
			if (!f_stat("sd:/ReiNX.bin", NULL))
			{
				ments[i_off].type = INI_CHOICE;
				ments[i_off].caption = "ReiNX.bin";
				ments[i_off].color = colors[(color_idx++) % 6];
				ments[i_off].data = "sd:/ReiNX.bin";
				i_off++;
			}

			while (true)
			{
				if (i > max_entries || !filelist[i * 256])
					break;
				ments[i + i_off].type = INI_CHOICE;
				ments[i + i_off].caption = &filelist[i * 256];
				ments[i + i_off].color = colors[(color_idx++) % 6];
				ments[i + i_off].data = &filelist[i * 256];

				i++;
			}
		}

		if (i > 0 || i_off > 2)
		{
			memset(&ments[i + i_off], 0, sizeof(ment_t));
			menu_t menu = { ments, "Choose file", 0, 0 };

			file_sec = (char *)tui_do_menu(&menu);

			if (!file_sec)
			{
				free(ments);
				free(dir);
				free(filelist);
				sd_end();

				gfx_clear_grey(0x1B);
				return;
			}
		}
		else
			EPRINTF("No payloads or modules found.");

		free(ments);
		free(filelist);
	}
	else
	{
		free(ments);
		goto out;
	}

	if (file_sec)
	{
		if (memcmp("sd:/", file_sec, 4) != 0)
		{
			memcpy(dir + strlen(dir), "/", 2);
			memcpy(dir + strlen(dir), file_sec, strlen(file_sec) + 1);
		}
		else
			memcpy(dir, file_sec, strlen(file_sec) + 1);

		launch_payload(dir, true);
		EPRINTF("Failed to launch payload.");
	}

out:
	sd_end();
	free(dir);

	hidWait();
}

void launch_hekate()
{
	sd_mount();
	if (!f_stat("bootloader/update.bin", NULL))
		launch_payload("bootloader/update.bin", false);
	else
	{
		gfx_clear_grey(0x1B);
		gfx_con_setpos(0, 0);
		EPRINTF("bootloader/update.bin not found!");
		hidWait();
	}
}

void dump_sysnand()
{
	h_cfg.emummc_force_disable = true;
	emu_cfg.enabled = false;
	dump_keys();
	h_cfg.emummc_force_disable = false; // Reset flag to allow subsequent emuMMC dumps
}

void dump_emunand()
{
	if (h_cfg.emummc_force_disable)
		return;
	emu_cfg.enabled = true;
	dump_keys();
}

void dump_amiibo_keys()
{
	gfx_clear_grey(0x1B);
	gfx_con_setpos(0, 0);
	derive_amiibo_keys();
}

void dump_prodinfo();
void restore_prodinfo();

void dump_mariko_partial_keys();

ment_t ment_partials[] = {
	MDEF_BACK(COLOR_TURQUOISE),
	MDEF_CHGLINE(),
	MDEF_CAPTION("Dumps results of writing zeros", COLOR_SOFT_WHITE),
	MDEF_CAPTION("over 32-bit portions of each keyslot", COLOR_SOFT_WHITE),
	MDEF_CAPTION("for bruteforce recovery on PC.", COLOR_SOFT_WHITE),
	MDEF_CHGLINE(),
	MDEF_CAPTION("Includes Mariko KEK, BEK, unique SBK", COLOR_SOFT_WHITE),
	MDEF_CHGLINE(),
	MDEF_CAPTION("Not useful for most users.", COLOR_SOFT_WHITE),
	MDEF_CHGLINE(),
	MDEF_CAPTION("IMPORTANT: Run BEFORE SysMMC/EmuMMC dump!", COLOR_WARNING),
	MDEF_CAPTION("Keyslots must have factory keys!", COLOR_WARNING),
	MDEF_CHGLINE(),
	MDEF_CAPTION("Warning: wipes keyslots!", COLOR_WARNING),
	MDEF_CAPTION("Console must restart!", COLOR_WARNING),
	MDEF_CAPTION("Modchip must run again!", COLOR_WARNING),
	MDEF_CAPTION("---------------", COLOR_WHITE),
	MDEF_HANDLER("Dump Mariko Partials", dump_mariko_partial_keys, COLOR_TURQUOISE),
	MDEF_END()
};

menu_t menu_partials = { ment_partials, NULL, 0, 0 };

power_state_t STATE_POWER_OFF           = POWER_OFF_RESET;
power_state_t STATE_REBOOT_FULL         = POWER_OFF_REBOOT;
power_state_t STATE_REBOOT_RCM          = REBOOT_RCM;
power_state_t STATE_REBOOT_BYPASS_FUSES = REBOOT_BYPASS_FUSES;

ment_t ment_top[] = {
	MDEF_HANDLER("Dump from SysMMC", dump_sysnand, COLOR_TURQUOISE),
	MDEF_HANDLER("Dump from EmuMMC", dump_emunand, COLOR_TURQUOISE),
	MDEF_CAPTION("---------------", COLOR_WHITE),
	MDEF_HANDLER("Restore PRODINFO", restore_prodinfo, COLOR_TURQUOISE),
	MDEF_CAPTION("---------------", COLOR_WHITE),
	MDEF_HANDLER("Dump Amiibo Keys", dump_amiibo_keys, COLOR_TURQUOISE),
	MDEF_MENU("Dump Mariko Partials (requires reboot)", &menu_partials, COLOR_TURQUOISE),
	MDEF_CAPTION("---------------", COLOR_WHITE),
	MDEF_HANDLER("Payloads...", launch_tools, COLOR_TURQUOISE),
	MDEF_HANDLER("Reboot to hekate", launch_hekate, COLOR_TURQUOISE),
	MDEF_CAPTION("---------------", COLOR_WHITE),
	MDEF_HANDLER_EX("Reboot (OFW)", &STATE_REBOOT_BYPASS_FUSES, power_set_state_ex, COLOR_TURQUOISE),
	MDEF_HANDLER_EX("Reboot (RCM)", &STATE_REBOOT_RCM, power_set_state_ex, COLOR_TURQUOISE),
	MDEF_HANDLER_EX("Power off", &STATE_POWER_OFF, power_set_state_ex, COLOR_TURQUOISE),
	MDEF_END()
};

menu_t menu_top = { ment_top, NULL, 0, 0 };

void grey_out_menu_item(ment_t *menu)
{
	menu->type = MENT_CAPTION;
	menu->color = 0xFF555555;
	menu->handler = NULL;
}

void dump_prodinfo()
{
	gfx_clear_grey(0x1B);

	// Draw title bar and bottom bar
	char title[64];
	s_printf(title, "[Lockpick RCM Pro v%d.%d.%d] - Dump PRODINFO", LP_VER_MJ, LP_VER_MN, LP_VER_BF);
	gfx_draw_title_bar(title);
	gfx_draw_bottom_bar("Hold VOL+: Screenshot   Any Button: Return");

	// Reset console position below title bar (use global UI settings)
	gfx_con_setpos(UI_CONTENT_START_X, UI_CONTENT_START_Y);

	// Silently derive BIS keys and load them into SE keyslots
	gfx_printf("%kBIS keys...\n", COLOR_WHITE);
	if (!derive_bis_keys_silently()) {
		gfx_printf("%kBIS failed!\n", COLOR_RED);
		gfx_printf("%kTry different payload.\n\n", COLOR_RED);
		goto out_wait;
	}
	gfx_printf("%kBIS OK.\n\n", COLOR_GREEN);

	// Mount SD card
	if (!sd_mount()) {
		EPRINTF("SD mount failed.");
		goto out_wait;
	}

	// Initialize eMMC storage
	gfx_printf("%keMMC init...\n", COLOR_WHITE);
	if (emummc_storage_init_mmc()) {
		EPRINTF("MMC init failed.");
		goto out;
	}

	// Parse GPT to find PRODINFO partition
	gfx_printf("%kParsing GPT...\n", COLOR_WHITE);
	LIST_INIT(gpt);
	nx_emmc_gpt_parse(&gpt, &emmc_storage);

	emmc_part_t *prodinfo_part = nx_emmc_part_find(&gpt, "PRODINFO");
	if (!prodinfo_part) {
		EPRINTF("PRODINFO not found.");
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Initialize BIS encryption for PRODINFO
	gfx_printf("%kBIS init...\n", COLOR_WHITE);
	nx_emmc_bis_init(prodinfo_part);

	// Calculate partition size
	u32 partition_sectors = prodinfo_part->lba_end - prodinfo_part->lba_start + 1;
	u32 partition_size = partition_sectors * NX_EMMC_BLOCKSIZE;

	gfx_printf("%kSize: %d KB\n", COLOR_CYAN_L, partition_size / 1024);

	// Create output directory
	f_mkdir("sd:/switch");

	// Open output file
	FIL fp;
	if (f_open(&fp, "sd:/switch/PRODINFO.bin", FA_CREATE_ALWAYS | FA_WRITE)) {
		EPRINTF("File create failed.");
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Allocate buffer for reading (256KB at a time)
	const u32 buf_size = 0x40000; // 256KB
	u8 *buffer = (u8 *)malloc(buf_size);
	if (!buffer) {
		EPRINTF("Buffer alloc failed.");
		f_close(&fp);
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Dump partition sector by sector with progress
	gfx_printf("%kDumping...\n", COLOR_WHITE);
	u32 num_sectors_per_read = buf_size / NX_EMMC_BLOCKSIZE;
	u32 sectors_read = 0;
	u32 prev_pct = 200;

	while (sectors_read < partition_sectors) {
		u32 sectors_to_read = MIN(num_sectors_per_read, partition_sectors - sectors_read);

		// Read and decrypt sectors
		if (nx_emmc_bis_read(sectors_read, sectors_to_read, buffer)) {
			gfx_printf("%kRead err @ %d\n", COLOR_RED, sectors_read);
			break;
		}

		// Write to file
		u32 bytes_to_write = sectors_to_read * NX_EMMC_BLOCKSIZE;
		UINT bytes_written;
		if (f_write(&fp, buffer, bytes_to_write, &bytes_written) || bytes_written != bytes_to_write) {
			gfx_printf("%kWrite err @ %d\n", COLOR_RED, sectors_read);
			break;
		}

		sectors_read += sectors_to_read;

		// Update progress
		u32 pct = (sectors_read * 100) / partition_sectors;
		if (pct != prev_pct && pct % 5 == 0) {
			u32 cx, cy;
			gfx_con_getpos(&cx, &cy);
			gfx_con_setpos(30, cy);
			gfx_printf("%kProgress: %d%%", COLOR_CYAN_L, pct);
			prev_pct = pct;
		}
	}

	// Cleanup
	free(buffer);
	f_close(&fp);
	nx_emmc_bis_finalize();
	nx_emmc_gpt_free(&gpt);

	if (sectors_read == partition_sectors) {
		gfx_printf("\n%kSaved to sd:/switch/PRODINFO.bin\n", COLOR_GREEN);
	} else {
		gfx_printf("\n%kDump incomplete!\n", COLOR_RED);
	}

out:
	emummc_storage_end();
	sd_end();

out_wait:
	// Wait for button press with hold detection for screenshot
	u32 vol_press_start = 0;
	while (true)
	{
		Input_t *inp = hidRead();
		u32 btn = inp->buttons;

		if (btn & (BtnVolP | JoyLUp))
		{
			if (vol_press_start == 0)
				vol_press_start = get_tmr_ms();
			else if (get_tmr_ms() - vol_press_start > 2000)
			{
				// Button held for 2 seconds - take screenshot
				int save_fb_to_bmp();
				int res = save_fb_to_bmp();
				if (!res)
					gfx_printf("\n%kScreenshot saved!\n", COLOR_GREEN);
				else
					gfx_printf("\n%kScreenshot failed!\n", COLOR_RED);

				msleep(1000);

				// Wait for button release
				while (hidRead()->buttons & (BtnVolP | JoyLUp))
					msleep(10);

				// Wait for any button press to return
				hidWait();
				break;
			}
		}
		else
		{
			vol_press_start = 0;
			// Any other button pressed - return immediately
			if (btn)
				break;
		}

		msleep(10);
	}
}

void restore_prodinfo()
{
	gfx_clear_grey(0x1B);

	// Draw title bar and bottom bar
	char title[64];
	s_printf(title, "[Lockpick RCM Pro v%d.%d.%d] - Restore PRODINFO", LP_VER_MJ, LP_VER_MN, LP_VER_BF);
	gfx_draw_title_bar(title);
	gfx_draw_bottom_bar("Hold VOL+: Screenshot   Any Button: Return");

	// Reset console position below title bar (use global UI settings)
	gfx_con_setpos(UI_CONTENT_START_X, UI_CONTENT_START_Y);

	// Warning with left margin
	gfx_printf("%kWARNING: This will overwrite your PRODINFO!\n", COLOR_RED);
	gfx_printf("%kMake sure you have a backup before proceeding!\n", COLOR_RED);
	gfx_printf("\n%kPress VOL+ to continue or any other button to cancel.\n", COLOR_WHITE);

	Input_t *inp = hidWait();
	if (!(inp->buttons & (BtnVolP | JoyLUp))) {
		gfx_printf("%kCancelled.\n", COLOR_WHITE);
		goto out_wait;
	}

	// Silently derive BIS keys and load them into SE keyslots
	gfx_printf("\n%kDeriving BIS encryption keys...\n", COLOR_WHITE);
	if (!derive_bis_keys_silently()) {
		gfx_printf("%kBIS derive failed! Try different payload.\n", COLOR_RED);
		goto out_wait;
	}
	gfx_printf("%kBIS keys derived successfully.\n", COLOR_GREEN);

	// Mount SD card
	if (!sd_mount()) {
		EPRINTF("SD mount failed.");
		goto out_wait;
	}

	// Get current device eMMC ID
	char emmc_id[16] = {0};
	if (!get_emmc_id_external(emmc_id)) {
		gfx_printf("%keMMC ID failed!\n", COLOR_RED);
		goto out;
	}

	gfx_printf("%kCurrent device ID: %s\n", COLOR_CYAN_L, emmc_id);

	// Build paths for new folder structure
	char hekate_path[80];
	char enc_path[80];
	char dec_path[80];
	char old_dec_path[] = "sd:/switch/prodinfo.dec";
	char old_enc_path[] = "sd:/switch/prodinfo.enc";

	s_printf(hekate_path, "sd:/backup/%s/partitions/PRODINFO", emmc_id);
	s_printf(enc_path, "sd:/backup/%s/dumps/prodinfo.enc", emmc_id);
	s_printf(dec_path, "sd:/backup/%s/dumps/prodinfo.dec", emmc_id);

	// Check which PRODINFO file exists - priority order
	FIL fp;
	bool is_encrypted = false;
	bool found = false;
	char found_path[80] = {0};

	// 1. Check Hekate-compatible location (encrypted, same device)
	if (f_open(&fp, hekate_path, FA_READ) == FR_OK) {
		is_encrypted = true;
		found = true;
		strcpy(found_path, hekate_path);
		gfx_printf("\n%kFound PRODINFO backup: partitions/PRODINFO\n", COLOR_GREEN);
		gfx_printf("%kSource device: %s (MATCH)\n", COLOR_GREEN, emmc_id);
	}
	// 2. Check new decrypted location (same device)
	else if (f_open(&fp, dec_path, FA_READ) == FR_OK) {
		is_encrypted = false;
		found = true;
		strcpy(found_path, dec_path);
		gfx_printf("\n%kFound PRODINFO backup: dumps/prodinfo.dec\n", COLOR_GREEN);
		gfx_printf("%kSource device: %s (MATCH)\n", COLOR_GREEN, emmc_id);
	}
	// 3. Check new encrypted location (same device)
	else if (f_open(&fp, enc_path, FA_READ) == FR_OK) {
		is_encrypted = true;
		found = true;
		strcpy(found_path, enc_path);
		gfx_printf("\n%kFound PRODINFO backup: dumps/prodinfo.enc\n", COLOR_GREEN);
		gfx_printf("%kSource device: %s (MATCH)\n", COLOR_GREEN, emmc_id);
	}
	// 4. Fall back to old location - decrypted (backward compatibility)
	else if (f_open(&fp, old_dec_path, FA_READ) == FR_OK) {
		is_encrypted = false;
		found = true;
		strcpy(found_path, old_dec_path);
		gfx_printf("\n%k/switch/prodinfo.dec\n", COLOR_RED);
		gfx_printf("%kCannot verify device!\n", COLOR_RED);
	}
	// 5. Fall back to old location - encrypted (backward compatibility)
	else if (f_open(&fp, old_enc_path, FA_READ) == FR_OK) {
		is_encrypted = true;
		found = true;
		strcpy(found_path, old_enc_path);
		gfx_printf("\n%k/switch/prodinfo.enc\n", COLOR_RED);
		gfx_printf("%kCannot verify device!\n", COLOR_RED);
	}

	// No backup found anywhere
	if (!found) {
		gfx_printf("%kNo backup found! Dump PRODINFO first!\n", COLOR_RED);
		goto out;
	}

	// Get file size
	u32 file_size = f_size(&fp);

	// Initialize eMMC storage
	gfx_printf("\n%kInitializing eMMC...\n", COLOR_WHITE);
	if (emummc_storage_init_mmc()) {
		EPRINTF("MMC init failed.");
		f_close(&fp);
		goto out;
	}

	// Parse GPT to find PRODINFO partition
	gfx_printf("%kParsing GPT...\n", COLOR_WHITE);
	LIST_INIT(gpt);
	nx_emmc_gpt_parse(&gpt, &emmc_storage);

	emmc_part_t *prodinfo_part = nx_emmc_part_find(&gpt, "PRODINFO");
	if (!prodinfo_part) {
		EPRINTF("PRODINFO not found.");
		f_close(&fp);
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Initialize BIS encryption for PRODINFO
	gfx_printf("%kInitializing BIS encryption...\n", COLOR_WHITE);
	nx_emmc_bis_init(prodinfo_part);

	// Calculate partition size
	u32 partition_sectors = prodinfo_part->lba_end - prodinfo_part->lba_start + 1;
	u32 partition_size = partition_sectors * NX_EMMC_BLOCKSIZE;

	gfx_printf("%kPartition size: %d KB (%d sectors)\n", COLOR_CYAN_L, partition_size / 1024, partition_sectors);
	gfx_printf("%kFile size: %d KB\n", COLOR_CYAN_L, file_size / 1024);

	// Verify file size matches partition
	if (file_size != partition_size) {
		gfx_printf("%kSize mismatch! Exp %d, got %d\n", COLOR_RED, partition_size, file_size);
		f_close(&fp);
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Allocate buffer for writing (256KB at a time)
	const u32 buf_size = 0x40000; // 256KB
	u8 *buffer = (u8 *)malloc(buf_size);
	if (!buffer) {
		EPRINTF("Buffer alloc failed.");
		f_close(&fp);
		nx_emmc_gpt_free(&gpt);
		goto out;
	}

	// Restore partition sector by sector with progress
	u32 progress_y = 0;
	if (is_encrypted)
		gfx_printf("\n%kRestoring encrypted PRODINFO (raw write)...\n", COLOR_WHITE);
	else
		gfx_printf("\n%kRestoring decrypted PRODINFO (encrypting)...\n", COLOR_WHITE);

	u32 num_sectors_per_write = buf_size / NX_EMMC_BLOCKSIZE;
	u32 sectors_written = 0;
	u32 prev_pct = 200;
	u32 lba_start = prodinfo_part->lba_start;

	while (sectors_written < partition_sectors) {
		u32 sectors_to_write = MIN(num_sectors_per_write, partition_sectors - sectors_written);

		// Read from file
		u32 bytes_to_read = sectors_to_write * NX_EMMC_BLOCKSIZE;
		UINT bytes_read;
		if (f_read(&fp, buffer, bytes_to_read, &bytes_read) || bytes_read != bytes_to_read) {
			gfx_printf("%kRead err @ %d\n", COLOR_RED, sectors_written);
			break;
		}

		// Write sectors - encrypted files go directly, decrypted files get encrypted
		if (is_encrypted) {
			// Write raw encrypted data directly to eMMC
			if (!sdmmc_storage_write(&emmc_storage, lba_start + sectors_written, sectors_to_write, buffer)) {
				gfx_printf("%kWrite err @ %d\n", COLOR_RED, sectors_written);
				break;
			}
		} else {
			// Encrypt decrypted data and write to eMMC
			if (nx_emmc_bis_write(sectors_written, sectors_to_write, buffer)) {
				gfx_printf("%kWrite err @ %d\n", COLOR_RED, sectors_written);
				break;
			}
		}

		sectors_written += sectors_to_write;

		// Update progress - save Y position on first update to keep progress stationary
		u32 pct = (sectors_written * 100) / partition_sectors;
		if (pct != prev_pct && pct % 5 == 0) {
			u32 cx, cy;
			gfx_con_getpos(&cx, &cy);
			if (progress_y == 0)
				progress_y = cy;
			gfx_con_setpos(30, progress_y);
			gfx_printf("%kProgress: %d%%", COLOR_CYAN_L, pct);
			prev_pct = pct;
		}
	}

	// Cleanup
	free(buffer);
	f_close(&fp);
	nx_emmc_bis_finalize();
	nx_emmc_gpt_free(&gpt);

	if (sectors_written == partition_sectors) {
		gfx_printf("\n\n%kPRODINFO restored successfully!\n", COLOR_GREEN);
		gfx_printf("%kYou should reboot your console now.\n", COLOR_CYAN_L);
	} else {
		gfx_printf("\n\n%kRestore incomplete!\n", COLOR_RED);
		gfx_printf("%kPRODINFO may be corrupt!\n", COLOR_RED);
	}

out:
	emummc_storage_end();
	sd_end();

out_wait:
	// Wait for button press with hold detection for screenshot
	u32 vol_press_start = 0;
	while (true)
	{
		Input_t *inp = hidRead();
		u32 btn = inp->buttons;

		if (btn & (BtnVolP | JoyLUp))
		{
			if (vol_press_start == 0)
				vol_press_start = get_tmr_ms();
			else if (get_tmr_ms() - vol_press_start > 2000)
			{
				// Button held for 2 seconds - take screenshot
				int save_fb_to_bmp();
				int res = save_fb_to_bmp();
				if (!res)
					gfx_printf("\n%kScreenshot saved!\n", COLOR_GREEN);
				else
					gfx_printf("\n%kScreenshot failed!\n", COLOR_RED);

				msleep(1000);

				// Wait for button release
				while (hidRead()->buttons & (BtnVolP | JoyLUp))
					msleep(10);

				// Wait for any button press to return
				hidWait();
				break;
			}
		}
		else
		{
			vol_press_start = 0;
			// Any other button pressed - return immediately
			if (btn)
				break;
		}

		msleep(10);
	}
}

void dump_mariko_partial_keys()
{
	gfx_clear_grey(0x1B);
	gfx_con_setpos(0, 0);

	if (h_cfg.t210b01) {
		int res = save_mariko_partial_keys(0, 16, false);
		if (res == 0 || res == 3)
		{
			// Grey out dumping menu items as the keyslots have been invalidated.
			grey_out_menu_item(&ment_top[0]); // Dump from SysMMC
			grey_out_menu_item(&ment_top[1]); // Dump from EmuMMC
			grey_out_menu_item(&ment_top[3]); // Restore PRODINFO
			grey_out_menu_item(&ment_partials[17]); // Dump Mariko Partials handler (index 17 after new warnings)
		}

		gfx_printf("\n%kPress a button to return to the menu.", COLOR_CYAN_L);
		hidWait();

		// Wait for button release to prevent accidental menu navigation
		while (hidRead()->buttons) msleep(10);
	}
}

extern void pivot_stack(u32 stack_top);

void ipl_main()
{
	// Do initial HW configuration. This is compatible with consecutive reruns without a reset.
	hw_init();

	// Pivot the stack so we have enough space.
	pivot_stack(IPL_STACK_TOP);

	// Tegra/Horizon configuration goes to 0x80000000+, package2 goes to 0xA9800000, we place our heap in between.
	heap_init(IPL_HEAP_START);

#ifdef DEBUG_UART_PORT
	uart_send(DEBUG_UART_PORT, (u8 *)"hekate: Hello!\r\n", 16);
	uart_wait_idle(DEBUG_UART_PORT, UART_TX_IDLE);
#endif

	// Set bootloader's default configuration.
	set_default_configuration();

	// Mount SD Card.
	diag_log("boot: sd_mount start");
	h_cfg.errors |= !sd_mount() ? ERR_SD_BOOT_EN : 0;
	diag_log_u32("boot: sd_mount result", !(h_cfg.errors & ERR_SD_BOOT_EN));
	diag_log("boot: config load start");
	load_lockpick_configuration();
	diag_log("boot: config load done");

	// Train DRAM and switch to max frequency.
	diag_log("boot: minerva init start");
	if (minerva_init()) //!TODO: Add Tegra210B01 support to minerva.
		h_cfg.errors |= ERR_LIBSYS_MTC;
	diag_log_u32("boot: minerva result", !(h_cfg.errors & ERR_LIBSYS_MTC));

	diag_log("boot: display init start");
	display_init();

	u32 *fb = display_init_framebuffer_pitch();
	gfx_init_ctxt(fb, 720, 1280, 720);

	gfx_con_init();

	display_backlight_pwm_init();
	diag_screen_step(1, "display ready");

	// Initialize HID input (Joy-Con support)
	diag_screen_step(2, "hid init start");
	hidInit();
	diag_screen_step(3, "hid init done");

	// Overclock BPMP.
	diag_screen_step(4, "clock set start");
	bpmp_clk_rate_set(h_cfg.t210b01 ? BPMP_CLK_DEFAULT_BOOST : BPMP_CLK_LOWER_BOOST);
	diag_screen_step(5, "clock set done");

	// Load emuMMC configuration from SD.
	diag_screen_step(6, "emummc cfg start");
	emummc_load_cfg();
	// Ignore whether emummc is enabled.
	h_cfg.emummc_force_disable = emu_cfg.sector == 0 && !emu_cfg.path;
	emu_cfg.enabled = !h_cfg.emummc_force_disable;
	diag_screen_step(7, "emummc cfg done");

	// Grey out emummc option if not present.
	if (h_cfg.emummc_force_disable)
	{
		grey_out_menu_item(&ment_top[1]); // Dump from EmuMMC
	}

	// Grey out reboot to RCM option if on Mariko or patched console.
	if (h_cfg.t210b01 || h_cfg.rcm_patched)
	{
		grey_out_menu_item(&ment_top[12]); // Reboot (RCM)
	}

	// Grey out Mariko partial dump option on Erista.
	if (!h_cfg.t210b01) {
		grey_out_menu_item(&ment_top[6]); // Dump Mariko Partials
	}

	// Grey out reboot to hekate option if no update.bin found.
	if (f_stat("bootloader/update.bin", NULL))
	{
		grey_out_menu_item(&ment_top[9]); // Reboot to hekate
	}

	minerva_change_freq(FREQ_800);
	diag_screen_step(8, "entering menu");

	while (true)
		tui_do_menu(&menu_top);

	// Halt BPMP if we managed to get out of execution.
	while (true)
		bpmp_halt();
}
