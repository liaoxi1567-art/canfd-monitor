# Hardware-C 原理图重画版

把仓库根目录的 `Hardware-C-schematic.pdf`（嘉立创EDA 工程 C062，11 页）按功能重新排版，汇总成一个 PDF：

**[Hardware-C-schematic-redrawn.pdf](Hardware-C-schematic-redrawn.pdf)**（14 页）

| 页 | 内容 |
| --- | --- |
| 索引 | 各页之间的电源和信号关系图（点方框跳到对应页）、画法说明、待确认事项 |
| 1–11 | 原图 11 页，页号和标题不变 |
| 附录 A | 第 12 页第 2 版（APM 按键模拟，提案；完整说明在 PR #1 的 `docs/apm-keysim/`） |
| 附录 B | APM 按键采集电路参考（仓库根目录的 `apm按键采集图.pdf`，未改动） |

## 和原图的关系

- 元件、位号、取值、料号和每一个网络都与原图一致，只改了排版。
- 校验：用 KiCad 10.0.8 导出网表，和原图逐脚比对：209 个元件、189 个网络（连接关系和网络名）、全部未接引脚一致；ERC 0 条。
- 排版上的改动：
  - 每页按信号流从左到右画，器件周边的电阻电容直接连线，不再满页网络标签。
  - 电源和地用电源符号；跨页信号用全局标签，标签后的 `[n]` 是这个信号还出现在哪几页。
  - U301 拆成 5 个单元：GPIO A–G 在第 1 页，电源和时钟引脚移到第 2 页，和去耦电容、晶振画在一起。
  - 打叉、灰色的元件默认不贴（DNP）。
- 只有电路图被重画；原图的 PCB、封装和 BOM 没有动。

## 待确认

重画时在原图里看到的，原图如此，本册没有改：

1. **第 9 页：** U113 TS5A3159 的 NO（1 脚）接 USB_VBUS_RAW。USB_SEL_IN 为高时（默认，Q122 截止）COM 接 NO，TPS2553 的 ILIM 直接接到 VBUS，而不是经电阻接地。请对照 TPS2553 数据手册确认这个默认状态。
2. **第 8 页：** EFUSE_RTN（U106 的 RTN、EP、MODE，以及 R703 / R704 / C701 的下端）在网表里没有接到 GND，U106 的 GND（9 脚）单独接地。请对照 TPS26600 数据手册确认 RTN 与 GND 的接法。
3. **只有上拉、没有接到 MCU 的状态脚：** EXT_FAULT_N（第 8 页 U106 FLT#）、PGOOD_3V3（第 10 页 U201 PG）；U112 /FAULT（第 9 页）只接了测试点 TP112。

## 文件

| 路径 | 内容 |
| --- | --- |
| `kicad/` | KiCad 10 工程（`hwc_c062.kicad_pro`，根页 + 11 个子页，符号库 `hwc.kicad_sym`），可以直接打开继续改 |
| `tools/` | 生成上面工程和 PDF 的脚本 |
| `tools/data/schematic-pins.json` | 从原工程导出的逐脚网表，比对用 |
| `tools/data/orig_parts.json` | 原工程的元件属性（取值、料号、封装、是否贴装） |
| `tools/data/page12-rev2.svg` | 附录 A 的图，复制自 PR #1 的 `docs/apm-keysim/schematic/`；那边改了要重新复制 |

## 重新生成

需要 KiCad 10 和 python3；最后合成 PDF 一步用 macOS 自带的 swift。

```sh
docs/hardware-c-schematic/tools/build.sh
```

脚本会重新生成 `kicad/`，导出网表和原图逐脚比对、跑 ERC，再合成 `Hardware-C-schematic-redrawn.pdf`。各页的布局在 `tools/pages.py`，IC 的引脚分组在 `tools/boxes.py`。
