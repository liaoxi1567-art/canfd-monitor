"""Page layouts. Each pNN(s) places the parts of original page NN and wires them."""
from lay import g, vpart, hpart, hdiode, vdiode, shunt
G = 2.54

PAPER = {5: "A4"}

def can_channel(s, ch, Ux, Uy):
    U = f"U40{ch}"
    R = lambda k: f"R4{ch}{k}"
    Cc = lambda k: f"C4{ch}{k}"
    u = s.place(U, Ux, Uy, fields="BL")
    # MCU side: series resistors and labels
    for pin, r in (("8", R(6)), ("1", R(4)), ("4", R(5))):
        y = u.p(pin)[1]
        hpart(s, r, Ux - g(19) - 3.81, y)
        s.wire((r, "2"), (U, pin))
        s.term((r, "1"), length=g(2))
    # VIO rail (+3V3 -> R413 0R -> +3V3_CANx)
    yv = Uy - g(7)
    xvio = u.p("5")[0]
    s.wire((U, "5"), (xvio, yv))
    hpart(s, R(3), Ux - g(23), yv)
    s.term((R(3), "1"), length=g(1))
    s.wire((R(3), "2"), (xvio, yv))
    s.power((xvio, yv), f"+3V3_CAN{ch}")
    vpart(s, R(1), Ux - g(9), yv, fields="L")
    s.touched.add((R(1), "1"))
    ystb = u.p("8")[1]
    s.wire((R(1), "2"), (Ux - g(9), ystb))
    shunt(s, Cc(2), (Ux - g(5), yv))
    # VCC rail (+5V -> FB -> +5V_CANx)
    yc = Uy - g(12)
    xvcc = u.p("3")[0]
    s.wire((U, "3"), (xvcc, yc))
    hpart(s, f"FB4{ch}1", Ux - g(23), yc)
    s.wire((f"FB4{ch}1", "2"), (Ux + g(13), yc))
    s.power((xvcc, yc), f"+5V_CAN{ch}")
    shunt(s, Cc(3), (Ux + g(5), yc))
    shunt(s, Cc(1), (Ux + g(9), yc))
    tp = f"TP21{ch - 1}"
    s.place(tp, Ux + g(13), yc)
    s.touched.add((tp, "1"))
    if ch == 1:
        # shared +5V bulk capacitor at the ferrite input
        xin = Ux - g(27)
        s.wire((f"FB4{ch}1", "1"), (xin, yc))
        s.power((xin, yc), "+5V")
        shunt(s, "C430", (xin - g(0), yc), x=xin - g(3), drop=0)
    else:
        s.term((f"FB4{ch}1", "1"), length=g(1))
    s.term((U, "2"), length=0)
    # bus side
    yh, yl = u.p("7")[1], u.p("6")[1]
    xt = Ux + g(12)
    vpart(s, R(2), xt, yh)
    vpart(s, R(7), xt, yl - 7.62)
    s.touched.add((R(2), "1")); s.touched.add((R(7), "2"))
    s.wire((R(2), "2"), (R(7), "1"))
    ym = s.pp(R(2), "2")[1]
    s.wire((xt, ym), (xt - g(1), ym)); s.label((xt - g(1), ym), f"CAN{ch}_TERM", "L")
    D = f"D4{ch}1"
    vdiode(s, D, Ux + g(18), yh, top="1")
    s.touched.add((D, "1")); s.touched.add((D, "2"))
    s.term((D, "3"), length=g(1))
    J = f"J40{ch}"
    s.place(J, Ux + g(28), yh + g(1))
    s.wire((U, "7"), (J, "1"))
    xj = Ux + g(23)
    s.wire((U, "6"), (xj, yl), (xj, yh + g(1)), (J, "2"))
    s.term((J, "3"), length=g(1))
    s.label((Ux + g(20), yh), f"CAN{ch}_H")
    s.label((Ux + g(20), yl), f"CAN{ch}_L")

def p05(s):
    can_channel(s, 1, g(37), g(23))
    can_channel(s, 2, g(37), g(53))
    note(s, g(78), g(10), "双路 CAN FD", [
        "U401 / U402 TJA1042T/3：VCC 5 V 经磁珠 FB411 / FB421，",
        "  VIO 3.3 V 经 0 Ω（R413 / R423）",
        "TXD / RXD / STB 各串 33 Ω；STB 10 kΩ 上拉到 VIO，",
        "  MCU 不驱动时收发器待机",
        "D411 / D421 PESD2CANFD24V 双向 TVS",
        "终端：R412 / R422 120 Ω 已贴，R417 / R427 0 Ω 默认",
        "  不贴，贴上后 CANH–CANL 之间才接入 120 Ω",
        "J401 / J402 端子、TP210 / TP211 默认不贴",
    ])

from lay import note, nmos_driver
PAPER.update({8: "A4", 9: "A4"})

def p08(s):
    Uy, Ux = g(16), g(60)
    u = s.place("U106", Ux, Uy)
    # input connector, fuse, TVS, input caps
    j = s.place("J103", g(9), Uy, mirror="y", fields="below")
    s.term(("J103", "2"), length=g(1))
    hpart(s, "F701", g(16), Uy)
    s.wire(("J103", "1"), ("F701", "1"))
    xin = u.p("1")[0]
    s.wire(("F701", "2"), ("U106", "1"))
    s.wire(("U106", "2"), (xin - g(1), u.p("2")[1]), (xin - g(1), Uy))
    vdiode(s, "D701", g(24), Uy, top="1"); s.touched.add(("D701", "1")); s.term(("D701", "2"), length=0)
    shunt(s, "C703", (g(33), Uy))
    shunt(s, "C702", (g(38), Uy))
    # UVLO / OVP divider to EFUSE_RTN
    xr = Ux - g(14)
    vpart(s, "R701", xr, Uy, fields="L"); s.touched.add(("R701", "1"))
    s.wire(("R701", "2"), ("U106", "3"))
    vpart(s, "R702", xr, u.p("3")[1], fields="L"); s.touched.add(("R702", "1"))
    s.wire(("R702", "2"), ("U106", "5"))
    vpart(s, "R703", xr, u.p("5")[1], fields="L"); s.touched.add(("R703", "1"))
    s.wire(("U106", "6"), ("R703", "2"))
    for n in ("7", "4", "13", "10"):
        s.nc(("U106", n))
    # EFUSE_RTN rail under the chip
    ybot = u.p("12")[1]
    yr = ybot + g(6) + 7.62
    shunt_top = lambda ref, pin: (vpart(s, ref, u.p(pin)[0], ybot), s.touched.add((ref, "1")))
    vpart(s, "C701", u.p("12")[0], ybot + g(6), fields="L"); s.wire(("U106", "12"), ("C701", "1"))
    vpart(s, "R704", u.p("11")[0], ybot + g(6)); s.wire(("U106", "11"), ("R704", "1"))
    s.wire(("R703", "2"), (xr, yr), (u.p("17")[0], yr))
    s.wire(("U106", "8"), (u.p("8")[0], yr))
    s.wire(("U106", "17"), (u.p("17")[0], yr))
    s.touched.add(("C701", "2")); s.touched.add(("R704", "2"))
    s.touched.add(("U106", "12")); s.touched.add(("U106", "11"))
    s.label((xr + g(2), yr), "EFUSE_RTN")
    s.term(("U106", "9"), length=0)
    # output
    xo = u.p("15")[0]
    s.wire(("U106", "16"), (xo + g(1), u.p("16")[1]), (xo + g(1), Uy))
    s.wire(("U106", "15"), (Ux + g(28), Uy))
    s.power((Ux + g(28), Uy), "EXT_PROTECTED")
    shunt(s, "C704", (Ux + g(17), Uy))
    shunt(s, "C705", (Ux + g(22), Uy))
    # fault pull-up
    yf = u.p("14")[1]
    vpart(s, "R705", Ux + g(12), yf - 7.62, top="2")
    s.term(("R705", "2"), length=0)
    s.wire(("U106", "14"), (Ux + g(15), yf))
    s.touched.add(("R705", "1"))
    s.label((Ux + g(15), yf), "EXT_FAULT_N")
    note(s, g(8), g(58), "外部 5 V / 12 V 输入保护", [
        "J103（默认不贴）→ F701 2 A 保险丝 → D701 SMCJ33CA 双向 TVS",
        "U106 TPS26600 电子保险丝：UVLO / OVP 由 R701–R703 分压设定，",
        "  限流由 R704 6.81 kΩ 设定，上电斜率由 C701 47 nF 设定",
        "输出 EXT_PROTECTED 去第 10 页电源选择",
        "EXT_FAULT_N 只接了 R705 上拉，没有接到 MCU",
    ])

def p09(s):
    ym = g(14)
    Ux = g(40)
    u = s.place("U112", Ux, ym)
    s.power((g(8), ym), "USB_VBUS_RAW")
    s.wire((g(8), ym), ("U112", "1"))
    shunt(s, "C706", (g(11), ym))
    shunt(s, "C707", (g(16), ym))
    shunt(s, "C710", (g(21), ym))
    xen = g(30)
    vpart(s, "R706", xen, ym); s.touched.add(("R706", "1"))
    yen = u.p("3")[1]
    s.wire(("R706", "2"), ("U112", "3"))
    nmos_driver(s, "Q121", "R713", "R714", (xen, yen), depth=g(8))
    s.wire(("U112", "6"), (Ux + g(28), ym))
    s.power((Ux + g(28), ym), "USB_LIMITED")
    shunt(s, "C708", (Ux + g(19), ym))
    shunt(s, "C709", (Ux + g(23), ym))
    yf = u.p("4")[1]
    s.place("TP112", Ux + g(13), yf)
    s.wire(("U112", "4"), ("TP112", "1"))
    s.term(("U112", "2"), length=0)
    # current-limit selection
    xs, ys = Ux + g(16), ym + g(17)
    v = s.place("U113", xs, ys)
    s.wire(("U112", "5"), (u.p("5")[0], v.p("4")[1]), ("U113", "4"))
    s.term(("U113", "5"), length=g(1))
    s.term(("U113", "1"), length=g(2))
    s.term(("U113", "2"), length=0)
    xn = v.p("3")[0] + g(4)
    s.wire(("U113", "3"), (xn + g(8), v.p("3")[1]))
    shunt(s, "R709", (xn, v.p("3")[1]))
    vpart(s, "R710", xn + g(8), v.p("3")[1]); s.touched.add(("R710", "1"))
    r710b = s.pp("R710", "2")
    q = s.place("Q120", r710b[0] - 2.54, r710b[1] + g(6) + 5.08)
    s.wire(("R710", "2"), ("Q120", "3"))
    s.term(("Q120", "2"), length=0)
    gx, gy = q.p("1")
    nx = gx - g(2)
    s.wire(("Q120", "1"), (nx, gy))
    shunt(s, "R712", (nx, gy), fields="L")
    hpart(s, "R711", nx - g(7) - 7.62, gy)
    s.wire(("R711", "2"), (nx, gy))
    s.term(("R711", "1"), length=g(2))
    yin = v.p("6")[1]
    xsel = v.p("6")[0] - g(7)
    s.wire(("U113", "6"), (xsel, yin))
    hpart(s, "R735", xsel - g(6), yin)
    s.wire(("R735", "2"), (xsel, yin))
    s.term(("R735", "1"), length=g(1))
    nmos_driver(s, "Q122", "R707", "R708", (xsel, yin), depth=g(3))
    note(s, g(8), g(58), "USB 输入限流", [
        "U112 TPS2553：USB_VBUS_RAW → USB_LIMITED，EN 由 R706 上拉，",
        "  USB_SUSPEND 为高时 Q121 拉低 EN，关断输出",
        "ILIM 经 U113 TS5A3159 选择：USB_500MA_EN 为高时 Q122 拉低 IN，",
        "  COM 接 NC = R709 60.4 kΩ；USB_HI_EN 为高时再并上 R710",
        "IN 为高（默认）时 COM 接 NO，而 NO 接的是 USB_VBUS_RAW，",
        "  即 ILIM 直接接到 VBUS（见索引页“待确认”）",
    ])

from lay import bank, tie, hrow, series_pd, load_switch
PAPER.update({1: "A3", 2: "A4", 3: "A4", 4: "A4", 6: "A4", 7: "A4", 10: "A4", 11: "A4"})

def p10(s):
    Ux, Uy = g(36), g(14)
    u = s.place("U107", Ux, Uy)
    s.wire((g(6), Uy), ("U107", "7")); s.power((g(6), Uy), "EXT_PROTECTED")
    shunt(s, "C714", (g(10), Uy))
    y2 = u.p("2")[1]
    s.wire((g(15), y2), ("U107", "2")); s.power((g(15), y2), "USB_LIMITED")
    shunt(s, "C715", (g(24), y2))
    for n in ("6", "5", "4"):
        s.term(("U107", n), length=g(2))
    ycp = u.p("3")[1]
    v = s.place("U114", Ux - g(20), ycp)
    s.wire(("U114", "2"), ("U107", "3"))
    shunt(s, "C712", (Ux - g(14), ycp)); shunt(s, "R715", (Ux - g(6), ycp))
    xin = v.p("1")[0]
    s.wire(("U114", "1"), (xin - g(6), ycp)); s.power((xin - g(6), ycp), "USB_VBUS_RAW")
    shunt(s, "C711", (xin - g(3), ycp), fields="L")
    s.term(("U114", "3"), length=0)
    yt = Uy + g(6)
    for i, (rt, rb) in enumerate((("R720", "R721"), ("R716", "R717"), ("R718", "R719"))):
        x = g(80) + i * g(11)
        vpart(s, rt, x, yt, fields="L"); s.term((rt, "1"), length=0)
        vpart(s, rb, x, yt + 7.62, fields="L"); s.touched.add((rt, "2")); s.touched.add((rb, "1"))
        s.term((rb, "2"), length=0)
        s.wire((x, yt + 7.62), (x + g(2), yt + 7.62)); s.label((x + g(2), yt + 7.62), s.net(rb, "1"), "R")
    xo = u.p("1")[0]
    s.wire(("U107", "8"), (xo + g(1), u.p("8")[1]), (xo + g(1), Uy))
    s.wire(("U107", "1"), (xo + g(1), Uy))
    yst = u.p("9")[1]
    xst = xo + g(6)
    s.wire(("U107", "9"), (xst + g(3), yst))
    vpart(s, "R722", xst, yst - 7.62, top="2"); s.term(("R722", "2"), length=0); s.touched.add(("R722", "1"))
    s.label((xst + g(3), yst), "EXT_SOURCE_SELECTED", "R")
    yil = u.p("10")[1]
    s.wire(("U107", "10"), (xo + g(11), yil)); shunt(s, "R723", (xo + g(11), yil))
    yss = u.p("11")[1]
    s.wire(("U107", "11"), (xo + g(6), yss)); shunt(s, "C713", (xo + g(6), yss))
    s.term(("U107", "12"), length=0)
    for i, c in enumerate(("C716", "C719", "C738", "C717", "C720")):
        shunt(s, c, (Ux + g(16) + i * g(4), Uy))
    s.wire((xo + g(1), Uy), (Ux + g(37), Uy))
    s.power((Ux + g(37), Uy), "VIN_SELECTED")
    # 3.3 V buck (second row)
    Bx, By = g(60), g(42)
    b = s.place("U201", Bx, By)
    xp = b.p("11")[0]
    s.wire(("U201", "11"), (xp - g(4), By)); s.power((xp - g(4), By), "VIN_SELECTED")
    s.wire(("U201", "12"), (xp - g(1), b.p("12")[1]), (xp - g(1), By))
    s.wire(("U201", "10"), (xp - g(1), b.p("10")[1]), (xp - g(1), By))
    yen = b.p("13")[1]
    s.place("SW101", Bx - g(27), yen, mirror="y")
    s.wire(("SW101", "2"), ("U201", "13"))
    shunt(s, "R727", (Bx - g(18), yen), fields="L")
    s.term(("SW101", "1"), length=g(1))
    s.term(("SW101", "3"), length=g(5))
    s.label((Bx - g(22), yen), "MAIN_ENABLE", "R")
    yss = b.p("9")[1]
    s.wire(("U201", "9"), (xp - g(6), yss)); shunt(s, "C718", (xp - g(6), yss))
    tie(s, "U201", ["6", "15", "16", "17", "7", "8"])
    xr = b.p("1")[0]
    s.wire(("U201", "2"), (xr + g(1), b.p("2")[1]))
    s.wire(("U201", "3"), (xr + g(1), b.p("3")[1]), (xr + g(1), By))
    s.wire(("U201", "1"), (xr + g(1), By))
    hpart(s, "L701", xr + g(7), By)
    s.wire((xr + g(1), By), ("L701", "1"))
    xv = xr + g(12)
    s.wire(("L701", "2"), (xr + g(32), By))
    s.wire(("U201", "14"), (xv, b.p("14")[1]), (xv, By))
    yfb = b.p("5")[1]
    xfb = xr + g(17)
    s.wire(("U201", "5"), (xfb, yfb))
    vpart(s, "R724", xfb, yfb - 7.62); s.touched.add(("R724", "2"))
    s.wire((xfb, By), ("R724", "1"))
    vpart(s, "R725", xfb, yfb); s.touched.add(("R725", "1")); s.term(("R725", "2"), length=0)
    shunt(s, "C721", (xr + g(23), By)); shunt(s, "C722", (xr + g(27), By))
    s.power((xr + g(32), By), "+3V3")
    s.term(("U201", "4"), length=g(2))
    hrow(s, "R726", xr + g(6), By + g(12), left="1", llen=g(2))
    note(s, g(6), g(66), "电源选择与 3.3 V", [
        "U107 TPS2121 在 EXT_PROTECTED（IN1）与 USB_LIMITED（IN2）之间切换，输出 VIN_SELECTED；",
        "  OV1 / PR1 / OV2 门限由 R716–R721 分压，CP2 接 U114 REF3312 的 1.25 V 基准；",
        "  ST = EXT_SOURCE_SELECTED 送 MCU。",
        "SW101 拨到 VIN_SELECTED 时 MAIN_ENABLE 为高，U201 TPS62130 输出 +3V3（FB：R724 / R725）。",
        "PGOOD_3V3 只接了 R726 上拉，没有接到 MCU。",
    ])

def p11(s):
    Cx, Cy = g(36), g(34)
    u = s.place("U202", Cx, Cy)
    xl = u.p("7")[0]
    s.term(("U202", "7"), length=g(2))
    series_pd(s, "R728", "R729", ("U202", "1"), node_dx=g(8))
    tie(s, "U202", ["2", "5"])
    yf = u.p("6")[1]
    s.wire(("U202", "6"), (xl - g(10), yf)); shunt(s, "R731", (xl - g(10), yf), fields="L")
    yv = u.p("18")[1]
    s.wire(("U202", "18"), (xl - g(7), yv)); shunt(s, "C725", (xl - g(7), yv))
    s.nc(("U202", "21"))
    # boot / switch network on top
    xb1, xs1, xs2, xb2 = (u.p(n)[0] for n in ("20", "8", "10", "19"))
    yt = u.p("20")[1]
    yc, yl = yt - g(8), yt - g(11)
    hpart(s, "C726", xb1, yc, left="1")
    hpart(s, "L702", xs1, yl, left="1")
    hpart(s, "C727", xs2, yc, left="2")
    s.wire(("U202", "20"), ("C726", "1"))
    s.wire(("U202", "8"), ("L702", "1"))
    s.wire(("U202", "10"), ("L702", "2"))
    s.wire(("U202", "19"), ("C727", "1"))
    s.touched |= {("C726", "2"), ("C727", "2")}
    # right side
    xr = u.p("11")[0]
    s.wire(("U202", "12"), (xr + g(1), u.p("12")[1]))
    s.wire(("U202", "13"), (xr + g(1), u.p("13")[1]), (xr + g(1), Cy))
    s.term(("U202", "3"), length=g(2))
    xfb = xr + g(21)
    s.wire(("U202", "11"), (xfb + g(26), Cy))
    yfb = u.p("14")[1]
    s.wire(("U202", "14"), (xfb, yfb))
    vpart(s, "R732", xfb, yfb - 7.62); s.touched.add(("R732", "2"))
    s.wire((xfb, Cy), ("R732", "1"))
    vpart(s, "R733", xfb, yfb); s.touched.add(("R733", "1")); s.term(("R733", "2"), length=0)
    s.nc(("U202", "16")); s.nc(("U202", "4"))
    yc2 = u.p("15")[1]
    s.wire(("U202", "15"), (xr + g(10), yc2))
    shunt(s, "C724", (xr + g(3), yc2))
    vpart(s, "R734", xr + g(10), yc2); s.touched.add(("R734", "1"))
    s.wire(("R734", "2"), (xr + g(16), yc2 + 7.62))
    shunt(s, "C723", (xr + g(16), yc2 + 7.62))
    for i, c in enumerate(("C731", "C732", "C733", "C734", "C739")):
        shunt(s, c, (xfb + g(6) + i * g(4), Cy))
    s.power((xfb + g(26), Cy), "+5V")
    s.term(("U202", "9"), length=0); s.term(("U202", "17"), length=0)
    bank(s, ["C728", "C729", "C735", "C736", "C737", "C730"], Cx + g(16), g(12))
    hrow(s, "R730", xr + g(30), Cy + g(16), left="1", llen=g(2))
    note(s, g(6), g(60), "5 V 升降压（显示屏和 CAN）", [
        "U202 TPS552872：VIN_SELECTED → +5V，L702 4.7 µH，FB 分压 R732 / R733",
        "EN/UVLO 由 MCU 的 5V_EN 经 R728 控制，R729 下拉：MCU 不驱动时 5 V 关闭",
        "5V_PG 经 R730 上拉到 +3V3 送 MCU；补偿 R734 + C723 / C724，开关频率由 R731 设定",
        "C737 默认不贴",
    ])

def p06(s):
    Jx, Jy = g(12), g(14)
    j = s.place("J101", Jx, Jy)
    xr = j.p("A4B9")[0]
    s.wire(("J101", "B4A9"), (xr + g(1), j.p("B4A9")[1]), (xr + g(1), Jy))
    s.wire(("J101", "A4B9"), (xr + g(26), Jy))
    s.power((xr + g(20), Jy), "USB_VBUS_RAW")
    vdiode(s, "D101", xr + g(5), Jy, top="1"); s.touched.add(("D101", "1")); s.term(("D101", "2"), length=0)
    hpart(s, "R161", xr + g(26), Jy); s.touched.add(("R161", "1"))
    s.term(("R161", "2"), length=g(1), kind="local")
    yd, ym = j.p("A6")[1], j.p("A7")[1]
    s.wire(("J101", "B6"), (xr + g(1), j.p("B6")[1]), (xr + g(1), yd))
    s.wire(("J101", "B7"), (xr + g(2), j.p("B7")[1]), (xr + g(2), ym))
    s.wire(("J101", "A6"), (xr + g(28), yd)); s.label((xr + g(28), yd), "USB_DP", "R")
    s.wire(("J101", "A7"), (xr + g(28), ym)); s.label((xr + g(28), ym), "USB_DM", "R")
    f = s.place_pin("U101", "3", xr + g(15), yd - g(1), mirror="x")
    s.wire(("U101", "3"), (f.p("3")[0], yd))
    s.wire(("U101", "5"), (f.p("5")[0], ym))
    s.nc(("U101", "1")); s.nc(("U101", "2"))
    gp = f.p("4")
    s.wire(("U101", "4"), (gp[0], gp[1] - g(1)), (gp[0] - g(5), gp[1] - g(1)))
    s.power((gp[0] - g(5), gp[1] - g(1)), "GND")
    t = s.place("U111", g(66), j.p("A5")[1])
    s.wire(("J101", "A5"), ("U111", "1"))
    s.wire(("J101", "B5"), ("U111", "2"))
    e = s.place_pin("U110", "3", xr + g(15), j.p("B5")[1] + g(3))
    s.wire(("U110", "3"), (e.p("3")[0], j.p("A5")[1]))
    s.wire(("U110", "5"), (e.p("5")[0], j.p("B5")[1]))
    s.nc(("U110", "1")); s.nc(("U110", "2")); s.term(("U110", "4"), length=0)
    s.nc(("J101", "A8")); s.nc(("J101", "B8"))
    tie(s, "J101", ["A1B12", "B1A12", "1", "2", "3", "4"])
    tie(s, "U111", ["3", "5", "11"])
    s.term(("U111", "7"), length=g(2)); s.term(("U111", "8"), length=g(2))
    s.term(("U111", "4"), length=g(2), kind="local")
    s.nc(("U111", "9"))
    yi = t.p("6")[1]
    xi = t.p("6")[0]
    s.wire(("U111", "6"), (xi + g(12), yi))
    vpart(s, "R162", xi + g(9), yi - 7.62, top="2"); s.term(("R162", "2"), length=0); s.touched.add(("R162", "1"))
    s.label((xi + g(12), yi), "TYPEC_INT_N", "R")
    s.term(("U111", "12"), length=g(1)); s.term(("U111", "10"), length=0)
    bank(s, ["C161"], t.p("12")[0] + g(8), t.p("12")[1] - g(2))
    qx, qy = g(86), g(20)
    q = s.place("Q106", qx, qy)
    s.wire(("Q106", "1"), (qx - g(4), qy)); s.power((qx - g(4), qy), "USB_VBUS_RAW")
    s.term(("Q106", "2"), length=0)
    d = q.p("3")
    vpart(s, "R141", d[0], d[1] - g(2) - 7.62, top="1")
    s.term(("R141", "1"), length=0)
    s.wire(("R141", "2"), ("Q106", "3"))
    s.wire((d[0], d[1] - g(1)), (d[0] + g(5), d[1] - g(1)))
    s.label((d[0] + g(5), d[1] - g(1)), "USB_VBUS_N", "R")
    note(s, g(6), g(64), "USB-C 接口", [
        "J101 USB-C：D+ / D− 两面并联后经 U101 ESD 送 MCU（USB_DP / USB_DM）",
        "CC1 / CC2 经 U110 ESD 接 U111 TUSB320LAI（I2C：EXP_SCL / EXP_SDA，ADDR 接地，",
        "  INT_N = TYPEC_INT_N），VBUS_DET 经 R161 887 kΩ 接 VBUS",
        "D101 SMF5.0A 保护 VBUS；Q106 把 VBUS 转成 USB_VBUS_N（有 VBUS 时为低）；SBU 未接",
    ])

def p07(s):
    load_switch(s, "U502", "C506", "R518", ["C503", "C504"], g(24), g(14))
    ux, uy = g(52), g(36)
    e = s.place("U503", ux, uy)
    jx = ux + g(30)
    jj = s.place("J502", jx, uy)
    xl = e.p("14")[0]
    for a, b in (("14", "8"), ("13", "7"), ("12", "5"), ("11", "3"), ("9", "2"), ("8", "1")):
        pass
    for a, b in (("1", "8"), ("2", "7"), ("3", "5"), ("4", "3"), ("6", "2"), ("7", "1")):
        s.wire(("U503", a), ("J502", b))
    for n in ("14", "13", "11", "9", "8"):
        s.term(("U503", n), length=g(12))
    yc = e.p("12")[1]
    hpart(s, "R516", xl - g(7) - 7.62, yc)
    s.wire(("R516", "2"), ("U503", "12"))
    s.term(("R516", "1"), length=g(2))
    s.term(("U503", "5"), length=0); s.term(("U503", "10"), length=0)
    ydet = jj.p("11")[1]
    s.wire(("J502", "11"), (xl - g(12), ydet)); s.label((xl - g(12), ydet), "SD_DET", "L")
    s.term(("J502", "4"), length=g(1))
    tie(s, "J502", ["6", "9", "10", "12", "13", "14"])
    for i, r in enumerate(("R510", "R511", "R512", "R513", "R514", "R515")):
        hrow(s, r, g(96), g(30) + i * g(4), left="1", llen=g(2), rlen=g(2))
    note(s, g(8), g(60), "microSD", [
        "U502 TPS22919 由 SD_PWR_EN 打开 +3V3_SD（R518 下拉：MCU 不驱动时卡断电）",
        "数据线经 U503 TPD6E05U06 ESD（直通走线）接 J502；CLK 串 R516 22 Ω",
        "DAT0–3 / CMD 47 kΩ 上拉到 +3V3_SD；SD_DET 47 kΩ 上拉到 +3V3",
    ])

def p04(s):
    load_switch(s, "U501", "C505", "R517", ["C501", "C502"], g(26), g(12))
    j = s.place("J501", g(46), g(46))
    for n in range(1, 17):
        s.term(("J501", str(n)), length=g(2), side=True)
    k = s.place("J503", g(90), g(30))
    for n in range(1, 43):
        s.term(("J503", str(n)), length=g(2), side=True)
    note(s, g(6), g(66), "显示接口", [
        "U501 TPS22919 由 LCD_PWR_EN 打开 +5V_LCD（R517 下拉）",
        "J501 16 针 FFC：SPI 屏（LCD_*）+ 电容触摸（I2C1、CTP_RST、CTP_INT），14 脚未接",
        "J503 40 针 RGB 屏预留，默认不贴",
    ])

def p03(s):
    Ux, Uy = g(28), g(14)
    u = s.place("U601", Ux, Uy)
    left = ["23", "24", "25", "26", "29", "30", "31", "32", "33", "34", "22", "35", "20", "21", "37", "19", "18", "17", "16", "15", "39"]
    for n in left:
        s.term(("U601", n), length=g(2))
    yc = u.p("38")[1]
    xl = u.p("38")[0]
    hpart(s, "R601", xl - g(7) - 7.62, yc, left="1")
    s.wire(("R601", "2"), ("U601", "38"))
    s.term(("R601", "1"), length=g(2))
    for n in ("2", "4", "5", "7", "8", "10", "11", "13", "42", "44", "45", "47", "48", "50", "51", "53"):
        s.term(("U601", n), length=g(2))
    s.nc(("U601", "36")); s.nc(("U601", "40"))
    tie(s, "U601", ["1", "14", "27", "3", "9", "43", "49"])
    tie(s, "U601", ["28", "41", "54", "6", "12", "46", "52"])
    load_switch(s, "U602", "C601", "R602", [], g(70), g(12))
    bank(s, ["C602", "C603", "C604", "C605", "C606", "C607", "C608", "C609"], g(58), g(30))
    hrow(s, "R603", g(64), g(42), left="1", llen=g(2), rlen=g(2))
    hrow(s, "R604", g(64), g(48), left="1", llen=g(2), rlen=g(2))
    note(s, g(52), g(54), "SDRAM", [
        "U601 W9812G6KH-6（16 MiB × 16）接 MCU FMC：SDNE0 / SDCKE0",
        "U602 TPS22919 由 SDRAM_PWR_EN 打开 +3V3_SDRAM（R602 下拉）",
        "SDCLK 串 R601 22 Ω；CS# 10 kΩ 上拉、CKE 10 kΩ 下拉",
        "DQ0–7、DQ8–15 在字节内换了顺序（见引脚旁网络名），SDRAM 允许这样接",
    ])

def p02(s):
    Px, Py = g(30), g(14)
    u = s.place("U301", Px, Py, unit=5)
    vdd = [n for n in u.pins() if s.net("U301", n) == "+3V3"]
    vdd.sort(key=lambda n: u.p(n)[1])
    tie(s, "U301", vdd, length=g(2))
    tie(s, "U301", ["33", "32"], length=g(2))
    s.term(("U301", "6"), length=g(2))
    vss = [n for n in u.pins() if s.net("U301", n) == "GND"]
    vss.sort(key=lambda n: u.p(n)[1])
    tie(s, "U301", vss, length=g(1))
    xr = u.p("23")[0]
    xa, xb = xr + g(13), xr + g(16)
    y0, y1 = u.p("23")[1], u.p("24")[1]
    yx = y0 + g(5)
    s.wire(("U301", "23"), (xb, y0), (xb, yx + g(2)))
    s.wire(("U301", "24"), (xa, y1), (xa, yx + g(2)))
    s.place("Y301", xa + 3.81, yx, mirror="y")
    s.touched |= {("Y301", "1"), ("Y301", "3")}
    s.term(("Y301", "2"), length=0); s.touched.add(("Y301", "4"))
    vpart(s, "C312", xb, yx + g(2)); s.touched.add(("C312", "1")); s.term(("C312", "2"), length=0)
    vpart(s, "C313", xa, yx + g(2), fields="L"); s.touched.add(("C313", "1")); s.term(("C313", "2"), length=0)
    xc, xd = xr + g(5), xr + g(8)
    y3, y4 = u.p("8")[1], u.p("9")[1]
    yl = y3 + g(6)
    s.wire(("U301", "8"), (xd, y3), (xd, yl + g(2)))
    s.wire(("U301", "9"), (xc, y4), (xc, yl + g(2)))
    s.place("Y302", xc + 3.81, yl, mirror="y")
    s.touched |= {("Y302", "1"), ("Y302", "2")}
    vpart(s, "C314", xd, yl + g(2)); s.touched.add(("C314", "1")); s.term(("C314", "2"), length=0)
    vpart(s, "C315", xc, yl + g(2), fields="L"); s.touched.add(("C315", "1")); s.term(("C315", "2"), length=0)
    for pin, cap, dx in (("71", "C308", g(10)), ("106", "C309", g(5))):
        yv = u.p(pin)[1]
        s.wire(("U301", pin), (xr + dx, yv)); shunt(s, cap, (xr + dx, yv))
    bx, by = g(62), g(14)
    s.place("BT301", bx, by + 5.08)
    s.term(("BT301", "2"), length=0)
    s.wire(("BT301", "1"), (bx, by - g(2)), (bx + g(8), by - g(2)))
    hpart(s, "R308", bx + g(8), by - g(2)); s.touched.add(("R308", "1"))
    s.place("D303", bx + g(8) + 7.62 + g(7) + 3.81, by - g(2), mirror="y")
    s.wire(("R308", "2"), ("D303", "1"))
    s.nc(("D303", "2"))
    k = s.pp("D303", "3")
    s.wire(("D303", "3"), (k[0] + g(12), k[1]))
    shunt(s, "C307", (k[0] + g(3), k[1])); shunt(s, "C317", (k[0] + g(7), k[1]))
    s.power((k[0] + g(12), k[1]), "VBAT_RTC")
    ax, ay = g(66), g(32)
    s.power((ax, ay), "+3V3")
    hpart(s, "FB301", ax + g(1), ay)
    s.wire((ax, ay), ("FB301", "1"))
    s.wire(("FB301", "2"), (ax + g(14), ay))
    shunt(s, "C310", (ax + g(6), ay)); shunt(s, "C311", (ax + g(10), ay))
    s.power((ax + g(14), ay), "+3V3A")
    bank(s, ["C306", "C320", "C321", "C322", "C323", "C324", "C325", "C326", "C327", "C328", "C329", "C330", "C331"],
         g(8), g(56), pitch=g(5))
    note(s, g(62), g(42), "MCU 供电、时钟与 RTC", [
        "U301 的电源和时钟引脚画在本页，GPIO 在第 1 页",
        "HSE 25 MHz（Y301，2 × 18 pF）；LSE 32.768 kHz（Y302，2 × 8.2 pF）",
        "VDDA / VREF+ 经 FB301 滤波（+3V3A）；VCAP_1 / VCAP_2 各 2.2 µF",
        "VBAT：BT301 纽扣电池 → R308 1 kΩ → D303 BAS116 → VBAT_RTC",
        "+3V3 去耦：C306 4.7 µF + 12 × 100 nF（C320–C331）",
    ])

def p01(s):
    pos = {1: (g(26), g(20)), 2: (g(82), g(20)), 3: (g(26), g(64)), 4: (g(82), g(64))}
    for unit, (x, y) in pos.items():
        u = s.place("U301", x, y, unit=unit)
        for n in u.pins():
            if unit == 4 and n in ("25", "138"):
                continue
            s.term(("U301", n), length=g(2))
    u4 = s.byref[("U301", 4)]
    xr = u4.p("25")[0]
    yr = u4.p("25")[1]
    s.wire(("U301", "25"), (xr + g(12), yr))
    vpart(s, "R307", xr + g(5), yr - 7.62, top="2"); s.term(("R307", "2"), length=0); s.touched.add(("R307", "1"))
    shunt(s, "C316", (xr + g(5), yr))
    s.place("TP302", xr + g(9), yr); s.touched.add(("TP302", "1"))
    s.label((xr + g(12), yr), "NRST", "R")
    yb = u4.p("138")[1]
    s.wire(("U301", "138"), (xr + g(9), yb))
    s.label((xr + g(9), yb), "BOOT0", "R")
    s.place("TP301", xr + g(1), yb); s.touched.add(("TP301", "1"))
    shunt(s, "R301", (xr + g(4), yb))
    jx, jy = g(132), g(20)
    s.place("J301", jx, jy)
    for n in ("2", "4", "6", "10"):
        s.term(("J301", n), length=g(2))
    s.nc(("J301", "7")); s.nc(("J301", "8"))
    s.term(("J301", "1"), length=g(1))
    tie(s, "J301", ["3", "5", "9"], length=g(1))
    s.place("J302", g(136), g(44))
    for n in range(1, 9):
        s.term(("J302", str(n)), length=g(2), side=True)
    hrow(s, "R304", g(126), g(60), left="1", llen=g(2), rlen=g(2))
    hrow(s, "R305", g(126), g(66), left="1", llen=g(2), rlen=g(2))
    hrow(s, "R302", g(124), g(76), left="1", llen=g(2), rlen=0)
    hdiode(s, "D301", s.pp("R302", "2")[0] + g(3), g(76), left="1")
    s.wire(("R302", "2"), ("D301", "1"))
    s.term(("D301", "2"), length=g(1))
    note(s, g(120), g(86), "MCU", [
        "U301 STM32H743ZIT6：GPIO 按端口画在本页，电源 / 时钟引脚在第 2 页",
        "J301 SWD（2×5 1.27 mm）、J302 串口 / I2C 扩展，都默认不贴",
        "BOOT0 10 kΩ 下拉 + TP301；NRST 10 kΩ 上拉 + 100 nF + TP302",
        "PC2_C、PC3_C、PG2 未用；LED_STAT（PA8）驱动 D301",
    ])

