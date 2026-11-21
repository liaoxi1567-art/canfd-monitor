"""Layout helpers shared by the page scripts."""
G = 2.54

def g(n):
    return n * G

def vpart(s, ref, x, ytop, top="1", fields="auto"):
    """2-pin part along y (R, C, L, FB, Fuse): pin `top` at (x, ytop)."""
    rot = 0 if top == "1" else 180
    return s.place(ref, x, ytop + 3.81, rot=rot, fields=fields)

def hpart(s, ref, xleft, y, left="1", fields="auto", span=3.81):
    """2-pin part along x: pin `left` at (xleft, y). Works for R/C/L/FB (rot 90/270)."""
    rot = 90 if left == "1" else 270
    return s.place(ref, xleft + span, y, rot=rot, fields=fields)

def hdiode(s, ref, xleft, y, left="1", fields="auto"):
    """diode-like parts whose pins lie on the library x axis."""
    sd = s.ctx.symbol_for(ref)
    p = sd.pin(1, left)
    rot = 0 if p["x"] < 0 else 180
    return s.place(ref, xleft - (p["x"] if rot == 0 else -p["x"]), y, rot=rot, fields=fields)

def vdiode(s, ref, x, ytop, top="1", fields="auto"):
    sd = s.ctx.symbol_for(ref)
    p = sd.pin(1, top)
    # rot 270: library -x -> screen up ; rot 90: library +x -> screen up
    rot = 270 if p["x"] < 0 else 90
    return s.place(ref, x, ytop + abs(p["x"]), rot=rot, fields=fields)

def shunt(s, ref, node, x=None, top=None, drop=0.0, fields="auto"):
    """Vertical part hanging below point `node` (x, y) with its other pin terminated (usually GND).
    top: which pin connects to the node; default = the pin whose net is not GND."""
    if top is None:
        top = "1" if s.net(ref, "1") != "GND" else "2"
    other = "2" if top == "1" else "1"
    x = node[0] if x is None else x
    pl = vpart(s, ref, x, node[1] + drop, top=top, fields=fields)
    if drop or x != node[0]:
        s.wire(node, (ref, top))
    else:
        s.touched.add((ref, top))
    s.term((ref, other), length=0)
    return pl

def rail(s, y, x1, x2):
    s.wire((x1, y), (x2, y))

def note(s, x, y, title, lines, size=1.5):
    """Text block: bold title then lines (each line a string)."""
    s.text(x, y, title, size=2.0, bold=True, color=(0, 60, 120, 1))
    yy = y + 4.2
    for ln in lines:
        s.text(x, yy, ln, size=size, color=(40, 40, 40, 1))
        yy += size * 1.75
    return yy

def nmos_driver(s, Q, Rser, Rpd, drain_node, depth=None, label_len=None):
    """Low-side N-MOSFET with drain wired up to drain_node, gate driven through Rser from a label,
    Rpd from gate to GND, source to GND."""
    depth = g(4) if depth is None else depth
    dx, dy = drain_node[0], drain_node[1] + depth
    q = s.place(Q, dx - 2.54, dy + 5.08)
    s.wire(drain_node, (Q, "3"))
    s.term((Q, "2"), length=0)
    gx, gy = q.p("1")
    nx = gx - g(2)
    s.wire((Q, "1"), (nx, gy))
    shunt(s, Rpd, (nx, gy), fields="L")
    hpart(s, Rser, nx - g(7) - 7.62, gy)
    s.wire((Rser, "2"), (nx, gy))
    s.term((Rser, "1"), length=label_len or g(2))
    return q

def bank(s, refs, x0, y, pitch=None, rail=None, lead=g(2)):
    """Vertical parts in a row hanging from a horizontal rail at y; their other pins to GND/power.
    The rail power symbol sits `lead` left of the first part."""
    pitch = pitch or g(5)
    xs = []
    top = None
    for i, r in enumerate(refs):
        x = x0 + i * pitch
        t = "1" if s.net(r, "1") != "GND" else "2"
        top = top or t
        vpart(s, r, x, y, top=t)
        s.touched.add((r, t))
        s.term((r, "2" if t == "1" else "1"), length=0)
        xs.append(x)
    rail = rail or s.net(refs[0], top)
    s.wire((x0 - lead, y), (xs[-1], y))
    s.power((x0 - lead, y), rail)
    return xs

def tie(s, ref, pins, length=g(1), where="end"):
    """Stub a set of same-side pins outwards by `length`, join them, and terminate once."""
    pl = s.placed_for(ref, pins[0])
    ov = pl.out(pins[0])
    pts = []
    for p in pins:
        a = s.pp(ref, p)
        b = (round((a[0] + ov[0] * length) * 100) / 100, round((a[1] + ov[1] * length) * 100) / 100)
        s.wire((ref, p), b)
        pts.append(b)
    vertical_join = ov[0] != 0
    pts.sort(key=lambda p: p[1] if vertical_join else p[0])
    if len(pts) > 1:
        s.wire(pts[0], pts[-1])
    net = s.net(ref, pins[0])
    if net is None:
        return pts
    if s.ctx.is_rail(net):
        gnd = s.ctx.rail_kind(net) == "gnd"
        if vertical_join:
            end = pts[-1] if gnd else pts[0]
            s.power(end, net, d="D" if gnd else "U")
        else:
            mid = pts[len(pts) // 2]
            e2 = (mid[0], mid[1] + ov[1] * g(1))
            s.wire(mid, e2)
            s.power(e2, net, d="D" if ov[1] > 0 else "U")
    else:
        s.label(pts[0], net, "L" if ov[0] < 0 else ("R" if ov[0] > 0 else ("U" if ov[1] < 0 else "D")))
    return pts

def hrow(s, ref, x, y, left="1", llen=g(2), rlen=g(2)):
    """Horizontal 2-pin part at (x = left pin) with both ends terminated (labels / power symbols)."""
    hpart(s, ref, x, y, left=left)
    right = "2" if left == "1" else "1"
    s.term((ref, left), length=llen)
    s.term((ref, right), length=rlen)

def series_pd(s, Rser, Rpd, pin, node_dx=g(8), label_len=g(2)):
    """pin <- node (Rpd to GND) <- Rser <- label. pin is (ref, num) on a left side."""
    p = s.pp(*pin)
    node = (p[0] - node_dx, p[1])
    s.wire(pin, node)
    shunt(s, Rpd, node, fields="L")
    hpart(s, Rser, node[0] - g(3) - 7.62, p[1])
    s.wire((Rser, "2"), node)
    s.term((Rser, "1"), length=label_len)

def load_switch(s, U, Cin, Rpd, Couts, x, y):
    """TPS22919 block: IN with Cin, ON from label with pull-down, VOUT+QOD to output rail with caps."""
    u = s.place(U, x, y)
    xi = u.p("1")[0]
    s.wire((U, "1"), (xi - g(9), y))
    s.power((xi - g(9), y), s.net(U, "1"))
    shunt(s, Cin, (xi - g(5), y), fields="L")
    yon = u.p("3")[1]
    s.wire((U, "3"), (xi - g(4), yon))
    shunt(s, Rpd, (xi - g(4), yon), fields="L")
    s.wire((xi - g(4), yon), (xi - g(7), yon))
    s.label((xi - g(7), yon), s.net(U, "3"), "L")
    s.nc((U, "4"))
    xo = u.p("6")[0]
    s.wire((U, "5"), (xo + g(1), u.p("5")[1]), (xo + g(1), y))
    xe = xo + g(4) + g(4) * len(Couts) + g(1)
    s.wire((U, "6"), (xe, y))
    for i, c in enumerate(Couts):
        shunt(s, c, (xo + g(4) + g(4) * i, y))
    s.power((xe, y), s.net(U, "6"))
    s.term((U, "2"), length=0)
    return u
