/*
 * Copyright © 2025 Hayden K <imaciidz@gmail.com>
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

#include "../con_.h"
#include "../engineapi.h"
#include "../errmsg.h"
#include "../feature.h"
#include "../gamedata.h"
#include "../gametype.h"
#include "../intdefs.h"
#include "../langext.h"
#include "../os.h"
#include "../x86.h"
#include "../x86util.h"

FEATURE("chat rate limit removal")

uchar *patch;

// Method from SAR. Basically, game only lets you chat every 0.66 seconds.
// So, instead of adding 0.66 to the current time, we subtract it, and that
// means we can always chat. lol.

// Find the add instruction
static inline bool find_chat_rate_limit_insn(struct con_cmd *cmd_say) {
	uchar *insns = (uchar *)cmd_say->cb;
	for (uchar *p = insns; p - insns < 128;) {
		// find FADD
		if (p[0] == X86_FLTBLK5 && p[1] == X86_MODRM(0, 0, 5)) {
			patch = p + 1;
			return true;
		}
		// Portal 2, L4D2 2125-2134, L4D:S all use SSE2, so try finding ADDSD
		if (p[0] == X86_PFX_REPN && p[1] == X86_2BYTE & p[2] == X86_2B_ADD) {
			patch = p + 2;
			return true;
		}
		NEXT_INSN(p, "chat rate limit");
	}
	return false;
}

// if FADD replace with FSUB; otherwise it is ADDSD, replace that with SUBSD 
static inline bool patch_chat_rate_limit(void) {
	if (!os_mprot(patch, 1, PAGE_EXECUTE_READWRITE)) {
		errmsg_errorsys("failed to patch chat rate limit: couldn't make memory "
			"writable");
		return false;
	}
	if (*patch == X86_MODRM(0, 0, 5)) {
		*patch = X86_MODRM(0, 4, 5);
	} else {
		*patch = X86_2B_SUB;
	}
	return true;
}

// same logic as above but in reverse
static inline void unpatch_chat_rate_limit(void) {
	if (*patch == X86_MODRM(0, 4, 5)) {
		*patch = X86_MODRM(0, 0, 5);
	} else {
		*patch = X86_2B_ADD;
	}
}

PREINIT {
	// Works in L4D1/2, L4D:S, and Portal 2.
	return GAMETYPE_MATCHES(L4Dbased);
}

INIT {
	struct con_cmd *cmd_say = con_findcmd("say");
	if_cold (!cmd_say) return false;
	if (!find_chat_rate_limit_insn(cmd_say)) {
		errmsg_errorx("couldn't find chat rate limit instruction");
		return false;
	}
	if (!patch_chat_rate_limit()) {
		errmsg_errorx("couldn't patch chat rate limit");
		return false;
	}
	return true;
}

END {
	unpatch_chat_rate_limit();
}
