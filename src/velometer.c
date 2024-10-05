/*
 * Copyright © 2024 Hayden K <erichkpr@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED “AS IS” AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define _USE_MATH_DEFINES // ... windows.
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "con_.h"
#include "engineapi.h"
#include "event.h"
#include "errmsg.h"
#include "ent.h"
#include "gamedata.h"
#include "gametype.h"
#include "hexcolour.h"
#include "hook.h"
#include "hud.h"
#include "intdefs.h"
#include "feature.h"
#include "mem.h"
#include "sst.h"
#include "vcall.h"
#include "x86.h"
#include "x86util.h"

FEATURE("Velocity HUD")
REQUIRE(ent)
REQUIRE(hud)
REQUIRE_GAMEDATA(off_vecvel)

ulong font;
int colourtouse;
float oldabsvel = 0;
float curabsvel = 0;

DEF_CVAR(sst_velocityhud, "Draw velocity HUD", 0, CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_colour_losing, "Velocity text colour (losing)",
		"FFA020C8", CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_colour, "Velocity text colour (same)", "F0F0FFFF",
		CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_colour_gaining, "Velocity text colour (gaining)",
		"40A0FF8C", CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR_MINMAX(sst_velocityhud_colour_mode, "", 1, 1, 2,
		CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_x, "Velocity HUD x position", 0, 
		CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_y, "Velocity HUD y position", 0, 
		CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_font, "Velocity HUD font", "Default", 
		CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_velocityhud_player, "Velocity HUD test", 1, 
		CON_ARCHIVE | CON_HIDDEN)

static struct rgba colours[3] = {
	{0, 255, 0, 255}, // green (increased)
	{255, 0, 0, 255}, // red (lost speed)
	{255, 255, 255, 255} // white (equal)
};

static void colourcb(struct con_var *v) {
	if (v == sst_velocityhud_colour_losing) {
		hexcolour_rgba(colours[1].bytes, con_getvarstr(v));
	}
	else if (v == sst_velocityhud_colour) {
		hexcolour_rgba(colours[2].bytes, con_getvarstr(v));
	}
	else /* v == sst_velocityhud_colour_gaining */ {
		hexcolour_rgba(colours[0].bytes, con_getvarstr(v));
	}
}

static void fontcb(struct con_var *v){
	font = hud_getfont(con_getvarstr(sst_velocityhud_font), true);
}

HANDLE_EVENT(Tick, bool simulating) {
	if (!simulating) return;
	struct edict *ed = ent_getedict(con_getvari(sst_velocityhud_player));
	if_cold (!ed || !ed->ent_unknown) {
		//errmsg_errorx("couldn't access player entity");
		return;
	}
	void *e = ed->ent_unknown;
	
	struct vec3f *velocity = mem_offset(e, off_vecvel);

	curabsvel = (sqrt(pow(velocity->x, 2)+(pow(velocity->y, 2))));
	if (con_getvari(sst_velocityhud_colour_mode) == 2) {
		if (fabs(curabsvel - oldabsvel) < 0.01)
			colourtouse = 2; // same
		else if (curabsvel > oldabsvel)
			colourtouse = 0; // gaining
		else
			colourtouse = 1; // losing
	} 
	else {
		colourtouse = 0; // white or default
	}
	oldabsvel = curabsvel;
}

HANDLE_EVENT(HudPaint, void) {
	if (!con_getvari(sst_velocityhud)) return;
	int xoffset = con_getvari(sst_velocityhud_x);
	int yoffset = con_getvari(sst_velocityhud_y);
	wchar_t buffer[100];
	swprintf(buffer, 100, L"%.0f", curabsvel);
	int len = wcslen(buffer);
	hud_drawtext(font, xoffset, yoffset, colours[colourtouse], buffer, len); 
}

INIT {
	font = hud_getfont(con_getvarstr(sst_velocityhud_font), true);
	if (!font) {
		errmsg_errorx("couldn't get font");
		return false;
	}
	// unhide cvars
	sst_velocityhud->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_colour_losing->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_colour_losing->cb = &colourcb;
	sst_velocityhud_colour->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_colour->cb = &colourcb;
	sst_velocityhud_colour_gaining->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_colour_gaining->cb = &colourcb;
	sst_velocityhud_colour_mode->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_x->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_y->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_font->base.flags &= ~CON_HIDDEN;
	sst_velocityhud_font->cb = &fontcb;
	sst_velocityhud_player->base.flags &= ~CON_HIDDEN;
	return true;
}

PREINIT {
	return GAMETYPE_MATCHES(L4Dx);
}

END {

}

// vi: sw=4 ts=4 noet tw=80 cc=80
