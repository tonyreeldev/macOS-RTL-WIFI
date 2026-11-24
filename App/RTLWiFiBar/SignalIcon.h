/*
 * SignalIcon.h - status bar + menu glyphs for RTLWiFiBar.
 *
 * The menu bar icon is the NATIVE Wi-Fi glyph: an SF Symbol ("wifi",
 * "wifi.slash") rendered into an exact 20x20 pt bitmap, template-tinted so
 * it matches the light and dark menu bars exactly like the stock macOS
 * Wi-Fi icon. No image assets, no custom bar ladder on the hot path.
 *
 * Two extra non-template glyphs (fixed colors) decorate the active network
 * row at the top of the collapsed Networks menu: a grey Wi-Fi icon on the
 * left and a white lock icon on the right of the known, activated network.
 *
 * Drawing uses NSBitmapImageRep + CoreGraphics only - the AppKit class
 * factory +[NSImage imageWithSize:flipped:rect:drawingHandler:] is no longer
 * exposed by the macOS 26 SDK and must not appear anywhere here.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, RTWIconState) {
    RTWIconStateMissing = 0,  /* wifi.slash - driver/dongle absent */
    RTWIconStateIdle,         /* wifi       - driver present, not linked */
    RTWIconStateLinked        /* wifi       - linked (native glyph) */
};

@interface RTWSignalIcon : NSObject
/* Menu bar status icon: native SF Symbol Wi-Fi glyph at 20x20 pt. */
+ (NSImage *)iconForState:(RTWIconState)state rssiPercent:(int32_t)rssiPercent;
/* Collapsed-menu glyphs for the pinned active network row. */
+ (NSImage *)menuWifiIcon;   /* grey, non-template */
+ (NSImage *)menuLockIcon;   /* white, non-template */
/* mejoras-optiplex: template variants for the active-network row, so the
 * image view tints them for light and dark menus. */
+ (NSImage *)menuWifiTemplateIcon;
+ (NSImage *)menuLockTemplateIcon;
@end

NS_ASSUME_NONNULL_END
