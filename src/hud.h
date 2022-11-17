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

#ifndef INC_HUD_H

#include "event.h"
#include "engineapi.h"
#include "intdefs.h"

DECL_EVENT(HudPaint)

struct hfont { ulong handle; };

enum fontflags {
	FONTFLAG_NONE,
	FONTFLAG_ITALIC = 0x001,
	FONTFLAG_UNDERLINE = 0x002,
	FONTFLAG_STRIKEOUT = 0x004,
	FONTFLAG_SYMBOL = 0x008,
	FONTFLAG_ANTIALIAS = 0x010,
	FONTFLAG_GAUSSIANBLUR = 0x020,
	FONTFLAG_ROTARY = 0x040,
	FONTFLAG_DROPSHADOW = 0x080,
	FONTFLAG_ADDITIVE = 0x100,
	FONTFLAG_OUTLINE = 0x200,
	FONTFLAG_CUSTOM	= 0x400,
	FONTFLAG_BITMAP	= 0x800,
};

// get the handle to a font from it's name (sourcescheme.res)
struct hfont hud_getfont(const char *name, bool proportional);

void hud_setcolour(struct rgba colour);

void hud_drawrect(int x0, int y0, int x1, int y1, struct rgba colour,
		bool filled);

void hud_drawline(int x0, int y0, int x1, int y1, struct rgba colour);

// draw a string of lines with `npoints` points from arrays x and y
void hud_drawpolyline(int *x, int *y, int npoints, struct rgba colour);

void hud_drawtext(struct hfont font, int x, int y, struct rgba c,
		ushort *str, size_t len);

void hud_getscreensize(int *width, int *height);

int hud_getfonttall(struct hfont font);

int hud_getcharwidth(struct hfont font, int);

struct hfont hud_createfont(const char *fontname, int tall, int weight,
		int blur, int scanlines, int flags);

#endif
