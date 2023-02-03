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

#include <stdlib.h> // used in generated code
#include <string.h> // "

#include "con_.h"
#include "engineapi.h"
#include "gamedata.h"
#include "gameinfo.h"
#include "gametype.h"
#include "intdefs.h"
#include "mem.h" // "
#include "os.h"
#include "vcall.h"
#include "x86.h"

u64 _gametype_tag = 0; // declared in gametype.h but seems sensible enough here

ifacefactory factory_client = 0, factory_server = 0, factory_engine = 0,
		factory_inputsystem = 0;

struct VEngineClient *engclient;
struct VEngineServer *engserver;

// this seems to be very stable, thank goodness
DECL_VFUNC(void *, GetGlobalVars, 1)
void *globalvars;

DECL_VFUNC_DYN(void *, GetAllServerClasses)

#include <entpropsinit.gen.h>

// nasty terrible horrible globals for jumpstuff to use
bool has_off_fallvel;
int off_fallvel;

static void initjumpprops(struct ServerClass *class) {
	if (!(has_off_SP_dt)) return;
	for (; class; class = class->next) {
		if (!strcmp(class->name, "CBasePlayer")) {
			struct SendTable *st = class->table,
				*st_local = NULL, *st_mlocal = NULL;
			for (struct SendProp *p = st->props;
					mem_diff(p, st->props) < st->nprops * sz_SendProp;
					p = mem_offset(p, sz_SendProp)) {
				const char *varname = mem_loadptr(mem_offset(p, off_SP_varname));
				if (!strcmp(varname, "localdata")) {
					st_local = mem_loadptr(mem_offset(p, off_SP_dt));
					break;
				}
			}
			if (!st_local) break;

			for (struct SendProp *p = st_local->props;
					mem_diff(p, st_local->props) < st_local->nprops * sz_SendProp;
					p = mem_offset(p, sz_SendProp)) {
				const char *varname = mem_loadptr(mem_offset(p, off_SP_varname));
				if (!strcmp(varname, "m_Local")) {
					st_mlocal = mem_loadptr(mem_offset(p, off_SP_dt));
					off_fallvel = abs(*(int *)mem_offset(p, off_SP_offset));
					break;
				}
			}
			if (!st_mlocal) break;

			for (struct SendProp *p = st_mlocal->props;
					mem_diff(p, st_mlocal->props) < st_mlocal->nprops * sz_SendProp;
					p = mem_offset(p, sz_SendProp)) {
				const char *varname = mem_loadptr(mem_offset(p, off_SP_varname));
				if (!strcmp(varname, "m_flFallVelocity")) {
					has_off_fallvel = true;
					off_fallvel += abs(*(int *)mem_offset(p, off_SP_offset));
					break;
				}
			}
			break;
		}
	}
}

bool engineapi_init(int pluginver) {
	if (!con_detect(pluginver)) return false;

	if (engclient = factory_engine("VEngineClient015", 0)) {
		_gametype_tag |= _gametype_tag_Client015;
	}
	else if (engclient = factory_engine("VEngineClient014", 0)) {
		_gametype_tag |= _gametype_tag_Client014;
	}
	else if (engclient = factory_engine("VEngineClient013", 0)) {
		_gametype_tag |= _gametype_tag_Client013;
	}
	else if (engclient = factory_engine("VEngineClient012", 0)) {
		_gametype_tag |= _gametype_tag_Client012;
	}

	if (engserver = factory_engine("VEngineServer021", 0)) {
		_gametype_tag |= _gametype_tag_Server021;
	}
	// else if (engserver = others as needed...) {
	// }

	void *pim = factory_server("PlayerInfoManager002", 0);
	if (pim) globalvars = GetGlobalVars(pim);

	void *srvdll;
	// TODO(compat): add this back when there's gamedata for 009 (no point atm)
	/*if (srvdll = factory_engine("ServerGameDLL009", 0)) {
		_gametype_tag |= _gametype_tag_SrvDLL009;
	}*/
	if (srvdll = factory_server("ServerGameDLL005", 0)) {
		_gametype_tag |= _gametype_tag_SrvDLL005;
	}

	// detect p1 for the benefit of specific features
	if (!GAMETYPE_MATCHES(Portal2) && con_findcmd("upgrade_portalgun")) {
		_gametype_tag |= _gametype_tag_Portal1;
	}

	if (GAMETYPE_MATCHES(L4D2)) {
		if (con_findvar("director_cs_weapon_spawn_chance")) {
			_gametype_tag |= _gametype_tag_TheLastStand;
		}
		else if (con_findvar("sv_zombie_touch_trigger_delay")) {
			_gametype_tag |= _gametype_tag_L4D2_2147;
		}
	}

	// need to do this now; ServerClass network table iteration requires
	// SendProp offsets
	gamedata_init();
	con_init();
	if (!gameinfo_init()) { con_disconnect(); return false; }
	if (has_vtidx_GetAllServerClasses && has_sz_SendProp &&
			has_off_SP_varname && has_off_SP_offset) {
		struct ServerClass *class_head = GetAllServerClasses(srvdll);
		initentprops(class_head);
		initjumpprops(class_head);
	}
	return true;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
