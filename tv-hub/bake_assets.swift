import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import AppKit

let pngDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    .appendingPathComponent("tv-hub/assets_png")
let outDir = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    .appendingPathComponent("tv-hub/assets")
try FileManager.default.createDirectory(at: pngDir, withIntermediateDirectories: true)
try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

let cs = CGColorSpaceCreateDeviceRGB()

func rgba(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1.0) -> CGColor {
    CGColor(colorSpace: cs, components: [r/255.0, g/255.0, b/255.0, a])!
}

func makeCtx(_ w: Int, _ h: Int, opaque: Bool = false) -> CGContext {
    let info = opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
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

// Artwork helpers drawn directly onto flattened hero card textures (zero extra child Views!)
func drawPlexEmblem(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, size: CGFloat, alpha: CGFloat) {
    ctx.saveGState()
    ctx.setAlpha(alpha)
    ctx.translateBy(x: cx - size * 0.5, y: cy - size * 0.5)
    ctx.scaleBy(x: size / 240.0, y: size / 240.0)

    let frame = roundedRectPath(CGRect(x: 24, y: 24, width: 192, height: 192), 48)
    ctx.saveGState()
    ctx.addPath(frame); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 24, y: 24), p1: CGPoint(x: 216, y: 216),
               colors: [rgba(42, 34, 20), rgba(18, 16, 13)], locs: [0.0, 1.0])
    drawRadial(ctx, cx: 120, cy: 120, r: 85, c0: rgba(229, 160, 13, 0.24), c1: rgba(229, 160, 13, 0))
    ctx.restoreGState()
    ctx.addPath(frame); ctx.setLineWidth(2.2); ctx.setStrokeColor(rgba(255, 202, 74, 0.48)); ctx.strokePath()

    let chev = CGMutablePath()
    chev.move(to: CGPoint(x: 84, y: 58)); chev.addLine(to: CGPoint(x: 126, y: 58))
    chev.addLine(to: CGPoint(x: 172, y: 120)); chev.addLine(to: CGPoint(x: 126, y: 182))
    chev.addLine(to: CGPoint(x: 84, y: 182)); chev.addLine(to: CGPoint(x: 130, y: 120)); chev.closeSubpath()

    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: 8), blur: 18, color: rgba(229, 160, 13, 0.38))
    ctx.addPath(chev); ctx.setFillColor(rgba(238, 168, 18)); ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(chev); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 80, y: 56), p1: CGPoint(x: 172, y: 184),
               colors: [rgba(255, 218, 112), rgba(239, 167, 18), rgba(196, 118, 4)], locs: [0.0, 0.48, 1.0])
    let facet = CGMutablePath()
    facet.move(to: CGPoint(x: 84, y: 58)); facet.addLine(to: CGPoint(x: 126, y: 58))
    facet.addLine(to: CGPoint(x: 146, y: 85)); facet.addLine(to: CGPoint(x: 104, y: 85)); facet.closeSubpath()
    ctx.addPath(facet); ctx.setFillColor(rgba(255, 255, 255, 0.25)); ctx.fillPath()
    ctx.restoreGState()
    ctx.restoreGState()
}

func drawYouTubeEmblem(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, size: CGFloat, alpha: CGFloat) {
    ctx.saveGState()
    ctx.setAlpha(alpha)
    ctx.translateBy(x: cx - size * 0.5, y: cy - size * 0.5)
    ctx.scaleBy(x: size / 240.0, y: size / 240.0)

    let frame = roundedRectPath(CGRect(x: 24, y: 24, width: 192, height: 192), 48)
    ctx.saveGState()
    ctx.addPath(frame); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 24, y: 24), p1: CGPoint(x: 216, y: 216),
               colors: [rgba(42, 20, 26), rgba(19, 12, 16)], locs: [0.0, 1.0])
    drawRadial(ctx, cx: 120, cy: 120, r: 85, c0: rgba(255, 43, 61, 0.24), c1: rgba(255, 43, 61, 0))
    ctx.restoreGState()
    ctx.addPath(frame); ctx.setLineWidth(2.2); ctx.setStrokeColor(rgba(255, 89, 103, 0.48)); ctx.strokePath()

    let pill = roundedRectPath(CGRect(x: 46, y: 72, width: 148, height: 96), 28)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: 8), blur: 18, color: rgba(255, 36, 54, 0.38))
    ctx.addPath(pill); ctx.setFillColor(rgba(240, 34, 52)); ctx.fillPath()
    ctx.restoreGState()

    ctx.saveGState()
    ctx.addPath(pill); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 46, y: 72), p1: CGPoint(x: 194, y: 168),
               colors: [rgba(255, 62, 78), rgba(212, 14, 32)], locs: [0.0, 1.0])
    ctx.addPath(roundedRectPath(CGRect(x: 48, y: 73, width: 144, height: 42), 22))
    ctx.setFillColor(rgba(255, 255, 255, 0.14)); ctx.fillPath()
    ctx.restoreGState()

    let tri = CGMutablePath()
    tri.move(to: CGPoint(x: 105, y: 97)); tri.addLine(to: CGPoint(x: 150, y: 120)); tri.addLine(to: CGPoint(x: 105, y: 143)); tri.closeSubpath()
    ctx.addPath(tri); ctx.setFillColor(rgba(255, 255, 255)); ctx.fillPath()
    ctx.restoreGState()
}

func drawSwitch2Emblem(_ ctx: CGContext, cx: CGFloat, cy: CGFloat, w: CGFloat, h: CGFloat, alpha: CGFloat) {
    ctx.saveGState()
    ctx.setAlpha(alpha)
    ctx.translateBy(x: cx - w * 0.5, y: cy - h * 0.5)
    ctx.scaleBy(x: h / 240.0, y: h / 240.0)

    let plate = roundedRectPath(CGRect(x: 14, y: 34, width: 244, height: 172), 44)
    ctx.saveGState()
    ctx.addPath(plate); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 14, y: 34), p1: CGPoint(x: 258, y: 206),
               colors: [rgba(28, 35, 50), rgba(15, 20, 30)], locs: [0.0, 1.0])
    drawRadial(ctx, cx: 56, cy: 120, r: 95, c0: rgba(0, 204, 238, 0.18), c1: rgba(0,0,0,0))
    drawRadial(ctx, cx: 216, cy: 120, r: 95, c0: rgba(245, 44, 62, 0.18), c1: rgba(0,0,0,0))
    ctx.restoreGState()
    ctx.addPath(plate); ctx.setLineWidth(2.0); ctx.setStrokeColor(rgba(255, 255, 255, 0.15)); ctx.strokePath()

    // Left Joy-Con (Cyan)
    let leftJC = CGMutablePath()
    leftJC.move(to: CGPoint(x: 70, y: 68)); leftJC.addLine(to: CGPoint(x: 58, y: 68))
    leftJC.addArc(center: CGPoint(x: 58, y: 90), radius: 22, startAngle: -.pi/2, endAngle: .pi, clockwise: true)
    leftJC.addLine(to: CGPoint(x: 36, y: 150))
    leftJC.addArc(center: CGPoint(x: 58, y: 150), radius: 22, startAngle: .pi, endAngle: .pi/2, clockwise: true)
    leftJC.addLine(to: CGPoint(x: 70, y: 172)); leftJC.closeSubpath()
    ctx.addPath(leftJC); ctx.setFillColor(rgba(32, 39, 53)); ctx.fillPath()
    ctx.addPath(leftJC); ctx.setLineWidth(2.4); ctx.setStrokeColor(rgba(24, 218, 250)); ctx.strokePath()
    ctx.addPath(roundedRectPath(CGRect(x: 64, y: 76, width: 5, height: 88), 2.5)); ctx.setFillColor(rgba(24, 218, 250)); ctx.fillPath()
    ctx.addEllipse(in: CGRect(x: 43.5, y: 89.5, width: 17, height: 17)); ctx.setFillColor(rgba(15, 20, 31)); ctx.fillPath()
    ctx.addEllipse(in: CGRect(x: 43.5, y: 89.5, width: 17, height: 17)); ctx.setLineWidth(2.2); ctx.setStrokeColor(rgba(24, 218, 250)); ctx.strokePath()
    for pt in [(52, 126), (44, 134), (60, 134), (52, 142)] {
        ctx.addEllipse(in: CGRect(x: Double(pt.0)-2.7, y: Double(pt.1)-2.7, width: 5.4, height: 5.4))
        ctx.setFillColor(rgba(154, 168, 190)); ctx.fillPath()
    }

    // Center Display
    let bezel = roundedRectPath(CGRect(x: 74, y: 68, width: 124, height: 104), 10)
    ctx.addPath(bezel); ctx.setFillColor(rgba(13, 17, 26)); ctx.fillPath()
    ctx.addPath(bezel); ctx.setLineWidth(2.0); ctx.setStrokeColor(rgba(255, 255, 255, 0.18)); ctx.strokePath()
    let scr = roundedRectPath(CGRect(x: 81, y: 75, width: 110, height: 90), 6)
    ctx.saveGState(); ctx.addPath(scr); ctx.clip()
    drawLinear(ctx, p0: CGPoint(x: 81, y: 75), p1: CGPoint(x: 191, y: 165),
               colors: [rgba(24, 33, 49), rgba(10, 14, 22)], locs: [0.0, 1.0])
    let sheen = CGMutablePath()
    sheen.move(to: CGPoint(x: 81, y: 75)); sheen.addLine(to: CGPoint(x: 158, y: 75))
    sheen.addLine(to: CGPoint(x: 108, y: 165)); sheen.addLine(to: CGPoint(x: 81, y: 165)); sheen.closeSubpath()
    ctx.addPath(sheen); ctx.setFillColor(rgba(255, 255, 255, 0.045)); ctx.fillPath()
    ctx.restoreGState()
    drawText(ctx, "2", x: 81, y: 116, size: 44, weight: .bold, color: NSColor(calibratedRed: 0.95, green: 0.965, blue: 0.99, alpha: 1.0), centerIn: 110)

    // Right Joy-Con (Coral)
    let rightJC = CGMutablePath()
    rightJC.move(to: CGPoint(x: 202, y: 68)); rightJC.addLine(to: CGPoint(x: 214, y: 68))
    rightJC.addArc(center: CGPoint(x: 214, y: 90), radius: 22, startAngle: -.pi/2, endAngle: 0, clockwise: false)
    rightJC.addLine(to: CGPoint(x: 236, y: 150))
    rightJC.addArc(center: CGPoint(x: 214, y: 150), radius: 22, startAngle: 0, endAngle: .pi/2, clockwise: false)
    rightJC.addLine(to: CGPoint(x: 202, y: 172)); rightJC.closeSubpath()
    ctx.addPath(rightJC); ctx.setFillColor(rgba(32, 39, 53)); ctx.fillPath()
    ctx.addPath(rightJC); ctx.setLineWidth(2.4); ctx.setStrokeColor(rgba(255, 72, 88)); ctx.strokePath()
    ctx.addPath(roundedRectPath(CGRect(x: 203, y: 76, width: 5, height: 88), 2.5)); ctx.setFillColor(rgba(255, 72, 88)); ctx.fillPath()
    for pt in [(220, 92), (212, 100), (228, 100), (220, 108)] {
        ctx.addEllipse(in: CGRect(x: Double(pt.0)-2.7, y: Double(pt.1)-2.7, width: 5.4, height: 5.4))
        ctx.setFillColor(rgba(154, 168, 190)); ctx.fillPath()
    }
    ctx.addEllipse(in: CGRect(x: 211.5, y: 125.5, width: 17, height: 17)); ctx.setFillColor(rgba(15, 20, 31)); ctx.fillPath()
    ctx.addEllipse(in: CGRect(x: 211.5, y: 125.5, width: 17, height: 17)); ctx.setLineWidth(2.2); ctx.setStrokeColor(rgba(255, 72, 88)); ctx.strokePath()
    ctx.restoreGState()
}

// 1. Opaque RGB Obsidian Backdrop (480x270 -> ASTC 6x6)
do {
    let w = 480, h = 270
    let ctx = makeCtx(w, h, opaque: true)
    drawLinear(ctx, p0: CGPoint(x: 0, y: 0), p1: CGPoint(x: 0, y: h),
               colors: [rgba(13, 17, 27), rgba(8, 10, 16), rgba(5, 7, 11)], locs: [0.0, 0.58, 1.0])
    ctx.saveGState(); ctx.scaleBy(x: 1.6, y: 1.0)
    drawRadial(ctx, cx: 150, cy: 8, r: 180, c0: rgba(46, 62, 98, 0.34), c1: rgba(46, 62, 98, 0))
    ctx.restoreGState()
    drawRadial(ctx, cx: 90, cy: 124, r: 130, c0: rgba(229, 160, 13, 0.085), c1: rgba(229, 160, 13, 0))
    drawRadial(ctx, cx: 240, cy: 120, r: 130, c0: rgba(255, 43, 61, 0.075), c1: rgba(255, 43, 61, 0))
    drawRadial(ctx, cx: 364, cy: 116, r: 110, c0: rgba(0, 200, 234, 0.075), c1: rgba(0, 200, 234, 0))
    drawRadial(ctx, cx: 420, cy: 132, r: 110, c0: rgba(242, 41, 58, 0.075), c1: rgba(242, 41, 58, 0))
    savePNG(ctx, pngDir, "bg_base.png")
}

// 2. Localized Gaussian Focus Halos (176x192 -> ASTC 6x6)
func bakeHalo(_ name: String, leftColor: CGColor, rightColor: CGColor) {
    let w = 176, h = 192
    let ctx = makeCtx(w, h)
    ctx.scaleBy(x: 0.5, y: 0.5)
    ctx.saveGState(); ctx.translateBy(x: 136, y: 192); ctx.scaleBy(x: 1.0, y: 1.15)
    drawRadial(ctx, cx: 0, cy: 0, r: 146, c0: leftColor, c1: rgba(0, 0, 0, 0)); ctx.restoreGState()
    ctx.saveGState(); ctx.translateBy(x: 216, y: 192); ctx.scaleBy(x: 1.0, y: 1.15)
    drawRadial(ctx, cx: 0, cy: 0, r: 146, c0: rightColor, c1: rgba(0, 0, 0, 0)); ctx.restoreGState()
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: 14), blur: 32, color: rgba(0, 0, 0, 0.72))
    ctx.addPath(roundedRectPath(CGRect(x: 42, y: 40, width: 268, height: 294), 20))
    ctx.setFillColor(rgba(0, 0, 0, 0.65)); ctx.fillPath(); ctx.restoreGState()
    savePNG(ctx, pngDir, name)
}
bakeHalo("halo_plex.png", leftColor: rgba(255, 190, 48, 0.36), rightColor: rgba(229, 154, 10, 0.32))
bakeHalo("halo_youtube.png", leftColor: rgba(255, 62, 78, 0.35), rightColor: rgba(235, 30, 48, 0.32))
bakeHalo("halo_switch2.png", leftColor: rgba(0, 208, 240, 0.36), rightColor: rgba(248, 46, 66, 0.36))

// 3. Flattened All-In-One Hero Cards (556x610 -> ASTC 4x4: plate + keycap + static badges + emblem + title + CTA + arrow!)
func bakeFlattenedCard(idx: Int, name: String, focused: Bool) {
    let w = 556, h = 610
    let ctx = makeCtx(w, h)
    let r: CGFloat = 34
    let path = roundedRectPath(CGRect(x: 2, y: 2, width: w - 4, height: h - 4), r)

    let accentL: CGColor = idx == 0 ? rgba(235, 166, 16, focused ? 0.24 : 0.10)
                         : idx == 1 ? rgba(255, 43, 61, focused ? 0.23 : 0.10)
                                    : rgba(0, 204, 238, focused ? 0.22 : 0.09)
    let accentR: CGColor = idx == 0 ? rgba(255, 198, 66, focused ? 0.15 : 0.06)
                         : idx == 1 ? rgba(255, 92, 106, focused ? 0.14 : 0.06)
                                    : rgba(245, 44, 62, focused ? 0.22 : 0.09)
    let topRim: [CGColor] = !focused ? [rgba(255,255,255,0), rgba(255,255,255,0.14), rgba(255,255,255,0)]
                          : idx == 0 ? [rgba(255,194,59,0.20), rgba(255,204,77,0.95), rgba(255,194,59,0.20)]
                          : idx == 1 ? [rgba(255,89,103,0.20), rgba(255,96,110,0.95), rgba(255,89,103,0.20)]
                                     : [rgba(0,204,238,0.85), rgba(240,246,255,0.95), rgba(245,44,62,0.85)]

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

    // Keycap [1] / [2] / [3] baked right in!
    let kcRect = CGRect(x: 36, y: 34, width: 34, height: 34)
    let kcPath = roundedRectPath(kcRect, 10)
    ctx.addPath(kcPath); ctx.setFillColor(rgba(255, 255, 255, focused ? 0.16 : 0.07)); ctx.fillPath()
    ctx.addPath(kcPath); ctx.setLineWidth(1.0); ctx.setStrokeColor(rgba(255, 255, 255, focused ? 0.28 : 0.13)); ctx.strokePath()
    drawText(ctx, "\(idx + 1)", x: 36, y: 51, size: 16, weight: .semibold,
             color: focused ? NSColor(calibratedRed: 0.953, green: 0.965, blue: 0.984, alpha: 1.0)
                            : NSColor(calibratedRed: 0.635, green: 0.682, blue: 0.769, alpha: 1.0), centerIn: 34)

    // Static badges baked in (YouTube: 4K HDR; Card 3: HDMI 1)
    if idx == 1 {
        let bp = roundedRectPath(CGRect(x: 424, y: 34, width: 96, height: 34), 17)
        ctx.addPath(bp); ctx.setFillColor(rgba(255, 255, 255, 0.055)); ctx.fillPath()
        ctx.addPath(bp); ctx.setLineWidth(1.0); ctx.setStrokeColor(rgba(255, 255, 255, 0.11)); ctx.strokePath()
        drawText(ctx, "4K HDR", x: 424, y: 51, size: 14, weight: .semibold,
                 color: NSColor(calibratedRed: 0.635, green: 0.682, blue: 0.769, alpha: 1.0), centerIn: 96)
    } else if idx == 2 {
        let bp = roundedRectPath(CGRect(x: 222, y: 34, width: 94, height: 34), 17)
        ctx.addPath(bp); ctx.setFillColor(rgba(0, 200, 234, 0.16)); ctx.fillPath()
        ctx.addPath(bp); ctx.setLineWidth(1.0); ctx.setStrokeColor(rgba(0, 200, 234, 0.42)); ctx.strokePath()
        drawText(ctx, "HDMI 1", x: 222, y: 51, size: 14, weight: .semibold,
                 color: NSColor(calibratedRed: 0.647, green: 0.953, blue: 0.988, alpha: 1.0), centerIn: 94)
    }

    // Center monumental artwork emblem (1.045x larger + 100% vivid on focus; calm 0.78 on idle)
    let emAlpha: CGFloat = focused ? 1.0 : 0.78
    let emScale: CGFloat = focused ? 1.045 : 1.0
    if idx == 0 { drawPlexEmblem(ctx, cx: 278, cy: 224, size: 252 * emScale, alpha: emAlpha) }
    else if idx == 1 { drawYouTubeEmblem(ctx, cx: 278, cy: 224, size: 252 * emScale, alpha: emAlpha) }
    else { drawSwitch2Emblem(ctx, cx: 278, cy: 224, w: 286 * emScale, h: 252 * emScale, alpha: emAlpha) }

    // Card Title ("Plex" / "YouTube" / "HDMI 1") baked crisp!
    let titles = ["Plex", "YouTube", "HDMI 1"]
    let titleColor = focused ? NSColor(calibratedRed: 0.953, green: 0.965, blue: 0.984, alpha: 1.0)
                             : NSColor(calibratedRed: 0.730, green: 0.775, blue: 0.845, alpha: 1.0)
    drawText(ctx, titles[idx], x: 36, y: 406, size: 50, weight: .semibold, color: titleColor)

    // CTA Pill + Right Arrow baked right into the card plate!
    let ctaRect = CGRect(x: 36, y: 508, width: 484, height: 62)
    let ctaPath = roundedRectPath(ctaRect, 18)
    ctx.saveGState(); ctx.addPath(ctaPath); ctx.clip()
    if !focused {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(255,255,255,0.065), rgba(255,255,255,0.045)], locs: [0.0, 1.0])
    } else if idx == 0 {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(255,198,66), rgba(226,148,6)], locs: [0.0, 1.0])
    } else if idx == 1 {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(255,72,88), rgba(222,22,40)], locs: [0.0, 1.0])
    } else {
        drawLinear(ctx, p0: CGPoint(x: 36, y: 508), p1: CGPoint(x: 520, y: 570),
                   colors: [rgba(248,250,253), rgba(218,229,244)], locs: [0.0, 1.0])
    }
    ctx.restoreGState()
    ctx.addPath(ctaPath); ctx.setLineWidth(1.5)
    ctx.setStrokeColor(!focused ? rgba(255,255,255,0.11)
                       : idx == 0 ? rgba(255,234,170,0.72)
                       : idx == 1 ? rgba(255,176,184,0.62)
                                  : rgba(255,255,255,0.9))
    ctx.strokePath()

    // Arrow icon inside CTA pill (nudged +5px on focus)
    let ax: CGFloat = focused ? 479 : 474
    let ay: CGFloat = 539
    let arrowCol = !focused ? rgba(162, 174, 196, 1.0)
                   : idx == 0 ? rgba(22, 16, 4, 1.0)
                   : idx == 1 ? rgba(255, 255, 255, 1.0)
                              : rgba(9, 13, 22, 1.0)
    ctx.setStrokeColor(arrowCol); ctx.setLineWidth(2.5); ctx.setLineCap(.round); ctx.setLineJoin(.round)
    ctx.move(to: CGPoint(x: ax, y: ay)); ctx.addLine(to: CGPoint(x: ax + 14, y: ay))
    ctx.move(to: CGPoint(x: ax + 8, y: ay - 6)); ctx.addLine(to: CGPoint(x: ax + 14, y: ay)); ctx.addLine(to: CGPoint(x: ax + 8, y: ay + 6))
    ctx.strokePath()

    savePNG(ctx, pngDir, name)
}

bakeFlattenedCard(idx: 0, name: "card_plex_idle.png", focused: false)
bakeFlattenedCard(idx: 0, name: "card_plex_focus.png", focused: true)
bakeFlattenedCard(idx: 1, name: "card_youtube_idle.png", focused: false)
bakeFlattenedCard(idx: 1, name: "card_youtube_focus.png", focused: true)
bakeFlattenedCard(idx: 2, name: "card_switch2_idle.png", focused: false)
bakeFlattenedCard(idx: 2, name: "card_switch2_focus.png", focused: true)

// 4. Pre-baked Dock Rail Frame (1082x76 -> ASTC 4x4, 7 balanced buttons: Inputs | VPN | Vol- Mute Vol+ | Free RAM Settings)
do {
    let w = 1082, h = 76
    let ctx = makeCtx(w, h)
    let p = roundedRectPath(CGRect(x: 1, y: 1, width: w - 2, height: h - 2), 24)
    ctx.addPath(p); ctx.setFillColor(rgba(17, 22, 34, 0.94)); ctx.fillPath()
    ctx.addPath(p); ctx.setLineWidth(1.4); ctx.setStrokeColor(rgba(255, 255, 255, 0.14)); ctx.strokePath()
    // 3 vertical hairlines matching exact 7-button dock layout (after Inputs x=172, after VPN x=324, after Vol+ x=726)
    ctx.setFillColor(rgba(255, 255, 255, 0.13))
    ctx.fill(CGRect(x: 172, y: 23, width: 1, height: 30))
    ctx.fill(CGRect(x: 324, y: 23, width: 1, height: 30))
    ctx.fill(CGRect(x: 726, y: 23, width: 1, height: 30))
    savePNG(ctx, pngDir, "dock_rail.png")
}

// 5. Crisp 48x48 White Alpha Vector Icons (saved directly as tiny ~500B PNGs into outDir)
func bakeIcon(_ name: String, draw: (CGContext) -> Void) {
    let s = 48
    let ctx = makeCtx(s, s)
    ctx.scaleBy(x: 2.0, y: 2.0)
    ctx.setStrokeColor(rgba(255, 255, 255, 1.0)); ctx.setFillColor(rgba(255, 255, 255, 1.0))
    ctx.setLineWidth(2.1); ctx.setLineCap(.round); ctx.setLineJoin(.round)
    draw(ctx)
    savePNG(ctx, outDir, name)
}

bakeIcon("icon_tv.png") { c in
    c.addPath(roundedRectPath(CGRect(x: 3, y: 4, width: 18, height: 12), 2.5)); c.strokePath()
    c.move(to: CGPoint(x: 8.5, y: 20)); c.addLine(to: CGPoint(x: 15.5, y: 20)); c.strokePath()
}
bakeIcon("icon_inputs.png") { c in
    c.addPath(roundedRectPath(CGRect(x: 2.5, y: 6, width: 19, height: 12), 3)); c.strokePath()
    c.move(to: CGPoint(x: 7, y: 12)); c.addLine(to: CGPoint(x: 11, y: 12))
    c.move(to: CGPoint(x: 15, y: 12)); c.addLine(to: CGPoint(x: 17, y: 12)); c.strokePath()
}
bakeIcon("icon_minus.png") { c in
    c.setLineWidth(2.4); c.move(to: CGPoint(x: 5, y: 12)); c.addLine(to: CGPoint(x: 19, y: 12)); c.strokePath()
}
bakeIcon("icon_plus.png") { c in
    c.setLineWidth(2.4)
    c.move(to: CGPoint(x: 12, y: 5)); c.addLine(to: CGPoint(x: 12, y: 19))
    c.move(to: CGPoint(x: 5, y: 12)); c.addLine(to: CGPoint(x: 19, y: 12)); c.strokePath()
}
bakeIcon("icon_vol.png") { c in
    let p = CGMutablePath()
    p.move(to: CGPoint(x: 11, y: 5)); p.addLine(to: CGPoint(x: 6, y: 9)); p.addLine(to: CGPoint(x: 2.5, y: 9))
    p.addLine(to: CGPoint(x: 2.5, y: 15)); p.addLine(to: CGPoint(x: 6, y: 15)); p.addLine(to: CGPoint(x: 11, y: 19)); p.closeSubpath()
    c.addPath(p); c.strokePath()
    c.addArc(center: CGPoint(x: 12, y: 12), radius: 4.5, startAngle: -.pi/4, endAngle: .pi/4, clockwise: false); c.strokePath()
    c.addArc(center: CGPoint(x: 12, y: 12), radius: 8.2, startAngle: -.pi/3.6, endAngle: .pi/3.6, clockwise: false); c.strokePath()
}
bakeIcon("icon_mute.png") { c in
    let p = CGMutablePath()
    p.move(to: CGPoint(x: 11, y: 5)); p.addLine(to: CGPoint(x: 6, y: 9)); p.addLine(to: CGPoint(x: 2.5, y: 9))
    p.addLine(to: CGPoint(x: 2.5, y: 15)); p.addLine(to: CGPoint(x: 6, y: 15)); p.addLine(to: CGPoint(x: 11, y: 19)); p.closeSubpath()
    c.addPath(p); c.strokePath()
    c.move(to: CGPoint(x: 16.5, y: 9)); c.addLine(to: CGPoint(x: 22, y: 15))
    c.move(to: CGPoint(x: 22, y: 9)); c.addLine(to: CGPoint(x: 16.5, y: 15)); c.strokePath()
}
bakeIcon("icon_ram.png") { c in
    c.addArc(center: CGPoint(x: 12, y: 12), radius: 7.5, startAngle: -.pi*0.75, endAngle: .pi*0.95, clockwise: false); c.strokePath()
    c.move(to: CGPoint(x: 4, y: 5)); c.addLine(to: CGPoint(x: 4, y: 10)); c.addLine(to: CGPoint(x: 9, y: 10)); c.strokePath()
}
bakeIcon("icon_settings.png") { c in
    c.addEllipse(in: CGRect(x: 9, y: 9, width: 6, height: 6)); c.strokePath()
    c.addEllipse(in: CGRect(x: 5, y: 5, width: 14, height: 14)); c.strokePath()
    for i in 0..<8 {
        let a = CGFloat(i) * .pi / 4.0
        c.move(to: CGPoint(x: 12 + cos(a)*7.0, y: 12 + sin(a)*7.0))
        c.addLine(to: CGPoint(x: 12 + cos(a)*9.8, y: 12 + sin(a)*9.8))
    }
    c.strokePath()
}
bakeIcon("icon_home.png") { c in
    c.move(to: CGPoint(x: 3, y: 10.5)); c.addLine(to: CGPoint(x: 12, y: 3)); c.addLine(to: CGPoint(x: 21, y: 10.5))
    c.move(to: CGPoint(x: 5.5, y: 9.5)); c.addLine(to: CGPoint(x: 5.5, y: 20)); c.addLine(to: CGPoint(x: 18.5, y: 20)); c.addLine(to: CGPoint(x: 18.5, y: 9.5)); c.strokePath()
}
bakeIcon("icon_vpn.png") { c in
    let p = CGMutablePath()
    p.move(to: CGPoint(x: 12, y: 2.5))
    p.addLine(to: CGPoint(x: 19.5, y: 5.5))
    p.addLine(to: CGPoint(x: 19.5, y: 11.5))
    p.addQuadCurve(to: CGPoint(x: 12, y: 21.2), control: CGPoint(x: 19.5, y: 17.6))
    p.addQuadCurve(to: CGPoint(x: 4.5, y: 11.5), control: CGPoint(x: 4.5, y: 17.6))
    p.addLine(to: CGPoint(x: 4.5, y: 5.5))
    p.closeSubpath()
    c.addPath(p); c.strokePath()
    c.move(to: CGPoint(x: 9, y: 12)); c.addLine(to: CGPoint(x: 11.2, y: 14.3)); c.addLine(to: CGPoint(x: 15.5, y: 9.6)); c.strokePath()
}

// 6. Combined 480x48 10-icon white alpha strip for bare-metal Tier 4 GLES2 (zero libpng needed!)
do {
    let iconNames = ["icon_tv.png", "icon_inputs.png", "icon_minus.png", "icon_mute.png", "icon_plus.png", "icon_ram.png", "icon_settings.png", "icon_home.png", "icon_vol.png", "icon_vpn.png"]
    let ctx = makeCtx(480, 48)
    for (i, n) in iconNames.enumerated() {
        let url = outDir.appendingPathComponent(n)
        if let src = CGImageSourceCreateWithURL(url as CFURL, nil),
           let cg = CGImageSourceCreateImageAtIndex(src, 0, nil) {
            ctx.saveGState()
            ctx.translateBy(x: CGFloat(i * 48), y: 48)
            ctx.scaleBy(x: 1, y: -1)
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: 48, height: 48))
            ctx.restoreGState()
        }
    }
    savePNG(ctx, pngDir, "icons_atlas.png")
}

print("Baked flattened source PNGs cleanly.")
