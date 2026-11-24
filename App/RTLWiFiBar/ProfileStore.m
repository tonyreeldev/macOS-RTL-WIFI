/*
 * ProfileStore.m - RTWProfileStore implementation (app-owned plist).
 *
 * Why not the Keychain: every rebuild of the binary loses the Keychain ACL
 * and macOS prompts for access on first use (the field bug "first use shows
 * a keychain popup"). The stock Realtek utility keeps plaintext-obfuscated
 * profiles in its own plist and never touches the Keychain; this store
 * mirrors that model. See ProfileStore.h for the file layout.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#import "ProfileStore.h"
#import "RTWLog.h"
#import <Security/Security.h>

/* Relative to the user's home: ~/Library/Application Support/RTLWiFiBar/ */
static NSString * const kStoreRelativePath =
    @"Library/Application Support/RTLWiFiBar/Profiles.plist";
static NSString * const kNetworksKey = @"networks";

static NSString * const kPassKey = @"pass";
static NSString * const kAuthKey = @"auth";
static NSString * const kChanKey = @"channel";
static NSString * const kRememberedKey = @"remembered";
/* v8.7.17: epoch seconds of the newest save/commit for this SSID - the
 * ordering key behind mostRecentlyUsedSsid (the startup auto-rejoin
 * target). Profiles written by older builds have no stamp and sort
 * behind stamped ones. */
static NSString * const kLastUsedKey = @"lastUsed";
/* mejoras-optiplex: the passphrase lives in the login Keychain; the plist
 * only keeps kc = YES. The app is signed with a fixed local certificate
 * ("RTLWiFiBar Local Signing"), so the Keychain ACL survives rebuilds.
 * If the Keychain refuses, the old obfuscated plist field is the fallback. */
static NSString * const kKeychainFlagKey = @"kc";
static NSString * const kKeychainService = @"com.rtlwifi.RTLWiFiBar.wifi";

#pragma mark Keychain (mejoras-optiplex)

static NSMutableDictionary *RTWKCQuery(NSString *ssid) {
    return [@{ (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
               (__bridge id)kSecAttrService: kKeychainService,
               (__bridge id)kSecAttrAccount: ssid } mutableCopy];
}

static BOOL RTWKCWrite(NSString *ssid, NSString *pass) {
    NSData *data = [pass dataUsingEncoding:NSUTF8StringEncoding];
    if (!ssid.length || !data.length) return NO;
    NSMutableDictionary *q = RTWKCQuery(ssid);
    OSStatus st = SecItemUpdate((__bridge CFDictionaryRef)q,
        (__bridge CFDictionaryRef)@{ (__bridge id)kSecValueData: data });
    if (st == errSecItemNotFound) {
        q[(__bridge id)kSecValueData] = data;
        q[(__bridge id)kSecAttrLabel] = [NSString stringWithFormat:@"RTLWiFiBar: %@", ssid];
        st = SecItemAdd((__bridge CFDictionaryRef)q, NULL);
    }
    if (st != errSecSuccess) RTWLog(@"keychain write for %@ failed (%d)", ssid, (int)st);
    return st == errSecSuccess;
}

static NSString *RTWKCRead(NSString *ssid) {
    if (!ssid.length) return nil;
    NSMutableDictionary *q = RTWKCQuery(ssid);
    q[(__bridge id)kSecReturnData] = @YES;
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef out = NULL;
    OSStatus st = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);
    if (st != errSecSuccess || !out) {
        if (st != errSecItemNotFound) RTWLog(@"keychain read for %@ failed (%d)", ssid, (int)st);
        return nil;
    }
    NSData *data = (__bridge_transfer NSData *)out;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static void RTWKCDelete(NSString *ssid) {
    if (!ssid.length) return;
    SecItemDelete((__bridge CFDictionaryRef)RTWKCQuery(ssid));
}

#pragma mark Obfuscation (obscure, NOT encryption)

/* XOR every byte with a fixed mask, then base64. The goal is only that the
 * passphrase never appears in clear inside the plist file; the router
 * password threat model here is the same as the stock utility's
 * ProfilesList.plist. */
static NSString *RTWEncodePassphrase(NSString *passphrase) {
    if (!passphrase.length) return @"";
    NSData *raw = [passphrase dataUsingEncoding:NSUTF8StringEncoding];
    if (!raw.length) return @"";
    uint8_t *bytes = malloc(raw.length);
    if (!bytes) return @"";
    const uint8_t *src = raw.bytes;
    for (NSUInteger i = 0; i < raw.length; ++i) {
        bytes[i] = src[i] ^ 0x5A;
    }
    NSData *masked = [NSData dataWithBytes:bytes length:raw.length];
    free(bytes);
    return [masked base64EncodedStringWithOptions:0];
}

static NSString *RTWDecodePassphrase(NSString *encoded) {
    if (!encoded.length) return @"";
    NSData *masked = [[NSData alloc] initWithBase64EncodedString:encoded
                                                         options:0];
    if (!masked.length) return @"";
    uint8_t *bytes = malloc(masked.length);
    if (!bytes) return @"";
    const uint8_t *src = masked.bytes;
    for (NSUInteger i = 0; i < masked.length; ++i) {
        bytes[i] = src[i] ^ 0x5A;
    }
    NSString *pass = [[NSString alloc] initWithBytes:bytes
                                              length:masked.length
                                            encoding:NSUTF8StringEncoding];
    free(bytes);
    return pass ?: @"";
}

#pragma mark RTWProfileStore

@implementation RTWProfileStore {
    NSMutableDictionary<NSString *, NSDictionary *> *_networks;
    NSMutableDictionary<NSString *, NSString *> *_passCache; /* one Keychain read per SSID */
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _networks = [NSMutableDictionary dictionary];
        _passCache = [NSMutableDictionary dictionary];
        [self loadStore];
        [self migrateToKeychain];
    }
    return self;
}

- (NSString *)storePath {
    NSString *home = NSHomeDirectory();
    if (!home.length) home = @"~";
    return [home stringByAppendingPathComponent:kStoreRelativePath];
}

- (void)loadStore {
    NSDictionary *stored =
        [NSDictionary dictionaryWithContentsOfFile:[self storePath]];
    if (!stored) return;
    NSDictionary *networks = stored[kNetworksKey];
    if (![networks isKindOfClass:[NSDictionary class]]) return;
    for (NSString *ssid in networks) {
        NSDictionary *entry = networks[ssid];
        if (![ssid isKindOfClass:[NSString class]] ||
            ![entry isKindOfClass:[NSDictionary class]]) continue;
        _networks[ssid] = entry;
    }
}

/* mejoras-optiplex: move every obfuscated plist passphrase into the
 * Keychain once; the plist field is removed only after a good write. */
- (void)migrateToKeychain {
    BOOL changed = NO;
    for (NSString *ssid in [_networks allKeys]) {
        NSDictionary *entry = _networks[ssid];
        NSString *encoded = entry[kPassKey];
        if (![encoded isKindOfClass:[NSString class]] || !encoded.length) continue;
        NSString *pass = RTWDecodePassphrase(encoded);
        if (!pass.length) continue;
        if (!RTWKCWrite(ssid, pass)) continue;
        NSMutableDictionary *m = [entry mutableCopy];
        [m removeObjectForKey:kPassKey];
        m[kKeychainFlagKey] = @YES;
        _networks[ssid] = m;
        _passCache[ssid] = pass;
        changed = YES;
        RTWLog(@"password for %@ moved to the Keychain", ssid);
    }
    if (changed) [self persistStore];
}

- (void)persistStore {
    NSString *path = [self storePath];
    NSString *dir = path.stringByDeletingLastPathComponent;
    NSFileManager *fm = [NSFileManager defaultManager];
    /* 0700 on the folder: only the owning user can even list it. */
    [fm createDirectoryAtPath:dir
      withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions:@0700}
                            error:nil];
    /* writeToFile:atomically: exists since 10.0 and is safe across every
     * SDK; the dictionary must be a property-list type at the top level. */
    NSDictionary *store = @{ kNetworksKey: _networks };
    if (![store writeToFile:path atomically:YES]) {
        RTWLog(@"failed writing %@", path);
        return;
    }
    /* 0600 on the file: owner read/write only. The selector is
     * setAttributes:ofItemAtPath:error: (there is no
     * setAttributesOfItemAtPath: variant on NSFileManager). */
    [fm setAttributes:@{NSFilePosixPermissions:@0600}
          ofItemAtPath:path
                error:nil];
}

- (nullable NSDictionary *)profileForSsid:(NSString *)ssid {
    if (!ssid.length) return nil;
    NSDictionary *entry = _networks[ssid];
    if ([entry isKindOfClass:[NSDictionary class]]) return entry;
    return nil;
}

- (nullable NSString *)passphraseForSsid:(NSString *)ssid {
    NSDictionary *entry = [self profileForSsid:ssid];
    if (!entry) return nil;
    NSString *cached = _passCache[ssid];
    if (cached.length) return cached;
    if ([entry[kKeychainFlagKey] boolValue]) {
        NSString *kc = RTWKCRead(ssid);
        if (kc.length) _passCache[ssid] = kc;
        return kc.length ? kc : nil;
    }
    NSString *encoded = entry[kPassKey];
    if (![encoded isKindOfClass:[NSString class]]) return nil;
    NSString *pass = RTWDecodePassphrase(encoded);
    return pass.length ? pass : nil;
}

- (BOOL)hasProfileForSsid:(NSString *)ssid {
    return [self profileForSsid:ssid] != nil;
}

- (uint8_t)authModeForSsid:(NSString *)ssid {
    NSDictionary *entry = [self profileForSsid:ssid];
    NSNumber *auth = entry[kAuthKey];
    if (![auth isKindOfClass:[NSNumber class]]) return 6u; /* WPA2-PSK */
    return (uint8_t)auth.unsignedIntValue;
}

- (uint32_t)channelForSsid:(NSString *)ssid {
    NSDictionary *entry = [self profileForSsid:ssid];
    NSNumber *channel = entry[kChanKey];
    if (![channel isKindOfClass:[NSNumber class]]) return 0u;
    return channel.unsignedIntValue;
}

- (BOOL)rememberNetwork:(NSString *)ssid
             passphrase:(NSString *)passphrase
               authMode:(uint8_t)authMode
                channel:(uint32_t)channel {
    if (!ssid.length) return NO;
    NSMutableDictionary *entry = [NSMutableDictionary dictionary];
    [_passCache removeObjectForKey:ssid];
    if (passphrase.length && RTWKCWrite(ssid, passphrase)) {
        entry[kKeychainFlagKey] = @YES;          /* mejoras-optiplex */
        _passCache[ssid] = passphrase;
    } else {
        if (!passphrase.length) RTWKCDelete(ssid); /* open network: no stale key */
        entry[kPassKey] = RTWEncodePassphrase(passphrase ?: @"");
    }
    entry[kAuthKey] = @((unsigned)authMode);
    entry[kChanKey] = @((unsigned)channel);
    entry[kRememberedKey] = @YES;
    /* v8.7.17: stamp the freshness so the app knows which saved network
     * to auto-rejoin once after the next restart. */
    entry[kLastUsedKey] = @([NSDate timeIntervalSinceReferenceDate]);
    _networks[ssid] = entry;
    [self persistStore];
    RTWLog(@"remembered network %@ (auth %u, ch %u, key %lu chars)%@",
                       ssid, (unsigned)authMode, (unsigned)channel,
                       (unsigned long)passphrase.length,
                       (authMode == 6u && passphrase.length > 0 && passphrase.length < 8)
                           ? @" - WARNING: WPA2 keys are 8-63 characters; this key looks too short, check for a missing character"
                           : @"");
    return YES;
}

- (BOOL)savePassphrase:(NSString *)passphrase forSsid:(NSString *)ssid {
    /* Legacy shim: keep whatever auth/channel an existing profile carries. */
    NSDictionary *existing = [self profileForSsid:ssid];
    uint8_t auth = 6u;
    uint32_t channel = 0u;
    if (existing) {
        NSNumber *oldAuth = existing[kAuthKey];
        NSNumber *oldChan = existing[kChanKey];
        if ([oldAuth isKindOfClass:[NSNumber class]]) auth = (uint8_t)oldAuth.unsignedIntValue;
        if ([oldChan isKindOfClass:[NSNumber class]]) channel = oldChan.unsignedIntValue;
    }
    return [self rememberNetwork:ssid passphrase:passphrase authMode:auth channel:channel];
}

- (BOOL)forgetSsid:(NSString *)ssid {
    if (!ssid.length) return YES;
    if (!_networks[ssid]) return YES;
    RTWKCDelete(ssid);                           /* mejoras-optiplex */
    [_passCache removeObjectForKey:ssid];
    [_networks removeObjectForKey:ssid];
    [self persistStore];
    RTWLog(@"forgot profile %@", ssid);
    return YES;
}

- (void)resetAllProfiles {
    for (NSString *ssid in [_networks allKeys]) RTWKCDelete(ssid); /* mejoras-optiplex */
    [_passCache removeAllObjects];
    [_networks removeAllObjects];
    NSString *path = [self storePath];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path]) {
        if ([fm removeItemAtPath:path error:nil]) {
            RTWLog(@"profile store removed: %@", path);
        } else {
            /* Fall back to an empty store so memory and disk agree. */
            [self persistStore];
            RTWLog(@"profile store could not be removed - emptied instead");
        }
    }
}

- (nullable NSArray<NSString *> *)savedSsids {
    if (!_networks.count) return @[];
    return [_networks.allKeys sortedArrayUsingSelector:@selector(compare:)];
}

/* v8.7.17: the saved network the startup auto-rejoin arms for - the one
 * with the newest lastUsed stamp (0 for unstamped older profiles, so
 * they only ever win when nothing newer exists). nil when empty. */
- (nullable NSString *)mostRecentlyUsedSsid {
    NSString *best = nil;
    NSTimeInterval bestStamp = -1.0;
    for (NSString *ssid in _networks) {
        NSDictionary *entry = _networks[ssid];
        NSNumber *stamp = [entry objectForKey:kLastUsedKey];
        NSTimeInterval value = [stamp isKindOfClass:[NSNumber class]] ? stamp.doubleValue : 0.0;
        if (value > bestStamp) {
            bestStamp = value;
            best = ssid;
        }
    }
    return best;
}

@end
