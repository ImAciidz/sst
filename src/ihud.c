/*
 * Copyright © 2022 Matthew Wozniak <sirtomato999@gmail.com>
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

#include "con_.h"
#include "engineapi.h"
#include "errmsg.h"
#include "feature.h"
#include "gamedata.h"
#include "hook.h"
#include "hud.h"
#include "mem.h"
#include "os.h"
#include "vcall.h"
#include "x86.h"
#include "x86util.h"

FEATURE("input hud")
REQUIRE_GAMEDATA(vtidx_CreateMove)
REQUIRE_GAMEDATA(vtidx_DecodeUserCmdFromBuffer)
REQUIRE_GAMEDATA(vtidx_GetUserCmd)
REQUIRE_GAMEDATA(vtidx_VClient_DecodeUserCmdFromBuffer)
REQUIRE_GLOBAL(factory_client)
REQUIRE(hud)

DECL_VFUNC_DYN(struct CUserCmd *, GetUserCmd, int)

typedef void (*VCALLCONV CreateMove_func)(void *, int, float, bool);
typedef void (*VCALLCONV DecodeUserCmdFromBuffer_func)(void *, void *, int);

static CreateMove_func orig_CreateMove;
static DecodeUserCmdFromBuffer_func orig_DecodeUserCmdFromBuffer;
static void *input = 0;
static struct hfont font;
static int buttons;

struct key {
	char x;
	char y;
	char w;
	char h;
	ushort ch;
	enum incode button;
};

static struct rgba unpressed = {0, 0, 0, 92};
static struct rgba pressed = {255, 255, 255, 200};
// default layout
static struct key layouts[1][10] = {
	{
		{0, 0, 1, 1, L'C', IN_DUCK},
		{1, 0, 3, 1, L'J', IN_JUMP},
		{4, 0, 1, 1, L'L', IN_ATTACK},
		{5, 0, 1, 1, L'R', IN_ATTACK2},
		{1, 1, 1, 1, L'A', IN_MOVELEFT},
		{2, 1, 1, 1, L'S', IN_BACK},
		{3, 1, 1, 1, L'D', IN_MOVERIGHT},
		{2, 2, 1, 1, L'W', IN_FORWARD},
		{3, 2, 1, 1, L'E', IN_USE},
		{0, 0, 0, 0, 0, IN_NONE},
	},
};

DEF_CVAR(sst_ihud, "Draw input HUD", 0, CON_HIDDEN)
DEF_CVAR(sst_ihud_colour_normal, "IHud key colour when not pressed (hex)",
		"0000005C", CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_ihud_colour_pressed, "IHud key colour when pressed (hex)",
		"FFFFFFC8", CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_ihud_gap, "IHud key gap (pixels)", 5, CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_ihud_keysize, "IHud key size (pixels)", 60, CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_ihud_x, "IHud x position", 0, CON_ARCHIVE | CON_HIDDEN)
DEF_CVAR(sst_ihud_y, "IHud y position", 0, CON_ARCHIVE | CON_HIDDEN)

// portalcolours.c:48
static void colourcb(struct con_var *v) {
	// this is stupid and ugly and has no friends, too bad!
	if (v == sst_ihud_colour_normal) {
		rgba_hexparse(unpressed.bytes, con_getvarstr(v));
	}
	else if (v == sst_ihud_colour_pressed) {
		rgba_hexparse(pressed.bytes, con_getvarstr(v));
	}
}

HANDLE_EVENT(HudPaint) {
	int w, h;
	if (!con_getvari(sst_ihud)) return;
	hud_getscreensize(&w, &h);
	int gap = con_getvari(sst_ihud_gap);
	int size = con_getvari(sst_ihud_keysize);
	int xoffset = con_getvari(sst_ihud_x);
	int yoffset = con_getvari(sst_ihud_y);
	for (struct key *k = layouts[0]; k->button; k++) {
		struct rgba colour = buttons & k->button ? pressed : unpressed;
		int x0 = xoffset + size * k->x + gap * (k->x+1);
		int y0 = -yoffset + h - size * (k->y+1) - gap * (k->y+1);
		int x1 = x0 + size * k->w + gap * (k->w-1);
		int y1 = y0 + size * k->h + gap * (k->h-1);
		hud_drawrect(x0, y0, x1, y1, colour, true);
		int tx = x1 - (x1 - x0) / 2 - hud_getcharwidth(font, k->ch) / 2;
		int ty = y1 - (y1 - y0) / 2 - hud_getfonttall(font) / 2;
		hud_drawtext(font, tx, ty, (struct rgba){255, 255, 255, 255},
				&k->ch, 1);
	};
}

// find the CInput "input" global
static inline bool find_input(void* vclient) {
#ifdef _WIN32
	// the only CHLClient::DecodeUserCmdFromBuffer does is call a virtual 
	// function, so find it's thisptr being loaded into ECX
	void* decodeusercmd =
		(*(void***)vclient)[vtidx_VClient_DecodeUserCmdFromBuffer];
	for (uchar *p = (uchar *)decodeusercmd; p - (uchar *)decodeusercmd < 32;) {
		if (p[0] == X86_MOVRMW && p[1] == X86_MODRM(0, 1, 5)) {
			void **indirect = mem_loadptr(p + 2);
			input = *indirect;
			return true;
		}
		NEXT_INSN(p, "input object");
	}
#else
#warning TODO(linux): implement linux equivalent (see demorec.c)
#endif
	return false;
}

void VCALLCONV hook_CreateMove(void *this, int seq, float ft, bool active) {
	orig_CreateMove(this, seq, ft, active);
	struct CUserCmd *cmd = GetUserCmd(this, seq);
	if (cmd) buttons = cmd->buttons;
}

void VCALLCONV hook_DecodeUserCmdFromBuffer(void *this, void *reader, int seq) {
	orig_DecodeUserCmdFromBuffer(this, reader, seq);
	struct CUserCmd *cmd = GetUserCmd(this, seq);
	if (cmd) buttons = cmd->buttons;
}


INIT {
	void *vclient = factory_client("VClient015", 0);
	if (!vclient) {
		errmsg_errorx("couldn't get client interface");
		return false;
	}
	if (!find_input(vclient)) {
		errmsg_errorx("couldn't find cinput global");
		return false;
	}
	void **vtable = *(void ***)input;
	// just unprotect the first few pointers (getusercmd is 8)
	if (!os_mprot(vtable, sizeof(void *) * 8, PAGE_READWRITE)) { 
		errmsg_errorsys("couldn't make virtual table writable");
		return false;
	}
	font = hud_createfont("Consolas", 18, 600, 0, 0, FONTFLAG_OUTLINE);
	if (!font.handle) {
		errmsg_errorx("couldn't create font");
		return false;
	}
	orig_CreateMove = (CreateMove_func)hook_vtable(vtable, vtidx_CreateMove,
			(void *)hook_CreateMove);
	orig_DecodeUserCmdFromBuffer = (DecodeUserCmdFromBuffer_func)hook_vtable(
			vtable, vtidx_DecodeUserCmdFromBuffer,
			(void *)hook_DecodeUserCmdFromBuffer);
	// unhide cvars
	sst_ihud->base.flags &= ~CON_HIDDEN;
	sst_ihud_gap->base.flags &= ~CON_HIDDEN;
	sst_ihud_keysize->base.flags &= ~CON_HIDDEN;
	sst_ihud_colour_pressed->base.flags &= ~CON_HIDDEN;
	sst_ihud_colour_pressed->cb = &colourcb;
	sst_ihud_colour_normal->base.flags &= ~CON_HIDDEN;
	sst_ihud_colour_normal->cb = &colourcb;
	sst_ihud_x->base.flags &= ~CON_HIDDEN;
	sst_ihud_y->base.flags &= ~CON_HIDDEN;
	return true;
}

END {
	void **vtable = *(void ***)input;
	unhook_vtable(vtable, vtidx_CreateMove, (void *)orig_CreateMove);
	unhook_vtable(vtable, vtidx_DecodeUserCmdFromBuffer,
			(void *)orig_DecodeUserCmdFromBuffer);
}

// vi: sw=4 ts=4 noet tw=80 cc=80
