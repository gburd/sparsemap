#!/usr/bin/env python

# Gererate a C function that contains a pre-calculated static table where each
# 8bit offset into that table encodes the required additional space for what is
# described.

# The 2 bit patters are:
#  00 -> 0 additional sm_bitvec_t
#  11 -> 0 additional sm_bitvec_t
#  10 -> 1 additional sm_bitvec_t
#  01 -> 1 additional sm_bitvec_t

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

# TODO...
