#ifndef __XEN_BITMAP_H
#define __XEN_BITMAP_H

#ifndef __ASSEMBLY__

#include <xen/lib.h>
#include <xen/types.h>
#include <xen/bitops.h>

/*
 * bitmaps provide bit arrays that consume one or more unsigned
 * longs.  The bitmap interface and available operations are listed
 * here, in bitmap.h
 *
 * Function implementations generic to all architectures are in
 * lib/bitmap.c.  Functions implementations that are architecture
 * specific are in various asm/bitops.h headers
 * and other arch/<arch> specific files.
 *
 * See lib/bitmap.c for more details.
 */

/*
 * The available bitmap operations and their rough meaning in the
 * case that the bitmap is a single unsigned long are thus:
 *
 * bitmap_zero(dst, nbits)			*dst = 0UL
 * bitmap_fill(dst, nbits)			*dst = ~0UL
 * bitmap_copy(dst, src, nbits)			*dst = *src
 * bitmap_and(dst, src1, src2, nbits)		*dst = *src1 & *src2
 * bitmap_or(dst, src1, src2, nbits)		*dst = *src1 | *src2
 * bitmap_xor(dst, src1, src2, nbits)		*dst = *src1 ^ *src2
 * bitmap_andnot(dst, src1, src2, nbits)	*dst = *src1 & ~(*src2)
 * bitmap_complement(dst, src, nbits)		*dst = ~(*src)
 * bitmap_equal(src1, src2, nbits)		Are *src1 and *src2 equal?
 * bitmap_intersects(src1, src2, nbits) 	Do *src1 and *src2 overlap?
 * bitmap_subset(src1, src2, nbits)		Is *src1 a subset of *src2?
 * bitmap_empty(src, nbits)			Are all bits zero in *src?
 * bitmap_full(src, nbits)			Are all bits set in *src?
 * bitmap_weight(src, nbits)			Hamming Weight: number set bits
 */

/*
 * Also the following operations in asm/bitops.h apply to bitmaps.
 *
 * set_bit(bit, addr)			*addr |= bit
 * clear_bit(bit, addr)			*addr &= ~bit
 * change_bit(bit, addr)		*addr ^= bit
 * test_bit(bit, addr)			Is bit set in *addr?
 * test_and_set_bit(bit, addr)		Set bit and return old value
 * test_and_clear_bit(bit, addr)	Clear bit and return old value
 * test_and_change_bit(bit, addr)	Change bit and return old value
 * find_first_zero_bit(addr, nbits)	Position first zero bit in *addr
 * find_first_bit(addr, nbits)		Position first set bit in *addr
 * find_next_zero_bit(addr, nbits, bit)	Position next zero bit in *addr >= bit
 * find_next_bit(addr, nbits, bit)	Position next set bit in *addr >= bit
 */

/*
 * The DECLARE_BITMAP(name,bits) macro, in xen/types.h, can be used
 * to declare an array named 'name' of just enough unsigned longs to
 * contain all bit positions from 0 to 'bits' - 1.
 */

/*
 * lib/bitmap.c provides these functions:
 */

int __bitmap_empty(const unsigned long *bitmap, unsigned int bits);
int __bitmap_full(const unsigned long *bitmap, unsigned int bits);
int __bitmap_equal(const unsigned long *bitmap1,
                   const unsigned long *bitmap2, unsigned int bits);
void __bitmap_complement(unsigned long *dst, const unsigned long *src,
                         unsigned int bits);
void __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
                  const unsigned long *bitmap2, unsigned int bits);
void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
                 const unsigned long *bitmap2, unsigned int bits);
void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
                  const unsigned long *bitmap2, unsigned int bits);
void __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
                     const unsigned long *bitmap2, unsigned int bits);
int __bitmap_intersects(const unsigned long *bitmap1,
                        const unsigned long *bitmap2, unsigned int bits);
int __bitmap_subset(const unsigned long *bitmap1,
                    const unsigned long *bitmap2, unsigned int bits);
unsigned int __bitmap_weight(const unsigned long *bitmap, unsigned int bits);
extern void __bitmap_set(unsigned long *map, unsigned int start, int len);
extern void __bitmap_clear(unsigned long *map, unsigned int start, int len);

extern int bitmap_find_free_region(unsigned long *bitmap, int bits, int order);
extern void bitmap_release_region(unsigned long *bitmap, int pos, int order);
extern int bitmap_allocate_region(unsigned long *bitmap, int pos, int order);

#define BITMAP_LAST_WORD_MASK(nbits)					\
(									\
	((nbits) % BITS_PER_LONG) ?					\
		(1UL<<((nbits) % BITS_PER_LONG))-1 : ~0UL		\
)

#define bitmap_bytes(nbits) (BITS_TO_LONGS(nbits) * sizeof(unsigned long))

#define bitmap_switch(nbits, zero, small, large)			  \
	unsigned int n__ = (nbits);					  \
	if (__builtin_constant_p(nbits) && !n__) {			  \
		zero;							  \
	} else if (__builtin_constant_p(nbits) && n__ <= BITS_PER_LONG) { \
		small;							  \
	} else {							  \
		large;							  \
	}

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = 0UL,
		memset(dst, 0, bitmap_bytes(nbits)));
}

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	size_t nlongs = BITS_TO_LONGS(nbits);

	switch (nlongs) {
	case 0:
		break;
	default:
		memset(dst, -1, (nlongs - 1) * sizeof(unsigned long));
		/* fall through */
	case 1:
		dst[nlongs - 1] = BITMAP_LAST_WORD_MASK(nbits);
		break;
	}
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src,
			unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = *src,
		memcpy(dst, src, bitmap_bytes(nbits)));
}

static inline void bitmap_and(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = *src1 & *src2,
		__bitmap_and(dst, src1, src2, nbits));
}

static inline void bitmap_or(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = *src1 | *src2,
		__bitmap_or(dst, src1, src2, nbits));
}

static inline void bitmap_xor(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = *src1 ^ *src2,
		__bitmap_xor(dst, src1, src2, nbits));
}

static inline void bitmap_andnot(unsigned long *dst, const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = *src1 & ~*src2,
		__bitmap_andnot(dst, src1, src2, nbits));
}

static inline void bitmap_complement(unsigned long *dst, const unsigned long *src,
			unsigned int nbits)
{
	bitmap_switch(nbits,,
		*dst = ~*src & BITMAP_LAST_WORD_MASK(nbits),
		__bitmap_complement(dst, src, nbits));
}

static inline int bitmap_equal(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,
		return -1,
		return !((*src1 ^ *src2) & BITMAP_LAST_WORD_MASK(nbits)),
		return __bitmap_equal(src1, src2, nbits));
}

static inline int bitmap_intersects(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,
		return -1,
		return ((*src1 & *src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0,
		return __bitmap_intersects(src1, src2, nbits));
}

static inline int bitmap_subset(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	bitmap_switch(nbits,
		return -1,
		return !((*src1 & ~*src2) & BITMAP_LAST_WORD_MASK(nbits)),
		return __bitmap_subset(src1, src2, nbits));
}

static inline int bitmap_empty(const unsigned long *src, unsigned int nbits)
{
	bitmap_switch(nbits,
		return -1,
		return !(*src & BITMAP_LAST_WORD_MASK(nbits)),
		return __bitmap_empty(src, nbits));
}

static inline int bitmap_full(const unsigned long *src, unsigned int nbits)
{
	bitmap_switch(nbits,
		return -1,
		return !(~*src & BITMAP_LAST_WORD_MASK(nbits)),
		return __bitmap_full(src, nbits));
}

static inline unsigned int bitmap_weight(const unsigned long *src,
                                         unsigned int nbits)
{
	return __bitmap_weight(src, nbits);
}

#include <xen/byteorder.h>

#ifdef __LITTLE_ENDIAN
#define BITMAP_MEM_ALIGNMENT 8
#else
#define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif
#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))

static inline void bitmap_set(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__set_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0xff, nbits / 8);
	else
		__bitmap_set(map, start, nbits);
}

static inline void bitmap_clear(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__clear_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0, nbits / 8);
	else
		__bitmap_clear(map, start, nbits);
}

#undef bitmap_switch
#undef bitmap_bytes

/**
 * bitmap_for_each - bitate over every set bit in a memory region
 * @bit: The integer bitator
 * @addr: The address to base the search on
 * @size: The maximum size to search
 */
#define bitmap_for_each(bit, addr, size)                        \
    for ( (bit) = find_first_bit(addr, size);                   \
          (bit) < (size);                                       \
          (bit) = find_next_bit(addr, size, (bit) + 1) )


struct xenctl_bitmap;
int xenctl_bitmap_to_bitmap(unsigned long *bitmap,
                            const struct xenctl_bitmap *xenctl_bitmap,
                            unsigned int nbits);
int bitmap_to_xenctl_bitmap(struct xenctl_bitmap *xenctl_bitmap,
                            const unsigned long *bitmap, unsigned int nbits);

#endif /* __ASSEMBLY__ */

#endif /* __XEN_BITMAP_H */
