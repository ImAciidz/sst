/*
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

#include <stdio.h>

#include "clientcon.h"
#include "con_.h"
#include "errmsg.h"
#include "feature.h"
#include "gamedata.h"
#include "gametype.h"
#include "hook.h"
#include "intdefs.h"
#include "langext.h"
#include "mem.h"
#include "os.h"
#include "sst.h"
#include "vcall.h"
#include "chunklets/x86.h"
#include "x86util.h"


// Players sometimes randomly get kicked with the No Steam Logon error, despite
// being logged into Steam. This feature prevents the player from getting 
// kicked, and sends a message in chat to indicate that it was prevented,
// as well as providing the reason why (for curiosity purposes)

// michael i swear to god if you kill the chat print feature i will jump
// from a ground floor window - hayden

FEATURE("No Steam Logon patche")
// untested on L4D:S, and besides, who cares.
// theoretically this can be expanded to Portal 2 in the future, as people
// experience it in coop there too
GAMESPECIFIC(L4D)
REQUIRE(clientcon)
REQUIRE_GAMEDATA(off_NoSteamLogonCallback)
REQUIRE_GAMEDATA(vtidx_GetClientName)

void *Steam3Server = 0;

// find CSteam3Server instance in hostname callback
static bool find_Steam3ServerObject(struct con_var *cvar_hostname) {
	const uchar *insns = (const uchar *)cvar_hostname->cb;
	for (const uchar *p = insns; p - insns < 24;) {
		if (p[0] == X86_CALL) {
			p = p + 5 + mem_loads32(p + 1);
			if (p[0] == X86_MOVEAXI) {
				Steam3Server = mem_loadptr(p + 1);
				return true;
			}
			else {
				errmsg_errorx("couldn't find Steam3Server object");
				return false;
			}
		}
		NEXT_INSN(p, "Steam3Server");
	}
	return false;
}

struct CCallback {
	void *vtable;
	u8 cb_flags;
	int cb_id;
	void *cb_thisptr;
	struct {
		void * cb_ptr;
		int thisptr_offset; // this might be incorrect
	} cb_member_func;
};

// CSteam3Server::OnGSClientDenyHelper (L4D1 + L4D2 2000-2105)
typedef void (*VCALLCONV ClientDenyHelper_func)(void *, void *, uint,
		char const *);
DEF_INLINE_HOOK(ClientDenyHelper_func, ClientDenyHelper)
// CSteam3Server::OnValidateAuthResponseTicketHelper (L4D2 2106+)
typedef void (*VCALLCONV OVATRH_func)(void *, void *, uint);
DEF_INLINE_HOOK(OVATRH_func, OVATRH)

/* idk how to bring this back with the update to the hooking api
static union { 
	ClientDenyHelper_func def;
	OVATRH_func L4D2_2106plus;
} orig_NoSteamLogonFunc;
*/

// make typesafe? 
DECL_VFUNC_DYN(void, char *, GetClientName)

static void VCALLCONV hookdest_ClientDenyHelper(void *this, void *cl, uint reason, 
			char const *optionaltext) {
	orig_ClientDenyHelper(this, cl, reason, optionaltext);
	char *name = GetClientName((mem_offset(cl, 4)));
	const char *stringptr;
	switch(reason) {
		case 3:
			stringptr = "k_EDenyNotLoggedOn";
			break;
		case 11:
			stringptr = "k_EDenySteamConnectionLost";
			break;
		case 12:
			stringptr = "k_EDenySteamConnectionError";
			break;
		default:
			return;
	}
	char buffer[250];
	snprintf(buffer, sizeof(buffer), "No Steam Logon prevented for %s. "
			"Reason: %s", name, stringptr);
	chat_msg(buffer);
}

static void VCALLCONV hookdest_OVATRH(void *this, void *cl, uint reason) {
	orig_OVATRH(this, cl, reason);
	char *name = GetClientName((mem_offset(cl, 4)));
	const char *stringptr;
	switch(reason) {
		case 1:
			stringptr = "k_EAuthSessionResponseUserNotConnectedToSteam";
			break;
		case 6:
			stringptr = "k_EAuthSessionResponseAuthTicketCanceled";
			break;
		case 7:
			stringptr = "k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed";
			break;
		case 8:
			stringptr = "k_EAuthSessionResponseAuthTicketInvalid";
			break;
		default:
			return;
	}
	char buffer[250];
	snprintf(buffer, sizeof(buffer), "No Steam Logon prevented for %s. "
			"Reason: %s", name, stringptr);
	chat_msg(buffer);
}


static struct CCallback *find_Steam3Callback(void) {
	struct CCallback *cb = mem_offset(Steam3Server, off_NoSteamLogonCallback);
	// the offset and cb_id are always the same in L4D1
	// but in L4D2, the offset pretty much changes constantly
	// so we end up just iterating until we find our desired callback
	// for versions before 2.1.0.6, the cb_id is 203, otherwise it's 143
	if (GAMETYPE_MATCHES(L4D1))
		return cb->cb_id == 203 ? cb : 0;

	while (mem_diff(cb, Steam3Server) <= off_NoSteamLogonCallback + 20) {
		if (cb->cb_id == 143 || cb->cb_id == 203) {
			return cb;
		}
		cb = mem_offset(cb, 4);
	}
	return 0;
}

static void *NoSteamLogonFunc;

static bool find_NoSteamLogonFunc(void *cb_ptr) {
	const uchar *insns = cb_ptr;
	int callcnt = 0;
	for (const uchar *p = insns; p - insns < 80;) {
		if (p[0] == X86_CALL && ++callcnt == 2) {
			NoSteamLogonFunc = (void *)(p + 5 + mem_loads32(p + 1));
			return true;
		}
		NEXT_INSN(p, "NoSteamLogon func");
	} 
	errmsg_errorx("couldn't find NoSteamLogon func");
	return false;
}

static void **jumptable;
static void *orig_jumptable_addrs[4];
static void *retaddr;
// in 2.1.0.6+ there are four jump table indices we want to patch/swap
// these indices are tied to the 'Reason' enum
int patchcases[] = {1, 6, 7, 8};
int patchcnt = 4;

static bool find_jumptable(int cb_id) {
	uchar *insns = NoSteamLogonFunc;
	for (uchar *p = insns; p - insns < 128;) {
		if (p[0] == X86_MISCMW && p[1] == 0x24 && p[2] == 0x85) {
			jumptable = (void *)(mem_loadptr(p + 3));
			goto ok;
		}
		NEXT_INSN(p, "jump table"); 
	}
	return false;
ok:
	// find the end of the no steam logon func
	if (cb_id == 203) { // old func
		insns = jumptable[2];
	} 
	else { // new func
		insns = jumptable[0];
	}
	for (uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_2BYTE && p[1] == X86_2B_JZII) {
			retaddr = (void*)(p + 6 + mem_loads32(p + 2));
			return true;
		}
		NEXT_INSN(p, "return address");
	}
	return false;
}

INIT {
	struct con_var *cvar_hostname = con_findvar("hostname");
	if_cold (!cvar_hostname) return FEAT_FAIL;
	find_Steam3ServerObject(cvar_hostname);
	struct CCallback *cb;
	if_cold (!(cb = find_Steam3Callback())) {
		errmsg_errorx("couldn't find Steam3 callback");
		return FEAT_FAIL;
	}
	if_cold(!find_NoSteamLogonFunc(cb->cb_member_func.cb_ptr)) {
		errmsg_errorx("couldn't find No Steam Logon function");
		return FEAT_FAIL;
	}
	if_cold(!find_jumptable(cb->cb_id)) {
		errmsg_errorx("couldn't find jump table in No Steam Logon function");
		return FEAT_FAIL;
	}
	if(!os_mprot(jumptable, 12 * sizeof(void *), PAGE_EXECUTE_READWRITE)) {
		errmsg_errorsys("couldn't make jump table writable");
		return FEAT_FAIL;
	}
	// before 2106 or L4D1 - old func
	if (cb->cb_id == 203) {
		patchcnt = 3;
		patchcases[0] = 3; patchcases[1] = 11; patchcases[2] = 12;
		struct hook_featsetup_ret_ClientDenyHelper h = 
				hook_featsetup_ClientDenyHelper((ClientDenyHelper_func)NoSteamLogonFunc);
		if_cold (h.err) return h.err;
		hook_commit_ClientDenyHelper(h.hookpos, &hookdest_ClientDenyHelper);
	}
	// after 2106 - new func
	else {
		struct hook_featsetup_ret_OVATRH h = hook_featsetup_OVATRH(
				(OVATRH_func)NoSteamLogonFunc);
		if_cold (h.err) return h.err;
		hook_commit_OVATRH(h.hookpos, &hookdest_OVATRH);
	}
	// replace the offending switch cases with the return of the function
	for (int i = 0; i < patchcnt; i++) {
		// rip hook_vtable
		orig_jumptable_addrs[i] = jumptable[patchcases[i]-1];
		jumptable[patchcases[i]-1] = retaddr;
	}
	return FEAT_OK;
}

END {
	if_cold (sst_userunloaded) {
		for (int i = 0; i < patchcnt; i++) {
			jumptable[patchcases[i]-1] = orig_jumptable_addrs[i];
		}
		if (patchcnt == 4) { 
			unhook_OVATRH();
		} 
		else {
			unhook_ClientDenyHelper();
		}
	}
}

// vi: sw=4 ts=4 noet tw=80 cc=80
