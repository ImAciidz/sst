/* THIS FILE SHOULD BE CALLED `con.c` BUT WINDOWS IS STUPID */
/*
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

#include <stddef.h> // should be implied by stdlib but glibc is dumb (offsetof)
#include <stdlib.h>
#include <stdio.h>

#include "abi.h"
#include "con_.h"
#include "engineapi.h" // for factories and rgba - XXX: is this a bit circular?
#include "extmalloc.h"
#include "gamedata.h"
#include "gametype.h"
#include "mem.h"
#include "os.h"
#include "vcall.h"
#include "version.h"
#include "x86.h"
#include "x86util.h"
#include "mem.h"

/******************************************************************************\
 * Have you ever noticed that when someone comments "here be dragons" there's *
 * no actual dragons? Turns out, that's because the dragons all migrated over *
 * here, so that they could build multiple inheritance vtables in C, by hand. *
 *                                                                            *
 * Don't get set on fire.                                                     *
\******************************************************************************/

static int dllid; // from AllocateDLLIdentifier(), lets us unregister in bulk
int con_cmdclient;

DECL_VFUNC(struct ICvar, void *, FindCommandBase_p2, 13, const char *)
DECL_VFUNC(struct ICvar, void *, FindCommand_nonp2, 14, const char *)
DECL_VFUNC(struct ICvar, void *, FindVar_nonp2, 12, const char *)
DECL_VFUNC(struct ICvar, void *, GetCommands_OE, 9)

DECL_VFUNC_DYN(struct ICvar, int, AllocateDLLIdentifier)
DECL_VFUNC_DYN(struct ICvar, void, RegisterConCommand, /*ConCommandBase*/ void *)
DECL_VFUNC_DYN(struct ICvar, void, UnregisterConCommands, int)
DECL_VFUNC_DYN(struct ICvar, struct con_var *, FindVar, const char *)
//DECL_VFUNC(struct ICvar, const struct con_var *, FindVar_const, 13, const char *)
DECL_VFUNC_DYN(struct ICvar, struct con_cmd *, FindCommand, const char *)
DECL_VFUNC_DYN(struct ICvar, void, CallGlobalChangeCallbacks, struct con_var *,
		const char *, float)
DECL_VFUNC(struct ICvar, void, CallGlobalChangeCallbacks_OE, 12, struct con_var *,
		const char *)
// sad: since adding the cool abstraction, we can't do varargs (because you
// can't pass varargs to other varargs of course). we only get a pointer to it
// via VFUNC so just declare the typedef here - I don't wanna write any more
// macros today.
typedef void (*ConsoleColorPrintf_func)(struct ICvar *, const struct rgba *,
		const char *, ...);
typedef void (*ConsoleColorPrintf_OE_func)(const struct rgba *,
		const char *, ...);

// these have to be extern for con_colourmsg(), due to varargs nonsense
struct ICvar *_con_iface;
ConsoleColorPrintf_func _con_colourmsgf;
ConsoleColorPrintf_OE_func _con_colourmsgoef;

struct con_var_common *getcommon(struct con_var *v) {
	if (GAMETYPE_MATCHES(OE)) {
		return &(v->v1);
	} else {
		return &(v->v2.common);
	}
}

static inline void initval(struct con_var *v) {
	v->v2.common.strval = extmalloc(v->v2.common.strlen); // note: strlen is preset in _DEF_CVAR()
	memcpy(v->v2.common.strval, v->v2.common.defaultval, v->v2.common.strlen);
}

// to try and match the engine even though it's probably not strictly required,
// we call the Internal* virtual functions via the actual vtable. since vtables
// are built dynamically (below), we store this index; other indices are just
// offset from it since these 3-or-4 functions are all right next to each other.
static int vtidx_InternalSetValue;
#define vtidx_InternalSetFloatValue (vtidx_InternalSetValue + 1)
#define vtidx_InternalSetIntValue (vtidx_InternalSetValue + 2)
#define vtidx_InternalSetColorValue (vtidx_InternalSetValue + 3)

static void VCALLCONV dtor(void *_) {} // we don't use constructors/destructors

static bool VCALLCONV IsCommand_cmd(void *this) { return true; }
static bool VCALLCONV IsCommand_var(void *this) { return false; }

static bool VCALLCONV IsFlagSet_cmd(struct con_cmd *this, int flags) {
	return !!(this->base.flags & flags);
}
static bool VCALLCONV IsFlagSet_var_OE(struct con_var *this, int flags) {
	return !!(this->v1.parent->base.flags & flags);
}
static bool VCALLCONV IsFlagSet_var(struct con_var *this, int flags) {
	return !!(this->v2.common.parent->base.flags & flags);
}
static void VCALLCONV AddFlags_cmd(struct con_cmd *this, int flags) {
	this->base.flags |= flags;
}
static void VCALLCONV AddFlags_var_OE(struct con_var *this, int flags) {
	this->v1.parent->base.flags |= flags;
}
static void VCALLCONV AddFlags_var(struct con_var *this, int flags) {
	this->v2.common.parent->base.flags |= flags;
}
static void VCALLCONV RemoveFlags_cmd(struct con_cmd *this, int flags) {
	this->base.flags &= ~flags;
}
// no OE counterpart for this (see con_init())
static void VCALLCONV RemoveFlags_var(struct con_var *this, int flags) {
	this->v2.common.parent->base.flags &= ~flags;
}
static int VCALLCONV GetFlags_cmd(struct con_cmd *this) {
	return this->base.flags;
}
// no OE counterpart for this (see con_init())
static int VCALLCONV GetFlags_var(struct con_var *this) {
	return this->v2.common.parent->base.flags;
}

static const char *VCALLCONV GetName_cmd(struct con_cmd *this) {
	return this->base.name;
}
static const char *VCALLCONV GetName_var_OE(struct con_var *this) {
	return this->v1.parent->base.name;
}
static const char *VCALLCONV GetName_var(struct con_var *this) {
	return this->v2.common.parent->base.name;
}
static const char *VCALLCONV GetHelpText_cmd(struct con_cmd *this) {
	return this->base.help;
}
static const char *VCALLCONV GetHelpText_var_OE(struct con_var *this) {
	return this->v1.parent->base.help;
}
static const char *VCALLCONV GetHelpText_var(struct con_var *this) {
	return this->v2.common.parent->base.help;
}
static bool VCALLCONV IsRegistered(struct con_cmdbase *this) {
	return this->registered;
}
static int VCALLCONV GetDLLIdentifier(struct con_cmdbase *this) {
	return dllid;
}
static void VCALLCONV Create_base(struct con_cmdbase *this, const char *name,
		const char *help, int flags) {} // nop, we static init already
static void VCALLCONV Init(struct con_cmdbase *this) {} // ""


static bool VCALLCONV ClampValue_OE(struct con_var *this, float *f) {
	if (this->v1.hasmin && this->v1.minval > *f) { *f = this->v1.minval; return true; }
	if (this->v1.hasmax && this->v1.maxval < *f) { *f = this->v1.maxval; return true; }
	return false;
}

static bool VCALLCONV ClampValue(struct con_var *this, float *f) {
	if (this->v2.common.hasmin && this->v2.common.minval > *f) { *f = this->v2.common.minval; return true; }
	if (this->v2.common.hasmax && this->v2.common.maxval < *f) { *f = this->v2.common.maxval; return true; }
	return false;
}

int VCALLCONV AutoCompleteSuggest(struct con_cmd *this, const char *partial,
		/*CUtlVector*/ void *commands) {
	// TODO(autocomplete): implement this if needed later
	return 0;
}
bool VCALLCONV CanAutoComplete(struct con_cmd *this) {
	return false;
}


int *cmd_argc = 0;
char *(*cmd_argv)[80];

void VCALLCONV Dispatch_OE(struct con_cmd *this) {
	struct con_cmdargs args;
	args.argc = *cmd_argc;
	for (int i = 0; i < 64; i++) {
		args.argv[i] = (*cmd_argv)[i];
	}
	struct con_cmdargs *argss = &args;
	this->cb(argss);
}

void VCALLCONV Dispatch(struct con_cmd *this, const struct con_cmdargs *args) {
	// only try cb; cbv1 and iface should never get used by us
	if (this->use_newcb && this->cb) this->cb(args);
}

static void VCALLCONV ChangeStringValue_OE(struct con_var *this, const char *s) {
	char *old = alloca(this->v1.strlen);
	memcpy(old, this->v1.strval, this->v1.strlen);
	int len = strlen(s) + 1;
	if (len > this->v1.strlen) {
		this->v1.strval = extrealloc(this->v1.strval, len);
		this->v1.strlen = len;
	}
	memcpy(this->v1.strval, s, len);
	// callbacks don't matter as far as ABI compat goes (and thank goodness
	// because e.g. portal2 randomly adds a *list* of callbacks!?). however we
	// do need callbacks for at least one feature, so do our own minimal thing
	if (this->v1.cb) this->v1.cb(this);
	// also call global callbacks, as is polite.
	CallGlobalChangeCallbacks_OE(_con_iface, this, old);
}

static void VCALLCONV ChangeStringValue(struct con_var *this, const char *s,
		float oldf) {
	char *old = alloca(this->v2.common.strlen);
	memcpy(old, this->v2.common.strval, this->v2.common.strlen);
	int len = strlen(s) + 1;
	if (len > this->v2.common.strlen) {
		this->v2.common.strval = extrealloc(this->v2.common.strval, len);
		this->v2.common.strlen = len;
	}
	memcpy(this->v2.common.strval, s, len);
	// callbacks don't matter as far as ABI compat goes (and thank goodness
	// because e.g. portal2 randomly adds a *list* of callbacks!?). however we
	// do need callbacks for at least one feature, so do our own minimal thing
	if (this->v2.common.cb) this->v2.common.cb(this);
	// also call global callbacks, as is polite.
	CallGlobalChangeCallbacks(_con_iface, this, old, oldf);
}

static void VCALLCONV InternalSetValue_impl_OE(struct con_var *this, const char *v) {
	float newf = atof(v);
	char tmp[32];
	// NOTE: calling our own ClampValue and ChangeString, not bothering with
	// vtable (it's internal anyway, so we're never calling into engine code)
	if (ClampValue(this, &newf)) {
		snprintf(tmp, sizeof(tmp), "%f", newf);
		v = tmp;
	}
	this->v1.fval = newf;
	this->v1.ival = (int)newf;
	if (!(this->base.flags & CON_NOPRINT)) ChangeStringValue_OE(this, v);
}

static void VCALLCONV InternalSetValue_impl(struct con_var *this, const char *v) {
	float oldf = this->v2.common.fval;
	float newf = atof(v);
	char tmp[32];
	// NOTE: calling our own ClampValue and ChangeString, not bothering with
	// vtable (it's internal anyway, so we're never calling into engine code)
	if (ClampValue(this, &newf)) {
		snprintf(tmp, sizeof(tmp), "%f", newf);
		v = tmp;
	}
	this->v2.common.fval = newf;
	this->v2.common.ival = (int)newf;
	if (!(this->base.flags & CON_NOPRINT)) ChangeStringValue(this, v, oldf);
}

static void VCALLCONV InternalSetFloatValue_impl_OE(struct con_var *this, float v) {
	if (v == this->v1.fval) return;
	ClampValue(this, &v);
	this->v1.fval = v; this->v1.ival = (int)this->v1.fval;
	if (!(this->base.flags & CON_NOPRINT)) {
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%f", this->v1.fval);
		ChangeStringValue_OE(this, tmp);
	}
}

static void VCALLCONV InternalSetFloatValue_impl(struct con_var *this, float v) {
	if (v == this->v2.common.fval) return;
	ClampValue(this, &v);
	float old = this->v2.common.fval;
	this->v2.common.fval = v; this->v2.common.ival = (int)this->v2.common.fval;
	if (!(this->base.flags & CON_NOPRINT)) {
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%f", this->v2.common.fval);
		ChangeStringValue(this, tmp, old);
	}
}

static void VCALLCONV InternalSetIntValue_impl_OE(struct con_var *this, int v) {
	if (v == this->v1.ival) return;
	float f = (float)v;
	if (ClampValue(this, &f)) v = (int)f;
	this->v1.fval = f; this->v1.ival = v;
	if (!(this->base.flags & CON_NOPRINT)) {
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%f", this->v1.fval);
		ChangeStringValue_OE(this, tmp);
	}
}

static void VCALLCONV InternalSetIntValue_impl(struct con_var *this, int v) {
	if (v == this->v2.common.ival) return;
	float f = (float)v;
	if (ClampValue(this, &f)) v = (int)f;
	float old = this->v2.common.fval;
	this->v2.common.fval = f; this->v2.common.ival = v;
	if (!(this->base.flags & CON_NOPRINT)) {
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "%f", this->v2.common.fval);
		ChangeStringValue(this, tmp, old);
	}
}

DECL_VFUNC_DYN(struct con_var, void, InternalSetValue, const char *)
DECL_VFUNC_DYN(struct con_var, void, InternalSetFloatValue, float)
DECL_VFUNC_DYN(struct con_var, void, InternalSetIntValue, int)
DECL_VFUNC_DYN(struct con_var, void, InternalSetColorValue, struct rgba)


static void VCALLCONV SetValue_str_thunk_OE(struct con_var *var, const char *v) {
	InternalSetValue(var->v1.parent, v);
}
static void VCALLCONV SetValue_f_thunk_OE(struct con_var *var, float v) {
	InternalSetFloatValue(var->v1.parent, v);
}
static void VCALLCONV SetValue_i_thunk_OE(struct con_var *var, int v) {
	InternalSetIntValue(var->v1.parent, v);
}

// IConVar calls get this-adjusted pointers, so just subtract the offset
static void VCALLCONV SetValue_str_thunk(void *thisoff, const char *v) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	InternalSetValue(this->v2.common.parent, v);
}
static void VCALLCONV SetValue_f_thunk(void *thisoff, float v) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	InternalSetFloatValue(this->v2.common.parent, v);
}
static void VCALLCONV SetValue_i_thunk(void *thisoff, int v) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	InternalSetIntValue(this->v2.common.parent, v);
}
static void VCALLCONV SetValue_colour_thunk(void *thisoff, struct rgba v) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	InternalSetColorValue(this->v2.common.parent, v);
}

// more misc thunks, hopefully these just compile to a lea and a jmp
static const char *VCALLCONV GetName_thunk(void *thisoff) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	return GetName_var(this);
}
static bool VCALLCONV IsFlagSet_thunk(void *thisoff, int flags) {
	struct con_var *this = mem_offset(thisoff,
			-offsetof(struct con_var, v2.vtable_iconvar));
	return IsFlagSet_var(this, flags);
}

// dunno what this is actually for...
static int VCALLCONV GetSplitScreenPlayerSlot(struct con_var *thisoff) {
	return 0;
}

// aand yet another Create nop
static void VCALLCONV Create_var(void *thisoff, const char *name,
		const char *defaultval, int flags, const char *helpstr, bool hasmin,
		float min, bool hasmax, float max, void *cb) {}

// the first few members of ConCommandBase are the same between versions
void *_con_vtab_cmd[14 + NVDTOR] = {
	(void *)&dtor,
#ifndef _WIN32
	(void *)&dtor,
#endif
	(void *)&IsCommand_cmd,
	(void *)&IsFlagSet_cmd,
	(void *)&AddFlags_cmd
};

// the engine does dynamic_casts on ConVar at some points so we have to fill out
// bare minimum rtti to prevent crashes. oh goody.
#ifdef _WIN32
DEF_MSVC_BASIC_RTTI(static, varrtti, _con_vtab_var, "sst_ConVar")
#else
DEF_ITANIUM_BASIC_RTTI(static, varrtti, "sst_ConVar")
#endif

struct _con_vtab_var_wrap _con_vtab_var_wrap = {
#ifndef _WIN32
	0, // this *is* the top, no offset needed :)
#endif
	&varrtti,
	(void *)&dtor,
#ifndef _WIN32
	(void *)&dtor,
#endif
	(void *)&IsCommand_var,
	// OE ruins everything
#ifndef _WIN32
	(void *)&IsFlagSet_var,
	(void *)&AddFlags_var
#endif
};

struct _con_vtab_iconvar_wrap _con_vtab_iconvar_wrap = {
#ifdef _WIN32
	0 // because of crazy overload vtable order we can't prefill *anything*
#else
	// RTTI members first on linux:
	-offsetof(struct con_var, vtable_iconvar),
	&varrtti,
	// colour is the last of the 4 on linux so we can at least prefill these 3
	(void *)&SetValue_str_thunk,
	(void *)&SetValue_f_thunk,
	(void *)&SetValue_i_thunk
#endif
};

void con_regvar(struct con_var *v) {
	initval(v);
	if (GAMETYPE_MATCHES(OE)) {
		v->v1 = v->v2.common;
	}
	RegisterConCommand(_con_iface, v);
}

void con_regcmd(struct con_cmd *c) {
	RegisterConCommand(_con_iface, c);
}


DECL_VFUNC(struct VEngineClient, void, Cmd_Argv, 32)

static bool find_argcandargv(void) {
	void *boaner = VFUNC(engclient, Cmd_Argv);
	const uchar *insns = (const uchar *)boaner;
	for (const uchar *p = insns; p - insns < 32;) {
		if (p[0] == X86_CALL) {
			insns = p + 5 + mem_loads32(p + 1);
			for (const uchar *p = insns; p - insns < 32;) {
				if (p[0] == 0x3B && p[1] == 0x05) {
					cmd_argc = mem_loadptr(p + 2);
				}
				if (p[0] == 0x8b && p[1] == 0x04 && p[2] == 0x85) {
					cmd_argv = mem_loadptr(p + 3);
					if (cmd_argc) {	return true; }
				}
				NEXT_INSN(p, "cmd_argv, cmd_argc");
			}
			return false;
		}
		NEXT_INSN(p, "::Cmd_Argv");
	}
	return false;
}

__declspec(dllimport) void *GetSpewOutputFunc(void);

//void *Con_ColorPrintf = 0;
static void *find_Con_ColorPrintf(void) {
	void *spew = GetSpewOutputFunc();
	const uchar *insns = (const uchar *)spew;
	for (const uchar *p = insns; p - insns < 320;) {
		if (p[0] == 0x51 && p[1] == 0x68 && p[6] == X86_CALL && p[11] == 0x83 &&
				p[12] == 0xC4 ) {
			con_msg("success!!!!!!!!!\n");
			con_warn("BIG GIGANTIC PENIS\n");
			return (void *)(p + 11 + mem_loads32(p + 7));
		}
		NEXT_INSN(p, "Con_ColorPrintf");
	}
	return 0;
}

int *host_initialized;

static void *find_host_initialized(void *Con_ColorPrintf) {
	const uchar *insns = (const uchar*)Con_ColorPrintf;
	for (const uchar *p = insns; p - insns < 16;) {
		if (p[0] == 0x80 && p[1] == 0x3D) {
			con_msg("host_initialized %p\n", (void*)mem_loads32(p + 2));
			return (void*)mem_loads32(p + 2);
		}
		NEXT_INSN(p, "host_initialized");
	}
	return 0;
}

// XXX: these should use vcall/gamedata stuff as they're only used for the
// setter API after everything is brought up. however that will require some
// kind of windows/linux conditionals in the gamedata system! this solution is
// just hacked in for now to get things working because it was broken before...
#ifdef _WIN32
static int vtidx_SetValue_str = 2, vtidx_SetValue_f = 1, vtidx_SetValue_i = 0;
#else
enum { vtidx_SetValue_str = 0, vtidx_SetValue_f = 1, vtidx_SetValue_i = 2 };
#endif

void con_init() {
	if (GAMETYPE_MATCHES(OE)) {
		find_argcandargv();
		_con_colourmsgoef = (ConsoleColorPrintf_OE_func)find_Con_ColorPrintf();
		con_msg("_con_colourmsgoef %p\n", _con_colourmsgoef);
		host_initialized = find_host_initialized((void *)_con_colourmsgoef);
		if (host_initialized) {
			if (*host_initialized == 0) {
				*host_initialized = 1;
			}
		}
	} else {
		_con_colourmsgf = VFUNC(_con_iface, ConsoleColorPrintf);
		dllid = AllocateDLLIdentifier(_con_iface);
	}

	void **pc = _con_vtab_cmd + 3 + NVDTOR, **pv = _con_vtab_var + 3 + NVDTOR,
			**pi = _con_vtab_iconvar
#ifndef _WIN32
				+ 3
#endif
			;
#ifdef _WIN32
	pv = _con_vtab_var + 1 + NVDTOR;
	if (GAMETYPE_MATCHES(OE)) {
		*pv++ = (void *)&IsFlagSet_var_OE;
		*pv++ = (void *)&AddFlags_var_OE;
	} else {
		*pv++ = (void *)&IsFlagSet_var;
		*pv++ = (void *)&AddFlags_var;
	}
#endif
	if (GAMETYPE_MATCHES(L4Dbased)) { // 007 base
		*pc++ = (void *)&RemoveFlags_cmd;
		*pc++ = (void *)&GetFlags_cmd;
		*pv++ = (void *)&RemoveFlags_var;
		*pv++ = (void *)&GetFlags_var;
	}
	// base stuff in cmd
	*pc++ = (void *)&GetName_cmd;
	*pc++ = (void *)&GetHelpText_cmd;
	*pc++ = (void *)&IsRegistered;
	if (!GAMETYPE_MATCHES(OE)) {
		*pc++ = (void *)&GetDLLIdentifier;
	}
	*pc++ = (void *)&Create_base;
	*pc++ = (void *)&Init;
	// cmd-specific
	*pc++ = (void *)&AutoCompleteSuggest;
	*pc++ = (void *)&CanAutoComplete;
	if (GAMETYPE_MATCHES(OE)) {
		*pc++ = (void *)&Dispatch_OE;
	} else {
		*pc++ = (void *)&Dispatch;
	}
	// base stuff in var
	if (GAMETYPE_MATCHES(OE)) {
		*pv++ = (void *)&GetName_var_OE;
		*pv++ = (void *)&GetHelpText_var_OE;
	} else {
		*pv++ = (void *)&GetName_var;
		*pv++ = (void *)&GetHelpText_var;
	}
	*pv++ = (void *)&IsRegistered;
	if (!GAMETYPE_MATCHES(OE)) {
		*pv++ = (void *)&GetDLLIdentifier;
	}
	*pv++ = (void *)&Create_base;
	*pv++ = (void *)&Init;
	// var-specific
	if (GAMETYPE_MATCHES(OE)) {
		*pv++ = (void *)&SetValue_i_thunk_OE;
		*pv++ = (void *)&SetValue_f_thunk_OE;
		*pv++ = (void *)&SetValue_str_thunk_OE;
		vtidx_InternalSetValue = pv - _con_vtab_var;
		vtidx_SetValue_f = vtidx_InternalSetValue - 2;
		vtidx_SetValue_i = vtidx_InternalSetValue - 3;
		vtidx_SetValue_str = vtidx_InternalSetValue - 1;
		*pv++ = (void *)&InternalSetValue_impl_OE;
		*pv++ = (void *)&InternalSetFloatValue_impl_OE;
		*pv++ = (void *)&InternalSetIntValue_impl_OE;
	} else {
		vtidx_InternalSetValue = pv - _con_vtab_var;
		*pv++ = (void *)&InternalSetValue_impl;
		*pv++ = (void *)&InternalSetFloatValue_impl;
		*pv++ = (void *)&InternalSetIntValue_impl;
	}
	if (GAMETYPE_MATCHES(L4D2x) || GAMETYPE_MATCHES(Portal2)) { // ugh, annoying
		// InternalSetColorValue, literally the same machine instructions as int
		*pv++ = (void *)&InternalSetIntValue_impl;
	}
	if (GAMETYPE_MATCHES(OE)) {
		*pv++ = (void *)&ClampValue_OE;
		*pv++ = (void *)&ChangeStringValue_OE;
	} else {
		*pv++ = (void *)&ClampValue;
		*pv++ = (void *)&ChangeStringValue;
	}
	*pv++ = (void *)&Create_var;
	if (GAMETYPE_MATCHES(L4D2x) || GAMETYPE_MATCHES(Portal2)) {
		*pi++ = (void *)&SetValue_colour_thunk;
#ifdef _WIN32
		// stupid hack for above mentioned crazy overload ordering
		++vtidx_SetValue_str;
		++vtidx_SetValue_i;
		++vtidx_SetValue_f;
#endif
	}
#ifdef _WIN32
	// see above: these aren't prefilled due the the reverse order
	*pi++ = (void *)&SetValue_i_thunk;
	*pi++ = (void *)&SetValue_f_thunk;
	*pi++ = (void *)&SetValue_str_thunk;
#endif
	*pi++ = (void *)&GetName_thunk;
	// GetBaseName (we just return actual name in all cases)
	if (GAMETYPE_MATCHES(L4Dbased)) *pi++ = (void *)&GetName_thunk;
	*pi++ = (void *)&IsFlagSet_thunk;
	// last one: not in 004, but doesn't matter. one less branch!
	*pi++ = (void *)&GetSplitScreenPlayerSlot;
}

static void helpuserhelpus(int pluginver, char ifaceverchar) {
	con_msg("\n");
	con_msg("-- Please include ALL of the following if asking for help:\n");
	con_msg("--   plugin:     " LONGNAME " v" VERSION "\n");
	con_msg("--   interfaces: %d/%c\n", pluginver, ifaceverchar);
	con_msg("\n");
}

// note: for now at least, not using errmsg_*() macros here because it doesn't
// really make sense for these messages to be coming from "con"

static void warnoe() {
	con_warn("sst: error: this engine version is not yet supported\n");
}

bool con_detect(int pluginver) {
	if (_con_iface = factory_engine("VEngineCvar007", 0)) {
		// GENIUS HACK (BUT STILL BAD): Portal 2 has everything in ICvar shifted
		// down 3 places due to the extra stuff in IAppSystem. This means that
		// if we look up the Portal 2-specific cvar using FindCommandBase, it
		// *actually* calls the const-overloaded FindVar on other branches,
		// which just happens to still work fine. From there, we can figure out
		// the actual ABI to use to avoid spectacular crashes.
		if (FindCommandBase_p2(_con_iface, "portal2_square_portals")) {
			_gametype_tag |= _gametype_tag_Portal2;
			return true;
		}
		if (FindCommand_nonp2(_con_iface, "l4d2_snd_adrenaline")) {
			// while we're here, also distinguish Survivors, the stupid Japanese
			// arcade game a few people seem to care about for some reason
			// (which for some other reason also has some vtable changes)
			if (FindVar_nonp2(_con_iface, "avatarbasemodel")) {
				_gametype_tag |= _gametype_tag_L4DS;
			}
			else {
				_gametype_tag |= _gametype_tag_L4D2;
			}
			return true;
		}
		if (FindVar_nonp2(_con_iface, "z_difficulty")) {
			_gametype_tag |= _gametype_tag_L4D1;
			return true;
		}
		con_warn("sst: error: game is unsupported (using VEngineCvar007)\n");
		helpuserhelpus(pluginver, '7');
		return false;
	}
	if (_con_iface = factory_engine("VEngineCvar004", 0)) {
		// TODO(compat): are there any cases where 004 is incompatible? could
		// this crash? find out!
		if (pluginver == 3) _gametype_tag |= _gametype_tag_2013;
		else _gametype_tag |= _gametype_tag_OrangeBox;
		return true;
	}
	if (_con_iface = factory_engine("VEngineCvar003", 0)) {
		_gametype_tag |= _gametype_tag_OE;
		//void *penis = VFUNC(_con_iface, GetCommands_OE);
		//void *penis = GetCommands_OE(_con_iface);
		//warnoe();
		//helpuserhelpus(pluginver, '3');
		return true;
	}
	// I don't suppose there's anything below 002 worth caring about? Shrug.
	// (are there any games that even use 002? only seems like 001 released)
	if (factory_engine("VEngineCvar002", 0)) {
		warnoe();
		helpuserhelpus(pluginver, '2');
		return false;
	}
	con_warn("sst: error: couldn't find a supported console interface\n");
	helpuserhelpus(pluginver, '?');
	return false;
}

void con_disconnect() {
	if (GAMETYPE_MATCHES(OE)) {
		const uchar *temp = (const uchar *)VFUNC(_con_iface, GetCommands_OE);
		temp = temp + 5 + mem_loads32(temp + 1);
		struct con_cmdbase **shit = mem_loadptr(temp + 1);
		while (*shit) {
			if ((*shit)->vtable == _con_vtab_cmd || (*shit)->vtable == _con_vtab_var) {
				struct con_cmdbase *next = (*shit)->next;
				(*shit)->next = 0;
				*shit = next;
			} else {
				shit = &(*shit)->next;
			}
		}
	} else {
		UnregisterConCommands(_con_iface, dllid);
	}
}

struct con_var *con_findvar(const char *name) {
	return FindVar(_con_iface, name);
}

struct con_cmd *con_findcmd(const char *name) {
	if (!GAMETYPE_MATCHES(OE)) return FindCommand(_con_iface, name);
	struct con_cmdbase *cmd = VCALL(_con_iface, GetCommands_OE);
	for ( ; cmd; cmd = cmd->next) {
		if ( !_stricmp(name, cmd->name)) {
			return (struct con_cmd *)cmd;
		}
	}
	return 0;
}

#define GETTER(T, N, M) T N(const struct con_var *v) { \
	if (GAMETYPE_MATCHES(OE)) { return v->v1.parent->v1.M; } \
	return v->v2.common.parent->v2.common.M; \
}
GETTER(const char *, con_getvarstr, strval)
GETTER(float, con_getvarf, fval)
GETTER(int, con_getvari, ival)
#undef GETTER

// XXX: move this to vcall/gamedata (will require win/linux conditionals first!)
// see also above comment on the vtidx definitions
#define SETTER(T, I, N) \
	void N(struct con_var *v, T x) { \
		if (GAMETYPE_MATCHES(OE)) { \
			((void (*VCALLCONV)(void *, T))(v->base.vtable[I]))( \
				&v->base.vtable, x); \
		} else { \
			((void (*VCALLCONV)(void *, T))(v->v2.vtable_iconvar[I]))( \
				&v->v2.vtable_iconvar, x); \
		} \
	}
SETTER(const char *, vtidx_SetValue_str, con_setvarstr)
SETTER(float, vtidx_SetValue_f, con_setvarf)
SETTER(int, vtidx_SetValue_i, con_setvari)
#undef SETTER

con_cmdcb con_getcmdcb(const struct con_cmd *cmd) {
	return cmd->cb;//!cmd->use_newcmdiface && cmd->use_newcb ? cmd->cb : 0;
}

con_cmdcbv1 con_getcmdcbv1(const struct con_cmd *cmd) {
	return cmd->cb_v1;//!cmd->use_newcmdiface && !cmd->use_newcb ? cmd->cb_v1 : 0;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
