/*
 * DriverClient.m - Realtek compat-protocol client implementation.
 *
 * Wire notes (verified against Driver/RtWlanUCompat.cpp):
 *  - Selector 9/10 (OID query/set): IOConnectCallStructMethod with a 0x9D4
 *    frame in AND out. The kext requires exactly these sizes.
 *  - Selector 0 (survey record): IOConnectCallMethod, one scalar input
 *    (record index) and a >= 0x640 structure output. Table ends with
 *    kIOReturnNotFound.
 *  - Queries want frame.length >= the answer size; we always pass the full
 *    0x9C4 capacity.
 *  - The join stream resets its pending state on the 0xFF070102 ssid set,
 *    so the order is: ssid -> auth -> key; the kext auto-joins on key set.
 *    An explicit 0xFF030004 trigger afterwards is a benign no-op then, and
 *    rescues the stream if ordering ever changed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import "DriverClient.h"
#import "DriverClientPrivate.h"
#import "RTWLog.h"
#import <IOKit/IOKitLib.h>
#import <string.h>
#import <ifaddrs.h>
#import <net/if.h>
#import <netinet/in.h>
#import <arpa/inet.h>

NSString * const RTWDriverErrorDomain = @"com.rtlwifi.RTLWiFiBar.driver";

/* kIOMainPortDefault exists from macOS 12 SDKs; fall back for older ones. */
#ifdef kIOMainPortDefault
#  define RTW_MASTER_PORT kIOMainPortDefault
#elif defined(kIOMasterPortDefault)
#  define RTW_MASTER_PORT kIOMasterPortDefault
#else
#  define RTW_MASTER_PORT ((mach_port_t)0)
#endif

#pragma mark - RTWNetworkEntry

@interface RTWNetworkEntry ()
@property (nonatomic, copy, readwrite) NSString *ssid;
@property (nonatomic, assign, readwrite) uint32_t channel;
@property (nonatomic, assign, readwrite) int32_t rssiPercent;
@property (nonatomic, assign, readwrite) int32_t rssiDbm;
@property (nonatomic, assign, readwrite) uint8_t authMode;
@property (nonatomic, assign, readwrite) BOOL joinable;
@property (nonatomic, assign, readwrite) BOOL needsPassphrase;
@property (nonatomic, copy, readwrite) NSString *securityLabel;
@end

@implementation RTWNetworkEntry {
    uint8_t _bssid[6];
}

- (nullable instancetype)initWithSurveyRecord:(const uint8_t *)record {
    self = [super init];
    if (!self) return nil;

    uint8_t ssidLength = record[RTWCOMPAT_REC_SSIDLEN_OFFSET];
    if (ssidLength > RTWCOMPAT_MAX_SSID) ssidLength = RTWCOMPAT_MAX_SSID;
    /* v8.7.9: trim the fixed-width survey field at the first control byte
     * (NUL/0x01 padding - the reference file's documented trap) so the
     * menu, the join target and the saved profile all carry the printable
     * name. UTF-8 bytes >= 0x80 stay intact. */
    uint32_t printable = 0;
    while (printable < ssidLength &&
           record[RTWCOMPAT_REC_SSID_OFFSET + printable] >= 0x20 &&
           record[RTWCOMPAT_REC_SSID_OFFSET + printable] != 0x7f) printable++;
    ssidLength = (uint8_t)printable;
    char ssid[RTWCOMPAT_MAX_SSID + 1];
    memset(ssid, 0, sizeof(ssid));
    memcpy(ssid, record + RTWCOMPAT_REC_SSID_OFFSET, ssidLength);
    _ssid = [[NSString alloc] initWithBytes:ssid
                                     length:ssidLength
                                   encoding:NSUTF8StringEncoding];
    if (!_ssid) _ssid = @"";

    memcpy(_bssid, record + RTWCOMPAT_REC_BSSID_OFFSET, 6);
    _channel = RTWCompatLoad16(record + RTWCOMPAT_REC_CHANNEL_OFFSET);
    _rssiPercent = (int32_t)record[RTWCOMPAT_REC_SIGNALSTRENGTH_OFFSET];
    _rssiDbm = _rssiPercent / 2 - 100; /* inverse of the survey scale */
    _authMode = record[RTWCOMPAT_REC_AUTH_OFFSET];

    /* Mirror the vendor join-dialog classifier (field round 9 ladder):
     * PSK classes are joinable, enterprise/WEP/SAE/adhoc are not. */
    const uint8_t siteEncry = record[RTWCOMPAT_REC_SITE_ENCRY_OFFSET];
    const uint32_t akm = RTWCompatLoad32(record + RTWCOMPAT_REC_AKMSUIT_OFFSET);
    if (siteEncry == 0) {
        _securityLabel = @"Open";
        _joinable = YES;
        _needsPassphrase = NO;
    } else if (siteEncry == 1) {
        _securityLabel = @"WEP (unsupported)";
        _joinable = NO;
        _needsPassphrase = NO;
    } else if (siteEncry >= 0x40) {
        if (akm & RTWCOMPAT_AKM_WPA2_PSK) {
            _securityLabel = @"WPA2-PSK (AES)";
            _joinable = YES; _needsPassphrase = YES;
        } else if (akm & RTWCOMPAT_AKM_SAE) {
            _securityLabel = @"WPA3-SAE (unsupported)";
            _joinable = NO; _needsPassphrase = NO;
        } else if (akm & RTWCOMPAT_AKM_WPA2_1X) {
            _securityLabel = @"WPA2-Enterprise (unsupported)";
            _joinable = NO; _needsPassphrase = NO;
        } else {
            _securityLabel = @"WPA2 (unsupported class)";
            _joinable = NO; _needsPassphrase = NO;
        }
    } else if (siteEncry >= 0x20) {
        if (akm & RTWCOMPAT_AKM_WPA2_PSK) {
            _securityLabel = @"WPA2-PSK (TKIP)";
            _joinable = YES; _needsPassphrase = YES;
        } else {
            _securityLabel = @"WPA2 (unsupported class)";
            _joinable = NO; _needsPassphrase = NO;
        }
    } else if (siteEncry >= 4) {
        if (akm & RTWCOMPAT_AKM_WPA_PSK) {
            _securityLabel = @"WPA-PSK (AES)";
            _joinable = YES; _needsPassphrase = YES;
        } else if (akm & RTWCOMPAT_AKM_WPA_1X) {
            _securityLabel = @"WPA-Enterprise (unsupported)";
            _joinable = NO; _needsPassphrase = NO;
        } else {
            _securityLabel = @"WPA (unsupported class)";
            _joinable = NO; _needsPassphrase = NO;
        }
    } else if (siteEncry >= 2) {
        if (akm & RTWCOMPAT_AKM_WPA_PSK) {
            _securityLabel = @"WPA-PSK (TKIP)";
            _joinable = YES; _needsPassphrase = YES;
        } else {
            _securityLabel = @"WPA (unsupported class)";
            _joinable = NO; _needsPassphrase = NO;
        }
    } else if (siteEncry == 0xFF) {
        _securityLabel = @"WPA-None adhoc (unsupported)";
        _joinable = NO;
        _needsPassphrase = NO;
    } else {
        _securityLabel = [NSString stringWithFormat:@"Encrypted (class %u)", siteEncry];
        _joinable = NO;
        _needsPassphrase = NO;
    }
    return self;
}

- (nullable instancetype)initWithSsid:(NSString *)ssid
                             authMode:(uint8_t)authMode
                       needsPassphrase:(BOOL)needsPassphrase {
    self = [super init];
    if (!self) return nil;
    _ssid = ssid ?: @"";
    _channel = 0;
    _rssiPercent = 0;
    _rssiDbm = -100;
    _authMode = authMode;
    _joinable = YES;
    _needsPassphrase = needsPassphrase;
    _securityLabel = needsPassphrase ? @"WPA2-PSK (saved)" : @"Open (saved)";
    return self;
}

- (void)getBSSID:(uint8_t *)out6 {
    if (out6) memcpy(out6, _bssid, 6);
}

- (NSString *)description {
    return [NSString stringWithFormat:@"<%@ ch%u %@%s>", self.ssid,
            (unsigned)self.channel, self.securityLabel,
            self.joinable ? "" : " (locked)"];
}

@end

#pragma mark - RTWDriverStatus

/* Readwrite redeclaration lives in DriverClientPrivate.h so AppDelegate.m
 * can build offline snapshots too; the @implementation below synthesizes
 * the accessors for every unit that linked against them. */

@implementation RTWDriverStatus
@end

#pragma mark - RTWDriverClient

@interface RTWDriverClient ()
@property (nonatomic, readwrite) RTWDriverError lastError;
@property (nonatomic, copy, readwrite) NSString *lastErrorDetail;
@property (nonatomic, copy, readwrite, nullable) NSString *adapterMAC;
@property (nonatomic, readwrite) NSTimeInterval openedAt;
@property (nonatomic, readwrite) BOOL lastPollFailed;
/* v8.7.4: log throttle for repeated identical errors (the 1 Hz poll
 * produced megabytes of "No RtWlanU service" lines in the field). */
@property (nonatomic, copy, nullable) NSString *lastLoggedErrorDetail;
@property (nonatomic, readwrite) NSTimeInterval lastLoggedErrorAt;
/* Private plumbing - ARC needs every selector visible before use. */
- (void)recordError:(RTWDriverError)code detail:(NSString *)detail;
- (BOOL)readDiagNumber:(NSString *)key value:(uint64_t *)value;
- (BOOL)oidQuery:(uint32_t)oid
           reply:(RTWCompatOidFrame *)outFrame
          detail:(NSString * _Nullable * _Nullable)errorDetail;
- (BOOL)oidSet:(uint32_t)oid
        payload:(const void *)bytes
         length:(uint32_t)length
         detail:(NSString * _Nullable * _Nullable)errorDetail;
- (BOOL)oidExchange:(uint32_t)selector
              frame:(RTWCompatOidFrame *)inFrame
           outFrame:(nullable RTWCompatOidFrame *)outFrame
             detail:(NSString * _Nullable * _Nullable)errorDetail;
- (BOOL)queryDWORD:(uint32_t)oid
             value:(uint32_t *)value
            detail:(NSString * _Nullable * _Nullable)errorDetail;
- (BOOL)queryMAC:(uint8_t *)mac detail:(NSString * _Nullable * _Nullable)errorDetail;
@end

@implementation RTWDriverClient {
    io_connect_t _connect;
    /* v8.7.3: the IOService handle, retained while the user client is open,
     * so IORegistry diagnostics (RTWDiagWPADeauthReason) can be read without
     * re-walking the matching tree on every poll. */
    io_service_t _service;
    NSLock *_ioLock;
    NSString *_cachedDriverVersion;
}

#pragma mark - USB presence (v8.7.5, moved inside the implementation)

+ (BOOL)rtlUsbDonglePresent {
    /* A dongle counts as present when either an already-matched RtWlanU
     * interface exists (any OEM vendor ID) or any USB device with the
     * stock Realtek vendor ID 0x0bda sits on the bus. Read-only registry
     * walk; safe from any queue; costs microseconds. */
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return NO;
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(RTW_MASTER_PORT, match, &iter);
    match = NULL; /* consumed either way */
    BOOL present = NO;
    if (kr == KERN_SUCCESS && iter != IO_OBJECT_NULL) {
        io_object_t candidate;
        while ((candidate = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
            CFTypeRef vendor = IORegistryEntryCreateCFProperty(candidate,
                CFSTR("idVendor"), kCFAllocatorDefault, 0);
            if (vendor) {
                if (CFGetTypeID(vendor) == CFNumberGetTypeID()) {
                    int32_t value = 0;
                    if (CFNumberGetValue((CFNumberRef)vendor, kCFNumberIntType, &value)
                        && value == 0x0bda) {
                        present = YES;
                    }
                }
                CFRelease(vendor);
            }
            IOObjectRelease(candidate);
            if (present) break;
        }
        IOObjectRelease(iter);
    }
    if (!present) {
        /* Fallback: an interface already claimed by the driver (covers
         * OEM vendor IDs that the kext matches but 0x0bda does not). */
        CFMutableDictionaryRef iface = IOServiceMatching("IOUSBHostInterface");
        if (iface) {
            io_iterator_t ifaceIter = IO_OBJECT_NULL;
            kern_return_t kr2 = IOServiceGetMatchingServices(RTW_MASTER_PORT, iface, &ifaceIter);
            iface = NULL;
            if (kr2 == KERN_SUCCESS && ifaceIter != IO_OBJECT_NULL) {
                io_object_t candidate;
                while ((candidate = IOIteratorNext(ifaceIter)) != IO_OBJECT_NULL) {
                    io_name_t cls = {0};
                    IOObjectGetClass(candidate, cls);
                    if (strcmp(cls, "RtWlanU") == 0 || strcmp(cls, "RtWlanU1827") == 0)
                        present = YES;
                    IOObjectRelease(candidate);
                    if (present) break;
                }
                IOObjectRelease(ifaceIter);
            }
        }
    }
    return present;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _connect = IO_OBJECT_NULL;
        _service = IO_OBJECT_NULL;
        _ioLock = [[NSLock alloc] init];
        _lastError = RTWDriverErrorNone;
        _lastErrorDetail = @"";
        _openedAt = 0.0;
        _lastPollFailed = NO;
    }
    return self;
}

- (void)dealloc {
    if (_connect != IO_OBJECT_NULL) IOServiceClose(_connect);
    if (_service != IO_OBJECT_NULL) IOObjectRelease(_service);
}

- (BOOL)isConnected { return _connect != IO_OBJECT_NULL; }

- (void)recordError:(RTWDriverError)code detail:(NSString *)detail {
    self.lastError = code;
    self.lastErrorDetail = detail ?: @"";
    /* v8.7.4: the 1 Hz poll retries openConnection forever while the
     * driver is missing, and every retry logged the same line (the
     * 2026-11-17 logs are one repeated line for nine minutes). Log a
     * repeated identical detail at most every 20 s; state changes always
     * log immediately. */
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (![detail isEqualToString:self.lastLoggedErrorDetail]
        || now - self.lastLoggedErrorAt >= 20.0) {
        RTWLog(@"client error %ld: %@", (long)code, detail);
        self.lastLoggedErrorDetail = detail;
        self.lastLoggedErrorAt = now;
    }
}

- (void)closeConnection {
    [_ioLock lock];
    if (_connect != IO_OBJECT_NULL) {
        IOServiceClose(_connect);
        _connect = IO_OBJECT_NULL;
        if (_service != IO_OBJECT_NULL) {
            IOObjectRelease(_service);
            _service = IO_OBJECT_NULL;
        }
        _cachedDriverVersion = nil;
        /* Invalidate the open age: joins must never trust a stale settle
         * window across a client reopen. */
        self.openedAt = 0.0;
        self.lastPollFailed = NO;
        RTWLog(@"connection closed");
    }
    [_ioLock unlock];
}

#pragma mark Open / discovery

- (BOOL)openConnection:(NSString * _Nullable * _Nullable)errorDetail {
    [_ioLock lock];
    if (_connect != IO_OBJECT_NULL) {
        [_ioLock unlock];
        if (errorDetail) *errorDetail = nil;
        return YES;
    }

    /* The kext publishes IOClass "RtWlanU" (a RTWUSBController subclass);
     * class matching accepts the superclass name. Fall back to the vendor
     * utility's discovery shape (IOEthernetController + IOClass filter). */
    io_iterator_t iter = IO_OBJECT_NULL;
    io_service_t service = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("RTWUSBController");
    if (match) {
        kern_return_t kr = IOServiceGetMatchingServices(RTW_MASTER_PORT, match, &iter);
        match = NULL; /* consumed either way */
        if (kr == KERN_SUCCESS && iter != IO_OBJECT_NULL) {
            io_object_t candidate;
            while ((candidate = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
                if (service == IO_OBJECT_NULL) service = candidate;
                else IOObjectRelease(candidate);
            }
            IOObjectRelease(iter);
        }
    }
    if (service == IO_OBJECT_NULL) {
        match = IOServiceMatching("IOEthernetController");
        if (match) {
            kern_return_t kr = IOServiceGetMatchingServices(RTW_MASTER_PORT, match, &iter);
            match = NULL;
            if (kr == KERN_SUCCESS && iter != IO_OBJECT_NULL) {
                io_object_t candidate;
                while ((candidate = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
                    if (service == IO_OBJECT_NULL) {
                        io_name_t cls = {0};
                        IOObjectGetClass(candidate, cls);
                        if (strcmp(cls, "RtWlanU") == 0 || strcmp(cls, "RtWlanU1827") == 0)
                            service = candidate;
                        else
                            IOObjectRelease(candidate);
                    } else {
                        IOObjectRelease(candidate);
                    }
                }
                IOObjectRelease(iter);
            }
        }
    }
    if (service == IO_OBJECT_NULL) {
        [_ioLock unlock];
        /* v8.7.4: name the actual situation - the two logs from
         * 2026-11-17 (08:41 and 18:06) could not distinguish these cases
         * and both read "kext not loaded or dongle unplugged". */
        NSString *why;
        if ([RTWDriverClient rtlUsbDonglePresent]) {
            why = @"No RtWlanU service in IORegistry - dongle IS on USB but the driver is not loaded (run Enable Driver at Boot)";
        } else {
            why = @"No RtWlanU service in IORegistry - no Realtek dongle visible on USB (plug the adapter in)";
        }
        [self recordError:RTWDriverErrorServiceMissing detail:why];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }

    io_connect_t connect = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(),
                                     RTW_COMPAT_USER_CLIENT_TYPE, &connect);
    if (kr == KERN_SUCCESS && connect != IO_OBJECT_NULL) {
        /* v8.7.3: keep the service alive for registry diagnostics reads;
         * released in closeConnection and dealloc. */
        _service = service;
    } else {
        IOObjectRelease(service);
    }
    if (kr != KERN_SUCCESS || connect == IO_OBJECT_NULL) {
        [_ioLock unlock];
        [self recordError:RTWDriverErrorOpenFailed
                   detail:[NSString stringWithFormat:@"IOServiceOpen failed: 0x%x", kr]];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }

    _connect = connect;
    self.openedAt = [NSDate timeIntervalSinceReferenceDate];
    self.lastPollFailed = NO;
    RTWLog(@"user client opened (type %u)", RTW_COMPAT_USER_CLIENT_TYPE);
    [_ioLock unlock];

    /* First contact: read the adapter MAC once as a driver-alive probe. */
    NSString *detail = nil;
    uint8_t mac[6] = {0};
    if ([self queryMAC:mac detail:&detail]) {
        self.adapterMAC = [NSString stringWithFormat:@"%02x:%02x:%02x:%02x:%02x:%02x",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]];
        RTWLog(@"adapter MAC %@", self.adapterMAC);
    } else {
        RTWLog(@"MAC probe failed: %@", detail);
    }
    if (errorDetail) *errorDetail = nil;
    return YES;
}

#pragma mark OID plumbing

- (BOOL)oidQuery:(uint32_t)oid
           reply:(RTWCompatOidFrame *)outFrame
          detail:(NSString * _Nullable * _Nullable)errorDetail {
    RTWCompatOidFrame in;
    memset(&in, 0, sizeof(in));
    in.oid = oid;                     /* host == little-endian on all targets */
    in.length = RTWCOMPAT_OID_DATA_MAX;
    return [self oidExchange:RTWCOMPAT_SEL_OID_QUERY frame:&in outFrame:outFrame
                      detail:errorDetail];
}

- (BOOL)oidSet:(uint32_t)oid
        payload:(const void *)bytes
         length:(uint32_t)length
         detail:(NSString * _Nullable * _Nullable)errorDetail {
    RTWCompatOidFrame in;
    memset(&in, 0, sizeof(in));
    in.oid = oid;
    if (length > RTWCOMPAT_OID_DATA_MAX) length = RTWCOMPAT_OID_DATA_MAX;
    in.length = length;
    if (bytes && length) memcpy(in.data, bytes, length);
    return [self oidExchange:RTWCOMPAT_SEL_OID_SET frame:&in outFrame:nil
                      detail:errorDetail];
}

- (BOOL)oidExchange:(uint32_t)selector
              frame:(RTWCompatOidFrame *)inFrame
           outFrame:(nullable RTWCompatOidFrame *)outFrame
             detail:(NSString * _Nullable * _Nullable)errorDetail {
    if (_connect == IO_OBJECT_NULL) {
        [self recordError:RTWDriverErrorCallFailed detail:@"not connected"];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }
    [_ioLock lock];
    RTWCompatOidFrame out;
    memset(&out, 0, sizeof(out));
    size_t outSize = sizeof(out);
    IOReturn kr = IOConnectCallStructMethod(_connect, selector,
                                            inFrame, sizeof(*inFrame),
                                            &out, &outSize);
    [_ioLock unlock];
    if (kr != KERN_SUCCESS || outSize != RTWCOMPAT_OID_FRAME_SIZE) {
        [self recordError:RTWDriverErrorCallFailed
            detail:[NSString stringWithFormat:@"OID sel %u 0x%08x failed: 0x%x (size %zu)",
                    selector, inFrame->oid, kr, outSize]];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }
    if (out.status != 0) {
        /* kIOReturnNotPrivileged would mean the compat gate refused us;
         * keep it visible so it can never masquerade as "no dongle". */
        RTWDriverError code = (out.status == kIOReturnNotPrivileged)
            ? RTWDriverErrorNotPrivileged : RTWDriverErrorCallFailed;
        [self recordError:code
            detail:[NSString stringWithFormat:@"OID 0x%08x driver status: 0x%x",
                    inFrame->oid, out.status]];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }
    if (outFrame) *outFrame = out;
    if (errorDetail) *errorDetail = nil;
    return YES;
}

- (BOOL)queryDWORD:(uint32_t)oid value:(uint32_t *)value detail:(NSString * _Nullable * _Nullable)errorDetail {
    RTWCompatOidFrame reply;
    if (![self oidQuery:oid reply:&reply detail:errorDetail]) return NO;
    if (reply.retLen < 4) {
        [self recordError:RTWDriverErrorCallFailed
            detail:[NSString stringWithFormat:@"OID 0x%08x short reply (%u)", oid, reply.retLen]];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }
    if (value) *value = RTWCompatLoad32(reply.data);
    return YES;
}

/* Silent variant of the network-status query: the kext refuses this OID
 * while no association is up (kr 0x10000003 on every poll), which is a
 * normal unassociated state - not an error worth recording once per
 * second (field log 2026-11-17 06:58/07:15: 1 Hz "client error 3" flood
 * and pointless user-client reopens while simply unassociated). */
- (BOOL)queryLinkStatusQuiet:(uint32_t *)value {
    if (_connect == IO_OBJECT_NULL) return NO;
    [_ioLock lock];
    RTWCompatOidFrame in, out;
    memset(&in, 0, sizeof(in));
    in.oid = RTWCOMPAT_QID_NETWORK_STATUS;
    in.length = RTWCOMPAT_OID_DATA_MAX;
    size_t outSize = sizeof(out);
    IOReturn kr = IOConnectCallStructMethod(_connect, RTWCOMPAT_SEL_OID_QUERY,
                                            &in, sizeof(in), &out, &outSize);
    [_ioLock unlock];
    if (kr != KERN_SUCCESS || outSize != RTWCOMPAT_OID_FRAME_SIZE) return NO;
    if (out.status != 0) return NO;
    if (out.retLen < 4) return NO;
    if (value) *value = RTWCompatLoad32(out.data);
    return YES;
}

/* v8.7.14: silent join-state query (kext 1.0.8 OID 0x00010115). Answers
 * 8 bytes: net80211 station state + ms since the last beacon. A NO here
 * (older driver, call failure) never records an error - the join monitor
 * falls back to the time window alone. */
- (BOOL)queryJoinState:(uint32_t * _Nullable)state
           beaconAgeMs:(uint32_t * _Nullable)beaconAge {
    if (_connect == IO_OBJECT_NULL) return NO;
    [_ioLock lock];
    RTWCompatOidFrame in, out;
    memset(&in, 0, sizeof(in));
    in.oid = RTWCOMPAT_QID_JOIN_STATE;
    in.length = RTWCOMPAT_OID_DATA_MAX;
    size_t outSize = sizeof(out);
    IOReturn kr = IOConnectCallStructMethod(_connect, RTWCOMPAT_SEL_OID_QUERY,
                                            &in, sizeof(in), &out, &outSize);
    [_ioLock unlock];
    if (kr != KERN_SUCCESS || outSize != RTWCOMPAT_OID_FRAME_SIZE) return NO;
    if (out.status != 0) return NO;
    if (out.retLen < 8) return NO;
    if (state) *state = RTWCompatLoad32(out.data);
    if (beaconAge) *beaconAge = RTWCompatLoad32(out.data + 4);
    return YES;
}

- (BOOL)queryMAC:(uint8_t *)mac detail:(NSString * _Nullable * _Nullable)errorDetail {
    RTWCompatOidFrame reply;
    if (![self oidQuery:RTWCOMPAT_QID_PERMANENT_ADDRESS reply:&reply detail:errorDetail]) return NO;
    if (reply.retLen < 6) {
        if (errorDetail) *errorDetail = @"short MAC reply";
        return NO;
    }
    memcpy(mac, reply.data, 6);
    return YES;
}

#pragma mark Status

- (nullable RTWDriverStatus *)pollStatus {
    self.lastPollFailed = NO;
    RTWDriverStatus *status = [[RTWDriverStatus alloc] init];
    status.driverReachable = [self isConnected];

    if (!status.driverReachable) {
        self.lastPollFailed = YES;
        status.lastError = self.lastErrorDetail ?: @"";
        return status;
    }

    NSString *detail = nil;
    uint32_t linked = 0;
    if ([self queryLinkStatusQuiet:&linked]) {
        status.linked = linked == 1;
    } else {
        /* The kext refuses the link-status OID while no association is up
         * (field log 2026-11-17 07:36: "OID sel 9 0x00010114 failed:
         * 0x10000003" on every poll of a freshly opened client). That is
         * a normal unassociated state, not a dead client: probe liveness
         * with the address query - it answers while unassociated - and
         * only a client that fails that too counts as a poll failure. */
        uint8_t mac[6];
        if ([self queryMAC:mac detail:nil]) {
            status.linked = NO; /* alive, just not associated (yet) */
        } else {
            /* A pulled dongle or terminated client shows up here. */
            self.lastPollFailed = YES;
            status.lastError = self.lastErrorDetail ?: @"";
            return status;
        }
    }

    /* 8.8.5: the kext answers "linked" as soon as net80211 reaches RUN, and
     * RUN comes BEFORE the WPA 4-way handshake. Field log 2026-11-22 11:11
     * (RTL8821AU, 5 GHz): the menu read connected while
     * RTWDiagWPASupplicantState=1, no EAPOL received and the port closed -
     * no page could load and Rejoin was greyed out. On a protected network
     * the link counts only once the port is valid. The status OID above has
     * just refreshed these registry values (the kext publishes them on every
     * OID call). A kext that does not publish them keeps the old behaviour. */
    if (status.linked) {
        uint64_t rsnOn = 0, portValid = 0;
        if ([self readDiagNumber:@"RTWDiagWPARSNEnabled" value:&rsnOn] && rsnOn != 0 &&
            [self readDiagNumber:@"RTWDiagWPAPortValid" value:&portValid] && portValid == 0) {
            status.linked = NO;
            status.securing = YES;
        }
    }

    if (status.linked) {
        RTWCompatOidFrame ssidReply;
        if ([self oidQuery:RTWCOMPAT_QID_CURRENT_SSID reply:&ssidReply detail:&detail]
            && ssidReply.retLen > 0 && ssidReply.retLen <= RTWCOMPAT_MAX_SSID) {
            /* v8.7.9: trim at the first control byte before decoding - the
             * join monitor compares this against the join target, and the
             * printable-prefix rule (reference file Model.hpp) starts at
             * the raw boundary. */
            uint32_t printable = 0;
            while (printable < ssidReply.retLen &&
                   ssidReply.data[printable] >= 0x20 &&
                   ssidReply.data[printable] != 0x7f) printable++;
            status.ssid = [[NSString alloc] initWithBytes:ssidReply.data
                                                  length:printable
                                                encoding:NSUTF8StringEncoding];
            if (!status.ssid) status.ssid = @"";
        }
        uint32_t rssi = 0, channel = 0;
        if ([self queryDWORD:RTWCOMPAT_QID_RSSI value:&rssi detail:&detail])
            status.rssiPercent = (int32_t)rssi;
        if ([self queryDWORD:RTWCOMPAT_QID_CHANNEL value:&channel detail:&detail])
            status.channel = channel;
    }

    if (_cachedDriverVersion.length == 0) {
        _cachedDriverVersion = [self copyDriverVersion:&detail];
        if (_cachedDriverVersion.length) {
            RTWLog(@"driver version %@", _cachedDriverVersion);
        }
    }
    status.driverVersion = _cachedDriverVersion;
    status.lastError = @"";
    return status;
}

- (nullable NSString *)copyDriverVersion:(NSString * _Nullable * _Nullable)errorDetail {
    RTWCompatOidFrame reply;
    if (![self oidQuery:RTWCOMPAT_QID_DRIVER_VERSION reply:&reply detail:errorDetail])
        return nil;
    if (!reply.retLen || reply.retLen > 64) return nil;
    return [[NSString alloc] initWithBytes:reply.data
                                   length:reply.retLen
                                 encoding:NSUTF8StringEncoding];
}

/* v8.7.3: the kext publishes ic_deauth_reason as RTWDiagWPADeauthReason and
 * refreshes it under its control gate on every status poll. A received
 * deauth OR disassoc stores the wire reason (stack ieee80211_input.cpp),
 * so reading this at the tick the link drops names the party that ended
 * the association. 0 = not published by this kext build or never set this
 * session; deauth reasons are recorded by every kext build, disassoc
 * reasons only since kext 1.0.4 (stack v8.7.3 change). */
- (uint32_t)lastDeauthReason {
    [_ioLock lock];
    io_service_t service = _service;
    if (service == IO_OBJECT_NULL) {
        [_ioLock unlock];
        return 0;
    }
    uint32_t reason = 0;
    CFNumberRef value = (CFNumberRef)IORegistryEntryCreateCFProperty(
        service, CFSTR("RTWDiagWPADeauthReason"), kCFAllocatorDefault, 0);
    [_ioLock unlock];
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            int64_t raw = 0;
            if (CFNumberGetValue(value, kCFNumberSInt64Type, &raw) &&
                raw > 0 && raw <= 0xFFFF)
                reason = (uint32_t)raw;
        }
        CFRelease(value);
    }
    return reason;
}

/* 8.8.5: one RTWDiag* number from the open driver service. NO when the
 * client is closed or the kext does not publish the key. The kext writes
 * every key it knows, zeros included (DiagnosticPublishPolicy.hpp), so a
 * missing key means an older kext, not a zero. */
- (BOOL)readDiagNumber:(NSString *)key value:(uint64_t *)value {
    [_ioLock lock];
    io_service_t service = _service;
    if (service == IO_OBJECT_NULL) {
        [_ioLock unlock];
        return NO;
    }
    CFTypeRef raw = IORegistryEntryCreateCFProperty(service, (__bridge CFStringRef)key,
                                                    kCFAllocatorDefault, 0);
    [_ioLock unlock];
    if (!raw) return NO;
    BOOL ok = NO;
    if (CFGetTypeID(raw) == CFNumberGetTypeID()) {
        int64_t number = 0;
        if (CFNumberGetValue((CFNumberRef)raw, kCFNumberSInt64Type, &number)) {
            if (value) *value = (uint64_t)number;
            ok = YES;
        }
    }
    CFRelease(raw);
    return ok;
}

- (NSString *)diagnosticValuesForKeys:(NSArray<NSString *> *)keys {
    NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithCapacity:keys.count];
    for (NSString *key in keys) {
        uint64_t number = 0;
        NSString *shortKey = [key hasPrefix:@"RTWDiag"] ? [key substringFromIndex:7] : key;
        if ([self readDiagNumber:key value:&number]) {
            [parts addObject:[NSString stringWithFormat:@"%@=%llu", shortKey,
                              (unsigned long long)number]];
        } else {
            [parts addObject:[NSString stringWithFormat:@"%@=-", shortKey]];
        }
    }
    return [parts componentsJoinedByString:@" "];
}

- (nullable NSString *)interfaceIPv4:(NSString * _Nullable * _Nullable)bsdName {
    if (bsdName) *bsdName = nil;
    /* The IOEthernetInterface below the controller carries "BSD Name". */
    [_ioLock lock];
    io_service_t service = _service;
    CFTypeRef nameRef = NULL;
    if (service != IO_OBJECT_NULL) {
        nameRef = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("BSD Name"),
                                                  kCFAllocatorDefault, kIORegistryIterateRecursively);
    }
    [_ioLock unlock];
    if (!nameRef) return nil;
    NSString *name = nil;
    if (CFGetTypeID(nameRef) == CFStringGetTypeID()) name = [(__bridge NSString *)nameRef copy];
    CFRelease(nameRef);
    if (!name.length) return nil;
    if (bsdName) *bsdName = name;

    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0 || !list) return nil;
    NSString *address = nil;
    const char *wanted = name.UTF8String;
    for (struct ifaddrs *it = list; it; it = it->ifa_next) {
        if (!it->ifa_name || !it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(it->ifa_name, wanted) != 0) continue;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)it->ifa_addr;
        uint32_t host = ntohl(sin->sin_addr.s_addr);
        if ((host & 0xFFFF0000u) == 0xA9FE0000u) continue; /* 169.254/16 self-assigned */
        if (host == 0) continue;
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) {
            address = [NSString stringWithUTF8String:text];
            break;
        }
    }
    freeifaddrs(list);
    return address;
}

#pragma mark Scan

- (BOOL)triggerScan:(NSString * _Nullable * _Nullable)errorDetail {
    return [self oidSet:RTWCOMPAT_SID_SCAN_TRIGGER payload:NULL length:4
                 detail:errorDetail];
}

- (BOOL)scanBusy:(NSString * _Nullable * _Nullable)errorDetail {
    uint32_t busy = 1;
    if (![self queryDWORD:RTWCOMPAT_QID_SCAN_BUSY value:&busy detail:errorDetail])
        return NO; /* poll failure reads as "not busy"; caller fetches what exists */
    return busy != 0;
}

- (nullable NSArray<RTWNetworkEntry *> *)surveyRecords:(NSString * _Nullable * _Nullable)errorDetail {
    NSMutableArray<RTWNetworkEntry *> *records = [NSMutableArray array];
    if (_connect == IO_OBJECT_NULL) {
        [self recordError:RTWDriverErrorCallFailed detail:@"not connected"];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return records;
    }
    [_ioLock lock];
    for (uint32_t index = 0; index < RTWCOMPAT_SURVEY_MAX; ++index) {
        uint64_t scalarIn = index;
        uint8_t record[RTWCOMPAT_SURVEY_RECORD];
        size_t recordSize = sizeof(record);
        /* Selector 0: scalar input = record index, STRUCTURE output = the
         * 0x640 record. Scalar outputs stay NULL (the MIG layer allows at
         * most 16 uint64 scalars - a 1600-byte buffer can never go there). */
        IOReturn kr = IOConnectCallMethod(_connect, RTWCOMPAT_SEL_SURVEY_RECORD,
                                          &scalarIn, 1, NULL, 0,
                                          NULL, NULL, record, &recordSize);
        if (kr != KERN_SUCCESS) {
            /* kIOReturnNotFound ends the table; anything else stops the walk. */
            if (kr != kIOReturnNotFound) {
                [_ioLock unlock];
                [self recordError:RTWDriverErrorCallFailed
                    detail:[NSString stringWithFormat:@"survey record %u failed: 0x%x", index, kr]];
                if (errorDetail) *errorDetail = self.lastErrorDetail;
                return records;
            }
            break;
        }
        if (recordSize < RTWCOMPAT_SURVEY_RECORD) break;
        RTWNetworkEntry *entry = [[RTWNetworkEntry alloc] initWithSurveyRecord:record];
        if (entry.ssid.length) [records addObject:entry];
    }
    [_ioLock unlock];
    if (errorDetail) *errorDetail = nil;
    return records;
}

#pragma mark Association

- (BOOL)joinNetwork:(RTWNetworkEntry *)entry
         passphrase:(nullable NSString *)passphrase
              error:(NSString * _Nullable * _Nullable)errorDetail {
    if (!entry || !entry.ssid.length || !entry.joinable) {
        [self recordError:RTWDriverErrorCallFailed detail:@"invalid join target"];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }

    /* 1. SSID set (resets the kext's pending join state). The driver trims
     *    at the first non-printable byte, so send plain printable UTF-8. */
    NSData *ssid = [entry.ssid dataUsingEncoding:NSUTF8StringEncoding];
    if (!ssid.length || ssid.length > RTWCOMPAT_MAX_SSID) {
        [self recordError:RTWDriverErrorCallFailed detail:@"ssid encoding failed"];
        if (errorDetail) *errorDetail = self.lastErrorDetail;
        return NO;
    }
    NSString *detail = nil;
    if (![self oidSet:RTWCOMPAT_SID_NETWORK_NAME payload:ssid.bytes
               length:(uint32_t)ssid.length detail:&detail]) {
        if (errorDetail) *errorDetail = detail;
        return NO;
    }

    /* mejoras: 1b. pin the AP the user picked (after the SSID set,
     * which resets the pending state; before auth/key, which fire the join).
     * Not fatal: an older kext accepts and ignores the OID. */
    if (entry.pinBssid) {
        uint8_t bssid[6] = {0};
        [entry getBSSID:bssid];
        BOOL any = NO;
        for (int i = 0; i < 6; i++) if (bssid[i]) any = YES;
        if (any) {
            NSString *pinDetail = nil;
            BOOL pinned = [self oidSet:RTWCOMPAT_SID_DESIRED_BSSID payload:bssid length:6 detail:&pinDetail];
            RTWLog(@"join pinned to AP %02x:%02x:%02x:%02x:%02x:%02x (ch %u) -> %@",
                   bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                   (unsigned)entry.channel, pinned ? @"ok" : (pinDetail ?: @"failed"));
        }
    }

    /* 2. Auth mode: echo the utility enum the driver packed into the survey
     *    record (+0x23). 0 = open (zero PMK), 3..6 = PSK classes. */
    uint8_t authPayload[4];
    RTWCompatStore32(authPayload, entry.authMode);
    if (![self oidSet:RTWCOMPAT_QID_AUTH_MODE payload:authPayload length:4
               detail:&detail]) {
        if (errorDetail) *errorDetail = detail;
        return NO;
    }

    /* 3. Passphrase for PSK classes; the kext derives the PMK itself. The
     *    key set completes the pending state, which fires the auto-join. */
    if (entry.needsPassphrase) {
        NSData *key = [passphrase dataUsingEncoding:NSUTF8StringEncoding];
        if (!key.length || key.length > 63) {
            [self recordError:RTWDriverErrorCallFailed
                detail:@"passphrase missing or outside the 8..63 character WPA range"];
            if (errorDetail) *errorDetail = self.lastErrorDetail;
            return NO;
        }
        if (![self oidSet:RTWCOMPAT_SID_WPA_KEY payload:key.bytes
                   length:(uint32_t)key.length detail:&detail]) {
            if (errorDetail) *errorDetail = detail;
            return NO;
        }
    }

    /* 4. Explicit trigger - benign no-op when the auto-join already ran. */
    if (![self oidSet:RTWCOMPAT_SID_ASSOC_TRIGGER payload:NULL length:0
               detail:&detail]) {
        RTWLog(@"assoc trigger returned %@ (auto-join may already hold the link)",
               detail);
    }
    if (errorDetail) *errorDetail = nil;
    return YES;
}

- (BOOL)disconnectFromNetwork:(NSString * _Nullable * _Nullable)errorDetail {
    return [self oidSet:RTWCOMPAT_SID_DISASSOCIATE payload:NULL length:0
                 detail:errorDetail];
}

/* 8.7.20: read the kext's diagnostic properties straight from the IORegistry
 * so a single log carries everything (no separate ioreg commands needed). */
- (NSString *)diagnosticSummary {
    io_service_t service = IO_OBJECT_NULL;
    io_iterator_t iter = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("RTWUSBController");
    if (match && IOServiceGetMatchingServices(RTW_MASTER_PORT, match, &iter) == KERN_SUCCESS
        && iter != IO_OBJECT_NULL) {
        io_object_t candidate;
        while ((candidate = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
            if (service == IO_OBJECT_NULL) service = candidate;
            else IOObjectRelease(candidate);
        }
        IOObjectRelease(iter);
    }
    if (service == IO_OBJECT_NULL) return @"(no driver service)";
    CFMutableDictionaryRef props = NULL;
    kern_return_t kr = IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS || !props) return @"(properties unreadable)";
    NSDictionary *all = CFBridgingRelease(props);
    NSArray *prefixes = @[@"RTWDiag", @"RTWSnap_", @"RTWEfuse", @"RTWTx88E", @"RTWResidual",
                          @"RTWStart", @"RTWSiliconFamily"];
    NSMutableArray *parts = [NSMutableArray array];
    for (NSString *key in [[all allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        BOOL keep = NO;
        for (NSString *prefix in prefixes) if ([key hasPrefix:prefix]) { keep = YES; break; }
        if (!keep) continue;
        id value = all[key];
        /* Skip zero counters to keep the line short. */
        if ([value isKindOfClass:[NSNumber class]] && [value unsignedLongLongValue] == 0) continue;
        [parts addObject:[NSString stringWithFormat:@"%@=%@", key, value]];
    }
    return parts.count ? [parts componentsJoinedByString:@" "] : @"(no diagnostic properties)";
}

@end
