/*
 * AppDelegate.m - RTLWiFiBar status bar application.
 *
 * Design notes:
 *  - Every driver call runs on a background serial queue; the main thread
 *    only paints the icon, keeps the menu snapshot and runs dialogs. The
 *    1 Hz poll never blocks the UI and never scans by itself.
 *  - A background NSActivity hint stops App Nap from suspending the poller
 *    after long idle periods. It deliberately does NOT block the Mac's own
 *    idle sleep the way NSActivityIdleSystemSleepDisabled would.
 *  - Scans happen on demand only (launch, wake, manual) plus one bounded
 *    recovery rescan while the station is unlinked and the survey is
 *    stale, so the Networks menu stays clickable. v8.7.13 (the user's
 *    decision): AUTO-JOIN IS GONE. The app never submits a join by
 *    itself - not on launch, not after a wake, not on a timer. The
 *    8.7.12 "driver settle" wait was a timer band-aid over a driver
 *    defect and is reverted with the feature it served. A network is
 *    remembered only after the user picks it and types its password,
 *    and the app starts CLEAN: nothing is saved before the password is
 *    entered, so a restart always asks for the password again.
 *    (Amended in v8.7.17 - see below: the user asked the saved network
 *    back, so profiles persist and ONE startup auto-rejoin is armed.)
 *    (kext 1.0.7 carries the matching join-path repair: a spent WPA
 *    handshake budget no longer freezes the association state machine,
 *    and the boot rescue can no longer re-plug a join that is still
 *    alive.)
 *  - v8.7.14 (field log 2026-11-18 07:03-07:05, build 8.7.13: "join
 *    submit -> ok" and then nothing, ever): TWO defects, both fixed.
 *    1) The join monitor existed but nothing ever CALLED it - dead code,
 *    so a join never confirmed, retried or failed out loud. It is wired
 *    into the poll tick now, follows the association live through the
 *    kext 1.0.8 join-state OID (scanning -> authenticating ->
 *    associating -> linked) and on failure names the furthest state
 *    reached plus any deauth the AP sent.
 *    2) The survey-freshness rescan fired 4 ms and 0.8 s after the join
 *    submit, and every scan trigger restarts the kext's STA sweep,
 *    erasing auth/assoc progress - the join could never converge. No
 *    scan runs while a join is in flight (the kext guards its scan
 *    handler too, driver 1.0.8), and the freshness gate no longer
 *    stacks a second scan behind one already running.
 *    The v8.7.3 deauth helpers are IMPLEMENTED instead of merely
 *    declared (the build warned about both; linkDropReasonNote was
 *    still called on every drop and would have crashed the agent).
 *  - v8.7.15 (field log 2026-11-18 07:42-07:58, build 8.7.14 + driver
 *    1.0.8: "it not reconnect after another reboot even if I try Forget
 *    network and change the USB Port"): the driver went silent at every
 *    teardown, so the AP kept a ghost association and ignored the next
 *    AUTH after a reboot - the join cycled scanning -> authenticating
 *    forever (progress states 1 -> 2 -> 1 -> 2, never 3). Kext 1.0.9
 *    says goodbye to the AP on every teardown (DISASSOC/DEAUTH through
 *    the core newstate INIT with a real reason arg while the chip is
 *    still alive) and re-plugs a latched-dead USB transport by itself
 *    (session rescue: 20 s persistence, 2 per attach) instead of
 *    demanding a manual replug. The app only renames the wedge hint to
 *    match: wait for the self re-plug, then rejoin.
 *  - v8.7.16 (packaging round, no code changes): the installer now puts
 *    RTLWiFiBar in /Applications (the old StatusBarApp utility and its
 *    RtWlanHelper are no longer shipped at all), the Wlan.Software
 *    launch agent points at /Applications/RTLWiFiBar.app and keeps it
 *    alive, and Create_Build.tool copies the built app next to the .pkg
 *    in Build/Release. The bundle ships a new high-resolution app icon.
 *    Join/connection behavior is exactly v8.7.15's.
 *  - v8.7.17 (the user's final-touch request, field report 2026-11-18:
 *    "everything is working perfectly ... after a reboot, the
 *    application needs to automatically recognize the previously saved
 *    network - currently, I have to manually reselect my network, after
 *    which it reconnects just as it did before the reboot"): the saved
 *    network now SURVIVES a restart and the app reconnects to it by
 *    itself. The v8.7.13 launch wipe is gone: Profiles.plist loads at
 *    launch, the most recently used saved network is pinned again, and
 *    ONE bounded auto-rejoin is armed for it. The rejoin fires only
 *    after the dongle is up and a survey actually sees the network, it
 *    runs through the exact manual join path (submit + join monitor +
 *    45 s window + at most two re-submits + honest failure), it fires
 *    once per launch, and it stands down the moment the user picks or
 *    forgets a network. Mid-session drops still wait for the user
 *    (v8.7.13 rule) - nothing joins on a timer, after a wake, or
 *    without a saved password. Kext untouched at 1.0.9.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import "AppDelegate.h"
#import "DriverClient.h"
#import "DriverClientPrivate.h"
#import "ProfileStore.h"
#import "RTWLog.h"
#import "SignalIcon.h"
#import <unistd.h>
#import <math.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <ifaddrs.h>
#import <net/if.h>
#import <netinet/in.h>
#import <arpa/inet.h>

/* Menu item tags keep the snapshot -> action mapping stable. */
static NSInteger const kTagStatusLine = 1;
static NSInteger const kTagScanningLine = 2;
static NSInteger const kTagNetworkBase = 100;      /* +index */
static NSInteger const kTagNetworksMenu = 3;       /* collapsed submenu holder */
static NSInteger const kTagRescan = 10;
static NSInteger const kTagForget = 12;
/* kTagDiagnostics (13) retired in v8.7.7: Copy Diagnostics was removed from
 * the menu - Reveal Log opens the very same log file. */
static NSInteger const kTagAbout = 14;
static NSInteger const kTagQuit = 15;
/* kTagBootSetup (16) retired in v8.7.8: "Enable Driver at Boot" and its
 * osascript setup are removed from the app entirely - loading the driver
 * at boot is the PKG installer's job, never the app's (user request). */

/* Defaults key: last network the user picked in the collapsed menu.
 * v8.7.13: the pin is never written to NSUserDefaults anymore and the
 * key is REMOVED at every launch. v8.7.17: selection memory lives in
 * the profile store now (the most recently used saved network is
 * re-pinned at launch); the key stays declared only so the launch wipe
 * (and Forget) can clear leftovers from older builds. */
static NSString * const kDefaultsActiveSsid = @"RTWActiveSsid";
/* mejoras: "pause Wi-Fi while a cable is connected" (off by default:
 * some users need both connections up at the same time). */
static NSString * const kDefaultsPauseWithCable = @"RTWPauseWifiWithCable";
/* mejoras: AP choice. Prefer 5 GHz (default ON) and the AP the
 * user last picked per network (ssid -> "aabbccddeeff"). */
static NSString * const kDefaultsPrefer5GHz = @"RTWPrefer5GHz";
/* mejoras: x of a menu item's text inside a custom row view. */
static const CGFloat kListTextInset = 25.0;
static NSString * const kDefaultsPreferredAP = @"RTWPreferredAP";

/* mejoras: YES when a wired Ethernet interface (other than the
 * dongle) is up with a routable IPv4 address. Wired = SystemConfiguration
 * type Ethernet, which excludes built-in Wi-Fi, bridges and VPN tunnels. */
static BOOL rtlWiredEthernetUp(NSString *dongleIfname, NSString **wiredName) {
    NSMutableSet<NSString *> *wired = [NSMutableSet set];
    CFArrayRef all = SCNetworkInterfaceCopyAll();
    if (all) {
        for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
            SCNetworkInterfaceRef itf = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
            CFStringRef type = SCNetworkInterfaceGetInterfaceType(itf);
            CFStringRef bsd = SCNetworkInterfaceGetBSDName(itf);
            if (type && bsd && CFEqual(type, kSCNetworkInterfaceTypeEthernet)) {
                [wired addObject:(__bridge NSString *)bsd];
            }
        }
        CFRelease(all);
    }
    if (dongleIfname.length) [wired removeObject:dongleIfname];
    if (!wired.count) return NO;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0 || !list) return NO;
    BOOL found = NO;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_name) continue;
        if ((a->ifa_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING)) continue;
        NSString *name = [NSString stringWithUTF8String:a->ifa_name];
        if (![wired containsObject:name]) continue;
        uint32_t ip = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
        if ((ip >> 16) == 0xA9FE) continue; /* 169.254.x.x self-assigned */
        if (wiredName) *wiredName = name;
        found = YES;
        break;
    }
    freeifaddrs(list);
    return found;
}
/* v8.7.19: seconds the dongle must have been reachable before the startup /
 * post-rescue auto-rejoin may submit. Field logs 2026-11-20: a dual-band
 * (802.11ac) dongle joined fine when the user picked the network ~40 s after
 * plugging it, but failed ("the AP never answered the authentication
 * request") when the auto-rejoin submitted ~1.5 s after the driver came up
 * after a reboot or a software re-plug. 802.11n dongles join in 3-4 s right
 * after attach, so they only lose a few seconds. Tune here. */
static const NSTimeInterval kAutoRejoinSettleSeconds = 5.0;

/* v8.7.9 (the reference file's documented rule, Model.hpp): fixed-width
 * driver fields can carry an SSID padded past the real name with NUL or
 * control bytes (the reference field capture carried exactly
 * "Network-Wifi\x01"). Any exact-length string comparison can therefore
 * never match a healthy link. Every SSID comparison in the app now runs
 * on printable prefixes. Padding is control bytes only (NUL, 0x01, ...);
 * UTF-8 bytes >= 0x80 stay intact so international SSIDs keep working. */
static NSUInteger rtlPrintableSsidLength(NSString *s) {
    if (!s) return 0;
    NSUInteger n = s.length;
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [s characterAtIndex:i];
        if (c < 0x20 || c == 0x7f) return i;
    }
    return n;
}

static BOOL rtlPrintableSsidEqual(NSString *a, NSString *b) {
    NSUInteger la = rtlPrintableSsidLength(a);
    NSUInteger lb = rtlPrintableSsidLength(b);
    if (!la || !lb || la != lb) return NO;
    return [a compare:b options:NSLiteralSearch range:NSMakeRange(0, la)] == NSOrderedSame;
}

/* 8.7.24: "Networks" row with a chevron, like the native Wi-Fi menu. A
 * view-based menu row keeps the menu open when clicked, so the list unfolds
 * in place. */
@interface RTLDisclosureRow : NSView
@property (copy) void (^onClick)(void);
@property (assign, nonatomic) BOOL expanded;
@property (assign) BOOL hovered;
@end

@implementation RTLDisclosureRow
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
            options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
              owner:self userInfo:nil];
        [self addTrackingArea:area];
    }
    return self;
}
- (void)setExpanded:(BOOL)expanded { _expanded = expanded; [self setNeedsDisplay:YES]; }
- (void)mouseEntered:(NSEvent *)e { self.hovered = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent *)e { self.hovered = NO; [self setNeedsDisplay:YES]; }
- (void)mouseUp:(NSEvent *)e { if (self.onClick) self.onClick(); }
- (void)drawRect:(NSRect)dirty {
    if (self.hovered) {
        [[NSColor selectedContentBackgroundColor] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 5, 0) xRadius:4 yRadius:4] fill];
    }
    NSColor *fg = self.hovered ? [NSColor whiteColor] : [NSColor labelColor];
    NSFont *font = [NSFont menuFontOfSize:13];
    NSDictionary *attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: fg};
    NSString *title = NSLocalizedString(@"Networks", nil);
    CGFloat h = [title sizeWithAttributes:attrs].height;
    CGFloat y = (NSHeight(self.bounds) - h) / 2.0;
    [title drawAtPoint:NSMakePoint(14, y) withAttributes:attrs];
    NSString *chev = self.expanded ? @"\u2304" : @"\u203A"; /* v or > */
    NSDictionary *cattrs = @{NSFontAttributeName: [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],
                             NSForegroundColorAttributeName: fg};
    NSSize cs = [chev sizeWithAttributes:cattrs];
    [chev drawAtPoint:NSMakePoint(NSWidth(self.bounds) - cs.width - 14, (NSHeight(self.bounds) - cs.height) / 2.0 + (self.expanded ? 3 : 0))
       withAttributes:cattrs];
}
@end

@interface AppDelegate () <NSMenuDelegate>
/* mejoras: AP currently linked, for the network list. */
@property (nonatomic, copy, nullable) NSString *listLinkedSsid;
@property (nonatomic, strong, nullable) RTWNetworkEntry *listLinkedEntry;
/* mejoras: network being switched to (leave-then-join in flight). */
@property (nonatomic, copy, nullable) NSString *switchingToSsid;
/* mejoras: network to go back to after a full sweep while linked. */
@property (nonatomic, copy, nullable) NSString *scanDetourSsid;
/* mejoras: bumps on every network switch; a newer pick cancels an older sweep. */
@property (assign) NSUInteger switchGeneration;
/* mejoras: band column of the network list, shared with the connected row. */
@property (assign) CGFloat listBandTab;
/* mejoras: "ssid|bssid" of every AP in the driver's LAST raw survey
 * (what the kext can pin right now; the menu list also keeps older APs). */
@property (copy, nullable) NSSet<NSString *> *driverSurveyKeys;
@property (copy, nullable) NSArray<RTWNetworkEntry *> *driverSurveyEntries;
/* mejoras: last "sweep to find the preferred AP" (bounds it to one a minute). */
@property (assign) NSTimeInterval lastPreferSweepAt;
/* mejoras: Wi-Fi paused because a cable is connected. */
@property (nonatomic, assign) BOOL cablePaused;
@property (nonatomic, copy, nullable) NSString *cablePausedSsid;
@property (nonatomic, assign) BOOL cableOverride;     /* user joined anyway; until the cable goes */
@property (nonatomic, assign) NSInteger cableGoneTicks;
@property (nonatomic, strong, nullable) NSTimer *cableTimer;
@property (strong) RTWDriverClient *driver;
@property (strong) RTWProfileStore *profiles;   /* app-owned plist, no Keychain */
@property (strong) NSStatusItem *statusItem;
@property (strong) NSMenu *menu;
/* Shared state, guarded by stateLock; written on the poll queue. */
@property (strong) NSLock *stateLock;
@property (strong, nullable) RTWDriverStatus *lastStatus;
@property (copy, nullable) NSArray<RTWNetworkEntry *> *lastNetworks;
@property (strong) NSMutableDictionary<NSString *, NSDictionary *> *seenNetworks; /* "ssid|bssid" -> @{e: entry, t: time} */
@property (assign) BOOL scanning;
@property (assign) NSTimeInterval lastScanFinished;
@property (copy, nullable) NSString *savedSsid;
@property (copy) NSArray<NSString *> *knownSsids;   /* ssids with a remembered profile */
@property (strong, nullable) NSMenu *networksSubmenu; /* collapsed Networks submenu */
@property (assign) BOOL networksExpanded;       /* 8.7.24 inline disclosure */
@property (assign) NSInteger networksInlineCount; /* rows inserted under the header */
/* Join outcome tracking (poll queue only). joinAttemptFailures counts
 * dead 45 s association windows of the CURRENT user join; the monitor
 * re-submits the user's own stream twice and then reports the failure
 * and stops. It is not a backoff ladder - nothing is ever submitted
 * automatically. */
@property (assign) NSInteger joinAttemptFailures;
@property (assign) BOOL linkWasUp;              /* link transition tracker (v8.7.2) */
@property (assign) NSTimeInterval lastLinkUpAt;
@property (assign) uint32_t deauthReasonAtLinkUp; /* v8.7.3: deauth-reason value while this link lived */
/* Join in-flight window (written from main + poll queue; the window only
 * gates retries, so the tiny cross-thread race is tolerable). */
@property (assign) NSTimeInterval joinStartedAt;
@property (assign) NSTimeInterval joinInFlightUntil;
@property (copy, nullable) NSString *joinInFlightSsid;
@property (strong, nullable) RTWNetworkEntry *joinWatchEntry; /* exact stream the monitor retries */
@property (copy, nullable) NSString *joinWatchKey;            /* retry key - session memory only */
/* v8.7.14: live join progress. joinLastSeenState mirrors the kext's
 * net80211 station state (0xFF = not polled yet); joinDeauthReasonSeen
 * captures a deauth that arrived during THIS join; lastJoinFailureNote
 * is the honest failure line shown in the menu until the next join or
 * link. All written on the poll queue. */
@property (assign) uint32_t joinLastSeenState;
@property (assign) uint32_t joinDeauthReasonSeen;
/* 2026-11-22: the watched join came from a menu click that used the saved
 * password (no prompt). If it ends in the WPA handshake, the saved password
 * is probably out of date (phone hotspot, changed router key): offer the
 * prompt then. Cleared with the join watch. */
@property (assign) BOOL joinWatchSavedPass;
@property (copy, nullable) NSString *lastJoinFailureNote;
@property (assign) NSInteger consecutivePollFailures;
@property (strong, nullable) NSSecureTextField *pendingPassField;
@property (strong, nullable) NSTextField *pendingPlainField; /* Show password twin */
@property (strong, nullable) dispatch_source_t pollTimer;
@property (strong, nullable) dispatch_queue_t pollQueue;
@property (strong, nullable) dispatch_queue_t workQueue;
@property (strong, nullable) id activityToken;
@property (strong, nullable) NSString *pendingJoinSsid; /* join awaiting passphrase */
/* v8.7.7: pending profile save. Armed ONLY by a join the user started from
 * the Networks menu, committed when the link is actually up - nothing is
 * ever written before the connection exists (the user's rule). */
@property (copy, nullable) NSString *pendingSaveSsid;
@property (copy, nullable) NSString *pendingSavePass;
@property (assign) uint8_t pendingSaveAuth;
@property (assign) uint32_t pendingSaveChannel;
/* v8.7.17: the startup auto-rejoin. Armed ONCE at launch when a saved
 * profile exists; consumed by the first survey that sees the network;
 * stands down the moment the user picks or forgets a network; gives up
 * after five surveys never saw it. Written at launch (main thread) and
 * read on the poll/work queues - the tiny cross-thread window only
 * affects the one-shot arming, same tolerance as the join window. */
@property (copy, nullable) NSString *autoRejoinArmedSsid;
@property (assign) NSInteger autoRejoinScansWaited;
/* v8.7.18: post-rescue rejoin. When the kext's boot rescue re-enumerates the
 * dongle (USB reset) the user client dies and reopens - but the startup
 * auto-rejoin has long been consumed and the join watch failed out, so
 * nothing ever retried (field 2026-11-19 23:05: the kext rescue fired at
 * 23:05:02, the dongle came back cured, the NIC stayed red). On the
 * driver's RETURN after a lost-client episode one bounded auto-rejoin is
 * re-armed for the most recently used profile - max 2 per session. */
@property (assign) BOOL awaitingDriverReturn;
@property (assign) NSInteger rescueRejoinsUsed;
/* v8.8.4: mid-session drop rejoin budget - at most 6 per rolling hour. */
@property (assign) NSInteger dropRejoinsUsed;
@property (assign) NSTimeInterval dropRejoinWindowStart;
/* v8.7.19: when the user client last became reachable (0 = not reachable).
 * Reset every time the client is lost, so a rescue re-plug restarts the
 * settle clock. */
@property (assign) NSTimeInterval driverReachableSince;
@property (assign) BOOL autoRejoinSettleLogged;
/* 8.8.5: post-connect address check (poll queue only). 10 s after the link
 * comes up the dongle's interface must hold a routable IPv4; the result is
 * logged with the DHCP/TX counters, and Rejoin stays clickable while the
 * link has no address. */
@property (assign) BOOL linkAddressChecked;
@property (assign) BOOL linkHasNoAddress;
@property (assign) NSTimeInterval linkAddressLastCheck;
@property (assign) BOOL securingLogged;
/* 8.8.5: "Disconnect USB WiFi" - the user took the dongle off the network.
 * Nothing joins or scans by itself until Reconnect (or a network picked in
 * the menu). Session only: the next launch rejoins the saved network. */
@property (assign) BOOL userDisconnected;
/* v8.7.8: the v8.7.4 boot-loader offer state (driverMissingSince,
 * bootSetupPrompted, bootSetupRunning) is gone together with the
 * "Enable Driver at Boot" flow - the app never runs osascript. */
/* v8.7.6: scan-refusal self-heal (kIOReturnNotAttached after boot attach). */
@property (assign) NSInteger scanTriggerFailures;     /* consecutive rejected scan triggers */
@property (assign) NSInteger scanHealCount;           /* client restarts done for scan refusal */
@property (assign) NSTimeInterval lastScanClientRestart; /* 0 = never restarted */
@property (assign) BOOL wedgedNoticeShown;            /* v8.7.11: boot-wedge notice active */
/* Private plumbing - ARC needs every selector visible before its first use. */
- (void)buildStatusItem;
- (void)startPollTimer;
- (void)stopPollTimer;
- (void)pollTick;
- (void)pollTickBody;
- (void)updateIcon:(nullable RTWDriverStatus *)status;
- (void)updateIconBody:(nullable RTWDriverStatus *)status;
- (NSString *)statusTooltip:(nullable RTWDriverStatus *)status;
- (void)performScan;
- (void)autoRescan;                          /* 8.8.5: silent rescan for automatic paths */
- (void)toggleUSBWifi:(nullable NSMenuItem *)sender; /* 8.8.5: Disconnect / Reconnect USB WiFi */
- (void)armAutoRejoinAfterDrop:(NSTimeInterval)now;  /* v8.8.4 */
- (void)checkLinkAddress:(nullable RTWDriverStatus *)status now:(NSTimeInterval)now; /* 8.8.5 */
- (NSString *)linkDropReasonNote;           /* v8.7.3: deauth-reason log note */
- (NSString *)decodeDeauthReason:(uint32_t)reason;
- (NSString *)nameForJoinState:(uint32_t)state; /* v8.7.14 progress names */
- (NSString *)joinFailureDiagnosis;             /* v8.7.14 honest failure */
- (NSString *)statusLine:(nullable RTWDriverStatus *)status;
- (NSString *)statusDetailLine:(nullable RTWDriverStatus *)status;
- (NSString *)titleForEntry:(RTWNetworkEntry *)entry;
- (void)submitJoin:(RTWNetworkEntry *)entry passphrase:(nullable NSString *)pass;
- (void)noteJoinSubmittedForEntry:(RTWNetworkEntry *)entry passphrase:(nullable NSString *)pass;
- (BOOL)monitorJoinFlight:(nullable RTWDriverStatus *)status;
- (void)clearJoinFlightWatch;
- (void)offerNewPasswordForEntry:(RTWNetworkEntry *)entry; /* 2026-11-22 */
/* v8.7.17: startup auto-rejoin plumbing. */
- (void)maybeFireStartupAutoRejoinWithRecords:(NSArray<RTWNetworkEntry *> *)records;
- (void)submitAutoRejoin:(RTWNetworkEntry *)entry passphrase:(nullable NSString *)pass;
- (void)standDownStartupAutoRejoin:(NSString *)reason;
- (void)revealLog:(nullable NSMenuItem *)sender;
- (void)toggleShowPassword:(NSButton *)sender;
- (nullable NSString *)promptPassphrase:(NSString *)ssid;
- (void)alert:(NSString *)title text:(NSString *)text;
/* v8.7.7: save-on-established-connection plumbing. */
- (void)armPendingNetworkSaveForEntry:(RTWNetworkEntry *)entry
                            passphrase:(nullable NSString *)pass;
- (void)commitPendingNetworkSave;
- (void)clearPendingNetworkSave;
/* Collapsed Networks menu plumbing. */
- (void)rebuildMainMenu;
- (void)rebuildNetworksSubmenu:(NSMenu *)subnet;
- (void)appendNetworksToMenu:(NSMenu *)subnet;
- (NSArray<NSString *> *)titlesForSubmenuRows:(NSArray<RTWNetworkEntry *> *)entries
                                    excluding:(NSString *)activeSsid
                                fallbackTitle:(NSString *)fallback;
- (CGFloat)submenuRowWidthForTitles:(NSArray<NSString *> *)titles;
- (nullable NSView *)activeNetworkRowForSsid:(NSString *)ssid
                                      status:(nullable RTWDriverStatus *)status
                                      locked:(BOOL)locked
                                      width:(CGFloat)width;
- (NSString *)activeRowTitleForSsid:(NSString *)ssid
                             status:(nullable RTWDriverStatus *)status;
- (BOOL)isKnownSsid:(NSString *)ssid;
- (void)markKnownSsid:(NSString *)ssid;
- (void)markActiveSsid:(NSString *)ssid;
@end

@implementation AppDelegate

#pragma mark Lifecycle

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    (void)note;
    NSDictionary *info = [NSBundle mainBundle].infoDictionary;
    RTWLog(@"RTLWiFiBar %@ (build %@) starting (pid %d)",
           info[@"CFBundleShortVersionString"] ?: @"?",
           info[@"CFBundleVersion"] ?: @"?", (int)getpid());

    /* App Nap hint for the 1 Hz poller (background, not sleep-blocking). */
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{kDefaultsPrefer5GHz: @YES}];
    self.activityToken = [[NSProcessInfo processInfo] beginActivityWithOptions:
                              NSActivityBackground reason:@"RTLWiFiBar driver poll"];

    self.pollQueue = dispatch_queue_create("com.rtlwifi.RTLWiFiBar.poll", DISPATCH_QUEUE_SERIAL);
    self.workQueue = dispatch_queue_create("com.rtlwifi.RTLWiFiBar.work", DISPATCH_QUEUE_SERIAL);

    /* Startup containment: a login-time AppKit or driver hiccup must not
     * kill the agent (Monterey boots were dying right here). Log it and
     * keep going; the poller below retries the status item via updateIcon
     * until it exists. */
    @try {
        self.driver = [[RTWDriverClient alloc] init];
        self.profiles = [[RTWProfileStore alloc] init];
        /* v8.7.17 (the user's final touch, field report 2026-11-18: "after
         * a reboot, the application needs to automatically recognize the
         * previously saved network - currently, I have to manually
         * reselect my network"): the v8.7.13 launch wipe is GONE. The
         * saved profiles load from Profiles.plist, the most recently used
         * one is pinned as the active network again, and ONE bounded
         * auto-rejoin is armed for it - it fires when a survey actually
         * sees the network, runs through the same manual join path with
         * its monitor, and stands down the moment the user picks or
         * forgets a network instead. The stale selection pin from older
         * builds is still removed from NSUserDefaults. */
        self.knownSsids = [self.profiles savedSsids] ?: @[];
        self.savedSsid = [self.profiles mostRecentlyUsedSsid];
        self.autoRejoinArmedSsid = self.savedSsid;
        self.autoRejoinScansWaited = 0;
        NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
        [defaults removeObjectForKey:kDefaultsActiveSsid];
        self.stateLock = [[NSLock alloc] init];
        if (self.savedSsid.length) {
            RTWLog(@"startup: saved network \"%@\" loaded - auto-rejoin armed, it fires when a scan sees the network",
                   self.savedSsid);
        } else {
            RTWLog(@"startup: no saved network - pick yours in Networks and enter the password; it reconnects by itself after the next restart");
        }

        [self buildStatusItem];

        /* Sleep/wake triggers exactly one rescan. */
        [NSNotificationCenter.defaultCenter addObserverForName:NSWorkspaceDidWakeNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(__unused NSNotification *n) {
            RTWLog(@"system woke - rescan");
            dispatch_async(self.workQueue, ^{ [self performScan]; });
        }];
    } @catch (NSException *exception) {
        RTWLog(@"startup exception contained: %@ (%@)", exception.name, exception.reason);
    }

    [self startPollTimer];
    /* mejoras: cable watch every 3 s (main thread). */
    self.cableTimer = [NSTimer scheduledTimerWithTimeInterval:3.0 target:self
        selector:@selector(cableWatchTick:) userInfo:nil repeats:YES];
    /* One site survey at launch so the first menu open has content. */
    dispatch_async(self.workQueue, ^{ [self performScan]; });
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app {
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note {
    (void)note;
    [self stopPollTimer];
    [self.driver closeConnection];
    if (self.activityToken) {
        [[NSProcessInfo processInfo] endActivity:self.activityToken];
        self.activityToken = nil;
    }
}

#pragma mark Status item + menu

- (void)buildStatusItem {
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    self.menu = [[NSMenu alloc] initWithTitle:@"RTLWiFiBar"];
    self.menu.autoenablesItems = NO; /* explicit enabled state, no surprises */
    self.menu.delegate = self;
    self.statusItem.menu = self.menu;
    [self updateIcon:nil];
}

- (void)startPollTimer {
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.pollQueue);
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW,
                              (uint64_t)NSEC_PER_SEC,           /* 1 Hz poll */
                              (uint64_t)(200 * NSEC_PER_MSEC)); /* leeway */
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        [weakSelf pollTick];
    });
    self.pollTimer = timer;
    dispatch_resume(timer);
}

- (void)stopPollTimer {
    if (self.pollTimer) {
        dispatch_source_cancel(self.pollTimer);
        self.pollTimer = nil;
    }
}

/* 1 Hz driver poll (background queue). Reconnects after a dongle swap,
 * confirms join outcomes and keeps the survey fresh while unlinked. */
- (void)pollTick {
    /* Containment: one bad tick is logged and dropped, never fatal. */
    @try {
        [self pollTickBody];
    } @catch (NSException *exception) {
        RTWLog(@"poll tick exception contained: %@ (%@)", exception.name, exception.reason);
    }
}

- (void)pollTickBody {
    if (![self.driver isConnected]) {
        /* 8.8.5: no user client = the dongle is gone (unplugged, kext rescue,
         * client reopen). Restart the settle clock HERE: the early return
         * below skips the settle-clock code further down, so an unplug was
         * never noticed - on re-plug the 5 s settle rescan and the "dongle is
         * back" survey never fired and the rejoin waited 20 s for the next
         * survey (field log 2026-11-22 13:52: re-plugged 13:52:51, linked
         * only 13:53:15). */
        self.driverReachableSince = 0;
        self.autoRejoinSettleLogged = NO;
        NSString *detail = nil;
        BOOL ok = [self.driver openConnection:&detail];
        if (!ok) {
            [self.stateLock lock];
            RTWDriverStatus *st = [[RTWDriverStatus alloc] init];
            st.driverReachable = NO;
            st.linked = NO;
            st.lastError = detail ?: @"";
            self.lastStatus = st;
            self.lastNetworks = @[];
            [self.seenNetworks removeAllObjects];
            [self.stateLock unlock];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self updateIcon:st];
            });
            return;
        }
    }

    RTWDriverStatus *status = [self.driver pollStatus];
    if (self.driver.lastPollFailed) {
        self.consecutivePollFailures++;
        /* A dead client (dongle swap, kext reload) fails every OID; a live
         * one answers again within a poll or two. After 3 consecutive
         * failures tear the client down so the next tick reopens from
         * scratch - field log 2026-11-16 21:00: the reopen recovered the
         * link within one backoff window. */
        if (self.consecutivePollFailures >= 3) {
            RTWLog(@"driver unresponsive for %ld polls - reopening user client",
                   (long)self.consecutivePollFailures);
            [self.driver closeConnection];
            self.consecutivePollFailures = 0;
            [self clearJoinFlightWatch];
            self.awaitingDriverReturn = YES; /* v8.7.18: watch for the cured driver's return */
            return;
        }
    } else {
        self.consecutivePollFailures = 0;
    }

    [self.stateLock lock];
    self.lastStatus = status;
    NSTimeInterval lastScanFinished = self.lastScanFinished;
    BOOL scanInFlight = self.scanning;
    [self.stateLock unlock];

    /* v8.7.14: the join monitor owns every tick while a user join is in
     * flight - link-up confirmation, live progress from the kext's
     * net80211 state, the two bounded re-submits and the honest failure
     * report. (v8.7.13 shipped this method but NOTHING ever called it:
     * dead code, which is why the 07:04 join sat at "accepted" forever
     * without a single follow-up line.) The old inline link-up block is
     * folded into the monitor - one owner, one log voice. */
    [self monitorJoinFlight:status];

    /* v8.7.18: the driver came back after a lost-client episode (kext boot
     * rescue re-plug, dongle swap, kext reload). If a saved profile exists
     * and nothing is linked or in flight, re-arm ONE bounded auto-rejoin so
     * the cured dongle actually reconnects instead of sitting red. */
    if (self.awaitingDriverReturn && status && status.driverReachable) {
        self.awaitingDriverReturn = NO;
        if (!status.linked && !status.securing && !self.userDisconnected &&
            !self.joinInFlightSsid.length && self.rescueRejoinsUsed < 2) {
            NSString *rejoin = [self.profiles mostRecentlyUsedSsid];
            if (rejoin.length && !self.autoRejoinArmedSsid.length) {
                ++self.rescueRejoinsUsed;
                self.autoRejoinArmedSsid = rejoin;
                self.autoRejoinScansWaited = 0;
                self.autoRejoinSettleLogged = NO; /* 8.8.5: fresh arm, fresh settle rescan */
                RTWLog(@"driver returned after a rescue - auto-rejoin re-armed for \"%@\" (%ld/2 this session)",
                       rejoin, (long)self.rescueRejoinsUsed);
            }
        }
    }

    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];

    /* v8.7.19: settle clock for the auto-rejoin gate. */
    if (status && status.driverReachable) {
        if (self.driverReachableSince <= 0) {
            self.driverReachableSince = now;
            /* 8.8.5: the dongle just came back (re-plug, kext rescue, client
             * reopen) and a rejoin is armed - survey NOW instead of waiting
             * for the 20 s freshness cadence. Field log 2026-11-22 13:38
             * (RTL8821AU): back at 13:38:10, first survey only at 13:38:19.
             * Only when the cadence below would skip it (a survey finished
             * less than 20 s ago), so this never doubles a scan. That survey
             * starts the 5 s settle wait, which then rescans and joins. */
            if (self.autoRejoinArmedSsid.length && !status.linked && !status.securing &&
                !scanInFlight && !self.joinInFlightSsid.length &&
                lastScanFinished > 0 && now - lastScanFinished < 20.0) {
                RTWLog(@"dongle is back - surveying now for \"%@\" (no 20 s wait)",
                       self.autoRejoinArmedSsid);
                dispatch_async(self.workQueue, ^{ [self performScan]; });
            }
        }
    } else {
        self.driverReachableSince = 0;
        /* 8.8.5: the settle clock restarts when the dongle comes back, so its
         * 5 s settle rescan must be scheduled again too (an unplug during the
         * settle wait left this YES and the rejoin waited 20 s for a survey). */
        self.autoRejoinSettleLogged = NO;
    }

    /* Link transition tracking (v8.7.2). A link that drops seconds after
     * connecting is a flap (field log 2026-11-17 07:36: connect, drop,
     * reconnect every 3-16 s). The app never reconnects on its own, so
     * this is logging only: the reason note names WHO ended the link and
     * the user decides whether to pick the network again. The one
     * exception is the v8.7.17 startup auto-rejoin, and it never re-arms
     * here - a mid-session drop still waits for the user. */
    if (status && status.driverReachable) {
        if (status.linked && !self.linkWasUp) {
            self.linkWasUp = YES;
            self.lastLinkUpAt = now;
            self.linkAddressChecked = NO;   /* 8.8.5: fresh address check */
            self.linkHasNoAddress = NO;
            self.linkAddressLastCheck = 0;
            /* v8.7.3: remember the deauth-reason registry value while this
             * link lived. On drop, a DIFFERENT value means the AP ended
             * the association; an unchanged one means a local reset. */
            self.deauthReasonAtLinkUp = [self.driver lastDeauthReason];
        } else if (!status.linked && self.linkWasUp && self.lastLinkUpAt > 0) {
            self.linkWasUp = NO;
            NSTimeInterval upFor = now - self.lastLinkUpAt;
            self.lastLinkUpAt = 0;
            self.linkHasNoAddress = NO;
            NSString *why = [self linkDropReasonNote];
            if (upFor < 30.0) {
                RTWLog(@"link flapped: up only %.0f s %@", upFor + 0.5, why);
                RTWLog(@"driver diagnostics at flap: %@", [self.driver diagnosticSummary]);
                /* 8.8.5: a flap is rejoined by itself too, inside the same
                 * 6-per-hour budget as a drop, so a flapping AP can never turn
                 * this into a join loop. Field log 2026-11-22 13:07 (Cudy,
                 * RTL8822BU, 5 GHz): the AP sent deauth reason 7 after 16 s
                 * and the app only scanned for 15 minutes. A wrong password
                 * never gets here: the link only counts as up once the WPA
                 * keys are installed. */
                [self armAutoRejoinAfterDrop:now];
            } else {
                RTWLog(@"link dropped after %.0f s %@", upFor + 0.5, why);
                RTWLog(@"driver diagnostics at drop: %@", [self.driver diagnosticSummary]);
                /* v8.8.4: a link that lived 30 s or more and ended (the AP
                 * deauthenticated us, or a local reset) is rejoined by
                 * itself. Field log 2026-11-22 (Archer T4U on 5 GHz): the AP
                 * dropped the station twice (reason 4, reason 7) and the app
                 * only scanned for 10+ minutes until "Rejoin" was clicked. */
                [self armAutoRejoinAfterDrop:now];
            }
        }
    }

    /* 8.8.5: associated but the WPA keys are not installed - say so once per
     * episode (the kext's own WPA recovery is working on it), then run the
     * post-connect address check while the link is up. */
    if (status && status.driverReachable && status.securing) {
        if (!self.securingLogged) {
            self.securingLogged = YES;
            RTWLog(@"associated - waiting for the WPA handshake before the link counts as up (%@)",
                   [self.driver diagnosticValuesForKeys:@[@"RTWDiagWPASupplicantState",
                       @"RTWDiagWPAEAPOLReceived", @"RTWDiagWPAEAPOLBadMIC",
                       @"RTWDiagWPARecoveryState", @"RTWDiagWPARecoveryRemainingMS"]]);
        }
    } else {
        self.securingLogged = NO;
    }
    [self checkLinkAddress:status now:now];

    /* Survey freshness while unlinked: the boot-time survey often sees
     * almost nothing while the radio warms up (field log 2026-11-16
     * 20:56:53 "scan done: 1 networks"), and the user can only click
     * networks the menu actually lists. One bounded rescan on a 20 s
     * cadence while unlinked keeps the list real; it stops the moment
     * the link is up. This is a menu refresh - it NEVER joins. (v8.7.17:
     * the completed survey is also where the armed startup auto-rejoin
     * consumes its match - inside performScan, on the records.) No log
     * line here on purpose: performScan already logs every trigger,
     * failure and refusal, and a 1 Hz gate line would only flood the
     * log the old "auto-join standing by" machinery did.
     * v8.7.14: there is NO scan of any kind while a user join is in
     * flight - every scan trigger restarts the kext's STA sweep and
     * erases auth/assoc progress, which is exactly what kept the
     * 07:04:16 join dead (scans landed 4 ms and 0.8 s after the
     * submit). scanInFlight additionally refuses to stack a second
     * scan behind one still queued or running: in the same capture two
     * ticks passed this gate 6 ms apart, because lastScanFinished only
     * stamps when a scan FINISHES. */
    /* 8.8.5: never while securing either - a scan restarts the STA sweep and
     * would kill the WPA handshake the kext is finishing. */
    if (status && status.driverReachable && !status.linked && !status.securing &&
        !self.userDisconnected && /* 8.8.5: a scan would bring the radio back up */
        !scanInFlight && !self.joinInFlightSsid.length &&
        (lastScanFinished <= 0 || now - lastScanFinished >= 20.0)) {
        /* v8.7.6: a never-finished scan counts as due - after the
         * boot-attach death no survey ever completed and the old gate
         * sat on "waiting for the first survey" forever (log
         * 2026-11-17 18:55: two user rescans refused, nothing else
         * ever retried). */
        dispatch_async(self.workQueue, ^{ [self performScan]; });
    }

    dispatch_async(dispatch_get_main_queue(), ^{ [self updateIcon:status]; });
}

- (void)updateIcon:(nullable RTWDriverStatus *)status {
    @try {
        [self updateIconBody:status];
    } @catch (NSException *exception) {
        RTWLog(@"icon exception contained: %@ (%@)", exception.name, exception.reason);
    }
}

- (void)updateIconBody:(nullable RTWDriverStatus *)status {
    if (!self.statusItem) {
        /* Login-time retry: if the first buildStatusItem threw, try again
         * on every poll until the status item exists. */
        [self buildStatusItem];
        if (!self.statusItem) return;
    }
    RTWIconState state = RTWIconStateMissing;
    int32_t rssi = 0;
    if (status.driverReachable) {
        state = status.linked ? RTWIconStateLinked : RTWIconStateIdle;
        rssi = status.rssiPercent;
    }
    /* 8.8.5: Disconnect USB WiFi shows the crossed-out Wi-Fi glyph, like the
     * stock Realtek StatusBarApp. */
    if (self.userDisconnected) state = RTWIconStateMissing;
    self.statusItem.button.image = [RTWSignalIcon iconForState:state rssiPercent:rssi];
    self.statusItem.button.toolTip = [self statusTooltip:status];
}

- (NSString *)statusTooltip:(nullable RTWDriverStatus *)status {
    if (!status || !status.driverReachable) {
        return [NSString stringWithFormat:@"RTLWiFiBar - %@", status.lastError ?: NSLocalizedString(@"driver unreachable", nil)];
    }
    if (self.userDisconnected) return [NSString stringWithFormat:@"RTLWiFiBar - %@", NSLocalizedString(@"USB WiFi disconnected", nil)];
    if (status.linked) {
        return [NSString stringWithFormat:NSLocalizedString(@"%@ - channel %u, %d dBm%@", nil),
                status.ssid ?: NSLocalizedString(@"connected", nil),
                (unsigned)status.channel, status.rssiPercent / 2 - 100,
                status.driverVersion ? [NSString stringWithFormat:NSLocalizedString(@" (driver %@)", nil), status.driverVersion] : @""];
    }
    if (self.joinInFlightSsid.length || (status.securing && self.savedSsid.length)) {
        return [NSString stringWithFormat:NSLocalizedString(@"Joining \"%@\" - not connected yet%@", nil),
                self.joinInFlightSsid.length ? self.joinInFlightSsid : self.savedSsid,
                status.driverVersion ? [NSString stringWithFormat:NSLocalizedString(@" (driver %@)", nil), status.driverVersion] : @""];
    }
    return [NSString stringWithFormat:NSLocalizedString(@"Dongle ready - not connected%@", nil),
            status.driverVersion ? [NSString stringWithFormat:NSLocalizedString(@" (driver %@)", nil), status.driverVersion] : @""];
}

#pragma mark Scan workflow

- (void)performScan {
    /* v8.7.14: a join owns the radio. Every scan trigger while a join is
     * working makes the kext restart its STA sweep (ieee80211_begin_scan
     * in the RTW_CONTROL_SCAN handler), erasing auth/assoc progress - the
     * field capture 2026-11-18 07:04 shows the freshness gate scanning
     * 4 ms and 0.8 s after the submit and the join never converging.
     * The kext guards its scan handler too (driver 1.0.8); this is the
     * app side of the same rule. */
    if (self.joinInFlightSsid.length) {
        RTWLog(@"scan skipped - the join to %@ is still working; the list refreshes when it ends",
               self.joinInFlightSsid);
        return;
    }
    if ([self.stateLock tryLock]) {
        if (self.scanning) {
            [self.stateLock unlock];
            return;
        }
        self.scanning = YES;
        [self.stateLock unlock];
    } else {
        return;
    }
    @try {
        NSString *detail = nil;
        if (![self.driver isConnected]) {
            if (![self.driver openConnection:&detail]) {
                RTWLog(@"scan aborted: %@", detail);
                return;
            }
        }
        if (![self.driver triggerScan:&detail]) {
            self.scanTriggerFailures++;
            RTWLog(@"scan trigger failed (%ld in a row): %@",
                   (long)self.scanTriggerFailures, detail);
            /* v8.7.15: name the wedge. 0xe00002d8 on every scan trigger
             * while MAC/version queries still answer = the chip is latched
             * dead at the USB transport (field log 2026-11-18 07:42-07:48:
             * six minutes of identical failures, client restarts useless,
             * cured only by a physical replug). Kext 1.0.9 carries the
             * session rescue: after 20 s of a persistently dead transport
             * the driver re-enumerates the dongle by itself (max 2 per
             * attach) - watch for it to come back, no replug needed. */
            if (!self.wedgedNoticeShown && self.scanTriggerFailures >= 3 &&
                [detail containsString:@"0xe00002d8"]) {
                self.wedgedNoticeShown = YES;
                RTWLog(@"driver diagnostics at wedge: %@", [self.driver diagnosticSummary]);
                RTWLog(@"the USB adapter chip is latched dead at the transport (0xe00002d8 on every scan) - "
                       "kext 1.0.9 re-enumerates it by itself within ~20-40 s (session rescue); "
                       "wait for the dongle to come back, then rejoin");
            }
            /* v8.7.6: 0xe00002d8 is kIOReturnNotAttached - the driver
             * instance this client was opened on has been terminated.
             * Boot-time USB re-enumeration kills the first attach while the
             * client lives on (log 2026-11-17 18:55: every scan of the
             * session refused, polls still answered, so the dead-client
             * heal after 3 poll failures never fired). Closing the client
             * makes the 1 Hz poll open a FRESH service - the closest a
             * user-space app gets to the re-plug that always fixed this.
             * Spacing: 30 s for the first 5 attempts, then 120 s. */
            NSTimeInterval nowTs = [NSDate timeIntervalSinceReferenceDate];
            if (self.scanTriggerFailures >= 2 &&
                nowTs - self.lastScanClientRestart >=
                    (self.scanHealCount >= 5 ? 120.0 : 30.0)) {
                self.scanHealCount++;
                self.lastScanClientRestart = nowTs;
                self.scanTriggerFailures = 0;
                RTWLog(@"scan refused by a dead driver instance - reopening the user client (attempt %ld)",
                       (long)self.scanHealCount);
                [self.driver closeConnection];
                /* v8.8.3: the dongle may be unplugged and plugged back after
                 * this restart (field 2026-11-22 T4U: replug, the driver
                 * returned, but nothing re-armed the saved network because
                 * only the poll-failure path set this flag). */
                self.awaitingDriverReturn = YES;
                if (self.scanHealCount == 5) {
                    RTWLog(@"scans still refused after 5 client restarts - if the link never comes up, "
                           "unplug the dongle and plug it back once (a boot-time USB port reset killed the first attach)");
                }
            }
            return;
        }
        self.scanTriggerFailures = 0;
        self.wedgedNoticeShown = NO;
        RTWLog(@"scan triggered");
        /* The kext releases busy on the first node or after 5 s; poll a bit
         * longer so a silent radio still ends with the deadline answer. */
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:7.0];
        while (1) {
            BOOL busy = [self.driver scanBusy:&detail];
            if (!busy) break;
            if ([deadline timeIntervalSinceNow] < 0.0) {
                RTWLog(@"scan poll deadline reached");
                break;
            }
            [NSThread sleepForTimeInterval:0.35];
        }
        NSArray<RTWNetworkEntry *> *records = [self.driver surveyRecords:&detail];
        if (detail) RTWLog(@"survey fetch issue: %@", detail);
        RTWLog(@"scan done: %lu networks", (unsigned long)records.count);
        /* mejoras: one line with what the survey holds whenever it
         * changes (SSID@channel/dBm), to tell a driver miss from a menu miss. */
        {
            NSMutableArray<NSString *> *seen = [NSMutableArray array];
            for (RTWNetworkEntry *e in records)
                [seen addObject:[NSString stringWithFormat:@"%@@%u/%d",
                                 e.ssid.length ? e.ssid : @"(hidden)", e.channel, e.rssiDbm]];
            NSString *line = [seen componentsJoinedByString:@", "];
            static NSString *lastSurveyLine = nil;
            if (![line isEqualToString:lastSurveyLine ?: @""]) {
                lastSurveyLine = [line copy];
                RTWLog(@"survey: %@", line);
            }
        }

        [self.stateLock lock];
        /* 2026-11-23: once the link is up the kext answers every scan from its
         * cache, and a join leaves only the networks of its own channel in it
         * (field log 05:10-05:13: 39 networks at start, 7-8 after a join, the
         * other saved network gone until Forget or a replug). Keep the
         * networks seen in the last 10 minutes in the menu list. Only the
         * menu list is merged: the auto-rejoin below still gets the raw survey. */
        if (!self.seenNetworks) self.seenNetworks = [NSMutableDictionary dictionary];
        NSTimeInterval seenNow = [NSDate timeIntervalSinceReferenceDate];
        NSMutableArray<RTWNetworkEntry *> *merged = [NSMutableArray arrayWithArray:records ?: @[]];
        /* mejoras: remember every AP (SSID + BSSID), not one entry
         * per SSID - otherwise the 5 GHz AP of the joined network, seen only
         * by the full sweep, is overwritten by the 2.4 GHz AP of the cache
         * after the rejoin and can never be picked. */
        NSMutableSet<NSString *> *presentKeys = [NSMutableSet set];
        for (RTWNetworkEntry *e in records) {
            if (!e.ssid.length) continue;
            uint8_t b[6] = {0};
            [e getBSSID:b];
            NSString *key = [NSString stringWithFormat:@"%@|%02x%02x%02x%02x%02x%02x",
                             e.ssid, b[0], b[1], b[2], b[3], b[4], b[5]];
            [presentKeys addObject:key];
            self.seenNetworks[key] = @{@"e": e, @"t": @(seenNow)};
        }
        for (NSString *key in [self.seenNetworks allKeys]) {
            NSDictionary *item = self.seenNetworks[key];
            if (seenNow - [item[@"t"] doubleValue] > 600.0) {
                [self.seenNetworks removeObjectForKey:key];
            } else if (![presentKeys containsObject:key]) {
                [merged addObject:item[@"e"]];
            }
        }
        self.lastNetworks = merged;
        self.driverSurveyKeys = presentKeys;
        self.driverSurveyEntries = records ?: @[];
        self.scanning = NO;
        self.lastScanFinished = [NSDate timeIntervalSinceReferenceDate];
        [self.stateLock unlock];

        /* v8.7.17: the armed startup auto-rejoin consumes the first
         * survey that actually sees the saved network - right here,
         * where the records landed and no join is in flight. */
        [self maybeFireStartupAutoRejoinWithRecords:records];
    } @catch (NSException *exception) {
        RTWLog(@"scan exception contained: %@ (%@)", exception.name, exception.reason);
    } @finally {
        [self.stateLock lock];
        self.scanning = NO;
        [self.stateLock unlock];
    }
}

#pragma mark Join outcome monitor

/* v8.7.13: the auto-join engine is GONE (the user's decision - "we have
 * decide to not included Join auto"). What remains here is the honest
 * bookkeeping for joins the USER started:
 *  - noteJoinSubmittedForEntry arms a 45 s association window and the
 *    in-memory retry key for that one user join.
 *  - monitorJoinFlight confirms the link, feeds the live "Joining..."
 *    status, re-submits the SAME user stream at most twice, and then
 *    reports the failure and stops. Nothing is ever submitted on a
 *    timer or after a wake - a dead join stays dead until the user
 *    picks the network again. (The single exception is the v8.7.17
 *    startup auto-rejoin: ONE bounded join of the saved network per
 *    launch, built on this exact machinery - see the section below.)
 *    The kext restarts its station
 *    cleanly on every join request (driver 1.0.7), so no settle timer
 *    is needed here; the 8.7.12 wait-for-the-driver timer is reverted.
 * The retry key lives in memory for this session only - nothing is
 * written to disk until the user confirms a password, and only a
 * user-initiated join arms that save at all (v8.7.7 rule). */
- (void)noteJoinSubmittedForEntry:(RTWNetworkEntry *)entry
                       passphrase:(nullable NSString *)pass {
    self.joinStartedAt = [NSDate timeIntervalSinceReferenceDate];
    self.joinInFlightUntil = self.joinStartedAt + 45.0;
    self.joinInFlightSsid = entry.ssid;
    self.joinWatchEntry = entry;
    self.joinWatchKey = pass;
    /* v8.7.14: fresh progress window for this join - previous attempts
     * must not leak their state or deauth into this one's report. */
    self.joinLastSeenState = 0xFF;
    self.joinDeauthReasonSeen = 0;
    self.lastJoinFailureNote = nil;
}

/* Drops the join watch - and with it the in-memory retry key. */
- (void)clearJoinFlightWatch {
    self.joinInFlightSsid = nil;
    self.joinWatchEntry = nil;
    self.joinWatchKey = nil;
    self.joinWatchSavedPass = NO;
    self.joinInFlightUntil = 0;
    self.joinStartedAt = 0;
}

/* Universal join monitor. Runs once per poll tick while a user join is
 * watched; returns YES when it owned the tick. Duties:
 *  1. Link-up confirmation - clears the watch, stamps the pending save.
 *  2. Live feedback - tooltip + menu line read Joining "<ssid>".
 *  3. Bounded retry - after the 45 s association window, re-submit the
 *     SAME user stream (same entry, same key) at most twice; then log
 *     the failure honestly and stop. The user decides what happens next.
 * A manual join without a remembered profile retries from the in-memory
 * key until the link comes up, the budget is spent, the app quits, or
 * another join replaces the watch. */
- (BOOL)monitorJoinFlight:(nullable RTWDriverStatus *)status {
    if (!self.joinInFlightSsid.length || !self.joinWatchEntry) return NO;
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];

    if (status && status.driverReachable && status.linked) {
        if (!status.ssid.length || rtlPrintableSsidEqual(status.ssid, self.joinInFlightSsid)) {
            RTWLog(@"join monitor: link is up on %@ %ld s after submit%@",
                   self.joinInFlightSsid, (long)((now - self.joinStartedAt) + 0.5),
                   self.joinLastSeenState != 0xFF
                       ? [NSString stringWithFormat:@" (driver state %u: %@)",
                           (unsigned)self.joinLastSeenState,
                           [self nameForJoinState:self.joinLastSeenState]]
                       : @"");
            [self commitPendingNetworkSave]; /* v8.7.7: link exists - save now */
            [self clearJoinFlightWatch];
            self.joinAttemptFailures = 0;
        } else {
            RTWLog(@"join monitor: linked to %@ instead - dropping the watch for %@",
                   status.ssid, self.joinInFlightSsid);
            [self clearPendingNetworkSave];
            [self clearJoinFlightWatch];
        }
        return YES;
    }
    if (!status || !status.driverReachable) return YES; /* reopen path owns recovery */

    /* v8.7.14: live progress from the kext (net80211 state + beacon age,
     * driver 1.0.8 join-state OID). Every transition is logged, so the
     * app log shows exactly how far the association got: scanning ->
     * authenticating -> associating -> linked. A driver without the OID
     * keeps the time-window behaviour; the silent query never records
     * an error, so an older kext cannot flood this log. */
    uint32_t joinState = 0, beaconAge = 0;
    if ([self.driver queryJoinState:&joinState beaconAgeMs:&beaconAge]) {
        if (self.joinLastSeenState == 0xFF || joinState != self.joinLastSeenState) {
            self.joinLastSeenState = joinState;
            RTWLog(@"join progress for %@: %@ (state %u)%@",
                   self.joinInFlightSsid, [self nameForJoinState:joinState],
                   (unsigned)joinState,
                   beaconAge ? [NSString stringWithFormat:@" - beacons %u ms old",
                                (unsigned)beaconAge] : @"");
        }
    }
    /* A deauth that arrives during THIS join is kept for the failure
     * report (wrong password is the classic case: the AP kills the
     * 4-way handshake with reason 15/16). */
    uint32_t reason = [self.driver lastDeauthReason];
    if (reason) self.joinDeauthReasonSeen = reason;

    /* Inside the 45 s association window: hands off, keep waiting. */
    if (now < self.joinInFlightUntil) return YES;

    self.joinAttemptFailures++;
    if (self.joinAttemptFailures > 2) {
        /* Budget spent: the driver accepted the join but the association
         * never completed. Report exactly WHERE it died and stop - no
         * silent loop, no automatic anything. */
        NSString *why = [self joinFailureDiagnosis];
        RTWLog(@"join to %@ DID NOT COMPLETE %ld s after submit: %@",
               self.joinInFlightSsid, (long)((now - self.joinStartedAt) + 0.5), why);
        RTWLog(@"driver diagnostics at join failure: %@", [self.driver diagnosticSummary]);
        self.lastJoinFailureNote = [NSString stringWithFormat:
            @"Join to \"%@\" failed - %@", self.joinInFlightSsid, why];
        /* 2026-11-22: a saved-password join that died in the WPA handshake
         * (driver state 3 or 4) most likely has an outdated password. */
        BOOL offerRetype = self.joinWatchSavedPass &&
            (self.joinLastSeenState == 3 || self.joinLastSeenState == 4);
        RTWNetworkEntry *retypeEntry = self.joinWatchEntry;
        [self clearPendingNetworkSave];
        [self clearJoinFlightWatch];
        if (offerRetype && retypeEntry) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self offerNewPasswordForEntry:retypeEntry];
            });
        }
        return YES;
    }

    RTWLog(@"join to %@ still not up %ld s after submit - re-submitting the same stream (attempt %ld)",
           self.joinInFlightSsid, (long)((now - self.joinStartedAt) + 0.5),
           (long)(self.joinAttemptFailures + 1));
    RTWLog(@"driver diagnostics at join stall: %@", [self.driver diagnosticSummary]);
    NSString *pass = self.joinWatchKey; /* session memory only */
    /* mejoras: the pinned AP did not come up - retry by SSID so a
     * vanished or unreachable AP can never keep us off the network. */
    if (self.joinWatchEntry.pinBssid) {
        self.joinWatchEntry.pinBssid = NO;
        RTWLog(@"re-submit drops the AP pin - joining %@ by network name", self.joinWatchEntry.ssid);
    }
    [self noteJoinSubmittedForEntry:self.joinWatchEntry passphrase:pass];
    NSString *detail = nil;
    if ([self.driver joinNetwork:self.joinWatchEntry passphrase:pass error:&detail]) {
        RTWLog(@"join re-submit for %@ -> ok", self.joinWatchEntry.ssid);
    } else {
        RTWLog(@"join re-submit for %@ -> failed (%@)", self.joinWatchEntry.ssid,
               detail ?: @"?");
    }
    return YES;
}

#pragma mark Startup auto-rejoin (v8.7.17)

/* The one bounded piece of automatic joining this app ever does, and only
 * because the user asked for it back (2026-11-18: "after a reboot, the
 * application needs to automatically recognize the previously saved
 * network"). Rules:
 *  - armed exactly once, at launch, only when a saved profile exists;
 *  - fires only when a survey actually sees the SSID (never a blind
 *    submit against a dead target), the driver is reachable, the link
 *    is down and no other join is in flight;
 *  - the join itself is the manual path: submit + monitor + 45 s window
 *    + at most two re-submits + honest failure - no new machinery;
 *  - stands down the moment the user picks or forgets a network;
 *  - gives up after five surveys never saw it (~100 s of the 20 s
 *    freshness cadence) and says so out loud;
 *  - v8.8.4: a mid-session drop of a link that lived 30 s or more re-arms
 *    it (armAutoRejoinAfterDrop:, max 6 per rolling hour); 8.8.5: flaps too. */
- (void)maybeFireStartupAutoRejoinWithRecords:(NSArray<RTWNetworkEntry *> *)records {
    if (self.userDisconnected) return; /* 8.8.5: never rejoin after Disconnect USB WiFi */
    if (self.cablePaused) return;      /* mejoras: Wi-Fi paused, cable connected */
    NSString *ssid = self.autoRejoinArmedSsid;
    if (!ssid.length) return;
    if (![self.profiles hasProfileForSsid:ssid]) {
        /* Forgot (or the store vanished) between launch and now. */
        [self standDownStartupAutoRejoin:@"the saved profile is gone"];
        return;
    }
    [self.stateLock lock];
    RTWDriverStatus *status = self.lastStatus;
    [self.stateLock unlock];
    if (status && status.linked) {
        /* Something linked already - a manual join won the race. */
        [self standDownStartupAutoRejoin:@"a link is already up"];
        return;
    }
    if (self.joinInFlightSsid.length) {
        /* The user's own join is working - never fight it for the radio. */
        [self standDownStartupAutoRejoin:@"your manual join is already in flight"];
        return;
    }
    if (!status || !status.driverReachable) return; /* dongle not up yet - keep waiting */
    /* 8.8.5: the kext is finishing a WPA handshake on its own - keep the arm
     * and let it finish; a join submit now would restart the association. */
    if (status.securing) return;
    /* v8.7.19: let the radio settle before the first join of this driver
     * instance. Deferring does NOT count toward the five-survey give-up. */
    {
        NSTimeInterval since = self.driverReachableSince;
        NSTimeInterval age = since > 0 ? [NSDate timeIntervalSinceReferenceDate] - since : 0;
        if (since <= 0 || age < kAutoRejoinSettleSeconds) {
            if (!self.autoRejoinSettleLogged) {
                self.autoRejoinSettleLogged = YES;
                RTWLog(@"startup auto-rejoin: waiting %.0f s for the dongle to settle before joining \"%@\"",
                       kAutoRejoinSettleSeconds, ssid);
                /* 8.7.21: do not wait for the 20 s survey tick - survey again
                 * as soon as the settle time is over so the join fires then. */
                __weak typeof(self) weakSelf = self;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                (int64_t)((kAutoRejoinSettleSeconds + 0.5) * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{
                    typeof(self) strongSelf = weakSelf;
                    if (strongSelf && strongSelf.autoRejoinArmedSsid.length) [strongSelf autoRescan];
                });
            }
            return;
        }
    }
    self.autoRejoinSettleLogged = NO;
    RTWNetworkEntry *found = nil;
    for (RTWNetworkEntry *entry in records) {
        if (entry.joinable && rtlPrintableSsidEqual(entry.ssid, ssid)) {
            found = entry;
            break;
        }
    }
    if (!found) {
        self.autoRejoinScansWaited++;
        if (self.autoRejoinScansWaited >= 5) {
            NSString *why = [NSString stringWithFormat:
                @"the network never appeared in %ld scans after the restart",
                (long)self.autoRejoinScansWaited];
            RTWLog(@"startup auto-rejoin gave up on %@ - %@; pick it in Networks to connect",
                   ssid, why);
            self.lastJoinFailureNote = [NSString stringWithFormat:
                @"Auto-rejoin to \"%@\" stopped - %@ - pick it in Networks",
                ssid, why];
            self.autoRejoinArmedSsid = nil;
            self.autoRejoinScansWaited = 0;
        }
        return; /* not seen yet - the 20 s freshness rescan keeps looking */
    }
    /* Found: consume the arm BEFORE submitting so this fires once. */
    self.autoRejoinArmedSsid = nil;
    self.autoRejoinScansWaited = 0;
    NSString *pass = [self.profiles passphraseForSsid:ssid];
    RTWLog(@"startup auto-rejoin: \"%@\" is in the survey - joining with the saved password%@",
           ssid, pass.length ? @"" : @" (open network, no key)");
    /* The same selection side effects a menu click has: pinned on top,
     * saved row with the lock, and the link-up commit re-stamps the
     * profile with the live auth/channel from this survey. */
    [self markActiveSsid:ssid];
    [self markKnownSsid:ssid];
    [self armPendingNetworkSaveForEntry:found passphrase:pass];
    [self joinSavedEntry:found passphrase:pass automatic:YES]; /* mejoras: preferred AP */
}

/* The auto-rejoin's own submit: identical to submitJoin minus the modal
 * failure alert - at login time nobody may be there to click it, and a
 * failed submit is already logged and shown on the menu line. */
- (void)submitAutoRejoin:(RTWNetworkEntry *)entry passphrase:(nullable NSString *)pass {
    /* Arm the join watch BEFORE the driver call (same rule as
     * submitJoin) so the monitor owns the outcome: link-up confirmation,
     * live progress, the two bounded re-submits, the honest failure. */
    [self noteJoinSubmittedForEntry:entry passphrase:pass];
    NSString *detail = nil;
    BOOL ok = [self.driver joinNetwork:entry passphrase:pass error:&detail];
    RTWLog(@"auto-rejoin submit for %@ -> %@%@", entry.ssid, ok ? @"ok" : @"failed",
           detail ? [NSString stringWithFormat:@" (%@)", detail] : @"");
    if (ok) {
        self.joinAttemptFailures = 0;
    } else {
        /* A rejected submit is a REAL failure - log it, show it, stop.
         * No ladder of our own: the monitor only handles an accepted but
         * unconverged join, and this one never started. */
        [self clearPendingNetworkSave];
        [self clearJoinFlightWatch];
        self.lastJoinFailureNote = [NSString stringWithFormat:
            @"Auto-rejoin to \"%@\" failed - %@ - pick it in Networks to connect",
            entry.ssid, detail ?: @"the driver rejected the association stream"];
    }
}

- (void)standDownStartupAutoRejoin:(NSString *)reason {
    NSString *ssid = self.autoRejoinArmedSsid;
    if (!ssid.length) return;
    self.autoRejoinArmedSsid = nil;
    self.autoRejoinScansWaited = 0;
    /* 8.8.5: an arm that stood down while still settling left this YES, and
     * every later arm of the session then skipped its 5 s settle rescan and
     * waited for the 20 s survey cadence instead (field log 2026-11-22
     * 12:12: driver back at 12:12:15, no settle line, join only at 12:12:36). */
    self.autoRejoinSettleLogged = NO;
    RTWLog(@"startup auto-rejoin stood down for %@ - %@", ssid, reason);
}

#pragma mark Join + link diagnostics

/* v8.7.14: human names for the net80211 station states the kext's
 * join-state OID reports (driver 1.0.8). */
- (NSString *)nameForJoinState:(uint32_t)state {
    switch (state) {
        case 0:  return @"idle";
        case 1:  return @"scanning for the network";
        case 2:  return @"authenticating";
        case 3:  return @"associating / WPA handshake";
        case 4:  return @"associated (RUN) - WPA keys next";
        default: return [NSString stringWithFormat:@"unknown state %u", (unsigned)state];
    }
}

/* v8.7.14: the honest end-of-join report. Names the furthest state the
 * association reached and any deauth the AP sent during the attempt -
 * the two facts that separate "the driver never found the network"
 * from "the AP refused the handshake". */
- (NSString *)joinFailureDiagnosis {
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    switch (self.joinLastSeenState) {
        case 0xFF:
            [parts addObject:@"the driver never reported any association progress"];
            break;
        case 4:
            [parts addObject:@"the station associated (RUN) but the WPA keys were never installed - the 4-way handshake did not finish"];
            break;
        case 3:
            [parts addObject:@"the AP accepted the association but the WPA 4-way handshake never completed"];
            if (self.joinDeauthReasonSeen) {
                [parts addObject:@"if the password has a typo the AP ends the handshake exactly like this - retype it carefully"];
            }
            break;
        case 2:
            [parts addObject:@"the AP never answered the authentication request"];
            break;
        case 1:
            [parts addObject:@"the driver never found or locked onto the network (still scanning - check the AP is up and the dongle is in range)"];
            break;
        default:
            [parts addObject:[NSString stringWithFormat:
                @"progress stopped in driver state %u (%@)",
                (unsigned)self.joinLastSeenState,
                [self nameForJoinState:self.joinLastSeenState]]];
            break;
    }
    if (self.joinDeauthReasonSeen) {
        [parts addObject:[NSString stringWithFormat:
            @"during the attempt the AP sent deauth reason %u (%@)",
            (unsigned)self.joinDeauthReasonSeen,
            [self decodeDeauthReason:self.joinDeauthReasonSeen]]];
    }
    [parts addObject:@"pick the network in Networks to try again"];
    return [parts componentsJoinedByString:@"; "];
}

/* v8.7.3 (declared since that build, IMPLEMENTED in v8.7.14: the
 * compiler warned about the missing definition and the call below ran
 * on every drop - an unrecognized-selector crash waiting for a drop).
 * v8.7.3 rule: the registry value is sampled at link-up; a DIFFERENT
 * value at drop time means a deauth/disassoc arrived while the link
 * lived - the AP ended it. An unchanged value means no deauth was
 * recorded - the station was reset locally (USB hiccup, quiesce,
 * sleep). */
/* v8.8.4: re-arm the same bounded auto-rejoin the startup uses (it fires when
 * a survey sees the saved network, runs the manual join path with its
 * monitor, and stands down when the user picks or forgets a network).
 * Bounded: at most 6 per rolling hour so an AP that keeps dropping us never
 * turns this into a join loop. 8.8.5: flaps (link up under 30 s) use it too. */
- (void)armAutoRejoinAfterDrop:(NSTimeInterval)now {
    if (self.userDisconnected) return; /* 8.8.5: the user disconnected on purpose */
    if (self.switchingToSsid.length) return; /* mejoras: leaving on purpose to switch */
    if (self.scanDetourSsid.length) return;  /* mejoras: left for a full sweep */
    if (self.cablePaused) return;            /* mejoras: paused while the cable is in */
    if (self.autoRejoinArmedSsid.length || self.joinInFlightSsid.length) return;
    NSString *ssid = [self.profiles mostRecentlyUsedSsid];
    if (!ssid.length || ![self.profiles hasProfileForSsid:ssid]) return;
    if (self.dropRejoinWindowStart <= 0 || now - self.dropRejoinWindowStart > 3600.0) {
        self.dropRejoinWindowStart = now;
        self.dropRejoinsUsed = 0;
    }
    if (self.dropRejoinsUsed >= 6) {
        RTWLog(@"link dropped again - auto-rejoin budget used up for this hour; use Rejoin in the menu");
        return;
    }
    ++self.dropRejoinsUsed;
    self.autoRejoinArmedSsid = ssid;
    self.autoRejoinScansWaited = 0;
    self.autoRejoinSettleLogged = NO; /* 8.8.5: fresh arm, fresh settle rescan */
    RTWLog(@"link dropped mid-session - auto-rejoin armed for \"%@\" (%ld/6 this hour)",
           ssid, (long)self.dropRejoinsUsed);
    __weak typeof(self) weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        typeof(self) strongSelf = weakSelf;
        if (strongSelf && strongSelf.autoRejoinArmedSsid.length) [strongSelf autoRescan];
    });
}

/* 8.8.5: the address check. Field log 2026-11-22 11:19-11:32 (RTL8821AU):
 * the link stayed "up" for 13 minutes and pages never loaded, and the log
 * could not say why - nothing was recorded after the connect line. 10 s
 * after the link comes up, log the interface's IPv4 (or its absence) with
 * the DHCP and data counters. Without an address, look again every 10 s
 * and log once when it arrives. Never joins by itself: the kext already
 * rebuilds a link whose DHCP stays unanswered (autoHealStuckDhcp,
 * autoHealSilentLease) and the app must not fight it. Poll queue only. */
- (void)checkLinkAddress:(nullable RTWDriverStatus *)status now:(NSTimeInterval)now {
    if (!status || !status.driverReachable || !status.linked ||
        !self.linkWasUp || self.lastLinkUpAt <= 0) return;
    NSTimeInterval upFor = now - self.lastLinkUpAt;
    BOOL firstCheck = !self.linkAddressChecked && upFor >= 10.0;
    BOOL recheck = self.linkAddressChecked && self.linkHasNoAddress &&
                   now - self.linkAddressLastCheck >= 10.0;
    if (!firstCheck && !recheck) return;
    self.linkAddressChecked = YES;
    self.linkAddressLastCheck = now;

    NSString *ifname = nil;
    NSString *ip = [self.driver interfaceIPv4:&ifname];
    if (ip.length) {
        if (firstCheck) {
            RTWLog(@"link check %.0f s after connect: %@ has IPv4 %@", upFor, ifname ?: @"?", ip);
        } else {
            RTWLog(@"link check: %@ got IPv4 %@ %.0f s after connect", ifname ?: @"?", ip, upFor);
        }
        self.linkHasNoAddress = NO;
        return;
    }
    if (firstCheck) {
        NSString *counters = [self.driver diagnosticValuesForKeys:@[
            @"RTWDiagWPAPortValid", @"RTWDiagDhcpEpisodeDiscovers", @"RTWDiagDhcpEpisodeReplies",
            @"RTWDiagTXDhcpSent", @"RTWDiagTXDhcpRepOK", @"RTWDiagTXDhcpDrops",
            @"RTWDiagTXDataFrames", @"RTWDiagRXDataToUs", @"RTWDiagRXDataBcast"]];
        RTWLog(@"link check %.0f s after connect: NO IPv4 on %@ (no DHCP lease, or only a self-assigned 169.254 address) - %@ - "
               "the driver rebuilds the link by itself if the lease never comes; Rejoin stays available meanwhile",
               upFor, ifname ?: @"the dongle interface", counters);
    }
    self.linkHasNoAddress = YES;
}

- (NSString *)linkDropReasonNote {
    uint32_t reason = [self.driver lastDeauthReason];
    if (reason && reason != self.deauthReasonAtLinkUp) {
        return [NSString stringWithFormat:@"- the AP ended it (deauth reason %u: %@)",
                (unsigned)reason, [self decodeDeauthReason:reason]];
    }
    return @"- no deauth recorded (local reset)";
}

- (NSString *)decodeDeauthReason:(uint32_t)reason {
    switch (reason) {
        case 1:  return @"unspecified";
        case 2:  return @"previous authentication no longer valid";
        case 3:  return @"station leaving / deauthenticated";
        case 4:  return @"inactivity";
        case 5:  return @"AP cannot handle all associated stations";
        case 6:  return @"class 2 frame from unauthenticated station";
        case 7:  return @"class 3 frame from unassociated station";
        case 8:  return @"station leaving (disassociated)";
        case 15: return @"4-way handshake timeout - wrong password is the most common cause";
        case 16: return @"group key handshake timeout";
        case 23: return @"802.1X authentication failed";
        case 34: return @"AP kicked the station (transition or low-ack policy)";
        default: return [NSString stringWithFormat:@"reason %u", (unsigned)reason];
    }
}

#pragma mark NSMenuDelegate

/* The main menu and the collapsed Networks submenu rebuild independently:
 * opening either refreshes it from the latest snapshot. Nothing here ever
 * opens or expands a menu programmatically, so clicking anywhere outside
 * the status item dismisses the menu and it stays closed until the user
 * clicks the icon again. */
- (void)menuNeedsUpdate:(NSMenu *)menu {
    @try {
        if (menu == self.networksSubmenu) {
            [self rebuildNetworksSubmenu:menu];
        } else {
            [self rebuildMainMenu];
        }
    } @catch (NSException *exception) {
        RTWLog(@"menu exception contained: %@ (%@)", exception.name, exception.reason);
    }
}

- (void)menuDidClose:(NSMenu *)menu {
    if (menu == self.menu) self.networksExpanded = NO; /* always reopens collapsed */
}

- (void)rebuildMainMenu {
    NSMenu *menu = self.menu;
    if (!menu) return;
    while (menu.numberOfItems > 0) [menu removeItemAtIndex:0];

    [self.stateLock lock];
    RTWDriverStatus *status = self.lastStatus;
    BOOL scanning = self.scanning;
    [self.stateLock unlock];

    /* 0. 8.8.5: Disconnect / Reconnect USB WiFi on top, like the stock
     *    Realtek StatusBarApp. Clickable while the dongle is reachable. */
    /* mejoras: name the network, so it reads as "disconnect from
     * this network" (same action: leave and stay off until you pick one). */
    NSString *toggleTitle;
    if (self.userDisconnected) {
        toggleTitle = self.savedSsid.length
            ? [NSString stringWithFormat:NSLocalizedString(@"Reconnect to \"%@\"", nil), self.savedSsid]
            : NSLocalizedString(@"Reconnect USB WiFi", nil);
    } else {
        NSString *joining = self.joinInFlightSsid.length ? self.joinInFlightSsid
                          : (self.scanDetourSsid.length ? self.scanDetourSsid : self.switchingToSsid);
        if (status.linked && status.ssid.length)
            toggleTitle = [NSString stringWithFormat:NSLocalizedString(@"Disconnect from \"%@\"", nil), status.ssid];
        else if (joining.length) /* mejoras: same action, clearer words */
            toggleTitle = [NSString stringWithFormat:NSLocalizedString(@"Cancel joining \"%@\"", nil), joining];
        else
            toggleTitle = NSLocalizedString(@"Disconnect USB WiFi", nil);
    }
    NSMenuItem *usbToggle = [[NSMenuItem alloc] initWithTitle:toggleTitle
        action:@selector(toggleUSBWifi:) keyEquivalent:@""];
    usbToggle.target = self;
    usbToggle.enabled = (status != nil && status.driverReachable);
    [menu addItem:usbToggle];
    [menu addItem:[NSMenuItem separatorItem]];

    /* 1. Status lines. */
    NSMenuItem *statusItem = [[NSMenuItem alloc] initWithTitle:[self statusLine:status]
                                                        action:nil keyEquivalent:@""];
    statusItem.tag = kTagStatusLine;
    statusItem.enabled = NO;
    /* mejoras: while connected, a clear two-line block instead of
     * the dimmed status text: "Connected to <network>" + band, signal, IP. */
    if (status.driverReachable && status.linked && !self.userDisconnected) {
        statusItem.view = [self connectedStatusViewForStatus:status];
    }
    [menu addItem:statusItem];
    if (!status || !status.driverReachable) {
        NSMenuItem *detailItem = [[NSMenuItem alloc] initWithTitle:[self statusDetailLine:status]
                                                            action:nil keyEquivalent:@""];
        detailItem.enabled = NO;
        [menu addItem:detailItem];
    } else if (scanning) {
        NSMenuItem *scanItem = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Scanning…", nil)
                                                          action:nil keyEquivalent:@""];
        scanItem.tag = kTagScanningLine;
        scanItem.enabled = NO;
        [menu addItem:scanItem];
    }

    /* 2. Networks: collapsed row with a chevron. Clicking it unfolds the
     *    list inside this same menu (no side window); the menu always
     *    reopens collapsed and closes on any outside click. */
    [menu addItem:[NSMenuItem separatorItem]];
    self.networksSubmenu = nil;
    self.networksInlineCount = 0;
    NSMenuItem *header = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Networks", nil) action:nil keyEquivalent:@""];
    header.tag = kTagNetworksMenu;
    RTLDisclosureRow *row = [[RTLDisclosureRow alloc] initWithFrame:NSMakeRect(0, 0, 260, 22)];
    row.expanded = self.networksExpanded;
    __weak typeof(self) weakSelf = self;
    __weak RTLDisclosureRow *weakRow = row;
    row.onClick = ^{ [weakSelf toggleNetworksInline:weakRow]; };
    header.view = row;
    [menu addItem:header];
    if (self.networksExpanded) [self insertNetworksInlineAfterHeader];

    /* 3. Actions. Rescan stays enabled even while unplugged: it retries
     *    the connection first, so it doubles as a manual "find the dongle". */
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *rescan = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Rescan Now", nil)
                                                    action:@selector(rescanNow:) keyEquivalent:@""];
    rescan.target = self;
    rescan.tag = kTagRescan;
    rescan.enabled = YES;
    [menu addItem:rescan];

    NSMenuItem *pauseCable = [[NSMenuItem alloc] initWithTitle:
        NSLocalizedString(@"Pause Wi-Fi while a cable is connected", nil)
        action:@selector(togglePauseWithCable:) keyEquivalent:@""];
    pauseCable.target = self;
    pauseCable.state = [[NSUserDefaults standardUserDefaults] boolForKey:kDefaultsPauseWithCable]
        ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:pauseCable];

    NSMenuItem *prefer5 = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Prefer 5 GHz", nil)
        action:@selector(togglePrefer5GHz:) keyEquivalent:@""];
    prefer5.target = self;
    prefer5.state = [[NSUserDefaults standardUserDefaults] boolForKey:kDefaultsPrefer5GHz]
        ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:prefer5];

    /* v8.7.17: profiles persist across restarts now, so the saved network
     * is pinned again at launch and reconnects by itself (one bounded
     * auto-rejoin, armed at launch). Rejoin stays as the USER's button
     * for every other case - a mid-session drop, an auto-rejoin that
     * gave up: a click, never an automatic action. Forget removes the
     * profile, the pin and the persistence. */
    if (self.savedSsid.length &&
        [self.profiles hasProfileForSsid:self.savedSsid]) {
        NSMenuItem *rejoin = [[NSMenuItem alloc] initWithTitle:
            [NSString stringWithFormat:NSLocalizedString(@"Rejoin \"%@\" Now", nil), self.savedSsid]
            action:@selector(rejoinSavedNow:) keyEquivalent:@""];
        rejoin.target = self;
        /* 8.8.5: also clickable while linked without an IPv4 address - the
         * kext rebuilds a stuck data path on a re-join to the same network
         * and leaves a healthy link untouched. */
        rejoin.enabled = (status != nil && status.driverReachable &&
                          (!status.linked || self.linkHasNoAddress));
        [menu addItem:rejoin];
    }
    NSString *forgetTarget = self.savedSsid;
    if (forgetTarget.length) {
        NSMenuItem *forget = [[NSMenuItem alloc] initWithTitle:
            [NSString stringWithFormat:NSLocalizedString(@"Forget \"%@\"", nil), forgetTarget]
            action:@selector(forgetNetwork:) keyEquivalent:@""];
        forget.target = self;
        forget.tag = kTagForget;
        forget.enabled = YES;
        [menu addItem:forget];
    }

    /* 4. Standard items. */
    [menu addItem:[NSMenuItem separatorItem]];
    /* v8.7.8: "Enable Driver at Boot" is gone from the menu entirely, and
     * the app never runs the osascript boot setup (user request). The PKG
     * installer already loads the driver at boot - the app has no job
     * there and never asks for the admin password. */

    /* v8.7.7: "Copy Diagnostics" removed - Reveal Log opens the very same
     * log file, so the duplicate item is gone. */
    NSMenuItem *revealLog = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Reveal Log", nil)
                                                       action:@selector(revealLog:) keyEquivalent:@""];
    revealLog.target = self;
    revealLog.enabled = YES;
    [menu addItem:revealLog];

    NSMenuItem *about = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"About RTLWiFiBar", nil)
                                                   action:@selector(showAbout:) keyEquivalent:@""];
    about.target = self;
    about.tag = kTagAbout;
    about.enabled = YES;
    [menu addItem:about];

    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:NSLocalizedString(@"Quit RTLWiFiBar", nil)
                                                  action:@selector(quitApp:) keyEquivalent:@""];
    quit.target = self;
    quit.tag = kTagQuit;
    quit.enabled = YES;
    [menu addItem:quit];
}

- (void)insertNetworksInlineAfterHeader {
    NSMenu *menu = self.menu;
    NSInteger idx = [menu indexOfItemWithTag:kTagNetworksMenu];
    if (!menu || idx < 0) return;
    NSMenu *tmp = [[NSMenu alloc] initWithTitle:@"tmp"];
    tmp.autoenablesItems = NO;
    [self appendNetworksToMenu:tmp];
    NSInteger n = 0;
    while (tmp.numberOfItems > 0) {
        NSMenuItem *it = [tmp itemAtIndex:0];
        [tmp removeItemAtIndex:0];
        [menu insertItem:it atIndex:idx + 1 + n];
        n++;
    }
    self.networksInlineCount = n;
}

- (void)toggleNetworksInline:(RTLDisclosureRow *)row {
    @try {
        NSMenu *menu = self.menu;
        NSInteger idx = [menu indexOfItemWithTag:kTagNetworksMenu];
        if (!menu || idx < 0) return;
        self.networksExpanded = !self.networksExpanded;
        row.expanded = self.networksExpanded;
        if (self.networksExpanded) {
            [self insertNetworksInlineAfterHeader];
        } else {
            for (NSInteger i = 0; i < self.networksInlineCount; i++) {
                if (idx + 1 < menu.numberOfItems) [menu removeItemAtIndex:idx + 1];
            }
            self.networksInlineCount = 0;
        }
    } @catch (NSException *exception) {
        RTWLog(@"networks toggle exception contained: %@ (%@)", exception.name, exception.reason);
    }
}

/* Contents of the collapsed Networks submenu: the active network pinned on
 * top (grey Wi-Fi icon left, white lock right - the lock ONLY on the known,
 * activated network), then the remaining scan results without any lock. */
- (void)rebuildNetworksSubmenu:(NSMenu *)subnet {
    if (!subnet) return;
    while (subnet.numberOfItems > 0) [subnet removeItemAtIndex:0];
    [self appendNetworksToMenu:subnet];
}

- (void)appendNetworksToMenu:(NSMenu *)subnet {
    if (!subnet) return;

    [self.stateLock lock];
    RTWDriverStatus *status = self.lastStatus;
    NSArray<RTWNetworkEntry *> *networks = self.lastNetworks;
    BOOL scanning = self.scanning;
    [self.stateLock unlock];

    NSArray<RTWNetworkEntry *> *sorted = [networks sortedArrayUsingComparator:
        ^NSComparisonResult(RTWNetworkEntry *a, RTWNetworkEntry *b) {
            if (a.joinable != b.joinable) return a.joinable ? NSOrderedAscending : NSOrderedDescending;
            return b.rssiPercent - a.rssiPercent;
        }];

    /* The active network: the link target while connected, otherwise the
     * network the user selected in this session (pinned for one click
     * Rejoin / Forget - it never joins by itself). */
    NSString *activeSsid = nil;
    if (status.linked && status.ssid.length) activeSsid = status.ssid;
    else if (self.savedSsid.length) activeSsid = self.savedSsid;

    NSString *activeTitle = nil;
    if (activeSsid) activeTitle = [self activeRowTitleForSsid:activeSsid status:status];

    /* Size the pinned row so its right-edge lock lines up with the rest of
     * the collapsed menu. */
    /* mejoras: while linked, hide only the scan record of the AP
     * we are on (same SSID + same channel); the network's other APs stay in
     * the list as information. The driver joins by SSID only, so they are
     * not clickable yet. */
    RTWNetworkEntry *linkedEntry = nil;
    if (status.linked && activeSsid) {
        for (RTWNetworkEntry *e in sorted) {
            if (rtlPrintableSsidEqual(e.ssid, activeSsid) && e.channel == status.channel) {
                linkedEntry = e;
                break;
            }
        }
    }
    self.listLinkedSsid = (status.linked && activeSsid) ? activeSsid : nil;
    self.listLinkedEntry = linkedEntry;

    CGFloat rowWidth = [self submenuRowWidthForTitles:
        [self titlesForSubmenuRows:sorted excluding:activeSsid fallbackTitle:activeTitle]];

    /* mejoras: "Mac + band" columns - name, "band · CH n", and the
     * lock + signal glyphs at the right edge, the same x as the connected
     * row's glyphs (its view starts 16 pt left of a menu item's text). */
    NSFont *rowFont = [NSFont menuFontOfSize:13];
    CGFloat nameWidth = activeSsid ? [activeSsid sizeWithAttributes:@{NSFontAttributeName: rowFont}].width : 0.0;
    for (RTWNetworkEntry *e in sorted) {
        NSString *n = e.ssid.length ? e.ssid : NSLocalizedString(@"(hidden)", nil);
        CGFloat w = [n sizeWithAttributes:@{NSFontAttributeName: rowFont}].width;
        if (w > nameWidth) nameWidth = w;
    }
    CGFloat bandTab = ceil(nameWidth) + 20.0;
    CGFloat bandWidth = ceil([@"2.4 GHz · CH 165" sizeWithAttributes:@{NSFontAttributeName: rowFont}].width);
    /* The width now comes from these columns only: the old text titles
     * ("name · band · signal ✓ · other access point") made the menu much
     * wider than the rows (field screenshot 2026-11-24 16:45). The pinned
     * row's fallback text ("name - saved") still counts. */
    CGFloat fallbackWidth = activeTitle.length
        ? [activeTitle sizeWithAttributes:@{NSFontAttributeName: rowFont}].width + 28.0 + 50.0 : 0.0;
    /* Measured on the field screenshot (2026-11-24 17:18): a menu item's
     * text starts 25 pt right of a custom row view's origin, and the title
     * is clipped ~45 pt before the menu's right edge (the signal glyph at
     * rowWidth-40 never showed). Glyphs: lock at rowWidth-80, signal at
     * rowWidth-60 in text coordinates = rowWidth-55 / -35 in the view. */
    rowWidth = MAX(220.0, MAX(ceil(fallbackWidth), kListTextInset + bandTab + bandWidth + 20.0 + 80.0));
    CGFloat lockTab = rowWidth - kListTextInset - 55.0;
    CGFloat barsTab = rowWidth - kListTextInset - 35.0;
    self.listBandTab = bandTab;

    BOOL pinnedKnown = activeSsid && [self isKnownSsid:activeSsid];
    if (activeSsid) {
        NSMenuItem *pinned = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        pinned.view = [self activeNetworkRowForSsid:activeSsid
                                             status:status
                                             locked:pinnedKnown
                                             width:rowWidth];
        pinned.enabled = NO;
        [subnet addItem:pinned];
    }

    NSInteger shown = 0;
    for (RTWNetworkEntry *entry in sorted) {
        if ([self isHiddenFromList:entry activeSsid:activeSsid]) continue;
        /* v8.7.10: 15 hid real APs - the 8.7.9 field scan saw 18 networks
         * and the user could not reach theirs in the list. macOS scrolls a
         * long submenu; 40 keeps every scan result clickable. */
        if (shown >= 40) break;
        /* mejoras: other APs of the linked network are clickable -
         * the join is pinned to the picked AP (kext 1.1.2-tr.1+). */
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:[self listTitleForEntry:entry]
                                                      action:@selector(joinFromMenu:)
                                               keyEquivalent:@""];
        item.attributedTitle = [self macRowTitleForEntry:entry font:rowFont
                                                 bandTab:bandTab lockTab:lockTab barsTab:barsTab];
        item.target = self;
        item.tag = kTagNetworkBase + shown;
        item.representedObject = entry;
        item.enabled = entry.joinable;
        [subnet addItem:item];
        shown++;
    }

    if (!activeSsid && !shown) {
        NSMenuItem *emptyItem = [[NSMenuItem alloc] initWithTitle:
            scanning ? NSLocalizedString(@"Scanning…", nil) : NSLocalizedString(@"No networks found", nil)
                                                      action:nil keyEquivalent:@""];
        emptyItem.enabled = NO;
        [subnet addItem:emptyItem];
    }
}

/* mejoras: list filtering. Linked: hide only the AP we are on.
 * Not linked: hide nothing - every AP of the saved network stays in the
 * list so a specific one (e.g. the 5 GHz AP) can be picked; the pinned row
 * above still rejoins by name. Field 2026-11-24 16:31: the link was down
 * and the 4 APs of 5 GHz in the driver cache never reached the menu. */
- (BOOL)isHiddenFromList:(RTWNetworkEntry *)entry activeSsid:(nullable NSString *)activeSsid {
    if (!activeSsid || !rtlPrintableSsidEqual(entry.ssid, activeSsid)) return NO;
    if (!self.listLinkedSsid) return NO;
    return entry == self.listLinkedEntry;
}

- (BOOL)isSiblingAccessPoint:(RTWNetworkEntry *)entry {
    return self.listLinkedSsid && entry != self.listLinkedEntry
        && rtlPrintableSsidEqual(entry.ssid, self.listLinkedSsid);
}

- (NSString *)listTitleForEntry:(RTWNetworkEntry *)entry {
    NSString *title = [self titleForEntry:entry];
    if (![self isSiblingAccessPoint:entry]) return title;
    return [NSString stringWithFormat:@"%@  ·  %@", title, NSLocalizedString(@"other access point", nil)];
}

/* mejoras (the user's pick, 2026-11-24): "Mac + band" list rows.
 *   RP3_UNIFI   5 GHz    [lock] [wifi glyph filled by signal]
 * The lock and the signal are the native SF Symbols (lock.fill, the
 * variable "wifi" glyph), drawn in the menu's text color. */
- (NSInteger)barsForDbm:(int32_t)dbm {
    if (dbm >= -60) return 4;
    if (dbm >= -67) return 3;
    if (dbm >= -75) return 2;
    return 1;
}

- (NSAttributedString *)macRowTitleForEntry:(RTWNetworkEntry *)entry
                                       font:(NSFont *)font
                                    bandTab:(CGFloat)bandTab
                                    lockTab:(CGFloat)lockTab
                                    barsTab:(CGFloat)barsTab {
    NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
    ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:bandTab options:@{}],
                    [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:lockTab options:@{}],
                    [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:barsTab options:@{}]];
    NSDictionary *base = @{NSFontAttributeName: font, NSParagraphStyleAttributeName: ps};
    NSString *name = entry.ssid.length ? entry.ssid : NSLocalizedString(@"(hidden)", nil);
    NSMutableAttributedString *t = [[NSMutableAttributedString alloc] initWithString:
        [NSString stringWithFormat:@"%@\t%@", name, [self bandForChannel:entry.channel]] attributes:base];
    [t appendAttributedString:[self channelSuffix:entry.channel font:font paragraph:ps]];
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t" attributes:base]];
    if (!entry.joinable) {
        [t appendAttributedString:[[NSAttributedString alloc] initWithString:entry.securityLabel attributes:base]];
        return t;
    }
    /* Text attachments are not template-tinted: pick the glyph color from
     * the menu's appearance (rebuilt every time the menu opens). */
    NSColor *ink = [self menuIsDark] ? [NSColor colorWithCalibratedWhite:1.0 alpha:0.9]
                                     : [NSColor colorWithCalibratedWhite:0.0 alpha:0.85];
    if (entry.needsPassphrase) {
        NSTextAttachment *lock = [[NSTextAttachment alloc] init];
        lock.image = [RTWSignalIcon menuLockIconTint:ink];
        lock.bounds = CGRectMake(0, -1, 12, 12);
        [t appendAttributedString:[NSAttributedString attributedStringWithAttachment:lock]];
    }
    [t appendAttributedString:[[NSAttributedString alloc] initWithString:@"\t" attributes:base]];
    NSTextAttachment *sig = [[NSTextAttachment alloc] init];
    sig.image = [RTWSignalIcon menuSignalIconBars:[self barsForDbm:entry.rssiDbm] tint:ink];
    sig.bounds = CGRectMake(0, -3, 15, 15);
    [t appendAttributedString:[NSAttributedString attributedStringWithAttachment:sig]];
    [t addAttributes:@{NSParagraphStyleAttributeName: ps} range:NSMakeRange(0, t.length)];
    return t;
}

/* " · CH 44" in the secondary label color (empty when the channel is unknown). */
- (NSAttributedString *)channelSuffix:(uint32_t)channel font:(NSFont *)font
                            paragraph:(nullable NSParagraphStyle *)ps {
    if (!channel) return [[NSAttributedString alloc] initWithString:@""];
    NSMutableDictionary *a = [@{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: [NSColor secondaryLabelColor]} mutableCopy];
    if (ps) a[NSParagraphStyleAttributeName] = ps;
    return [[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@" · CH %u", channel] attributes:a];
}

#pragma mark AP choice (mejoras)

- (NSString *)bssidHexForEntry:(RTWNetworkEntry *)e {
    uint8_t b[6] = {0};
    [e getBSSID:b];
    return [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]];
}

- (nullable NSString *)userPickedAPForSsid:(NSString *)ssid {
    if (!ssid.length) return nil;
    id v = [[NSUserDefaults standardUserDefaults] dictionaryForKey:kDefaultsPreferredAP][ssid];
    return [v isKindOfClass:[NSString class]] ? v : nil;
}

- (void)setUserPickedAP:(nullable NSString *)hex forSsid:(NSString *)ssid {
    if (!ssid.length) return;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary *m = [[d dictionaryForKey:kDefaultsPreferredAP] mutableCopy] ?: [NSMutableDictionary dictionary];
    if (hex.length) m[ssid] = hex; else [m removeObjectForKey:ssid];
    [d setObject:m forKey:kDefaultsPreferredAP];
}

/* Which AP of `ssid` an automatic or Reconnect join should pin: the one the
 * user picked last, else (Prefer 5 GHz) the strongest 5 GHz AP at -75 dBm
 * or better. nil = join by name (the kext takes the strongest AP, which in
 * the field is nearly always a 2.4 GHz one). */
- (nullable RTWNetworkEntry *)preferredAPForSsid:(NSString *)ssid
                                       inRecords:(nullable NSArray<RTWNetworkEntry *> *)records
                                          reason:(NSString * _Nullable * _Nullable)why {
    NSString *want = [self userPickedAPForSsid:ssid];
    if (want.length) {
        for (RTWNetworkEntry *e in records) {
            if (e.joinable && rtlPrintableSsidEqual(e.ssid, ssid) &&
                [[self bssidHexForEntry:e] isEqualToString:want]) {
                if (why) *why = @"the AP you picked";
                return e;
            }
        }
    }
    if ([[NSUserDefaults standardUserDefaults] boolForKey:kDefaultsPrefer5GHz]) {
        RTWNetworkEntry *best = nil;
        for (RTWNetworkEntry *e in records) {
            if (!e.joinable || !rtlPrintableSsidEqual(e.ssid, ssid) || e.channel <= 14 || e.rssiDbm < -75) continue;
            if (!best || e.rssiDbm > best.rssiDbm) best = e;
        }
        if (best) { if (why) *why = @"5 GHz preferred"; return best; }
    }
    return nil;
}

- (BOOL)wantsSpecificAPForSsid:(NSString *)ssid {
    return [self userPickedAPForSsid:ssid].length > 0 ||
           [[NSUserDefaults standardUserDefaults] boolForKey:kDefaultsPrefer5GHz];
}

/* Automatic rejoins and Reconnect go through here instead of joining by
 * name: pin the preferred AP when the driver's survey has it; otherwise
 * (at most once a minute) leave, sweep every channel until it shows up -
 * up to 9 s - and then join; if it never shows up, join by name. */
- (void)joinSavedEntry:(RTWNetworkEntry *)fallback
            passphrase:(nullable NSString *)pass
             automatic:(BOOL)automatic {
    NSString *ssid = fallback.ssid;
    void (^submit)(RTWNetworkEntry *) = ^(RTWNetworkEntry *target) {
        if (automatic) {
            [self submitAutoRejoin:target passphrase:pass];
        } else {
            [self submitJoin:target passphrase:pass];
            if (pass.length > 0 && self.joinInFlightSsid.length) self.joinWatchSavedPass = YES;
        }
    };
    [self.stateLock lock];
    NSArray<RTWNetworkEntry *> *records = self.driverSurveyEntries;
    [self.stateLock unlock];
    NSString *why = nil;
    RTWNetworkEntry *pick = [self preferredAPForSsid:ssid inRecords:records reason:&why];
    if (pick) {
        pick.pinBssid = YES;
        RTWLog(@"rejoin \"%@\": pinning %@ on ch %u (%@)", ssid, [self bssidHexForEntry:pick], pick.channel, why);
        submit(pick);
        return;
    }
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (![self wantsSpecificAPForSsid:ssid] || now - self.lastPreferSweepAt < 60.0) {
        submit(fallback);
        return;
    }
    self.lastPreferSweepAt = now;
    self.scanDetourSsid = ssid;
    RTWLog(@"rejoin \"%@\": the preferred AP is not in the driver's survey - sweeping first", ssid);
    dispatch_async(self.workQueue, ^{
        NSString *detail = nil;
        [self.driver disconnectFromNetwork:&detail]; /* clears the kext's join guard so it sweeps */
        [NSThread sleepForTimeInterval:1.0];
        [self performScan];
        RTWNetworkEntry *found = nil;
        NSString *foundWhy = nil;
        for (int i = 0; i < 13 && !found; i++) {
            [NSThread sleepForTimeInterval:0.7];
            if (!rtlPrintableSsidEqual(self.scanDetourSsid, ssid)) return;
            found = [self preferredAPForSsid:ssid inRecords:[self.driver surveyRecords:NULL] reason:&foundWhy];
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!rtlPrintableSsidEqual(self.scanDetourSsid, ssid) || self.userDisconnected) return;
            self.scanDetourSsid = nil;
            if (found) {
                found.pinBssid = YES;
                RTWLog(@"rejoin \"%@\": sweep found %@ on ch %u (%@) - pinning it",
                       ssid, [self bssidHexForEntry:found], found.channel, foundWhy);
            } else {
                RTWLog(@"rejoin \"%@\": sweep did not find the preferred AP - joining by name", ssid);
            }
            submit(found ?: fallback);
        });
    });
}

- (void)togglePrefer5GHz:(nullable NSMenuItem *)sender {
    (void)sender;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    BOOL on = ![d boolForKey:kDefaultsPrefer5GHz];
    [d setBool:on forKey:kDefaultsPrefer5GHz];
    RTWLog(@"prefer 5 GHz: %@", on ? @"ON" : @"OFF");
}

- (NSString *)surveyKeyForEntry:(RTWNetworkEntry *)e {
    uint8_t b[6] = {0};
    [e getBSSID:b];
    return [NSString stringWithFormat:@"%@|%02x%02x%02x%02x%02x%02x",
            e.ssid ?: @"", b[0], b[1], b[2], b[3], b[4], b[5]];
}

- (BOOL)menuIsDark {
    if (@available(macOS 10.14, *)) {
        NSAppearanceName m = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:
                              @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        return [m isEqualToString:NSAppearanceNameDarkAqua];
    }
    return NO;
}

- (NSArray<NSString *> *)titlesForSubmenuRows:(NSArray<RTWNetworkEntry *> *)entries
                                    excluding:(NSString *)activeSsid
                                fallbackTitle:(NSString *)fallback {
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    if (fallback.length) [titles addObject:fallback];
    NSInteger shown = 0;
    for (RTWNetworkEntry *entry in entries) {
        if ([self isHiddenFromList:entry activeSsid:activeSsid]) continue;
        if (shown >= 40) break; /* v8.7.10: width parity with the list cap */
        [titles addObject:[self listTitleForEntry:entry]];
        shown++;
    }
    return titles;
}

- (CGFloat)submenuRowWidthForTitles:(NSArray<NSString *> *)titles {
    NSFont *font = [NSFont menuFontOfSize:13];
    CGFloat widest = 0.0;
    for (NSString *title in titles) {
        CGFloat w = [title sizeWithAttributes:@{NSFontAttributeName: font}].width;
        if (w > widest) widest = w;
    }
    /* Item padding + icon gutter headroom; never narrower than a compact
     * default so the pinned row keeps its proportions. */
    return MAX(240.0, ceil(widest) + 44.0);
}

- (NSString *)activeRowTitleForSsid:(NSString *)ssid
                             status:(nullable RTWDriverStatus *)status {
    if (status.linked && rtlPrintableSsidEqual(status.ssid, ssid)) {
        /* mejoras: "Mac + band" - name and band; the signal is the
         * glyph on the right of the row. */
        return [NSString stringWithFormat:@"%@   %@", ssid, [self bandForChannel:status.channel]];
    }
    /* "saved" only when a remembered profile exists; a merely selected
     * network reads "selected" so it never masquerades as saved. */
    return [NSString stringWithFormat:@"%@ - %@", ssid,
            [self isKnownSsid:ssid] ? NSLocalizedString(@"saved", nil) : NSLocalizedString(@"selected", nil)];
}

- (BOOL)isKnownSsid:(NSString *)ssid {
    /* v8.7.9: printable-prefix membership - a padded survey record must
     * still recognize the remembered network. */
    if (!ssid.length) return NO;
    for (NSString *known in self.knownSsids) {
        if (rtlPrintableSsidEqual(known, ssid)) return YES;
    }
    return NO;
}

/* The active row: grey Wi-Fi icon on the left, network title, and the
 * white lock on the right ONLY when this network is the known, activated
 * one. Display-only (enabled = NO), so it never highlights. */
- (nullable NSView *)activeNetworkRowForSsid:(NSString *)ssid
                                      status:(nullable RTWDriverStatus *)status
                                      locked:(BOOL)locked
                                      width:(CGFloat)width {
    NSView *row = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 22)];

    NSImageView *wifi = [[NSImageView alloc] initWithFrame:NSMakeRect(6, 3, 16, 16)];
    /* mejoras: the connected network reads like the native menu
     * (accent-colored Wi-Fi glyph, normal label text) instead of a fixed
     * grey that looks disabled. Not linked: secondary color. */
    BOOL rowLinked = status.linked && rtlPrintableSsidEqual(status.ssid, ssid);
    wifi.image = [RTWSignalIcon menuWifiTemplateIcon];
    if (@available(macOS 10.14, *)) {
        wifi.contentTintColor = rowLinked ? [NSColor controlAccentColor]
                                          : [NSColor secondaryLabelColor];
    }
    [row addSubview:wifi];

    /* Hand-built label: the AppKit class factory
     * +[NSTextField labelTextFieldWithString:] is no longer declared by the
     * macOS 26 SDK, so recreate the exact label appearance it produced
     * (editable off, selectable off, no bezel, no background). This compiles
     * against every SDK from 10.7 to current. */
    NSTextField *label = [[NSTextField alloc] initWithFrame:
        NSMakeRect(28, 3, width - 28 - 60, 16)];
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.bordered = NO;
    label.drawsBackground = NO;
    label.stringValue = [self activeRowTitleForSsid:ssid status:status];
    /* mejoras: same look as the list - signal glyph on the right,
     * the lock just left of it. */
    if (rowLinked) {
        NSImageView *sig = [[NSImageView alloc] initWithFrame:NSMakeRect(width - 35, 3, 16, 16)];
        sig.image = [RTWSignalIcon menuSignalIconBars:[self barsForDbm:status.rssiPercent / 2 - 100] tint:nil];
        if (@available(macOS 10.14, *)) sig.contentTintColor = [NSColor labelColor];
        [row addSubview:sig];
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.usesSingleLineMode = YES;
    label.font = [NSFont menuFontOfSize:13];
    label.textColor = rowLinked ? [NSColor labelColor] : [NSColor secondaryLabelColor];
    if (rowLinked && self.listBandTab > 28.0 - kListTextInset) {
        /* mejoras: band column lined up with the list below (this
         * label starts 12 pt right of a menu item's text). */
        NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
        ps.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft
                                                         location:self.listBandTab - (28.0 - kListTextInset) options:@{}]];
        NSFont *f = [NSFont menuFontOfSize:13];
        NSMutableAttributedString *t = [[NSMutableAttributedString alloc] initWithString:
            [NSString stringWithFormat:@"%@\t%@", ssid, [self bandForChannel:status.channel]]
            attributes:@{NSFontAttributeName: f, NSParagraphStyleAttributeName: ps,
                         NSForegroundColorAttributeName: [NSColor labelColor]}];
        [t appendAttributedString:[self channelSuffix:status.channel font:f paragraph:ps]];
        label.attributedStringValue = t;
    }
    [row addSubview:label];

    NSImageView *lock = [[NSImageView alloc] initWithFrame:
        NSMakeRect(rowLinked ? width - 55 : width - 22, rowLinked ? 5 : 3, rowLinked ? 12 : 16, rowLinked ? 12 : 16)];
    if (locked) {
        lock.image = rowLinked ? [RTWSignalIcon menuLockIconTint:nil] : [RTWSignalIcon menuLockTemplateIcon];
        if (@available(macOS 10.14, *)) lock.contentTintColor = [NSColor labelColor];
    }
    [row addSubview:lock];

    return row;
}

- (NSString *)statusLine:(nullable RTWDriverStatus *)status {
    if (!status || !status.driverReachable) {
        /* v8.7.7: the menu says the truth - "Unplugged" was wrong while the
         * dongle sat on USB with the driver never loaded (that state read
         * as "Unplugged" for nine minutes in the 18:06 field log). */
        if ([RTWDriverClient rtlUsbDonglePresent]) return NSLocalizedString(@"Dongle on USB - driver not loaded", nil);
        return NSLocalizedString(@"Dongle: Unplugged", nil);
    }
    if (self.userDisconnected) return NSLocalizedString(@"USB WiFi disconnected", nil); /* 8.8.5 */
    if (self.cablePaused) return NSLocalizedString(@"Wi-Fi paused - cable connected", nil);
    if (self.scanning && !status.linked) return NSLocalizedString(@"Scanning…", nil);
    if (status.linked) {
        return [NSString stringWithFormat:NSLocalizedString(@"%@ - %d dBm, channel %u", nil),
                status.ssid ?: NSLocalizedString(@"Connected", nil), status.rssiPercent / 2 - 100,
                (unsigned)status.channel];
    }
    /* Live join feedback (v8.7.1) replaces the removed modal alert: the
     * line is re-read every time the menu opens. */
    if (self.joinInFlightSsid.length) {
        NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
        if (self.joinAttemptFailures > 0) {
            return [NSString stringWithFormat:NSLocalizedString(@"Joining \"%@\" - no link, retrying (attempt %ld)", nil),
                    self.joinInFlightSsid, (long)(self.joinAttemptFailures + 1)];
        }
        return [NSString stringWithFormat:NSLocalizedString(@"Joining \"%@\" - %ld s", nil),
                self.joinInFlightSsid, (long)((now - self.joinStartedAt) + 0.5)];
    }
    /* 8.8.5: associated without a user join in flight (the kext's own WPA
     * recovery, a rekey) - not connected yet, and never "Connected". */
    if (status.securing && self.savedSsid.length) {
        return [NSString stringWithFormat:NSLocalizedString(@"Joining \"%@\" - not connected yet%@", nil),
                self.savedSsid, @""];
    }
    /* v8.7.14: the honest failure report stays on the menu until the
     * next join or the next link replaces it. */
    if (self.lastJoinFailureNote.length) return self.lastJoinFailureNote;
    /* v8.7.11: the boot wedge deserves its own honest line, not a fake
     * "Dongle ready" while every scan is refused with 0xe00002d8. Cleared
     * by the first successful scan trigger (driver recovered, kext 1.0.7
     * boot-rescue re-plug worked, or the user replugged). */
    if (self.wedgedNoticeShown && !status.linked) {
        return NSLocalizedString(@"Adapter chip did not start - re-plug it once (driver retrying)", nil);
    }
    return NSLocalizedString(@"Dongle ready - not connected", nil);
}

#pragma mark Cable watch (mejoras)

- (void)togglePauseWithCable:(nullable NSMenuItem *)sender {
    (void)sender;
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    BOOL on = ![d boolForKey:kDefaultsPauseWithCable];
    [d setBool:on forKey:kDefaultsPauseWithCable];
    self.cableOverride = NO;
    RTWLog(@"pause Wi-Fi while a cable is connected: %@", on ? @"ON" : @"OFF");
    [self cableWatchTick:nil];
}

- (void)cableWatchTick:(nullable NSTimer *)timer {
    (void)timer;
    @try {
        BOOL enabled = [[NSUserDefaults standardUserDefaults] boolForKey:kDefaultsPauseWithCable];
        if (!enabled) {
            if (self.cablePaused) [self resumeFromCablePause:@"the option was turned off"];
            return;
        }
        [self.stateLock lock];
        RTWDriverStatus *status = self.lastStatus;
        [self.stateLock unlock];
        if (!status.driverReachable) return;

        NSString *dongle = nil;
        [self.driver interfaceIPv4:&dongle];
        NSString *wiredName = nil;
        BOOL cable = rtlWiredEthernetUp(dongle, &wiredName);

        if (cable) {
            self.cableGoneTicks = 0;
            if (self.cablePaused || self.cableOverride || self.userDisconnected) return;
            if (self.joinInFlightSsid.length || self.scanDetourSsid.length ||
                self.switchingToSsid.length) return; /* let a running join finish */
            NSString *ssid = status.linked && status.ssid.length ? status.ssid : self.savedSsid;
            self.cablePaused = YES;
            self.cablePausedSsid = ssid;
            [self standDownStartupAutoRejoin:@"Wi-Fi paused - cable connected"];
            if (status.linked) {
                NSString *detail = nil;
                BOOL ok = [self.driver disconnectFromNetwork:&detail];
                RTWLog(@"cable on %@ - pausing Wi-Fi (left \"%@\": %@)", wiredName ?: @"?",
                       ssid ?: @"?", ok ? @"ok" : (detail ?: @"failed"));
            } else {
                RTWLog(@"cable on %@ - Wi-Fi paused (was not linked)", wiredName ?: @"?");
            }
        } else {
            self.cableOverride = NO;
            if (!self.cablePaused) return;
            if (++self.cableGoneTicks < 2) return; /* ~6 s without cable: not a blip */
            [self resumeFromCablePause:@"cable unplugged"];
        }
    } @catch (NSException *exception) {
        RTWLog(@"cable watch exception contained: %@ (%@)", exception.name, exception.reason);
    }
}

- (void)resumeFromCablePause:(NSString *)reason {
    NSString *ssid = self.cablePausedSsid;
    self.cablePaused = NO;
    self.cablePausedSsid = nil;
    self.cableGoneTicks = 0;
    RTWLog(@"Wi-Fi resumed - %@", reason);
    if (self.userDisconnected || !ssid.length || ![self.profiles hasProfileForSsid:ssid]) return;
    self.savedSsid = ssid;
    [self rejoinSavedNow:nil];
}

- (NSView *)connectedStatusViewForStatus:(RTWDriverStatus *)status {
    const CGFloat width = 300.0;
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 40)];

    NSImageView *wifi = [[NSImageView alloc] initWithFrame:NSMakeRect(14, 12, 18, 18)];
    wifi.image = [RTWSignalIcon menuWifiTemplateIcon];
    if (@available(macOS 10.14, *)) wifi.contentTintColor = [NSColor controlAccentColor];
    [view addSubview:wifi];

    NSString *name = status.ssid.length ? status.ssid : @"?";
    NSString *prefix = NSLocalizedString(@"Connected to", nil);
    NSFont *regular = [NSFont menuFontOfSize:13];
    NSFont *bold = [NSFont boldSystemFontOfSize:13];
    NSMutableAttributedString *line1 = [[NSMutableAttributedString alloc]
        initWithString:[prefix stringByAppendingString:@" "]
            attributes:@{NSFontAttributeName: regular, NSForegroundColorAttributeName: [NSColor labelColor]}];
    [line1 appendAttributedString:[[NSAttributedString alloc] initWithString:name
        attributes:@{NSFontAttributeName: bold, NSForegroundColorAttributeName: [NSColor labelColor]}]];

    NSString *ip = [self.driver interfaceIPv4:NULL];
    NSMutableString *detail = [NSMutableString stringWithFormat:@"%@  ·  %d dBm",
        [self bandForChannel:status.channel], status.rssiPercent / 2 - 100];
    if (ip.length) [detail appendFormat:@"  ·  %@", ip];

    NSArray *lines = @[line1, [[NSAttributedString alloc] initWithString:detail
        attributes:@{NSFontAttributeName: [NSFont menuFontOfSize:11],
                     NSForegroundColorAttributeName: [NSColor secondaryLabelColor]}]];
    CGFloat ys[2] = {20, 4};
    for (NSUInteger i = 0; i < 2; i++) {
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(38, ys[i], width - 38 - 12, 16)];
        f.editable = NO; f.selectable = NO; f.bezeled = NO; f.bordered = NO;
        f.drawsBackground = NO; f.usesSingleLineMode = YES;
        f.lineBreakMode = NSLineBreakByTruncatingTail;
        f.attributedStringValue = lines[i];
        [view addSubview:f];
    }
    return view;
}

- (NSString *)statusDetailLine:(nullable RTWDriverStatus *)status {
    if (!status) return NSLocalizedString(@"Driver not reachable", nil);
    return status.lastError.length ? status.lastError : NSLocalizedString(@"Driver not reachable", nil);
}

- (NSString *)titleForEntry:(RTWNetworkEntry *)entry {
    NSString *name = entry.ssid.length ? entry.ssid : NSLocalizedString(@"(hidden)", nil);
    if (entry.joinable) {
        /* The checkmark marks a REMEMBERED network only - a merely selected
         * one must never read as "saved" (field complaint v8.6). v8.7.17:
         * profiles persist across restarts, so ANY saved profile counts,
         * not just this session's active pick. */
        BOOL remembered = [self isKnownSsid:entry.ssid];
        /* mejoras: short text - name · band · signal (· open) ✓ */
        NSMutableString *t = [NSMutableString stringWithFormat:@"%@  ·  %@  ·  %@",
                              name, [self bandForChannel:entry.channel],
                              [self signalWordForDbm:entry.rssiDbm]];
        if (!entry.needsPassphrase) [t appendFormat:@"  ·  %@", NSLocalizedString(@"open", nil)];
        if (remembered) [t appendString:@"  ✓"];
        return t;
    }
    return [NSString stringWithFormat:@"%@  ·  %@", name, entry.securityLabel];
}

/* mejoras helpers for the short text format. */
- (NSString *)bandForChannel:(uint32_t)channel {
    if (channel == 0) return @"?";
    return channel <= 14 ? @"2.4 GHz" : @"5 GHz";
}

- (NSString *)signalWordForDbm:(int32_t)dbm {
    if (dbm >= -67) return NSLocalizedString(@"good signal", nil);
    if (dbm >= -75) return NSLocalizedString(@"fair signal", nil);
    return NSLocalizedString(@"weak signal", nil);
}

#pragma mark Actions

/* 8.8.5: Disconnect / Reconnect USB WiFi (the stock Realtek StatusBarApp
 * item). Disconnect: the kext leaves the network and stops the station
 * (kext 1.1.2 OID), the icon shows the crossed-out glyph, and nothing joins
 * or scans by itself. Reconnect: rejoins the saved network right away with
 * the saved password - the same path as Rejoin. */
- (void)toggleUSBWifi:(nullable NSMenuItem *)sender {
    (void)sender;
    if (!self.userDisconnected) {
        self.userDisconnected = YES;
        [self standDownStartupAutoRejoin:@"you clicked Disconnect USB WiFi"];
        [self clearPendingNetworkSave];
        [self clearJoinFlightWatch];
        self.lastJoinFailureNote = nil;
        self.switchingToSsid = nil;  /* mejoras: cancel a pending switch sweep */
        self.scanDetourSsid = nil;   /* ... and a sweep for the preferred AP */
        self.switchGeneration++;
        RTWLog(@"Disconnect USB WiFi clicked - leaving the network; nothing reconnects by itself until Reconnect USB WiFi");
        [self updateIcon:self.lastStatus];
        dispatch_async(self.workQueue, ^{
            NSString *detail = nil;
            if ([self.driver disconnectFromNetwork:&detail]) {
                RTWLog(@"driver accepted the disconnect");
                /* A kext older than 1.1.2 ACCEPTS and ignores unknown set
                 * OIDs, so "ok" alone proves nothing: if the link is still
                 * up 3 s later the driver did not leave - undo and say so. */
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{
                    if (!self.userDisconnected || !self.lastStatus.linked) return;
                    RTWLog(@"the link is still up 3 s after Disconnect - the installed kext ignores the disconnect command (needs kext 1.1.2 or newer)");
                    self.userDisconnected = NO;
                    [self updateIcon:self.lastStatus];
                    [self alert:NSLocalizedString(@"Disconnect USB WiFi", nil)
                           text:NSLocalizedString(@"Disconnect needs the updated driver - reinstall the kext", nil)];
                });
                return;
            }
            /* An older kext has no disconnect OID: say so and undo, so the
             * menu never claims "disconnected" over a live link. */
            RTWLog(@"disconnect refused by the driver (%@) - Disconnect USB WiFi needs kext 1.1.2 or newer",
                   detail ?: @"?");
            dispatch_async(dispatch_get_main_queue(), ^{
                self.userDisconnected = NO;
                [self updateIcon:self.lastStatus];
                [self alert:NSLocalizedString(@"Disconnect USB WiFi", nil)
                       text:NSLocalizedString(@"Disconnect needs the updated driver - reinstall the kext", nil)];
            });
        });
        return;
    }
    self.userDisconnected = NO;
    RTWLog(@"Reconnect USB WiFi clicked");
    [self updateIcon:self.lastStatus];
    if (self.savedSsid.length && [self.profiles hasProfileForSsid:self.savedSsid]) {
        [self rejoinSavedNow:nil]; /* direct join with the saved password */
    } else {
        [self autoRescan]; /* no saved network: refresh the list, the user picks one */
    }
}

/* 8.8.5: the automatic rejoin paths (settle wait, link drop/flap) used to
 * call rescanNow: - the menu action, which shows a modal "Driver not
 * reachable" alert when the dongle is gone. Unplugging the dongle therefore
 * popped that alert 2 s later (field report 2026-11-22 13:54). Automatic
 * rescans are silent: performScan already logs "scan aborted" when no driver
 * is there, and the poll tick surveys again the moment the dongle is back. */
- (void)autoRescan {
    RTWLog(@"rescan requested (automatic)");
    dispatch_async(self.workQueue, ^{ [self performScan]; });
}

- (void)rescanNow:(nullable NSMenuItem *)sender {
    (void)sender;
    /* mejoras: while linked the kext never sweeps (a sweep would
     * drop the link; it serves a cache that only holds the networks of the
     * joined channel). On the user's click: leave for a moment, do a real
     * sweep, then go back to the same network - unless the user picks
     * another one from the fresh list first. */
    [self.stateLock lock];
    RTWDriverStatus *st = self.lastStatus;
    [self.stateLock unlock];
    if (st.driverReachable && st.linked && st.ssid.length && !self.userDisconnected &&
        !self.joinInFlightSsid.length && !self.scanDetourSsid.length &&
        [self.profiles hasProfileForSsid:st.ssid]) {
        NSString *back = st.ssid;
        self.scanDetourSsid = back;
        [self standDownStartupAutoRejoin:@"full scan requested"];
        RTWLog(@"rescan requested while linked to \"%@\" - leaving for a full sweep, then rejoining", back);
        dispatch_async(self.workQueue, ^{
            NSString *detail = nil;
            BOOL left = [self.driver disconnectFromNetwork:&detail];
            RTWLog(@"leave for full sweep -> %@%@", left ? @"ok" : @"failed",
                   detail ? [NSString stringWithFormat:@" (%@)", detail] : @"");
            [NSThread sleepForTimeInterval:1.5];
            [self performScan];
            /* The kext ends the busy poll on the first node in its tree, and
             * the AP just left is still there: the first read comes back in
             * ~2 ms with only that AP while the radio keeps sweeping (~200 ms
             * per channel, ~36 channels). Field log 2026-11-24 16:21/16:23:
             * "scan done: 1 networks", rejoin 1.5 s later, the 5 GHz APs were
             * never reached. Let the sweep finish, then read the tree again. */
            RTWLog(@"full sweep: waiting for the radio to cover every channel");
            [NSThread sleepForTimeInterval:9.0];
            if (rtlPrintableSsidEqual(self.scanDetourSsid, back)) [self performScan];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self.menu update];
                /* The user may already have picked another network (that
                 * join clears scanDetourSsid). Otherwise go back. */
                if (!rtlPrintableSsidEqual(self.scanDetourSsid, back)) return;
                self.scanDetourSsid = nil;
                if (self.userDisconnected) return; /* clicked Disconnect meanwhile */
                self.savedSsid = back;
                RTWLog(@"full sweep done - rejoining \"%@\"", back);
                [self rejoinSavedNow:nil];
            });
        });
        return;
    }
    RTWLog(@"rescan requested");
    dispatch_async(self.workQueue, ^{
        if (![self.driver isConnected]) {
            NSString *detail = nil;
            if (![self.driver openConnection:&detail]) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self alert:NSLocalizedString(@"Driver not reachable", nil) text:detail ?: NSLocalizedString(@"No RtWlanU service found. Load the macOS-RTL-WIFI kext or plug the dongle in.", nil)];
                });
                return;
            }
        }
        [self performScan];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.menu update];
        });
    });
}

- (void)joinFromMenu:(NSMenuItem *)sender {
    RTWNetworkEntry *entry = sender.representedObject;
    if (!entry || !entry.joinable) {
        /* v8.7.10: never swallow a click silently again. The 8.7.9 field
         * session shows a two-minute gap with no join submit and no log
         * line - the user read that as "the app only say selected". A
         * click that cannot join must at least tell the log why. */
        RTWLog(@"menu click ignored - \"%@\" has no joinable scan record",
               entry.ssid ?: @"(no scan record)");
        return;
    }
    if (![self.driver isConnected]) {
        [self alert:NSLocalizedString(@"Driver not reachable", nil) text:NSLocalizedString(@"No user client connection. Rescan and try again.", nil)];
        return;
    }

    /* mejoras: a row picked in the menu is a specific AP - pin it. */
    entry.pinBssid = YES;
    BOOL siblingPick = [self isSiblingAccessPoint:entry];
    [self setUserPickedAP:[self bssidHexForEntry:entry] forSsid:entry.ssid];

    /* v8.7.17: the user is joining a network themselves - the armed
     * startup auto-rejoin stands down immediately (same network: the
     * manual join is doing the job; different network: the user chose). */
    if (self.autoRejoinArmedSsid.length) {
        [self standDownStartupAutoRejoin:
            rtlPrintableSsidEqual(entry.ssid, self.autoRejoinArmedSsid)
                ? @"you picked the same network yourself"
                : @"you picked a different network"];
    }

    NSString *pass = nil;
    BOOL usedSavedPassword = NO;
    if (entry.needsPassphrase) {
        /* v8.7.7 rule (always prompt) replaced on 2026-11-22 (the user's
         * decision: a user switching between home Wi-Fi and a phone hotspot
         * had to retype both passwords every time): a network that already
         * has a saved password joins with it - no prompt. The prompt stays
         * for networks never saved here, and after Forget (which removes
         * the profile) it asks again, which is also the way to correct a
         * password that changed. */
        NSString *saved = [self.profiles hasProfileForSsid:entry.ssid]
            ? [self.profiles passphraseForSsid:entry.ssid] : nil;
        if (saved.length) {
            pass = saved;
            usedSavedPassword = YES;
            RTWLog(@"joining \"%@\" with its saved password (no prompt; Forget it to type a new one)",
                   entry.ssid);
        } else {
            pass = [self promptPassphrase:entry.ssid];
            if (!pass) { /* cancelled */
                RTWLog(@"password prompt cancelled for %@", entry.ssid);
                return;
            }
        }
        /* v8.7.10 (the user's demand: "SAVE IS GONE we have in earlier
         * build"): the profile is written the moment the password is
         * confirmed - exactly the earlier-build behaviour - NOT only after
         * the link comes up. The v8.7.7 link-up gate turned every
         * association hiccup into a permanent dead "selected" state: no
         * profile, no Rejoin, only Forget. Saving on confirm makes the
         * network read "saved" immediately and keeps Forget available; a
         * wrong password is overwritten by retyping it or removed with
         * Forget. v8.7.17: the profile now PERSISTS - the launch reset is
         * gone, and the most recently used saved network is re-pinned and
         * auto-rejoined once after the next restart. The link-up commit
         * below stays as the confirmation stamp. */
        [self.profiles rememberNetwork:entry.ssid
                             passphrase:pass
                               authMode:entry.authMode
                                channel:entry.channel];
        [self markKnownSsid:entry.ssid];
    }
    /* Selecting a network makes it the active one: pinned on top of the
     * collapsed menu for this session. Nothing joins automatically -
     * the submit below is the user's own click (v8.7.13). */
    [self armPendingNetworkSaveForEntry:entry passphrase:pass];
    [self markActiveSsid:entry.ssid];

    /* mejoras: switching from a linked network to a different one.
     * Joining on top of a live link did not always move (the user had to
     * Forget or unplug/replug). Leave the current network first (kext 1.1.2
     * disassociate OID), then join the new one 2 s later. The drop that the
     * leave causes must not arm the auto-rejoin to the old network. */
    [self.stateLock lock];
    RTWDriverStatus *now = self.lastStatus;
    [self.stateLock unlock];
    /* mejoras: the kext can only pin an AP that is in its node
     * tree; otherwise it joins by name and lands on the strongest AP (field
     * 2026-11-24 17:00: CH 36 picked while a Reconnect was still joining,
     * the tree only had the two CH 11 APs, it stayed on CH 11). Such a pick
     * takes the same leave -> sweep -> join path, linked or not. */
    [self.stateLock lock];
    BOOL apInDriver = [self.driverSurveyKeys containsObject:[self surveyKeyForEntry:entry]];
    [self.stateLock unlock];
    BOOL needSweep = !apInDriver;
    if ((now.linked && now.ssid.length &&
         (!rtlPrintableSsidEqual(now.ssid, entry.ssid) || siblingPick)) || needSweep) {
        RTWLog(@"switching from \"%@\" to \"%@\" - leaving the current network first%@",
               now.ssid.length ? now.ssid : @"(not linked)", entry.ssid,
               needSweep ? @" (the picked AP is not in the driver's survey)" : @"");
        [self clearJoinFlightWatch]; /* a running join would block the sweep */
        self.switchingToSsid = entry.ssid;
        [self standDownStartupAutoRejoin:@"you switched to another network"];
        NSString *leaveDetail = nil;
        BOOL left = [self.driver disconnectFromNetwork:&leaveDetail];
        RTWLog(@"leave before switch -> %@%@", left ? @"ok" : @"failed",
               leaveDetail ? [NSString stringWithFormat:@" (%@)", leaveDetail] : @"");
        /* While linked the kext never sweeps, so its node tree only holds
         * the AP we were on; a pin to an AP it does not know falls back to
         * a join by name and lands on the strongest AP (field 2026-11-24
         * 16:35: ch 36 picked, stayed on ch 44). Sweep every channel first
         * (same wait as "Scan now" while linked), then join the pinned AP.
         * A newer pick cancels this one (switchGeneration). */
        NSUInteger generation = ++self.switchGeneration;
        RTWLog(@"switch: sweeping every channel before joining the picked AP (ch %u)", entry.channel);
        __weak typeof(self) weakSelf = self;
        dispatch_async(self.workQueue, ^{
            [NSThread sleepForTimeInterval:1.5];
            typeof(self) s = weakSelf;
            if (!s || s.switchGeneration != generation) return;
            [s performScan];
            /* Poll the survey (no new trigger - that restarts the sweep)
             * until the picked AP shows up, at most ~9 s. */
            NSString *want = [s surveyKeyForEntry:entry];
            for (int i = 0; i < 13; i++) {
                [NSThread sleepForTimeInterval:0.7];
                if (s.switchGeneration != generation) return;
                BOOL seen = NO;
                for (RTWNetworkEntry *e in [s.driver surveyRecords:NULL] ?: @[])
                    if ([[s surveyKeyForEntry:e] isEqualToString:want]) { seen = YES; break; }
                if (seen) { RTWLog(@"switch: picked AP seen after %.1f s of sweep", (i + 1) * 0.7); break; }
            }
            [s performScan];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (s.switchGeneration != generation ||
                    !rtlPrintableSsidEqual(s.switchingToSsid, entry.ssid)) return;
                [s.menu update];
                [s submitJoin:entry passphrase:pass];
                if (usedSavedPassword && s.joinInFlightSsid.length) s.joinWatchSavedPass = YES;
            });
        });
        return;
    }

    [self submitJoin:entry passphrase:pass];
    /* After submitJoin: it arms the join watch (and a rejected submit clears
     * it), so the flag only lives with a watch that is really running. */
    if (usedSavedPassword && self.joinInFlightSsid.length) self.joinWatchSavedPass = YES;
}

/* 2026-11-22: the saved password did not get through the WPA handshake -
 * say so and let the user type the new one. Cancel leaves everything as is
 * (the menu already shows the failure note). The typed password is saved
 * like any other manual join, replacing the old one. */
- (void)offerNewPasswordForEntry:(RTWNetworkEntry *)entry {
    if (!entry.ssid.length || self.userDisconnected || self.joinInFlightSsid.length) return;
    [self.stateLock lock];
    RTWDriverStatus *status = self.lastStatus;
    [self.stateLock unlock];
    if (status && status.linked) return;
    RTWLog(@"saved password for \"%@\" was not accepted - asking for the current one", entry.ssid);
    [self alert:[NSString stringWithFormat:NSLocalizedString(@"Could not join \"%@\"", nil), entry.ssid]
           text:NSLocalizedString(@"The saved password was not accepted. If the password changed (a phone hotspot or a new router key), type the new one in the next window.", nil)];
    NSString *pass = [self promptPassphrase:entry.ssid];
    if (!pass) {
        RTWLog(@"new password prompt cancelled for %@", entry.ssid);
        return;
    }
    [self.profiles rememberNetwork:entry.ssid
                         passphrase:pass
                           authMode:entry.authMode
                            channel:entry.channel];
    [self markKnownSsid:entry.ssid];
    [self armPendingNetworkSaveForEntry:entry passphrase:pass];
    [self markActiveSsid:entry.ssid];
    [self submitJoin:entry passphrase:pass];
}

- (void)markKnownSsid:(NSString *)ssid {
    if (!ssid.length || [self.knownSsids containsObject:ssid]) return;
    self.knownSsids = [self.knownSsids arrayByAddingObject:ssid];
}

- (void)markActiveSsid:(NSString *)ssid {
    if (!ssid.length) return;
    /* v8.7.13: session memory only. The pin is never written to
     * NSUserDefaults anymore - the launch reset removes the key, so no
     * selection can survive a restart. */
    self.savedSsid = ssid;
}

/* v8.7.10: a manual join writes the profile at password-confirm time
 * (joinFromMenu:). The armed pending save stays as the link-up
 * confirmation stamp: when the monitor sees the link it stamps the same
 * values again and logs "connection established". Rejoin never arms it -
 * it consumes what the user established. v8.7.13: everything here lives
 * for the current session only; the launch reset wipes Profiles.plist.
 * The password lands in the app's own profile file, never the Keychain. */
- (void)armPendingNetworkSaveForEntry:(RTWNetworkEntry *)entry
                            passphrase:(nullable NSString *)pass {
    self.pendingSaveSsid = entry.ssid ?: @"";
    self.pendingSavePass = pass ?: @"";
    self.pendingSaveAuth = entry.authMode;
    self.pendingSaveChannel = entry.channel;
}

- (void)commitPendingNetworkSave {
    if (!self.pendingSaveSsid.length) return;
    NSString *ssid = self.pendingSaveSsid;
    [self.profiles rememberNetwork:ssid
                        passphrase:self.pendingSavePass
                          authMode:self.pendingSaveAuth
                           channel:self.pendingSaveChannel];
    [self markKnownSsid:ssid];
    [self markActiveSsid:ssid];
    self.rescueRejoinsUsed = 0; /* v8.8.3: a good link earns a fresh rescue budget */
    RTWLog(@"connection established - network \"%@\" saved (the app reconnects to it by itself after a restart; Forget removes it)", ssid);
    RTWLog(@"driver diagnostics at connect: %@", [self.driver diagnosticSummary]);
    [self clearPendingNetworkSave];
}

- (void)clearPendingNetworkSave {
    self.pendingSaveSsid = nil;
    self.pendingSavePass = nil;
}

- (void)rejoinSavedNow:(nullable NSMenuItem *)sender {
    (void)sender;
    if (!self.savedSsid.length) return;
    if (![self.profiles hasProfileForSsid:self.savedSsid]) {
        [self alert:NSLocalizedString(@"Not a remembered network", nil)
                 text:NSLocalizedString(@"Select the network in the Networks list and enter its password. It stays saved and the app reconnects to it by itself after a restart; Forget removes it.", nil)];
        return;
    }
    NSString *pass = [self.profiles passphraseForSsid:self.savedSsid];
    /* Open networks rejoin without a key stream; PSK classes use the
 * stored passphrase and the auth mode saved with the profile. */
    RTWNetworkEntry *target = [[RTWNetworkEntry alloc] initWithSsid:self.savedSsid
                                                            authMode:[self.profiles authModeForSsid:self.savedSsid]
                                                      needsPassphrase:(pass.length > 0)];
    /* mejoras: preferred AP (picked / 5 GHz) instead of by name.
     * The saved-password retype flag (2026-11-22) is set inside. */
    [self joinSavedEntry:target passphrase:pass automatic:NO];
}

- (void)submitJoin:(RTWNetworkEntry *)entry passphrase:(nullable NSString *)pass {
    /* 8.8.5: any join the user starts (a network picked in the menu, Rejoin,
     * Reconnect USB WiFi) ends the Disconnect state. */
    self.userDisconnected = NO;
    self.switchingToSsid = nil; /* mejoras: the switch is now a normal join */
    self.scanDetourSsid = nil;  /* mejoras: a join ends the full-sweep detour */
    if (self.cablePaused) {     /* mejoras: the user joins with the cable in - respect it */
        self.cablePaused = NO;
        self.cableOverride = YES;
        RTWLog(@"join while paused for the cable - Wi-Fi stays on until the cable is unplugged");
    }
    /* v8.7.13: the 20 s "driver settle" gate is REVERTED - it was the
     * timer fix the user rejected. The kext now restarts its station and
     * clears a halted RX pipe on every join request (driver 1.0.7), so a
     * join submitted right after the client opens starts the same clean
     * association path as any other. No timer decides anything here. */
    /* Arm the join watch BEFORE the driver call so any poll tick running
     * alongside this submit sees the in-flight window and the monitor
     * owns the tick (no duplicate stream mid-handshake). */
    [self noteJoinSubmittedForEntry:entry passphrase:pass];
    NSString *detail = nil;
    BOOL ok = [self.driver joinNetwork:entry passphrase:pass error:&detail];
    RTWLog(@"join submit for %@ -> %@%@", entry.ssid, ok ? @"ok" : @"failed",
           detail ? [NSString stringWithFormat:@" (%@)", detail] : @"");
    if (ok && pass.length > 0 && pass.length < 8) {
        RTWLog(@"NOTE: join submitted with a %lu-character key - WPA2 keys are 8-63 characters; if the link keeps dropping, re-check the password for a missing character",
               (unsigned long)pass.length);
    }
    if (ok) {
        self.joinAttemptFailures = 0;
    } else {
        [self clearPendingNetworkSave]; /* v8.7.7: failed join never saves */
        [self clearJoinFlightWatch];
        /* v8.7.14: a rejected submit is a REAL failure - show it, keep
         * it on the menu, never let it look like nothing happened. */
        self.lastJoinFailureNote = [NSString stringWithFormat:
            @"Join to \"%@\" failed - %@", entry.ssid,
            detail ?: @"the driver rejected the association stream."];
        [self alert:NSLocalizedString(@"Join failed", nil) text:detail ?: NSLocalizedString(@"The driver rejected the association stream.", nil)];
    }
}

- (void)forgetNetwork:(nullable NSMenuItem *)sender {
    (void)sender;
    /* v8.7.13: the forget target is the active selection of this session.
     * Forgetting always clears the selection, the profile and any pin
     * leftover from older builds in NSUserDefaults. */
    NSString *ssid = self.savedSsid;
    if (!ssid.length) return;
    /* v8.7.17: forgetting the armed network also cancels its startup
     * auto-rejoin - a forgotten network must never be rejoined. */
    if (self.autoRejoinArmedSsid.length &&
        rtlPrintableSsidEqual(self.autoRejoinArmedSsid, ssid)) {
        [self standDownStartupAutoRejoin:@"you forgot this network"];
    }
    if ([self.profiles hasProfileForSsid:ssid]) {
        [self.profiles forgetSsid:ssid];
    }
    /* v8.7.6: plain removal instead of NSPredicate ("SELF != %@") - same
     * parser-crash class as the auto-join filter, same ban. */
    NSMutableArray<NSString *> *remaining = [NSMutableArray arrayWithArray:self.knownSsids];
    [remaining removeObject:ssid];
    self.knownSsids = remaining;
    self.savedSsid = self.knownSsids.firstObject;
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kDefaultsActiveSsid];
    [self setUserPickedAP:nil forSsid:ssid]; /* mejoras */
    RTWLog(@"forgot network %@ - selection and pin cleared", ssid);
}

- (void)showAbout:(nullable NSMenuItem *)sender {
    (void)sender;
    [NSApp activateIgnoringOtherApps:YES];
    /* About pane wording (requested 2026-11-19): "Version 8.8 (N)" plus the
     * chris1111 copyright line. 1.2.2: derived from Info.plist instead of
     * hardcoded (it stayed on "8.7 (18)"): CFBundleShortVersionString
     * "major.minor.N" shows as "major.minor (N)". */
    NSDictionary *info = [[NSBundle mainBundle] infoDictionary];
    NSString *shortVersion = info[@"CFBundleShortVersionString"] ?: @"?";
    NSString *aboutVersion = shortVersion;
    NSString *aboutBuild = info[@"CFBundleVersion"] ?: @"";
    NSArray<NSString *> *versionParts = [shortVersion componentsSeparatedByString:@"."];
    if (versionParts.count >= 3) {
        aboutVersion = [NSString stringWithFormat:@"%@.%@", versionParts[0], versionParts[1]];
        aboutBuild = versionParts[2];
    }
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        @"ApplicationName": @"RTLWiFiBar",
        @"ApplicationVersion": aboutVersion,
        @"Version": aboutBuild,
        @"Copyright": @"Copyright © 2026 chris1111, All Right Reserved.",
    }];
}

- (void)quitApp:(nullable NSMenuItem *)sender {
    (void)sender;
    [NSApp terminate:nil];
}

#pragma mark Dialogs

- (nullable NSString *)promptPassphrase:(NSString *)ssid {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = [NSString stringWithFormat:NSLocalizedString(@"Password for \"%@\"", nil), ssid];
    alert.informativeText = NSLocalizedString(@"Enter the router password to establish the connection. The password is stored in the macOS Keychain and the app reconnects to this network by itself after a restart; use Forget in the menu to remove it.", nil);
    alert.alertStyle = NSAlertStyleInformational;
    [alert addButtonWithTitle:NSLocalizedString(@"Join", nil)];
    [alert addButtonWithTitle:NSLocalizedString(@"Cancel", nil)];

    /* AppKit's NSSecureTextField has no runtime "secure" toggle (that is
     * an iOS UITextField property - the v8.7 build broke on '.secure').
     * Show password therefore swaps between a secure field and a plain
     * twin at the same frame; both stay value-synced, so the final value
     * is read from whichever field is visible when the dialog closes.
     * v8.7.8: the accessory is laid out with real autolayout constraints.
     * The old fixed-frame accessory dropped the checkbox on some systems
     * (field report: "Show password missing") because AppKit sizes alert
     * accessories from constraints, not from fixed frames. Same behaviour,
     * solid layout - the box is always there and always works. */
    NSSecureTextField *field = [[NSSecureTextField alloc] init];
    field.placeholderString = NSLocalizedString(@"Passphrase (8..63 characters)", nil);

    NSTextField *plainField = [[NSTextField alloc] init];
    plainField.placeholderString = NSLocalizedString(@"Passphrase (8..63 characters)", nil);
    plainField.hidden = YES;

    NSButton *showPassword = [[NSButton alloc] init];
    showPassword.title = NSLocalizedString(@"Show password", nil);
    showPassword.buttonType = NSButtonTypeSwitch;
    showPassword.state = NSControlStateValueOff;
    showPassword.target = self;
    showPassword.action = @selector(toggleShowPassword:);
    showPassword.toolTip = NSLocalizedString(@"Show the passphrase while typing", nil);

    NSView *accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 54)];
    field.translatesAutoresizingMaskIntoConstraints = NO;
    plainField.translatesAutoresizingMaskIntoConstraints = NO;
    showPassword.translatesAutoresizingMaskIntoConstraints = NO;
    [accessory addSubview:field];
    [accessory addSubview:plainField];
    [accessory addSubview:showPassword];
    [NSLayoutConstraint activateConstraints:@[
        [field.topAnchor constraintEqualToAnchor:accessory.topAnchor],
        [field.leadingAnchor constraintEqualToAnchor:accessory.leadingAnchor],
        [field.trailingAnchor constraintEqualToAnchor:accessory.trailingAnchor],
        [field.heightAnchor constraintEqualToConstant:24],
        [plainField.topAnchor constraintEqualToAnchor:accessory.topAnchor],
        [plainField.leadingAnchor constraintEqualToAnchor:accessory.leadingAnchor],
        [plainField.trailingAnchor constraintEqualToAnchor:accessory.trailingAnchor],
        [plainField.heightAnchor constraintEqualToConstant:24],
        [showPassword.leadingAnchor constraintEqualToAnchor:accessory.leadingAnchor],
        [showPassword.topAnchor constraintEqualToAnchor:field.bottomAnchor constant:6],
        [showPassword.heightAnchor constraintEqualToConstant:22],
        [accessory.bottomAnchor constraintGreaterThanOrEqualToAnchor:showPassword.bottomAnchor],
    ]];
    alert.accessoryView = accessory;
    alert.window.initialFirstResponder = field;
    self.pendingPassField = field;
    self.pendingPlainField = plainField;

    [NSApp activateIgnoringOtherApps:YES];
    NSInteger button = [alert runModal];
    /* Commit any uncommitted edit in the visible field before reading it. */
    [alert.window endEditingFor:nil];
    NSString *value = plainField.hidden ? field.stringValue : plainField.stringValue;
    self.pendingPassField = nil;
    self.pendingPlainField = nil;
    if (button != NSAlertFirstButtonReturn) return nil;
    return value;
}

/* Show password: swap the secure field for its plain twin (and back),
 * syncing the value both ways on every flip. */
- (void)toggleShowPassword:(NSButton *)sender {
    NSSecureTextField *secure = self.pendingPassField;
    NSTextField *plain = self.pendingPlainField;
    if (!secure || !plain) return;
    BOOL show = (sender.state == NSControlStateValueOn);
    if (show) {
        plain.stringValue = secure.stringValue;
        plain.hidden = NO;
        secure.hidden = YES;
        [secure.window makeFirstResponder:plain];
    } else {
        secure.stringValue = plain.stringValue;
        plain.hidden = YES;
        secure.hidden = NO;
        [secure.window makeFirstResponder:secure];
    }
}

- (void)revealLog:(nullable NSMenuItem *)sender {
    (void)sender;
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:
                      @"Library/Logs/RTLWiFiBar.log"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path]) {
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:
            @[ [NSURL fileURLWithPath:path] ]];
    } else {
        /* No log yet: open the Logs folder so it is one click away. */
        [[NSWorkspace sharedWorkspace] openURL:
            [NSURL fileURLWithPath:path.stringByDeletingLastPathComponent]];
    }
    RTWLog(@"log revealed in Finder");
}

- (void)alert:(NSString *)title text:(NSString *)text {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = text ?: @"";
    alert.alertStyle = NSAlertStyleInformational;
    [alert runModal];
}

@end
