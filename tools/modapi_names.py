#!/usr/bin/env python3
"""Extract function and global names from the Spore ModAPI SDK's Ghidra XML (SDKtoGhidra/SporeGhidra_march2017.xml)
into a plain CSV (kind,address,name) for tools/ghidra/ApplyNames.java. Only names and addresses are taken; the SDK
itself stays outside the repo (~/spore-re-data/ext) and the output goes to re/decomp (gitignored).

Usage: modapi_names.py <SporeGhidra_march2017.xml> <out.csv>
"""
import csv
import sys
import xml.etree.ElementTree as ET


def main():
    src, out = sys.argv[1], sys.argv[2]
    rows = []
    for _, el in ET.iterparse(src):
        if el.tag == "FUNCTION" and el.get("ADDRESS"):
            rows.append(("function", el.get("ADDRESS"), el.get("NAME")))
        elif el.tag == "SYMBOL" and el.get("ADDRESS"):
            rows.append(("symbol", el.get("ADDRESS"), el.get("NAME")))
        el.clear()
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("kind", "address", "name"))
        w.writerows(sorted(set(rows), key=lambda r: int(r[1], 16)))
    print(f"{len(rows)} names -> {out}")


if __name__ == "__main__":
    main()
