import argparse
import os
import sys

from . import solve


def main(argv=None):
    p = argparse.ArgumentParser(prog="python -m havne.discover",
                                description="which fields of a recording's DIDs are which "
                                            "signals: report.json, report.txt and signals.json")
    p.add_argument("recording", help="a recording's folder")
    p.add_argument("--oracle", help="a logger's CSV of the same drive, with an engine speed "
                                    "column")
    p.add_argument("--out", help="the folder to write into (<recording>-discovery beside it "
                                 "by default)")
    p.add_argument("--catalog", help="a wanted list other than the package's")
    p.add_argument("--packs", help="the wanted list's packs, comma separated (all by default)")
    args = p.parse_args(argv)
    packs = args.packs.split(",") if args.packs else None
    report = solve(args.recording, oracle=args.oracle, catalog=args.catalog, packs=packs)
    out = args.out or os.path.normpath(args.recording) + "-discovery"
    report.write(out)
    counts = {s: len(report.by_status(s)) for s in ("solved", "likely", "related", "unnamed")}
    print(f"{out}: " + ", ".join(f"{n} {s}" for s, n in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
