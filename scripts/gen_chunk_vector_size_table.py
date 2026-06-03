#!/usr/bin/env python

# Gererate a C function that contains a pre-calculated static table where each
# 8bit offset into that table encodes the required additional space for what is
# described.

# 2 bit patterns:
#  00 -> 0 additional sm_bitvec_t (ZEROS)
#  11 -> 0 additional sm_bitvec_t (ONES)
#  10 -> 1 additional sm_bitvec_t (MIXED)
#  01 -> 0 additional sm_bitvec_t (NONE)
#
# A byte has FOUR such pairs.  Loop iterates 0..3 (range(4)).

# The goal is to output this:

# /**
#  * Calculates the number of sm_bitvec_ts required by a single byte with flags
#  * (in m_data[0]).
#  */
# static size_t
# __sm_chunk_calc_vector_size(uint8_t b)
# {
#   // clang-format off
#   static int lookup[] = {
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     2,  2,  3,  2,  2,  2,  3,  2,  3,  3,  4,  3,  2,  2,  3,  2,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0,
#     1,  1,  2,  1,  1,  1,  2,  1,  2,  2,  3,  2,  1,  1,  2,  1,
#     0,  0,  1,  0,  0,  0,  1,  0,  1,  1,  2,  1,  0,  0,  1,  0
#   };
#   // clang-format on
#   return (size_t)lookup[b];
# }

def create_lookup_table_c_format():
  """Creates a lookup table in C-style format.

  For each byte b in 0..255, count the number of two-bit pairs whose
  pattern is exactly '10' (SM_PAYLOAD_MIXED) — that's how many extra
  __sm_bitvec_t words a chunk with that flag byte needs.

  A byte has FOUR two-bit pairs (positions 0-1, 2-3, 4-5, 6-7).  An
  earlier version of this script used range(3) and undercounted the
  high pair; the inlined table in sm.c is the correct
  one.  scripts/check_chunk_vector_size_table.sh enforces the match.
  """
  lookup_table = []
  for byte in range(256):
    count = 0
    for i in range(4):
      if (byte >> (i * 2)) & 3 == 2:
        count += 1
    lookup_table.append(count)

  # Format the output as a C array
  output = "static int lookup[] = {\n"
  for i in range(0, 256, 16):
    line = "  " + ", ".join(str(x) for x in lookup_table[i:i+16]) + ",\n"
    output += line
  output += "};"

  print(output)

if __name__ == "__main__":
  create_lookup_table_c_format()
