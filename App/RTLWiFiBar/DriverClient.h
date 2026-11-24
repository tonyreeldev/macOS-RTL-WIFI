/*
 * DriverClient.h - RTLWiFiBar user-mode client for the macOS-RTL-WIFI kext.
 *
 * Speaks the Realtek utility compatibility protocol (see RTWCompat.h), the
 * same one the stock StatusBarApp utility uses, so every call here is legal
 * for the logged-in console user - no root, no helper. See the kext's
 * Driver/RTWUserClient.cpp for the privilege model.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import <Foundation/Foundation.h>
#import "RTWCompat.h"

NS_ASSUME_NONNULL_BEGIN

/* One scan result, decoded from a 0x640 survey record. */
@interface RTWNetworkEntry : NSObject
@property (nonatomic, copy, readonly) NSString *ssid;
@property (nonatomic, assign, readonly) uint32_t channel;
@property (nonatomic, assign, readonly) int32_t rssiPercent;   /* 0..100 */
@property (nonatomic, assign, readonly) int32_t rssiDbm;       /* approx dBm */
@property (nonatomic, assign, readonly) uint8_t authMode;      /* utility auth enum */
@property (nonatomic, assign, readonly) BOOL joinable;         /* open or WPA/WPA2 PSK */
@property (nonatomic, assign, readonly) BOOL needsPassphrase;  /* secured PSK class */
@property (nonatomic, copy, readonly) NSString *securityLabel; /* "Open"/"WPA2-PSK"... */
- (nullable instancetype)initWithSurveyRecord:(const uint8_t *)record;
- (nullable instancetype)initWithSsid:(NSString *)ssid
                             authMode:(uint8_t)authMode
                       needsPassphrase:(BOOL)needsPassphrase; /* synthetic join target */
- (void)getBSSID:(uint8_t *)out6; /* writes 6 bytes */
/* mejoras: YES = join exactly this AP (the user picked this row).
 * Automatic rejoins and re-submits leave it NO and join by SSID. */
@property (nonatomic, assign) BOOL pinBssid;
@end

/* Live driver snapshot. */
@interface RTWDriverStatus : NSObject
@property (nonatomic, assign, readonly) BOOL driverReachable; /* user client open */
@property (nonatomic, assign, readonly) BOOL linked;          /* net80211 RUN + WPA keys installed */
/* 8.8.5: associated (net80211 RUN) but the WPA 4-way handshake has not
 * installed the keys yet - no data can pass. linked is NO while this is YES. */
@property (nonatomic, assign, readonly) BOOL securing;
@property (nonatomic, copy, readonly, nullable) NSString *ssid;
@property (nonatomic, assign, readonly) uint32_t channel;
@property (nonatomic, assign, readonly) int32_t rssiPercent;  /* 0..100, 0 unknown */
@property (nonatomic, copy, readonly, nullable) NSString *driverVersion;
@property (nonatomic, copy, readonly) NSString *lastError;    /* human readable */
@end

typedef NS_ENUM(NSInteger, RTWDriverError) {
    RTWDriverErrorNone = 0,
    RTWDriverErrorServiceMissing,   /* no RtWlanU service: kext not loaded / dongle out */
    RTWDriverErrorOpenFailed,       /* IOServiceOpen failed */
    RTWDriverErrorCallFailed,       /* an OID call failed */
    RTWDriverErrorNotPrivileged,    /* kIOReturnNotPrivileged from a call */
};

extern NSString * const RTWDriverErrorDomain;

@interface RTWDriverClient : NSObject

/* Connection state ------------------------------------------------------- */
@property (nonatomic, readonly, getter=isConnected) BOOL connected;
@property (nonatomic, readonly) RTWDriverError lastError;
@property (nonatomic, readonly, copy) NSString *lastErrorDetail;
@property (nonatomic, readonly, copy, nullable) NSString *adapterMAC;
/* Timestamp of the last successful user-client open (0 = never opened or
 * currently closed). Joins inside the first ~20 s after an open wedge the
 * kext's association state, so callers gate on this. */
@property (nonatomic, readonly) NSTimeInterval openedAt;
/* YES when the most recent pollStatus hit an OID failure (client dying,
 * dongle swap, kext reload). Callers count consecutive failures and
 * reopen the client. */
@property (nonatomic, readonly) BOOL lastPollFailed;

- (BOOL)openConnection:(NSString * _Nullable * _Nullable)errorDetail;
- (void)closeConnection;

/* Status (1 Hz poll budget: 1 query idle, 3 linked) ---------------------- */
- (nullable RTWDriverStatus *)pollStatus;

/* Scan ------------------------------------------------------------------- */
- (BOOL)triggerScan:(NSString * _Nullable * _Nullable)errorDetail;
- (BOOL)scanBusy:(NSString * _Nullable * _Nullable)errorDetail;
- (nullable NSArray<RTWNetworkEntry *> *)surveyRecords:(NSString * _Nullable * _Nullable)errorDetail;

/* Association ------------------------------------------------------------- */
/* Streams the Realtek association OIDs: ssid set -> auth set -> key set;
 * the kext's compat layer auto-joins once the state is complete and derives
 * the PMK from the passphrase itself (4096-round PBKDF2 in-kernel). */
- (BOOL)joinNetwork:(RTWNetworkEntry *)entry
         passphrase:(nullable NSString *)passphrase
              error:(NSString * _Nullable * _Nullable)errorDetail;

/* 8.8.5: "Disconnect USB WiFi" - the kext leaves the network, stops the
 * station and erases its profile (kext 1.1.2 compat OID 0x0D010115). An
 * older kext refuses the OID: NO with the error in errorDetail. */
- (BOOL)disconnectFromNetwork:(NSString * _Nullable * _Nullable)errorDetail;

/* Diagnostics ------------------------------------------------------------- */
- (nullable NSString *)copyDriverVersion:(NSString * _Nullable * _Nullable)errorDetail;
/* v8.7.3: last deauth/disassoc reason the kext recorded (its IORegistry
 * property RTWDiagWPADeauthReason, refreshed on every status poll). 0 =
 * unknown / never set. Read at the moment a link drops, this tells the app
 * WHO ended the association: the AP (wire reason 1-45) or a local reset
 * (stale value unchanged since link-up, or nothing published). */
- (uint32_t)lastDeauthReason;
/* 8.7.20: one-line dump of the kext's diagnostic IORegistry properties
 * (RTWDiag*, RTWSnap_*, RTWEfuse88E, RTWResidual, ...) for the log. */
- (NSString *)diagnosticSummary;
/* 8.8.5: "Key=value" for exactly these RTWDiag keys, zeros included
 * ("Key=-" when the driver does not publish it). Short, for link checks. */
- (NSString *)diagnosticValuesForKeys:(NSArray<NSString *> *)keys;
/* 8.8.5: the dongle's BSD interface (en2, en7, ...) and its routable IPv4
 * address. Returns nil when the interface has no IPv4 or only a
 * self-assigned 169.254.x.x one. bsdName gets the interface name if known. */
- (nullable NSString *)interfaceIPv4:(NSString * _Nullable * _Nullable)bsdName;
/* v8.7.14: net80211 station state + beacon age while a join is in flight
 * (kext 1.0.8 OID 0x00010115). States: 0 INIT, 1 SCAN, 2 AUTH, 3 ASSOC,
 * 4 RUN. Returns NO when the driver build predates the OID or the call
 * fails - callers then fall back to the time window alone. Silent: a NO
 * here never touches lastError/lastErrorDetail (the 1 Hz join monitor
 * would otherwise flood the log on an older driver). */
- (BOOL)queryJoinState:(uint32_t * _Nullable)state
           beaconAgeMs:(uint32_t * _Nullable)beaconAge;

/* v8.7.4: YES when a Realtek USB device (idVendor 0x0bda) is visible on
 * the USB bus, or an RtWlanU interface is already matched. Read-only, no
 * user client, safe from any queue. Lets the app tell "dongle unplugged"
 * apart from "dongle there, driver not loaded" without any external tool. */
+ (BOOL)rtlUsbDonglePresent;

@end

NS_ASSUME_NONNULL_END
