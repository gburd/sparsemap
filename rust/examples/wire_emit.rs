//! Emit or describe a named test set for the cross-language wire check
//! (see `ci/wire_compat.sh`).  `emit <name>` writes the serialized bytes
//! to stdout; `describe <name>` prints the set's bits, one per line;
//! `read` decodes a serialized map from stdin (e.g. produced by the C
//! library, including its small-set mode) and prints its bits, one per
//! line -- the C->Rust direction.
#![allow(clippy::pedantic)]
use sparsemap::SparseMap;
use std::io::{Read, Write};

fn build(name: &str) -> SparseMap {
    match name {
        "single" => [42].into_iter().collect(),
        "scattered" => [1, 2, 3, 2047, 2048, 4096, 100_000].into_iter().collect(),
        "run5000" => (0..5000).collect(),
        "run4w" => (0..8192).collect(),
        "clusters" => (0..100).chain(10_000..10_050).collect(),
        "offset" => (1000..7000).collect(),
        // New shapes mirroring the C small-mode / RLE-chunk fixtures.
        // Rust always emits chunk mode (it is representation-agnostic);
        // this checks C reads those bytes back to the same set.
        "smallzero" => [0].into_iter().collect(),
        "smallword0" => [0, 1, 5, 63].into_iter().collect(),
        "smallfullword" => (0..64).collect(),
        "smalltwowords" => [5, 70].into_iter().collect(),
        "smallscatter" => [3, 17, 88, 200, 511, 900, 1023].into_iter().collect(),
        "rle1000" => (0..=1000).collect(),
        "rle1023" => (0..=1023).collect(),
        "rle5000" => (0..=5000).collect(),
        "rle2048" => (2048..=4095).collect(),
        "mixedhi" => (0..=1200).chain([50000, 50003, 123456]).collect(),
        "straddle" => [0, 1, 1023, 1024, 1025, 2050].into_iter().collect(),
        other => panic!("unknown set {other}"),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(String::as_str);
    if cmd == Some("read") {
        let mut buf = Vec::new();
        std::io::stdin().read_to_end(&mut buf).unwrap();
        match SparseMap::from_bytes(&buf) {
            Ok(m) => {
                for b in &m {
                    println!("{b}");
                }
            }
            // An empty map may serialize to a body the decoder treats as
            // empty; print nothing, matching the C reader.
            Err(e) => {
                eprintln!("decode error: {e}");
                std::process::exit(1);
            }
        }
        return;
    }
    let name = args.get(2).map(String::as_str);
    let m = build(name.expect("usage: wire_emit <emit|describe|read> [name]"));
    match cmd {
        Some("emit") => std::io::stdout().write_all(&m.to_bytes()).unwrap(),
        Some("describe") => {
            for b in &m {
                println!("{b}");
            }
        }
        _ => panic!("usage: wire_emit <emit|describe|read> [name]"),
    }
}
