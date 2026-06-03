//! Wire-compatible serialization with the C sparsemap library.
//!
//! The format is a 16-byte header followed by the body the C library
//! keeps in memory:
//!
//! ```text
//! offset  size  field
//!   0      4    magic       0x30316d73  ("sm10", little-endian)
//!   4      1    version     1
//!   5      1    flags       bit0 = 1 if the body is little-endian
//!   6      2    reserved    0
//!   8      8    cardinality hint (recomputed on read)
//!  16      4    chunk count
//!  20    ...    chunks
//! ```
//!
//! Each chunk is `[u32 start][u64 descriptor][u64 payload...]`.  A
//! descriptor whose top two bits are `01` is a run-length chunk
//! (capacity in bits 61:31, length in bits 30:0); otherwise it is a
//! sparse chunk of thirty-two 2-bit flags (`00` zero, `11` one, `10`
//! mixed-with-payload), least-significant slot first.
//!
//! Because chunk starts are 32-bit, the serialized format addresses the
//! `[0, 2^32)` universe, exactly as the C library does.  Reading honors
//! the body's declared endianness, so a buffer written on a big-endian
//! host can be read on a little-endian one (an improvement over the C
//! reader, which rejects the cross-endian case).

use crate::{Chunk, SparseMap, BITS_PER_WORD, CHUNK_BITS, WORDS_PER_CHUNK};
use alloc::boxed::Box;
use alloc::vec::Vec;

const MAGIC: u32 = 0x3031_6d73;
const VERSION: u8 = 1;
const HEADER_LEN: usize = 16;
const FLAG_LE: u8 = 0x01;
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

/// Error returned by [`SparseMap::to_bytes`].
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum EncodeError {
    /// A set bit is at or above 2^32 and so cannot be addressed by the
    /// 32-bit on-wire chunk start.
    IndexTooLarge(u64),
}

impl core::fmt::Display for EncodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            EncodeError::IndexTooLarge(i) => {
                write!(f, "index {i} exceeds the 2^32 wire universe")
            }
        }
    }
}

#[cfg(feature = "std")]
impl std::error::Error for EncodeError {}

/// Classify a word for the sparse flag encoding.
fn flag_of(word: u64) -> u64 {
    match word {
        0 => 0b00,
        u64::MAX => 0b11,
        _ => 0b10,
    }
}

impl SparseMap {
    /// Serializes the map into the C-compatible wire format.
    ///
    /// # Errors
    ///
    /// Returns [`EncodeError::IndexTooLarge`] if the map contains a bit
    /// at or above `2^32`, which the 32-bit on-wire chunk start cannot
    /// represent (the C library has the same limit).
    pub fn to_bytes(&self) -> Result<Vec<u8>, EncodeError> {
        if let Some(mx) = self.max() {
            if mx >= 1u64 << 32 {
                return Err(EncodeError::IndexTooLarge(mx));
            }
        }

        let mut out = Vec::new();
        out.extend_from_slice(&MAGIC.to_le_bytes());
        out.push(VERSION);
        out.push(FLAG_LE);
        out.extend_from_slice(&[0, 0]); // reserved
        out.extend_from_slice(&self.cardinality().to_le_bytes());

        // Body: chunk count placeholder, then chunks.
        let count_pos = out.len();
        out.extend_from_slice(&0u32.to_le_bytes());

        let mut count: u32 = 0;
        for (&base, chunk) in &self.chunks {
            match chunk {
                Chunk::Dense(w) => {
                    write_sparse_chunk(&mut out, base as u32, w);
                    count += 1;
                }
                Chunk::Run(n) => {
                    // One RLE chunk per <=2^31-bit span.
                    let mut remaining = u64::from(*n) * CHUNK_BITS;
                    let mut start = base;
                    while remaining > 0 {
                        let span = remaining.min(RLE_MAX_SPAN & !(CHUNK_BITS - 1));
                        write_rle_chunk(&mut out, start as u32, span, span);
                        start += span;
                        remaining -= span;
                        count += 1;
                    }
                }
            }
        }
        out[count_pos..count_pos + 4].copy_from_slice(&count.to_le_bytes());
        Ok(out)
    }

    /// Deserializes a buffer produced by [`SparseMap::to_bytes`] or by
    /// the C `sm_serialize`.
    ///
    /// # Errors
    ///
    /// Returns a [`DecodeError`] for any malformed input rather than
    /// panicking; arbitrary bytes are safe to feed in.
    pub fn from_bytes(buf: &[u8]) -> Result<SparseMap, DecodeError> {
        if buf.len() < HEADER_LEN + 4 {
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
        let count = read_u32(body, 0, le).ok_or(DecodeError::Corrupt)?;
        let mut pos = 4usize;

        let mut chunks = alloc::collections::BTreeMap::new();
        // Run-coalescing builder state.
        let mut run_base = 0u64;
        let mut run_len = 0u64;
        let mut prev_start: Option<u64> = None;

        for _ in 0..count {
            let start = u64::from(read_u32(body, pos, le).ok_or(DecodeError::Corrupt)?);
            if let Some(p) = prev_start {
                if start <= p {
                    return Err(DecodeError::Corrupt);
                }
            }
            prev_start = Some(start);
            pos += 4;
            let desc = read_u64(body, pos, le).ok_or(DecodeError::Corrupt)?;
            pos += 8;

            if desc & RLE_FLAG_MASK == RLE_FLAG_BITS {
                // RLE: bits [start, start+len) set.
                let len = desc & RLE_MAX_SPAN;
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
    if *run_len > 0 && *run_base + *run_len * CHUNK_BITS == base {
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

fn write_sparse_chunk(out: &mut Vec<u8>, start: u32, w: &[u64; WORDS_PER_CHUNK]) {
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

fn write_rle_chunk(out: &mut Vec<u8>, start: u32, cap: u64, len: u64) {
    let desc = RLE_FLAG_BITS | ((cap & RLE_MAX_SPAN) << 31) | (len & RLE_MAX_SPAN);
    out.extend_from_slice(&start.to_le_bytes());
    out.extend_from_slice(&desc.to_le_bytes());
}

fn read_u32(b: &[u8], at: usize, le: bool) -> Option<u32> {
    let s = b.get(at..at + 4)?;
    let a: [u8; 4] = s.try_into().unwrap();
    Some(if le {
        u32::from_le_bytes(a)
    } else {
        u32::from_be_bytes(a)
    })
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
