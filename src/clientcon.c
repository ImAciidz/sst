/*
 * Copyright © Michael Smith <mikesmiffy128@gmail.com>
 * Copyright © Hayden K <imaciidz@gmail.com>
 * Copyright © Willian Henrique <wsimanbrazil@yahoo.com.br>
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

#include "clientcon.h"
#include "con_.h"
#include "engineapi.h"
#include "ent.h"
#include "feature.h"
#include "gamedata.h"
#include "gametype.h"
#include "mem.h"
#include "chunklets/x86.h"
#include "x86util.h"

FEATURE()
REQUIRE(ent)
REQUIRE_GAMEDATA(vtidx_ClientPrintf)
REQUIRE_GLOBAL(engserver)

DECL_VFUNC_DYN(struct VEngineServer, void, ClientPrintf, struct edict *,
		const char *)

typedef void (*UTIL_ClientPrintFilter_func)(void *, int, const char *,
		const char *, const char *, const char *, const char *);
static UTIL_ClientPrintFilter_func orig_UTIL_ClientPrintFilter;

static bool find_UTIL_ClientPrintFilter(struct con_cmd *cmd_ent_dump) {
	const uchar *insns = (const uchar *)cmd_ent_dump->cb;
	for (const uchar *p = insns; p - insns < 128;) {
		if (p[-3] == X86_PUSHI8 && p[-2] == 0x02 && p[0] == X86_CALL) {
			p = p + 5 + mem_loads32(p + 1);
			insns = p;
			for (p = insns; p - insns < 96;) {
				if (p[-1] == X86_PUSHEAX && p[0] == X86_CALL) {
					orig_UTIL_ClientPrintFilter =
					(UTIL_ClientPrintFilter_func)(p + 5 + mem_loads32(p + 1));
					return true;
				}
				NEXT_INSN(p, "UTIL_ClientPrintFilter");
			}
			errmsg_errorx("couldn't find UTIL_ClientPrintFilter");
			return false;
		}
		NEXT_INSN(p, "UTIL_ClientPrintFilter");
	}
	errmsg_errorx("couldn't find UTIL_ClientPrintFilter");
	return false;
}


// CBroadcastRecipientFilter
struct FortniteStruct {
	void *one;
	int two;
	int three;
	int four;
	int five;
	int six;
	bool test;
	bool farp;
	bool cock;
	bool penis;
	bool benis;
	bool billswenis;
	//void *nine;
};

// CBroadcastRecipientFilter
static void *interesting_vtable;

static void *effects;

typedef void (*VCALLCONV AddAllPlayers_func)(void*);
static AddAllPlayers_func orig_AddAllPlayers;

static bool find_AddAllPlayers(void *CFS_Beam) {
	uchar *insns = (uchar *)CFS_Beam;
	int callcnt = 0;
	for (const uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_CALL && ++callcnt == 2) {
			orig_AddAllPlayers = (AddAllPlayers_func)(p + 5 + mem_loads32(p + 1));
			return true;
		}
		NEXT_INSN(p, "AddAllPlayers");
	}
	errmsg_errorx("couldn't find AddAllPlayers");
	return false;
}

static bool find_interesting_vtable(void *CFS_Beam) {
	uchar *insns = (uchar *)CFS_Beam;
	for (const uchar *p = insns; p - insns < 32;) {
		if (GAMETYPE_MATCHES(L4D2_2125plus)) {
			if (p[0] == X86_MOVMIW && p[1] == 0x45 && p[2] == 0xE0) {
				// mov dword ptr [EBP + local_24],PTR_FUN_105cb78c
				interesting_vtable = mem_loadptr(p + 3);
				return true;
			}
		} else {
			if (p[0] == 0xC7 && p[1] == 0x44 && p[2] == 0x24 && p[3] == 0x04) {
				// mov dword ptr [ESP + local_20],PTR_FUN_10425e24
				interesting_vtable = mem_loadptr(p + 4);
				return true;
			}
		}
		NEXT_INSN(p, "interesting vtable");
	}
	errmsg_errorx("couldn't find the interesting vtable");
	return false;
}

void clientcon_msg(struct edict *e, const char *s) {
	ClientPrintf(engserver, e, s);
}

void clientcon_reply(const char *s) {
	struct edict *e = ent_getedict(con_cmdclient + 1);
	if (e) { clientcon_msg(e, s); return; }
}

void chat_msg(const char *msg) {
	struct FortniteStruct balls = {0};
	balls.one = interesting_vtable;
	orig_AddAllPlayers(&balls);
	orig_UTIL_ClientPrintFilter(&balls, 3, msg, "", "", "", "");
	return;
}

// this is supposedly stable from 2003 onwards so yippee
struct IEffects;
DECL_VFUNC(struct IEffects, void, Beam, 1)

INIT {
	effects = factory_server("IEffects001", 0);
	void *dildo = (void *)VFUNC(effects, Beam);
	if_cold (!find_AddAllPlayers(dildo)) return FEAT_INCOMPAT;
	if_cold (!find_interesting_vtable(dildo)) return FEAT_INCOMPAT;

	struct con_cmd *cmd_ent_dump = con_findcmd("ent_dump");
	if_cold (!cmd_ent_dump) return FEAT_INCOMPAT;
	if_cold (!find_UTIL_ClientPrintFilter(cmd_ent_dump)) return FEAT_INCOMPAT;
	return FEAT_OK;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
