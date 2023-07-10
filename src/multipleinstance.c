/*
 * Copyright © 2023 Hayden K <imaciidz@gmail.com>
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

#include <Windows.h>
#include "con_.h"

DEF_CCMD_HERE(sst_allow_multiple_instances, "Releases \"hl2_singleton_mutex\" to enable running multiple instances.", 0)
{
	HANDLE handle = OpenMutexA(SYNCHRONIZE, false, "hl2_singleton_mutex");
	if (handle) {
		if (ReleaseMutex(handle))
			con_msg("Released hl2_singleton_mutex. You can start another instance now.\n");
		else
			con_warn("** sst: Failed to release hl2_singleton_mutex. **");
		CloseHandle(handle);
	}
	else
		con_warn("** sst: Failed to obtain hl2_singleton_mutex handle. **");
}
