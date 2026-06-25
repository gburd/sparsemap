//! A sparse, compressed bitmap (bitset) over `u64` indices, optimized
//! for workloads with long runs of consecutive set bits.
//!
//! `sparsemap` is the Rust port of the C
//! [sparsemap](https://codeberg.org/gregburd/sparsemap) library.  It is
//! functionally identical: the same set semantics, the same compression
//! behavior, and a **wire-compatible** serialized format, so a map
//! written by either implementation can be read by the other.
//!
//! # Model
//!
//! The universe is divided into fixed 2048-bit *windows*.  Each window
//! that contains data is stored as one of two chunk kinds:
//!
//! * a **run** of one or more entirely-set windows, stored in `O(1)`
//!   space regardless of length — a run of two billion set bits is a
//!   handful of bytes; and
//! * a **dense** window of 2048 bits (32 × [`u64`]) for mixed regions,
//!   costing one bit per bit.
//!
//! Empty windows are not stored at all.  This makes `sparsemap` cheap
//! for the two regimes that matter — long runs and sparse clusters —
//! while degrading gracefully to a plain bitmap for random data.
//!
//! # Example
//!
//! ```
//! use sparsemap::SparseMap;
//!
//! let mut m = SparseMap::new();
//! m.insert(42);
//! m.insert(1024);
//! assert!(m.contains(42));
//! assert_eq!(m.cardinality(), 2);
//!
//! // Idiomatic set algebra.
//! let a: SparseMap = (0..100).collect();
//! let b: SparseMap = (50..150).collect();
//! assert_eq!((&a & &b).cardinality(), 50);
//! assert_eq!((&a | &b).cardinality(), 150);
//!
//! // Ascending iteration.
//! let evens: SparseMap = (0..10).filter(|n| n % 2 == 0).collect();
//! assert_eq!(evens.iter().collect::<Vec<_>>(), vec![0, 2, 4, 6, 8]);
//! ```
//!
//! # Thread safety
//!
//! `SparseMap` is `Send + Sync` and follows the usual Rust borrowing
//! rules: shared references allow concurrent reads, a unique reference
//! is required to mutate.  There is no interior mutability.
//!
//! # `no_std`
//!
//! The crate is `#![no_std]` and depends only on `alloc`.

#![cfg_attr(not(feature = "std"), no_std)]
#![forbid(unsafe_code)]
#![cfg_attr(docsrs, feature(doc_cfg))]
// Domain-justified relaxations of clippy::pedantic.  A compressed bitmap
// is full of deliberate, bounds-checked width casts (chunk starts and
// run counts are < 2^32 by construction; bit offsets are < 2048), and a
// few internal `expect()`s assert structural invariants that cannot
// fail and are not part of the public contract.
#![allow(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    clippy::cast_possible_wrap,
    clippy::missing_panics_doc,
    clippy::items_after_statements
)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::collections::BTreeMap;

mod iter;
mod ops;
mod serialize;

pub use iter::Iter;
pub use serialize::DecodeError;

/// Bits in a machine word.
const BITS_PER_WORD: u64 = 64;
/// Words in a dense window.
const WORDS_PER_CHUNK: usize = 32;
/// Bits per window: 64 × 32 = 2048.
const CHUNK_BITS: u64 = BITS_PER_WORD * WORDS_PER_CHUNK as u64;

/// The 2048-aligned base of the window containing `idx`.
#[inline]
const fn window_base(idx: u64) -> u64 {
    idx & !(CHUNK_BITS - 1)
}

/// Offset of `idx` within its window, in `[0, 2048)`.
#[inline]
const fn window_offset(idx: u64) -> usize {
    (idx & (CHUNK_BITS - 1)) as usize
}

/// A populated 2048-bit window.
///
/// Maintained in canonical form: a `Dense` chunk is never all-zero
/// (absent instead) and never all-one (promoted to a `Run`), and
/// adjacent `Run`s are always merged.  Canonical form makes equal bit
/// sets compare equal structurally.
#[derive(Clone, PartialEq, Eq, Hash)]
enum Chunk {
    /// `count` consecutive entirely-set windows beginning at the key,
    /// covering `count * 2048` bits.  `count >= 1`.
    Run(u32),
    /// A single window stored as 32 words, least-significant word first;
    /// within a word, bit `b` is `1 << b`.
    Dense(Box<[u64; WORDS_PER_CHUNK]>),
}

impl Chunk {
    /// Number of set bits in this chunk.
    #[inline]
    fn count(&self) -> u64 {
        match self {
            Chunk::Run(n) => u64::from(*n) * CHUNK_BITS,
            Chunk::Dense(w) => w.iter().map(|x| u64::from(x.count_ones())).sum(),
        }
    }
}

/// A sparse, compressed, run-length-encoded bitset over `u64` indices.
///
/// See the [crate-level documentation](crate) for the data model.
#[derive(Clone, Default, PartialEq, Eq, Hash)]
pub struct SparseMap {
    /// Window base → chunk, ordered by base.  Chunks never overlap.
    pub(crate) chunks: BTreeMap<u64, Chunk>,
}

impl SparseMap {
    /// Creates an empty map.
    #[must_use]
    pub const fn new() -> Self {
        SparseMap {
            chunks: BTreeMap::new(),
        }
    }

    /// Returns `true` if the map contains no set bits.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.chunks.is_empty()
    }

    /// Removes all bits, retaining no allocation.
    pub fn clear(&mut self) {
        self.chunks.clear();
    }

    /// Returns the number of set bits (the cardinality).
    ///
    /// Runs are counted in `O(1)`, so this is cheap even for maps
    /// dominated by long runs.
    #[must_use]
    pub fn cardinality(&self) -> u64 {
        self.chunks.values().map(Chunk::count).sum()
    }

    /// Returns the number of set bits.
    ///
    /// An alias for [`cardinality`](Self::cardinality), named to match
    /// the Rust collection convention (and `roaring::RoaringBitmap`),
    /// so the type drops in for code written against those APIs.
    #[must_use]
    pub fn len(&self) -> u64 {
        self.cardinality()
    }

    /// Returns `true` if bit `idx` is set.
    #[must_use]
    pub fn contains(&self, idx: u64) -> bool {
        // The covering chunk, if any, is the one with the greatest base
        // <= idx.
        if let Some((&base, chunk)) = self.chunks.range(..=idx).next_back() {
            match chunk {
                Chunk::Run(n) => idx < base + u64::from(*n) * CHUNK_BITS,
                Chunk::Dense(w) => {
                    base == window_base(idx) && {
                        let o = window_offset(idx);
                        w[o / 64] & (1u64 << (o % 64)) != 0
                    }
                }
            }
        } else {
            false
        }
    }

    /// Sets bit `idx`.  Returns `true` if the bit was newly set,
    /// `false` if it was already set (mirroring [`alloc::collections::BTreeSet::insert`]).
    pub fn insert(&mut self, idx: u64) -> bool {
        let base = window_base(idx);

        // Already covered by a run?
        if let Some((&rbase, Chunk::Run(n))) = self.chunks.range(..=idx).next_back() {
            if idx < rbase + u64::from(*n) * CHUNK_BITS {
                return false;
            }
        }

        match self.chunks.get_mut(&base) {
            Some(Chunk::Run(_)) => false, // covered above; unreachable in practice
            Some(Chunk::Dense(w)) => {
                let o = window_offset(idx);
                let mask = 1u64 << (o % 64);
                if w[o / 64] & mask != 0 {
                    return false;
                }
                w[o / 64] |= mask;
                if w.iter().all(|x| *x == u64::MAX) {
                    self.chunks.insert(base, Chunk::Run(1));
                    self.coalesce_runs(base);
                }
                true
            }
            None => {
                let mut w = Box::new([0u64; WORDS_PER_CHUNK]);
                let o = window_offset(idx);
                w[o / 64] |= 1u64 << (o % 64);
                self.chunks.insert(base, Chunk::Dense(w));
                true
            }
        }
    }

    /// Clears bit `idx`.  Returns `true` if the bit had been set.
    pub fn remove(&mut self, idx: u64) -> bool {
        let base = window_base(idx);

        // Is idx inside a run?  (The covering chunk is the floor entry.)
        if let Some((&rbase, Chunk::Run(n))) = self.chunks.range(..=idx).next_back() {
            let n = u64::from(*n);
            if idx < rbase + n * CHUNK_BITS {
                self.split_run(rbase, n, base, idx);
                return true;
            }
        }

        match self.chunks.get_mut(&base) {
            Some(Chunk::Dense(w)) => {
                let o = window_offset(idx);
                let mask = 1u64 << (o % 64);
                if w[o / 64] & mask == 0 {
                    return false;
                }
                w[o / 64] &= !mask;
                if w.iter().all(|x| *x == 0) {
                    self.chunks.remove(&base);
                }
                true
            }
            _ => false,
        }
    }

    /// Splits a run `[rbase, rbase + n*2048)` because bit `idx` (in
    /// window `wbase`) is being cleared.  Produces up to a leading run,
    /// a dense window with that one bit clear, and a trailing run.
    fn split_run(&mut self, rbase: u64, n: u64, wbase: u64, idx: u64) {
        self.chunks.remove(&rbase);

        let lead = (wbase - rbase) / CHUNK_BITS;
        if lead > 0 {
            self.chunks.insert(rbase, Chunk::Run(lead as u32));
        }

        // The opened window: all bits set except idx.
        let mut w = Box::new([u64::MAX; WORDS_PER_CHUNK]);
        let o = window_offset(idx);
        w[o / 64] &= !(1u64 << (o % 64));
        self.chunks.insert(wbase, Chunk::Dense(w));

        let trail = n - lead - 1;
        if trail > 0 {
            self.chunks
                .insert(wbase + CHUNK_BITS, Chunk::Run(trail as u32));
        }
    }

    /// After promoting the window at `base` to `Run(1)`, merge it with
    /// an immediately-preceding and/or -following run.
    fn coalesce_runs(&mut self, base: u64) {
        // Merge with the next run if it starts where this one ends.
        let this_count = match self.chunks.get(&base) {
            Some(Chunk::Run(n)) => u64::from(*n),
            _ => return,
        };
        let next_base = base + this_count * CHUNK_BITS;
        if let Some(Chunk::Run(nn)) = self.chunks.get(&next_base) {
            let merged = this_count + u64::from(*nn);
            self.chunks.remove(&next_base);
            self.chunks.insert(base, Chunk::Run(merged as u32));
        }

        // Merge with the previous run if it ends where this one starts.
        if let Some((&pbase, Chunk::Run(pn))) = self.chunks.range(..base).next_back() {
            let pn = u64::from(*pn);
            if pbase + pn * CHUNK_BITS == base {
                let this_count = match self.chunks.get(&base) {
                    Some(Chunk::Run(n)) => u64::from(*n),
                    _ => return,
                };
                self.chunks.remove(&base);
                self.chunks
                    .insert(pbase, Chunk::Run((pn + this_count) as u32));
            }
        }
    }

    /// Returns the smallest set bit, or `None` if the map is empty.
    #[must_use]
    pub fn min(&self) -> Option<u64> {
        let (&base, chunk) = self.chunks.iter().next()?;
        Some(match chunk {
            Chunk::Run(_) => base,
            Chunk::Dense(w) => base + dense_first_set(w).expect("dense never empty"),
        })
    }

    /// Returns the largest set bit, or `None` if the map is empty.
    #[must_use]
    pub fn max(&self) -> Option<u64> {
        let (&base, chunk) = self.chunks.iter().next_back()?;
        Some(match chunk {
            Chunk::Run(n) => base + u64::from(*n) * CHUNK_BITS - 1,
            Chunk::Dense(w) => base + dense_last_set(w).expect("dense never empty"),
        })
    }

    /// Returns the number of set bits strictly less than `idx`
    /// (the standard succinct-structure *rank*).
    ///
    /// `rank(0)` is always 0; `rank(u64::MAX) + contains(u64::MAX) as
    /// u64` is the cardinality.
    #[must_use]
    pub fn rank(&self, idx: u64) -> u64 {
        let mut total = 0;
        for (&base, chunk) in &self.chunks {
            if base >= idx {
                break;
            }
            match chunk {
                Chunk::Run(n) => {
                    let end = base + u64::from(*n) * CHUNK_BITS;
                    total += if idx >= end { end - base } else { idx - base };
                }
                Chunk::Dense(w) => {
                    if idx >= base + CHUNK_BITS {
                        total += chunk.count();
                    } else {
                        total += dense_rank(w, (idx - base) as usize);
                    }
                }
            }
        }
        total
    }

    /// Returns the position of the `n`-th set bit (0-based), or `None`
    /// if there are `n` or fewer set bits.
    ///
    /// `select(0)` is [`min`](Self::min).
    #[must_use]
    pub fn select(&self, mut n: u64) -> Option<u64> {
        for (&base, chunk) in &self.chunks {
            let c = chunk.count();
            if n < c {
                return Some(match chunk {
                    Chunk::Run(_) => base + n,
                    Chunk::Dense(w) => base + dense_select(w, n).expect("n < count"),
                });
            }
            n -= c;
        }
        None
    }

    /// Sets every bit in the half-open range `[start, end)`.
    pub fn insert_range(&mut self, start: u64, end: u64) {
        // A whole-window fast path keeps long ranges cheap.
        let mut i = start;
        while i < end {
            let base = window_base(i);
            if i == base && end - i >= CHUNK_BITS && self.window_is_empty(base) {
                // Fill an entire empty window as a run, then coalesce.
                self.chunks.insert(base, Chunk::Run(1));
                self.coalesce_runs(base);
                i += CHUNK_BITS;
            } else {
                self.insert(i);
                i += 1;
            }
        }
    }

    /// Returns `true` if the window based at `base` holds no set bits and
    /// is not covered by an enclosing run.
    fn window_is_empty(&self, base: u64) -> bool {
        match self.chunks.range(..=base).next_back() {
            Some((&rb, Chunk::Run(n))) => base >= rb + u64::from(*n) * CHUNK_BITS,
            Some((&b2, Chunk::Dense(_))) => b2 != base,
            None => true,
        }
    }

    /// Clears every bit in the half-open range `[start, end)`.
    pub fn remove_range(&mut self, start: u64, end: u64) {
        let mut i = start;
        while i < end {
            i += 1;
            self.remove(i - 1);
        }
    }

    /// Returns the start of the first run of at least `len` consecutive
    /// bits all equal to `value`, beginning the search at `start`, or
    /// `None` if there is no such run.
    #[must_use]
    pub fn span(&self, start: u64, len: u64, value: bool) -> Option<u64> {
        if len == 0 {
            return Some(start);
        }
        let mut run_start = start;
        let mut i = start;
        loop {
            if self.contains(i) == value {
                if i - run_start + 1 >= len {
                    return Some(run_start);
                }
            } else {
                run_start = i + 1;
            }
            // Guard against wraparound at the top of the universe.
            if i == u64::MAX {
                return None;
            }
            i += 1;
            // For value==false the span can extend past the last set
            // bit forever; only search a bounded distance past max.
            if !value {
                if let Some(mx) = self.max() {
                    if run_start > mx + 1 && run_start.saturating_sub(start) >= len {
                        return Some(run_start);
                    }
                    if i > mx + len + 1 {
                        return Some(run_start.max(start));
                    }
                } else {
                    return Some(start);
                }
            }
        }
    }
}

/// First set bit within a dense window, relative to its base.
fn dense_first_set(w: &[u64; WORDS_PER_CHUNK]) -> Option<u64> {
    for (i, &word) in w.iter().enumerate() {
        if word != 0 {
            return Some(i as u64 * BITS_PER_WORD + u64::from(word.trailing_zeros()));
        }
    }
    None
}

/// Last set bit within a dense window, relative to its base.
fn dense_last_set(w: &[u64; WORDS_PER_CHUNK]) -> Option<u64> {
    for (i, &word) in w.iter().enumerate().rev() {
        if word != 0 {
            return Some(
                i as u64 * BITS_PER_WORD + (BITS_PER_WORD - 1 - u64::from(word.leading_zeros())),
            );
        }
    }
    None
}

/// Count of set bits in `w` strictly below relative index `bit`.
fn dense_rank(w: &[u64; WORDS_PER_CHUNK], bit: usize) -> u64 {
    let word = bit / 64;
    let mut total = 0u64;
    for &x in &w[..word] {
        total += u64::from(x.count_ones());
    }
    let rem = bit % 64;
    if rem != 0 {
        total += u64::from((w[word] & ((1u64 << rem) - 1)).count_ones());
    }
    total
}

/// Relative index of the `n`-th set bit within `w`.
fn dense_select(w: &[u64; WORDS_PER_CHUNK], mut n: u64) -> Option<u64> {
    for (i, &word) in w.iter().enumerate() {
        let c = u64::from(word.count_ones());
        if n < c {
            // Find the n-th set bit in `word`.
            let mut word = word;
            for _ in 0..n {
                word &= word - 1;
            }
            return Some(i as u64 * BITS_PER_WORD + u64::from(word.trailing_zeros()));
        }
        n -= c;
    }
    None
}

#[cfg(test)]
mod tests;
