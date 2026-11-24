/*
 * RTWCompat.h - RTLWiFiBar side of the Realtek "RtWlanU" utility protocol.
 *
 * RTLWiFiBar does NOT use the kext's RTWControl v1 user-client shapes: every
 * RTWControl selector is gated on kIOClientPrivilegeAdministrator (root) by
 * RTWUserClient::externalMethod, and a LaunchAgent menu bar app runs as the
 * logged-in console user. The kext instead mirrors the stock Realtek
 * privilege model on the vendor utility protocol (Driver/RtWlanUCompat.cpp):
 * read-only queries, the scan trigger and the whole association OID stream
 * are answered for ANY authenticated caller - exactly how the old
 * StatusBarApp utility works without a root hop.
 *
 * So RTLWiFiBar speaks that protocol. Wire constants below are byte-identical
 * to Driver/RtWlanUCompat.h in the kext tree. Keep both files in sync.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RTW_COMPAT_H
#define RTW_COMPAT_H

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* User client type - RTWUSBController::newUserClient accepts only this. */
#define RTW_COMPAT_USER_CLIENT_TYPE 0u

/* IOKit selectors used by the Realtek utility (RtWlanUCompat.h). */
#define RTWCOMPAT_SEL_SURVEY_RECORD 0u  /* IOConnectCallMethod:   one 0x640 record  */
#define RTWCOMPAT_SEL_DISABLED_COMP 4u  /* IOConnectCallScalarMethod: capability gate */
#define RTWCOMPAT_SEL_DISABLED_LEVL 6u  /* IOConnectCallScalarMethod: capability gate */
#define RTWCOMPAT_SEL_SEND_PACKET   7u  /* unsupported by this driver          */
#define RTWCOMPAT_SEL_OID_QUERY     9u  /* IOConnectCallStructMethod 0x9D4 in/out */
#define RTWCOMPAT_SEL_OID_SET       10u /* IOConnectCallStructMethod 0x9D4 in/out */

/* Fixed frame sizes recovered from the utility binaries. */
#define RTWCOMPAT_OID_FRAME_SIZE 0x9D4u  /* 2516: 16-byte header + 2500 data */
#define RTWCOMPAT_OID_DATA_MAX   0x9C4u  /* 2500 */
#define RTWCOMPAT_SURVEY_RECORD  0x640u  /* 1600 bytes per BSS record */
#define RTWCOMPAT_SURVEY_MAX     0x200u  /* 512 records */
#define RTWCOMPAT_MAX_SSID       32u     /* SSID wire cap (matches kext ABI) */

/* The fixed frames are little-endian by definition; every supported macOS
 * CPU is little-endian. Fail the build rather than byte-swap silently. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#  if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#    error "RTLWiFiBar supports little-endian targets only"
#  endif
#endif

/* OID frame header (little-endian, fixed layout):
 *   +0x00 oid       QID/SID constant
 *   +0x04 length    query: buffer capacity 0x9C4; set: payload length
 *   +0x08 retLen    driver writes back bytes present in data[]
 *   +0x0C status    driver writes back IOReturn-style code (0 = OK)
 *   +0x10 data[2500] */
typedef struct RTWCompatOidFrame {
    uint32_t oid;
    uint32_t length;
    uint32_t retLen;
    uint32_t status;
    uint8_t data[RTWCOMPAT_OID_DATA_MAX];
} RTWCompatOidFrame;

_Static_assert(sizeof(RTWCompatOidFrame) == RTWCOMPAT_OID_FRAME_SIZE,
               "compat OID frame ABI size");

/* Query OIDs (answered for any caller). */
#define RTWCOMPAT_QID_PERMANENT_ADDRESS 0x01010102u  /* 6 bytes, adapter MAC   */
#define RTWCOMPAT_QID_CHANNEL           0xFF010182u  /* dword                  */
#define RTWCOMPAT_QID_AUTH_MODE         0xFF010194u  /* dword (utility enum)   */
#define RTWCOMPAT_QID_HARDWARE_TYPE     0xFF0101B1u  /* dword                  */
#define RTWCOMPAT_QID_WIRELESS_MODE     0xFF0101BBu  /* dword                  */
#define RTWCOMPAT_QID_JOIN_CONDITION    0xFF0101BCu  /* dword                  */
#define RTWCOMPAT_QID_SCAN_BUSY         0xFF0101BDu  /* dword, 0 = results ready */
#define RTWCOMPAT_QID_DRIVER_VERSION    0xFF010416u  /* string                 */
#define RTWCOMPAT_QID_IF_STATUS         0xFF010418u  /* dword, 1 = driver bound */
#define RTWCOMPAT_QID_SURVEY_COUNT      0xFF010419u  /* dword                  */
#define RTWCOMPAT_QID_ENCRYPTION_STATUS 0xFF01041Au  /* dword                  */
#define RTWCOMPAT_QID_CURRENT_SSID      0xFF01041Bu  /* ssid bytes             */
#define RTWCOMPAT_QID_HW_RF_OFF         0xFF818081u  /* byte, 0 = radio on     */
#define RTWCOMPAT_QID_NETWORK_STATUS    0x00010114u  /* dword, 1 = linked      */
/* v8.7.14: honest join progress (kext 1.0.8). Answer: dword0 = net80211
 * station state (0 INIT, 1 SCAN, 2 AUTH, 3 ASSOC, 4 RUN), dword1 = ms
 * since the last beacon of the associated BSS (0 = none). The join
 * monitor polls this so a dead join reports WHERE it died. */
#define RTWCOMPAT_QID_JOIN_STATE        0x00010115u
#define RTWCOMPAT_QID_LINK_STATUS       0xFF819048u  /* dword, 1 = linked      */
#define RTWCOMPAT_QID_ASSOC_STATE       0xFF819053u  /* dword, 1 = linked      */
#define RTWCOMPAT_QID_INFRA_MODE        0x0D010108u  /* dword, 1 = infrastructure */
#define RTWCOMPAT_QID_BSSID             0x0D010119u  /* 6 bytes                */
#define RTWCOMPAT_QID_RSSI              0x0D010206u  /* dword, signal percent  */

/* Set OIDs (the scan trigger and the association stream - allowed for any
 * caller by the kext's compat layer, stock privilege model). */
#define RTWCOMPAT_SID_NETWORK_NAME      0xFF070102u  /* resets join state + ssid */
#define RTWCOMPAT_SID_WEP_KEY           0xFF070113u  /* accepted, ignored       */
#define RTWCOMPAT_SID_SCAN_TRIGGER      0xFF07011Au  /* arms a sweep            */
#define RTWCOMPAT_SID_WPA_KEY           0xFF010305u  /* passphrase bytes        */
#define RTWCOMPAT_SID_ASSOC_TRIGGER     0xFF030004u  /* perform pending join    */
#define RTWCOMPAT_SID_HIBERNATE_MODE    0xFF01041Cu  /* accepted                */
#define RTWCOMPAT_SID_CURRENT_SSID      0xFF01041Bu  /* join-path ssid set      */
#define RTWCOMPAT_SID_DISASSOCIATE      0x0D010115u  /* leave + stop (kext 1.1.2) */
#define RTWCOMPAT_SID_DESIRED_BSSID     0x0D01011Au  /* mejoras: 6-byte AP pin; older kexts ignore it */

/* Survey record layout (0x640 bytes, unused bytes zero). Decoded from the
 * vendor GUI parser; see Driver/RtWlanUCompat.h for the field history. */
#define RTWCOMPAT_REC_SSID_OFFSET        0x00u  /* 33 bytes SSID area      */
#define RTWCOMPAT_REC_SSIDLEN_OFFSET     0x21u  /* byte  SSID length       */
#define RTWCOMPAT_REC_NETTYPE_OFFSET     0x22u  /* byte  0=infra 1=adhoc   */
#define RTWCOMPAT_REC_AUTH_OFFSET        0x23u  /* byte  utility auth enum */
#define RTWCOMPAT_REC_BSSID_OFFSET       0x24u  /* 6 bytes BSSID           */
#define RTWCOMPAT_REC_CHANNEL_OFFSET     0x2Au  /* word  radio channel     */
#define RTWCOMPAT_REC_SITE_ENCRY_OFFSET  0x22Cu /* byte  capability ladder */
#define RTWCOMPAT_REC_AKMSUIT_OFFSET     0x230u /* dword AKM bitmask       */
#define RTWCOMPAT_REC_SIGNALSTRENGTH_OFFSET 0x234u /* byte  signal percent  */

/* Utility auth enum (vendor _CmdAkm, NOT the NDIS enum). */
#define RTWCOMPAT_AUTH_OPEN         0u
#define RTWCOMPAT_AUTH_SHARED       1u
#define RTWCOMPAT_AUTH_WPAPSK       3u
#define RTWCOMPAT_AUTH_WPAPSK_AES   4u
#define RTWCOMPAT_AUTH_WPA2PSK_TKIP 5u
#define RTWCOMPAT_AUTH_WPA2PSK      6u

/* AKM capability bitmask (+0x230). */
#define RTWCOMPAT_AKM_WPA_1X   0x1u
#define RTWCOMPAT_AKM_WPA_PSK  0x2u
#define RTWCOMPAT_AKM_WPA2_1X  0x10u
#define RTWCOMPAT_AKM_WPA2_PSK 0x20u
#define RTWCOMPAT_AKM_SAE      0x1000u

/* net80211 state 4 = IEEE80211_S_RUN; the kext answers "linked" only there. */
#define RTWCOMPAT_NET80211_RUN 4u

/* Little-endian helpers (macOS is LE on every supported CPU; helpers keep
 * the intent explicit for the fixed wire layout). */
static inline uint32_t RTWCompatLoad32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void RTWCompatStore32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t RTWCompatLoad16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

__END_DECLS

#endif /* RTW_COMPAT_H */
