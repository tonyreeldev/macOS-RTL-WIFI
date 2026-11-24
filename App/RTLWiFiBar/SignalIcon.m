/*
 * SignalIcon.m - native SF Symbol Wi-Fi status icon + menu glyphs.
 *
 * Menu bar pipeline: +[NSImage imageWithSystemSymbolName:] (macOS 11+, the
 * same API the system Wi-Fi item uses) -> drawn at the symbol's default
 * Regular weight, aspect-fit centered into an exact 20x20 pt @2x bitmap ->
 * template image, so
 * the menu bar tints it identically to the native Wi-Fi icon. If SF Symbols
 * are unavailable the runtime-drawn bar ladder from earlier builds remains
 * as a fallback, so the icon never comes out blank.
 *
 * Menu glyphs (grey Wi-Fi / white lock) are drawn the same way but tinted
 * with a fixed color and shipped as non-template images so the menu shows
 * the requested grey/white verbatim in both appearances.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import "SignalIcon.h"

@interface RTWSignalIcon ()
+ (NSImage *)drawIcon:(RTWIconState)state bars:(NSInteger)bars;      /* fallback */
+ (NSImage *)drawWifiArcsInColor:(NSColor *)color points:(CGFloat)points;
+ (NSImage *)drawLockInColor:(NSColor *)color points:(CGFloat)points;
+ (nullable NSImage *)symbolBitmapNamed:(NSString *)symbolName
                                 points:(CGFloat)points
                                 tint:(nullable NSColor *)tint;
@end

static const CGFloat kPi = 3.14159265358979323846;

@implementation RTWSignalIcon

/* One cached image per key; all call sites are main-thread (icon paint +
 * menu build), the lock only guards against the first poll racing a menu. */
static NSMutableDictionary<NSString *, NSImage *> *gIconCache;
static NSLock *gIconCacheLock;

+ (void)initialize {
    if (self == [RTWSignalIcon class]) {
        gIconCache = [NSMutableDictionary dictionary];
        gIconCacheLock = [[NSLock alloc] init];
    }
}

+ (NSImage *)cached:(NSString *)key maker:(NSImage *(^)(void))maker {
    [gIconCacheLock lock];
    NSImage *cached = gIconCache[key];
    [gIconCacheLock unlock];
    if (cached) return cached;

    NSImage *image = maker();

    [gIconCacheLock lock];
    gIconCache[key] = image;
    [gIconCacheLock unlock];
    return image;
}

#pragma mark Menu bar status icon (native SF Symbol, 20x20)

+ (NSImage *)iconForState:(RTWIconState)state rssiPercent:(int32_t)rssiPercent {
    (void)rssiPercent; /* the native glyph carries no strength ladder */
    NSString *name = (state == RTWIconStateMissing) ? @"wifi.slash" : @"wifi";
    NSString *key = [NSString stringWithFormat:@"sf-%@", name];

    NSImage *image = [self cached:key maker:^NSImage *{
        NSImage *native = [self symbolBitmapNamed:name points:20.0 tint:nil];
        if (native) return native;
        /* Fallback: the runtime-drawn ladder (never blank). */
        NSInteger bars = (state == RTWIconStateLinked) ? 4 : 0;
        return [self drawIcon:state bars:bars];
    }];
    return image;
}

/* Renders one SF Symbol into an exact points x points pt bitmap (@2x),
 * aspect preserved, glyph centered. tint == nil keeps the glyph black and
 * marks the image template (menu bar tinting); a tint ships the color
 * verbatim (non-template) for in-menu use. */
+ (nullable NSImage *)symbolBitmapNamed:(NSString *)symbolName
                                 points:(CGFloat)points
                                  tint:(nullable NSColor *)tint {
    if (![NSImage respondsToSelector:@selector(imageWithSystemSymbolName:accessibilityDescription:)]) {
        return nil;
    }
    NSImage *symbol = [NSImage imageWithSystemSymbolName:symbolName
                                  accessibilityDescription:@"Wi-Fi"];
    if (!symbol) return nil;

    /* NOTE: no NSImageSymbolConfiguration / NSSymbolWeight here. The macOS
     * 26 SDK dropped the NSSymbolWeight enum constants, so the point-size
     * and weight configuration is no longer portable. The default symbol
     * image already carries the Regular weight, and the aspect-fit draw into
     * the exact points x points canvas below re-renders the vector at the
     * target size - identical result to a size-configured glyph. */

    const CGFloat kScale = 2.0; /* @2x */
    const NSInteger kPixels = (NSInteger)(points * kScale);

    NSBitmapImageRep *rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:kPixels
                                                pixelsHigh:kPixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSCalibratedRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    if (!ctx) return nil;

    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];
    CGContextRef cg = ctx.CGContext;
    /* Bitmap-rep context: AppKit treats it as flipped (origin top-left),
     * and NSImage drawInRect: corrects for that flip by itself. Scale the
     * CTM for the @2x pixel density and NOTHING else - pairing drawInRect
     * with a manual y-flip double-flips and lands the glyph upside down
     * (the raw-CG fallback paths below do need their own flip, because
     * CGContext primitives never self-correct). */
    CGContextScaleCTM(cg, kScale, kScale);

    NSSize glyph = symbol.size;
    if (glyph.width <= 0.0 || glyph.height <= 0.0) glyph = NSMakeSize(points, points);
    /* Aspect-fit inside the square canvas, centered. */
    CGFloat fit = MIN(points / glyph.width, points / glyph.height);
    CGFloat w = glyph.width * fit;
    CGFloat h = glyph.height * fit;
    CGRect rect = NSMakeRect((points - w) / 2.0, (points - h) / 2.0, w, h);
    [symbol drawInRect:rect
               fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:1.0
          respectFlipped:YES
                   hints:nil];
    [NSGraphicsContext restoreGraphicsState];

    if (tint) {
        /* Recolor where the glyph drew: SourceIn keeps the glyph alpha and
         * replaces every drawn pixel with the fixed tint color. */
        CGContextRef cg2 = ctx.CGContext;
        CGContextSetBlendMode(cg2, kCGBlendModeSourceIn);
        CGContextSetFillColorWithColor(cg2, tint.CGColor);
        CGContextFillRect(cg2, CGRectMake(0, 0, kPixels, kPixels));
        CGContextSetBlendMode(cg2, kCGBlendModeNormal);
    }

    rep.size = NSMakeSize(points, points);
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
    [image addRepresentation:rep];
    image.template = (tint == nil);
    return image;
}

#pragma mark Collapsed-menu glyphs

+ (NSImage *)menuWifiIcon {
    return [self cached:@"menu-wifi" maker:^NSImage *{
        NSImage *native = [self symbolBitmapNamed:@"wifi" points:16.0
                                             tint:[NSColor colorWithCalibratedWhite:0.45 alpha:1.0]];
        if (native) return native;
        return [self drawWifiArcsInColor:[NSColor colorWithCalibratedWhite:0.45 alpha:1.0]
                                  points:16.0];
    }];
}

+ (NSImage *)menuLockIcon {
    return [self cached:@"menu-lock" maker:^NSImage *{
        NSImage *native = [self symbolBitmapNamed:@"lock.fill" points:16.0
                                             tint:[NSColor whiteColor]];
        if (native) return native;
        return [self drawLockInColor:[NSColor whiteColor] points:16.0];
    }];
}

+ (NSImage *)menuWifiTemplateIcon {
    return [self cached:@"menu-wifi-template" maker:^NSImage *{
        NSImage *native = [self symbolBitmapNamed:@"wifi" points:16.0 tint:nil];
        if (native) return native;
        NSImage *drawn = [self drawWifiArcsInColor:[NSColor blackColor] points:16.0];
        drawn.template = YES;
        return drawn;
    }];
}

+ (NSImage *)menuLockTemplateIcon {
    return [self cached:@"menu-lock-template" maker:^NSImage *{
        NSImage *native = [self symbolBitmapNamed:@"lock.fill" points:16.0 tint:nil];
        if (native) return native;
        NSImage *drawn = [self drawLockInColor:[NSColor blackColor] points:16.0];
        drawn.template = YES;
        return drawn;
    }];
}

#pragma mark Fallback glyphs (drawn, no SF Symbols)

/* Classic Wi-Fi arcs: three strokes + bottom dot, all in one fixed color. */
+ (NSImage *)drawWifiArcsInColor:(NSColor *)color points:(CGFloat)points {
    const CGFloat kScale = 2.0;
    const NSInteger kPixels = (NSInteger)(points * kScale);
    NSBitmapImageRep *rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:kPixels
                                                pixelsHigh:kPixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSCalibratedRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    if (!ctx) {
        NSImage *empty = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
        empty.template = NO;
        return empty;
    }
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];
    CGContextRef cg = ctx.CGContext;
    CGContextScaleCTM(cg, kScale, kScale);
    CGContextTranslateCTM(cg, 0, points);
    CGContextScaleCTM(cg, 1.0, -1.0);
    CGContextSetAllowsAntialiasing(cg, YES);

    CGContextSetStrokeColorWithColor(cg, color.CGColor);
    CGContextSetFillColorWithColor(cg, color.CGColor);
    CGContextSetLineWidth(cg, 1.7);
    CGContextSetLineCap(cg, kCGLineCapRound);

    CGFloat cx = points / 2.0;
    CGFloat cy = 4.2;
    CGFloat radii[3] = {3.0, 5.6, 8.2};
    for (NSInteger i = 0; i < 3; ++i) {
        /* ~220 deg sweep over the top (200 deg -> -20 deg, y-up space):
         * arcs bulge upward and open toward the bottom dot. */
        CGContextBeginPath(cg);
        CGContextAddArc(cg, cx, cy, radii[i], kPi + 0.35, -0.35, 1);
        CGContextStrokePath(cg);
    }
    CGContextFillEllipseInRect(cg, CGRectMake(cx - 1.7, cy - 1.7, 3.4, 3.4));

    [NSGraphicsContext restoreGraphicsState];
    rep.size = NSMakeSize(points, points);
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
    [image addRepresentation:rep];
    image.template = NO;
    return image;
}

/* Padlock: filled rounded body + stroked shackle, one fixed color. */
+ (NSImage *)drawLockInColor:(NSColor *)color points:(CGFloat)points {
    const CGFloat kScale = 2.0;
    const NSInteger kPixels = (NSInteger)(points * kScale);
    NSBitmapImageRep *rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:kPixels
                                                pixelsHigh:kPixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSCalibratedRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    if (!ctx) {
        NSImage *empty = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
        empty.template = NO;
        return empty;
    }
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];
    CGContextRef cg = ctx.CGContext;
    CGContextScaleCTM(cg, kScale, kScale);
    CGContextTranslateCTM(cg, 0, points);
    CGContextScaleCTM(cg, 1.0, -1.0);
    CGContextSetAllowsAntialiasing(cg, YES);

    CGContextSetFillColorWithColor(cg, color.CGColor);
    CGContextSetStrokeColorWithColor(cg, color.CGColor);
    CGContextSetLineWidth(cg, 1.6);

    CGFloat cx = points / 2.0;
    /* Shackle: 180 deg -> 0 deg over the top (y-up space). */
    CGContextBeginPath(cg);
    CGContextAddArc(cg, cx, 8.2, 3.1, kPi, 0.0, 1);
    CGContextStrokePath(cg);
    /* Body. */
    CGPathRef body = CGPathCreateWithRoundedRect(CGRectMake(cx - 4.6, 2.6, 9.2, 7.4), 1.8, 1.8, NULL);
    CGContextAddPath(cg, body);
    CGPathRelease(body);
    CGContextFillPath(cg);

    [NSGraphicsContext restoreGraphicsState];
    rep.size = NSMakeSize(points, points);
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
    [image addRepresentation:rep];
    image.template = NO;
    return image;
}

#pragma mark Legacy bar ladder (fallback only)

+ (NSImage *)drawIcon:(RTWIconState)state bars:(NSInteger)bars {
    const CGFloat kPoints = 20.0;
    const CGFloat kScale = 2.0;                 /* @2x */
    const NSInteger kPixels = (NSInteger)(kPoints * kScale);

    NSBitmapImageRep *rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                pixelsWide:kPixels
                                                pixelsHigh:kPixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSCalibratedRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];

    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    if (!ctx) {
        /* Absolute fallback: empty template image. */
        NSImage *empty = [[NSImage alloc] initWithSize:NSMakeSize(kPoints, kPoints)];
        empty.template = YES;
        return empty;
    }

    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:ctx];
    CGContextRef cg = ctx.CGContext;
    /* A bitmap-rep context is flipped (origin top-left) and its user space
     * equals pixel space. Scale by the pixel density ourselves, then flip,
     * so all glyph geometry below is written in 20x20 y-up point space. */
    CGContextScaleCTM(cg, kScale, kScale);
    CGContextTranslateCTM(cg, 0, kPoints);
    CGContextScaleCTM(cg, 1.0, -1.0);

    CGContextSetAllowsAntialiasing(cg, YES);
    CGContextSetFillColorWithColor(cg, [[NSColor blackColor] CGColor]);
    CGContextSetStrokeColorWithColor(cg, [[NSColor blackColor] CGColor]);

    /* Four bars, bottom-aligned baseline y=3, tops staggered by 3.5 pt. */
    const CGFloat barWidth = 2.6;
    const CGFloat gap = 1.2;
    const CGFloat baseline = 3.5;
    const CGFloat step = 3.5;
    const CGFloat x0 = 1.8;

    void (^barPath)(CGFloat, CGFloat) = ^(CGFloat x, CGFloat top) {
        CGRect rect = CGRectIntegral(CGRectMake(x, baseline, barWidth, top - baseline));
        CGPathRef path = CGPathCreateWithRoundedRect(rect, 0.8, 0.8, NULL);
        CGContextAddPath(cg, path);
        CGPathRelease(path);
    };

    if (state == RTWIconStateMissing) {
        /* Big X spanning the icon. */
        CGFloat inset = 2.5;
        CGFloat w = kPoints - inset * 2.0;
        CGContextSetLineWidth(cg, 2.4);
        CGContextSetLineCap(cg, kCGLineCapRound);
        CGContextBeginPath(cg);
        CGContextMoveToPoint(cg, inset, inset);
        CGContextAddLineToPoint(cg, inset + w, inset + w);
        CGContextMoveToPoint(cg, inset + w, inset);
        CGContextAddLineToPoint(cg, inset, inset + w);
        CGContextStrokePath(cg);
    } else {
        for (NSInteger i = 0; i < 4; ++i) {
            CGFloat top = baseline + step * (CGFloat)(i + 1);
            CGFloat x = x0 + (barWidth + gap) * (CGFloat)i;
            BOOL lit = (state == RTWIconStateLinked) && (i < bars);
            if (lit) {
                barPath(x, top);
                CGContextFillPath(cg);
            } else {
                /* Hollow stub bar: outline of the lower half, so idle reads
                 * as "radio present, no link" and never as a glitch. */
                CGFloat stubTop = baseline + step * 0.55;
                barPath(x, stubTop);
                CGContextFillPath(cg);
                (void)top;
            }
        }
    }

    [NSGraphicsContext restoreGraphicsState];

    /* Point size AFTER drawing: the image treats the 36x36 px buffer as an
     * 20 pt @2x representation. */
    rep.size = NSMakeSize(kPoints, kPoints);
    NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(kPoints, kPoints)];
    [image addRepresentation:rep];
    image.template = YES;
    image.name = @"RTLWiFiBarStatusIcon";
    return image;
}

@end
