"""Root sheet: block diagram of the 11 pages (clickable sheet boxes), conventions and open questions."""
import sx
from sx import Sym
from sheet import uid, rnd

SUB = {
    1: ["STM32H743ZIT6：GPIO A–G", "SWD · 串口 / I2C 扩展 · 状态灯"],
    2: ["MCU 电源 / 时钟引脚、去耦", "HSE 25 MHz · LSE 32.768 kHz · RTC 电池"],
    3: ["W9812G6KH-6 16 MiB SDRAM", "TPS22919 电源开关"],
    4: ["16 针 FFC：SPI 屏 + 触摸", "40 针 RGB 屏预留（DNP）"],
    5: ["2 × TJA1042T/3", "TVS · 120 Ω 终端（可选）"],
    6: ["USB-C · ESD · TUSB320LAI", "VBUS 检测"],
    7: ["microSD · TPD6E05U06", "TPS22919 电源开关"],
    8: ["J103 · 保险丝 · TVS", "TPS26600 电子保险丝"],
    9: ["TPS2553 限流", "TS5A3159 选择限流档位"],
    10: ["TPS2121 电源选择", "TPS62130 → +3V3"],
    11: ["TPS552872 升降压", "→ +5V"],
}

BOX = {   # page: (x, y, w, h)
    6: (20, 42, 74, 24), 9: (118, 42, 74, 24), 8: (118, 82, 74, 24), 10: (216, 62, 74, 24), 11: (314, 62, 74, 24),
    2: (20, 138, 74, 24), 1: (118, 138, 74, 24), 3: (216, 128, 74, 24), 4: (314, 128, 74, 24),
    7: (118, 178, 74, 24), 5: (216, 178, 74, 24),
}

def _font(size, bold=False, color=None):
    f = [Sym("font"), [Sym("size"), size, size]]
    if bold:
        f.append([Sym("bold"), Sym("yes")])
    if color:
        f.append([Sym("color")] + list(color))
    return f

def text(x, y, s, size=1.6, bold=False, color=None, justify="left top"):
    return [Sym("text"), s, [Sym("exclude_from_sim"), Sym("no")], [Sym("at"), rnd(x), rnd(y), 0],
            [Sym("effects"), _font(size, bold, color), [Sym("justify")] + [Sym(j) for j in justify.split()]],
            [Sym("uuid"), uid("root", "t", x, y, s)]]

def line(pts, width=0.35, color=(0, 60, 120, 1), fill=False):
    n = [Sym("polyline"), [Sym("pts")] + [[Sym("xy"), rnd(x), rnd(y)] for x, y in pts],
         [Sym("stroke"), [Sym("width"), width], [Sym("type"), Sym("solid")], [Sym("color")] + list(color)]]
    n.append([Sym("fill"), [Sym("type"), Sym("color") if fill else Sym("none")]] + ([[Sym("color")] + list(color)] if fill else []))
    n.append([Sym("uuid"), uid("root", "pl", tuple(pts))])
    return n

def arrow(pts, label=None, lpos=None):
    out = [line(pts)]
    (x1, y1), (x2, y2) = pts[-2], pts[-1]
    dx, dy = (x2 > x1) - (x2 < x1), (y2 > y1) - (y2 < y1)
    L, W = 2.6, 1.1
    tip = (x2, y2)
    base = (x2 - dx * L, y2 - dy * L)
    a = (base[0] - dy * W, base[1] + dx * W)
    b = (base[0] + dy * W, base[1] - dx * W)
    out.append(line([tip, a, b, tip], width=0.1, fill=True))
    if label:
        lx, ly = lpos
        out.append(text(lx, ly, label, size=1.5, color=(0, 60, 120, 1), justify="left bottom"))
    return out

def sheet_node(s, x, y, w, h, project, root_uuid, suuid):
    return [Sym("sheet"), [Sym("at"), x, y], [Sym("size"), w, h], [Sym("exclude_from_sim"), Sym("no")],
            [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")], [Sym("dnp"), Sym("no")],
            [Sym("fields_autoplaced"), Sym("yes")],
            [Sym("stroke"), [Sym("width"), 0.3], [Sym("type"), Sym("solid")], [Sym("color"), 132, 0, 0, 1]],
            [Sym("fill"), [Sym("color"), 255, 255, 225, 1]],
            [Sym("uuid"), suuid],
            [Sym("property"), "Sheetname", s.title, [Sym("at"), x, rnd(y - 0.8), 0],
             [Sym("effects"), _font(1.8, True), [Sym("justify"), Sym("left"), Sym("bottom")]]],
            [Sym("property"), "Sheetfile", f"{s.key}.kicad_sch", [Sym("at"), x, rnd(y + h + 0.6), 0],
             [Sym("effects"), _font(1.27), [Sym("justify"), Sym("left"), Sym("top")], [Sym("hide"), Sym("yes")]]],
            [Sym("instances"), [Sym("project"), project, [Sym("path"), f"/{root_uuid}", [Sym("page"), str(s.page)]]]]]

NOTES = [
    "本册由 Hardware-C 原理图 C062（嘉立创EDA，11 页）重画。元件、位号、取值和每一个网络都与原图一致，只改了排版。",
    "每页按信号流从左到右画；电源和地用电源符号；跨页信号用全局标签，标签后的 [n] 是这个信号还出现在哪几页。",
    "打叉、灰色的元件默认不贴（DNP）。点上面的方框可以跳到对应页，点标签后的页码也可以跳转。",
    "U301 STM32H743ZIT6 拆成 5 个单元：GPIO A–G 在第 1 页，电源和时钟引脚在第 2 页（和去耦电容、晶振画在一起）。",
    "校验：用 KiCad 10.0.8 导出网表，与原图逐脚比对，209 个元件、189 个网络以及全部未接引脚完全一致；ERC 0 条。",
    "附录 A：第 12 页第 2 版（APM 按键模拟，提案，不在上述网表内）。附录 B：APM 按键采集电路参考。",
]
OPEN = [
    "1. 第 9 页：U113 TS5A3159 的 NO（1 脚）接 USB_VBUS_RAW。USB_SEL_IN 为高时（默认，Q122 截止）COM 接 NO，",
    "   TPS2553 的 ILIM 直接接到 VBUS，而不是经电阻接地。请对照 TPS2553 数据手册确认这个默认状态。",
    "2. 第 8 页：EFUSE_RTN（U106 的 RTN、EP、MODE，以及 R703 / R704 / C701 的下端）在网表里没有接到 GND，",
    "   U106 的 GND（9 脚）单独接地。请对照 TPS26600 数据手册确认 RTN 与 GND 的接法。",
    "3. 只有上拉、没有接到 MCU 的状态脚：EXT_FAULT_N（第 8 页 U106 FLT#）、PGOOD_3V3（第 10 页 U201 PG）；",
    "   U112 /FAULT（第 9 页）只接了测试点 TP112。",
]

def root_sheet(c, sheets, project, root_uuid, sheet_uuid):
    doc = [Sym("kicad_sch"), [Sym("version"), 20250114], [Sym("generator"), "eeschema"], [Sym("generator_version"), "9.0"],
           [Sym("uuid"), root_uuid], [Sym("paper"), "A3"],
           [Sym("title_block"), [Sym("title"), "Hardware-C C062 schematic - redrawn (index)"], [Sym("date"), c.date],
            [Sym("rev"), c.rev], [Sym("company"), c.company],
            [Sym("comment"), 1, "Netlist identical to the original C062 schematic (209 parts, 189 nets)"]],
           [Sym("lib_symbols")]]
    items = []
    items.append(text(20, 16, "Hardware-C（C062）原理图 · 重画版", size=4.0, bold=True, color=(0, 60, 120, 1)))
    items.append(text(20, 24, "共 11 页 + 2 个附录。下图是各页之间的电源和信号关系，点方框进入对应页。", size=1.8))
    by_page = {s.page: s for s in sheets}
    for pg, (x, y, w, h) in BOX.items():
        if pg not in by_page:
            continue
        s = by_page[pg]
        doc.append(sheet_node(s, x, y, w, h, project, root_uuid, sheet_uuid[s.key]))
        for i, ln in enumerate(SUB[pg]):
            items.append(text(x + 2.5, y + 7 + i * 4.6, ln, size=1.6))
        items.append(text(x + w - 2, y + h - 2, f"第 {pg} 页", size=1.4, color=(120, 120, 120, 1), justify="right bottom"))
    # power flow
    items += arrow([(94, 54), (118, 54)], "USB_VBUS_RAW", (95.5, 52.5))
    items += arrow([(192, 54), (204, 54), (204, 70), (216, 70)], "USB_LIMITED", (193.5, 52.5))
    items += arrow([(192, 94), (204, 94), (204, 80), (216, 80)], "EXT_PROTECTED", (193.5, 98.5))
    items += arrow([(290, 74), (314, 74)], "VIN_SELECTED", (291.5, 72.5))
    items.append(text(62, 88, "外部 5–12 V（J103）", size=1.6, color=(0, 60, 120, 1), justify="left bottom"))
    items += arrow([(62, 94), (118, 94)], "EXT_RAW", (64, 92.5))
    items += arrow([(253, 86), (253, 110)], "+3V3：MCU、SDRAM、microSD、CAN VIO …", (255, 104))
    items += arrow([(351, 86), (351, 100), (398, 100), (398, 140), (388, 140)], "+5V：显示、CAN", (353, 98.5))
    items += arrow([(398, 140), (398, 190), (290, 190)], None)
    # MCU links
    items += arrow([(192, 145), (216, 145)], "FMC", (198, 143.5))
    items += arrow([(118, 158), (108, 158), (108, 190), (118, 190)], "SDMMC", (96, 175))
    items += arrow([(192, 156), (206, 156), (206, 194), (216, 194)], "FDCAN1 / 2", (193.5, 154.5))
    items += arrow([(170, 138), (170, 120), (300, 120), (300, 140), (314, 140)], "SPI · I2C · RGB", (258, 118.5))
    items += arrow([(130, 138), (130, 118), (30, 118), (30, 66)], "USB · I2C · VBUS 检测", (32, 116.5))
    items.append(line([(94, 150), (118, 150)]))
    items.append(text(97, 148.5, "电源/时钟", size=1.5, color=(0, 60, 120, 1), justify="left bottom"))
    # notes
    y = 214
    items.append(text(20, y, "说明", size=2.2, bold=True, color=(0, 60, 120, 1)))
    for i, ln in enumerate(NOTES):
        items.append(text(20, y + 5 + i * 3.4, ln, size=1.6))
    y2 = y + 5 + len(NOTES) * 3.4 + 4
    items.append(text(20, y2, "待确认（重画时在原图里看到的，原图如此，本册未改）", size=2.2, bold=True, color=(160, 40, 0, 1)))
    for i, ln in enumerate(OPEN):
        items.append(text(20, y2 + 5 + i * 3.4, ln, size=1.6))
    doc.extend(items)
    doc.append([Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "0"]]])
    doc.append([Sym("embedded_fonts"), Sym("no")])
    return doc
