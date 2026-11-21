"""Compare a KiCad netlist with the original schematic-pins.json. usage: netcmp.py net.net [refs-subset]"""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sx
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
P = json.load(open(f"{DATA}/schematic-pins.json"))
x = sx.parse(open(sys.argv[1], encoding="utf-8").read())[0]
nets = sx.find(x, "nets")
kn = {}
for n in sx.findall(nets, "net"):
    name = sx.find(n, "name")[1]
    nodes = {(sx.find(d, "ref")[1], sx.find(d, "pin")[1]) for d in sx.findall(n, "node")}
    kn[name] = nodes
comps = {sx.find(c, "ref")[1] for c in sx.findall(sx.find(x, "components"), "comp")}
refs = set(P) if len(sys.argv) < 3 else set(sys.argv[2].split(","))
if refs == {"auto"}:
    refs = {r for r in comps if not r.startswith("#")}
orig = collections.defaultdict(set)
for r in refs:
    for n, p in P[r]["pins"].items():
        if p["net"]:
            orig[p["net"]].add((r, n))
# KiCad side restricted to refs; unconnected pins appear as "unconnected-(...)" nets
k = {}
uncon = set()
for name, nodes in kn.items():
    nodes = {(r, p) for r, p in nodes if r in refs}
    if not nodes:
        continue
    if name.startswith("unconnected-"):
        uncon |= nodes
        continue
    k[name] = nodes
def base(nm):
    return nm.rsplit("/", 1)[-1]
bad = 0
o_by_pins = {frozenset(v): n for n, v in orig.items()}
for name, nodes in sorted(k.items()):
    on = o_by_pins.get(frozenset(nodes))
    if on is None:
        bad += 1
        print("MISMATCH kicad net", name, sorted(nodes)[:8])
    elif base(name) != on and not name.startswith("Net-("):
        print("NAME", name, "vs original", on)
    elif name.startswith("Net-("):
        print("UNNAMED", name, "= original", on)
for n, v in orig.items():
    if frozenset(v) not in {frozenset(x) for x in k.values()}:
        bad += 1
        print("MISSING original net", n, sorted(v)[:8])
want_unc = {(r, n) for r in refs for n, p in P[r]["pins"].items() if not p["net"]}
if uncon != want_unc:
    bad += 1
    print("UNCONNECTED differ: extra", sorted(uncon - want_unc)[:8], "missing", sorted(want_unc - uncon)[:8])
missing_comp = refs - comps
if missing_comp:
    bad += 1; print("components missing", sorted(missing_comp))
print(f"compared {len(refs)} parts, {len(orig)} nets: {'OK' if not bad else str(bad) + ' problems'}")
sys.exit(1 if bad else 0)
