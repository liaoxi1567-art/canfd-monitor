"""Schematic sheet engine: place symbols, draw wires, terminate pins with labels / power symbols,
check connectivity against the original netlist, and write a .kicad_sch file."""
import uuid, math, collections
import sx
from sx import Sym

G = 2.54
NS = uuid.UUID("6f1c2a3e-0000-4000-8000-c062c062c062")

def uid(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))

def rnd(v):
    return round(v * 100) / 100.0

def P(x, y):
    return (rnd(x), rnd(y))

# screen direction vectors (y down)
DIRS = {"R": (1, 0), "L": (-1, 0), "U": (0, -1), "D": (0, 1)}

def vec_dir(v):
    for k, d in DIRS.items():
        if d == v:
            return k
    raise ValueError(v)

def xform(x, y, rot, mirror=None):
    """library (y-up) offset -> screen offset for a symbol at rotation rot (CCW, degrees)."""
    xs, ys = x, -y
    r = math.radians(rot)
    c, s = round(math.cos(r)), round(math.sin(r))
    xr, yr = xs * c + ys * s, -xs * s + ys * c
    if mirror == "x":
        yr = -yr
    elif mirror == "y":
        xr = -xr
    return xr, yr

class Placed:
    def __init__(self, sheet, ref, sd, x, y, rot, unit, mirror):
        self.sheet, self.ref, self.sd = sheet, ref, sd
        self.x, self.y, self.rot, self.unit, self.mirror = x, y, rot, unit, mirror
        self.fields = {}

    def p(self, num):
        pin = self.sd.pin(self.unit, str(num))
        dx, dy = xform(pin["x"], pin["y"], self.rot, self.mirror)
        return P(self.x + dx, self.y + dy)

    def out(self, num):
        """unit vector pointing away from the body at the pin."""
        pin = self.sd.pin(self.unit, str(num))
        a = math.radians(pin["angle"] + 180)
        vx, vy = round(math.cos(a)), round(math.sin(a))
        dx, dy = xform(vx, vy, self.rot, self.mirror)
        return (int(dx), int(dy))

    def pins(self):
        return self.sd.unit_pins(self.unit)

class Sheet:
    def __init__(self, ctx, key, page, title, paper="A3", comments=()):
        self.ctx, self.key, self.page, self.title, self.paper = ctx, key, page, title, paper
        self.comments = comments
        self.parts = []            # Placed
        self.wires = []            # (p1, p2)
        self.labels = []           # (kind, name, pt, angle, shape)
        self.powers = []           # (net, pt, rot)
        self.ncs = []
        self.texts = []            # raw nodes
        self.touched = set()       # (ref, num)
        self.byref = {}
        self.npwr = 0

    # ---------- placement ----------
    def place(self, ref, x, y, rot=0, unit=1, mirror=None, sym=None, fields="auto"):
        sd = self.ctx.symbol_for(ref, sym)
        pl = Placed(self, ref, sd, rnd(x), rnd(y), rot, unit, mirror)
        self.parts.append(pl)
        self.byref[(ref, unit)] = pl
        pl.field_mode = fields
        return pl

    def place_pin(self, ref, pin, x, y, rot=0, unit=1, mirror=None, sym=None, fields="auto"):
        """Place so that `pin` lands on (x, y)."""
        sd = self.ctx.symbol_for(ref, sym)
        p = sd.pin(unit, str(pin))
        dx, dy = xform(p["x"], p["y"], rot, mirror)
        return self.place(ref, x - dx, y - dy, rot=rot, unit=unit, mirror=mirror, sym=sym, fields=fields)

    def pp(self, ref, num):
        """position of pin; finds the unit that has the pin."""
        for (r, u), pl in self.byref.items():
            if r == ref and str(num) in pl.pins():
                return pl.p(num)
        raise KeyError(f"{ref}.{num} not placed on {self.key}")

    def placed_for(self, ref, num):
        for (r, u), pl in self.byref.items():
            if r == ref and str(num) in pl.pins():
                return pl
        raise KeyError(f"{ref}.{num} not placed on {self.key}")

    def net(self, ref, num):
        return self.ctx.net(ref, num)

    # ---------- primitives ----------
    def _pt(self, a):
        if isinstance(a, tuple) and len(a) == 2 and isinstance(a[0], str):
            self.touched.add((a[0], str(a[1])))
            return self.pp(*a)
        return P(*a)

    def wire(self, *pts, hv=True):
        """Polyline through points; points are (x, y) or ('R1', '2') pin refs.
        Diagonal steps become an L: horizontal first if hv else vertical first."""
        q = [self._pt(a) for a in pts]
        for a, b in zip(q, q[1:]):
            if a == b:
                continue
            if a[0] != b[0] and a[1] != b[1]:
                m = (b[0], a[1]) if hv else (a[0], b[1])
                self.wires.append((a, m)); self.wires.append((m, b))
            else:
                self.wires.append((a, b))
        return q[-1]

    def label(self, pt, name, d="R", kind=None, shape=None):
        """d = direction the text extends from the anchor."""
        pt = self._pt(pt)
        ang = {"R": 0, "U": 90, "L": 180, "D": 270}[d]
        self.labels.append((kind, name, pt, ang, shape))

    def power(self, pt, net, rot=None, d=None, side=False):
        pt = self._pt(pt)
        kind = self.ctx.rail_kind(net)
        if side and d in ("L", "R"):
            if kind == "gnd":
                rot = 270 if d == "L" else 90
            else:
                rot = 90 if d == "L" else 270
        if rot is None:
            if kind == "gnd":
                rot = 180 if d == "U" else 0
            else:
                rot = 180 if d == "D" else 0
        self.powers.append((net, pt, rot))

    def nc(self, pin):
        self.touched.add((pin[0], str(pin[1])))
        self.ncs.append(self.pp(*pin))

    def text(self, x, y, s, size=1.27, bold=False, justify="left top", angle=0, italic=False, color=None):
        eff = [Sym("effects"), [Sym("font"), [Sym("size"), size, size]] + ([[Sym("bold"), Sym("yes")]] if bold else []) + ([[Sym("italic"), Sym("yes")]] if italic else [])]
        if color:
            eff[1].append([Sym("color")] + list(color))
        eff.append([Sym("justify")] + [Sym(j) for j in justify.split()])
        self.texts.append([Sym("text"), s, [Sym("exclude_from_sim"), Sym("no")], [Sym("at"), rnd(x), rnd(y), angle], eff,
                           [Sym("uuid"), uid(self.key, "text", len(self.texts), s)]])

    def rect(self, x1, y1, x2, y2, width=0.15, dash="dash", color=None):
        st = [Sym("stroke"), [Sym("width"), width], [Sym("type"), Sym(dash)]]
        if color:
            st.append([Sym("color")] + list(color))
        self.texts.append([Sym("rectangle"), [Sym("start"), rnd(x1), rnd(y1)], [Sym("end"), rnd(x2), rnd(y2)], st,
                           [Sym("fill"), [Sym("type"), Sym("none")]], [Sym("uuid"), uid(self.key, "rect", len(self.texts))]])

    def group(self, x1, y1, x2, y2, title, notes=()):
        """dashed frame with a title in the top-left corner and optional note lines under it."""
        self.rect(x1, y1, x2, y2, color=(120, 120, 120, 1))
        self.text(x1 + 1.27, y1 + 1.27, title, size=1.8, bold=True, color=(0, 60, 120, 1))
        for i, n in enumerate(notes):
            self.text(x1 + 1.27, y1 + 4.6 + i * 2.3, n, size=1.27, italic=False, color=(80, 80, 80, 1))

    # ---------- high level ----------
    def term(self, pin, length=G, d=None, name=None, kind=None, at_end=None, side=False):
        """Stub from a pin outwards and terminate it according to its original net."""
        ref, num = pin[0], str(pin[1])
        pl = self.placed_for(ref, num)
        net = self.net(ref, num)
        self.touched.add((ref, num))
        p0 = pl.p(num)
        if net is None:
            self.ncs.append(p0); return p0
        ov = pl.out(num)
        if d:
            ov = DIRS[d]
        dname = vec_dir(ov)
        end = P(p0[0] + ov[0] * length, p0[1] + ov[1] * length) if length else p0
        if length:
            self.wires.append((p0, end))
        if self.ctx.is_rail(net) and kind != "label":
            self.power(end, net, d=dname, side=side)
        else:
            self.label(end, name or net, dname, kind=None if kind in (None, "label") else kind)
        return end

    def auto_terminate(self):
        for pl in self.parts:
            for num in pl.pins():
                if (pl.ref, num) not in self.touched:
                    self.ctx.warn(f"{self.key}: auto-terminated {pl.ref}.{num} ({self.net(pl.ref, num)})")
                    self.term((pl.ref, num))

    # ---------- connectivity ----------
    def connectivity(self):
        """Union-find over drawn geometry. Returns list of groups: dict(pins=set, names=set)."""
        parent = {}
        def f(a):
            parent.setdefault(a, a)
            while parent[a] != a:
                parent[a] = parent[parent[a]]; a = parent[a]
            return a
        def u(a, b):
            parent[f(a)] = f(b)
        pts = collections.defaultdict(list)    # point -> items
        for pl in self.parts:
            for num in pl.pins():
                pts[pl.p(num)].append(("pin", pl.ref, num))
        for (kind, name, pt, ang, shape) in self.labels:
            pts[pt].append(("name", name))
        for (net, pt, rot) in self.powers:
            pts[pt].append(("name", net))
        for pt in self.ncs:
            pts[pt].append(("nc",))
        segs = list(self.wires)
        allpts = set(pts) | {a for s in segs for a in s}
        for a in allpts:
            f(a)
        def on(p, s):
            (x1, y1), (x2, y2) = s
            if x1 == x2 == p[0]:
                return min(y1, y2) <= p[1] <= max(y1, y2)
            if y1 == y2 == p[1]:
                return min(x1, x2) <= p[0] <= max(x1, x2)
            return False
        for s in segs:
            u(s[0], s[1])
            for p in allpts:
                if p not in s and on(p, s):
                    u(p, s[0])
        # collinear overlapping segments
        for i, s in enumerate(segs):
            for t in segs[i + 1:]:
                if on(t[0], s) or on(t[1], s) or on(s[0], t) or on(s[1], t):
                    u(s[0], t[0])
        groups = collections.defaultdict(lambda: dict(pins=set(), names=set(), nc=False, pts=set()))
        for p in allpts:
            g = groups[f(p)]
            g["pts"].add(p)
            for it in pts.get(p, []):
                if it[0] == "pin":
                    g["pins"].add((it[1], it[2]))
                elif it[0] == "name":
                    g["names"].add(it[1])
                else:
                    g["nc"] = True
        return list(groups.values())

    def autoname(self):
        """Give each wired-but-unnamed group its original net name: on the longest horizontal segment that
        fits the text, else on a vertical segment that fits (text reads upwards)."""
        import symlib
        self.finalize()
        self.unnamed = []
        for g in self.connectivity():
            if g["names"] or g["nc"] or not g["pins"]:
                continue
            nets = {self.net(r, n) for r, n in g["pins"]} - {None}
            if len(nets) != 1 or not self.ctx.autoname_ok(nets):
                continue
            net = nets.pop()
            need = symlib.text_w(net) + 3.0
            ws = [w for w in self.wires if w[0] in g["pts"]]
            hor = sorted([w for w in ws if w[0][1] == w[1][1]], key=lambda w: -abs(w[1][0] - w[0][0]))
            ver = sorted([w for w in ws if w[0][0] == w[1][0]], key=lambda w: -abs(w[1][1] - w[0][1]))
            if hor and abs(hor[0][1][0] - hor[0][0][0]) >= max(need, 10.0):
                a, b = hor[0]
                x0 = min(a[0], b[0]) + 1.27
                self.labels.append((None, net, (rnd(round(x0 / 1.27) * 1.27), a[1]), 0, None))
            elif ver and abs(ver[0][1][1] - ver[0][0][1]) >= need:
                a, b = ver[0]
                y0 = max(a[1], b[1]) - 1.27
                self.labels.append((None, net, (a[0], rnd(round(y0 / 1.27) * 1.27)), 90, None))
            else:
                self.unnamed.append(net)
                self.ctx.warn(f"{self.key}: no room to name {net}")

    def finalize(self):
        """Split wire segments at every connection point lying inside them, drop duplicates."""
        cps = set()
        for a, b in self.wires:
            cps.add(a); cps.add(b)
        for pl in self.parts:
            for n in pl.pins():
                cps.add(pl.p(n))
        for l in self.labels:
            cps.add(l[2])
        for pw in self.powers:
            cps.add(pw[1])
        out = []
        for a, b in self.wires:
            if a == b:
                continue
            inner = []
            for p in cps:
                if p in (a, b):
                    continue
                if a[0] == b[0] == p[0] and min(a[1], b[1]) < p[1] < max(a[1], b[1]):
                    inner.append(p)
                elif a[1] == b[1] == p[1] and min(a[0], b[0]) < p[0] < max(a[0], b[0]):
                    inner.append(p)
            pts = sorted([a, b] + inner, key=lambda p: (p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2)
            for u, v in zip(pts, pts[1:]):
                seg = (min(u, v), max(u, v))
                if seg not in out:
                    out.append(seg)
        self.wires = out

    def junctions(self):
        cnt = collections.Counter()
        for a, b in self.wires:
            cnt[a] += 1; cnt[b] += 1
        pinpts = collections.Counter(pl.p(n) for pl in self.parts for n in pl.pins())
        for pw in self.powers:
            pinpts[pw[1]] += 1
        J = set()
        for p, c in cnt.items():
            k = c + pinpts.get(p, 0)
            if k >= 3:
                J.add(p)
        for p in cnt:
            for s in self.wires:
                (x1, y1), (x2, y2) = s
                if p in s:
                    continue
                if (x1 == x2 == p[0] and min(y1, y2) < p[1] < max(y1, y2)) or \
                   (y1 == y2 == p[1] and min(x1, x2) < p[0] < max(x1, x2)):
                    J.add(p)
        return sorted(J)

    def check(self):
        errs = []
        ends = collections.Counter()
        for a, b in self.wires:
            ends[a] += 1; ends[b] += 1
        anchors = {pl.p(n) for pl in self.parts for n in pl.pins()} | {l[2] for l in self.labels} | \
                  {pw[1] for pw in self.powers} | set(self.ncs)
        for p, c in ends.items():
            if c == 1 and p not in anchors:
                errs.append(f"dangling wire end at {p}")
        for g in self.connectivity():
            nets = {self.net(r, n) for r, n in g["pins"]}
            if g["nc"]:
                if g["pins"] and nets != {None}:
                    errs.append(f"no-connect on connected pin {g['pins']}")
                continue
            if not g["pins"] and not g["names"]:
                continue
            if None in nets:
                errs.append(f"unconnected pin wired: {sorted(g['pins'])}")
            nets.discard(None)
            if len(nets) > 1:
                errs.append(f"short between {sorted(nets)}: {sorted(g['pins'])}")
            if len(g["names"]) > 1:
                errs.append(f"two names on one group {sorted(g['names'])}")
            if nets and g["names"] and g["names"] != nets:
                errs.append(f"name {g['names']} on net {nets}")
            if not g["pins"]:
                errs.append(f"dangling label/power {g['names']} at {sorted(g['pts'])[:2]}")
        # every pin's net must be reachable: a group with pins but no name must contain all pins of that net on this sheet
        groups = self.connectivity()
        for g in groups:
            if g["nc"] or g["names"] or not g["pins"]:
                continue
            nets = {self.net(r, n) for r, n in g["pins"]} - {None}
            for net in nets:
                total = self.ctx.net_pins_total(net)
                if g["pins"] != total:
                    errs.append(f"net {net}: unnamed group {sorted(g['pins'])} but net has {sorted(total)}")
        return errs

    def _isr_pos(self, name, pt, ang):
        import symlib
        w = symlib.text_w(name) + 3.6
        dx = {0: w, 180: -w, 90: 0, 270: 0}[ang]
        dy = {0: 0, 180: 0, 90: -w, 270: w}[ang]
        return rnd(pt[0] + dx), rnd(pt[1] + dy)

    # ---------- output ----------
    def symbol_nodes(self, project, path):
        out = []
        for pl in self.parts:
            out.append(self.ctx.instance_node(self, pl, project, path))
        for i, (net, pt, rot) in enumerate(self.powers):
            out.append(self.ctx.power_node(self, i, net, pt, rot, project, path))
        return out

    def sexpr(self, project, path, root_uuid):
        ctx = self.ctx
        used = {pl.sd.name: pl.sd for pl in self.parts}
        for net, pt, rot in self.powers:
            sd = ctx.power_sym(net); used[sd.name] = sd
        doc = [Sym("kicad_sch"), [Sym("version"), 20250114], [Sym("generator"), "eeschema"],
               [Sym("generator_version"), "9.0"], [Sym("uuid"), uid(self.key, "sheet")],
               [Sym("paper"), self.paper]]
        tb = [Sym("title_block"), [Sym("title"), self.title], [Sym("date"), ctx.date], [Sym("rev"), ctx.rev],
              [Sym("company"), ctx.company]]
        for i, c in enumerate(self.comments, start=1):
            tb.append([Sym("comment"), i, c])
        doc.append(tb)
        doc.append([Sym("lib_symbols")] + [used[k].lib_node() for k in sorted(used)])
        for p in self.junctions():
            doc.append([Sym("junction"), [Sym("at"), p[0], p[1]], [Sym("diameter"), 0], [Sym("color"), 0, 0, 0, 0],
                        [Sym("uuid"), uid(self.key, "j", p)]])
        for p in self.ncs:
            doc.append([Sym("no_connect"), [Sym("at"), p[0], p[1]], [Sym("uuid"), uid(self.key, "nc", p)]])
        for i, (a, b) in enumerate(self.wires):
            doc.append([Sym("wire"), [Sym("pts"), [Sym("xy"), a[0], a[1]], [Sym("xy"), b[0], b[1]]],
                        [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]], [Sym("uuid"), uid(self.key, "w", i, a, b)]])
        for i, (kind, name, pt, ang, shape) in enumerate(self.labels):
            kind = kind or ("global" if ctx.is_global(name) else "local")
            shape = shape or ctx.shape(name, self)
            just = {0: "left", 90: "left", 180: "right", 270: "right"}[ang]
            if kind == "local":
                doc.append([Sym("label"), name, [Sym("at"), pt[0], pt[1], ang],
                            [Sym("effects"), [Sym("font"), [Sym("size"), 1.27, 1.27]], [Sym("justify"), Sym(just), Sym("bottom")]],
                            [Sym("uuid"), uid(self.key, "l", i, name, pt)]])
            else:
                w = 0
                doc.append([Sym("global_label"), name, [Sym("shape"), Sym(shape)], [Sym("at"), pt[0], pt[1], ang],
                            [Sym("fields_autoplaced"), Sym("yes")],
                            [Sym("effects"), [Sym("font"), [Sym("size"), 1.27, 1.27]], [Sym("justify"), Sym(just)]],
                            [Sym("uuid"), uid(self.key, "g", i, name, pt)],
                            [Sym("property"), "Intersheetrefs", "${INTERSHEET_REFS}",
                             [Sym("at")] + list(self._isr_pos(name, pt, ang)) + [0],
                             [Sym("effects"), [Sym("font"), [Sym("size"), 1.27, 1.27]], [Sym("justify"), Sym(just)]]]])
        doc.extend(self.texts)
        doc.extend(self.symbol_nodes(project, path))
        return doc
