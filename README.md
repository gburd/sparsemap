`sparsemap` is a sparse, compressed bitmap. In best case, it can store 2048
bits in just 8 bytes. In worst case, it stores the 2048 bits uncompressed and
requires additional 8 bytes of overhead.

The "best" case happens when large consecutive sequences of the bits are
either set ("1") or not set ("0"). If your numbers are consecutive 64bit
integers then sparsemap can compress up to 16kb in just 8 bytes.

## How does it work?

On the lowest level stores bits in sm_bitvec_t's (a uint32_t or uint64_t).

Each sm_bitvec_t has an additional descriptor (2 bits). A single word prepended
to each sm_bitvec_t describes its condition. The descriptor word and the
sm_bitvec_t's have the same size.) The descriptor of a sm_bitvec_t
specifies whether the sm_bitvec_t consists only of set bits ("1"), unset
bits ("0") or has a mixed payload. In the first and second case the
sm_bitvec_t is not stored.

An example shows a sequence of 4 x 16 bits (here, each sm_bitvec_t and the
Descriptor word has 16 bits):

      Descriptor:
      00 00 00 00 11 00 11 10
      ^^ ^^ ^^ ^^-- sm_bitvec_t #0 - #3 are "0000000000000000"
                  ^^-- sm_bitvec_t #4 is "1111111111111111"
                     ^^-- sm_bitvec_t #5 is "0000000000000000"
                        ^^-- sm_bitvec_t #7 is "1111111111111111"
                           ^^-- sm_bitvec_t #7 is "0110010101111001"

Since the first 7 sm_bitvec_t's are either all "1" or "0" they are not stored.
The actual memory sequence looks like this:

      0000000011001110 0110010101111001

Instead of storing 8 Words (16 bytes), we only store 2 Words (2 bytes): one
for the descriptor, one for last sm_bitvec_t #7.

The sparsemap stores a list of chunk maps, and for each chunk map it stores the
absolute address (i.e. if the user sets bit 0 and bit 10000, and the chunk map
capacity is 2048, the sparsemap creates two chunk maps; the first starts at
offset 0, the second starts at offset 8192).

# Usage instructions

The file `examples/ex_1.c` has example code.

## Final words

This bitmap has efficient compression when used on long sequences of set (or
unset) bits (i.e. with a word size of 64bit, and a payload of consecutive
numbers without gaps, the payload of 2048 x sizeof(uint64_t) = 16kb will occupy
only 8 bytes!

However, if the sequence is not consecutive and has gaps, it's possible that
the compression is inefficient, and the size (in the worst case) is identical
to an uncompressed bit vector (sometimes higher due to the bytes required for
metadata). In such cases, other compression schemes are more efficient (i.e.
http://lemire.me/blog/archives/2008/08/20/the-mythical-bitmap-index/).

This library was originally created for hamsterdb [http://hamsterdb.com] in
C++ and then translated to C99 code by Greg Burd <greg@burd.me>.
