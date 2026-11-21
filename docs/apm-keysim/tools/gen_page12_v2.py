"""Generate Hardware-C page 12, revision 2: four APM key channels sharing one 13-bit ladder."""
import html
import json
import sys
from collections import defaultdict

OUT = sys.argv[1]
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None

# b0..b11 as revision 1, b12 = 100 ohm added at the low-resistance end
BITS = [200, 390, 820, 1600, 3300, 6200, 13000, 24000, 47000, 91000, 180000, 360000, 100]
NB = len(BITS)
CH = 4


def fmt_ohm(r):
    if r >= 1000:
        return f"{r / 1000:g}k"
    return str(r)


# ---------------------------------------------------------------- netlist
parts = {}


def add(ref, part, pins):
    parts[ref] = {"part": part, "pins": {str(k): {"pin_name": n, "net": net} for k, (n, net) in pins.items()}}


def gate_net(i):
    return f"WS_G{i}"


u801 = {1: ("INT", None), 2: ("A1", "GND"), 3: ("RESET", "WS_RST_N"), 12: ("GND", "GND"),
        21: ("A0", "GND"), 22: ("SCL", "EXP_SCL"), 23: ("SDA", "EXP_SDA"), 24: ("VCC", "+3V3")}
P1_NETS = ["WS_G8", "WS_G9", "WS_G10", "WS_G11", "WS_SHORT_G", "WS_G12", "CH_A0", "CH_A1"]
for i in range(8):
    u801[4 + i] = (f"P0{i}", f"WS_G{i}")
    u801[13 + i] = (f"P1{i}", P1_NETS[i])
add("U801", "TCA9539PWR", u801)
add("C801", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
add("R814", "10kΩ", {1: ("1", "WS_RST_N"), 2: ("2", "GND")})
for i in range(12):
    add(f"R{801 + i}", f"{fmt_ohm(BITS[i])}Ω 1%", {1: ("1", "WS_NODE"), 2: ("2", f"WS_D{i}")})
add("R831", "100Ω 1%", {1: ("1", "WS_NODE"), 2: ("2", "WS_D12")})
for q in range(6):
    a, b = 2 * q, 2 * q + 1
    add(f"Q{801 + q}", "BSS138DW", {1: ("S1", "GND"), 2: ("G1", f"WS_G{a}"), 3: ("D2", f"WS_D{b}"),
                                    4: ("S2", "GND"), 5: ("G2", f"WS_G{b}"), 6: ("D1", f"WS_D{a}")})
# Q808: half A switches b12, half B inverts WS_RST_N into the channel enable
add("Q808", "BSS138DW", {1: ("S1", "GND"), 2: ("G1", "WS_G12"), 3: ("D2", "CH_EN_N"),
                         4: ("S2", "GND"), 5: ("G2", "WS_RST_N"), 6: ("D1", "WS_D12")})
add("Q807", "AO3400A", {1: ("G", "WS_SHORT_G"), 2: ("S", "GND"), 3: ("D", "WS_NODE")})
for n in range(3):
    pins = {}
    for k in range(4):
        pins[1 + k] = (str(1 + k), f"WS_G{4 * n + k}")
        pins[8 - k] = (str(8 - k), "GND")
    add(f"RN{801 + n}", "100kΩ ×4", pins)
add("R813", "100kΩ", {1: ("1", "WS_SHORT_G"), 2: ("2", "GND")})
add("R832", "100kΩ", {1: ("1", "WS_G12"), 2: ("2", "GND")})

# per channel: J801 -> TVS -> PTC -> photorelay -> WS_NODE; 300k sense tap at the connector side
SENSE_PIN = {1: 12, 2: 14, 3: 15, 4: 11}       # CD74HC4052 A0..A3
SEL_PIN = {1: 4, 2: 5, 3: 6, 4: 7}             # SN74LVC139A 1Y0..1Y3
for k in range(1, CH + 1):
    add(f"D80{k}", "SMF15CA", {1: ("1", f"WS_K{k}"), 2: ("2", "GND")})
    add(f"F80{k}", "PTC 50mA", {1: ("1", f"WS_K{k}"), 2: ("2", f"WS_KF{k}")})
    add(f"U80{k + 1}", "TLP176AM", {1: ("Anode", f"WS_LA{k}"), 3: ("Cathode", f"SEL{k}_N"),
                                     4: ("Drain", "WS_NODE"), 6: ("Drain", f"WS_KF{k}")})
    add(f"R{819 + k}", "330Ω 1%", {1: ("1", "+3V3"), 2: ("2", f"WS_LA{k}")})
    add(f"R{814 + k}", "300kΩ 0.1%", {1: ("1", f"WS_K{k}"), 2: ("2", f"WS_S{k}")})
    add(f"R{826 + k}", "DNP", {1: ("1", f"WS_K{k}"), 2: ("2", "GND")})
add("R819", "100kΩ 0.1%", {1: ("1", "WS_ADC"), 2: ("2", "GND")})
add("C802", "10nF", {1: ("1", "WS_ADC"), 2: ("2", "GND")})
add("R824", "10kΩ", {1: ("1", "+3V3"), 2: ("2", "CH_EN_N")})
add("R825", "100kΩ", {1: ("1", "CH_A0"), 2: ("2", "GND")})
add("R826", "100kΩ", {1: ("1", "CH_A1"), 2: ("2", "GND")})
u806 = {1: ("1G", "CH_EN_N"), 2: ("1A", "CH_A0"), 3: ("1B", "CH_A1"), 8: ("GND", "GND"),
        9: ("2Y3", None), 10: ("2Y2", None), 11: ("2Y1", None), 12: ("2Y0", None),
        13: ("2B", "GND"), 14: ("2A", "GND"), 15: ("2G", "+3V3"), 16: ("VCC", "+3V3")}
for k in range(1, CH + 1):
    u806[SEL_PIN[k]] = (f"1Y{k - 1}", f"SEL{k}_N")
add("U806", "SN74LVC139APWR", u806)
u807 = {1: ("B0", "GND"), 2: ("B2", "GND"), 3: ("COM B", "GND"), 4: ("B3", "GND"), 5: ("B1", "GND"),
        6: ("E", "CH_EN_N"), 7: ("VEE", "GND"), 8: ("GND", "GND"), 9: ("S1", "CH_A1"), 10: ("S0", "CH_A0"),
        13: ("COM A", "WS_ADC"), 16: ("VCC", "+3V3")}
for k in range(1, CH + 1):
    u807[SENSE_PIN[k]] = (f"A{k - 1}", f"WS_S{k}")
add("U807", "CD74HC4052PWR", u807)
add("C803", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
add("C804", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
j801 = {}
for k in range(1, CH + 1):
    j801[2 * k - 1] = (str(2 * k - 1), f"WS_K{k}")
    j801[2 * k] = (str(2 * k), "GND")
add("J801", "KF2EDGR-3.81-8P", j801)

mcu_changes = {"U301": {"87": {"pin_name": "PG2", "net": "WS_RST_N"},
                        "28": {"pin_name": "PC2_C", "net": "WS_ADC"}}}

net_pins = defaultdict(list)
for ref, d in parts.items():
    for p, v in d["pins"].items():
        if v["net"]:
            net_pins[v["net"]].append(f"{ref}.{p}")
for p, v in mcu_changes["U301"].items():
    net_pins[v["net"]].append(f"U301.{p}")
existing = {"GND", "+3V3", "EXP_SCL", "EXP_SDA"}
single = [n for n, ps in net_pins.items() if len(ps) < 2 and n not in existing]
assert not single, single
n_parts = len(parts)
n_dnp = sum(1 for d in parts.values() if d["part"] == "DNP")
n_pins = sum(len(d["pins"]) for d in parts.values())
new_nets = sorted(n for n in net_pins if n not in existing)
netlist_json = json.dumps({"revision": 2, "new_parts": parts, "changed_pins": mcu_changes},
                          ensure_ascii=False, indent=1)

# ---------------------------------------------------------------- SVG
S = []


def cls_attr(cls):
    return f' class="{cls}"' if cls else ""


def ln(x1, y1, x2, y2, cls=""):
    S.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"{cls_attr(cls)}/>')


def pl(pts, cls=""):
    p = " ".join(f"{x},{y}" for x, y in pts)
    S.append(f'<polyline points="{p}" fill="none"{cls_attr(cls)}/>')


def rect(x, y, w, h, cls=""):
    S.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none"{cls_attr(cls)}/>')


def txt(x, y, s, anchor="start", cls="", rot=None):
    t = f' transform="rotate({rot} {x} {y})"' if rot is not None else ""
    S.append(f'<text x="{x}" y="{y}" text-anchor="{anchor}"{cls_attr(cls)}{t}>{html.escape(s)}</text>')


def dot(x, y, cls=""):
    S.append(f'<circle cx="{x}" cy="{y}" r="2.6" fill="currentColor"{cls_attr(cls)}/>')


def gnd(x, y):
    ln(x, y, x, y + 8)
    ln(x - 8, y + 8, x + 8, y + 8)
    ln(x - 5, y + 11, x + 5, y + 11)
    ln(x - 2, y + 14, x + 2, y + 14)


def vcc(x, y, label="+3V3"):
    """Supply bar with the lead going down from (x, y)."""
    ln(x - 8, y, x + 8, y)
    txt(x, y - 5, label, anchor="middle", cls="tiny")


def nc(x, y):
    ln(x - 4, y - 4, x + 4, y + 4)
    ln(x - 4, y + 4, x + 4, y - 4)


def flag(x, y, name, side="right", cls=""):
    """Net label tag. side='right': the wire arrives from the left at (x, y)."""
    w = 8 + 6.6 * len(name)
    if side == "right":
        pts = f"{x},{y} {x + 8},{y - 7} {x + 8 + w},{y - 7} {x + 8 + w},{y + 7} {x + 8},{y + 7}"
        tx = x + 8 + w / 2
    else:
        pts = f"{x},{y} {x - 8},{y - 7} {x - 8 - w},{y - 7} {x - 8 - w},{y + 7} {x - 8},{y + 7}"
        tx = x - 8 - w / 2
    S.append(f'<polygon points="{pts}" fill="none"{cls_attr(cls)}/>')
    txt(tx, y + 4, name, anchor="middle", cls=f"net tiny {cls}".strip())


def res_v(x, y1, y2, ref=None, val=None, side="right", dashed=False):
    """Vertical resistor from y1 to y2 (body in the middle)."""
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 15)
    rect(x - 5, mid - 15, 10, 30, "dashed" if dashed else "")
    ln(x, mid + 15, x, y2)
    tx = x + 10 if side == "right" else x - 10
    anchor = "start" if side == "right" else "end"
    if ref:
        txt(tx, mid - 2, ref, anchor=anchor, cls="ref tiny")
    if val:
        txt(tx, mid + 10, val, anchor=anchor, cls="tiny")


def res_h(x1, x2, y, ref=None, val=None):
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 15, y)
    rect(mid - 15, y - 5, 30, 10)
    ln(mid + 15, y, x2, y)
    if ref:
        txt(mid, y - 9, ref, anchor="middle", cls="ref tiny")
    if val:
        txt(mid, y + 17, val, anchor="middle", cls="tiny")


def cap_v(x, y1, y2, ref, val):
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 3)
    ln(x - 9, mid - 3, x + 9, mid - 3)
    ln(x - 9, mid + 3, x + 9, mid + 3)
    ln(x, mid + 3, x, y2)
    txt(x + 12, mid - 2, ref, cls="ref tiny")
    txt(x + 12, mid + 10, val, cls="tiny")


def nmos_at(x, yc, xg, ytop, ybot, pulldown=True):
    """N-MOS with drain lead from (x, ytop), source lead to (x, ybot), gate at (xg, yc)."""
    xc = x - 8
    pl([(x, ytop), (x, yc - 12), (xc, yc - 12)])
    ln(xc, yc - 16, xc, yc + 16)
    ln(xc - 5, yc - 14, xc - 5, yc + 14)
    pl([(xc, yc + 12), (x, yc + 12), (x, ybot)])
    S.append(f'<polygon points="{xc + 1},{yc + 12} {xc + 7},{yc + 8.5} {xc + 7},{yc + 15.5}" fill="currentColor"/>')
    ln(xc - 5, yc, xg, yc)
    if pulldown:
        ln(xg, yc, xg, yc + 18)
        rect(xg - 4, yc + 18, 8, 24)
        ln(xg, yc + 42, xg, ybot)
        dot(xg, yc)


def tvs_v(x, y1, y2):
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 16)
    S.append(f'<polygon points="{x - 8},{mid - 16} {x + 8},{mid - 16} {x},{mid}" fill="none"/>')
    S.append(f'<polygon points="{x - 8},{mid + 16} {x + 8},{mid + 16} {x},{mid}" fill="none"/>')
    pl([(x - 11, mid - 3), (x - 8, mid), (x + 8, mid), (x + 11, mid + 3)])
    ln(x, mid + 16, x, y2)


def ptc_h(x1, x2, y, ref):
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 18, y)
    rect(mid - 18, y - 7, 36, 14)
    ln(mid - 14, y + 10, mid + 14, y - 10)
    ln(mid + 18, y, x2, y)
    txt(mid, y - 13, ref, anchor="middle", cls="ref tiny")


# --- U301 (partial)
rect(20, 100, 130, 250, "part")
txt(20, 92, "U301", cls="ref")
txt(85, 122, "STM32H743ZIT6", anchor="middle", cls="small muted")
for y, name, c in [(150, "PD12", ""), (180, "PD13", ""), (230, "PG2 · 87", "accent"), (320, "PC2_C · 28", "accent")]:
    txt(144, y + 4, name, anchor="end", cls=f"pin {c}".strip())
    ln(150, y, 175, y, c)
ln(175, 150, 255, 150)
ln(175, 180, 255, 180)
txt(185, 144, "EXP_SCL", cls="net")
txt(185, 174, "EXP_SDA", cls="net")
ln(175, 230, 255, 230, "accent")
txt(182, 224, "WS_RST_N", cls="net accent")
dot(215, 230, "accent")
ln(215, 230, 215, 246)
rect(209, 246, 12, 38)
ln(215, 284, 215, 296)
gnd(215, 296)
txt(226, 262, "R814", cls="ref small")
txt(226, 275, "10k", cls="small")
ln(175, 320, 188, 320, "accent")
flag(188, 320, "WS_ADC", cls="accent")
txt(20, 380, "I²C4 已在用：", cls="small muted")
txt(20, 396, "U111 TUSB320、J302", cls="small muted")
txt(20, 412, "上拉 R304/R305 4.7 kΩ 已有", cls="small muted")

# --- U801 TCA9539
rect(280, 110, 130, 330, "part")
txt(280, 102, "U801  TCA9539PWR", cls="ref")
txt(345, 432, "I²C 0x74", anchor="middle", cls="small muted")
for y, name, num in [(130, "VCC", 24), (150, "SCL", 22), (180, "SDA", 23), (230, "RESET", 3),
                     (330, "A0", 21), (350, "A1", 2), (380, "INT", 1), (420, "GND", 12)]:
    txt(286, y + 4, name, cls="pin")
    txt(276, y - 3, str(num), anchor="end", cls="pinno")
    ln(255, y, 280, y)
pl([(255, 130), (240, 130), (240, 116)])
ln(232, 116, 248, 116)
txt(240, 110, "+3V3", anchor="middle", cls="small")
txt(160, 128, "C801 100nF", cls="small muted")
pl([(255, 330), (255, 350)])
dot(255, 350)
ln(255, 350, 255, 356)
gnd(255, 356)
nc(255, 380)
gnd(255, 420)
RIGHT_LABELS = [f"G{i}" for i in range(12)] + ["SHORT", "G12", "CH_A0", "CH_A1"]
for i in range(16):
    y = 130 + i * 20
    name = f"P0{i}" if i < 8 else f"P1{i - 8}"
    txt(404, y + 4, name, anchor="end", cls="pin")
    txt(414, y - 3, str(4 + i if i < 8 else 5 + i), cls="pinno")
    new = i >= 13
    ln(410, y, 432, y, "accent2" if new else "")
    txt(436, y + 4, RIGHT_LABELS[i], cls="net accent2" if new else "net")

# --- ladder, 13 bits + short
BUS_Y, RAIL_Y = 150, 340
xs = [530 + i * 52 for i in range(NB)]
XS = 1222
ln(470, BUS_Y, 1300, BUS_Y, "bus")
txt(470, 140, "WS_NODE", cls="net")
for i, x in enumerate(xs):
    new = i == 12
    dot(x, BUS_Y)
    txt(x, 134, f"b{i}", anchor="middle", cls="small accent2" if new else "small muted")
    ln(x, BUS_Y, x, 170)
    rect(x - 6, 170, 12, 50, "accent2" if new else "")
    ref = "R831" if new else f"R{801 + i}"
    txt(x - 10, 195, ref, anchor="middle", cls="ref tiny", rot=-90)
    txt(x + 15, 195, fmt_ohm(BITS[i]), anchor="middle", cls="small", rot=-90)
    ln(x, 220, x, 262)
    xg = x - 24
    nmos_at(x, 280, xg, 262, RAIL_Y)
    txt(xg - 3, 275, f"G{i}", anchor="end", cls="net tiny")
    qref = "Q808A" if new else f"Q{801 + i // 2}{'AB'[i % 2]}"
    txt(x - 6, 360, qref, anchor="middle", cls="ref tiny")
dot(XS, BUS_Y)
ln(XS, BUS_Y, XS, 262)
nmos_at(XS, 280, XS - 24, 262, RAIL_Y)
txt(XS - 27, 275, "SHORT", anchor="end", cls="net tiny")
txt(XS - 6, 360, "Q807", anchor="middle", cls="ref tiny")
txt(XS - 6, 373, "AO3400A", anchor="middle", cls="tiny muted")
txt(XS + 8, 214, "0 Ω 直通", cls="tiny muted")
ln(470, RAIL_Y, 1240, RAIL_Y)
gnd(470, RAIL_Y)
txt(500, 398, "Q801–Q806、Q808A = BSS138DW，b0–b12 共 13 位；b12 = 100 Ω 为本版新增", cls="small muted")
txt(500, 414, "栅极下拉 100 kΩ：RN801–RN803（G0–G11）、R832（G12）、R813（SHORT）", cls="small muted")

# --- four channels on the right
NODE_X = 1300
ROWS = [150, 300, 450, 600]
J_X = 1800
ln(NODE_X, BUS_Y, NODE_X, ROWS[-1], "bus")
txt(NODE_X + 6, 236, "WS_NODE", cls="net tiny", rot=90)
rect(J_X, 128, 40, 516, "part")
txt(J_X + 20, 120, "J801", anchor="middle", cls="ref")
txt(J_X + 20, 662, "KF2EDGR-3.81-8P", anchor="middle", cls="tiny muted")
for k, y in enumerate(ROWS, start=1):
    if y != BUS_Y:
        dot(NODE_X, y)
    ln(NODE_X, y, 1340, y)
    # photorelay
    rect(1340, y - 26, 84, 52, "part")
    txt(1382, y - 31, f"U80{k + 1}  TLP176AM", anchor="middle", cls="ref tiny")
    txt(1336, y - 4, "4", anchor="end", cls="pinno")
    txt(1428, y - 4, "6", cls="pinno")
    # switch contacts inside
    ln(1340, y, 1368, y)
    ln(1368, y, 1396, y - 10)
    ln(1398, y, 1424, y)
    # LED inside + arrows
    S.append(f'<polygon points="{1352},{y + 8} {1366},{y + 8} {1359},{y + 18}" fill="none"/>')
    ln(1352, y + 18, 1366, y + 18)
    ln(1370, y + 12, 1378, y + 6)
    ln(1370, y + 17, 1378, y + 11)
    # LED pins: 1 anode, 3 cathode
    pl([(1359, y + 8), (1359, y + 2), (1350, y + 2)])  # cosmetic tie to anode lead
    ln(1350, y + 2, 1350, y + 26)
    ln(1359, y + 18, 1359, y + 22)
    pl([(1359, y + 22), (1396, y + 22), (1396, y + 26)])
    txt(1346, y + 36, "1", anchor="end", cls="pinno")
    txt(1400, y + 36, "3", cls="pinno")
    ln(1350, y + 26, 1350, y + 34)
    res_v(1350, y + 34, y + 80, f"R{819 + k}", "330", side="left")
    ln(1350, y + 80, 1350, y + 86)
    ln(1342, y + 86, 1358, y + 86)
    txt(1350, y + 98, "+3V3", anchor="middle", cls="tiny")
    ln(1396, y + 26, 1396, y + 56)
    flag(1396, y + 56, f"SEL{k}_N", cls="accent")
    # PTC and node
    ln(1424, y, 1450, y)
    ptc_h(1450, 1530, y, f"F80{k}")
    dot(1560, y)
    ln(1530, y, 1560, y)
    tvs_v(1560, y, y + 70)
    gnd(1560, y + 70)
    txt(1572, y + 30, f"D80{k}", cls="ref tiny")
    txt(1572, y + 42, "SMF15CA", cls="tiny")
    # sense tap
    ln(1560, y, J_X, y)
    dot(1630, y)
    res_v(1630, y, y + 62, f"R{814 + k}", "300k 0.1%")
    ln(1630, y + 62, 1630, y + 70)
    flag(1630, y + 78, f"WS_S{k}", cls="accent")
    ln(1630, y + 70, 1630, y + 78)
    # idle resistor footprint
    dot(1720, y)
    res_v(1720, y, y + 62, f"R{826 + k}", "DNP", dashed=True)
    gnd(1720, y + 62)
    txt(1546, y - 10, f"WS_K{k}", anchor="end", cls="net tiny")
    # connector pins
    txt(J_X + 6, y + 4, str(2 * k - 1), cls="pinno")
    ln(J_X - 26, y + 24, J_X, y + 24)
    gnd(J_X - 26, y + 24)
    txt(J_X + 6, y + 28, str(2 * k), cls="pinno")
    txt(J_X + 48, y + 4, f"K{k}", cls="ref small")
    txt(J_X + 48, y + 18, "至 APM 按键", cls="tiny muted")

# --- channel select block (bottom left)
txt(20, 486, "通道选择：同一时刻只接通一路（译码器保证）；PG2 为低时 4 路全部断开", cls="small")

# inverter Q808B: WS_RST_N high -> CH_EN_N low (enabled)
flag(110, 640, "WS_RST_N", side="left", cls="accent")
nmos_at(200, 640, 110, 580, 700, pulldown=False)
gnd(200, 700)
vcc(200, 512)
res_v(200, 512, 580, "R824", "10k")
dot(200, 580)
ln(200, 580, 250, 580)
flag(250, 580, "CH_EN_N")
txt(212, 668, "Q808B", cls="ref tiny")
txt(212, 680, "BSS138DW", cls="tiny muted")
txt(40, 730, "PG2 高 → CH_EN_N 低 → 使能", cls="tiny muted")

# address pull-downs
for y, net, ref in [(540, "CH_A0", "R825"), (660, "CH_A1", "R826")]:
    flag(360, y, net, side="left", cls="accent2")
    ln(360, y, 380, y)
    res_v(380, y, y + 80, ref, "100k")
    gnd(380, y + 80)

# decoder U806
DX0, DX1 = 560, 680
rect(DX0, 525, DX1 - DX0, 210, "part")
txt(DX0, 517, "U806  SN74LVC139APW", cls="ref")
for y, name, num in [(545, "1G", 1), (570, "1A", 2), (595, "1B", 3), (635, "2G", 15),
                     (655, "2A", 14), (675, "2B", 13), (700, "VCC", 16), (720, "GND", 8)]:
    txt(DX0 + 6, y + 4, name, cls="pin")
    txt(DX0 - 4, y - 3, str(num), anchor="end", cls="pinno")
for y, net in [(545, "CH_EN_N"), (570, "CH_A0"), (595, "CH_A1")]:
    ln(520, y, DX0, y)
    flag(520, y, net, side="left", cls="accent2" if net.startswith("CH_A") else "")
ln(540, 635, DX0, 635)
ln(540, 635, 540, 625)
vcc(540, 625)
ln(530, 655, DX0, 655)
ln(530, 675, DX0, 675)
ln(530, 655, 530, 675)
dot(530, 675)
gnd(530, 675)
ln(520, 700, DX0, 700)
txt(516, 704, "+3V3 · C803", anchor="end", cls="tiny muted")
ln(540, 720, DX0, 720)
gnd(540, 720)
for y, name, num, net in [(545, "1Y0", 4, "SEL1_N"), (570, "1Y1", 5, "SEL2_N"),
                          (595, "1Y2", 6, "SEL3_N"), (620, "1Y3", 7, "SEL4_N")]:
    txt(DX1 - 6, y + 4, name, anchor="end", cls="pin")
    txt(DX1 + 4, y - 3, str(num), cls="pinno")
    ln(DX1, y, DX1 + 20, y)
    flag(DX1 + 20, y, net, cls="accent")
for i, (y, num) in enumerate([(655, 12), (675, 11), (695, 10), (715, 9)]):
    txt(DX1 - 6, y + 4, f"2Y{i}", anchor="end", cls="pin")
    txt(DX1 + 4, y - 3, str(num), cls="pinno")
    ln(DX1, y, DX1 + 14, y)
    nc(DX1 + 14, y)

# mux U807
MX0, MX1 = 900, 1030
rect(MX0, 525, MX1 - MX0, 210, "part")
txt(MX0, 517, "U807  CD74HC4052PW", cls="ref")
for y, name, num, net in [(545, "A0", 12, "WS_S1"), (565, "A1", 14, "WS_S2"), (585, "A2", 15, "WS_S3"),
                          (605, "A3", 11, "WS_S4"), (640, "E", 6, "CH_EN_N"), (660, "S0", 10, "CH_A0"),
                          (680, "S1", 9, "CH_A1")]:
    txt(MX0 + 6, y + 4, name, cls="pin")
    txt(MX0 - 4, y - 3, str(num), anchor="end", cls="pinno")
    ln(MX0 - 16, y, MX0, y)
    c = "accent" if net.startswith("WS_S") else ("accent2" if net.startswith("CH_A") else "")
    flag(MX0 - 16, y, net, side="left", cls=c)
for y, name, num in [(545, "COM A", 13), (590, "VCC", 16), (620, "VEE", 7), (640, "GND", 8)]:
    txt(MX1 - 6, y + 4, name, anchor="end", cls="pin")
    txt(MX1 + 4, y - 3, str(num), cls="pinno")
    ln(MX1, y, MX1 + 20, y)
txt(MX1 - 6, 684, "B0–B3、COM B", anchor="end", cls="pin")
txt(MX1 + 4, 677, "1–5", cls="pinno")
ln(MX1, 680, MX1 + 20, 680)
pl([(MX1 + 20, 620), (MX1 + 20, 680)])
dot(MX1 + 20, 640)
dot(MX1 + 20, 680)
gnd(MX1 + 20, 680)
txt(MX1 + 24, 594, "+3V3 · C804", cls="tiny muted")
# WS_ADC: divider bottom + filter
ln(MX1 + 20, 545, 1235, 545)
dot(1145, 545)
res_v(1145, 545, 640, "R819", "100k 0.1%")
gnd(1145, 640)
dot(1215, 545)
cap_v(1215, 545, 610, "C802", "10nF")
gnd(1215, 610)
flag(1235, 545, "WS_ADC", cls="accent")
txt(900, 762, "WS_Sk 经 R815–R818（300 kΩ）来自各路接口；选中一路时 WS_ADC = 接口电压 ÷ 4", cls="tiny muted")
txt(900, 776, "U806 第 2 组和 U807 的 B 段不用：输入接地或接 +3V3，输出悬空", cls="tiny muted")

VB_W, VB_H = 1940, 790
svg = (f'<svg viewBox="0 0 {VB_W} {VB_H}" role="img" aria-label="第 12 页第 2 版原理图：U801 驱动 13 位电阻阵和 0 Ω 直通，'
       '公共节点 WS_NODE 经 4 个 TLP176AM 光耦继电器分别接到 J801 的 K1–K4，每路有 PTC 和 TVS；'
       'SN74LVC139A 译码保证只接通一路，CD74HC4052 把所选通道的 1:4 分压接到 PC2_C。">'
       '<g stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">'
       + "".join(S) + "</g></svg>")

if PREVIEW:
    pv = svg.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ', 1)
    pv = pv.replace('stroke="currentColor"', 'stroke="#111"').replace('fill="currentColor"', 'fill="#111"')
    style = ('<style>text{font-family:Menlo,monospace;font-size:12px;fill:#111;stroke:none}'
             '.small{font-size:11px}.tiny{font-size:10px}.pinno{font-size:9px;fill:#777}.muted{fill:#666}'
             '.accent{stroke:#0b7a6e}text.accent{fill:#0b7a6e;stroke:none}.accent2{stroke:#b4530a}text.accent2{fill:#b4530a;stroke:none}'
             '.bus{stroke-width:2.2}.dashed{stroke-dasharray:3 3}</style>')
    pv = pv.replace('<g ', f'<rect width="{VB_W}" height="{VB_H}" fill="#fff"/>{style}<g ', 1)
    with open(PREVIEW, "w", encoding="utf-8") as f:
        f.write(pv)

# ---------------------------------------------------------------- tables
ladder_rows = []
for i, r in enumerate(BITS):
    port = f"P0{i}" if i < 8 else (f"P1{i - 8}" if i < 12 else "P15")
    pin = 4 + i if i < 8 else (5 + i if i < 12 else 18)
    ref = "R831" if i == 12 else f"R{801 + i}"
    q = "Q808A" if i == 12 else f"Q{801 + i // 2}{'AB'[i % 2]}"
    src = "本版新增" if i == 12 else ("第 1 版新增" if i >= 8 else "apm.md 原值")
    tr_cls = ' class="added"' if i == 12 else ''
    ladder_rows.append(
        f"<tr{tr_cls}><td>b{i}</td><td>{ref}</td><td class=num>{fmt_ohm(r)} Ω</td>"
        f"<td class=num>{1e6 / r:,.1f}</td><td>{q}</td><td>{port} · {pin}</td><td>{src}</td></tr>")
ladder_rows.append("<tr><td>SHORT</td><td>—</td><td class=num>0 Ω</td><td class=num>—</td><td>Q807</td><td>P14 · 17</td><td>AO3400A 直通</td></tr>")

bom = [
    ("U801", "TCA9539PWR", "TI", "TSSOP-24", 1, "16 位 I²C IO 扩展；P15–P17 本版用上"),
    ("Q801–Q806、Q808", "BSS138DW", "Diodes", "SOT-363", 7, "双 N-MOS：13 路电阻开关 + 1 个使能反相"),
    ("Q807", "AO3400A", "AOS", "SOT-23", 1, "0 Ω 直通"),
    ("R801–R812、R831", "100 Ω … 360 kΩ，1%", "", "0603", 13, "电阻阵，取值见下表"),
    ("RN801–RN803", "100 kΩ ×4 排阻", "", "4×0603", 3, "G0–G11 栅极下拉"),
    ("R813、R832", "100 kΩ 1%", "", "0603", 2, "SHORT、G12 栅极下拉"),
    ("R814", "10 kΩ 1%", "", "0603", 1, "WS_RST_N 下拉"),
    ("U802–U805", "TLP176AM", "Toshiba", "4-pin SO6", 4, "光耦继电器，60 V，导通 ≤ 2 Ω，每路一个"),
    ("R820–R823", "330 Ω 1%", "", "0603", 4, "继电器 LED 限流，约 5.5–6 mA"),
    ("U806", "SN74LVC139APWR", "TI", "TSSOP-16", 1, "2-4 译码，输出低有效，只用第 1 组"),
    ("U807", "CD74HC4052PWR", "TI", "TSSOP-16", 1, "双 4 选 1 模拟开关，只用 A 段做回读"),
    ("R824", "10 kΩ 1%", "", "0603", 1, "CH_EN_N 上拉：复位时 4 路全断"),
    ("R825、R826", "100 kΩ 1%", "", "0603", 2, "CH_A0、CH_A1 下拉，复位时输入不悬空"),
    ("R815–R818", "300 kΩ 0.1%", "", "0603", 4, "每路回读分压上臂，取在接口侧"),
    ("R819", "100 kΩ 0.1%", "", "0603", 1, "回读分压下臂（4 路共用）"),
    ("C801、C803、C804", "100 nF X7R", "", "0603", 3, "U801、U806、U807 去耦"),
    ("C802", "10 nF X7R", "", "0603", 1, "回读滤波，靠近 U301.28"),
    ("F801–F804", "PTC 自恢复保险丝", "", "0805/1206", 4, "保持电流 50 mA，耐压 ≥ 15 V，每路一个"),
    ("D801–D804", "SMF15CA", "", "SOD-123FL", 4, "15 V 双向 TVS，每路一个"),
    ("R827–R830", "DNP", "", "0603", 4, "空闲电阻预留位，默认不贴"),
    ("J801", "KF2EDGR-3.81-8P", "", "3.81 mm", 1, "奇数脚 K1–K4，偶数脚 GND"),
]
bom_rows = "".join(
    f"<tr><td>{a}</td><td>{b}</td><td>{c}</td><td>{d}</td><td class=num>{e}</td><td>{f}</td></tr>" for a, b, c, d, e, f in bom)
assert sum(r[4] for r in bom) == n_parts, (sum(r[4] for r in bom), n_parts)

tca_rows = []
for i in range(16):
    port = f"P0{i}" if i < 8 else f"P1{i - 8}"
    pin = 4 + i if i < 8 else 5 + i
    if i < 12:
        use, note = f"b{i}（{fmt_ohm(BITS[i])} Ω）", "电阻阵"
    elif i == 12:
        use, note = "SHORT", "0 Ω 直通"
    elif i == 13:
        use, note = "b12（100 Ω）", "本版新增"
    elif i == 14:
        use, note = "CH_A0", "通道地址低位，本版新增"
    else:
        use, note = "CH_A1", "通道地址高位，本版新增"
    tr = ' class="added"' if i >= 13 else ''
    tca_rows.append(f"<tr{tr}><td>{port} · {pin}</td><td>{use}</td><td>{note}</td></tr>")

PAGE = f"""<title>Hardware-C 车窗按键模拟</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>
/* Layout: one reading column; the schematic breaks out full width with its own horizontal scroll */
:root {{
  --bg: #f5f7f8; --panel: #ffffff; --fg: #15202b; --muted: #5b6875; --line: #d5dce2;
  --accent: #0b7a6e; --accent-soft: #e2f3f0; --accent2: #b4530a; --warn: #9a5b00;
  --sans: "IBM Plex Sans", "PingFang SC", "Noto Sans SC", "Microsoft YaHei", system-ui, sans-serif;
  --mono: "IBM Plex Mono", "SF Mono", Menlo, Consolas, monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  --bg: #0f1418; --panel: #161d23; --fg: #dbe3ea; --muted: #8d9aa6; --line: #2a343d;
  --accent: #3cc9b4; --accent-soft: #12302c; --accent2: #f0a050; --warn: #e0a548; color-scheme: dark; }} }}
:root[data-theme="dark"] {{
  --bg: #0f1418; --panel: #161d23; --fg: #dbe3ea; --muted: #8d9aa6; --line: #2a343d;
  --accent: #3cc9b4; --accent-soft: #12302c; --accent2: #f0a050; --warn: #e0a548; color-scheme: dark; }}
body {{ background: var(--bg); color: var(--fg); font-family: var(--sans); font-size: 15px; line-height: 1.6;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); padding-block: 32px 64px; }}
main {{ max-width: 1120px; margin: 0 auto; display: flex; flex-direction: column; gap: 40px; }}
.prose {{ max-width: 72ch; }}
h1, h2, h3 {{ text-wrap: balance; line-height: 1.25; margin: 0; font-weight: 600; }}
h1 {{ font-size: 1.9rem; }}
h2 {{ font-size: 1.25rem; }}
h3 {{ font-size: 1rem; }}
p {{ margin: 0; }}
section {{ display: flex; flex-direction: column; gap: 14px; min-width: 0; }}
.eyebrow {{ font-family: var(--mono); font-size: .78rem; letter-spacing: .06em; color: var(--muted); text-transform: uppercase; }}
.lede {{ color: var(--muted); }}
.facts {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; }}
.fact {{ border-top: 2px solid var(--line); padding-top: 8px; }}
.fact b {{ display: block; font-family: var(--mono); font-weight: 500; font-size: 1.15rem; font-variant-numeric: tabular-nums; }}
.fact.new {{ border-color: var(--accent); }}
.fact span {{ color: var(--muted); font-size: .88rem; }}
figure {{ margin: 0; background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 12px; }}
.scroll {{ overflow-x: auto; }}
figure svg {{ display: block; width: 100%; min-width: 1400px; height: auto; color: var(--fg); }}
figure svg text {{ font-family: var(--mono); font-size: 12px; fill: currentColor; stroke: none; }}
figure svg .small {{ font-size: 11px; }}
figure svg .tiny {{ font-size: 10px; }}
figure svg .ref {{ font-weight: 500; }}
figure svg .pin {{ font-size: 11px; }}
figure svg .pinno {{ font-size: 9px; fill: var(--muted); }}
figure svg .muted {{ fill: var(--muted); }}
figure svg .net {{ font-size: 11px; }}
figure svg .accent {{ color: var(--accent); stroke: var(--accent); }}
figure svg .accent2 {{ color: var(--accent2); stroke: var(--accent2); }}
figure svg text.accent {{ fill: var(--accent); stroke: none; }}
figure svg text.accent2 {{ fill: var(--accent2); stroke: none; }}
figure svg .bus {{ stroke-width: 2.2; }}
figure svg .dashed {{ stroke-dasharray: 3 3; }}
figcaption {{ color: var(--muted); font-size: .9rem; padding: 10px 4px 2px; max-width: 86ch; }}
.key {{ display: inline-block; width: 18px; height: 3px; vertical-align: middle; margin: 0 4px 0 10px; }}
.table {{ overflow-x: auto; border: 1px solid var(--line); border-radius: 6px; background: var(--panel); }}
table {{ border-collapse: collapse; width: 100%; font-size: .9rem; }}
th, td {{ text-align: left; padding: 7px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }}
th {{ font-weight: 500; color: var(--muted); font-size: .8rem; letter-spacing: .03em; white-space: nowrap; }}
tr:last-child td {{ border-bottom: 0; }}
td:first-child, td.num {{ font-family: var(--mono); white-space: nowrap; }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
tr.added td {{ background: var(--accent-soft); }}
.pill {{ font-family: var(--mono); font-size: .75rem; padding: 1px 7px; border-radius: 999px; border: 1px solid var(--accent); color: var(--accent); white-space: nowrap; }}
.pill.keep {{ border-color: var(--line); color: var(--muted); }}
.grid2 {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 24px; }}
.grid2 > * {{ min-width: 0; }}
ol, ul {{ margin: 0; padding-left: 1.3em; display: flex; flex-direction: column; gap: 8px; }}
code {{ font-family: var(--mono); font-size: .88em; }}
pre {{ font-family: var(--mono); font-size: .8rem; line-height: 1.5; background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 12px 14px; overflow-x: auto; margin: 0; }}
details {{ border: 1px solid var(--line); border-radius: 6px; background: var(--panel); }}
details > summary {{ cursor: pointer; padding: 10px 14px; font-weight: 500; }}
details[open] > summary {{ border-bottom: 1px solid var(--line); }}
details pre {{ border: 0; border-radius: 0; max-height: 420px; }}
.row {{ display: flex; gap: 10px; align-items: center; padding: 10px 14px 0; flex-wrap: wrap; }}
button {{ font: inherit; font-size: .85rem; padding: 4px 12px; border-radius: 5px; border: 1px solid var(--line); background: var(--bg); color: var(--fg); cursor: pointer; }}
button:focus-visible, summary:focus-visible {{ outline: 2px solid var(--accent); outline-offset: 2px; }}
.note {{ color: var(--muted); font-size: .88rem; }}
.warn {{ color: var(--warn); }}
</style>

<main>
<header class="prose" style="display:flex;flex-direction:column;gap:12px">
  <div class="eyebrow">Hardware-C · C/062 之后 · 原理图第 12 页 · 第 2 版</div>
  <h1>APM 车窗按键模拟</h1>
  <p class="lede">第 2 版把模拟口从 1 路扩到 4 路，按“同一时刻只按一路”的要求复用一套电阻阵：4 个光耦继电器把公共节点分别接到 4 个接口，译码器保证任何时候最多接通一路。每路接口侧都有回读，所以任何一路都能做按键学习。</p>
</header>

<div class="facts">
  <div class="fact new"><b>4 路</b><span>共用一套 13 位电阻阵，硬件保证同一时刻只接通一路</span></div>
  <div class="fact"><b>2 个</b><span>主控引脚改动，和第 1 版相同：PG2、PC2_C；PC3_C 仍空着</span></div>
  <div class="fact"><b>{n_parts} 个器件</b><span>其中 {n_dnp} 个默认不贴；{n_pins} 个引脚，{len(new_nets)} 个新网络</span></div>
  <div class="fact"><b>23.9 mV</b><span>200–4600 mV 每 200 mV 的最差误差：1% 电阻随机抽样，每路串联电阻 5–17 Ω 并已自动标定</span></div>
</div>

<figure>
  <div class="scroll">{svg}</div>
  <figcaption>第 12 页第 2 版的全部连接。上半部分是第 1 版的电阻阵加 1 位；右边是 4 路接口，每路依次是 J801、TVS、PTC、光耦继电器，最后接到公共节点 WS_NODE；左下是通道选择。<span class="key" style="background:var(--accent)"></span>主控和通道信号 <span class="key" style="background:var(--accent2)"></span>本版新增的 U801 引脚。同名网络标签相连。</figcaption>
</figure>

<section>
  <h2>4 路怎么切换</h2>
  <div class="grid2">
    <div class="table"><table>
      <thead><tr><th>PG2</th><th>CH_A1</th><th>CH_A0</th><th>接通的通道</th><th>回读</th></tr></thead>
      <tbody>
        <tr><td>低</td><td>×</td><td>×</td><td>全部断开</td><td>0</td></tr>
        <tr><td>高</td><td>0</td><td>0</td><td>K1（U802）</td><td>K1 ÷ 4</td></tr>
        <tr><td>高</td><td>0</td><td>1</td><td>K2（U803）</td><td>K2 ÷ 4</td></tr>
        <tr><td>高</td><td>1</td><td>0</td><td>K3（U804）</td><td>K3 ÷ 4</td></tr>
        <tr><td>高</td><td>1</td><td>1</td><td>K4（U805）</td><td>K4 ÷ 4</td></tr>
      </tbody>
    </table></div>
    <ul class="prose">
      <li>PG2 经 Q808B 反相成 CH_EN_N，同时控制 U806 的 1G 和 U807 的 E。主控复位、上电或 HardFault 时 PG2 被 R814 拉低：U801 复位，电阻阵全关，4 个继电器全断。</li>
      <li>运行时总有一路接在公共节点上。电阻阵不下拉时，这一路只多了 400 kΩ 回读负载，对 APM 的影响约 0.5%。</li>
      <li>换通道时，固件先把电阻阵切到高阻，再改 CH_A0/CH_A1，然后等 5 ms。TLP176AM 断开最长 0.5 ms，接通最长 3 ms，天然先断后通。</li>
      <li>选中后用 0 Ω 直通档量一次接口电压，算出这一路的 PTC + 继电器串联电阻，之后按这个值求解。没有这一步，最差误差约 38 mV。</li>
    </ul>
  </div>
</section>

<section>
  <h2>U801 引脚分配</h2>
  <div class="table"><table>
    <thead><tr><th>端口 · 引脚</th><th>用途</th><th>说明</th></tr></thead>
    <tbody>{''.join(tca_rows)}</tbody>
  </table></div>
</section>

<section>
  <h2>电阻阵</h2>
  <p class="prose">每路串联了 PTC 和继电器，约 5–17 Ω。12 位电阻阵全开时最小约 100 Ω，加上串联电阻后到不了 200 mV 这一档，所以在低阻端加了 b12 = 100 Ω，全开最小约 52 Ω。</p>
  <div class="table"><table>
    <thead><tr><th>位</th><th>电阻</th><th>阻值</th><th>电导 µS</th><th>开关管</th><th>U801 引脚</th><th>来源</th></tr></thead>
    <tbody>{''.join(ladder_rows)}</tbody>
  </table></div>
</section>

<section>
  <h2>和第 1 版相比改了什么</h2>
  <ol class="prose">
    <li><b>1 路扩到 4 路。</b>J801 从 2 脚换成 8 脚。原来接口侧的 F801、D801 和 300 kΩ 分压电阻，变成每路一套（F801–F804、D801–D804、R815–R818）。</li>
    <li><b>每路加一个光耦继电器 TLP176AM。</b>关断耐压 60 V，导通电阻最大 2 Ω，LED 由 U806 直接驱动。</li>
    <li><b>回读改由 4 选 1 模拟开关选通。</b>分压上臂留在各路接口侧，下臂 R819 和 C802 共用，所以选中哪一路，PC2_C 就读到哪一路接口的真实电压，包括原车开关接着时的电压。</li>
    <li><b>电阻阵 12 位加到 13 位</b>（b12 = 100 Ω），用掉 U801 的 P15。P16、P17 做通道地址。</li>
    <li><b>使能改由 PG2 反相得到</b>（Q808B + R824），不再占用 U801 的引脚。</li>
    <li><b>每路预留一个空闲电阻位</b>（R827–R830，默认不贴）。如果某车型的 APM 把开路判成故障，可以在这里贴上原车开关空闲时的阻值。贴了之后，这一路的被动学习读数会偏。</li>
  </ol>
</section>

<div class="grid2">
<section>
  <h2>固件对接</h2>
<pre>// U801 TCA9539 @ 0x74，I2C4 (PD12/PD13)
// 输出寄存器 0x02：P00–P07 = b0–b7
// 输出寄存器 0x03：P10–P13 = b8–b11
//   P14 = SHORT，P15 = b12，P16 = CH_A0，P17 = CH_A1
// 初始化顺序不变：先写输出 0，再设方向

// 选通道 k (0–3)
winsim_set_hiz();                 // 先断开电阻阵
out1 = (out1 &amp; 0x3F) | (k &lt;&lt; 6);
i2c_write(0x74, 0x03, out1);
delay_ms(5);                      // 继电器接通 ≤ 3 ms
// 标定串联电阻
v_open  = adc();                  // 高阻
set_short(1); v_s = adc(); set_short(0);
r_series = 2200 * v_s / (v_open - v_s);

// 被动学习：不下拉，只读 WS_ADC
// 控制字 13 位，查找表 8192 项（32 KB）</pre>
</section>
<section>
  <h2>PCB 摆放</h2>
  <ul>
    <li>J801 放在 J401/J402 同一侧板边，D801–D804、F801–F804 紧挨 J801。</li>
    <li>R815–R818 尽量靠近各自的 PTC 前端，WS_S1–WS_S4 到 U807 的走线远离电阻阵和继电器的输出线。</li>
    <li>U802–U805 排成一列，4 个输出端汇到一条短的 WS_NODE。继电器输入、输出两侧按 5 mm 爬电距离留空。</li>
    <li>R819、C802 贴近 U301 第 28 脚；C803、C804 贴近各自芯片的 VCC 脚。</li>
    <li>J801 的 4 个 GND 用短而宽的线就近接地平面，整块电路远离 U202、U201 的开关节点。</li>
  </ul>
</section>
</div>

<section>
  <h2>主控引脚改动</h2>
  <div class="table"><table>
    <thead><tr><th>U301 引脚</th><th>原状态</th><th>新网络</th><th>用途</th></tr></thead>
    <tbody>
      <tr><td>87 · PG2</td><td>未连接</td><td><span class="pill">WS_RST_N</span></td><td>推挽输出。高电平时 U801 工作、4 路中的一路接通；复位或高阻时 R814 拉低，全部关断。</td></tr>
      <tr><td>28 · PC2_C</td><td>未连接</td><td><span class="pill">WS_ADC</span></td><td>模拟输入 ADC3_INP0。所选通道的接口电压 ÷ 4，满量程约 13.2 V。</td></tr>
      <tr><td>81/82 · PD12/PD13</td><td>EXP_SCL/SDA</td><td><span class="pill keep">不变</span></td><td>I²C4，已有 TUSB320（0x47/0x67）和 J302。U801 地址 0x74，不冲突。</td></tr>
    </tbody>
  </table></div>
</section>

<section>
  <h2>元件清单</h2>
  <div class="table"><table>
    <thead><tr><th>位号</th><th>型号 / 值</th><th>厂商</th><th>封装</th><th>数量</th><th>说明</th></tr></thead>
    <tbody>{bom_rows}</tbody>
  </table></div>
</section>

<section>
  <h2>逐脚网表</h2>
  <p class="note prose">格式和仓库里的 schematic-pins.json 相同。在嘉立创EDA画完后导出网表，可以和这份逐脚对照。</p>
  <details>
    <summary>new_parts + changed_pins（{n_parts} 个器件，{n_pins} 个引脚）</summary>
    <div class="row"><button type="button" id="copy-netlist">复制 JSON</button><span class="note" id="copy-status" aria-live="polite"></span></div>
    <pre id="netlist">{html.escape(netlist_json)}</pre>
  </details>
</section>

<section class="prose">
  <h2>依据和还没验证的</h2>
  <ul>
    <li>已按数据手册核对：TLP176AM（引脚 1 阳极、3 阴极、4/6 输出；60 V，导通 ≤ 2 Ω，触发电流 ≤ 3 mA，接通 ≤ 3 ms，断开 ≤ 0.5 ms）；SN74LVC139A（引脚表、G 和输出都是低有效，3 V 时灌电流 24 mA）；CD74HC4052（引脚表、E 低有效、HC 型 2–6 V 供电，开关钳位电流 ±20 mA）。PC2_C = ADC3_INP0 来自社区引用的数据手册引脚表。</li>
    <li>PTC 的具体料号和阻值还没定，误差计算按 5–17 Ω 估算；选定料号后用实际阻值重算。</li>
    <li>15 V 的 TVS 只按 12 V 系统设计。误接 24 V 时 TVS 会先于 PTC 导通而过热；要支持 24 V 车，TVS 换 SMF33CA，并复核电阻阵的耐压。</li>
    <li>0 mV 这一点受串联电阻限制，直通时约 11–39 mV，仍远低于 APM 的对地短路门限。</li>
    <li>这页还没画进嘉立创EDA，PCB 没更新，没跑 DRC，也没有实板测试。驱动要相应改成 13 位并加上通道选择，这次还没改。</li>
  </ul>
</section>
</main>

<script>
document.getElementById('copy-netlist').addEventListener('click', function () {{
  var pre = document.getElementById('netlist'), status = document.getElementById('copy-status');
  function selectIt() {{ var r = document.createRange(); r.selectNodeContents(pre); var s = window.getSelection(); s.removeAllRanges(); s.addRange(r); status.textContent = '已选中，按 ⌘C 复制'; }}
  try {{
    navigator.clipboard.writeText(pre.textContent).then(function () {{ status.textContent = '已复制'; }}, selectIt);
  }} catch (e) {{ selectIt(); }}
}});
</script>
"""

with open(OUT, "w", encoding="utf-8") as f:
    f.write(PAGE)
print("parts", n_parts, "dnp", n_dnp, "pins", n_pins, "new nets", len(new_nets))
print(new_nets)
