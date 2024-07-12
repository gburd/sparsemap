# Sparsemap

Bitsets, also called bitmaps, are commonly used as fast data structures.
Unfortunately, they can use too much memory. To compensate, we often use
compressed bitmaps.

`sparsemap` is a sparse, compressed bitmap. In the best case, it can store 2048
bits in just 8 bytes. In the worst case, it stores the 2048 bits uncompressed
and requires an additional 8 bytes of overhead.

The "best" case happens when large consecutive sequences of the bits are
either set ("1") or not set ("0"). If your numbers are consecutive 64 bit
integers then sparsemap can compress up to 16kb in just 8 bytes.

## How does it work?

On the lowest level a bitmap contains a number of chunks.  Each chunk has a
starting offset (`uint32_t`), a descriptor (the first `sm_bitvec_t`), and may
require a variable amount of additional space for encoding some bit patterns.

So, if the user sets bit 0 and bit 10000, and the chunk capacity is 2048,
the sparsemap creates two vectors; the first starts at offset 0, the second
starts at offset 8192.  Offsets must align with the capacity of a vector.

Every 2 bit pair within the descriptor (the first vector size portion of the
chunk after the 4 bytes for the offset) indicates the encoded bit pattern at
that location's relative offset.  This can be only set bits ("1"), only unset
bits ("0"), a mixed payload, or a run-length encoded extent of set bits
("1s"). A mixed vector consumes an additional `sm_bitvec_t`'s worth of space in
the buffer used to encode the bit pattern within that range.

Our examples below ignore the 4 byte overhead for the starting offset of these
chunks because they focus on the compressed encoding.  Also, for brevity, we use
16 bit wide vectors (`sm_bitvec_t`), rather than 64 bits.

The first example, shows a sequence of 4 x 16 bits:

      Descriptor:
      00 00 00 00 11 00 11 10
      ^^ ^^ ^^ ^^-- sm_bitvec_t [0..3] are "0000000000000000"
                  ^^-- sm_bitvec_t 4 is "1111111111111111"
                     ^^-- sm_bitvec_t 5 is "0000000000000000"
                        ^^-- sm_bitvec_t 6 is "1111111111111111"
                           ^^-- sm_bitvec_t 7 is "0110010101111001"

Since the first 7 (0 through 6) `sm_bitvec_t`'s are either all "1" or "0" and
their encoding reqiures no additional storage in the buffer, so the actual
memory sequence for this chunk within the buffer looks like this:

    0000000011001110 0110010101111001

Instead of storing 16 bytes, we only store 2 bytes: one for the descriptor, and
one for the last `sm_bitvec_t` #7.

A 2nd example shows a chunk with reduced capacity.

      Descriptor:
      00 00 00 00 11 01 01 01
      ^^ ^^ ^^ ^^-- sm_bitvec_t [0..3] are "0000000000000000"
                  ^^-- sm_bitvec_t 4 is "1111111111111111"
                     ^^ ^^ ^^-- sm_bitvec_t [5..8] represent nothing

The memory sequence for this second, truncated chunk, looks like this:

    0000000011010101

The bit pattern "01" can exist at the end of a chunk to indicate a reduced chunk
capacity.  In this case the chunk's last 3 descriptors indicate that it can
encode up to 5 * 16 or 80 bit positions rather than the normal 128 (when using
16 bit wide vectors, `sm_bitvec_t`).  When a chunk's capacity is entirely
truncated, it is empty and removed from the sparsemap entirely.

A 3rd example shows a single vector representing a long run of adjacent 1s
greater than the vector width (16 bits).  Let's examine the representation:

      Descriptor:
      01 00 00 00 10 01 00 00
      ^^-- sm_bitvec_t #0 is '01' indicating a run-length encoding of 1s
         ^^ ^^ ^^ ^^ ^^ ^^ ^^-- the lenght of the run, 144

When (if, and only if) the first 2 bits of the descriptor are '01' they indicate
that this is an run-length encoded (RLE) vector. The number of 1s is the
remaining portion of the descriptor -- in this case 14 of the 16 bits -- encode
the run length.  Simply mask the first two bits and interpret the remaining as an
`size_t`.

With that in mind, the memory sequence for this third example looks like this:

      01 00 00 00 10 00 01 00

Which decodes to a run of 144 adjacent 1s:

      1111 ... <then another 139 1s followed by the final> ... 1

The run must always be modulo the width of the descriptor (144 % 16 = 0).  The
next chunk would encode any additional 1s adjacent to this set of 144 unless
there were 16 more, then this chunk would change to:

      Descriptor:
      01 00 00 00 10 10 00 00
      ^^-- sm_bitvec_t #0 is '01' meaning RLE a set of adjacent 1s
         ^^ ^^ ^^ ^^ ^^ ^^ ^^-- the new length of the run is 160

Using this method of RLE for adjacent 1s we can compress (again, in this case
where bitvec_t is 16 bits wide) 2^14 or 16348 adjacent 1s to the width of a
single descriptor, 2 bytes in this case, rather than the approximately 4096
bytes without RLE.

## Usage instructions

Copy the files `src/sparsemap.c` and `include/sparsemap.h` into your project.
Review the `examples/*` and `tests/*` code.

## Final words

This bitmap has efficient compression when used on long sequences of set (or
unset) bits (i.e. with a word size of 64 bit and a payload of consecutive
numbers without gaps, the payload of 2048 x sizeof(uint64_t) = 16kb will occupy
only 8 bytes!).

However, if the sequence is not consecutive and has gaps, it's possible that
the compression is inefficient, and the size (in the worst case) is identical
to an uncompressed bit vector (sometimes higher due to the bytes required for
metadata). In such cases, other compression schemes are more efficient (i.e.
http://lemire.me/blog/archives/2008/08/20/the-mythical-bitmap-index/).  We
include in `lib` the amalgamated (git `2dc8070`) and well-known
[Roaring Bitmaps](https://github.com/RoaringBitmap/CRoaring/tree/master) and
use it in the soak test to ensure our results are as accurate as theirs.

This library was originally created by [Christoph Rupp](https://crupp.de) in
C++ and then translated to C and further improved by Greg Burd <greg@burd.me>
for use in LMDB and OpenLDAP.
