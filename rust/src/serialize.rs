//! Wire-compatible serialization with the C sparsemap library.
//!
//! The format is a 16-byte header followed by the body the C library
//! keeps in memory:
//!
//! ```text
//! offset  size  field
//!   0      4    magic       0x30316d73  ("sm10", little-endian)
//!   4      1    version     2
//!   5      1    flags       bit0 = 1 if the body is little-endian
//!   6      2    reserved    0
//!   8      8    cardinality hint (recomputed on read)
//!  16      8    chunk-count header (u64 little-endian)
//!  24    ...    chunks
//! ```
//!
//! Each chunk is `[u64 start][u64 descriptor][u64 payload...]`.  A
//! descriptor whose top two bits are `01` is a run-length chunk
//! (capacity in bits 61:31, length in bits 30:0); otherwise it is a
//! sparse chunk of thirty-two 2-bit flags (`00` zero, `11` one, `10`
//! mixed-with-payload), least-significant slot first.
//!
//! Chunk starts are 64-bit, so the format addresses the full 64-bit
//! universe the public API advertises.  Reading honors the body's
//! declared endianness, so a buffer written on a big-endian host can
//! be read on a little-endian one.
//!
//! Version 2 of the wire format (sparsemap 4.0.0+).  Earlier 4-byte-
//! start v1 buffers are not read; consumers should re-serialize through
//! the C 4.0.0 (or later) library, which produces v2.
//!
//! The chunk-count header is a full 64-bit value (sparsemap 5.1.0+).
//! It was a u32 in the low 4 bytes of the 8-byte slot through 5.0,
//! with the high 4 bytes always zero; reading and writing the whole
//! slot as u64 is byte-identical for any count < 2^32 (every real
//! map), so v2 buffers stay mutually readable across 5.0 and 5.1.
//! The wider count removes the 2^32-chunk ceiling.

use crate::{Chunk, SparseMap, BITS_PER_WORD, CHUNK_BITS, WORDS_PER_CHUNK};
use alloc::boxed::Box;
use alloc::vec::Vec;

const MAGIC: u32 = 0x3031_6d73;
const VERSION: u8 = 2;
const HEADER_LEN: usize = 16;
const FLAG_LE: u8 = 0x01;
/* On-disk overhead (chunk-count header and per-chunk start width) is
 * 8 bytes, matching the C library's SM_SIZEOF_OVERHEAD =
 * sizeof(uint64_t).  The count occupies the full 8-byte header as a
 * little-endian u64 (sparsemap 5.1.0+; wire-compatible with the
 * earlier u32-in-low-4-bytes encoding because the high bytes were
 * always zero). */
const OVERHEAD: usize = 8;
const RLE_FLAG_BITS: u64 = 0b01 << 62;
const RLE_FLAG_MASK: u64 = 0b11 << 62;
const RLE_MAX_SPAN: u64 = 0x7FFF_FFFF; // 31-bit cap/len fields

/// Error returned by [`SparseMap::from_bytes`] for malformed input.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum DecodeError {
    /// The buffer is shorter than a valid header + body.
    TooShort,
    /// The magic number did not match.
    BadMagic,
    /// The version byte is not understood.
    UnsupportedVersion(u8),
    /// A chunk's declared size runs past the end of the buffer, or the
    /// chunk starts are not strictly increasing.
    Corrupt,
}

impl core::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            DecodeError::TooShort => f.write_str("buffer too short"),
            DecodeError::BadMagic => f.write_str("bad magic"),
            DecodeError::UnsupportedVersion(v) => {
                write!(f, "unsupported version {v}")
            }
            DecodeError::Corrupt => f.write_str("corrupt chunk stream"),
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for DecodeError {}

/// Classify a word for the sparse flag encoding.
fn flag_of(word: u64) -> u64 {
    match word {
        0 => 0b00,
        u64::MAX => 0b11,
        _ => 0b10,
    }
}

impl SparseMap {
    /// Serializes the map into the C-compatible wire format (version 2:
    /// 8-byte chunk-start offsets, addressing the full 64-bit universe).
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&MAGIC.to_le_bytes());
        out.push(VERSION);
        out.push(FLAG_LE);
        out.extend_from_slice(&[0, 0]); // reserved
        out.extend_from_slice(&self.cardinality().to_le_bytes());

        // Body: 8-byte chunk-count header (little-endian u64), then
        // chunks.
        let count_pos = out.len();
        out.extend_from_slice(&[0u8; OVERHEAD]);

        let mut count: u64 = 0;
        for (&base, chunk) in &self.chunks {
            match chunk {
                Chunk::Dense(w) => {
                    write_sparse_chunk(&mut out, base, w);
                    count += 1;
                }
                Chunk::Run(n) => {
                    // One RLE chunk per <=2^31-bit span.
                    let mut remaining = u64::from(*n) * CHUNK_BITS;
                    let mut start = base;
                    while remaining > 0 {
                        let span = remaining.min(RLE_MAX_SPAN & !(CHUNK_BITS - 1));
                        write_rle_chunk(&mut out, start, span, span);
                        // The final add may reach 2^64 for a run that
                        // touches the top of the universe; the wrapped
                        // value is never read because the loop then ends.
                        start = start.wrapping_add(span);
                        remaining -= span;
                        count += 1;
                    }
                }
            }
        }
        out[count_pos..count_pos + OVERHEAD].copy_from_slice(&count.to_le_bytes());
        out
    }

    /// Deserializes a buffer produced by [`SparseMap::to_bytes`] or by
    /// the C `sm_serialize`.
    ///
    /// # Errors
    ///
    /// Returns a [`DecodeError`] for any malformed input rather than
    /// panicking; arbitrary bytes are safe to feed in.
    pub fn from_bytes(buf: &[u8]) -> Result<SparseMap, DecodeError> {
        if buf.len() < HEADER_LEN + OVERHEAD {
            return Err(DecodeError::TooShort);
        }
        let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
        if magic != MAGIC {
            return Err(DecodeError::BadMagic);
        }
        let version = buf[4];
        if version != VERSION {
            return Err(DecodeError::UnsupportedVersion(version));
        }
        let le = buf[5] & FLAG_LE != 0;

        let body = &buf[HEADER_LEN..];
        let count = read_u64(body, 0, le).ok_or(DecodeError::Corrupt)?;
        // Chunks begin after the 8-byte chunk-count header.
        let mut pos = OVERHEAD;

        let mut chunks = alloc::collections::BTreeMap::new();
        // Run-coalescing builder state.
        let mut run_base = 0u64;
        let mut run_len = 0u64;
        // Inclusive index of the last bit the previous chunk's span
        // claimed; the next chunk must start strictly above it (no
        // overlap).  Tracked inclusively so a chunk touching the top of
        // the universe (last bit == u64::MAX) needs no 2^64 sentinel.
        // `None` before the first chunk.
        let mut prev_last: Option<u64> = None;

        for _ in 0..count {
            let start = read_u64(body, pos, le).ok_or(DecodeError::Corrupt)?;
            // Chunk starts must be aligned to the window width, exactly
            // as the C library emits them; an unaligned start cannot be
            // reproduced on round-trip.
            if start % CHUNK_BITS != 0 {
                return Err(DecodeError::Corrupt);
            }
            // Strictly ascending, and no overlap with the previous
            // chunk's claimed span.
            if let Some(last) = prev_last {
                if start <= last {
                    return Err(DecodeError::Corrupt);
                }
            }
            pos += 8;
            let desc = read_u64(body, pos, le).ok_or(DecodeError::Corrupt)?;
            pos += 8;

            if desc & RLE_FLAG_MASK == RLE_FLAG_BITS {
                // RLE: capacity in bits 61:31, length in bits 30:0.  The
                // chunk claims `cap` bits [start, start+cap); the first
                // `len` of them are set.
                let len = desc & RLE_MAX_SPAN;
                let cap = (desc >> 31) & RLE_MAX_SPAN;
                // A run cannot be longer than its capacity, its capacity
                // must be positive, and its last claimed bit must not run
                // off the end of the universe.
                if len > cap || cap == 0 {
                    return Err(DecodeError::Corrupt);
                }
                let last = start.checked_add(cap - 1).ok_or(DecodeError::Corrupt)?;
                prev_last = Some(last);
                // len <= cap and start + (cap-1) did not overflow, so
                // every add/mul on `start` below stays in range.
                let full = len / CHUNK_BITS;
                if full > 0 {
                    push_run(&mut chunks, &mut run_base, &mut run_len, start, full);
                }
                let rem = len % CHUNK_BITS;
                if rem > 0 {
                    // Flush any pending run first.
                    if run_len > 0 {
                        chunks.insert(run_base, Chunk::Run(run_len as u32));
                        run_len = 0;
                    }
                    let pbase = start + full * CHUNK_BITS;
                    let mut w = Box::new([0u64; WORDS_PER_CHUNK]);
                    fill_prefix(&mut w, rem as usize);
                    insert_window(&mut chunks, &mut run_base, &mut run_len, pbase, *w);
                }
            } else {
                // Sparse chunk claims exactly one window; its last bit is
                // start + CHUNK_BITS - 1 (== u64::MAX for the top
                // window, which is valid and must round-trip).
                let last = start.checked_add(CHUNK_BITS - 1).ok_or(DecodeError::Corrupt)?;
                prev_last = Some(last);
                // Sparse: 32 flags, payload word per mixed slot.
                let mut w = [0u64; WORDS_PER_CHUNK];
                for (i, slot) in w.iter_mut().enumerate() {
                    match (desc >> (2 * i)) & 0b11 {
                        0b00 | 0b01 => *slot = 0,
                        0b11 => *slot = u64::MAX,
                        _ => {
                            *slot = read_u64(body, pos, le).ok_or(DecodeError::Corrupt)?;
                            pos += 8;
                        }
                    }
                }
                insert_window(&mut chunks, &mut run_base, &mut run_len, start, w);
            }
        }
        if run_len > 0 {
            chunks.insert(run_base, Chunk::Run(run_len as u32));
        }
        Ok(SparseMap { chunks })
    }
}

/// Extend or start a run of `span` all-ones windows at `base`,
/// flushing any non-adjacent pending run.
fn push_run(
    chunks: &mut alloc::collections::BTreeMap<u64, Chunk>,
    run_base: &mut u64,
    run_len: &mut u64,
    base: u64,
    span: u64,
) {
    // The pending run ends at `run_base + run_len*CHUNK_BITS`; that sum
    // never overflows here because `from_bytes` rejects any chunk whose
    // span reaches 2^64, but use a checked compare so the invariant is
    // explicit rather than assumed.
    let contiguous = *run_len > 0 && run_base.checked_add(*run_len * CHUNK_BITS) == Some(base);
    if contiguous {
        *run_len += span;
    } else {
        if *run_len > 0 {
            chunks.insert(*run_base, Chunk::Run(*run_len as u32));
        }
        *run_base = base;
        *run_len = span;
    }
}

/// Push a fully decoded window, coalescing runs and canonicalizing.
fn insert_window(
    chunks: &mut alloc::collections::BTreeMap<u64, Chunk>,
    run_base: &mut u64,
    run_len: &mut u64,
    base: u64,
    w: [u64; WORDS_PER_CHUNK],
) {
    if w.iter().all(|&x| x == 0) {
        return;
    }
    if w.iter().all(|&x| x == u64::MAX) {
        push_run(chunks, run_base, run_len, base, 1);
        return;
    }
    if *run_len > 0 {
        chunks.insert(*run_base, Chunk::Run(*run_len as u32));
        *run_len = 0;
    }
    chunks.insert(base, Chunk::Dense(Box::new(w)));
}

fn fill_prefix(w: &mut [u64; WORDS_PER_CHUNK], bits: usize) {
    let full = bits / 64;
    for slot in w.iter_mut().take(full) {
        *slot = u64::MAX;
    }
    let rem = bits % 64;
    if rem > 0 {
        w[full] = (1u64 << rem) - 1;
    }
}

fn write_sparse_chunk(out: &mut Vec<u8>, start: u64, w: &[u64; WORDS_PER_CHUNK]) {
    let mut desc = 0u64;
    for (i, &word) in w.iter().enumerate() {
        desc |= flag_of(word) << (2 * i);
    }
    out.extend_from_slice(&start.to_le_bytes());
    out.extend_from_slice(&desc.to_le_bytes());
    for &word in w {
        if flag_of(word) == 0b10 {
            out.extend_from_slice(&word.to_le_bytes());
        }
    }
}

fn write_rle_chunk(out: &mut Vec<u8>, start: u64, cap: u64, len: u64) {
    let desc = RLE_FLAG_BITS | ((cap & RLE_MAX_SPAN) << 31) | (len & RLE_MAX_SPAN);
    out.extend_from_slice(&start.to_le_bytes());
    out.extend_from_slice(&desc.to_le_bytes());
}

fn read_u64(b: &[u8], at: usize, le: bool) -> Option<u64> {
    let s = b.get(at..at + 8)?;
    let a: [u8; 8] = s.try_into().unwrap();
    Some(if le {
        u64::from_le_bytes(a)
    } else {
        u64::from_be_bytes(a)
    })
}

const _: () = assert!(BITS_PER_WORD == 64);

#[cfg(test)]
mod hostile {
    //! Structural-validation tests: hostile serialized buffers must be
    //! rejected with [`DecodeError::Corrupt`], not decoded into a
    //! corrupt map (that iterates out of order, fails round-trip, or
    //! overflows on iteration).  These reproduce the 2026-09 fuzzing
    //! findings; each one decoded `Ok(..)` (or panicked) before the
    //! overflow/validation hardening.
    use super::*;
    use crate::SparseMap;
    use alloc::vec::Vec;

    const SPAN31: u64 = 0x7FFF_FFFF;

    fn header(count: u64) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&MAGIC.to_le_bytes());
        b.push(VERSION);
        b.push(FLAG_LE);
        b.extend_from_slice(&[0, 0]);
        b.extend_from_slice(&0u64.to_le_bytes()); // cardinality hint (recomputed)
        b.extend_from_slice(&count.to_le_bytes());
        b
    }

    fn chunk(b: &mut Vec<u8>, start: u64, desc: u64) {
        b.extend_from_slice(&start.to_le_bytes());
        b.extend_from_slice(&desc.to_le_bytes());
    }

    fn rle_desc(cap: u64, len: u64) -> u64 {
        RLE_FLAG_BITS | ((cap & SPAN31) << 31) | (len & SPAN31)
    }

    /// A sparse descriptor with every slot ONES (`0b11`).
    fn sparse_all_ones() -> u64 {
        let mut d = 0u64;
        for i in 0..WORDS_PER_CHUNK {
            d |= 0b11u64 << (2 * i);
        }
        d
    }

    #[test]
    fn rejects_rle_length_exceeding_capacity() {
        let mut b = header(1);
        chunk(&mut b, 0, rle_desc(CHUNK_BITS, CHUNK_BITS * 2));
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_unaligned_chunk_start() {
        let mut b = header(1);
        chunk(&mut b, 100, rle_desc(CHUNK_BITS, CHUNK_BITS));
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_start_plus_capacity_overflow() {
        // A run whose last claimed bit runs past u64::MAX.
        let mut b = header(1);
        chunk(&mut b, u64::MAX - (CHUNK_BITS - 1), rle_desc(CHUNK_BITS * 2, CHUNK_BITS * 2));
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_overlapping_chunk_spans() {
        // Chunk 0 claims [0, 4096); chunk 1 starts at 2048, inside it.
        let mut b = header(2);
        chunk(&mut b, 0, rle_desc(CHUNK_BITS * 2, CHUNK_BITS * 2));
        chunk(&mut b, CHUNK_BITS, sparse_all_ones());
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_non_ascending_starts() {
        let mut b = header(2);
        chunk(&mut b, CHUNK_BITS, sparse_all_ones());
        chunk(&mut b, 0, sparse_all_ones());
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_duplicate_starts() {
        let mut b = header(2);
        chunk(&mut b, CHUNK_BITS, sparse_all_ones());
        chunk(&mut b, CHUNK_BITS, sparse_all_ones());
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn rejects_zero_capacity_rle() {
        let mut b = header(1);
        chunk(&mut b, 0, rle_desc(0, 0));
        assert_eq!(SparseMap::from_bytes(&b), Err(DecodeError::Corrupt));
    }

    #[test]
    fn accepts_single_bit_in_top_window() {
        // A sparse chunk at the top window claims [2^64-2048, 2^64):
        // valid, and its span's last bit is exactly u64::MAX.
        let base = u64::MAX - (CHUNK_BITS - 1);
        let mut m = SparseMap::new();
        m.insert(base);
        let back = SparseMap::from_bytes(&m.to_bytes()).expect("top-window bit is valid");
        assert_eq!(back, m);
    }

    #[test]
    fn accepts_full_top_window_run() {
        // A full top window promotes to a run ending at 2^64; it must
        // decode, iterate ascending, and round-trip.
        let base = u64::MAX - (CHUNK_BITS - 1);
        let mut m = SparseMap::new();
        for off in 0..CHUNK_BITS {
            m.insert(base + off);
        }
        let back = SparseMap::from_bytes(&m.to_bytes()).expect("top-window run is valid");
        assert_eq!(back, m);
        assert_eq!(back.max(), Some(u64::MAX));
        assert_eq!(back.cardinality(), CHUNK_BITS);
        let bits: Vec<u64> = back.iter().collect();
        assert!(bits.windows(2).all(|w| w[0] < w[1]), "iterates ascending");
    }

    #[test]
    fn decoded_maps_are_ascending_and_stable() {
        // Every buffer either rejects or yields a map that iterates
        // strictly ascending and round-trips unchanged.  A spread of
        // hostile descriptors that used to decode into corrupt maps.
        let base_top = u64::MAX - (CHUNK_BITS - 1);
        let mut bufs: Vec<Vec<u8>> = Vec::new();
        for &(start, desc) in &[
            (0u64, rle_desc(CHUNK_BITS, CHUNK_BITS * 2)),
            (base_top, rle_desc(SPAN31, SPAN31)),
            (0, rle_desc(SPAN31, SPAN31)),
            (CHUNK_BITS, sparse_all_ones()),
            (base_top, sparse_all_ones()),
        ] {
            let mut b = header(1);
            chunk(&mut b, start, desc);
            bufs.push(b);
        }
        for buf in bufs {
            if let Ok(m) = SparseMap::from_bytes(&buf) {
                let bits: Vec<u64> = m.iter().take(8192).collect();
                assert!(
                    bits.windows(2).all(|w| w[0] < w[1]),
                    "decoded map iterates non-ascending"
                );
                let re = SparseMap::from_bytes(&m.to_bytes());
                assert_eq!(re.as_ref(), Ok(&m), "decoded map is not round-trip stable");
            }
        }
    }
}
