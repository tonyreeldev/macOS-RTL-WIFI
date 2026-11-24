/* SPDX-License-Identifier: GPL-2.0-only */
/* Realtek utility compatibility engine. See RtWlanUCompat.h for the wire
 * protocol this implements and where it was recovered from. */
#include "RtWlanUCompat.h"
#include "RTWUserClient.hpp"
#include "RTWUSBController.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Minimal SHA-1 + HMAC-SHA1 + PBKDF2-HMAC-SHA1 (RFC 2898).            */
/* WPA2-Personal PMK = PBKDF2(passphrase, ssid, 4096, 32).             */
/* The RTWControl JOIN ABI receives the derived PMK only; passphrase   */
/* material never leaves this translation layer.                       */
/* ------------------------------------------------------------------ */
namespace RtwWlanUCompat {

struct Sha1 {
    uint32_t h[5];
    uint64_t bits;
    uint8_t buffer[64];
    size_t used;
};

static inline uint32_t rol(uint32_t v, unsigned bits) {
    return (v << bits) | (v >> (32u - bits));
}

static void sha1Init(Sha1* c) {
    c->h[0] = 0x67452301u; c->h[1] = 0xEFCDAB89u; c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u; c->h[4] = 0xC3D2E1F0u;
    c->bits = 0; c->used = 0;
}

static void sha1Block(Sha1* c, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t k, t;
        if (i < 20)      { k = 0x5A827999u; t = (b & cc) | (~b & d); }
        else if (i < 40) { k = 0x6ED9EBA1u; t = b ^ cc ^ d; }
        else if (i < 60) { k = 0x8F1BBCDCu; t = (b & cc) | (b & d) | (cc & d); }
        else             { k = 0xCA62C1D6u; t = b ^ cc ^ d; }
        uint32_t tmp = rol(a, 5) + t + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1Update(Sha1* c, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    c->bits += (uint64_t)len * 8u;
    while (len) {
        size_t take = 64u - c->used;
        if (take > len) take = len;
        memcpy(c->buffer + c->used, p, take);
        c->used += take; p += take; len -= take;
        if (c->used == 64u) { sha1Block(c, c->buffer); c->used = 0; }
    }
}

static void sha1Final(Sha1* c, uint8_t out[20]) {
    uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    sha1Update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56u) sha1Update(c, &zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = (uint8_t)(bits >> (56 - i*8));
    for (int i = 0; i < 8; ++i) c->buffer[56 + i] = tail[i];
    sha1Block(c, c->buffer);
    c->used = 0;
    for (int i = 0; i < 5; ++i) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

/* PBKDF2-HMAC-SHA1, single dkLen<=64 block, standard 4096 WPA rounds. */
static void pbkdf2Sha1(const char* pass, size_t passLen,
                       const uint8_t* salt, size_t saltLen,
                       uint8_t out[32]) {
    uint8_t key[64];
    memset(key, 0, sizeof(key));
    if (passLen > sizeof(key)) passLen = sizeof(key);
    memcpy(key, pass, passLen);
    for (size_t i = 0; i < sizeof(key); ++i) key[i] ^= 0x36u;
    Sha1 ipad; sha1Init(&ipad); sha1Update(&ipad, key, sizeof(key));
    for (size_t i = 0; i < sizeof(key); ++i) key[i] ^= 0x36u ^ 0x5Cu;
    Sha1 opad; sha1Init(&opad); sha1Update(&opad, key, sizeof(key));
    memset(key, 0, sizeof(key));

    uint8_t saltBlock[40];
    if (saltLen > sizeof(saltBlock)) saltLen = sizeof(saltBlock);
    memcpy(saltBlock, salt, saltLen);
    saltBlock[saltLen]   = 0; saltBlock[saltLen+1] = 0;
    saltBlock[saltLen+2] = 0; saltBlock[saltLen+3] = 1;

    uint8_t u[20], sum[20];
    Sha1 inner = ipad; sha1Update(&inner, saltBlock, saltLen + 4u);
    sha1Final(&inner, u);
    Sha1 outer = opad; sha1Update(&outer, u, 20);
    sha1Final(&outer, sum);
    memcpy(out, sum, 20);
    for (uint32_t round = 1; round < 4096u; ++round) {
        Sha1 a = ipad; sha1Update(&a, sum, 20); sha1Final(&a, u);
        Sha1 b = opad; sha1Update(&b, u, 20); sha1Final(&b, sum);
        for (int i = 0; i < 20; ++i) out[i] ^= sum[i];
    }
    /* second 32-20=12 bytes: only needed for keys >20 bytes; PMK is 32.
     * Repeat the chain for block 2 (count=2). */
    uint8_t out2[20];
    memset(out2, 0, sizeof(out2));
    saltBlock[saltLen+3] = 2;
    Sha1 i2 = ipad; sha1Update(&i2, saltBlock, saltLen + 4u); sha1Final(&i2, u);
    Sha1 o2 = opad; sha1Update(&o2, u, 20); sha1Final(&o2, sum);
    memcpy(out2, sum, 20);
    for (uint32_t round = 1; round < 4096u; ++round) {
        Sha1 a = ipad; sha1Update(&a, sum, 20); sha1Final(&a, u);
        Sha1 b = opad; sha1Update(&b, u, 20); sha1Final(&b, sum);
        for (int i = 0; i < 20; ++i) out2[i] ^= sum[i];
    }
    memcpy(out + 20, out2, 12);
    memset(u, 0, sizeof(u)); memset(sum, 0, sizeof(sum));
    memset(out2, 0, sizeof(out2));
    ipad.h[0]=0; ipad.h[1]=0; ipad.h[2]=0; ipad.h[3]=0; ipad.h[4]=0;
    opad.h[0]=0; opad.h[1]=0; opad.h[2]=0; opad.h[3]=0; opad.h[4]=0;
}

/* ------------------------------------------------------------------ */
/* Load/store helpers for the little-endian fixed-layout frames.       */
/* ------------------------------------------------------------------ */
static inline uint32_t load32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void store32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void store16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* Pending association state captured from the Realtek OID stream. */
struct JoinState {
    char     ssid[RTW_CONTROL_MAX_SSID + 1];
    uint8_t  ssidLength;
    uint32_t authMode;
    char     passphrase[64];
    uint8_t  passphraseLength;
    bool     hasSsid;
    bool     hasAuth;
    uint8_t  bssid[6];   /* mejoras: desired AP, valid when hasBssid */
    bool     hasBssid;
};

static JoinState g_joinState{};

/* One-shot diagnostic guard: the first IF_STATUS query of each kext load
 * logs the live device state so the utility gate can be verified from the
 * kernel log without adding per-poll noise. */
static bool gateLogged_ = false;

/* Monotonic uptime in milliseconds. Identifiers must avoid the vendored
 * OpenBSD compat layer's macro names (e.g. `ticks` in _clock.h is a
 * statement-expression macro). RTWUSBController::nowMS() does the same
 * conversion but is private to the controller, hence this local twin. */
static uint64_t uptimeMS() {
    uint64_t abstime = 0, nanos = 0;
    clock_get_uptime(&abstime);
    absolutetime_to_nanoseconds(abstime, &nanos);
    return nanos / 1000000u;
}

/* The utility arms a sweep with SetInformation(0xFF07011A) and then spins
 * on QID 0xFF0101BD until it reads zero; the stock driver holds the flag
 * non-zero while the radio sweeps. net80211 sweeps asynchronously (about
 * 200 ms per channel) and re-arms itself forever when nothing matches, so
 * the raw scan-state bit can never be the ready signal: with only
 * encrypted APs in range the station stays parked in S_SCAN indefinitely.
 * The busy answer is therefore node-count based: stay busy only while the
 * sweep window is open AND the node tree is still empty. The poll releases
 * the moment any network appears - earlier than the stock driver's
 * full-sweep answer the utility was built around, so the utility's own
 * poll patience always suffices - and the deadline bounds the poll when
 * the radio hears nothing at all. Field round 2 showed an empty menu even
 * with every call reaching the driver: if the utility abandons its poll
 * before a 5 s window expires, a pure deadline answer reads as empty, and
 * this release-on-first-node shape removes that race entirely. Each cycle
 * logs its trigger, first poll and release for field diagnosis. All
 * accesses run under the controller's IOCommandGate. */
static const uint64_t kScanBusyWindowMS = 5000u;
static uint64_t g_scanBusyDeadlineMS = 0;
static uint64_t g_scanTriggerMS = 0;
static bool g_scanPollLogged = false;
static bool g_scanReadyLogged = false;

/* Last value the link-state answers (0xFF819048/0xFF819053) reported;
 * the transition boundary is the only thing worth logging, the watchdog
 * polls these every few seconds. The paired state tracker logs every
 * net80211 state step, not just the RUN flip. */
static bool g_linkAnswerLogged = false;
static uint32_t g_lastAnsweredState = 0xFFFFFFFFu;

/* Transition log for OID_RT_NETWORK_STATUS (0x00010114), the utility's
 * GetConnectionStatus and the FIRST gate of NicIFLinkStatusWatchdog. */
static bool g_netStatusLogged = false;
static uint32_t g_lastNetStatusState = 0xFFFFFFFFu;

/* Transition log for the RSSI percent answer (0x0D010206): log only when
 * the percent value changes so the field capture shows the bar ladder
 * without flooding the ring on every watchdog tick. */
static int32_t g_rssiPercentLogged = -1;

/* One-shot log for survey-record fetches that do not match the decoded
 * utility shape: field round 4 proved the driver holds 21 cached networks
 * while the menu stayed empty, so the remaining suspects are the record
 * fetch shape and the app-side list build. A silent shape rejection made
 * the first one undiagnosable - log the caller's actual shape once. */
static bool g_recordShapeLogged = false;

/* First-sight logging for OIDs outside the decoded set.
 *
 * Unknown QUERIES keep answering unsupported - the utility reads that as
 * "optional feature absent" and hides the feature, which is exactly how it
 * behaves against the stock driver - but each distinct OID is logged once
 * so a field log lists everything the utility still asks for.
 *
 * Unknown SETS are answered success and ignored. The stock driver accepts
 * the registry-style sets it does not implement, and field round 5 showed
 * the utility treats a failed set as a fatal command error on the
 * association path: its WirelessAssociate streams 0xFF01041B (SSID) as a
 * set, so an unsupported answer killed the join before any radio traffic
 * (the RTWDiag counters stayed at zero: no TX, no EAPOL). Accepting the
 * unknown sets removes every remaining hard failure from that path. */
static uint32_t g_seenUnsupportedQueries[16];
static unsigned g_seenUnsupportedQueryCount;
static uint32_t g_seenIgnoredSets[16];
static unsigned g_seenIgnoredSetCount;

/* Returns true the first time an OID value is seen (and remembers it; the
 * table tail is reused once full so late OIDs stay observable). */
static bool firstSight(uint32_t* table, unsigned* count, uint32_t oid) {
    for (unsigned i = 0; i < *count; ++i)
        if (table[i] == oid) return false;
    if (*count < 16u) table[(*count)++] = oid;
    else table[15u] = oid;
    return true;
}

/* handleControl validates every control call against the v1 request
 * contract: a 16-byte RTWControlRequest header whose version field is set
 * and whose reserved bytes are zero. Null or zeroed input fails that
 * validation before the selector ever dispatches, which silently disabled
 * the whole shim-to-driver path (scan trigger, survey count, survey
 * records, live status). Every headerless call below therefore carries
 * the proper request header; payload-bearing calls (JOIN) build their own
 * versioned structure. */
static IOReturn controlCall(RTWUSBController* controller, uint32_t selector,
                            void* output, size_t* outputLength) {
    RTWControlRequest request{};
    request.version = RTW_CONTROL_VERSION;
    return controller->controlRequest(selector, &request, sizeof(request),
                                      output, outputLength);
}

static void resetJoin(JoinState* s) {
    memset(s, 0, sizeof(*s));
}

/* Map RTW_CONTROL_SECURITY_* flags to the auth byte (+0x23) the utility
 * understands. Field round 10: this is the utility's own enum (vendor utility
 * _CmdAkm, GUI @0x10003c770), whose numbers double as the join-dialog
 * dropdown tags — 0 open, 1 shared WEP, 3/4 WPA-PSK (TKIP/AES), 5/6
 * WPA2-PSK (TKIP/AES). WirelessAssociate echoes this byte into the
 * 0xFF010194 auth-mode set, so it must stay inside the utility's enum or
 * the join stream carries a mode the utility never offered. */
static uint8_t securityToAuth(uint32_t securityFlags) {
    if (securityFlags & RTW_CONTROL_SECURITY_WPA2_PSK)
        return RTWLANU_AUTH_WPA2PSK;          /* 6 = WPA2-PSK AES tag */
    if (securityFlags & RTW_CONTROL_SECURITY_WPA2_PSK_TKIP)
        return RTWLANU_AUTH_WPA2PSK_TKIP;     /* 5 */
    if (securityFlags & RTW_CONTROL_SECURITY_WPA_PSK_CCMP)
        return RTWLANU_AUTH_WPAPSK_AES;       /* 4 */
    if (securityFlags & RTW_CONTROL_SECURITY_WPA_PSK)
        return RTWLANU_AUTH_WPAPSK;           /* 3 */
    if (securityFlags & RTW_CONTROL_SECURITY_PRIVACY)
        return RTWLANU_AUTH_SHARED;           /* 1 = shared WEP */
    return RTWLANU_AUTH_OPEN;
}

/* Pack one RTWControlScanRecord into the utility's 0x640-byte record.
 *
 * Field round 9 (2026-11-08): the utility's join-dialog classifier was
 * disassembled to instruction level (fieldCtrl_encryTypeSetting:
 * preferrAuth:AKMsuit:NetType:, GUI @0x100051ea0) and its Site_Encry
 * ladder recovered exactly:
 *
 *   Site_Encry >= 0x40 -> WPA2-AES class: AKMsuit & 0x20 -> "WPA2-PSK AES"
 *                         (dialog tag 6, the auto password box Monterey
 *                         shows), AKMsuit & 0x10 -> WPA2-Enterprise-AES,
 *                         AKMsuit & 0x1000 -> WPA3 SAE.
 *   0x20..0x3f         -> WPA2-TKIP class: AKMsuit & 0x20 -> "WPA2-PSK
 *                         TKIP" (tag 5), & 0x10 -> WPA2-Enterprise-TKIP.
 *   4..0x1f            -> WPA-AES class: AKMsuit & 0x2 -> "WPA-PSK AES",
 *                         & 0x1 -> WPA-Enterprise-AES.
 *   2..3               -> WPA-TKIP class: AKMsuit & 0x2 -> "WPA-PSK TKIP".
 *   1                  -> WEP (open/shared picked from the auth byte).
 *   0                  -> open/none ([Case 1][OPEN_NONE] join path).
 *   0xff               -> WPA-None (adhoc) special case.
 *
 * +0x230 AKMsuit is NOT an 802.11 AKM suite selector — it is the
 * utility's capability bitmask (0x1 WPA-1X, 0x2 WPA-PSK, 0x10 WPA2-1X,
 * 0x20 WPA2-PSK, 0x1000 SAE). Round 8 packed "suite selector 2", which
 * left every high-class branch untaken: WPA2 networks showed the legacy
 * fallback box (WEP/WPA-TKIP options only, no WPA2-PSK AES) and every
 * manual pick failed. Round 8's Site_Encry=3 also landed in the 2..3
 * WPA-TKIP class for the same reason.
 *
 * The driver now reports the finer RTW_CONTROL_SECURITY_* classes and
 * this packer translates them onto the ladder. RSSI is packed nowhere —
 * the GUI's signal readout consumes +0x234 SignalStrength. */
static void packSurveyRecord(const RTWControlScanRecord* in, uint8_t* out) {
    memset(out, 0, RTWLANUC_SURVEY_RECORD);
    uint32_t ssidLength = in->ssidLength;
    if (ssidLength > RTW_CONTROL_MAX_SSID) ssidLength = RTW_CONTROL_MAX_SSID;
    memcpy(out + RTWLANU_REC_SSID_OFFSET, in->ssid, ssidLength);
    out[RTWLANU_REC_SSIDLEN_OFFSET] = (uint8_t)ssidLength;
    out[RTWLANU_REC_NETTYPE_OFFSET] = 0; /* station scan: infrastructure */
    out[RTWLANU_REC_AUTH_OFFSET] = securityToAuth(in->securityFlags);
    memcpy(out + RTWLANU_REC_BSSID_OFFSET, in->bssid, 6);
    store16(out + RTWLANU_REC_CHANNEL_OFFSET, (uint16_t)in->channel);
    uint8_t siteEncry = 0; /* open / none */
    uint32_t akmSuit = 0;
    if (in->securityFlags & RTW_CONTROL_SECURITY_WPA2_PSK) {
        siteEncry = 0x40; /* SITE_WPA2_AES class */
        akmSuit = 0x20;   /* utility AKM bit: WPA2-PSK */
    } else if (in->securityFlags & RTW_CONTROL_SECURITY_WPA2_PSK_TKIP) {
        siteEncry = 0x20; /* SITE_WPA2_TKIP class */
        akmSuit = 0x20;
    } else if (in->securityFlags & RTW_CONTROL_SECURITY_WPA_PSK_CCMP) {
        siteEncry = 4;  /* SITE_WPA_AES class */
        akmSuit = 0x2;  /* utility AKM bit: WPA-PSK */
    } else if (in->securityFlags & RTW_CONTROL_SECURITY_WPA_PSK) {
        siteEncry = 2; /* SITE_WPA_TKIP class */
        akmSuit = 0x2;
    } else if (in->securityFlags & RTW_CONTROL_SECURITY_PRIVACY) {
        siteEncry = 1; /* WEP; open/shared follows the auth byte */
    }
    out[RTWLANU_REC_SITE_ENCRY_OFFSET] = siteEncry;
    store32(out + RTWLANU_REC_AKMSUIT_OFFSET, akmSuit);
    int32_t rssi = in->rssi;
    int32_t percent = (rssi + 100) * 2; /* -100 dBm -> 0 %, -50 dBm -> 100 % */
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    out[RTWLANU_REC_SIGNALSTRENGTH_OFFSET] = (uint8_t)percent;
}

/* Answer one OID query into the 0x9d4 frame. */
static IOReturn oidQuery(RTWUSBController* controller, RTWlanUOidFrame* frame,
                         const RTWControlStatus* status) {
    frame->status = 0;
    switch (frame->oid) {
    case RTWLANU_QID_PERMANENT_ADDRESS: {
        /* GetPermanentAddress: feeds the first six bytes through a hex
         * formatter to build the per-adapter key of every settings file
         * ("<mac>rfoff.rtl", profile storage). The attach path refuses to
         * publish a zero or multicast MAC, so status->mac is always a valid
         * adapter address here. */
        if (frame->length < 6u) { frame->retLen = 0; return kIOReturnBadArgument; }
        if (status) memcpy(frame->data, status->mac, 6);
        else memset(frame->data, 0, 6);
        frame->retLen = 6;
        return kIOReturnSuccess;
    }
    case RTWLANU_QID_SURVEY_COUNT: {
        RTWControlScanResults results{};
        size_t outLen = sizeof(results);
        uint8_t raw[sizeof(results)];
        IOReturn r = controlCall(controller, RTW_CONTROL_SCAN_RESULTS,
            raw, &outLen);
        if (r != kIOReturnSuccess) { frame->retLen = 0; frame->status = r; return r; }
        memcpy(&results, raw, sizeof(results));
        IOLog("RtWlanUCompat: survey count query -> %u\n", results.count);
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, results.count);
        frame->retLen = 4;
        return kIOReturnSuccess;
    }
    case RTWLANU_QID_SCAN_BUSY: {
        /* IsScanResultReady decoded from vendor utility: value != 0 means the
         * sweep is still running and the utility keeps polling; zero lets it
         * read the survey count and the records. See the window comment
         * above for the release conditions: any node in the tree, or the
         * deadline. The per-cycle logs mark the first poll (with the live
         * scan-state bit) and the release so one menu cycle pins down
         * exactly where a field failure sits. */
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        uint32_t busy = 0;
        uint32_t found = 0;
        const uint32_t flags = status ? status->flags : 0;
        if (uptimeMS() < g_scanBusyDeadlineMS) {
            RTWControlScanResults results{};
            size_t outLen = sizeof(results);
            uint8_t raw[sizeof(results)];
            if (controlCall(controller, RTW_CONTROL_SCAN_RESULTS,
                            raw, &outLen) == kIOReturnSuccess) {
                memcpy(&results, raw, sizeof(results));
                found = results.count;
            }
            if (!found) busy = 1u;
        }
        if (!g_scanPollLogged) {
            g_scanPollLogged = true;
            IOLog("RtWlanUCompat: first busy poll, busy=%u found=%u scanning=%u\n",
                  busy, found, (unsigned)!!(flags & RTW_CONTROL_SCANNING));
        } else if (!busy && !g_scanReadyLogged) {
            g_scanReadyLogged = true;
            IOLog("RtWlanUCompat: busy released after %u ms, found=%u\n",
                  (unsigned)(uptimeMS() - g_scanTriggerMS), found);
        }
        store32(frame->data, busy);
        frame->retLen = 4;
        return kIOReturnSuccess;
    }
    case RTWLANU_QID_CHANNEL:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, status ? status->channel : 0);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_DRIVER_VERSION:
        frame->retLen = (uint32_t)strlen(RTWLANU_DRIVER_VERSION);
        memcpy(frame->data, RTWLANU_DRIVER_VERSION, frame->retLen);
        return kIOReturnSuccess;
    case RTWLANU_QID_IF_STATUS:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        /* The utility's startup gate (IsRTL8180Enabled / CmdIfStatus): the
         * NOTICE8 "enable the card in Network preferences" loop. The stock
         * RtWlanU driver answers enabled as soon as the utility holds an open
         * user client - the gate means "driver bound", not "radio running".
         * Gating on RTW_CONTROL_RUNNING or RTW_CONTROL_DEVICE keeps the
         * utility in the notice loop whenever the radio is between states,
         * and the user client connection itself proves the driver is bound,
         * so answer 1 unconditionally. The one-shot log marks the boot and
         * surfaces the real device state for diagnosis. */
        if (!gateLogged_) {
            gateLogged_ = true;
            IOLog("RtWlanUCompat: IF_STATUS query, device=%u radio=%u running=%u\n",
                  status ? (unsigned)!!(status->flags & RTW_CONTROL_DEVICE) : 0u,
                  status ? (unsigned)!!(status->flags & RTW_CONTROL_RADIO) : 0u,
                  status ? (unsigned)!!(status->flags & RTW_CONTROL_RUNNING) : 0u);
        }
        store32(frame->data, 1u);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_HW_RF_OFF:
        /* Hardware RF-kill switch readout (IsRfOff:/turnRfOn/turnRfOff and
         * CheckSoftRF). USB dongles carry no such switch, so the radio is
         * always available. Answering unsupported made the utility treat
         * the switch state as unknown and skip its scan. */
        if (!frame->length) { frame->retLen = 0; return kIOReturnBadArgument; }
        frame->data[0] = 0;
        frame->retLen = 1;
        return kIOReturnSuccess;
    case RTWLANU_QID_ENCRYPTION_STATUS:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        /* NDIS encryption-status readback; the association path fixes
         * WPA2-CCMP and the utility re-sets this value before joining. */
        store32(frame->data, 3u); /* Ndis802_11Encryption3Enabled (AES) */
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_AUTH_MODE:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, g_joinState.hasAuth ? g_joinState.authMode
                                                 : (uint32_t)RTWLANU_AUTH_OPEN);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_CURRENT_SSID:
        /* NDIS semantics: the SSID of the current association. The status
         * snapshot carries ic_bss's SSID, which only counts as "linked"
         * while the station is in IEEE80211_S_RUN (4); outside RUN it is
         * the desired target, so fall back to the pending join state
         * (stock answers the desired SSID while not yet associated).
         * Field round 7: this used to answer only the pending join state,
         * which performJoin wipes on success - so even a linked station
         * reported an empty SSID and the utility never saw the join. */
        if (status && status->state == 4u /* IEEE80211_S_RUN */
            && status->ssidLength) {
            uint32_t linked = status->ssidLength;
            if (linked > RTW_CONTROL_MAX_SSID) linked = RTW_CONTROL_MAX_SSID;
            memcpy(frame->data, status->ssid, linked);
            frame->retLen = linked;
            return kIOReturnSuccess;
        }
        frame->retLen = g_joinState.hasSsid ? g_joinState.ssidLength : 0u;
        memcpy(frame->data, g_joinState.ssid, frame->retLen);
        return kIOReturnSuccess;
    case RTWLANU_QID_HARDWARE_TYPE:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, 0);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_WIRELESS_MODE:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, (status && (status->flags & RTW_CONTROL_BAND_2GHZ))
                                 ? 5u /* b+g mask */ : 5u);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_JOIN_CONDITION:
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, 1u);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_NETWORK_STATUS:
        /* OID_RT_NETWORK_STATUS (0x00010114): -[WLANClientUtilityModel
         * GetConnectionStatus] (GUI @0x10001cb10) reads this dword and
         * NicIFLinkStatusWatchdog's first instruction-level gate is
         * "dword == 1 else BarImageMode_Disconnect + case2 reconnect".
         * Every earlier field round left it unmapped, so the gate failed
         * every tick even with the station in RUN. 1 = linked. */
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        {
            const bool linked = status && status->state == 4u;
            const uint32_t state = status ? status->state : 0u;
            if (linked != g_netStatusLogged || state != g_lastNetStatusState) {
                g_netStatusLogged = linked;
                g_lastNetStatusState = state;
                IOLog("RtWlanUCompat: network status 0x00010114 -> %u (state=%u)\n",
                      linked ? 1u : 0u, (unsigned)state);
            }
            store32(frame->data, linked ? 1u : 0u);
        }
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_JOIN_STATE:
        /* 1.0.8 (RTLWiFiBar 8.7.14): the join monitor polls this while a
         * user join is in flight so the app log shows exactly how far the
         * association got - scanning, authenticating, associating, RUN -
         * instead of a silent "accepted" that never moves. State values
         * are the net80211 station states; beacon age distinguishes a
         * healthy RUN from a zombie. Transitions are logged so one field
         * capture pins down the furthest state a dead join reached. */
        if (frame->length < 8u) { frame->retLen = 0; return kIOReturnBadArgument; }
        {
            const uint32_t state = status ? status->state : 0u;
            static uint32_t joinStateReported = 0xFFFFFFFFu;
            if (state != joinStateReported) {
                joinStateReported = state;
                IOLog("RtWlanUCompat: join state report -> %u "
                      "(auth=%u keylen=%u ssidlen=%u)\n",
                      (unsigned)state, (unsigned)g_joinState.authMode,
                      (unsigned)g_joinState.passphraseLength,
                      (unsigned)g_joinState.ssidLength);
            }
            store32(frame->data, state);
            store32(frame->data + 4, status ? status->beaconAgeMs : 0u);
        }
        frame->retLen = 8;
        return kIOReturnSuccess;
    case RTWLANU_QID_LINK_STATUS:
    case RTWLANU_QID_ASSOC_STATE:
        /* NicIFLinkStatusWatchdog polls these to decide the bar image and
         * whether to keep re-associating. The utility reads the FIRST byte
         * of the answer: 1 = linked (supplicant success), 3 = 4-way fail.
         * IEEE80211_S_RUN (4) is the only state in which the association is
         * usable, so that is the linked answer. Field round 6 log: with no
         * mapping the helper zeroed both answers, the watchdog reset to
         * BarImageMode_Disconnect every tick and the "case2" reconnect
         * loop re-joined every ~12 s forever. Transitions are logged so one
         * field capture shows whether the station ever reaches RUN. */
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        {
            const bool linked = status && status->state == 4u;
            const uint32_t state = status ? status->state : 0u;
            /* Log the answer on every visible change: the answer flip AND
             * each net80211 state step (1=SCAN 2=AUTH 3=ASSOC 4=RUN), so a
             * field capture shows exactly how far a join progressed when
             * the station never reaches RUN. */
            if (linked != g_linkAnswerLogged || state != g_lastAnsweredState) {
                char ssid[RTW_CONTROL_MAX_SSID + 1];
                const uint32_t len = status ?
                    (status->ssidLength < RTW_CONTROL_MAX_SSID ?
                        status->ssidLength : RTW_CONTROL_MAX_SSID) : 0u;
                if (status) memcpy(ssid, status->ssid, len);
                ssid[len] = 0;
                g_linkAnswerLogged = linked;
                g_lastAnsweredState = state;
                IOLog("RtWlanUCompat: link answer -> %s (state=%u ssid=%s)\n",
                      linked ? "connected" : "disconnected",
                      (unsigned)state, ssid);
            }
            store32(frame->data, linked ? 1u : 0u);
        }
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_INFRA_MODE:
        /* NDIS OID_802_11_INFRASTRUCTURE_MODE readback issued right after
         * the utility's Disconnected-to-Connected transition: 1 =
         * Ndis802_11Infrastructure (the only mode this station drives). */
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        store32(frame->data, 1u);
        frame->retLen = 4;
        return kIOReturnSuccess;
    case RTWLANU_QID_BSSID:
        /* NDIS OID_802_11_BSSID: the current association's BSSID, consumed
         * by the helper's GetBSSID: (StatusWatchdog "[9][PostLink]" log and
         * the profile BSSIDKey). Zero while not associated. */
        if (frame->length < 6u) { frame->retLen = 0; return kIOReturnBadArgument; }
        if (status) memcpy(frame->data, status->bssid, 6);
        else memset(frame->data, 0, 6);
        frame->retLen = 6;
        return kIOReturnSuccess;
    case RTWLANU_QID_RSSI:
        /* NDIS OID_802_11_RSSI. Decoded utility path (field round 12, see
         * the header note): getSignalStrength = QueryInformation(0x0D010206);
         * the watchdog passes the low byte to SetStatusBarImage:param: whose
         * mode-0 ladder is a percent scale and paints BarSignal_0 on zero.
         * Rescale the live dBm with the survey percent formula so the byte
         * lands on the same 0..100 scale the utility expects. Zero stays
         * "unknown" - correct while not associated. */
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        {
            int32_t percent = 0;
            if (status && status->rssi) {
                percent = ((int32_t)status->rssi + 100) * 2;
                if (percent < 0) percent = 0;
                if (percent > 100) percent = 100;
            }
            if (percent != g_rssiPercentLogged) {
                g_rssiPercentLogged = percent;
                IOLog("RtWlanUCompat: rssi query -> %d%% (dbm=%d)\n",
                      (int)percent, status ? (int)status->rssi : 0);
            }
            store32(frame->data, (uint32_t)percent);
        }
        frame->retLen = 4;
        return kIOReturnSuccess;
    default:
        if (firstSight(g_seenUnsupportedQueries, &g_seenUnsupportedQueryCount,
                       frame->oid))
            IOLog("RtWlanUCompat: query 0x%08x unsupported (no mapping)\n",
                  frame->oid);
        frame->retLen = 0;
        frame->status = kIOReturnUnsupported;
        return kIOReturnUnsupported;
    }
}

/* Perform the association with everything the OID stream collected. */
static IOReturn performJoin(RTWUSBController* controller) {
    JoinState* s = &g_joinState;
    if (!s->hasSsid || !s->ssidLength) {
        /* Build 67: an explicit trigger after the auto-join already ran
         * is normal (the state was consumed). Answer success so the
         * utility never reads the duplicate trigger as a fatal error. */
        IOLog("RtWlanUCompat: join trigger with no pending state (already joined), ok\n");
        return kIOReturnSuccess;
    }

    /* Build 69 field capture: the utility's watchdog re-sends the whole
     * profile about 11 s after every successful join (station reached RUN
     * at 156.4 s, identical profile re-submitted at 163.5 s). Re-running
     * the join tears down a healthy association every cycle, so the link
     * flaps, DHCP never finishes and the app flips green -> scanning
     * forever. While the station is already associated with this exact
     * SSID, answer success and leave the live link untouched. */
    {
        RTWControlStatus live{};
        uint8_t liveRaw[sizeof(live)]{};
        size_t liveLen = sizeof(live);
        if (controlCall(controller, RTW_CONTROL_STATUS, liveRaw,
                        &liveLen) == kIOReturnSuccess) {
            memcpy(&live, liveRaw, sizeof(live));
            if (live.state == 4u /* IEEE80211_S_RUN */ &&
                live.ssidLength && live.ssidLength == s->ssidLength &&
                memcmp(live.ssid, s->ssid, live.ssidLength) == 0) {
                /* 1.0.12: the beacon age decides. Fresh beacons (< 8 s)
                 * with the carrier UP are answered idempotent by the kext;
                 * a zombie RUN (no beacons) falls into the real rejoin
                 * path below. The kext-side guard owns the decision. */
                IOLog("RtWlanUCompat: join submit while RUN on %s "
                      "(beacon age %u ms) - kext guard decides\n",
                      s->ssid, live.beaconAgeMs);
            }
        }
    }

    RTWControlJoin join{};
    join.version = RTW_CONTROL_VERSION;
    join.ssidLength = s->ssidLength;
    memcpy(join.ssid, s->ssid, s->ssidLength);
    if (s->hasBssid) { /* mejoras: pin the AP the user picked */
        uint8_t* rsv = reinterpret_cast<uint8_t*>(join.reserved);
        memcpy(rsv, s->bssid, 6);
        rsv[6] = RTW_CONTROL_JOIN_BSSID_SET;
        IOLog("RtWlanUCompat: join pinned to bssid %02x:%02x:%02x:%02x:%02x:%02x\n",
              s->bssid[0], s->bssid[1], s->bssid[2], s->bssid[3], s->bssid[4], s->bssid[5]);
    }

    /* Field round 10 root cause: the utility writes its OWN auth enum to
     * 0xFF010194 (decoded from _CmdAkm @0x10003c770): 0 open, 1/2 shared
     * WEP, 3/4 WPA-PSK, 5/6 WPA2-PSK — dropdown tag == auth value, so the
     * WPA2-PSK AES selection arrives as auth=6. Builds <=59 derived the
     * PMK only for the Windows-enum values {4,7}, so every WPA2-PSK AES
     * join silently ran with a zero PMK: the driver disarmed RSN, the AP
     * never completed the link, and the utility's retry loop fell back to
     * the green idle icon. PSK is now anything in 3..7 (7 = the legacy
     * constant, still accepted for cached profiles that echo it). */
    const bool pskJoin = (s->authMode >= RTWLANU_AUTH_WPAPSK &&
                          s->authMode <= RTWLANU_AUTH_PSK_MAX);
    if (pskJoin) {
        if (!s->passphraseLength) {
            IOLog("RtWlanUCompat: join trigger for %s without WPA key "
                  "(auth=%u), refusing\n", s->ssid, s->authMode);
            return kIOReturnNotReady;
        }
        pbkdf2Sha1(s->passphrase, s->passphraseLength,
                   reinterpret_cast<const uint8_t*>(s->ssid), s->ssidLength,
                   join.pmk);
    }
    /* OPEN (0) and shared-WEP (1/2) classes join with a zero PMK; the kext
     * treats that as unauthenticated association, matching the utility's
     * OPEN path. WEP itself is outside the RTWControl v1 JOIN contract. */

    IOLog("RtWlanUCompat: join submit ssid=%s auth=%u keylen=%u pmk=%s\n",
          s->ssid, s->authMode, (unsigned)s->passphraseLength,
          pskJoin ? "derived" : "zero");
    uint8_t raw[80];
    memcpy(raw, &join, sizeof(join));
    size_t outLen = sizeof(RTWControlReply);
    uint8_t reply[16];
    IOReturn r = controller->controlRequest(RTW_CONTROL_JOIN, raw, sizeof(raw),
                                            reply, &outLen);
    IOLog("RtWlanUCompat: join submit -> 0x%x\n", r);
    memset(raw, 0, sizeof(raw));
    if (r == kIOReturnSuccess) resetJoin(s);
    return r;
}

/* Handle one OID set frame. Returns kIOReturnSuccess with retLen/status
 * filled, or an error the utility's caller maps to a failed command. */

/* Build 67: the utility's join stream ends at the SSID set - build 66
 * dmesg shows the key/auth/ssid OIDs landing and no assoc trigger ever
 * arriving, leaving net80211 scanning forever (1015 channel hops, no
 * auth state, zero EAPOL). Run the join as soon as the pending state is
 * complete; the explicit trigger still works and is a benign no-op if
 * the auto-join already consumed the state. */
static void tryAutoJoin(RTWUSBController* controller) {
    JoinState* s = &g_joinState;
    if (!s->hasSsid || !s->ssidLength || !s->hasAuth) return;
    const bool psk = (s->authMode >= RTWLANU_AUTH_WPAPSK &&
                      s->authMode <= RTWLANU_AUTH_PSK_MAX);
    if (psk && !s->passphraseLength) return;
    IOReturn r = performJoin(controller);
    IOLog("RtWlanUCompat: auto-join -> 0x%x\n", r);
}

static IOReturn oidSet(RTWUSBController* controller, RTWlanUOidFrame* frame) {
    frame->status = 0;
    switch (frame->oid) {
    case RTWLANU_SID_SCAN_TRIGGER: {
        /* Arm the busy window before the sweep call: the trigger itself
         * blocks until the station is up (first call) and the scan state is
         * entered, so the utility's poll starts right after this returns.
         * Restart the per-cycle diagnostic logs with every trigger. */
        const uint64_t now = uptimeMS();
        g_scanBusyDeadlineMS = now + kScanBusyWindowMS;
        g_scanTriggerMS = now;
        g_scanPollLogged = false;
        g_scanReadyLogged = false;
        size_t outLen = sizeof(RTWControlReply);
        uint8_t reply[16];
        IOReturn r = controlCall(controller, RTW_CONTROL_SCAN, reply, &outLen);
        IOLog("RtWlanUCompat: scan trigger -> 0x%x\n", r);
        if (r == kIOReturnBusy) r = kIOReturnSuccess; /* already sweeping */
        if (r != kIOReturnSuccess) { frame->retLen = 0; frame->status = r; }
        else frame->retLen = 4;
        return r;
    }
    case RTWLANU_SID_NETWORK_NAME: {
        uint32_t len = frame->length;
        if (len > RTW_CONTROL_MAX_SSID) len = RTW_CONTROL_MAX_SSID;
        uint32_t real = 0;
        while (real < len && frame->data[real] >= 0x20 &&
               frame->data[real] <= 0x7e) real++;
        len = real; /* padded fixed-width field: empty when all-NUL */
        resetJoin(&g_joinState);
        memcpy(g_joinState.ssid, frame->data, len);
        g_joinState.ssid[len] = 0;
        g_joinState.ssidLength = (uint8_t)len;
        g_joinState.hasSsid = len != 0;
        frame->retLen = len;
        return kIOReturnSuccess;
    }
    case RTWLANU_SID_DESIRED_BSSID: {
        /* mejoras: never triggers the join by itself. */
        if (frame->length < 6u) { frame->retLen = 0; return kIOReturnBadArgument; }
        bool any = false;
        for (unsigned i = 0; i < 6u; ++i) if (frame->data[i]) any = true;
        memcpy(g_joinState.bssid, frame->data, 6);
        g_joinState.hasBssid = any;
        frame->retLen = 6;
        IOLog("RtWlanUCompat: desired bssid %s\n", any ? "set" : "cleared");
        return kIOReturnSuccess;
    }
    case RTWLANU_QID_AUTH_MODE:
    case RTWLANU_SID_HIBERNATE_MODE: {
        if (frame->length < 4u) { frame->retLen = 0; return kIOReturnBadArgument; }
        g_joinState.authMode = load32(frame->data);
        g_joinState.hasAuth = true;
        frame->retLen = 4;
        IOLog("RtWlanUCompat: auth mode set: %u\n", g_joinState.authMode);
        tryAutoJoin(controller);
        return kIOReturnSuccess;
    }
    case RTWLANU_SID_WPA_KEY: {
        uint32_t len = frame->length;
        if (len > sizeof(g_joinState.passphrase) - 1u)
            len = sizeof(g_joinState.passphrase) - 1u;
        /* Build 66 dmesg: the utility sends a fixed-width passphrase field
 * (len=63) padded after the real password. PBKDF2 must see exactly
 * the typed characters or the PMK is wrong and the 4-way handshake
 * can never complete. Build 69: trim at the first non-printable byte
 * (the app also pads fields with control bytes such as 0x01). */
        uint32_t real = 0;
        while (real < len && frame->data[real] >= 0x20 &&
               frame->data[real] <= 0x7e) real++;
        len = real;
        memcpy(g_joinState.passphrase, frame->data, len);
        g_joinState.passphrase[len] = 0;
        g_joinState.passphraseLength = (uint8_t)len;
        /* Length only - never log the key material itself. */
        IOLog("RtWlanUCompat: wpa key set: len=%u\n", (unsigned)len);
        frame->retLen = len;
        tryAutoJoin(controller);
        return kIOReturnSuccess;
    }
    case RTWLANU_QID_ENCRYPTION_STATUS:
        /* Accepted and ignored: the association path fixes WPA2-CCMP. */
        frame->retLen = frame->length >= 4u ? 4u : frame->length;
        return kIOReturnSuccess;
    case RTWLANU_SID_ASSOC_TRIGGER:
        frame->retLen = 0;
        return performJoin(controller);
    case RTWLANU_SID_DISASSOCIATE: {
        /* 1.1.2: user "Disconnect USB WiFi". Drop the pending join stream
         * first so a stray auth/key set can never auto-join again, then
         * leave: softLeave() says goodbye to the AP and erases the profile;
         * the chip stays powered (a stopped chip latched core_error on the
         * next calibration tick - field log 2026-11-22 15:04). */
        resetJoin(&g_joinState);
        size_t outLen = sizeof(RTWControlReply);
        uint8_t reply[16];
        IOReturn r = controlCall(controller, RTW_CONTROL_LEAVE, reply, &outLen);
        IOLog("RtWlanUCompat: disassociate (user disconnect) -> 0x%x\n", r);
        frame->retLen = 0;
        if (r != kIOReturnSuccess) frame->status = r;
        return r;
    }
    case RTWLANU_SID_WEP_KEY:
        /* WEP is not supported by the RTWControl v1 JOIN contract; answer
         * success without state so the utility keeps operating. */
        frame->retLen = frame->length;
        return kIOReturnSuccess;
    case RTWLANU_SID_CURRENT_SSID: {
        uint32_t len = frame->length;
        if (len > RTW_CONTROL_MAX_SSID) len = RTW_CONTROL_MAX_SSID;
        const uint32_t raw = len;
        /* Build 66 dmesg: the SSID arrives as a fixed-width field padded past
 * the real name ("len=32" for a 12-char SSID). net80211 BSS matching
 * is exact-length, so the padded form never matches any beacon and
 * the stack rescans forever. Build 69: trim at the first non-printable
 * byte - the retry stream appends a 0x01 control byte after the SSID
 * (rtw-capture: ssid=Network-Wifi\x01), which NUL-trimming kept and
 * left the join hanging in S_SCAN forever. */
        uint32_t real = 0;
        while (real < len && frame->data[real] >= 0x20 &&
               frame->data[real] <= 0x7e) real++;
        len = real;
        memcpy(g_joinState.ssid, frame->data, len);
        g_joinState.ssid[len] = 0;
        g_joinState.ssidLength = (uint8_t)len;
        g_joinState.hasSsid = len != 0;
        IOLog("RtWlanUCompat: join ssid set, len=%u (raw=%u) auth=%u keylen=%u\n",
              (unsigned)len, (unsigned)raw, g_joinState.authMode,
              (unsigned)g_joinState.passphraseLength);
        frame->retLen = len;
        tryAutoJoin(controller);
        return kIOReturnSuccess;
    }
    default:
        /* Accept and ignore, matching the stock driver's handling of the
         * registry-style sets it does not implement; the utility treats a
         * failed set as a fatal command error, so no optional OID set may
         * dead-end the association stream. First sight is logged. */
        if (firstSight(g_seenIgnoredSets, &g_seenIgnoredSetCount, frame->oid))
            IOLog("RtWlanUCompat: set 0x%08x accepted, ignored (len=%u)\n",
                  frame->oid, frame->length);
        frame->retLen = frame->length;
        return kIOReturnSuccess;
    }
}

/* Entry point called from RTWUserClient::externalMethod. Returns
 * kIOReturnUnsupported when the selector/shape is not part of the Realtek
 * protocol so the caller can fall back to the RTWControl path. */
IOReturn handleExternalMethod(RTWUSBController* controller,
                              RTWUserClient* client, uint32_t selector,
                              IOExternalMethodArguments* arguments) {
    /* Build 68: one-time proof-of-life banner. dmesg67 arrived with zero
     * shim lines, so we could not tell "join evidence overwritten" from
     * "the utility never reached the shim". This line settles it: if the
     * app talks to the shim at all, this appears exactly once per boot. */
    static bool g_shimAliveLogged = false;
    if (!g_shimAliveLogged) {
        g_shimAliveLogged = true;
        IOLog("RtWlanUCompat: shim alive, driver " RTWLANU_DRIVER_VERSION
              " (utility connected)\n");
    }
    if (client->isInactive() || !controller || controller->isInactive())
        return kIOReturnNotAttached;

    switch (selector) {
    case RTWLANUC_SEL_SURVEY_RECORD: {
        /* Decoded utility shape: record index in scalarInput[0], one 0x640
         * structure output. Field round 4 showed the driver's scan cache
         * holding 21 networks while the menu stayed empty, so accept a
         * deliberately looser shape here - the index is optional (defaults
         * to 0) and extra caller arguments are tolerated - so a mis-decoded
         * detail cannot dead-end the list. Only the output buffer really
         * matters: without room for one full record there is nothing safe
         * to fill. Rejections are logged once with the live shape. */
        if (arguments->structureOutputDescriptor ||
            !arguments->structureOutput ||
            arguments->structureOutputSize < RTWLANUC_SURVEY_RECORD) {
            if (!g_recordShapeLogged) {
                g_recordShapeLogged = true;
                IOLog("RtWlanUCompat: survey record fetch rejected, "
                      "scalarIn=%u scalarOut=%u structIn=%p outSize=%u\n",
                      (unsigned)arguments->scalarInputCount,
                      (unsigned)arguments->scalarOutputCount,
                      arguments->structureInput,
                      (unsigned)arguments->structureOutputSize);
            }
            return kIOReturnUnsupported; /* not the utility's shape */
        }
        uint32_t index = 0;
        if (arguments->scalarInputCount >= 1)
            index = (uint32_t)arguments->scalarInput[0];
        if (index >= RTWLANUC_SURVEY_MAX) return kIOReturnBadArgument;

        RTWControlScanResults results{};
        uint8_t raw[sizeof(results)];
        size_t outLen = sizeof(results);
        IOReturn r = controlCall(controller, RTW_CONTROL_SCAN_RESULTS,
            raw, &outLen);
        if (r != kIOReturnSuccess) return r;
        memcpy(&results, raw, sizeof(results));
        if (index >= results.count) return kIOReturnNotFound;
        if (!index)
            IOLog("RtWlanUCompat: record 0/%u ssidlen=%u rssi=%d auth=0x%x ch=%u\n",
                  (unsigned)results.count,
                  (unsigned)results.records[0].ssidLength,
                  (int)results.records[0].rssi,
                  (unsigned)results.records[0].securityFlags,
                  (unsigned)results.records[0].channel);
        packSurveyRecord(&results.records[index],
                         static_cast<uint8_t*>(arguments->structureOutput));
        arguments->structureOutputSize = RTWLANUC_SURVEY_RECORD;
        return kIOReturnSuccess;
    }
    case RTWLANUC_SEL_DISABLED_COMP:
    case RTWLANUC_SEL_DISABLED_LEVL: {
        if (arguments->scalarInputCount || arguments->structureInput ||
            arguments->structureInputSize || arguments->scalarOutputCount != 1 ||
            arguments->structureOutput)
            return kIOReturnUnsupported; /* not the utility's shape */
        /* No commands are disabled in this implementation. */
        arguments->scalarOutput[0] = 0;
        return kIOReturnSuccess;
    }
    case RTWLANUC_SEL_SEND_PACKET:
        return kIOReturnUnsupported;
    case RTWLANUC_SEL_OID_QUERY:
    case RTWLANUC_SEL_OID_SET: {
        if (arguments->scalarInputCount || arguments->scalarOutputCount ||
            arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
            !arguments->structureInput ||
            arguments->structureInputSize != RTWLANUC_OID_FRAME_SIZE ||
            !arguments->structureOutput ||
            arguments->structureOutputSize != RTWLANUC_OID_FRAME_SIZE)
            return kIOReturnUnsupported; /* not the utility's shape */

        RTWlanUOidFrame frame;
        memcpy(&frame, arguments->structureInput, sizeof(frame));

        RTWControlStatus status{};
        uint8_t raw[sizeof(status)];
        size_t outLen = sizeof(status);
        if (controlCall(controller, RTW_CONTROL_STATUS, raw,
                        &outLen) == kIOReturnSuccess)
            memcpy(&status, raw, sizeof(status));

        IOReturn r;
        if (selector == RTWLANUC_SEL_OID_QUERY) {
            r = oidQuery(controller, &frame, &status);
        } else {
            /* Set path: the stock RtWlanU driver lets the unprivileged GUI
             * utility issue every OID in this protocol - the utility's own
             * scan/join/RF flows run directly from the menubar process, with
             * no root hop. Requiring kIOClientPrivilegeAdministrator here
             * made every association from the GUI fail silently, so the
             * whole Realtek set path follows the stock privilege model. The
             * project's own fixed-layout RTWControl client keeps its
             * administrator gate (see RTWUserClient::externalMethod). */
            r = oidSet(controller, &frame);
        }
        memset(arguments->structureOutput, 0, RTWLANUC_OID_FRAME_SIZE);
        memcpy(arguments->structureOutput, &frame, sizeof(frame));
        arguments->structureOutputSize = RTWLANUC_OID_FRAME_SIZE;
        memset(&frame, 0, sizeof(frame));
        return r;
    }
    default:
        return kIOReturnUnsupported;
    }
}

void purgePendingJoin() {
    resetJoin(&g_joinState);
}

} /* namespace RtwWlanUCompat */
