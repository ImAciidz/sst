/*
 * Copyright © 2025 Hayden K <imaciidz@gmail.com>
 * Copyright © 2025 Willian Henrique <wsimanbrazil@yahoo.com.br>
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


#include "../con_.h"
#include "../errmsg.h"
#include "../feature.h"
#include "../gamedata.h"
#include "../gametype.h"
#include "../hook.h"
#include "../intdefs.h"
#include "../mem.h"
#include "../vcall.h"
#include "../x86.h"
#include "../x86util.h"

FEATURE("L4D1 demo playback compatibility")

// L4D1 bumps the demo protocol version with every update to the game, which
// means whenever there is a security update, you cannot watch old demos. From 
// minimal testing, it seems demos recorded on version 1022 and onwards are 
// compatible with the latest version of the game, so this code lets us watch
// 1022+ demos on any later version of the game.

typedef int (*GetHostVersion_func)(void);
static GetHostVersion_func orig_GetHostVersion;

typedef void (*VCALLCONV ReadDemoHeader_func)(void*);
static ReadDemoHeader_func orig_ReadDemoHeader;

// Find the call to ReadDemoHeader in the listdemo callback
static inline bool find_ReadDemoHeader(con_cmdcb cb) {
	const uchar *insns = (const uchar*)cb;
	for (const uchar *p = insns; p - insns < 192;) {
		if (p[0] == X86_LEA && p[1] == X86_MODRM(2, 1, 4) && p[2] == 0x24 &&
				p[7] == X86_CALL && p[12] == X86_LEA &&
				p[13] == X86_MODRM(2, 1, 4) && p[14] == 0x24){
			orig_ReadDemoHeader =
					(ReadDemoHeader_func)(p + 12 + mem_loads32(p + 8));
			return true;
		}
		NEXT_INSN(p, "ReadDemoHeader");
	}
	return false;
}

// GetHostVersion is called just after the third JZ insn in ReadDemoHeader
static inline bool find_GetHostVersion(void) {
	uchar *insns = (uchar *)orig_ReadDemoHeader;
	int jzcnt = 0;
	for (uchar *p = insns; p - insns < 192;) {
		if (p[0] == X86_JZ && ++jzcnt == 3) {
			orig_GetHostVersion =
					(GetHostVersion_func)(p + 7 + mem_loads32(p + 3));
			return true;
		}
		NEXT_INSN(p, "GetHostVersion");
	}
	return false;
}

static void *midfunchookaddr;

static inline bool find_mid_func_hook_addr(void) {
	const uchar *insns = (const uchar *)orig_ReadDemoHeader;
	for (const uchar *p = insns; p - insns < 128;) {
		if (p[0] == X86_PUSHIW && p[5] == X86_PUSHEBX && p[6] == X86_CALL
				&& !memcmp(*(char **)(p + 1), "HL2DEMO", 7)) {
			midfunchookaddr = (void *)(p + 11);
			return true;
		}
		NEXT_INSN(p, "l4d1 demo compat mid func hook addr");
	}
	return false;
}

int desiredversion;
int realversion;

// If the demo version is 1022 or later, and not newer than the version we are
// currently using, then we spoof the game version to let the demo play.
static int hook_GetHostVersion(void) {
	if (desiredversion >= 1022 && desiredversion <= realversion) {
		return desiredversion;
	}
	return realversion;
}

void *epicthisptr;

// Cache the CDemoFile thisptr for use later
static void VCALLCONV hook_ReadDemoHeader(void *this) {
	epicthisptr = this;
	orig_ReadDemoHeader(this);
}

// Grab the game version (net protocol) the demo was recorded on
void dummy(void) {
	desiredversion = *(int *)mem_offset(epicthisptr, 0x110);
}

__attribute__((naked)) int midfunchook(void) {
	__asm__ volatile(
		"pushal\n"
		"pushfl\n"
		"call %P0\n"
		"popfl\n"
		"popal\n"
		"jmp *(_midfunchookaddr)\n"
		: : "p"(dummy)
	);
}

PREINIT {
	// For now, we don't care about letting players on older versions than 1022
	// watch demos from even older versions.
	return GAMETYPE_MATCHES(L4D1_1022plus);
}

INIT {
	con_cmdcb orig_listdemo_cb = con_findcmd("listdemo")->cb;
	if_cold (!orig_listdemo_cb) return false;
	if_cold (!find_ReadDemoHeader(orig_listdemo_cb)) {
		errmsg_errorx("couldn't find ReadDemoHeader function");
		return false;
	}
	if_cold (!find_GetHostVersion()) {
		errmsg_errorx("couldn't find GetHostVersion function");
		return false;
	}
	if_cold(!find_mid_func_hook_addr()) {
		errmsg_errorx("couldn't find where to put the mid function hook");
		return false;
	}
	orig_GetHostVersion = (GetHostVersion_func)hook_inline(
			(void *)orig_GetHostVersion, (void *)&hook_GetHostVersion);
	orig_ReadDemoHeader = (ReadDemoHeader_func)hook_inline(
			(void *)orig_ReadDemoHeader, (void *)&hook_ReadDemoHeader);
	realversion = orig_GetHostVersion();
	midfunchookaddr = hook_inline(
			(void *)midfunchookaddr, (void *)&midfunchook);
	return true;
}

END {
	unhook_inline((void *)orig_GetHostVersion);
	unhook_inline((void *)orig_ReadDemoHeader);
	unhook_inline((void *)midfunchookaddr);
	return;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
