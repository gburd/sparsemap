import sys

from sparsemap import SparseMap

SETS = {
    "single": [42],
    "scattered": [1, 2, 3, 2047, 2048, 4096, 100_000],
    "run5000": list(range(5000)),
    "run4w": list(range(8192)),
    "clusters": list(range(100)) + list(range(10_000, 10_050)),
    "offset": list(range(1000, 7000)),
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
