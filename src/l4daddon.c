/*
 * Copyright © 2024 Hayden K <imaciidz@gmail.com>
 * Copyright © 2024 Willian Henrique <wsimanbrazil@yahoo.com.br>
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
#include "gametype.h"
#include "hook.h"
#include "intdefs.h"
#include "langext.h"
#include "sst.h"
#include "mem.h"
#include "os.h"
#include "ppmagic.h"
#include "vcall.h"
#include "x86.h"
#include "x86util.h"
#include <string.h>

FEATURE()
REQUIRE_GLOBAL(engclient)

static int *addonvecsz;
static char last_mission[128] = {0};
static char last_gamemode[128] = {0};
static int old_addonvecsz;

typedef void (*FSMAFAS_func)(bool, char *, char *, bool);
static FSMAFAS_func orig_FSMAFAS;
static void hook_FSMAFAS(bool param1, char *param2, char *param3, bool param4) {
	// param1: if addons are disallowed in this mode (versus, scav, etc)
	// param2: campaign/mission
	// param3: gamemode
	// param4: is cur mode a mutation
	con_msg("======================\n");
	con_msg("testicles\n");
	con_msg("last_mission %s\n", last_mission);
	con_msg("last_gamemode %s\n", last_gamemode);
	con_msg("the old number of addons: %u\n", old_addonvecsz);
	if (*addonvecsz > 0) {
		old_addonvecsz = *addonvecsz;
		if (param2 && param3) {
			if ((strcmp(param2, last_mission)) || strcmp(param3, last_gamemode)) {
				con_msg("\ncalling original function (p2/p3 are not null, one of them changed)\n\n");
				orig_FSMAFAS(param1, param2, param3, param4);
			}
			strcpy(last_mission, param2);
			strcpy(last_gamemode, param3);
		} else {
			con_msg("\ncalling FSMAFAS (p2 and/or p3 are null)\n\n");
			strcpy(last_mission, "");
			strcpy(last_gamemode, "");
			orig_FSMAFAS(param1, param2, param3, param4);
		}
	} else if (old_addonvecsz > 0) {
		old_addonvecsz = *addonvecsz;
		con_msg("\ncalling FSMAFAS\n\n");
		orig_FSMAFAS(param1, param2, param3, param4);
	} else {
		con_msg("\ndid not call FSMAFAS, addons are disabled and they were not just disabled a moment ago\n\n");
	}

	con_msg("isAddonsDisallowedInMode: %u\n", param1);
	con_msg("mission: %s\n", param2);
	con_msg("mode: %s\n", param3);
	con_msg("isMutation: %u\n", param4);
	con_msg("the number of addons thing idk: %u\n", *addonvecsz);
	con_msg("======================\n");
}

DECL_VFUNC(void, CEC_FSMAFAS, 179)

static inline bool find_FSMAFAS(void) {
	const uchar *insns = (const uchar*)VFUNC(engclient, CEC_FSMAFAS);
	for (const uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_CALL) {
			orig_FSMAFAS = (FSMAFAS_func)(p + 5 + mem_loads32(p + 1));
			return true;
		}
		NEXT_INSN(p, "FileSystem_ManageAddonsForActiveSession");
	}
	return false;
}

static con_cmdcb orig_show_addon_metadata_cb;

static inline bool find_addonvecsz(void) {
	const uchar *insns = (const uchar*)orig_show_addon_metadata_cb;
	// The show_addon_metadata command does...
	for (const uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_ALUMI8S && p[1] == X86_MODRM(0, 7, 5) && p[6] == 0) {
			addonvecsz = mem_loadptr(p + 2);
			return true;
		}
		NEXT_INSN(p, "addonvecsz");
	}
	return false;
}

static void *broken_addon_check;
static u8 orig_broken_addon_check_bytes[13];
static bool has_broken_addon_check = false;

static inline void fix_broken_addon_check(bool larger_jmp_insn) {
	const u8 nop[] = 
		HEXBYTES(66, 0F, 1F, 84, 00, 00, 00, 00, 00, 0F, 1F, 40, 00);
	memcpy(orig_broken_addon_check_bytes, broken_addon_check, 13);
	if (larger_jmp_insn) {
		if_hot(os_mprot(broken_addon_check, 13, PAGE_EXECUTE_READWRITE)) {
			memcpy(broken_addon_check, nop, 13);
		} else {
			errmsg_warnsys("unable to fix broken addon check: "
					"couldn't make make memory writable");
		}
	} else {
		if_hot(os_mprot(broken_addon_check, 9, PAGE_EXECUTE_READWRITE)) {
			memcpy(broken_addon_check, nop, 9);
		} else {
			errmsg_warnsys("unable to fix broken addon check: "
					"couldn't make make memory writable");
		}
	}
}

static inline bool find_broken_addon_check(void) {
	uchar *insns = (uchar *)orig_FSMAFAS;
	for (uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_ALUMI8S && p[1] == X86_MODRM(0, 7, 5) &&
				mem_loadptr(p + 2) == addonvecsz) {
			broken_addon_check = p;
			has_broken_addon_check = true;
			if (p[7] == X86_2BYTE) {
				fix_broken_addon_check(true);
			} else {
				fix_broken_addon_check(false);
			}
			return true;
		}
		NEXT_INSN(p, "broken addon check");
	}
	return false;
}

PREINIT {
	return GAMETYPE_MATCHES(L4D2_2147plus);
}

INIT {
    struct con_cmd *cmd_show_addon_metadata = con_findcmd("show_addon_metadata");
    if_cold (!cmd_show_addon_metadata) return false;
	orig_show_addon_metadata_cb = con_getcmdcb(cmd_show_addon_metadata);
	if_cold (!find_FSMAFAS()) {
		errmsg_errorx("couldn't find FileSystem_ManageAddonsForActiveSession");
		return false;
	}
	if_cold (!find_addonvecsz()) {
		errmsg_errorx("couldn't find addon metadata counter");
		return false;
	}
	find_broken_addon_check();
	orig_FSMAFAS = (FSMAFAS_func)hook_inline( (void *)orig_FSMAFAS, 
			(void *)&hook_FSMAFAS);
    return true;
}

END {
	if_cold (sst_userunloaded) {
		if (has_broken_addon_check) {
			memcpy(broken_addon_check, orig_broken_addon_check_bytes, 13);
		}
	}
	unhook_inline((void *)orig_FSMAFAS);
}

// vi: sw=4 ts=4 noet tw=80 cc=80
