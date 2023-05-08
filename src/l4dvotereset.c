/*
 * Copyright © 2023 Willian Henrique <wsimanbrazil@yahoo.com.br>
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
#include "ent.h"
#include "errmsg.h"
#include "feature.h"
#include "gamedata.h"
#include "gametype.h"
#include "intdefs.h"
#include "mem.h"
#include "x86.h"
#include "x86util.h"

FEATURE("Left 4 Dead vote cooldown resetting")
REQUIRE_GAMEDATA(vtidx_Spawn)

static void **ptr_votecontroller = 0;
static int off_callerrecords = 0;

// Elements on the vote caller record vector
// Ended up not needing it, but might as well keep the RE'd struct here
/*struct CallerRecord {
	u32 steamid_trunc;
	float last_vote_time;
	int votes_passed;
	int votes_failed;
	int last_issue_idx;
	bool last_vote_passed;
};*/

DEF_CCMD_HERE_UNREG(sst_l4d_vote_creation_timer_reset,
		"Reset the vote cooldown for all players", CON_SERVERSIDE | CON_CHEAT) {
	void *votectrlr = *ptr_votecontroller;
	if (!votectrlr) {
		con_warn("Vote controller not initialized\n");
		return;
	}
	// Basically equivalent to CUtlVector::RemoveAll. The elements don't need
	// to be destructed. This state is equivalent to when no one has voted yet
	struct CUtlVector *recordvector = mem_offset(votectrlr, off_callerrecords);
	recordvector->sz = 0;
}

PREINIT {
	return GAMETYPE_MATCHES(L4Dx) && !!con_findvar("sv_vote_creation_timer");
}

// The "listissues" command calls CVoteController::ListIssues, loading
// g_voteController into ECX
static inline bool find_votecontroller(con_cmdcbv1 cmdcb) {
	const uchar *insns = (const uchar *)cmdcb;
	for (const uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_MOVRMW && p[1] == X86_MODRM(0, 1, 5)) {
			ptr_votecontroller = mem_loadptr(p + 2);
			return true;
		}
		NEXT_INSN(p, "g_voteController object");
	}
	return false;
}

// This finds the caller record vector using a pointer to the 
// CVoteController::Spawn function
static inline bool find_votecallers(void* votectrlspawn) {
	const uchar *insns = (const uchar *)votectrlspawn;
	for (const uchar *p = insns; p - insns < 64;) {
		// Unsure what the member on this offset actually is (the game seems to
		// want it to be set to 0 to allow votes to happen), but the vector we
		// want seems to consistently be 8 bytes after whatever this is
		// "mov dword ptr [<reg> + off], 0", mod == 0b11
		if (p[0] == X86_MOVMIW && (p[1] & 0xC0) == 0x80 &&
				mem_load32(p + 6) == 0) {
			off_callerrecords = mem_load32(p + 2) + 8;
			return true;
		}
		NEXT_INSN(p, "vote caller record vector");
	}
	return false;
}

INIT {
	struct con_cmd *cmd_listissues = con_findcmd("listissues");
	if (!cmd_listissues) {
		errmsg_errorx("couldn't find \"listissues\" command");
		return false;
	}
	con_cmdcbv1 listissues_cb = con_getcmdcbv1(cmd_listissues);
	if (!find_votecontroller(listissues_cb)) {
		errmsg_errorx("couldn't find vote controller instance");
		return false;
	}

	// g_voteController may have not been initialized yet so we get the vtable
	// from the ent factory
	const struct CEntityFactory *vote_fact = ent_getfactory("vote_controller");
	if (!vote_fact) {
		errmsg_errorx("couldn't find vote controller entity factory");
		return false;
	}
	void **vtable = ent_findvtable(vote_fact, "CVoteController");
	if (!vtable) {
		errmsg_errorx("couldn't find vote controller vtable");
		return false;
	}
	if (!find_votecallers(vtable[vtidx_Spawn])) {
		errmsg_errorx("couldn't find vote callers vector offset");
		return false;
	}

	con_reg(sst_l4d_vote_creation_timer_reset);
	return true;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
