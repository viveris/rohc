/*
 * Copyright 2010,2011,2012,2013,2016 Didier Barvaux
 * Copyright 2007,2009,2010,2012,2013,2014 Viveris Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file   schemes/comp_wlsb.c
 * @brief  Window-based Least Significant Bits (W-LSB) encoding
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 * @author David Moreau from TAS
 */

#include "comp_wlsb.h"
#include "interval.h" /* for the rohc_f_*bits() functions */

#include <string.h>
#include <assert.h>


/*
 * Private function prototypes:
 */

static size_t wlsb_get_next_older(const size_t entry, const size_t max)
	__attribute__((warn_unused_result, const));

static size_t wlsb_ack_remove(struct c_wlsb *const wlsb, const size_t pos)
	__attribute__((warn_unused_result, nonnull(1)));


/*
 * Public functions
 */

/**
 * @brief Create a new Window-based Least Significant Bits (W-LSB) encoding
 *        object
 *
 * @param[in,out] wlsb The W-LSB encoding object to create
 * @param bits         The maximal number of bits for representing a value
 * @param window_width The number of entries in the window (power of 2)
 * @return             true if the W-LSB encoding object was created,
 *                     false if it was not
 */
bool wlsb_new(struct c_wlsb *const wlsb,
              const size_t bits,
              const size_t window_width)
{
	size_t i;

	assert(bits > 0);
	assert(window_width > 0);
	assert(window_width <= ROHC_WLSB_WIDTH_MAX);

	wlsb->window = malloc(sizeof(struct c_window) * window_width);
	if(wlsb->window == NULL)
	{
		goto error;
	}

	wlsb->oldest = 0;
	wlsb->next = 0;
	wlsb->count = 0;
	wlsb->window_width = window_width;
	wlsb->bits = bits;

	for(i = 0; i < wlsb->window_width; i++)
	{
		wlsb->window[i].used = false;
	}

	return true;

error:
	return false;
}


/**
 * @brief Create a new Window-based Least Significant Bits (W-LSB) encoding
 *        object from another
 *
 * @param[in,out] dst  The W-LSB encoding object to create
 * @param src          The W-LSB encoding object to copy
 * @return             true if the W-LSB encoding object was created,
 *                     false if it was not
 */
bool wlsb_copy(struct c_wlsb *const dst,
               const struct c_wlsb *const src)
{
	const size_t window_mem_size = sizeof(struct c_window) * dst->window_width;

	dst->oldest = src->oldest;
	dst->next = src->next;
	dst->count = src->count;
	dst->window_width = src->window_width;
	dst->bits = src->bits;

	dst->window = malloc(window_mem_size);
	if(dst->window == NULL)
	{
		goto error;
	}
	memcpy(dst->window, src->window, window_mem_size);

	return true;

error:
	return false;
}


/**
 * @brief Destroy a Window-based LSB (W-LSB) encoding object
 *
 * @param wlsb  The W-LSB object to destroy
 */
void wlsb_free(struct c_wlsb *const wlsb)
{
	free(wlsb->window);
}


/**
 * @brief Add a value into a W-LSB encoding object
 *
 * @param wlsb  The W-LSB object
 * @param sn    The Sequence Number (SN) for the new entry
 * @param value The value to base the LSB coding on
 */
void c_add_wlsb(struct c_wlsb *const wlsb,
                const uint32_t sn,
                const uint32_t value)
{
	/* if window is full, an entry is overwritten */
	if(wlsb->count == wlsb->window_width)
	{
		wlsb->oldest = (wlsb->oldest + 1) % wlsb->window_width;
	}
	else
	{
		wlsb->count++;
	}

	wlsb->window[wlsb->next].used = true;
	wlsb->window[wlsb->next].sn = sn;
	wlsb->window[wlsb->next].value = value;
	wlsb->next = (wlsb->next + 1) % wlsb->window_width;
}


/**
 * @brief Find out whether the given number of bits is enough to encode value
 *
 * The function is dedicated to 8-bit fields.
 *
 * @param wlsb   The W-LSB object
 * @param value  The value to encode using the LSB algorithm
 * @param k      The number of bits for encoding
 * @param p      The shift parameter p
 * @return       true if the number of bits is enough for encoding or not
 */
bool wlsb_is_kp_possible_8bits(const struct c_wlsb *const wlsb,
                               const uint8_t value,
                               const size_t k,
                               const rohc_lsb_shift_t p)
{
	bool enc_possible = false;

	assert(k <= wlsb->bits);

	if(k == wlsb->bits)
	{
		enc_possible = true;
	}
	/* use all bits if the window contains no value */
	else if(wlsb->count == 0)
	{
		enc_possible = !!(k >= wlsb->bits);
	}
	else
	{
		const uint8_t interval_width = (1U << k) - 1; /* interval width = 2^k - 1 */
		size_t i;

		/* find the minimal number of bits of the value required to be able
		 * to recreate it thanks to ANY value in the window */
		for(i = 0; i < wlsb->window_width; i++)
		{
			const struct c_window *const entry = wlsb->window + i;

			if(entry->used)
			{
				const uint8_t v_ref = entry->value;

				/* compute the minimal and maximal values of the interval:
				 *   min = v_ref - p
				 *   max = v_ref + interval_with - p
				 *
				 * Straddling the lower and upper wraparound boundaries
				 * is handled without additional operation */
				const uint8_t min = v_ref - p;
				const uint8_t max = min + interval_width;

				if(min <= max)
				{
					/* interpretation interval does not straddle field boundaries,
					 * check if value is in [min, max] */
					if(value < min || value > max)
					{
						break;
					}
				}
				else
				{
					if(value < min && value > max)
					{
						break;
					}
				}
			}
		}
		if(i == wlsb->window_width)
		{
			enc_possible = true;
		}
	}

	return enc_possible;
}


/**
 * @brief Find out whether the given number of bits is enough to encode value
 *
 * The function is dedicated to 16-bit fields.
 *
 * @param wlsb   The W-LSB object
 * @param value  The value to encode using the LSB algorithm
 * @param k      The number of bits for encoding
 * @param p      The shift parameter p
 * @return       true if the number of bits is enough for encoding or not
 */
bool wlsb_is_kp_possible_16bits(const struct c_wlsb *const wlsb,
                                const uint16_t value,
                                const size_t k,
                                const rohc_lsb_shift_t p)
{
	bool enc_possible = false;

	/* use all bits if the window contains no value */
	if(wlsb->count == 0)
	{
		enc_possible = !!(k >= wlsb->bits);
	}
	else
	{
		const uint16_t interval_width = (1U << k) - 1; /* interval width = 2^k - 1 */
		size_t i;

		/* find the minimal number of bits of the value required to be able
		 * to recreate it thanks to ANY value in the window */
		for(i = 0; i < wlsb->window_width; i++)
		{
			const struct c_window *const entry = wlsb->window + i;

			if(entry->used)
			{
				const uint16_t v_ref = entry->value;

				/* compute the minimal and maximal values of the interval:
				 *   min = v_ref - p
				 *   max = v_ref + interval_with - p
				 *
				 * Straddling the lower and upper wraparound boundaries
				 * is handled without additional operation */
				const uint16_t min = v_ref - p;
				const uint16_t max = min + interval_width;

				if(min <= max)
				{
					/* interpretation interval does not straddle field boundaries,
					 * check if value is in [min, max] */
					if(value < min || value > max)
					{
						break;
					}
				}
				else
				{
					/* the interpretation interval does straddle the field boundaries,
					 * check if value is in [min, 0xffff] or [0, max] */
					if(value < min && value > max)
					{
						break;
					}
				}
			}
		}
		if(i == wlsb->window_width)
		{
			enc_possible = true;
		}
	}

	return enc_possible;
}


/**
 * @brief Find out whether the given number of bits is enough to encode value
 *
 * The function is dedicated to 32-bit fields.
 *
 * @param wlsb   The W-LSB object
 * @param value  The value to encode using the LSB algorithm
 * @param k      The number of bits for encoding
 * @param p      The shift parameter p
 * @return       true if the number of bits is enough for encoding or not
 */
bool wlsb_is_kp_possible_32bits(const struct c_wlsb *const wlsb,
                                const uint32_t value,
                                const size_t k,
                                const rohc_lsb_shift_t p)
{
	bool enc_possible = false;

	assert(k <= wlsb->bits);

	if(k == wlsb->bits)
	{
		enc_possible = true;
	}
	/* use all bits if the window contains no value */
	else if(wlsb->count == 0)
	{
		enc_possible = !!(k >= wlsb->bits);
	}
	else
	{
		const uint32_t interval_width = (1U << k) - 1; /* interval width = 2^k - 1 */
		size_t i;

		/* find the minimal number of bits of the value required to be able
		 * to recreate it thanks to ANY value in the window */
		for(i = 0; i < wlsb->window_width; i++)
		{
			const struct c_window *const entry = wlsb->window + i;

			if(entry->used)
			{
				const uint32_t v_ref = entry->value;

				/* compute the minimal and maximal values of the interval:
				 *   min = v_ref - p
				 *   max = v_ref + interval_with - p
				 *
				 * Straddling the lower and upper wraparound boundaries
				 * is handled without additional operation */
				const uint32_t min = v_ref - p;
				const uint32_t max = min + interval_width;

				if(min <= max)
				{
					/* interpretation interval does not straddle field boundaries,
					 * check if value is in [min, max] */
					if(value < min || value > max)
					{
						break;
					}
				}
				else
				{
					if(value < min && value > max)
					{
						break;
					}
				}
			}
		}
		if(i == wlsb->window_width)
		{
			enc_possible = true;
		}
	}

	return enc_possible;
}


/**
 * @brief Acknowledge based on the Sequence Number (SN)
 *
 * Removes all window entries older (and including) than the one that matches
 * the given SN bits.
 *
 * @param wlsb        The W-LSB object
 * @param sn_bits     The LSB of the SN to acknowledge
 * @param sn_bits_nr  The number of LSB of the SN to acknowledge
 * @return            The number of acked window entries
 */
size_t wlsb_ack(struct c_wlsb *const wlsb,
                const uint32_t sn_bits,
                const size_t sn_bits_nr)
{
	size_t entry = wlsb->next;
	uint32_t sn_mask;
	size_t i;

	if(sn_bits_nr < 32)
	{
		sn_mask = (1U << sn_bits_nr) - 1;
	}
	else
	{
		sn_mask = 0xffffffffUL;
	}

	/* search for the window entry that matches the given SN LSB
	 * starting from the one */
	for(i = 0; i < wlsb->count; i++)
	{
		entry = wlsb_get_next_older(entry, wlsb->window_width - 1);
		if((wlsb->window[entry].sn & sn_mask) == sn_bits)
		{
			/* remove the window entry and all the older ones if found */
			return wlsb_ack_remove(wlsb, entry);
		}
	}

	return 0;
}


/**
 * @brief Whether the given SN is present in the given WLSB window
 *
 * @param wlsb  The WLSB in which to search for the SN
 * @param sn    The SN to search for
 * @return      true if the SN is found, false if not
 */
bool wlsb_is_sn_present(struct c_wlsb *const wlsb, const uint32_t sn)
{
	size_t entry = wlsb->next;
	size_t i;

	/* search for the window entry that matches the given SN LSB
	 * starting from the one */
	for(i = 0; i < wlsb->count; i++)
	{
		entry = wlsb_get_next_older(entry, wlsb->window_width - 1);
		if(sn == wlsb->window[entry].sn)
		{
			return true;
		}
		else if(sn > wlsb->window[entry].sn)
		{
			return false;
		}
	}

	return false;
}


/*
 * Private functions
 */


/**
 * @brief Get the next older entry
 *
 * @param entry  The entry for which to get the next older entry
 * @param max    The max entry value
 * @return       The next older entry
 */
static size_t wlsb_get_next_older(const size_t entry, const size_t max)
{
	return ((entry == 0) ? max : (entry - 1));
}


/**
 * @brief Removes all W-LSB window entries prior to the given position
 *
 * @param wlsb  The W-LSB object
 * @param pos   The position to set as the oldest
 * @return      The number of acked window entries
 */
static size_t wlsb_ack_remove(struct c_wlsb *const wlsb, const size_t pos)
{
	size_t acked_nr = 0;

	while(wlsb->oldest != pos)
	{
		/* remove the oldest entry */
		wlsb->window[wlsb->oldest].used = false;
		wlsb->oldest = (wlsb->oldest + 1) % wlsb->window_width;
		wlsb->count--;
		acked_nr++;
	}

	return acked_nr;
}

