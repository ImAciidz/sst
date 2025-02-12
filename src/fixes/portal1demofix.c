/*
 * Copyright © 2024 Michael Smith <mikesmiffy128@gmail.com>
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

#include <string.h>

#include "../errmsg.h"
#include "../feature.h"
#include "../gametype.h"
#include "../langext.h"
#include "../mem.h"
#include "../os.h"
#include "../ppmagic.h"
#include "../sst.h"

FEATURE() // nameless feature until the code is not specific to just 4104

char *patch;

static bool do_demo_fix(void) {
	// TODO(compat): this is an absolutely atrocious way to implement this. it
	// should only be temporary in the interests of getting 4104 working right
	// away. since other versions also have broken demos, a more general fix
	// should be done... eventually...
	void *EyeAngles = mem_offset(clientlib, 0x19D1B0); // in C_PortalPlayer
	static const char match[] =
			HEXBYTES(56, 8B, F1, E8, 48, 50, EA, FF, 84, C0, 74, 25);
	if (!memcmp(EyeAngles, match, sizeof(match))) {
		patch = mem_offset(EyeAngles, 39);
		if (patch[0] == 0x75 && patch[1] == 0x08) {
			if_hot (os_mprot(patch, 2, PAGE_EXECUTE_READWRITE)) {
				patch[0] = 0x90; patch[1] = 0x90; // replace je with nop
                return true;
			}
			else {
				errmsg_warnsys("unable to fix 4104 demo playback bug: "
						"couldn't make memory writable");
                return false;
			}
		} else {
            return false;
        }
	}
}

PREINIT {
    return GAMETYPE_MATCHES(Portal1);
}

INIT {
    if (!do_demo_fix()) return false;
    return true;
}

END {
    patch[0] = 0x75; patch[1] = 0x08;
}

// vi: sw=4 ts=4 noet tw=80 cc=80
