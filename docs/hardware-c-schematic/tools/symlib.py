"""Symbol definitions: stock KiCad symbols (optionally renumbered) and generated box symbols."""
import copy, math
import sx
from sx import Sym

LIBDIR = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"
_libs = {}
G = 2.54

def _lib(name):
    if name not in _libs:
        _libs[name] = sx.parse(open(f"{LIBDIR}/{name}.kicad_sym", encoding="utf-8").read())[0]
    return _libs[name]

def _strip_v10(node):
    """Drop tokens a version-20250114 reader might not know."""
    drop = {"duplicate_pin_numbers_are_jumpers", "jumper_pin_groups", "body_styles"}
    out = []
    for e in node:
        if isinstance(e, list):
            if e and e[0] in drop:
                continue
            out.append(_strip_v10(e))
        else:
            out.append(e)
    return out

class SymDef:
    """name: library-local name (no 'hwc:' prefix). node: the (symbol ...) list.
    pins: {(unit, num): dict(x, y, angle, length, name)} in library coords (y up)."""
    def __init__(self, name, node, ref_prefix="U"):
        self.name, self.node, self.ref_prefix = name, node, ref_prefix
        self.index()

    def index(self):
        self.pins = {}
        self.units = set()
        for sub in sx.findall(self.node, "symbol"):
            parts = sub[1].rsplit("_", 2)
            unit = int(parts[1])
            if unit:
                self.units.add(unit)
            for p in sx.findall(sub, "pin"):
                at = sx.find(p, "at"); ln = sx.find(p, "length")
                nm = sx.find(p, "name")[1]; no = sx.find(p, "number")[1]
                self.pins[(unit, no)] = dict(x=float(at[1]), y=float(at[2]), angle=int(float(at[3])),
                                             length=float(ln[1]), name=nm)
        if not self.units:
            self.units = {1}

    def pin(self, unit, num):
        if (unit, num) in self.pins:
            return self.pins[(unit, num)]
        if (0, num) in self.pins:
            return self.pins[(0, num)]
        raise KeyError(f"{self.name}: no pin {num} in unit {unit}")

    def unit_pins(self, unit):
        return [n for (u, n) in self.pins if u in (0, unit)]

    def lib_node(self, prefix="hwc:"):
        n = copy.deepcopy(self.node)
        n[1] = prefix + self.name
        return n

    def prop(self, key):
        for p in sx.findall(self.node, "property"):
            if p[1] == key:
                return p
        return None

def stock(lib, name, newname=None, renumber=None, ref_prefix=None, extra_pins=None):
    """Copy a stock symbol. renumber: {old_num: new_num}. extra_pins: list of pin nodes to add to unit 1."""
    for e in _lib(lib):
        if isinstance(e, list) and e[0] == "symbol" and e[1] == name:
            node = _strip_v10(copy.deepcopy(e))
            break
    else:
        raise KeyError(f"{lib}:{name}")
    newname = newname or name
    node[1] = newname
    for sub in sx.findall(node, "symbol"):
        assert sub[1].startswith(name + "_"), sub[1]
        sub[1] = newname + sub[1][len(name):]
        if renumber:
            for p in sx.findall(sub, "pin"):
                no = sx.find(p, "number")
                if no[1] in renumber:
                    no[1] = renumber[no[1]]
    if extra_pins:
        subs = [s for s in sx.findall(node, "symbol") if s[1].endswith("_1_1")]
        if not subs:
            sub = [Sym("symbol"), f"{newname}_1_1"]; node.append(sub)
        else:
            sub = subs[0]
        sub.extend(extra_pins)
    rp = ref_prefix
    if rp is None:
        r = None
        for p in sx.findall(node, "property"):
            if p[1] == "Reference":
                r = p[2]
        rp = r or "U"
    return SymDef(newname, node, rp)

def pin_node(num, name, x, y, angle, length=G, etype="passive", hide=False, name_size=1.27, num_size=1.27):
    n = [Sym("pin"), Sym(etype), Sym("line"), [Sym("at"), x, y, angle], [Sym("length"), length]]
    if hide:
        n.append([Sym("hide"), Sym("yes")])
    n.append([Sym("name"), name, [Sym("effects"), [Sym("font"), [Sym("size"), name_size, name_size]]]])
    n.append([Sym("number"), num, [Sym("effects"), [Sym("font"), [Sym("size"), num_size, num_size]]]])
    return n

def prop_node(key, val, x=0.0, y=0.0, angle=0, hide=False, justify=None, size=1.27):
    eff = [Sym("effects"), [Sym("font"), [Sym("size"), size, size]]]
    if justify:
        eff.append([Sym("justify")] + [Sym(j) for j in justify.split()])
    if hide:
        eff.append([Sym("hide"), Sym("yes")])
    return [Sym("property"), key, val, [Sym("at"), x, y, angle], eff]

def text_w(s, size=1.27):
    """Rough width of KiCad stroke text in mm."""
    w = 0.0
    for ch in s:
        if ord(ch) > 0x2E80:
            w += size * 1.6
        elif ch in "il1.,:;|!' ":
            w += size * 0.5
        elif ch in "MW#@%":
            w += size * 1.15
        elif ch.isupper() or ch.isdigit() or ch in "_-+/#=":
            w += size * 0.97
        else:
            w += size * 0.82
    return w

def gridup(v, g=G):
    return math.ceil(v / g - 1e-9) * g

def box(name, units, ref_prefix="U", value="", pin_len=G, show_numbers=True, min_hw=2 * G,
        tb_pitch=2 * G):
    """units: list of dicts with optional keys left/right/top/bottom (lists of (num, name) or None gaps)
    and 'title' (text drawn inside box top). Pins on a 2.54 grid; symbol origin at the box centre-ish."""
    node = [Sym("symbol"), name,
            [Sym("pin_names"), [Sym("offset"), 0.762]],
            [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")]]
    if not show_numbers:
        node.insert(2, [Sym("pin_numbers"), [Sym("hide"), Sym("yes")]])
    geo = {}
    subs = []
    for ui, u in enumerate(units, start=1):
        L, R = u.get("left", []), u.get("right", []), 
        T, B = u.get("top", []), u.get("bottom", [])
        rows = max(len(L), len(R), 1)
        def maxw(lst):
            return max([text_w(e[1]) for e in lst if e] + [0])
        tbw = max(maxw(T), maxw(B))
        inner = maxw(L) + maxw(R) + 2 * 0.762 + 2.0
        ncol = max(len(T), len(B))
        span = max([abs(x) for x in (lambda n: [(-math.floor(((n - 1) * tb_pitch / 2) / G + 0.5) * G + i * tb_pitch) for i in range(n)])(ncol)] + [0])
        hw = gridup(max(inner / 2, min_hw, span + G), G)
        if u.get("title"):
            hw = gridup(max(hw, text_w(u["title"]) / 2 + 1.27))
        top_margin = gridup(max(G, (tbw + 1.6) if T else G) + (2 * G if u.get("title") else 0))
        bot_margin = gridup(max(G, (tbw + 1.6) if B else G))
        y_top_pin = 0.0                     # first row pin y (library coords, y up)
        box_top = y_top_pin + top_margin
        box_bot = y_top_pin - (rows - 1) * G - bot_margin
        items = []
        items.append([Sym("rectangle"), [Sym("start"), -hw, box_top], [Sym("end"), hw, box_bot],
                      [Sym("stroke"), [Sym("width"), 0.254], [Sym("type"), Sym("default")]],
                      [Sym("fill"), [Sym("type"), Sym("background")]]])
        if u.get("title"):
            items.append([Sym("text"), u["title"], [Sym("at"), 0, box_top - G, 0],
                          [Sym("effects"), [Sym("font"), [Sym("size"), 1.27, 1.27], [Sym("bold"), Sym("yes")]]]])
        pins = []
        for i, e in enumerate(L):
            if e:
                pins.append(pin_node(e[0], e[1], -hw - pin_len, y_top_pin - i * G, 0, pin_len, *e[2:]))
        for i, e in enumerate(R):
            if e:
                pins.append(pin_node(e[0], e[1], hw + pin_len, y_top_pin - i * G, 180, pin_len, *e[2:]))
        def tbx(n):
            start = -math.floor(((n - 1) * tb_pitch / 2) / G + 0.5) * G
            return [start + i * tb_pitch for i in range(n)]
        for x, e in zip(tbx(len(T)), T):
            if e:
                pins.append(pin_node(e[0], e[1], x, box_top + pin_len, 270, pin_len, *e[2:]))
        for x, e in zip(tbx(len(B)), B):
            if e:
                pins.append(pin_node(e[0], e[1], x, box_bot - pin_len, 90, pin_len, *e[2:]))
        subs.append([Sym("symbol"), f"{name}_{ui}_1"] + items + pins)
        geo[ui] = dict(hw=hw, top=box_top, bot=box_bot)
    node.append(prop_node("Reference", ref_prefix, -geo[1]["hw"], geo[1]["top"] + 1.27, justify="left bottom"))
    node.append(prop_node("Value", value or name, -geo[1]["hw"], geo[1]["bot"] - 1.27, justify="left top"))
    node.append(prop_node("Footprint", "", 0, 0, hide=True))
    node.append(prop_node("Datasheet", "", 0, 0, hide=True))
    node.append(prop_node("Description", "", 0, 0, hide=True))
    node.extend(subs)
    s = SymDef(name, node, ref_prefix)
    s.geo = geo
    return s

def power(name, kind):
    """kind 'gnd' or 'rail'. One symbol per net name so the hidden pin and value agree."""
    base = stock("power", "GND" if kind == "gnd" else "+3V3", newname="PWR_" + name.replace("+", "P"))
    for p in sx.findall(base.node, "property"):
        if p[1] == "Value":
            p[2] = name
        if p[1] == "Description":
            p[2] = f"Power symbol for net {name}"
    for sub in sx.findall(base.node, "symbol"):
        for pn in sx.findall(sub, "pin"):
            sx.find(pn, "name")[1] = name
    base.index()
    base.kind = kind
    return base
