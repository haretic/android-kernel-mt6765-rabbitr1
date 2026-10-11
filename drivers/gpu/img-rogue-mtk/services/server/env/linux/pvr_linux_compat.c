// SPDX-License-Identifier: GPL-2.0
/*
 * Helpers for kernel APIs the DDK uses that mainline has removed.
 */

#include <linux/minmax.h>
#include <linux/string.h>

size_t pvr_strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);

	if (size) {
		size_t n = min(len, size - 1);

		memcpy(dst, src, n);
		dst[n] = '\0';
	}

	return len;
}
