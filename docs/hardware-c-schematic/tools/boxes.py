"""Box symbols for ICs and connectors, with the original pin numbers. Names follow the original
schematic unless noted; pins are grouped by function. None = a one-row gap."""
import json, os
import symlib

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
_P = json.load(open(f"{DATA}/schematic-pins.json"))

def nm(ref, num):
    return _P[ref]["pins"][str(num)]["pin_name"]

def L(ref, *nums, names=None):
    out = []
    for n in nums:
        if n is None:
            out.append(None)
        else:
            out.append((str(n), (names or {}).get(str(n), nm(ref, n))))
    return out

def stm32():
    pins = _P["U301"]["pins"]
    byname = {v["pin_name"]: k for k, v in pins.items()}
    def port(p, rng=range(16), skip=()):
        out = []
        for i in rng:
            n = f"P{p}{i}"
            cand = [k for k, v in pins.items() if v["pin_name"].split("-")[0].split("_")[0] == n]
            if cand and n not in skip:
                out.append((cand[0], pins[cand[0]]["pin_name"]))
        return out
    vdd = [k for k, v in pins.items() if v["pin_name"] == "VDD"]
    vss = [k for k, v in pins.items() if v["pin_name"] == "VSS"]
    sort = lambda l: sorted(l, key=int)
    pc = [e for e in port("C") if e[1] not in ("PC14-OSC32_IN", "PC15-OSC32_OUT")]
    ph = [e for e in port("H")]
    units = [
        dict(title="GPIO A / B", left=port("A"), right=port("B")),
        dict(title="GPIO C / D", left=pc, right=port("D")),
        dict(title="GPIO E / F", left=port("E"), right=port("F")),
        dict(title="GPIO G / reset / boot", left=port("G"),
             right=[(byname["NRST"], "NRST"), None, (byname["BOOT0"], "BOOT0")]),
        dict(title="Power / clocks",
             left=[(k, "VDD") for k in sort(vdd)] + [(byname["VDDUSB"], "VDDUSB"), (byname["PDR_ON"], "PDR_ON"), None,
                   (byname["VDDA"], "VDDA"), (byname["VREF+"], "VREF+"), None, None, None, (byname["VBAT"], "VBAT")],
             right=ph + [None] + [(byname["PC14-OSC32_IN"], "PC14-OSC32_IN"), (byname["PC15-OSC32_OUT"], "PC15-OSC32_OUT"), None]
                   + [(k, "VSS") for k in sort(vss)] + [(byname["VSSA"], "VSSA")]
                   + [None, None, (byname["Vcap_1"], "VCAP_1"), None, (byname["Vcap_2"], "VCAP_2")]),
    ]
    return symlib.box("STM32H743ZIT6", units, "U", "STM32H743ZIT6")

def w9812():
    r = "U601"
    left = L(r, 23, 24, 25, 26, 29, 30, 31, 32, 33, 34, 22, 35, None, 20, 21, None, None, 38, None, 37, 19, 18, 17, 16, None, 15, 39)
    right = L(r, 2, 4, 5, 7, 8, 10, 11, 13, 42, 44, 45, 47, 48, 50, 51, 53, None, 36, 40)
    top = L(r, 1, 14, 27, 3, 9, 43, 49)
    bot = L(r, 28, 41, 54, 6, 12, 46, 52)
    return symlib.box("W9812G6KH-6", [dict(left=left, right=right, top=top, bottom=bot)], "U", "W9812G6KH-6")

def tps22919():
    r = "U602"
    return symlib.box("TPS22919", [dict(left=L(r, 1, None, None, None, None, 3, 4), right=L(r, 6, None, 5), bottom=L(r, 2))], "U", "TPS22919DCKR")

def tja1042():
    r = "U401"
    return symlib.box("TJA1042T3", [dict(left=L(r, 8, None, None, 1, None, None, 4),
                                         right=L(r, 7, None, None, None, None, None, None, 6), top=L(r, 5, 3), bottom=L(r, 2))],
                      "U", "TJA1042T/3")

def tpd2e2u06():
    r = "U101"
    return symlib.box("TPD2E2U06", [dict(top=L(r, 3, 5), bottom=L(r, 4), right=L(r, 1, 2))], "U", "TPD2E2U06DRLR", min_hw=5.08)

def tusb320():
    r = "U111"
    return symlib.box("TUSB320LAI", [dict(left=L(r, 1, 2, None, None, None, 3, 5, 11), right=L(r, 7, 8, 6, None, 4, None, 9),
                                          top=L(r, 12), bottom=L(r, 10))], "U", "TUSB320LAIRWBR")

def tpd6e05():
    r = "U503"
    return symlib.box("TPD6E05U06", [dict(left=L(r, 14, None, 13, None, 12, None, 11, None, 9, None, 8), right=L(r, 1, None, 2, None, 3, None, 4, None, 6, None, 7),
                                          bottom=L(r, 5, 10))], "U", "TPD6E05U06RVZR")

def tps26600():
    r = "U106"
    return symlib.box("TPS26600", [dict(left=L(r, 1, 2, None, 3, None, None, 5, None, 7, 6, None, 4, 13),
                                         right=L(r, 15, 16, None, None, None, None, None, None, 14, None, 10),
                                         bottom=L(r, 12, None, 11, None, 8, 17, 9))], "U", "TPS26600PWPR")

def tps2553():
    r = "U112"
    return symlib.box("TPS2553", [dict(left=L(r, 1, None, None, 3), right=L(r, 6, None, None, 4), bottom=L(r, 5, 2))], "U", "TPS2553DBVR")

def ts5a3159():
    r = "U113"
    return symlib.box("TS5A3159", [dict(left=L(r, 4, None, None, None, 6), right=L(r, 1, None, None, None, 3), top=L(r, 5), bottom=L(r, 2))],
                      "U", "TS5A3159DBVR")

def tps2121():
    r = "U107"
    return symlib.box("TPS2121", [dict(left=L(r, 7, None, None, None, None, None, 2, None, 6, 5, 4, None, 3),
                                       right=L(r, 1, 8, None, None, None, None, 9, None, 10, 11), bottom=L(r, 12))], "U", "TPS2121RUXR")

def ref3312():
    r = "U114"
    return symlib.box("REF3312", [dict(left=L(r, 1), right=L(r, 2), bottom=L(r, 3))], "U", "REF3312AIDBZR")

def tps62130():
    r = "U201"
    return symlib.box("TPS62130", [dict(left=L(r, 11, 12, 10, None, 13, 9),
                                        right=L(r, 1, 2, 3, None, 14, 5, None, 4),
                                        bottom=L(r, 6, 15, 16, 17, 7, 8))], "U", "TPS62130RGTR")

def tps552872():
    r = "U202"
    return symlib.box("TPS552872", [dict(left=L(r, 7, None, 1, None, 2, 5, None, 6, None, 18, 21),
                                         right=L(r, 11, 12, 13, None, 3, 14, 16, 15, None, 4),
                                         top=L(r, 20, 8, 10, 19), bottom=L(r, 9, 17))], "U", "TPS552872RYQR", tb_pitch=3 * 2.54)

def typec():
    r = "J101"
    return symlib.box("USB_C_16P", [dict(right=L(r, "A4B9", "B4A9", None, None, None, None, None, None, None, None, None, None, None, "A6", "B6", "A7", "B7", None, "A5", "B5", None, "A8", "B8"),
                                         bottom=L(r, "A1B12", "B1A12", 1, 2, 3, 4))], "J", "TYPE-C-31-M-12", tb_pitch=2 * 2.54)

def microsd():
    r = "J502"
    names = {"11": "DET", "9": "GND", "10": "GND", "12": "GND", "13": "GND", "14": "GND"}
    return symlib.box("MICROSD", [dict(left=L(r, 8, None, 7, None, 5, None, 3, None, 2, None, 1, None, None, None, None, 11, names=names), top=L(r, 4),
                                       bottom=L(r, 6, 9, 10, 12, 13, 14, names=names))], "J", "1040310811")

def ffc40():
    r = "J503"
    return symlib.box("FFC40_RGB", [dict(left=L(r, *range(1, 22), names={str(i): "~" for i in range(1, 43)}),
                                         right=L(r, *range(22, 43), names={str(i): "~" for i in range(1, 43)}))], "J", "AFC01-S40FCA-00",
                      min_hw=3 * 2.54)

BOXES = {"STM32": stm32, "W9812": w9812, "TPS22919": tps22919, "TJA1042": tja1042, "TPD2E2U06": tpd2e2u06,
         "TUSB320": tusb320, "TPD6E05": tpd6e05, "TPS26600": tps26600, "TPS2553": tps2553, "TS5A3159": ts5a3159,
         "TPS2121": tps2121, "REF3312": ref3312, "TPS62130": tps62130, "TPS552872": tps552872, "TYPEC": typec,
         "MICROSD": microsd, "FFC40": ffc40}
REF_BOX = {"U301": "STM32", "U601": "W9812", "U602": "TPS22919", "U501": "TPS22919", "U502": "TPS22919",
           "U401": "TJA1042", "U402": "TJA1042", "U101": "TPD2E2U06", "U110": "TPD2E2U06", "U111": "TUSB320",
           "U503": "TPD6E05", "U106": "TPS26600", "U112": "TPS2553", "U113": "TS5A3159", "U107": "TPS2121",
           "U114": "REF3312", "U201": "TPS62130", "U202": "TPS552872", "J101": "TYPEC", "J502": "MICROSD", "J503": "FFC40"}
