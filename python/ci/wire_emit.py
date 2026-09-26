import sys

from sparsemap import SparseMap

SETS = {
    "single": [42],
    "scattered": [1, 2, 3, 2047, 2048, 4096, 100_000],
    "run5000": list(range(5000)),
    "run4w": list(range(8192)),
    "clusters": list(range(100)) + list(range(10_000, 10_050)),
    "offset": list(range(1000, 7000)),
    # small-mode shapes (C serializes these with the body top bit set)
    "small_zero": [0],
    "small_word0": [0, 1, 5, 63],
    "small_fullword": list(range(64)),
    "small_twowords": [5, 70],
    "small_scatter": [3, 17, 88, 200, 511, 900, 1023],
    # RLE-chunk shapes
    "rle_run_0_1000": list(range(1001)),
    "rle_run_0_1023": list(range(1024)),
    "rle_run_0_5000": list(range(5001)),
    "rle_run_2048_4095": list(range(2048, 4096)),
    # mixed
    "mixed_lowrun_highsparse": list(range(1201)) + [50_000, 50_003, 123_456],
    "mixed_straddle_1024": [0, 1, 1023, 1024, 1025, 2050],
}

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("emit", "describe"):
        raise SystemExit("usage: wire_emit.py <emit|describe> <name>")
    m = SparseMap(SETS[sys.argv[2]])
    if sys.argv[1] == "emit":
        sys.stdout.buffer.write(m.to_bytes())
    else:
        for b in m:
            print(b)

if __name__ == "__main__":
    main()
