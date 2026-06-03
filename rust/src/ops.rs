//! Set algebra, operator overloads, and the standard collection traits.

use crate::{Chunk, SparseMap, CHUNK_BITS, WORDS_PER_CHUNK};
use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use core::cmp::Ordering;
use core::ops::{BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, Sub, SubAssign};

/// The value of a single window during a merge.
#[derive(Clone, Copy)]
enum Win<'a> {
    /// All 2048 bits set (a slice of a run).
    Ones,
    /// A dense window.
    Words(&'a [u64; WORDS_PER_CHUNK]),
}

/// A forward cursor over the populated windows of a map, run-aware.
struct Cursor<'a> {
    iter: alloc::collections::btree_map::Iter<'a, u64, Chunk>,
    base: u64,
    /// For a run, windows remaining (each all-ones); for dense, 1.
    remaining: u64,
    win: Option<Win<'a>>,
}

impl<'a> Cursor<'a> {
    fn new(map: &'a SparseMap) -> Self {
        let mut c = Cursor {
            iter: map.chunks.iter(),
            base: 0,
            remaining: 0,
            win: None,
        };
        c.advance_chunk();
        c
    }

    fn advance_chunk(&mut self) {
        match self.iter.next() {
            Some((&base, Chunk::Run(n))) => {
                self.base = base;
                self.remaining = u64::from(*n);
                self.win = Some(Win::Ones);
            }
            Some((&base, Chunk::Dense(w))) => {
                self.base = base;
                self.remaining = 1;
                self.win = Some(Win::Words(w));
            }
            None => self.win = None,
        }
    }

    /// Base of the current window, or `None` at the end.
    fn peek(&self) -> Option<u64> {
        self.win.map(|_| self.base)
    }

    /// Advance past `k` windows of the current (run) chunk.
    fn advance(&mut self, k: u64) {
        debug_assert!(k >= 1 && k <= self.remaining);
        self.remaining -= k;
        if self.remaining == 0 {
            self.advance_chunk();
        } else {
            self.base += k * CHUNK_BITS;
        }
    }
}

/// Accumulates merged windows (delivered in ascending base order) into a
/// canonical-form map: adjacent runs coalesce, all-zero windows drop,
/// all-one windows become runs.
struct Builder {
    chunks: BTreeMap<u64, Chunk>,
    run_base: u64,
    run_len: u64,
}

impl Builder {
    fn new() -> Self {
        Builder {
            chunks: BTreeMap::new(),
            run_base: 0,
            run_len: 0,
        }
    }

    fn flush_run(&mut self) {
        if self.run_len > 0 {
            self.chunks
                .insert(self.run_base, Chunk::Run(self.run_len as u32));
            self.run_len = 0;
        }
    }

    /// Append `span` all-ones windows starting at `base`.
    fn push_ones(&mut self, base: u64, span: u64) {
        if self.run_len > 0 && self.run_base + self.run_len * CHUNK_BITS == base {
            self.run_len += span;
        } else {
            self.flush_run();
            self.run_base = base;
            self.run_len = span;
        }
    }

    /// Append one dense window's worth of `words` at `base`.
    fn push_words(&mut self, base: u64, words: [u64; WORDS_PER_CHUNK]) {
        if words.iter().all(|&x| x == 0) {
            return;
        }
        if words.iter().all(|&x| x == u64::MAX) {
            self.push_ones(base, 1);
            return;
        }
        self.flush_run();
        self.chunks.insert(base, Chunk::Dense(Box::new(words)));
    }

    fn finish(mut self) -> SparseMap {
        self.flush_run();
        SparseMap {
            chunks: self.chunks,
        }
    }
}

/// Combine two present windows word-by-word with `f`.
fn combine_words<F: Fn(u64, u64) -> u64>(a: Win, b: Win, f: F) -> [u64; WORDS_PER_CHUNK] {
    let mut out = [0u64; WORDS_PER_CHUNK];
    for (i, slot) in out.iter_mut().enumerate() {
        let av = match a {
            Win::Ones => u64::MAX,
            Win::Words(w) => w[i],
        };
        let bv = match b {
            Win::Ones => u64::MAX,
            Win::Words(w) => w[i],
        };
        *slot = f(av, bv);
    }
    out
}

/// Materialize a single window as words (for the one-sided cases).
fn as_words(w: Win) -> [u64; WORDS_PER_CHUNK] {
    match w {
        Win::Ones => [u64::MAX; WORDS_PER_CHUNK],
        Win::Words(words) => *words,
    }
}

impl SparseMap {
    /// Returns the union (`self ∪ other`): bits set in either map.
    #[must_use]
    pub fn union(&self, other: &SparseMap) -> SparseMap {
        let mut a = Cursor::new(self);
        let mut b = Cursor::new(other);
        let mut out = Builder::new();
        loop {
            match (a.peek(), b.peek()) {
                (None, None) => break,
                (Some(ab), None) => {
                    push_side(&mut out, &mut a, ab, u64::MAX);
                }
                (None, Some(bb)) => {
                    push_side(&mut out, &mut b, bb, u64::MAX);
                }
                (Some(ab), Some(bb)) => match ab.cmp(&bb) {
                    Ordering::Less => {
                        push_side(&mut out, &mut a, ab, (bb - ab) / CHUNK_BITS);
                    }
                    Ordering::Greater => {
                        push_side(&mut out, &mut b, bb, (ab - bb) / CHUNK_BITS);
                    }
                    Ordering::Equal => {
                        if both_ones(&a, &b) {
                            let span = a.remaining.min(b.remaining);
                            out.push_ones(ab, span);
                            a.advance(span);
                            b.advance(span);
                        } else {
                            let w = combine_words(a.win.unwrap(), b.win.unwrap(), |x, y| x | y);
                            out.push_words(ab, w);
                            a.advance(1);
                            b.advance(1);
                        }
                    }
                },
            }
        }
        out.finish()
    }

    /// Returns the intersection (`self ∩ other`): bits set in both maps.
    #[must_use]
    pub fn intersection(&self, other: &SparseMap) -> SparseMap {
        let mut a = Cursor::new(self);
        let mut b = Cursor::new(other);
        let mut out = Builder::new();
        while let (Some(ab), Some(bb)) = (a.peek(), b.peek()) {
            if ab < bb {
                a.advance(1);
            } else if bb < ab {
                b.advance(1);
            } else if both_ones(&a, &b) {
                let span = a.remaining.min(b.remaining);
                out.push_ones(ab, span);
                a.advance(span);
                b.advance(span);
            } else {
                let w = combine_words(a.win.unwrap(), b.win.unwrap(), |x, y| x & y);
                out.push_words(ab, w);
                a.advance(1);
                b.advance(1);
            }
        }
        out.finish()
    }

    /// Returns the difference (`self \ other`): bits set in `self` but
    /// not in `other`.
    #[must_use]
    pub fn difference(&self, other: &SparseMap) -> SparseMap {
        let mut a = Cursor::new(self);
        let mut b = Cursor::new(other);
        let mut out = Builder::new();
        loop {
            match (a.peek(), b.peek()) {
                (None, _) => break,
                (Some(ab), None) => {
                    push_side(&mut out, &mut a, ab, u64::MAX);
                }
                (Some(ab), Some(bb)) => match ab.cmp(&bb) {
                    Ordering::Less => {
                        push_side(&mut out, &mut a, ab, (bb - ab) / CHUNK_BITS);
                    }
                    Ordering::Greater => {
                        // Skip b's windows that precede a; they cannot
                        // contribute to a \ b.
                        let skip = b.remaining.min((ab - bb) / CHUNK_BITS);
                        b.advance(skip);
                    }
                    Ordering::Equal => {
                        let w = combine_words(a.win.unwrap(), b.win.unwrap(), |x, y| x & !y);
                        out.push_words(ab, w);
                        a.advance(1);
                        b.advance(1);
                    }
                },
            }
        }
        out.finish()
    }

    /// Returns the symmetric difference (`self △ other`): bits set in
    /// exactly one map.
    #[must_use]
    pub fn symmetric_difference(&self, other: &SparseMap) -> SparseMap {
        let mut a = Cursor::new(self);
        let mut b = Cursor::new(other);
        let mut out = Builder::new();
        loop {
            match (a.peek(), b.peek()) {
                (None, None) => break,
                (Some(ab), None) => push_side(&mut out, &mut a, ab, u64::MAX),
                (None, Some(bb)) => push_side(&mut out, &mut b, bb, u64::MAX),
                (Some(ab), Some(bb)) => match ab.cmp(&bb) {
                    Ordering::Less => {
                        push_side(&mut out, &mut a, ab, (bb - ab) / CHUNK_BITS);
                    }
                    Ordering::Greater => {
                        push_side(&mut out, &mut b, bb, (ab - bb) / CHUNK_BITS);
                    }
                    Ordering::Equal => {
                        let w = combine_words(a.win.unwrap(), b.win.unwrap(), |x, y| x ^ y);
                        out.push_words(ab, w);
                        a.advance(1);
                        b.advance(1);
                    }
                },
            }
        }
        out.finish()
    }

    /// Returns `true` if `self` and `other` share at least one bit.
    #[must_use]
    pub fn intersects(&self, other: &SparseMap) -> bool {
        let mut a = Cursor::new(self);
        let mut b = Cursor::new(other);
        while let (Some(ab), Some(bb)) = (a.peek(), b.peek()) {
            if ab < bb {
                a.advance(1);
            } else if bb < ab {
                b.advance(1);
            } else if both_ones(&a, &b) {
                return true;
            } else {
                let aw = as_words(a.win.unwrap());
                let bw = as_words(b.win.unwrap());
                if aw.iter().zip(&bw).any(|(x, y)| x & y != 0) {
                    return true;
                }
                a.advance(1);
                b.advance(1);
            }
        }
        false
    }

    /// Returns `true` if every bit set in `self` is also set in `other`.
    #[must_use]
    pub fn is_subset(&self, other: &SparseMap) -> bool {
        // self \ other is empty.
        self.difference(other).is_empty()
    }

    /// Returns `true` if every bit set in `other` is also set in `self`.
    #[must_use]
    pub fn is_superset(&self, other: &SparseMap) -> bool {
        other.is_subset(self)
    }

    /// Returns a copy with every bit shifted by `offset` (positive =
    /// toward higher indices).  Bits that would move below zero or above
    /// `u64::MAX` are dropped, matching the C `sm_offset`.
    #[must_use]
    pub fn shifted(&self, offset: i64) -> SparseMap {
        let mut out = SparseMap::new();
        for bit in self {
            let shifted = if offset >= 0 {
                bit.checked_add(offset as u64)
            } else {
                bit.checked_sub(offset.unsigned_abs())
            };
            if let Some(s) = shifted {
                out.insert(s);
            }
        }
        out
    }
}

#[inline]
fn both_ones(a: &Cursor, b: &Cursor) -> bool {
    matches!(a.win, Some(Win::Ones)) && matches!(b.win, Some(Win::Ones))
}

/// Emit up to `max_windows` of the current window(s) of one side and
/// advance it by that much.  `max_windows` is `u64::MAX` for "the rest
/// of this chunk".
fn push_side(out: &mut Builder, c: &mut Cursor, base: u64, max_windows: u64) {
    match c.win.unwrap() {
        Win::Ones => {
            let span = c.remaining.min(max_windows);
            out.push_ones(base, span);
            c.advance(span);
        }
        Win::Words(w) => {
            out.push_words(base, *w);
            c.advance(1);
        }
    }
}

// ---- operator overloads (by reference, like the standard collections) ----

impl BitOr for &SparseMap {
    type Output = SparseMap;
    fn bitor(self, rhs: &SparseMap) -> SparseMap {
        self.union(rhs)
    }
}
impl BitAnd for &SparseMap {
    type Output = SparseMap;
    fn bitand(self, rhs: &SparseMap) -> SparseMap {
        self.intersection(rhs)
    }
}
impl Sub for &SparseMap {
    type Output = SparseMap;
    fn sub(self, rhs: &SparseMap) -> SparseMap {
        self.difference(rhs)
    }
}
impl BitXor for &SparseMap {
    type Output = SparseMap;
    fn bitxor(self, rhs: &SparseMap) -> SparseMap {
        self.symmetric_difference(rhs)
    }
}

impl BitOrAssign<&SparseMap> for SparseMap {
    fn bitor_assign(&mut self, rhs: &SparseMap) {
        *self = self.union(rhs);
    }
}
impl BitAndAssign<&SparseMap> for SparseMap {
    fn bitand_assign(&mut self, rhs: &SparseMap) {
        *self = self.intersection(rhs);
    }
}
impl SubAssign<&SparseMap> for SparseMap {
    fn sub_assign(&mut self, rhs: &SparseMap) {
        *self = self.difference(rhs);
    }
}
impl BitXorAssign<&SparseMap> for SparseMap {
    fn bitxor_assign(&mut self, rhs: &SparseMap) {
        *self = self.symmetric_difference(rhs);
    }
}

// ---- collection traits ----

impl FromIterator<u64> for SparseMap {
    fn from_iter<I: IntoIterator<Item = u64>>(iter: I) -> Self {
        let mut m = SparseMap::new();
        m.extend(iter);
        m
    }
}

impl Extend<u64> for SparseMap {
    fn extend<I: IntoIterator<Item = u64>>(&mut self, iter: I) {
        for i in iter {
            self.insert(i);
        }
    }
}

impl<'a> Extend<&'a u64> for SparseMap {
    fn extend<I: IntoIterator<Item = &'a u64>>(&mut self, iter: I) {
        for &i in iter {
            self.insert(i);
        }
    }
}

impl core::fmt::Debug for SparseMap {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_set().entries(self.iter()).finish()
    }
}
