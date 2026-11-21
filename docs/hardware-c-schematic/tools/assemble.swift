// usage: assemble out.pdf kicad.pdf page12.svg apm.pdf
import AppKit
import PDFKit

let a = CommandLine.arguments
let outURL = URL(fileURLWithPath: a[1])
let W: CGFloat = 1190.55, H: CGFloat = 841.89, M: CGFloat = 40
let blue = NSColor(calibratedRed: 0, green: 0.235, blue: 0.47, alpha: 1)

func drawText(_ s: String, _ x: CGFloat, _ y: CGFloat, size: CGFloat, bold: Bool = false, color: NSColor = .black) {
    let font = NSFont(name: bold ? "PingFangSC-Semibold" : "PingFangSC-Regular", size: size) ?? NSFont.systemFont(ofSize: size)
    NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color]).draw(at: NSPoint(x: x, y: y))
}

func frame(_ ctx: CGContext, title: String, sub: [String], page: String) {
    ctx.setStrokeColor(NSColor(calibratedRed: 0.52, green: 0, blue: 0, alpha: 1).cgColor)
    ctx.setLineWidth(0.8)
    ctx.stroke(CGRect(x: 20, y: 20, width: W - 40, height: H - 40))
    drawText(title, M, H - M - 26, size: 22, bold: true, color: blue)
    for (i, l) in sub.enumerated() { drawText(l, M, H - M - 50 - CGFloat(i) * 18, size: 12, color: .darkGray) }
    drawText("Hardware-C C062 原理图 · 重画版 · " + page, M, 28, size: 10, color: .gray)
}

let data = NSMutableData()
var box = CGRect(x: 0, y: 0, width: W, height: H)
let ctx = CGContext(consumer: CGDataConsumer(data: data as CFMutableData)!, mediaBox: &box, nil)!
NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)

// Appendix A: page 12 rev 2 (SVG, drawn as vector)
ctx.beginPDFPage(nil)
frame(ctx, title: "附录 A · 第 12 页第 2 版：APM 按键模拟（提案）",
      sub: ["这一页是新增电路的设计提案，还没有画进嘉立创EDA，不在本册的网表比对范围内。",
            "引脚分配、电阻阵取值、元件清单和逐脚网表见仓库 docs/apm-keysim/schematic/page12-rev2.html。"],
      page: "附录 A")
let svg = NSImage(contentsOf: URL(fileURLWithPath: a[3]))!
let sw = W - 2 * M, sh = sw * svg.size.height / svg.size.width
svg.draw(in: NSRect(x: M, y: (H - 120 - sh) / 2 + 30, width: sw, height: sh))
ctx.endPDFPage()

// Appendix B: APM input reference (original PDF page, vector)
let apm = CGPDFDocument(URL(fileURLWithPath: a[4]) as CFURL)!
for i in 1...apm.numberOfPages {
    let pg = apm.page(at: i)!
    ctx.beginPDFPage(nil)
    frame(ctx, title: "附录 B · APM 按键采集电路参考",
          sub: ["原始文件 apm按键采集图.pdf，原样放入，未改动。第 12 页第 2 版的回读和学习功能按这份电路设计。"],
          page: "附录 B")
    let r = pg.getBoxRect(.mediaBox)
    let s = min((W - 2 * M) / r.width, (H - 150) / r.height)
    ctx.saveGState()
    ctx.translateBy(x: (W - r.width * s) / 2, y: (H - 110 - r.height * s) / 2 + 20)
    ctx.scaleBy(x: s, y: s)
    ctx.translateBy(x: -r.origin.x, y: -r.origin.y)
    ctx.drawPDFPage(pg)
    ctx.restoreGState()
    ctx.endPDFPage()
}
ctx.closePDF()

let doc = PDFDocument(url: URL(fileURLWithPath: a[2]))!
let app = PDFDocument(data: data as Data)!
let first = doc.pageCount
for i in 0..<app.pageCount { doc.insert(app.page(at: i)!, at: doc.pageCount) }
let root = doc.outlineRoot ?? PDFOutline()
if doc.outlineRoot == nil { doc.outlineRoot = root }
for (i, t) in ["附录 A · 第 12 页第 2 版（提案）", "附录 B · APM 按键采集电路参考"].enumerated() where first + i < doc.pageCount {
    let o = PDFOutline(); o.label = t
    o.destination = PDFDestination(page: doc.page(at: first + i)!, at: NSPoint(x: 0, y: H))
    root.insertChild(o, at: root.numberOfChildren)
}
doc.documentAttributes = [PDFDocumentAttribute.titleAttribute: "Hardware-C C062 原理图 · 重画版",
                          PDFDocumentAttribute.creatorAttribute: "KiCad 10.0.8 + hwc redraw"]
doc.write(to: outURL)
print("pages", doc.pageCount, "outline", root.numberOfChildren)
