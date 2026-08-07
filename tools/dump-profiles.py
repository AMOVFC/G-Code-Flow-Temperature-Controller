#!/usr/bin/env python3
"""Dump extruder and filament profiles from a legacy Config.sdb.

A read-only convenience for inspecting the existing database and for exporting
calibration into the JSON form the CLI accepts. The C++ SqliteProfileRepository is the
real implementation; this exists because the schema is worth being able to inspect
without a build.

Usage:
    python tools/dump-profiles.py <Config.sdb> [--json]
"""

import argparse
import json
import sqlite3
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("database")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = ap.parse_args()

    # Read-only URI, so inspecting a user's live database cannot alter it.
    con = sqlite3.connect(f"file:{args.database}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row

    tables = [r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")]

    out = {}
    for table in ("EXTRUDER", "FILAMENT"):
        if table not in tables:
            print(f"warning: no {table} table in this database", file=sys.stderr)
            continue
        out[table] = [dict(r) for r in con.execute(f"SELECT * FROM {table}")]

    if args.json:
        # PRINTER_CONFIG holds an entire embedded printer profile; it drowns everything
        # else in a dump, so summarise it.
        for row in out.get("EXTRUDER", []):
            cfg = row.get("PRINTER_CONFIG")
            if isinstance(cfg, str) and len(cfg) > 120:
                row["PRINTER_CONFIG"] = f"<{len(cfg)} chars>"
        print(json.dumps(out, indent=2, default=str))
        return 0

    for table, rows in out.items():
        print(f"=== {table}  ({len(rows)} row(s))")
        for row in rows:
            print()
            for key, value in row.items():
                if isinstance(value, str) and len(value) > 100:
                    value = f"<{len(value)} chars>"
                print(f"  {key:<20} {value}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
