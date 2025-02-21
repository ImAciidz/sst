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

// Keeping this here since it will be useful for Future Addon Feature Plans™
/*struct SAddOnMetaData { // literal psychopath capitalization
	char absolutePath;
	char name;
	int type; // (0 = content, 1 = mission, 2 = mode)
	int unknown;
};*/

// count of how many addon metadata entries there is - this is the m_Size member
// of s_vecAddonMetaData (which is a CUtlVector<SAddOnMetaData>)
static int *addonvecsz;
static char last_mission[128] = {0};
static char last_gamemode[128] = {0};
static int old_addonvecsz;
bool old_is_addons_banned;

// note: the 4th parameter was first added in 2.2.0.4 (Oct 21st 2020), but
// we don't have to worry about that since it's cdecl
typedef void (*FSMAFAS_func)(bool, char *, char *, bool);
static FSMAFAS_func orig_FSMAFAS;
// p1: if addons are disallowed in this mode (versus, scav, etc)
// p2: campaign/mission
// p3: gamemode
// p4: is cur mode a mutation
static void hook_FSMAFAS(bool p1, char *p2, char *p3, bool p4) {
	// When you load into a level, most noticeably on versions 2.2.0.4 and
	// beyond (check nop_addon_check() for context) and in campaigns with L4D1
	// common infected, you get can get a ton of hitches caused by the game
	// trying to load uncached materials as models are rendered. This function
	// (FileSystem_ManageAddonsForActiveSession, FSMAFAS for short) is the
	// primary cause of materials being uncached. When hosting a server, you get
	// 2 calls to this function per map load: first by the server module calling
	// CVEngineServer::MAFAS and then by the client module calling
	// CEngineClient::MAFAS (non-hosts only get this call). Omitting the second
	// call (for partially unknown reasons) fixes most hitches, but the work
	// done by FSMAFAS can be omitted in a few more cases. The function tries to
	// evaluate which addon vpks should be loaded in the FS based on the current
	// gamemode, campaign and addon restrictions, adding and removing vpk's from
	// the FS interface as necessary, and invalidates many caches (material,
	// model and audio) to ensure proper reload of the assets when needed.
	// Given that the enabled addons and parameters passed to this function
	// change rarely, we can avoid unnecessary cache invalidation by checking
	// the parameters passed to the function and if addons are enabled
	// currently. Enabled addons and addon restrictions are not expected to
	// change mid-campaign and resources can remain cached between maps of the
	// same campaign and mode. Both the action of disconnecting from a server
	// and using the addons menu call FSMAFAS to allow every enabled vpk to be
	// loaded, so we let those calls go through. The only edge case not handled
	// by this implementation is if you're a client reconnecting to the same
	// server you were just on, and the server hasn't changed maps (it's unclear
	// why hitches still occur in this case and would require further research
	// on the cache states throughout loads). Finally, as a bonus, every call to
	// FSMAFAS that we avoid saves about 1s on loads, so that's nice.

	// assumption: enabled addons aren't being changed mid-campaign.
	// originally we assumed mode config for addon blocking would be static as
	// well, but you could connect to a different server on the same campaign
	// and gamemode as your current server that has a different config, which
	// would lead to you having addons enabled on a server that forbids them

	int curaddonvecsz = *addonvecsz;
	// addons changed, which means we are in the main menu, another call to
	// FSMAFAS with null p2 and/or p3 already happened and our "last_" variables
	// have been cleared already. update the addon count and run original
	// function
	if (curaddonvecsz != old_addonvecsz) {
		old_addonvecsz = curaddonvecsz;
		goto hook_end;
	}

	// addons didn't change and no addons are enabled: do nothing
	if (!curaddonvecsz) return;

	// cache campaign and mode names and, if we were given a campaign and a
	// mode name, and the campaign is not an empty string, try to early exit.
	// campaign can be an empty string if you are playing a gamemode not
	// supported by the current level (such as survival on c8m1), so always
	// execute FSMAFAS when that is the case, since we can't know if we
	// changed campaigns since the last time FSMAFAS was executed.
	if (p2 && p3 && *p2) {
		bool earlyret = !memcmp(p2, last_mission, sizeof(last_mission))
				&& !memcmp(p3, last_gamemode, sizeof(last_gamemode))
				&& (p1 == old_is_addons_banned);
		if (earlyret) return;
		// We're not certain what the supported max length for mission
		// filenames is. It's something higher than 128, but less than 200 or
		// so. Nobody in their right mind should be creating custom maps or
		// modes with names this long, so we will just cap at 128. If someone
		// encounters a map (or mode) that exceeds this limit, we can look into
		// raising it.
		int len1 = strnlen(p2, sizeof(last_mission));
		if_cold (len1 == sizeof(last_mission)) {
			errmsg_errorx("mission string exceeded assumed max length. If you"
					"see this, please contact an SST developer.");
			goto elseblock;
		}
		int len2 = strnlen(p3, sizeof(last_gamemode));
		if_cold (len2 == sizeof(last_gamemode)) {
			errmsg_errorx("gamemode string exceeded assumed max length. If you"
					"see this, please contact an SST developer.");
			goto elseblock;
		}
		memcpy(last_mission, p2, len1);
		memcpy(last_gamemode, p3, len2);
		old_is_addons_banned = p1;
	} else {
elseblock:
		old_is_addons_banned = false;
		last_mission[0] = '\0';
		last_gamemode[0] = '\0';
	}
hook_end:
	orig_FSMAFAS(p1, p2, p3, p4);
}

DECL_VFUNC(void, CEC_MAFAS, 179) // CEngineClient::ManageAddonsForActiveSession

static inline bool find_FSMAFAS(void) {
	const uchar *insns = (const uchar*)VFUNC(engclient, CEC_MAFAS);
	// CEngineClient::ManageAddonsForActiveSession just calls FSMAFAS
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
	// show_addon_metadata immediately checks if s_vecAddonMetadata.m_Size is
	// 0, so we can just grab it from the CMP instruction
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

static inline void nop_addon_check(bool larger_jmp_insn) {
	// In versions prior to 2.2.0.4 (Oct 21st 2020), FSMAFAS checks if any
	// addons are enabled before doing anything else. If no addons are enabled,
	// then the function just returns immediately. FSMAFAS gets called by
	// update_addon_paths, and that gets called when you click 'Done' in the
	// addons menu. This is problematic because whatever addons you last
	// disabled won't get properly disabled since the rest of FSMAFAS doesn't
	// execute. If, for example, you only had a common infected retexture addon
	// enabled, and you decided to disable it, your commons would be invisible
	// until you either restarted the game or enabled another addon.
	// We simply NOP the relevant CMP and JZ instructions to get rid of this
	// check so that FSMAFAS always executes. Depending on the size of the JZ
	// instruction, we need to NOP either 9 bytes (e.g. 2.2.0.3) or 13 bytes
	// (e.g. 2.1.4.7). So, we always write a 9-byte NOP, and sometimes also
	// write a 4-byte NOP afterwards.
	const u8 nop[] =
		HEXBYTES(66, 0F, 1F, 84, 00, 00, 00, 00, 00, 0F, 1F, 40, 00);
	memcpy(orig_broken_addon_check_bytes, broken_addon_check, 13);
	int nop_size = larger_jmp_insn ? 13 : 9;
	if_hot(os_mprot(broken_addon_check, nop_size, PAGE_EXECUTE_READWRITE)) {
		memcpy(broken_addon_check, nop, nop_size);
	}
	else {
		errmsg_warnsys("unable to fix broken addon check: "
				"couldn't make make memory writable");
	}
}

static inline void try_fix_broken_addon_check(void) {
	uchar *insns = (uchar *)orig_FSMAFAS;
	for (uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_ALUMI8S && p[1] == X86_MODRM(0, 7, 5) &&
				mem_loadptr(p + 2) == addonvecsz) {
			broken_addon_check = p;
			nop_addon_check(p[7] == X86_2BYTE);
			return;
		}
		int _len = x86_len(p);
		if_cold(_len == -1) {
			errmsg_errorx("unknown or invalid instruction looking for broken "
				"addon check");
			return;
		}
		p += _len;
	}
	return;
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
	try_fix_broken_addon_check();
	orig_FSMAFAS = (FSMAFAS_func)hook_inline( (void *)orig_FSMAFAS,
			(void *)&hook_FSMAFAS);
	return true;
}

END {
	if_cold (sst_userunloaded) {
		if (broken_addon_check) {
			memcpy(broken_addon_check, orig_broken_addon_check_bytes, 13);
		}
	}
	unhook_inline((void *)orig_FSMAFAS);
}

// vi: sw=4 ts=4 noet tw=80 cc=80
