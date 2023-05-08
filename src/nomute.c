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
#include "errmsg.h"
#include "feature.h"
#include "hook.h"
#include "intdefs.h"
#include "mem.h"
#include "os.h"
#include "x86.h"
#include "x86util.h"

// system headers at the end to avoid windows def headaches that os.h solves
#include <mmeapi.h>
#include <dsound.h>

FEATURE("inactive window audio adjustment")

DEF_CVAR_UNREG(snd_mute_losefocus,
		"Keep audio while tabbed out (SST reimplementation)", 1,
		CON_ARCHIVE | CON_HIDDEN)

// IDirectSound Windows API interface
static IDirectSoundVtbl *ds_vt = 0;
static typeof(ds_vt->CreateSoundBuffer) orig_CreateSoundBuffer;
static con_cmdcbv1 snd_restart_cb = 0;

static HRESULT __stdcall hook_CreateSoundBuffer(IDirectSound *this,
		LPCDSBUFFERDESC constdesc, LPDIRECTSOUNDBUFFER *buff, LPUNKNOWN unk) {
	LPDSBUFFERDESC desc = (LPDSBUFFERDESC) constdesc; // const casted away
	int newflag = DSBCAPS_GLOBALFOCUS * !con_getvari(snd_mute_losefocus);
	desc->dwFlags |= newflag;
	return orig_CreateSoundBuffer(this, desc, buff, unk);
}

static void snd_mute_losefocus_cb(struct con_var *v) {
	snd_restart_cb();
}

PREINIT {
	if (con_findvar("snd_mute_losefocus")) return false;
	con_reg(snd_mute_losefocus);
	return true;
}

INIT {
	IDirectSound *ds_obj = 0;
	if (DirectSoundCreate(NULL, &ds_obj, NULL) != DS_OK) {
		errmsg_errorx("couldn't create IDirectSound instance");
		return false;
	}

	ds_vt = ds_obj->lpVtbl;
	if (!os_mprot(&ds_vt->CreateSoundBuffer, sizeof(void *), PAGE_READWRITE)) {
		errmsg_errorx("couldn't make virtual table writable");
		return false;
	}

	ds_obj->lpVtbl->Release(ds_obj);

	orig_CreateSoundBuffer = ds_vt->CreateSoundBuffer;
	ds_vt->CreateSoundBuffer = &hook_CreateSoundBuffer;
	snd_mute_losefocus->base.flags &= ~CON_HIDDEN;

	struct con_cmd *snd_restart = con_findcmd("snd_restart");
	if (snd_restart) {
		snd_restart_cb = con_getcmdcbv1(snd_restart);
		snd_mute_losefocus->cb = &snd_mute_losefocus_cb;
	}
	else {
		errmsg_warnx("couldn't find snd_restart");
		errmsg_note("changing snd_mute_losefocus will have no effect until the"
				" game/sound system is restarted");
	}

	return true;
}

END {
	ds_vt->CreateSoundBuffer = orig_CreateSoundBuffer;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
