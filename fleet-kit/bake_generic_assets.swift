import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import AppKit

// Bakes generic HDMI 1 card textures for Samsung CU8000 TVs.
// Outputs PNGs to fleet-kit/assets/generic_png/ for astcenc compression to fleet-kit/assets/generic/.

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let pngDir = root.appendingPathComponent("tv-hub/assets_png")
try FileManager.default.createDirectory(at: pngDir, withIntermediateDirectories: true)

let cs = CGColorSpaceCreateDeviceRGB()

func rgba(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1.0) -> CGColor {
    CGColor(colorSpace: cs, components: [r/255.0, g/255.0, b/255.0, a])!
}

func makeCtx(_ w: Int, _ h: Int) -> CGContext {
    let info = CGImageAlphaInfo.premultipliedLast.rawValue
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs, bitmapInfo: info)!
    ctx.setAllowsAntialiasing(true)
    ctx.setShouldAntialias(true)
    ctx.interpolationQuality = .high
    ctx.translateBy(x: 0, y: CGFloat(h))
    ctx.scaleBy(x: 1, y: -1)
    return ctx
}

func savePNG(_ ctx: CGContext, _ dir: URL, _ name: String) {
    let img = ctx.makeImage()!
    let url = dir.appendingPathComponent(name)
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, img, nil)
    CGImageDestinationFinalize(dest)
}

func roundedRectPath(_ rect: CGRect, _ r: CGFloat) -> CGPath {
    CGPath(roundedRect: rect, cornerWidth: r, cornerHeight: r, transform: nil)
}

func drawRadial(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, r: CGFloat, c0: CGColor, c1: CGColor) {
    let grad = CGGradient(colorsSpace: cs, colors: [c0, c1] as CFArray, locations: [0.0, 1.0])!
    ctx.drawRadialGradient(grad, startCenter: CGPoint(x: cx, y: cy), startRadius: 0,
                           endCenter: CGPoint(x: cx, y: cy), endRadius: r, options: [.drawsAfterEndLocation])
}

func drawLinear(_ ctx: CGContext, p0: CGPoint, p1: CGPoint, colors: [CGColor], locs: [CGFloat]) {
    let grad = CGGradient(colorsSpace: cs, colors: colors as CFArray, locations: locs)!
    ctx.drawLinearGradient(grad, start: p0, end: p1, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
}

func drawText(_ ctx: CGContext, _ text: String, x: CGFloat, y: CGFloat, size: CGFloat, weight: NSFont.Weight, color: NSColor, centerIn width: CGFloat? = nil) {
    let nsCtx = NSGraphicsContext(cgContext: ctx, flipped: true)
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = nsCtx
    let font = NSFont.systemFont(ofSize: size, weight: weight)
    let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
    let str = NSAttributedString(string: text, attributes: attrs)
    let sz = str.size()
    let dx = (width != nil) ? (x + (width! - sz.width) * 0.5) : x
    str.draw(at: CGPoint(x: dx, y: y - sz.height * 0.5))
    NSGraphicsContext.restoreGraphicsState()
}

func drawHdmiEmblem(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, size: CGFloat, alpha: CGFloat) {
    ctx.saveGState()
    ctx.setAlpha(alpha)
    ctx.translateBy(x: cx - size * 0.5, y: cy - size * 0.5)
    ctx.scaleBy(x: size / 240.0, y: size / 240.0)

    let frame = roundedRectPath(CGRect(x: 24, y: 24, width: 192, height: 192), 48)
    ctx.saveGState()
    ctx.addPath(frame); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 24, y: 24), p1: CGPoint(x: 216, y: 216),
               colors: [rgba(18, 36, 48), rgba(11, 18, 28)], locs: [0.0, 1.0])
    drawRadial(ctx, cx: 120, cy: 120, r: 92, c0: rgba(0, 200, 234, 0.28), c1: rgba(0, 200, 234, 0))
    ctx.restoreGState()
    ctx.addPath(frame); ctx.setLineWidth(2.2); ctx.setStrokeColor(rgba(56, 218, 248, 0.52)); ctx.strokePath()

    // Sleek HDMI trapezoid port silhouette
    let port = CGMutablePath()
    port.move(to: CGPoint(x: 62, y: 92))
    port.addLine(to: CGPoint(x: 178, y: 92))
    port.addLine(to: CGPoint(x: 178, y: 126))
    port.addLine(to: CGPoint(x: 162, y: 146))
    port.addLine(to: CGPoint(x: 78, y: 146))
    port.addLine(to: CGPoint(x: 62, y: 126))
    port.closeSubpath()

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: 8), blur: 18, color: rgba(0, 204, 238, 0.35))
    ctx.addPath(port)
    ctx.setFillColor(rgba(12, 22, 34))
    ctx.fillPath()
    ctx.restoreGState()

    ctx.addPath(port)
    ctx.setLineWidth(5.0)
    ctx.setLineJoin(.round)
    ctx.setStrokeColor(rgba(120, 232, 252))
    ctx.strokePath()

    // Inner contact bar + gold pins
    let bar = roundedRectPath(CGRect(x: 82, y: 112, width: 76, height: 12), 4)
    ctx.addPath(bar)
    ctx.setFillColor(rgba(56, 218, 248, 0.85))
    ctx.fillPath()
    for i in 0..<5 {
        let px = 88.0 + Double(i) * 14.5
        ctx.setFillColor(rgba(255, 220, 120, 0.92))
        ctx.fill(CGRect(x: px, y: 105, width: 6, height: 5))
    }

    ctx.restoreGState()
}

// 1. Generic cyan-slate halo (exact match to halo_switch2: 176x192 -> 6x6 ASTC, 15376 bytes)
do {
    let w = 176, h = 192
    let ctx = makeCtx(w, h)
    drawRadial(ctx, cx: 68, cy: 96, r: 90, c0: rgba(0, 200, 234, 0.36), c1: rgba(0, 0, 0, 0))
    drawRadial(ctx, cx: 108, cy: 96, r: 90, c0: rgba(56, 152, 255, 0.32), c1: rgba(0, 0, 0, 0))
    savePNG(ctx, pngDir, "halo_switch2.png")
}

// 2. Generic HDMI 1 flattened cards (556x610)
func bakeGenericCard(name: String, focused: Bool) {
    let w = 556, h = 610
    let ctx = makeCtx(w, h)
    let path = roundedRectPath(CGRect(x: 2, y: 2, width: w - 4, height: h - 4), 34)

    let accentL = rgba(0, 200, 234, focused ? 0.22 : 0.09)
    let accentR = rgba(56, 152, 255, focused ? 0.20 : 0.08)
    let topRim: [CGColor] = !focused
        ? [rgba(255,255,255,0), rgba(255,255,255,0.14), rgba(255,255,255,0)]
        : [rgba(0,204,238,0.85), rgba(224,244,255,0.95), rgba(56,152,255,0.85)]

    ctx.saveGState()
    ctx.addPath(path); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 0, y: 0), p1: CGPoint(x: 60, y: h),
               colors: focused ? [rgba(29, 36, 52), rgba(16, 21, 32)] : [rgba(20, 25, 37), rgba(12, 16, 24)],
               locs: [0.0, 1.0])
    drawRadial(ctx, cx: focused ? 175 : 220, cy: 220, r: 290, c0: accentL, c1: rgba(0, 0, 0, 0))
    drawRadial(ctx, cx: focused ? 385 : 340, cy: 220, r: 290, c0: accentR, c1: rgba(0, 0, 0, 0))

    let rimRect = CGRect(x: 28.0, y: 2.0, width: Double(w - 56), height: focused ? 3.0 : 1.5)
    ctx.saveGState(); ctx.clip(to: rimRect)
    drawLinear(ctx, p0: CGPoint(x: 28, y: 2), p1: CGPoint(x: w - 28, y: 2), colors: topRim, locs: [0.0, 0.5, 1.0])
    ctx.restoreGState()
    ctx.restoreGState()

    ctx.addPath(path)
    ctx.setLineWidth(focused ? 3.0 : 1.8)
    ctx.setStrokeColor(focused ? topRim[1] : rgba(255, 255, 255, 0.11))
    ctx.strokePath()

    // Keycap [3]
    let kcRect = CGRect(x: 36, y: 34, width: 34, height: 34)
    let kcPath = roundedRectPath(kcRect, 10)
    ctx.addPath(kcPath); ctx.setFillColor(rgba(255, 255, 255, focused ? 0.16 : 0.07)); ctx.fillPath()
    ctx.addPath(kcPath); ctx.setLineWidth(1.0); ctx.setStrokeColor(rgba(255, 255, 255, focused ? 0.28 : 0.13)); ctx.strokePath()
    drawText(ctx, "3", x: 36, y: 51, size: 16, weight: .semibold,
             color: focused ? NSColor(calibratedRed: 0.953, green: 0.965, blue: 0.984, alpha: 1.0)
                            : NSColor(calibratedRed: 0.635, green: 0.682, blue: 0.769, alpha: 1.0), centerIn: 34)

    // Static badge "INPUT"
    let bp = roundedRectPath(CGRect(x: 222, y: 34, width: 94, height: 34), 17)
    ctx.addPath(bp); ctx.setFillColor(rgba(0, 200, 234, 0.16)); ctx.fillPath()
    ctx.addPath(bp); ctx.setLineWidth(1.0); ctx.setStrokeColor(rgba(0, 200, 234, 0.42)); ctx.strokePath()
    drawText(ctx, "HDMI 1", x: 222, y: 51, size: 14, weight: .semibold,
             color: NSColor(calibratedRed: 0.647, green: 0.953, blue: 0.988, alpha: 1.0), centerIn: 94)

    // Center HDMI emblem
    let emAlpha: CGFloat = focused ? 1.0 : 0.78
    let emScale: CGFloat = focused ? 1.045 : 1.0
    drawHdmiEmblem(ctx, cx: 278, cy: 224, size: 252 * emScale, alpha: emAlpha)

    // Card Title ("HDMI 1")
    let titleColor = focused ? NSColor(calibratedRed: 0.953, green: 0.965, blue: 0.984, alpha: 1.0)
                             : NSColor(calibratedRed: 0.730, green: 0.775, blue: 0.845, alpha: 1.0)
    drawText(ctx, "HDMI 1", x: 36, y: 406, size: 50, weight: .semibold, color: titleColor)

    // CTA Pill + Arrow
    let ctaRect = CGRect(x: 36, y: 508, width: 484, height: 62)
    let ctaPath = roundedRectPath(ctaRect, 18)
    ctx.saveGState(); ctx.addPath(ctaPath); ctx.clip()
    if !focused {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(255,255,255,0.065), rgba(255,255,255,0.045)], locs: [0.0, 1.0])
    } else {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(248,250,253), rgba(218,229,244)], locs: [0.0, 1.0])
    }
    ctx.restoreGState()
    ctx.addPath(ctaPath); ctx.setLineWidth(1.5)
    ctx.setStrokeColor(!focused ? rgba(255,255,255,0.11) : rgba(255,255,255,0.9))
    ctx.strokePath()

    let ax: CGFloat = focused ? 479 : 474
    let ay: CGFloat = 539
    let arrowCol = !focused ? rgba(162, 174, 196, 1.0) : rgba(9, 13, 22, 1.0)
    ctx.setStrokeColor(arrowCol); ctx.setLineWidth(2.5); ctx.setLineCap(.round); ctx.setLineJoin(.round)
    ctx.move(to: CGPoint(x: ax, y: ay)); ctx.addLine(to: CGPoint(x: ax + 14, y: ay))
    ctx.move(to: CGPoint(x: ax + 8, y: ay - 6)); ctx.addLine(to: CGPoint(x: ax + 14, y: ay)); ctx.addLine(to: CGPoint(x: ax + 8, y: ay + 6))
    ctx.strokePath()

    savePNG(ctx, pngDir, name)
}

bakeGenericCard(name: "card_switch2_idle.png", focused: false)
bakeGenericCard(name: "card_switch2_focus.png", focused: true)
print("Baked generic HDMI 1 source PNGs cleanly.")
