/*
 * Copyright © Hayden K <imaciidz@gmail.com>
 * Copyright © Willian Henrique <wsimanbrazil@yahoo.com.br>
 * Copyright © Michael Smith <mikesmiffy128@gmail.com>
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

#include "accessor.h"
#include "con_.h"
#include "errmsg.h"
#include "feature.h"
#include "hook.h"
#include "intdefs.h"
#include "mem.h"
#include "sst.h"
#include "vcall.h"
#include "x86.h"
#include "x86util.h"

FEATURE("Left 4 Dead 1 demo file backwards compatibility")
GAMESPECIFIC(L4D1_1022plus)

// NOTE: not bothering to put this in gamedata since it's actually a constant.
// We could optimise the gamedata system further to constant-fold things with no
// leaves beyond the GAMESPECIFIC cutoff or whatever. But that sounds annoying.
#define off_CDemoFile_protocol 272
DEF_ACCESSORS(int, CDemoFile_protocol)

// L4D1 bumps the demo protocol version with every update to the game, which
// means whenever there is a security update, you cannot watch old demos. From
// minimal testing, it seems demos recorded on version 1022 and onwards are
// compatible with the latest version of the game, so this code lets us watch
// 1022+ demos on any later version of the game.

typedef int (*GetHostVersion_func)();
static GetHostVersion_func orig_GetHostVersion;

typedef void (*VCALLCONV ReadDemoHeader_func)(void *);
static ReadDemoHeader_func orig_ReadDemoHeader;

static inline bool find_ReadDemoHeader(con_cmdcb cb) {
	// Find the call to ReadDemoHeader in the listdemo callback
	const uchar *insns = (const uchar*)cb;
	for (const uchar *p = insns; p - insns < 192;) {
		if (p[0] == X86_LEA && p[1] == X86_MODRM(2, 1, 4) && p[2] == 0x24 &&
				p[7] == X86_CALL && p[12] == X86_LEA &&
				p[13] == X86_MODRM(2, 1, 4) && p[14] == 0x24) {
			orig_ReadDemoHeader =
					(ReadDemoHeader_func)(p + 12 + mem_loads32(p + 8));
			return true;
		}
		NEXT_INSN(p, "ReadDemoHeader");
	}
	return false;
}

static void *ReadDemoHeader_midpoint;

static inline bool find_midpoint() {
	uchar *insns = (uchar *)orig_ReadDemoHeader;
	for (uchar *p = insns; p - insns < 128;) {
		if (p[0] == X86_PUSHIW && p[5] == X86_PUSHEBX && p[6] == X86_CALL &&
				!memcmp(mem_loadptr(p + 1), "HL2DEMO", 7)) {
			ReadDemoHeader_midpoint = (p + 11);
			return true;
		}
		NEXT_INSN(p, "ReadDemoHeader hook midpoint");
	}
	return false;
}

static inline bool find_GetHostVersion() {
	uchar *insns = (uchar *)orig_ReadDemoHeader;
	int jzcnt = 0;
	for (uchar *p = insns; p - insns < 192;) {
		// GetHostVersion() is called right after the third JZ insn in
		// ReadDemoHeader()
		if (p[0] == X86_JZ && ++jzcnt == 3) {
			orig_GetHostVersion =
					(GetHostVersion_func)(p + 7 + mem_loads32(p + 3));
			return true;
		}
		NEXT_INSN(p, "GetHostVersion");
	}
	return false;
}

static int demoversion, gameversion;

static int hook_GetHostVersion() {
	// If the demo version is 1022 or later, and not newer than the version we
	// are currently using, then we spoof the game version to let the demo play.
	if (demoversion >= 1022 && demoversion <= gameversion) return demoversion;
	return gameversion;
}

static int *this_protocol;
static void VCALLCONV hook_ReadDemoHeader(void *this) {
	// The mid-function hook needs to get the protocol from `this`, but by that
	// point we won't be able to rely on the ECX register and/or any particular
	// stack spill layout. So... offset the pointer and stick it in a global.
	this_protocol = getptr_CDemoFile_protocol(this);
	orig_ReadDemoHeader(this);
}

#if defined(__clang__)
__attribute__((naked))
#elif defined(_MSC_VER)
#error Inadequate inline assembly syntax, use Clang instead.
#else
#error No way to do naked functions! We only support Clang at the moment.
#endif
static int hook_midpoint() {
	__asm__ volatile (
		"pushl %%eax\n"
		"movl %1, %%eax\n"
		"movl (%%eax), %%eax\n" // dereference this_protocol
		"movl %%eax, %0\n" // store in demoversion
		"popl %%eax\n"
		"jmpl *%2\n"
		: "=m" (demoversion)
		: "m" (this_protocol), "m" (ReadDemoHeader_midpoint)
	);
}

INIT {
	con_cmdcb orig_listdemo_cb = con_findcmd("listdemo")->cb;
	if_cold (!orig_listdemo_cb) return FEAT_INCOMPAT;
	if_cold (!find_ReadDemoHeader(orig_listdemo_cb)) {
		errmsg_errorx("couldn't find ReadDemoHeader function");
		return FEAT_INCOMPAT;
	}
	if_cold (!find_midpoint()) {
		errmsg_errorx("couldn't find mid-point for ReadDemoHeader hook");
		return FEAT_INCOMPAT;
	}
	if_cold (!find_GetHostVersion()) {
		errmsg_errorx("couldn't find GetHostVersion function");
		return FEAT_INCOMPAT;
	}
	gameversion = orig_GetHostVersion();
	orig_GetHostVersion = (GetHostVersion_func)hook_inline(
			(void *)orig_GetHostVersion, (void *)&hook_GetHostVersion);
	if (!orig_GetHostVersion) {
		errmsg_errorsys("couldn't hook GetHostVersion");
		return FEAT_FAIL;
	}
	orig_ReadDemoHeader = (ReadDemoHeader_func)hook_inline(
			(void *)orig_ReadDemoHeader, (void *)&hook_ReadDemoHeader);
	if (!orig_ReadDemoHeader) {
		errmsg_errorsys("couldn't hook ReadDemoHeader");
		goto e1;
	}
	ReadDemoHeader_midpoint = hook_inline(
			(void *)ReadDemoHeader_midpoint, (void *)&hook_midpoint);
	if (!ReadDemoHeader_midpoint) {
		errmsg_errorsys("couldn't hook ReadDemoHeader midpoint");
		goto e2;
	}
	return FEAT_OK;

e2:	unhook_inline((void *)orig_ReadDemoHeader);
e1:	unhook_inline((void *)orig_GetHostVersion);
	return FEAT_FAIL;
}

END {
	if_cold (sst_userunloaded) {
		unhook_inline((void *)ReadDemoHeader_midpoint);
		unhook_inline((void *)orig_ReadDemoHeader);
		unhook_inline((void *)orig_GetHostVersion);
	}
}

// vi: sw=4 ts=4 noet tw=80 cc=80
