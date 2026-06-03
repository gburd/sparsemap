//! Ascending iteration over the set bits of a [`SparseMap`].

use crate::{Chunk, SparseMap, BITS_PER_WORD, CHUNK_BITS, WORDS_PER_CHUNK};
use alloc::collections::btree_map;

/// State for expanding the chunk currently being yielded.
enum Cursor<'a> {
    /// A run yielding `next..end`.
    Run { next: u64, end: u64 },
    /// A dense window: `base`, the word index, and the remaining bits
    /// of the current word.
    Dense {
        base: u64,
        word: usize,
        bits: u64,
        words: &'a [u64; WORDS_PER_CHUNK],
    },
}

/// An iterator over the set bits of a [`SparseMap`] in ascending order.
///
/// Created by [`SparseMap::iter`] (and by `&SparseMap`'s
/// [`IntoIterator`]).
#[must_use = "iterators are lazy and do nothing unless consumed"]
pub struct Iter<'a> {
    chunks: btree_map::Iter<'a, u64, Chunk>,
    cur: Option<Cursor<'a>>,
}

impl<'a> Iter<'a> {
    pub(crate) fn new(map: &'a SparseMap) -> Self {
        Iter {
            chunks: map.chunks.iter(),
            cur: None,
        }
    }

    fn load_next_chunk(&mut self) -> bool {
        match self.chunks.next() {
            Some((&base, Chunk::Run(n))) => {
                self.cur = Some(Cursor::Run {
                    next: base,
                    end: base + u64::from(*n) * CHUNK_BITS,
                });
                true
            }
            Some((&base, Chunk::Dense(w))) => {
                self.cur = Some(Cursor::Dense {
                    base,
                    word: 0,
                    bits: w[0],
                    words: w,
                });
                true
            }
            None => false,
        }
    }
}

impl Iterator for Iter<'_> {
    type Item = u64;

    fn next(&mut self) -> Option<u64> {
        loop {
            match &mut self.cur {
                Some(Cursor::Run { next, end }) => {
                    if *next < *end {
                        let v = *next;
                        *next += 1;
                        return Some(v);
                    }
                    self.cur = None;
                }
                Some(Cursor::Dense {
                    base,
                    word,
                    bits,
                    words,
                }) => {
                    while *bits == 0 {
                        *word += 1;
                        if *word >= WORDS_PER_CHUNK {
                            break;
                        }
                        *bits = words[*word];
                    }
                    if *word < WORDS_PER_CHUNK {
                        let tz = bits.trailing_zeros();
                        *bits &= *bits - 1; // clear lowest set bit
                        return Some(*base + *word as u64 * BITS_PER_WORD + u64::from(tz));
                    }
                    self.cur = None;
                }
                None => {
                    if !self.load_next_chunk() {
                        return None;
                    }
                }
            }
        }
    }
}

impl<'a> IntoIterator for &'a SparseMap {
    type Item = u64;
    type IntoIter = Iter<'a>;

    fn into_iter(self) -> Iter<'a> {
        self.iter()
    }
}

impl SparseMap {
    /// Returns an iterator over the set bits, in ascending order.
    ///
    /// ```
    /// use sparsemap::SparseMap;
    /// let m: SparseMap = [3, 1, 4, 1, 5].into_iter().collect();
    /// assert_eq!(m.iter().collect::<Vec<_>>(), vec![1, 3, 4, 5]);
    /// ```
    pub fn iter(&self) -> Iter<'_> {
        Iter::new(self)
    }

    /// Collects the set bits into a `Vec<u64>`, ascending.
    ///
    /// Allocates; provided for parity with the C `sm_to_array`.
    #[must_use]
    pub fn to_vec(&self) -> alloc::vec::Vec<u64> {
        self.iter().collect()
    }
}
