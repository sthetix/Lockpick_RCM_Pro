/*
 * Copyright (c) 2018 naehrwert
 * Copyright (c) 2018 CTCaer
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

#include <display/di.h>
#include "gfx.h"
#include "tui.h"
#include "../hid/hid.h"
#include "../config.h"
#include <power/max17050.h>
#include <utils/btn.h>
#include <input/touch.h>
#include <utils/util.h>
#include <utils/sprintf.h>
#include <libs/fatfs/ff.h>
#include <string.h>

extern hekate_config h_cfg;

void tui_sbar(bool force_update)
{
	u32 cx, cy;
	static u32 sbar_time_keeping = 0;

	u32 timePassed = get_tmr_s() - sbar_time_keeping;
	if (!force_update)
		if (timePassed < 5)
			return;

	u8 prevFontSize = gfx_con.fntsz;
	gfx_con.fntsz = 16;
	sbar_time_keeping = get_tmr_s();

	u32 battPercent = 0;

	gfx_con_getpos(&cx, &cy);
	gfx_con_setpos(1050, 704); // Right side of bottom bar, aligned with legend (y=704)

	max17050_get_property(MAX17050_RepSOC, (int *)&battPercent);

	// Save graphics state before drawing battery info with white background
	u8 saved_fillbg = gfx_con.fillbg;
	u32 saved_bgcol = gfx_con.bgcol;

	// RepSOC: upper byte = integer percentage (0-100), lower byte = fractional (1/256)
	// Cyan text on dark grey background
	gfx_printf("%K%k Batt: %d%%", 0xFF3D3D3D, 0xFF00D8FF,
		battPercent >> 8);

	// Restore graphics state to prevent white background from leaking to subsequent text
	gfx_con.fillbg = saved_fillbg;
	gfx_con.bgcol = saved_bgcol;

	gfx_con.fntsz = prevFontSize;
	gfx_con_setpos(cx, cy);
}

void tui_pbar(int x, int y, u32 val, u32 fgcol, u32 bgcol)
{
	u32 cx, cy;
	if (val > 200)
		val = 200;

	gfx_con_getpos(&cx, &cy);

	gfx_con_setpos(x, y);

	// Only show progress for intermediate values, not 100% (completion)
	if (val < 100)
		gfx_printf("%k[%3d%%]%k", fgcol, val, 0xFFCCCCCC);
	else
		gfx_printf("%k%k", 0xFFCCCCCC, 0xFFCCCCCC); // Reset color even at 100%

	// Progress bar drawing disabled for landscape - gfx_line needs rework
	// x += 7 * gfx_con.fntsz;
	// for (u32 i = 0; i < (gfx_con.fntsz >> 3) * 4; i++)
	// {
	// 	gfx_line(x, y + i + 1, x + 2 * val, y + i + 1, fgcol);
	// 	gfx_line(x + 2 * val, y + i + 1, x + 2 * 100, y + i + 1, bgcol);
	// }

	gfx_con_setpos(cx, cy);

	// Update status bar.
	tui_sbar(false);
}

void *tui_do_menu(menu_t *menu)
{
	int idx = 0, prev_idx = -1, cnt = 0x7FFFFFFF;
	int need_full_redraw = 1; // Force initial full redraw
	int last_drawn_idx = -1; // Track last drawn selection

	gfx_clear_grey(0x1B); // Clear full screen initially

	tui_sbar(true);

	// Snapshot current button state so any buttons already held when the menu
	// opens are treated as "already seen" and don't fire immediately.
	u32 btn_last = hidRead()->buttons;

	while (true)
	{
		// Skip caption or separator lines selection.
		while (menu->ents[idx].type == MENT_CAPTION ||
			menu->ents[idx].type == MENT_CHGLINE)
		{
			if (prev_idx <= idx || (!idx && prev_idx == cnt - 1))
			{
				idx++;
				if (idx > (cnt - 1))
				{
					idx = 0;
					prev_idx = 0;
				}
			}
			else
			{
				idx--;
				if (idx < 0)
				{
					idx = cnt - 1;
					prev_idx = cnt;
				}
			}
		}

		// Check if selection changed
		if (idx != prev_idx)
		{
			prev_idx = idx;
		}

		// Handle full redraw (initial or after handler)
		if (need_full_redraw)
		{
			need_full_redraw = 0;

			// Clear screen
			gfx_clear_grey(0x1B);

			// Draw title bar and bottom bar
			char title[64];
			s_printf(title, "[Lockpick RCM Pro v%d.%d.%d]", LP_VER_MJ, LP_VER_MN, LP_VER_BF);
			gfx_draw_title_bar(title);
			gfx_draw_bottom_bar("Joy-Con/Btns: Move   A/Power: Select   Cap+: Screenshot");

			// Draw all menu items (use global UI settings)
			u32 start_x = UI_MENU_START_X;
			u32 start_y = UI_MENU_START_Y;

			for (cnt = 0; menu->ents[cnt].type != MENT_END; cnt++)
			{
				gfx_con_setpos(start_x, start_y + (cnt * 24));

				if (cnt == idx)
					gfx_con_setcol(0xFF1B1B1B, 1, 0xFFCCCCCC);
				else
					gfx_con_setcol(0xFFCCCCCC, 1, 0xFF1B1B1B);

				if (menu->ents[cnt].type != MENT_CHGLINE)
				{
					if (cnt == idx)
						gfx_printf(" %s", menu->ents[cnt].caption);
					else
						gfx_printf("%k %s", menu->ents[cnt].color, menu->ents[cnt].caption);
				}

				if(menu->ents[cnt].type == MENT_MENU)
					gfx_printf("%k...", 0xFF0099EE);
			}

			last_drawn_idx = idx;

			// Force initial battery status display
			tui_sbar(true);
		}
		// Handle cursor movement - only redraw changed items (TegraExplorer style)
		else if (idx != last_drawn_idx)
		{
			u32 start_x = UI_MENU_START_X;
			u32 start_y = UI_MENU_START_Y;  // Use global UI settings

			// Redraw old selection (unhighlight)
			if (last_drawn_idx >= 0 && menu->ents[last_drawn_idx].type != MENT_CHGLINE)
			{
				gfx_con_setpos(start_x, start_y + (last_drawn_idx * 24));
				gfx_con_setcol(0xFF1B1B1B, 1, 0xFF1B1B1B); // Clear with background color
				gfx_printf("             "); // Clear with spaces
				gfx_con_setpos(start_x, start_y + (last_drawn_idx * 24));
				gfx_con_setcol(0xFFCCCCCC, 1, 0xFF1B1B1B);
				gfx_printf("%k %s", menu->ents[last_drawn_idx].color, menu->ents[last_drawn_idx].caption);
				if(menu->ents[last_drawn_idx].type == MENT_MENU)
					gfx_printf("%k...", 0xFF0099EE);
			}

			// Redraw new selection (highlight)
			if (menu->ents[idx].type != MENT_CHGLINE)
			{
				gfx_con_setpos(start_x, start_y + (idx * 24));
				gfx_con_setcol(0xFF1B1B1B, 1, 0xFF1B1B1B); // Clear with background color
				gfx_printf("             "); // Clear with spaces
				gfx_con_setpos(start_x, start_y + (idx * 24));
				gfx_con_setcol(0xFF1B1B1B, 1, 0xFFCCCCCC);
				gfx_printf(" %s", menu->ents[idx].caption);
				if(menu->ents[idx].type == MENT_MENU)
					gfx_printf("%k...", 0xFF0099EE);
			}

			gfx_con_setcol(0xFFCCCCCC, 1, 0xFF1B1B1B);
			last_drawn_idx = idx;
		}

		display_backlight_brightness(h_cfg.backlight, 1000);

		// Non-blocking input read using HID system
		Input_t *inp = hidRead();
		u32 btn = inp->buttons;

		// Only process button presses (ignore button releases and repeats)
		if (btn == btn_last)
		{
			// Still check for VOL+ hold (1 second) for screenshot
			static u32 vol_press_start = 0;
			if (btn & (BtnVolP | JoyLUp))
			{
				if (vol_press_start == 0)
					vol_press_start = get_tmr_ms();
				else if (get_tmr_ms() - vol_press_start > 1000)
				{
					// Button held for 1 second - take screenshot
					vol_press_start = 0;
					btn_last = 0; // Reset to allow next button press

					f_mkdir("sd:/switch");
					f_mkdir("sd:/switch/screenshot");

					int save_fb_to_bmp();
					int res = save_fb_to_bmp();

					gfx_con_setpos(UI_NOTIFY_X, UI_NOTIFY_Y);
					if (!res) {
						gfx_printf("%kScreenshot saved!%k                              ", COLOR_CYAN_L, COLOR_SOFT_WHITE);
					} else {
						gfx_printf("%kScreenshot failed!%k                             ", COLOR_ERROR, COLOR_SOFT_WHITE);
					}
					msleep(1000);

					need_full_redraw = 1;
					continue;
				}
			}
			else
			{
				vol_press_start = 0;
			}

			msleep(10);
			continue;
		}

		btn_last = btn;

		// Ignore button releases (when btn becomes 0)
		if (!btn)
		{
			msleep(10);
			continue;
		}

		// Navigation: Joy-Con D-pad, Left Stick, or physical volume buttons
		if ((btn & (JoyLDown | BtnVolM)) && idx < (cnt - 1))
			idx++;
		else if ((btn & (JoyLDown | BtnVolM)) && idx == (cnt - 1))
		{
			idx = 0;
			prev_idx = -1;
		}
		else if ((btn & (JoyLUp | BtnVolP)) && idx > 0)
			idx--;
		else if ((btn & (JoyLUp | BtnVolP)) && idx == 0)
		{
			idx = cnt - 1;
			prev_idx = cnt;
		}

		// Selection: Joy-Con A button or physical Power button
		if (btn & (JoyA | BtnPow))
		{
			ment_t *ent = &menu->ents[idx];
			switch (ent->type)
			{
			case MENT_HANDLER:
				// Wait for button release BEFORE calling handler so that any
				// menus opened inside the handler don't inherit this press.
				while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
				btn_last = 0;
				ent->handler(ent->data);
				need_full_redraw = 1; // Full redraw needed after handler clears screen
				// Also wait after handler returns (it may have left buttons held).
				while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
				btn_last = 0;
				break;
			case MENT_MENU:
				{
					// Wait for button release before entering sub-menu for same reason.
					while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
					btn_last = 0;
					void *result = tui_do_menu(ent->menu);
					// Clear full screen after returning from sub-menu
					gfx_clear_grey(0x1B);
					// Wait for all buttons to be released to prevent double-trigger
					while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
					btn_last = 0;
					return result;
				}
			case MENT_DATA:
				return ent->data;
			case MENT_BACK:
				return NULL;
			case MENT_HDLR_RE:
				// Wait for button release before calling handler (same as MENT_HANDLER).
				while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
				btn_last = 0;
				ent->handler(ent);
				need_full_redraw = 1; // Full redraw needed after handler clears screen
				if (!ent->data)
					return NULL;
				// Wait for all buttons to be released to prevent double-trigger
				while (hidRead()->buttons & (JoyA | BtnPow)) msleep(10);
				btn_last = 0;
				break;
			default:
				break;
			}
			gfx_con.fntsz = 16;
			need_full_redraw = 1; // Full redraw after handler
		}
		tui_sbar(false);
	}

	return NULL;
}
