/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2015 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include "openconnect-internal.h"

#define GET_BITS(bits)							\
do {									\
	/* Strictly speaking, this check ought to be on			\
	 * (srclen < 1 + (bits_left < bits)). However, when bits == 9	\
	 * the (bits_left < bits) comparison is always true so it	\
	 * always comes out as (srclen < 2).				\
	 * And bits is only anything *other* than 9 when we're reading	\
	 * reading part of a match encoding. And in that case, there	\
	 * damn well ought to be an end marker (7 more bits) after	\
	 * what we're reading now, so it's perfectly OK to use		\
	 * (srclen < 2) in that case too. And a *lot* cheaper. */	\
	if (srclen < 2)							\
		return -EINVAL;						\
	/* Explicit comparison with 8 to optimise it into a tautology	\
	 * in the the bits == 9 case, because the compiler doesn't	\
	 * know that bits_left can never be larger than 8. */		\
	if (bits >= 8 || bits >= bits_left) {				\
		/* We need *all* the bits that are left in the current	\
		 * byte. Take them and bump the input pointer. */	\
		data = (src[0] << (bits - bits_left)) & ((1 << bits) - 1); \
		src++;							\
		srclen--;						\
		bits_left += 8 - bits;					\
		if (bits > 8 || bits_left < 8) {			\
			/* We need bits from the next byte too... */	\
			data |= src[0] >> bits_left;			\
			/* ...if we used *all* of them then (which can	\
			 * only happen if bits > 8), then bump the	\
			 * input pointer again so we never leave	\
			 * bits_left == 0. */				\
			if (bits > 8 && !bits_left) {			\
				bits_left = 8;				\
				src++;					\
				srclen--;				\
			}						\
		}							\
	} else {							\
		/* We need fewer bits than are left in the current byte */ \
		data = (src[0] >> (bits_left - bits)) & ((1ULL << bits) - 1); \
		bits_left -= bits;					\
	}								\
} while (0)

int lzs_decompress(unsigned char *dst, int dstlen, const unsigned char *src, int srclen)
{
	int outlen = 0;
	int bits_left = 8; /* Bits left in the current byte at *src */
	uint32_t data;
	uint16_t offset, length;

	while (1) {
		/* Get 9 bits, which is the minimum and a common case */
		GET_BITS(9);

		/* 0bbbbbbbb is a literal byte. The loop gives a hint to
		 * the compiler that we expect to see a few of these. */
		while (data < 0x100) {
			if (outlen == dstlen)
				return -EFBIG;
			dst[outlen++] = data;
			GET_BITS(9);
		}

		/* 110000000 is the end marker */
		if (data == 0x180)
			return outlen;

		/* 11bbbbbbb is a 7-bit offset */
		offset = data & 0x7f;

		/* 10bbbbbbbbbbb is an 11-bit offset, so get the next 4 bits */
		if (data < 0x180) {
			GET_BITS(4);

			offset <<= 4;
			offset |= data;
		}

		/* This is a compressed sequence; now get the length */
		GET_BITS(2);
		if (data != 3) {
			/* 00, 01, 10 ==> 2, 3, 4 */
			length = data + 2;
		} else {
			GET_BITS(2);
			if (data != 3) {
				/* 1100, 1101, 1110 => 5, 6, 7 */
				length = data + 5;
			} else {
				/* For each 1111 prefix add 15 to the length. Then add
				   the value of final nybble. */
				length = 8;

				while (1) {
					GET_BITS(4);
					if (data != 15) {
						length += data;
						break;
					}
					length += 15;
				}
			}
		}
		if (offset > outlen)
			return -EINVAL;
		if (length + outlen > dstlen)
			return -EFBIG;

		while (length) {
			dst[outlen] = dst[outlen - offset];
			outlen++;
			length--;
		}
	}
	return -EINVAL;
}

#define PUT_BITS(nr, bits)					\
do {								\
	outbits <<= (nr);					\
	outbits |= (bits);					\
	nr_outbits += (nr);					\
	if ((nr) > 8) {						\
		nr_outbits -= 8;				\
		if (outpos == dstlen)				\
			return -EFBIG;				\
		dst[outpos++] = outbits >> nr_outbits;		\
	}							\
	if (nr_outbits >= 8) {					\
		nr_outbits -= 8;				\
		if (outpos == dstlen)				\
			return -EFBIG;				\
		dst[outpos++] = outbits >> nr_outbits;		\
	}							\
} while (0)

/*
 * Much of the compression algorithm used here is based very loosely on ideas
 * from isdn_lzscomp.c by Andre Beck: http://micky.ibh.de/~beck/stuff/lzs4i4l/
 */
int lzs_compress(unsigned char *dst, int dstlen, const unsigned char *src, int srclen)
{
	int length, offset;
	int inpos = 0, outpos = 0;
	uint16_t longest_match_len;
	uint16_t hofs, longest_match_ofs;
	uint16_t hash;
	uint32_t outbits = 0;
	int nr_outbits = 0;

	/*
	 * This is theoretically a hash. But RAM is cheap and just loading the
	 * 16-bit value and using it as a hash is *much* faster.
	 */
#define HASH_BITS 16
#define HASH_TABLE_SIZE (1ULL << HASH_BITS)
#define HASH(p) (((struct oc_packed_uint16_t *)(p))->d)

	/*
	 * There are two data structures for tracking the history. The first
	 * is the true hash table, an array indexed by the hash value described
	 * above. It yields the offset in the input buffer at which the given
	 * hash was most recently seen. We use INVALID_OFS (0xffff) for none
	 * since we know IP packets are limited to 64KiB and we can never be
	 * *starting* a match at the penultimate byte of the packet.
	 */
#define INVALID_OFS 0xffff
	uint16_t hash_table[HASH_TABLE_SIZE]; /* Buffer offset for first match */

	/*
	 * The second data structure allows us to find the previous occurrences
	 * of the same hash value. It is a ring buffer containing links only for
	 * the latest MAX_HISTORY bytes of the input. The lookup for a given
	 * offset will yield the previous offset at which the same data hash
	 * value was found.
	 */
#define MAX_HISTORY (1<<11) /* Highest offset LZS can represent is 11 bits */
	uint16_t hash_chain[MAX_HISTORY];

	/* Just in case anyone tries to use this in a more general-purpose
	 * scenario... */
	if (srclen > INVALID_OFS + 1)
		return -EFBIG;

	/* No need to initialise hash_chain since we can only ever follow
	 * links to it that have already been initialised. */
	memset(hash_table, 0xff, sizeof(hash_table));

	while (inpos < srclen - 2) {
		hash = HASH(src + inpos);
		hofs = hash_table[hash];

		hash_chain[inpos & (MAX_HISTORY - 1)] = hofs;
		hash_table[hash] = inpos;

		if (hofs == INVALID_OFS || hofs + MAX_HISTORY <= inpos) {
			PUT_BITS(9, src[inpos]);
			inpos++;
			continue;
		}

		/* Since the hash is 16-bits, we *know* the first two bytes match */
		longest_match_len = 2;
		longest_match_ofs = hofs;

		for (; hofs != INVALID_OFS && hofs + MAX_HISTORY > inpos;
		     hofs = hash_chain[hofs & (MAX_HISTORY - 1)]) {

			/* We only get here if longest_match_len is >= 2. We need to find
			   a match of longest_match_len + 1 for it to be interesting. */
			if (!memcmp(src + hofs + 2, src + inpos + 2, longest_match_len - 1)) {
				longest_match_ofs = hofs;

				do {
					longest_match_len++;

					/* If we cannot *have* a longer match because we're at the
					 * end of the input, stop looking */
					if (longest_match_len + inpos == srclen)
						goto got_match;

				} while (src[longest_match_len + inpos] == src[longest_match_len + hofs]);
			}

			/* Typical compressor tuning would have a break out of the loop
			   here depending on the number of potential match locations we've
			   tried, or a value of longest_match_len that's considered "good
			   enough" so we stop looking for something better. We could also
			   do a hybrid where we count the total bytes compared, so 5
			   attempts to find a match better than 10 bytes is worth the same
			   as 10 attempts to find a match better than 5 bytes. Or
			   something. Anyway, we currently don't give up until we run out
			   of reachable history — maximal compression. */
		}
	got_match:
		/* Output offset, as 7-bit or 11-bit as appropriate */
		offset = inpos - longest_match_ofs;
		length = longest_match_len;

		if (offset < 0x80)
			PUT_BITS(9, 0x180 | offset);
		else
			PUT_BITS(13, 0x1000 | offset);

		/* Output length */
		if (length < 5)
			PUT_BITS(2, length - 2);
		else if (length < 8)
			PUT_BITS(4, length + 7);
		else {
			length += 7;
			while (length >= 30) {
				PUT_BITS(8, 0xff);
				length -= 30;
			}
			if (length >= 15)
				PUT_BITS(8, 0xf0 + length - 15);
			else
				PUT_BITS(4, length);
		}

		/* If we're already done, don't bother updating the hash tables. */
		if (inpos + longest_match_len >= srclen - 2) {
			inpos += longest_match_len;
			break;
		}

		/* We already added the first byte to the hash tables. Add the rest. */
		inpos++;
		while (--longest_match_len) {
			hash = HASH(src + inpos);
			hash_chain[inpos & (MAX_HISTORY - 1)] = hash_table[hash];
			hash_table[hash] = inpos++;
		}
	}

	/* Special cases at the end */
	if (inpos == srclen - 2) {
		hash = HASH(src + inpos);
		hofs = hash_table[hash];

		if (hofs != INVALID_OFS && hofs + MAX_HISTORY > inpos) {
			offset = inpos - hofs;

			if (offset < 0x80)
				PUT_BITS(9, 0x180 | offset);
			else
				PUT_BITS(13, 0x1000 | offset);

			/* The length is 2 bytes */
			PUT_BITS(2, 0);
		} else {
			PUT_BITS(9, src[inpos]);
			PUT_BITS(9, src[inpos + 1]);
		}
	} else if (inpos == srclen - 1) {
		PUT_BITS(9, src[inpos]);
	}

	/* End marker, with 7 trailing zero bits to ensure that it's flushed. */
	PUT_BITS(16, 0xc000);

	return outpos;
}
