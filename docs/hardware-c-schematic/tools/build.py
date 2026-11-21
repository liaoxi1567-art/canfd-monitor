"""Build the redrawn KiCad project. usage: build.py OUTDIR"""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sx, ctx as C, sheet as S, pages, symlib
from sx import Sym
from sheet import uid

OUT = sys.argv[1]
PROJECT = "hwc_c062"
os.makedirs(OUT, exist_ok=True)
c = C.Ctx()
only = [int(a) for a in sys.argv[2:]]
sheets = []
for page in range(1, 12):
    if only and page not in only:
        continue
    fn = getattr(pages, f"p{page:02d}", None)
    s = S.Sheet(c, f"p{page:02d}", page, c.titles[page], paper=pages.PAPER.get(page, "A3"))
    if fn:
        fn(s)
    sheets.append(s)

where = collections.defaultdict(set)
for s in sheets:
    for pl in s.parts:
        for num in pl.pins():
            n = c.net(pl.ref, num)
            if n:
                where[n].add(s.page)
c.global_nets = {n for n, pg in where.items() if len(pg) > 1 and n not in C.RAILS}
errors = []
for s in sheets:
    s.auto_terminate()
    if getattr(pages, "AUTONAME", True) and s.page not in getattr(pages, "NO_AUTONAME", ()):
        s.autoname()
    s.finalize()
    cur = collections.defaultdict(set)
    for pl in s.parts:
        for num in pl.pins():
            n = c.net(pl.ref, num)
            if n:
                cur[n].add((pl.ref, num))
    c._cur_netpins = cur
    for e in s.check():
        errors.append(f"{s.key}: {e}")
# completeness
placed = collections.Counter()
for s in sheets:
    for pl in s.parts:
        for num in pl.pins():
            placed[(pl.ref, num)] += 1
if not only:
    for r, d in c.pins.items():
        for n in d["pins"]:
            if placed[(r, n)] != 1:
                errors.append(f"pin {r}.{n} placed {placed[(r, n)]} times")
for w in c.warnings:
    print("WARN", w)
for e in errors:
    print("ERR ", e)

root_uuid = uid("root")
sheet_uuid = {s.key: uid("sheetsym", s.key) for s in sheets}
for s in sheets:
    path = f"/{root_uuid}/{sheet_uuid[s.key]}"
    doc = s.sexpr(PROJECT, path, root_uuid)
    doc.append([Sym("embedded_fonts"), Sym("no")])
    open(f"{OUT}/{s.key}.kicad_sch", "w", encoding="utf-8").write(sx.dump(doc) + "\n")

# root / index sheet
import index
root = index.root_sheet(c, sheets, PROJECT, root_uuid, sheet_uuid)
open(f"{OUT}/{PROJECT}.kicad_sch", "w", encoding="utf-8").write(sx.dump(root) + "\n")

# library + project files
lib = [Sym("kicad_symbol_lib"), [Sym("version"), 20251024], [Sym("generator"), "hwc_redraw"], [Sym("generator_version"), "10.0"]]
for k in sorted(c.syms):
    lib.append(c.syms[k].lib_node(prefix=""))
open(f"{OUT}/hwc.kicad_sym", "w", encoding="utf-8").write(sx.dump(lib) + "\n")
open(f"{OUT}/sym-lib-table", "w").write('(sym_lib_table\n\t(version 7)\n\t(lib (name "hwc") (type "KiCad") (uri "${KIPRJMOD}/hwc.kicad_sym") (options "") (descr "Hardware-C redraw symbols"))\n)\n')
pro = {
    "meta": {"filename": f"{PROJECT}.kicad_pro", "version": 3},
    "schematic": {"drawing": {"intersheets_ref_own_page": False, "intersheets_ref_prefix": "[", "intersheets_ref_short": False,
                              "intersheets_ref_show": True, "intersheets_ref_suffix": "]", "default_line_thickness": 6.0,
                              "default_text_size": 50.0, "label_size_ratio": 0.375, "pin_symbol_size": 25.0,
                              "text_offset_ratio": 0.15, "junction_size_choice": 3},
                  "legacy_lib_dir": "", "legacy_lib_list": []},
    "erc": {"rule_severities": {"pin_not_driven": "ignore", "power_pin_not_driven": "ignore", "pin_to_pin": "ignore",
                                "lib_symbol_mismatch": "ignore", "lib_symbol_issues": "warning",
                                "footprint_link_issues": "ignore", "simulation_model_issue": "ignore"}},
    "sheets": [[root_uuid, "Root"]] + [[sheet_uuid[s.key], s.key] for s in sheets],
    "text_variables": {},
}
json.dump(pro, open(f"{OUT}/{PROJECT}.kicad_pro", "w"), indent=2)
print("sheets", len(sheets), "errors", len(errors))
sys.exit(1 if errors else 0)
