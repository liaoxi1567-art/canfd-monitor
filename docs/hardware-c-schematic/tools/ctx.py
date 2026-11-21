"""Design context: original netlist + part attributes, symbol choice per part, instance/field output,
project writer (root index sheet + one child sheet per original page)."""
import json, os, re, collections
import sx, symlib, sheet as sh
from sx import Sym
from sheet import uid, rnd

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
G = 2.54

RAILS = {"GND": "gnd", "+3V3": "rail", "+3V3A": "rail", "+3V3_SDRAM": "rail", "+3V3_SD": "rail",
         "+3V3_CAN1": "rail", "+3V3_CAN2": "rail", "+5V": "rail", "+5V_LCD": "rail", "+5V_CAN1": "rail",
         "+5V_CAN2": "rail", "VIN_SELECTED": "rail", "USB_VBUS_RAW": "rail", "USB_LIMITED": "rail",
         "EXT_PROTECTED": "rail", "VBAT_RTC": "rail"}

TWO_PIN_Y = {"R", "C", "C_Pol", "L", "FB", "Fuse"}       # pins along library y axis
TWO_PIN_X = {"D", "LED_AK", "TVS_UNI", "TVS_BI", "XTAL", "D_BAS116"}
BODY = {"R": (1.1, 1.1), "C": (2.2, 1.0), "C_Pol": (2.4, 1.0), "L": (1.2, 1.2), "FB": (1.8, 1.2), "Fuse": (1.2, 1.2),
        "D": (1.4, 1.4), "LED_AK": (1.6, 1.6), "TVS_UNI": (1.4, 1.4), "TVS_BI": (1.4, 1.4), "XTAL": (2.4, 2.4),
        "D_BAS116": (1.4, 1.4)}

class Ctx:
    def __init__(self):
        self.pins = json.load(open(f"{DATA}/schematic-pins.json"))
        o = json.load(open(f"{DATA}/orig_parts.json"))
        self.attrs = {r: d["attrs"] for r, d in o["parts"].items()}
        self.orig_page = {r: d["page"] for r, d in o["parts"].items()}
        self.titles = {int(k): v for k, v in o["titles"].items()}
        self.netpins = collections.defaultdict(set)
        for r, d in self.pins.items():
            for n, p in d["pins"].items():
                if p["net"]:
                    self.netpins[p["net"]].add((r, n))
        self.syms = {}
        self.sym_of_ref = {}
        self.warnings = []
        self.sheets = []
        self.date, self.rev, self.company = "2026-11-21", "C062 redraw", "Hardware-C (CANFD monitor)"
        self.shapes = {}
        self.global_nets = set()
        self.placed_refs = collections.Counter()
        import boxes
        self.boxdefs = boxes.BOXES
        self.box_for_ref = boxes.REF_BOX

    def warn(self, s):
        self.warnings.append(s)

    # ---------- netlist ----------
    def net(self, ref, num):
        return self.pins[ref]["pins"][str(num)]["net"]

    def net_pins_total(self, net):
        """pins of `net` that are placed on the sheet being checked (filled by build)."""
        return self._cur_netpins.get(net, set())

    def is_rail(self, net):
        return net in RAILS

    def rail_kind(self, net):
        return RAILS[net]

    def autoname_ok(self, nets):
        return True

    def is_global(self, name):
        return name in self.global_nets

    def shape(self, name, sheet=None):
        d = self.shapes.get(name)
        if d is None:
            return "passive"
        if d == "bi":
            return "bidirectional"
        # d = page number of the driving sheet
        return "output" if sheet is not None and sheet.page == d else "input"

    # ---------- symbols ----------
    def lib(self, key):
        if key in self.syms:
            return self.syms[key]
        S = symlib.stock
        f = {
            "R": lambda: S("Device", "R"),
            "C": lambda: S("Device", "C"),
            "C_Pol": lambda: S("Device", "C_Polarized", newname="C_Pol"),
            "L": lambda: S("Device", "L"),
            "FB": lambda: S("Device", "FerriteBead", newname="FB"),
            "Fuse": lambda: S("Device", "Fuse"),
            "TP": lambda: S("Connector", "TestPoint", newname="TP"),
            "NMOS": lambda: S("Transistor_FET", "Q_NMOS_GSD", newname="NMOS"),
            "BAT": lambda: S("Device", "Battery_Cell", newname="BAT"),
            "SPDT": lambda: S("Switch", "SW_SPDT", newname="SPDT"),
            "XTAL4": lambda: S("Device", "Crystal_GND24", newname="XTAL4"),
            "XTAL": lambda: S("Device", "Crystal", newname="XTAL"),
            "LED_AK": lambda: S("Device", "LED", newname="LED_AK", renumber={"1": "2", "2": "1"}),
            "D_BAS116": lambda: S("Device", "D", newname="D_BAS116", renumber={"1": "3", "2": "1"},
                                  extra_pins=[symlib.pin_node("2", "NC", 0, -3.81, 90, 2.54, "no_connect")]),
            "TVS_UNI": lambda: S("Device", "D_Zener", newname="TVS_UNI"),
            "TVS_BI": lambda: S("Device", "D_TVS", newname="TVS_BI"),
            "TVS_DUAL": lambda: S("Device", "D_TVS_Dual_AAC", newname="TVS_DUAL"),
            "TERM3": lambda: S("Connector", "Screw_Terminal_01x03", newname="TERM3"),
            "TERM2": lambda: S("Connector", "Screw_Terminal_01x02", newname="TERM2"),
            "CONN2x5": lambda: S("Connector_Generic", "Conn_02x05_Odd_Even", newname="CONN2x5"),
            "CONN1x8": lambda: S("Connector_Generic", "Conn_01x08", newname="CONN1x8"),
            "CONN1x16": lambda: S("Connector_Generic", "Conn_01x16", newname="CONN1x16"),
        }.get(key)
        if f:
            sd = f()
        elif key in self.boxdefs:
            sd = self.boxdefs[key]()
        else:
            raise KeyError(key)
        sd.key = key
        self.syms[key] = sd
        return sd

    def default_key(self, ref):
        if ref in self.box_for_ref:
            return self.box_for_ref[ref]
        fixed = {"C704": "C_Pol", "C716": "C_Pol", "Y301": "XTAL4", "Y302": "XTAL", "D301": "LED_AK",
                 "D303": "D_BAS116", "D101": "TVS_UNI", "D701": "TVS_BI", "D411": "TVS_DUAL", "D421": "TVS_DUAL",
                 "J401": "TERM3", "J402": "TERM3", "J103": "TERM2", "J301": "CONN2x5", "J302": "CONN1x8",
                 "J501": "CONN1x16", "F701": "Fuse", "SW101": "SPDT", "BT301": "BAT"}
        if ref in fixed:
            return fixed[ref]
        m = re.match(r"[A-Z]+", ref).group(0)
        return {"R": "R", "C": "C", "L": "L", "FB": "FB", "TP": "TP", "Q": "NMOS"}[m]

    def symbol_for(self, ref, key=None):
        key = key or self.default_key(ref)
        sd = self.lib(key)
        # every original pin must exist in the symbol
        want = set(self.pins[ref]["pins"])
        have = {n for (u, n) in sd.pins}
        if want != have:
            raise ValueError(f"{ref}: symbol {key} pins {sorted(have)} != original {sorted(want)}")
        self.sym_of_ref[ref] = key
        return sd

    def power_sym(self, net):
        key = "PWR:" + net
        if key not in self.syms:
            sd = symlib.power(net, RAILS[net]); sd.key = key
            self.syms[key] = sd
        return self.syms[key]

    # ---------- part attributes ----------
    def value_of(self, ref):
        a = self.attrs[ref]
        v = (a.get("Value") or "").strip()
        mpn = (a.get("Manufacturer Part") or "").strip()
        if not v or v.startswith("={"):
            v = mpn or a.get("Name") or ""
        return v

    def dnp(self, ref):
        a = self.attrs[ref]
        return str(a.get("Add into BOM", "yes")).lower() in ("no", "false", "0") or "DNP" in (a.get("Value") or "")

    # ---------- output ----------
    def _prop(self, key, val, x, y, ang, hide=False, size=1.27, justify=None, bold=False):
        font = [Sym("font"), [Sym("size"), size, size]]
        if bold:
            font.append([Sym("bold"), Sym("yes")])
        eff = [Sym("effects"), font]
        if justify:
            eff.append([Sym("justify")] + [Sym(j) for j in justify.split()])
        if hide:
            eff.append([Sym("hide"), Sym("yes")])
        return [Sym("property"), key, val, [Sym("at"), rnd(x), rnd(y), ang], eff]

    def field_pos(self, pl):
        """returns ((rx, ry), (vx, vy), hide_value) for centred, horizontal Reference / Value text."""
        sd, key = pl.sd, pl.sd.key
        ref, val = pl.ref, self.value_of(pl.ref) + (" DNP" if self.dnp(pl.ref) and "DNP" not in self.value_of(pl.ref) else "")
        wr, wv = symlib.text_w(ref), symlib.text_w(val)
        mode = pl.field_mode
        x, y = pl.x, pl.y
        if key in TWO_PIN_Y or key in TWO_PIN_X:
            p1, p2 = list(pl.pins())[:2]
            a, b = pl.p(p1), pl.p(p2)
            vertical = a[0] == b[0]
            across, along = BODY[key]
            if vertical:
                side = mode if mode in ("L", "R") else "R"
                s = 1 if side == "R" else -1
                rx = x + s * (across + 0.6 + wr / 2)
                vx = x + s * (across + 0.6 + wv / 2)
                return (rx, y - 1.27), (vx, y + 1.27)
            side = mode if mode in ("U", "D") else "UD"
            if side == "UD":
                return (x, y - across - 1.5), (x, y + across + 1.5)
            s = -1 if side == "U" else 1
            return (x, y + s * (across + 1.5 + (2.6 if s < 0 else 0))), (x, y + s * (across + 1.5 + (0 if s < 0 else 2.6)))
        if key == "NMOS":
            return (x + 4.0 + wr / 2, y - 1.27), (x + 4.0 + wv / 2, y + 1.27)
        if key == "TP":
            return (x + 1.6 + wr / 2, y - 2.4), (x + 1.6 + wv / 2, y - 0.2)
        if key == "BAT":
            return (x + 3.2 + wr / 2, y - 0.4), (x + 3.2 + wv / 2, y + 2.0)
        if key == "SPDT":
            return (x, y - 5.0), (x, y + 5.0)
        if key == "TVS_DUAL":
            if pl.rot in (90, 270):
                s = -1 if mode == "L" else 1
                return (x + s * (2.4 + wr / 2), y - 2.54), (x + s * (2.4 + wv / 2), y + 2.54)
            return (x, y - 4.2), (x, y + 5.6)
        if hasattr(sd, "geo"):
            g = sd.geo[pl.unit]
            top = y - g["top"]; bot = y - g["bot"]
            lx, rx_ = x - g["hw"], x + g["hw"]
            has_top = any(sd.pin(pl.unit, n)["angle"] == 270 for n in pl.pins())
            has_bot = any(sd.pin(pl.unit, n)["angle"] == 90 for n in pl.pins())
            if isinstance(mode, tuple):
                return mode
            if mode == "BL":
                xr_ = lx - 0.8 - max(wr, wv) / 2
                return (xr_, bot + 1.4), (xr_, bot + 3.8)
            if has_top:
                xr_ = lx - 0.8 - max(wr, wv) / 2 if mode != "TR" else rx_ + 0.8 + max(wr, wv) / 2
                rp, vp = (xr_, top - 3.6), (xr_, top - 1.2)
            else:
                rp, vp = (lx + wr / 2, top - 3.6), (lx + wv / 2, top - 1.2)
            return rp, vp
        if key in ("TERM3", "TERM2", "CONN1x8", "CONN1x16", "CONN2x5"):
            pts = [pl.p(n) for n in pl.pins()]
            ys = [p[1] for p in pts]; xs = [p[0] for p in pts]
            top, bot = min(ys) - 2.54, max(ys) + 2.54
            if key == "CONN2x5":
                return (x + 1.27, top - 1.0), (x + 1.27, bot + 1.0)
            if mode == "below":
                return (x - 2.0 + wr / 2, top - 1.0), (x - 2.0 + wv / 2, bot + 1.0)
            return (x + 2.5 + wr / 2, top - 1.0), (x + 2.5 + wv / 2, top + 1.4)
        # generic: library default (rot 0) positions
        rp, vp = sd.prop("Reference"), sd.prop("Value")
        ra, va = sx.find(rp, "at"), sx.find(vp, "at")
        dx, dy = sh.xform(float(ra[1]), float(ra[2]), pl.rot, pl.mirror)
        ex, ey = sh.xform(float(va[1]), float(va[2]), pl.rot, pl.mirror)
        return (x + dx, y + dy), (x + ex, y + ey)

    def instance_node(self, sheet, pl, project, path):
        ref = pl.ref
        self.placed_refs[ref] += 1
        a = self.attrs[ref]
        (rx, ry), (vx, vy) = self.field_pos(pl)
        fang = pl.rot % 180
        val = self.value_of(ref)
        dnp = self.dnp(ref)
        node = [Sym("symbol"), [Sym("lib_id"), "hwc:" + pl.sd.name], [Sym("at"), pl.x, pl.y, pl.rot]]
        if pl.mirror:
            node.append([Sym("mirror"), Sym(pl.mirror)])
        node += [[Sym("unit"), pl.unit], [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")],
                 [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("yes" if dnp else "no")],
                 [Sym("uuid"), uid(sheet.key, "sym", ref, pl.unit)]]
        hide_val = getattr(pl, "hide_value", False) or pl.sd.key == "TP"
        node.append(self._prop("Reference", ref, rx, ry, fang))
        node.append(self._prop("Value", val, vx, vy, fang, hide=hide_val))
        node.append(self._prop("Footprint", a.get("Footprint") or "", pl.x, pl.y, 0, hide=True))
        node.append(self._prop("Datasheet", "", pl.x, pl.y, 0, hide=True))
        node.append(self._prop("Description", a.get("Description") or "", pl.x, pl.y, 0, hide=True))
        mpn = (a.get("Manufacturer Part") or "").strip()
        if mpn:
            node.append(self._prop("MPN", mpn, pl.x, pl.y, 0, hide=True))
        if a.get("Manufacturer"):
            node.append(self._prop("Manufacturer", a["Manufacturer"], pl.x, pl.y, 0, hide=True))
        for num in pl.pins():
            node.append([Sym("pin"), num, [Sym("uuid"), uid(sheet.key, "pin", ref, num)]])
        node.append([Sym("instances"), [Sym("project"), project, [Sym("path"), path, [Sym("reference"), ref], [Sym("unit"), pl.unit]]]])
        return node

    def power_node(self, sheet, i, net, pt, rot, project, path):
        sd = self.power_sym(net)
        sheet.npwr += 1
        pref = f"#PWR{sheet.page:02d}{sheet.npwr:03d}"
        w = symlib.text_w(net)
        if RAILS[net] == "gnd":
            vx, vy = (pt[0], pt[1] + 4.6) if rot == 0 else (pt[0], pt[1] - 4.6)
        elif rot == 90:
            vx, vy = pt[0] - 4.2 - w / 2, pt[1]
        elif rot == 270:
            vx, vy = pt[0] + 4.2 + w / 2, pt[1]
        else:
            vx, vy = (pt[0], pt[1] - 4.0) if rot == 0 else (pt[0], pt[1] + 4.0)
        node = [Sym("symbol"), [Sym("lib_id"), "hwc:" + sd.name], [Sym("at"), pt[0], pt[1], rot], [Sym("unit"), 1],
                [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")],
                [Sym("dnp"), Sym("no")], [Sym("uuid"), uid(sheet.key, "pwr", i, net, pt)],
                self._prop("Reference", pref, pt[0], pt[1], 0, hide=True),
                self._prop("Value", net, vx, vy, rot % 180, hide=(net == "GND")),
                self._prop("Footprint", "", pt[0], pt[1], 0, hide=True),
                self._prop("Datasheet", "", pt[0], pt[1], 0, hide=True),
                self._prop("Description", "", pt[0], pt[1], 0, hide=True),
                [Sym("pin"), "1", [Sym("uuid"), uid(sheet.key, "pwrpin", i, net, pt)]],
                [Sym("instances"), [Sym("project"), project, [Sym("path"), path, [Sym("reference"), pref], [Sym("unit"), 1]]]]]
        return node
