/*
 * Copyright © 2022 Michael Smith <mikesmiffy128@gmail.com>
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

#include <string.h>

#include "con_.h"
#include "engineapi.h"
#include "ent.h"
#include "errmsg.h"
#include "event.h"
#include "feature.h"
#include "gamedata.h"
#include "gametype.h"
#include "intdefs.h"
#include "hook.h"
#include "mem.h"
#include "os.h"
#include "sst.h"
#include "vcall.h"

FEATURE("autojump")
REQUIRE_GAMEDATA(off_mv)
REQUIRE_GAMEDATA(vtidx_CheckJumpButton)
REQUIRE_GLOBAL(factory_client) // note: server will never be null

DEF_CVAR(sst_autojump, "Jump upon hitting the ground while holding space", 0,
		CON_REPLICATE | CON_DEMO | CON_HIDDEN)

#define IN_JUMP 2
#define NIDX 256 // *completely* arbitrary lol
static bool justjumped[NIDX] = {0};
static inline int handleidx(ulong h) { return h & (1 << 12) - 1; }

static void *gmsv = 0, *gmcl = 0;
typedef bool (*VCALLCONV CheckJumpButton_func)(void *);
static CheckJumpButton_func origsv, origcl;

DECL_VFUNC_DYN(void *, GetBaseEntity)
DEF_CVAR(sst_jumpman, "Allows you to double jump", 0,
		CON_REPLICATE | CON_DEMO | CON_HIDDEN)
static bool canjumpman = false;
static bool hasdj[NIDX] = {0};
extern bool has_off_fallvel;
extern int off_fallvel;

static void **vtterrorplayer = 0;
static bool halffalldmg = false, trigfix = false, triedontakedamagehook = false;
// should be (void *, struct CTakeDamageInfo *), but CTakeDamageInfo
// isn't stable between games
typedef int (*VCALLCONV OnTakeDamage_func)(void *, void *);
static OnTakeDamage_func origtakedmg;
struct con_var *director_no_survivor_bots = 0;

static void jumpmancb(struct con_var *v) {
	con_setvari(director_no_survivor_bots, con_getvari(v));
}

static void dojumpman(void *player, struct CMoveData *mv,
		bool *playerhasdj, bool onground) {
	if (!onground && *playerhasdj) {
		float *fallvel = (float *)mem_offset(player, off_fallvel);
		*playerhasdj = false;
		mv->vel.z = 350;
		*fallvel = 0; // resets fall dmg and fixes client interp issues
	}
}

static bool VCALLCONV hooksv(void *this) {
	struct CMoveData *mv = mem_loadptr(mem_offset(this, off_mv));
	int idx = handleidx(mv->playerhandle);
	// check these before autojump potentially clears the jump flag
	// and CheckJumpButton clears the onground flag
	bool shouldjumpman = idx == 1 && canjumpman && con_getvari(sst_jumpman);
	bool pressedjump = (mv->buttons & IN_JUMP) && !(mv->oldbuttons & IN_JUMP);
	bool onground = false;
	if (shouldjumpman) {
		void *player = mem_loadptr(mem_offset(this, off_gm_player));
		int flags = *(int *)mem_offset(player, off_flags);
		onground = flags & 1;
	}

	if (con_getvari(sst_autojump) && mv->firstrun && !justjumped[idx]) {
		mv->oldbuttons &= ~IN_JUMP;
	}
	bool ret = origsv(this);
	if (mv->firstrun) justjumped[idx] = ret;

	if (shouldjumpman) {
		void *player = mem_loadptr(mem_offset(this, off_gm_player));
		if(pressedjump) dojumpman(player, mv, &hasdj[idx], onground);
	}
	return ret;
}

static bool VCALLCONV hookcl(void *this) {
	struct CMoveData *mv = mem_loadptr(mem_offset(this, off_mv));
	// FIXME: this will stutter in the rare case where justjumped is true.
	// currently doing clientside justjumped handling makes multiplayer
	// prediction in general wrong, so this'll need more work to do totally
	// properly.
	//if (con_getvari(sst_autojump) && !justjumped[0]) mv->oldbuttons &= ~IN_JUMP;
	if (con_getvari(sst_autojump)) mv->oldbuttons &= ~IN_JUMP;
	return justjumped[0] = origcl(this);
}

static int VCALLCONV hooktakedmg(void *this, void *info) {
	const int DMG_FALL = 32;
	int *dmgtype = (int *)mem_offset(info, off_dmgtype);
	float *dmg = (float *)mem_offset(info, off_dmg);
	// Remove the DMG_FALL flag from triggers so they hurt the player normally
	if (trigfix) {
		ulong inflictor = *(ulong *)mem_offset(info, off_inflictor);
		int idx = handleidx(inflictor);
		struct edict *ed = ent_getedict(idx);
		if (ed && ed->ent_unknown) {
			void *e = GetBaseEntity(ed->ent_unknown);
			char **nameptr = mem_offset(e, off_classname);
			if (e && *nameptr && !strcmp(*nameptr, "trigger_hurt"))
				*dmgtype &= ~DMG_FALL;
		}
	}
	if (*dmgtype & DMG_FALL) *dmg /= 2;
	return origtakedmg(this, info);
}

HANDLE_EVENT(Tick) {
	if (!canjumpman || !con_getvari(sst_jumpman)) return;

	const int playeridx = 1; // TODO: make it work on coop someday
	struct edict *ed = ent_getedict(playeridx);
	if (!ed || !ed->ent_unknown) return;
	void *player = GetBaseEntity(ed->ent_unknown);
	if (!player) return;

	// hook OnTakeDamage to halve falling damage
	if (!triedontakedamagehook && halffalldmg) {
		triedontakedamagehook = true;
		vtterrorplayer = *(void ***)player;
		if (!os_mprot(vtterrorplayer + vtidx_OnTakeDamage, sizeof(void *),
				PAGE_READWRITE)) {
			errmsg_errorsys("couldn't make virtual table writable");
			con_warn("player will take full fall damage\n");
			return;
		}
		origtakedmg = (OnTakeDamage_func)hook_vtable(vtterrorplayer,
			vtidx_OnTakeDamage, (void *)&hooktakedmg);
	}
	int flags = *(int *)mem_offset(player, off_flags);
	bool onground = flags & 1;
	hasdj[playeridx] |= onground; 
}

static bool unprot(void *gm) {
	void **vtable = *(void ***)gm;
	bool ret = os_mprot(vtable + vtidx_CheckJumpButton, sizeof(void *),
			PAGE_READWRITE);
	if (!ret) errmsg_errorsys("couldn't make virtual table writable");
	return ret;
}

static bool initjumpman() {
	if (!(has_off_fallvel && has_off_flags && has_off_gm_player))
		return false;

	trigfix = has_off_classname && has_off_inflictor;
	if (!trigfix)
		con_warn("triggers may not damage the player correctly in jumpman\n");

	if (GAMETYPE_MATCHES(L4D)) {
		director_no_survivor_bots = con_findvar("director_no_survivor_bots");
		if (!director_no_survivor_bots)
			con_warn("bots will spawn in jumpman\n");
		else
			sst_jumpman->cb = &jumpmancb;
	}

	halffalldmg = has_off_dmg && has_off_dmgtype;
	if (!halffalldmg)
		con_warn("player will take full fall damage in jumpman\n");

	sst_jumpman->base.flags &= ~CON_HIDDEN;
	return true;
}

INIT {
	gmsv = factory_server("GameMovement001", 0);
	if (!gmsv) {
		errmsg_errorx("couldn't get server-side game movement interface");
		return false;
	}
	if (!unprot(gmsv)) return false;
	gmcl = factory_client("GameMovement001", 0);
	if (!gmcl) {
		errmsg_errorx("couldn't get client-side game movement interface");
		return false;
	}
	if (!unprot(gmcl)) return false;
	origsv = (CheckJumpButton_func)hook_vtable(*(void ***)gmsv,
			vtidx_CheckJumpButton, (void *)&hooksv);
	origcl = (CheckJumpButton_func)hook_vtable(*(void ***)gmcl,
			vtidx_CheckJumpButton, (void *)&hookcl);

	sst_autojump->base.flags &= ~CON_HIDDEN;
	if (GAMETYPE_MATCHES(Portal1)) {
		// this is a stupid, stupid policy that doesn't make any sense, but I've
		// tried arguing about it already and with how long it takes to convince
		// the Portal guys of anything I'd rather concede for now and maybe try
		// and revert this later if anyone eventually decides to be sensible.
		// the alternative is nobody's allowed to use SST in runs - except of
		// course the couple of people who just roll the dice anyway, and
		// thusfar haven't actually been told to stop. yeah, whatever.
		sst_autojump->base.flags |= CON_CHEAT;
	}
	canjumpman = initjumpman();
	return true;
}

END {
	unhook_vtable(*(void ***)gmsv, vtidx_CheckJumpButton, (void *)origsv);
	unhook_vtable(*(void ***)gmcl, vtidx_CheckJumpButton, (void *)origcl);
	if (origtakedmg) {
		unhook_vtable(vtterrorplayer, vtidx_OnTakeDamage, (void *)origtakedmg);
		vtterrorplayer = 0; origtakedmg = 0; director_no_survivor_bots = 0;
		triedontakedamagehook = false;
	}
}

// vi: sw=4 ts=4 noet tw=80 cc=80
