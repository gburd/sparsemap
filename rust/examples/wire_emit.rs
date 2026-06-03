//! Emit or describe a named test set for the cross-language wire check
//! (see `ci/wire_compat.sh`).  `emit <name>` writes the serialized bytes
//! to stdout; `describe <name>` prints the set's bits, one per line.
#![allow(clippy::pedantic)]
use sparsemap::SparseMap;
use std::io::Write;

fn build(name: &str) -> SparseMap {
    match name {
        "single" => [42].into_iter().collect(),
        "scattered" => [1, 2, 3, 2047, 2048, 4096, 100_000].into_iter().collect(),
        "run5000" => (0..5000).collect(),
        "run4w" => (0..8192).collect(),
        "clusters" => (0..100).chain(10_000..10_050).collect(),
        "offset" => (1000..7000).collect(),
        other => panic!("unknown set {other}"),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let (cmd, name) = (
        args.get(1).map(String::as_str),
        args.get(2).map(String::as_str),
    );
    let m = build(name.expect("usage: wire_emit <emit|describe> <name>"));
    match cmd {
        Some("emit") => std::io::stdout().write_all(&m.to_bytes().unwrap()).unwrap(),
        Some("describe") => {
            for b in &m {
                println!("{b}");
            }
        }
        _ => panic!("usage: wire_emit <emit|describe> <name>"),
    }
}
