/*-------------------------------------------------------------------------
 *
 * bitmapset_standalone.c
 *	  Standalone version of PostgreSQL generic bitmap set package
 *
 * Adapted from PostgreSQL's bitmapset.c to compile without any
 * PostgreSQL dependencies. Uses standard C library and GCC builtins only.
 *
 * A bitmap set can represent any set of nonnegative integers, although
 * it is mainly intended for sets where the maximum value is not large,
 * say at most a few hundred.  By convention, we always represent a set with
 * the minimum possible number of words, i.e, there are never any trailing
 * zero words.  Enforcing this requires that an empty set is represented as
 * NULL.  Because an empty Bitmapset is represented as NULL, a non-NULL
 * Bitmapset always has at least 1 Bitmapword.  We can exploit this fact to
 * speed up various loops over the Bitmapset's words array by using "do while"
 * loops instead of "for" loops.  This means the code does not waste time
 * checking the loop condition before the first iteration.  For Bitmapsets
 * containing only a single word (likely the majority of them) this halves the
 * number of loop condition checks.
 *
 * Callers must ensure that the set returned by functions in this file which
 * adjust the members of an existing set is assigned to all pointers pointing
 * to that existing set.  No guarantees are made that we'll ever modify the
 * existing set in-place and return it.
 *
 * Original copyright (c) 2003-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "bitmapset_standalone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Memory allocation helpers that abort on failure, replacing PostgreSQL's
 * palloc/palloc0/pfree/repalloc.
 */
static inline void *
bms_alloc(size_t size)
{
	void	   *p = malloc(size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
bms_alloc0(size_t size)
{
	void	   *p = calloc(1, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

static inline void *
bms_realloc(void *ptr, size_t size)
{
	void	   *p = realloc(ptr, size);

	if (!p)
	{
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}

#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

#define BITMAPSET_SIZE(nwords)	\
	(offsetof(Bitmapset, words) + (nwords) * sizeof(bitmapword))

/*----------
 * This is a well-known cute trick for isolating the rightmost one-bit
 * in a word.  It assumes two's complement arithmetic.  Consider any
 * nonzero value, and focus attention on the rightmost one.  The value is
 * then something like
 *				xxxxxx10000
 * where x's are unspecified bits.  The two's complement negative is formed
 * by inverting all the bits and adding one.  Inversion gives
 *				yyyyyy01111
 * where each y is the inverse of the corresponding x.  Incrementing gives
 *				yyyyyy10000
 * and then ANDing with the original value gives
 *				00000010000
 * This works for all cases except original value = zero, where of course
 * we get zero.
 *----------
 */
#define RIGHTMOST_ONE(x) ((signedbitmapword) (x) & -((signedbitmapword) (x)))

#define HAS_MULTIPLE_ONES(x)	((bitmapword) RIGHTMOST_ONE(x) != (x))

/* Min macro, if not already defined */
#ifndef Min
#define Min(a, b)	((a) < (b) ? (a) : (b))
#endif

/* Branch prediction hints (no-op if not GCC/Clang) */
#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x)	__builtin_expect((x) != 0, 0)
#else
#define unlikely(x)	(x)
#endif

/*
 * bms_copy - make a copy of a bitmapset
 */
Bitmapset *
bms_copy(const Bitmapset *a)
{
	Bitmapset  *result;
	size_t		size;

	if (a == NULL)
		return NULL;

	size = BITMAPSET_SIZE(a->nwords);
	result = (Bitmapset *) bms_alloc(size);
	memcpy(result, a, size);
	return result;
}

/*
 * bms_equal - are two bitmapsets equal? or both NULL?
 */
bool
bms_equal(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return true;
		return false;
	}
	else if (b == NULL)
		return false;

	/* can't be equal if the word counts don't match */
	if (a->nwords != b->nwords)
		return false;

	/* check each word matches */
	i = 0;
	do
	{
		if (a->words[i] != b->words[i])
			return false;
	} while (++i < a->nwords);

	return true;
}

/*
 * bms_compare - qsort-style comparator for bitmapsets
 *
 * This guarantees to report values as equal iff bms_equal would say they are
 * equal.  Otherwise, the highest-numbered bit that is set in one value but
 * not the other determines the result.  (This rule means that, for example,
 * {6} is greater than {5}, which seems plausible.)
 */
int
bms_compare(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return (b == NULL) ? 0 : -1;
	else if (b == NULL)
		return +1;

	/* the set with the most words must be greater */
	if (a->nwords != b->nwords)
		return (a->nwords > b->nwords) ? +1 : -1;

	i = a->nwords - 1;
	do
	{
		bitmapword	aw = a->words[i];
		bitmapword	bw = b->words[i];

		if (aw != bw)
			return (aw > bw) ? +1 : -1;
	} while (--i >= 0);
	return 0;
}

/*
 * bms_make_singleton - build a bitmapset containing a single member
 */
Bitmapset *
bms_make_singleton(int x)
{
	Bitmapset  *result;
	int			wordnum,
				bitnum;

	if (x < 0)
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	result = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(wordnum + 1));
	result->nwords = wordnum + 1;
	result->words[wordnum] = ((bitmapword) 1 << bitnum);
	return result;
}

/*
 * bms_free - free a bitmapset
 *
 * Same as free except for allowing NULL input
 */
void
bms_free(Bitmapset *a)
{
	if (a)
		free(a);
}


/*
 * bms_union - create and return a new set containing all members from both
 * input sets.  Both inputs are left unmodified.
 */
Bitmapset *
bms_union(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return bms_copy(a);
	/* Identify shorter and longer input; copy the longer one */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = bms_copy(a);
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	return result;
}

/*
 * bms_intersect - create and return a new set containing members which both
 * input sets have in common.  Both inputs are left unmodified.
 */
Bitmapset *
bms_intersect(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			lastnonzero;
	int			resultlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL || b == NULL)
		return NULL;

	/* Identify shorter and longer input; copy the shorter one */
	if (a->nwords <= b->nwords)
	{
		result = bms_copy(a);
		other = b;
	}
	else
	{
		result = bms_copy(b);
		other = a;
	}
	/* And intersect the longer input with the result */
	resultlen = result->nwords;
	lastnonzero = -1;
	i = 0;
	do
	{
		result->words[i] &= other->words[i];

		if (result->words[i] != 0)
			lastnonzero = i;
	} while (++i < resultlen);
	/* If we computed an empty result, we must return NULL */
	if (lastnonzero == -1)
	{
		free(result);
		return NULL;
	}

	/* get rid of trailing zero words */
	result->nwords = lastnonzero + 1;
	return result;
}

/*
 * bms_difference - create and return a new set containing all the members of
 * 'a' without the members of 'b'.
 */
Bitmapset *
bms_difference(const Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return bms_copy(a);

	/*
	 * An empty result is a very common case, so it's worth optimizing for
	 * that by testing bms_nonempty_difference().  This saves us a
	 * malloc/free cycle compared to checking after-the-fact.
	 */
	if (!bms_nonempty_difference(a, b))
		return NULL;

	/* Copy the left input */
	result = bms_copy(a);

	/* And remove b's bits from result */
	if (result->nwords > b->nwords)
	{
		/*
		 * We'll never need to remove trailing zero words when 'a' has more
		 * words than 'b' as the additional words must be non-zero.
		 */
		i = 0;
		do
		{
			result->words[i] &= ~b->words[i];
		} while (++i < b->nwords);
	}
	else
	{
		int			lastnonzero = -1;

		/* we may need to remove trailing zero words from the result. */
		i = 0;
		do
		{
			result->words[i] &= ~b->words[i];

			/* remember the last non-zero word */
			if (result->words[i] != 0)
				lastnonzero = i;
		} while (++i < result->nwords);

		/* trim off trailing zero words */
		result->nwords = lastnonzero + 1;
	}
	assert(result->nwords != 0);

	/* Need not check for empty result, since we handled that case above */
	return result;
}

/*
 * bms_offset_members
 *		Creates a new Bitmapset with all members of 'a' adjusted to add the
 *		value of 'offset' to each member.
 *
 * Members which would become negative as a result of a negative offset will
 * be removed from the set, whereas too large an offset, which would result in
 * a member going > INT_MAX, will result in an abort.
 */
Bitmapset *
bms_offset_members(const Bitmapset *a, int offset)
{
	Bitmapset  *result;
	int			offset_words;
	int			offset_bits;
	int			new_nwords;
	int			old_nwords;
	int32_t		high_bit;
	int			old_highest;
	int			new_highest;

	/* nothing to do for empty sets */
	if (a == NULL)
		return NULL;

	old_nwords = a->nwords;
	offset_words = WORDNUM(offset);
	offset_bits = BITNUM(offset);
	high_bit = bmw_leftmost_one_pos(a->words[a->nwords - 1]);
	old_highest = (old_nwords - 1) * BITS_PER_BITMAPWORD + high_bit;

	/* don't create a set with a member that doesn't fit into an int32 */
	if (__builtin_add_overflow(old_highest, offset, &new_highest))
	{
		fprintf(stderr, "bitmapset overflow\n");
		abort();
	}
	/* return NULL if the new set would be empty */
	else if (new_highest < 0)
		return NULL;

	new_nwords = WORDNUM(new_highest) + 1;
	result = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(new_nwords));
	result->nwords = new_nwords;

	/* handle zero and positive offsets (bitshift left) */
	if (offset >= 0)
	{
		/*
		 * We special-case offsetting only by whole words so we don't have to
		 * special-case bitshifting by BITS_PER_BITMAPWORD places, which has
		 * an undefined behavior.
		 */
		if (offset_bits == 0)
		{
			int			i = 0;

			/*
			 * The old set is guaranteed to have at least 1 word, so use
			 * do/while to save the redundant initial loop bounds check.
			 */
			do
			{
				assert(i + offset_words < new_nwords);
				result->words[i + offset_words] = a->words[i];
			} while (++i < old_nwords);
		}
		else
		{
			int			carry_bits = BITS_PER_BITMAPWORD - offset_bits;
			bitmapword	prev_carry = 0;
			int			i = 0;

			do
			{
				bitmapword	carry = (a->words[i] >> carry_bits);

				assert(i + offset_words < new_nwords);
				/* shift bits up and carry bits from the previous word */
				result->words[i + offset_words] = (a->words[i] << offset_bits) | prev_carry;
				prev_carry = carry;
			} while (++i < old_nwords);
			result->words[new_nwords - 1] |= prev_carry;
		}
	}

	/* handle negative offset (bitshift right) */
	else
	{
		/* make the negative offset_words and offset_bits positive */
		offset_words = 0 - offset_words;
		offset_bits = 0 - offset_bits;

		/* as above, special case shifting only by whole words */
		if (offset_bits == 0)
		{
			int			i = 0;

			do
			{
				assert(i + offset_words < old_nwords);
				result->words[i] = a->words[i + offset_words];
			} while (++i < new_nwords);
		}
		else
		{
			int			carry_bits = BITS_PER_BITMAPWORD - offset_bits;
			bitmapword	prev_carry = 0;
			int			i = new_nwords - 1;

			/* carry bits from any word just above where the loop starts */
			if (old_nwords > new_nwords + offset_words)
				prev_carry = (a->words[new_nwords + offset_words] << carry_bits);

			/*
			 * We loop backward over the array so we correctly carry bits from
			 * higher words.
			 */
			do
			{
				bitmapword	carry = (a->words[i + offset_words] << carry_bits);

				assert(i + offset_words < old_nwords);

				/* shift bits down and carry bits from the previous word */
				result->words[i] = (a->words[i + offset_words] >> offset_bits) | prev_carry;
				prev_carry = carry;
			} while (--i >= 0);
		}
	}

	return result;
}

/*
 * bms_is_subset - is A a subset of B?
 */
bool
bms_is_subset(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return true;			/* empty set is a subset of anything */
	if (b == NULL)
		return false;

	/* 'a' can't be a subset of 'b' if it contains more words */
	if (a->nwords > b->nwords)
		return false;

	/* Check all 'a' members are set in 'b' */
	i = 0;
	do
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return false;
	} while (++i < a->nwords);
	return true;
}

/*
 * bms_subset_compare - compare A and B for equality/subset relationships
 *
 * This is more efficient than testing bms_is_subset in both directions.
 */
BMS_Comparison
bms_subset_compare(const Bitmapset *a, const Bitmapset *b)
{
	BMS_Comparison result;
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
	{
		if (b == NULL)
			return BMS_EQUAL;
		return BMS_SUBSET1;
	}
	if (b == NULL)
		return BMS_SUBSET2;

	/* Check common words */
	result = BMS_EQUAL;			/* status so far */
	shortlen = Min(a->nwords, b->nwords);
	i = 0;
	do
	{
		bitmapword	aword = a->words[i];
		bitmapword	bword = b->words[i];

		if ((aword & ~bword) != 0)
		{
			/* a is not a subset of b */
			if (result == BMS_SUBSET1)
				return BMS_DIFFERENT;
			result = BMS_SUBSET2;
		}
		if ((bword & ~aword) != 0)
		{
			/* b is not a subset of a */
			if (result == BMS_SUBSET2)
				return BMS_DIFFERENT;
			result = BMS_SUBSET1;
		}
	} while (++i < shortlen);
	/* Check extra words */
	if (a->nwords > b->nwords)
	{
		/* if a has more words then a is not a subset of b */
		if (result == BMS_SUBSET1)
			return BMS_DIFFERENT;
		return BMS_SUBSET2;
	}
	else if (a->nwords < b->nwords)
	{
		/* if b has more words then b is not a subset of a */
		if (result == BMS_SUBSET2)
			return BMS_DIFFERENT;
		return BMS_SUBSET1;
	}
	return result;
}

/*
 * bms_is_member - is X a member of A?
 */
bool
bms_is_member(int x, const Bitmapset *a)
{
	int			wordnum,
				bitnum;

	/* XXX better to just return false for x<0 ? */
	if (x < 0)
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	if (a == NULL)
		return false;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);
	if (wordnum >= a->nwords)
		return false;
	if ((a->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
		return true;
	return false;
}

/*
 * bms_member_index
 *		determine 0-based index of member x in the bitmap
 *
 * Returns (-1) when x is not a member.
 */
int
bms_member_index(Bitmapset *a, int x)
{
	int			bitnum;
	int			wordnum;
	int			result = 0;
	bitmapword	mask;

	/* return -1 if not a member of the bitmap */
	if (!bms_is_member(x, a))
		return -1;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* count bits in preceding words */
	for (int i = 0; i < wordnum; i++)
		result += bmw_popcount(a->words[i]);

	/*
	 * Now add bits of the last word, but only those before the item. We can
	 * do that by applying a mask and then using popcount again. To get
	 * 0-based index, we want to count only preceding bits, not the item
	 * itself, so we subtract 1.
	 */
	mask = ((bitmapword) 1 << bitnum) - 1;
	result += bmw_popcount(a->words[wordnum] & mask);

	return result;
}

/*
 * bms_overlap - do sets overlap (ie, have a nonempty intersection)?
 */
bool
bms_overlap(const Bitmapset *a, const Bitmapset *b)
{
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL || b == NULL)
		return false;
	/* Check words in common */
	shortlen = Min(a->nwords, b->nwords);
	i = 0;
	do
	{
		if ((a->words[i] & b->words[i]) != 0)
			return true;
	} while (++i < shortlen);
	return false;
}

/*
 * bms_nonempty_difference - do sets have a nonempty difference?
 *
 * i.e., are any members set in 'a' that are not also set in 'b'.
 */
bool
bms_nonempty_difference(const Bitmapset *a, const Bitmapset *b)
{
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return false;
	if (b == NULL)
		return true;
	/* if 'a' has more words then it must contain additional members */
	if (a->nwords > b->nwords)
		return true;
	/* Check all 'a' members are set in 'b' */
	i = 0;
	do
	{
		if ((a->words[i] & ~b->words[i]) != 0)
			return true;
	} while (++i < a->nwords);
	return false;
}

/*
 * bms_singleton_member - return the sole integer member of set
 *
 * Aborts if |a| is not 1.
 */
int
bms_singleton_member(const Bitmapset *a)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
	{
		fprintf(stderr, "bitmapset is empty\n");
		abort();
	}

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
			{
				fprintf(stderr, "bitmapset has multiple members\n");
				abort();
			}
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	/* we don't expect non-NULL sets to be empty */
	assert(result >= 0);
	return result;
}

/*
 * bms_get_singleton_member
 *
 * Test whether the given set is a singleton.
 * If so, set *member to the value of its sole member, and return true.
 * If not, return false, without changing *member.
 *
 * This is more convenient and faster than calling bms_membership() and then
 * bms_singleton_member(), if we don't care about distinguishing empty sets
 * from multiple-member sets.
 */
bool
bms_get_singleton_member(const Bitmapset *a, int *member)
{
	int			result = -1;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return false;

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result >= 0 || HAS_MULTIPLE_ONES(w))
				return false;
			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
		}
	} while (++wordnum < nwords);

	/* we don't expect non-NULL sets to be empty */
	assert(result >= 0);
	*member = result;
	return true;
}

/*
 * bms_num_members - count members of set
 */
int
bms_num_members(const Bitmapset *a)
{
	int			result = 0;
	int			nwords;
	int			i;

	if (a == NULL)
		return 0;

	nwords = a->nwords;
	i = 0;
	do
	{
		result += bmw_popcount(a->words[i]);
	} while (++i < nwords);

	return result;
}

/*
 * bms_membership - does a set have zero, one, or multiple members?
 *
 * This is faster than making an exact count with bms_num_members().
 */
BMS_Membership
bms_membership(const Bitmapset *a)
{
	BMS_Membership result = BMS_EMPTY_SET;
	int			nwords;
	int			wordnum;

	if (a == NULL)
		return BMS_EMPTY_SET;

	nwords = a->nwords;
	wordnum = 0;
	do
	{
		bitmapword	w = a->words[wordnum];

		if (w != 0)
		{
			if (result != BMS_EMPTY_SET || HAS_MULTIPLE_ONES(w))
				return BMS_MULTIPLE;
			result = BMS_SINGLETON;
		}
	} while (++wordnum < nwords);
	return result;
}


/*
 * bms_add_member - add a specified member to set
 *
 * 'a' is recycled when possible.
 */
Bitmapset *
bms_add_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	if (a == NULL)
		return bms_make_singleton(x);

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* enlarge the set if necessary */
	if (wordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(wordnum + 1));
		a->nwords = wordnum + 1;
		/* zero out the enlarged portion */
		i = oldnwords;
		do
		{
			a->words[i] = 0;
		} while (++i < a->nwords);
	}

	a->words[wordnum] |= ((bitmapword) 1 << bitnum);

	return a;
}

/*
 * bms_del_member - remove a specified member from set
 *
 * No error if x is not currently a member of set
 *
 * 'a' is recycled when possible.
 */
Bitmapset *
bms_del_member(Bitmapset *a, int x)
{
	int			wordnum,
				bitnum;

	if (x < 0)
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	if (a == NULL)
		return NULL;

	wordnum = WORDNUM(x);
	bitnum = BITNUM(x);

	/* member can't exist.  Return 'a' unmodified */
	if (unlikely(wordnum >= a->nwords))
		return a;

	a->words[wordnum] &= ~((bitmapword) 1 << bitnum);

	/* when last word becomes empty, trim off all trailing empty words */
	if (a->words[wordnum] == 0 && wordnum == a->nwords - 1)
	{
		/* find the last non-empty word and make that the new final word */
		for (int i = wordnum - 1; i >= 0; i--)
		{
			if (a->words[i] != 0)
			{
				a->nwords = i + 1;
				return a;
			}
		}

		/* the set is now empty */
		free(a);
		return NULL;
	}
	return a;
}

/*
 * bms_add_members - like bms_union, but left input is recycled when possible
 */
Bitmapset *
bms_add_members(Bitmapset *a, const Bitmapset *b)
{
	Bitmapset  *result;
	const Bitmapset *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
		return a;

	/* Identify shorter and longer input; copy the longer one if needed */
	if (a->nwords < b->nwords)
	{
		result = bms_copy(b);
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	if (result != a)
		free(a);

	return result;
}

/*
 * bms_replace_members
 *		Remove all existing members from 'a' and repopulate the set with members
 *		from 'b', recycling 'a', when possible.
 */
Bitmapset *
bms_replace_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	if (a == NULL)
		return bms_copy(b);
	if (b == NULL)
	{
		free(a);
		return NULL;
	}

	if (a->nwords < b->nwords)
		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(b->nwords));

	i = 0;
	do
	{
		a->words[i] = b->words[i];
	} while (++i < b->nwords);

	a->nwords = b->nwords;

	return a;
}

/*
 * bms_add_range
 *		Add members in the range of 'lower' to 'upper' to the set.
 *
 * Note this could also be done by calling bms_add_member in a loop, however,
 * using this function will be faster when the range is large as we work at
 * the bitmapword level rather than at bit level.
 */
Bitmapset *
bms_add_range(Bitmapset *a, int lower, int upper)
{
	int			lwordnum,
				lbitnum,
				uwordnum,
				ushiftbits,
				wordnum;

	/* do nothing if nothing is called for, without further checking */
	if (upper < lower)
		return a;

	if (lower < 0)
	{
		fprintf(stderr, "negative bitmapset member not allowed\n");
		abort();
	}
	uwordnum = WORDNUM(upper);

	if (a == NULL)
	{
		a = (Bitmapset *) bms_alloc0(BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = uwordnum + 1;
	}
	else if (uwordnum >= a->nwords)
	{
		int			oldnwords = a->nwords;
		int			i;

		/* ensure we have enough words to store the upper bit */
		a = (Bitmapset *) bms_realloc(a, BITMAPSET_SIZE(uwordnum + 1));
		a->nwords = uwordnum + 1;
		/* zero out the enlarged portion */
		i = oldnwords;
		do
		{
			a->words[i] = 0;
		} while (++i < a->nwords);
	}

	wordnum = lwordnum = WORDNUM(lower);

	lbitnum = BITNUM(lower);
	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(upper) + 1);

	/*
	 * Special case when lwordnum is the same as uwordnum we must perform the
	 * upper and lower masking on the word.
	 */
	if (lwordnum == uwordnum)
	{
		a->words[lwordnum] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1)
			& (~(bitmapword) 0) >> ushiftbits;
	}
	else
	{
		/* turn on lbitnum and all bits left of it */
		a->words[wordnum++] |= ~(bitmapword) (((bitmapword) 1 << lbitnum) - 1);

		/* turn on all bits for any intermediate words */
		while (wordnum < uwordnum)
			a->words[wordnum++] = ~(bitmapword) 0;

		/* turn on upper's bit and all bits right of it. */
		a->words[uwordnum] |= (~(bitmapword) 0) >> ushiftbits;
	}

	return a;
}

/*
 * bms_int_members - like bms_intersect, but left input is recycled when
 * possible
 */
Bitmapset *
bms_int_members(Bitmapset *a, const Bitmapset *b)
{
	int			lastnonzero;
	int			shortlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
	{
		free(a);
		return NULL;
	}

	/* Intersect b into a; we need never copy */
	shortlen = Min(a->nwords, b->nwords);
	lastnonzero = -1;
	i = 0;
	do
	{
		a->words[i] &= b->words[i];

		if (a->words[i] != 0)
			lastnonzero = i;
	} while (++i < shortlen);

	/* If we computed an empty result, we must return NULL */
	if (lastnonzero == -1)
	{
		free(a);
		return NULL;
	}

	/* get rid of trailing zero words */
	a->nwords = lastnonzero + 1;

	return a;
}

/*
 * bms_del_members - delete members in 'a' that are set in 'b'.  'a' is
 * recycled when possible.
 */
Bitmapset *
bms_del_members(Bitmapset *a, const Bitmapset *b)
{
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return NULL;
	if (b == NULL)
		return a;

	/* Remove b's bits from a; we need never copy */
	if (a->nwords > b->nwords)
	{
		/*
		 * We'll never need to remove trailing zero words when 'a' has more
		 * words than 'b'.
		 */
		i = 0;
		do
		{
			a->words[i] &= ~b->words[i];
		} while (++i < b->nwords);
	}
	else
	{
		int			lastnonzero = -1;

		/* we may need to remove trailing zero words from the result. */
		i = 0;
		do
		{
			a->words[i] &= ~b->words[i];

			/* remember the last non-zero word */
			if (a->words[i] != 0)
				lastnonzero = i;
		} while (++i < a->nwords);

		/* check if 'a' has become empty */
		if (lastnonzero == -1)
		{
			free(a);
			return NULL;
		}

		/* trim off any trailing zero words */
		a->nwords = lastnonzero + 1;
	}

	return a;
}

/*
 * bms_join - like bms_union, but *either* input *may* be recycled
 */
Bitmapset *
bms_join(Bitmapset *a, Bitmapset *b)
{
	Bitmapset  *result;
	Bitmapset  *other;
	int			otherlen;
	int			i;

	/* Handle cases where either input is NULL */
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	/* Identify shorter and longer input; use longer one as result */
	if (a->nwords < b->nwords)
	{
		result = b;
		other = a;
	}
	else
	{
		result = a;
		other = b;
	}
	/* And union the shorter input into the result */
	otherlen = other->nwords;
	i = 0;
	do
	{
		result->words[i] |= other->words[i];
	} while (++i < otherlen);
	if (other != result)		/* pure paranoia */
		free(other);

	return result;
}

/*
 * bms_next_member - find next member of a set
 *
 * Returns smallest member greater than "prevbit", or -2 if there is none.
 * "prevbit" must NOT be less than -1, or the behavior is unpredictable.
 *
 * This is intended as support for iterating through the members of a set.
 * The typical pattern is
 *
 *			x = -1;
 *			while ((x = bms_next_member(inputset, x)) >= 0)
 *				process member x;
 *
 * Notice that when there are no more members, we return -2, not -1 as you
 * might expect.  The rationale for that is to allow distinguishing the
 * loop-not-started state (x == -1) from the loop-completed state (x == -2).
 * It makes no difference in simple loop usage, but complex iteration logic
 * might need such an ability.
 */
int
bms_next_member(const Bitmapset *a, int prevbit)
{
	int			nwords;
	bitmapword	mask;

	if (a == NULL)
		return -2;
	nwords = a->nwords;
	prevbit++;
	mask = (~(bitmapword) 0) << BITNUM(prevbit);
	for (int wordnum = WORDNUM(prevbit); wordnum < nwords; wordnum++)
	{
		bitmapword	w = a->words[wordnum];

		/* ignore bits before prevbit */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_rightmost_one_pos(w);
			return result;
		}

		/* in subsequent words, consider all bits */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_prev_member - find prev member of a set
 *
 * Returns largest member less than "prevbit", or -2 if there is none.
 * "prevbit" must NOT be more than one above the highest possible bit that can
 * be set in the Bitmapset at its current size.
 *
 * To ease finding the highest set bit for the initial loop, the special
 * prevbit value of -1 can be passed to have the function find the highest
 * valued member in the set.
 *
 * This is intended as support for iterating through the members of a set in
 * reverse.  The typical pattern is
 *
 *			x = -1;
 *			while ((x = bms_prev_member(inputset, x)) >= 0)
 *				process member x;
 *
 * Notice that when there are no more members, we return -2, not -1 as you
 * might expect.  The rationale for that is to allow distinguishing the
 * loop-not-started state (x == -1) from the loop-completed state (x == -2).
 * It makes no difference in simple loop usage, but complex iteration logic
 * might need such an ability.
 */

int
bms_prev_member(const Bitmapset *a, int prevbit)
{
	int			ushiftbits;
	bitmapword	mask;

	/*
	 * If set is NULL or if there are no more bits to the right then we've
	 * nothing to do.
	 */
	if (a == NULL || prevbit == 0)
		return -2;

	/* Validate callers didn't give us something out of range */
	assert(prevbit <= a->nwords * BITS_PER_BITMAPWORD);
	assert(prevbit >= -1);

	/* transform -1 to the highest possible bit we could have set */
	if (prevbit == -1)
		prevbit = a->nwords * BITS_PER_BITMAPWORD - 1;
	else
		prevbit--;

	ushiftbits = BITS_PER_BITMAPWORD - (BITNUM(prevbit) + 1);
	mask = (~(bitmapword) 0) >> ushiftbits;
	for (int wordnum = WORDNUM(prevbit); wordnum >= 0; wordnum--)
	{
		bitmapword	w = a->words[wordnum];

		/* mask out bits left of prevbit */
		w &= mask;

		if (w != 0)
		{
			int			result;

			result = wordnum * BITS_PER_BITMAPWORD;
			result += bmw_leftmost_one_pos(w);
			return result;
		}

		/* in subsequent words, consider all bits */
		mask = (~(bitmapword) 0);
	}
	return -2;
}

/*
 * bms_hash_value - compute a hash key for a Bitmapset
 *
 * Uses FNV-1a hash instead of PostgreSQL's hash_any.
 */
uint32_t
bms_hash_value(const Bitmapset *a)
{
	if (a == NULL)
		return 0;				/* All empty sets hash to 0 */

	uint32_t	hash = 2166136261u;

	for (int i = 0; i < a->nwords; i++)
	{
		unsigned char *bytes = (unsigned char *) &a->words[i];

		for (size_t j = 0; j < sizeof(bitmapword); j++)
		{
			hash ^= bytes[j];
			hash *= 16777619u;
		}
	}
	return hash;
}
