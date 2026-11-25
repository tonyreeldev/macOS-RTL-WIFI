/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTWLANU_COMPAT_H
#define RTWLANU_COMPAT_H

/* Realtek "RtWlanU" utility compatibility layer.
 *
 * The stock Realtek vendor utility/RtWlanHelper pair (the Airport-style utility
 * shipped by Realtek for RtWlanU.kext) discovers its driver through
 * IOServiceMatching("IOEthernetController") and then filters on the registry
 * IOClass string "RtWlanU" or "RtWlanU1827". It then drives the driver with a
 * private user-client protocol:
 *
 *   selector 0  IOConnectCallMethod       site-survey record readout
 *                                         scalarInput[0] = record index
 *                                         structureOutput = one 0x640 record
 *   selector 4  IOConnectCallScalarMethod DisabledCmdComp (capability gate)
 *   selector 6  IOConnectCallScalarMethod DisabledCmdLevl (capability gate)
 *   selector 7  SendPacket paths (monitor injection; unsupported here)
 *   selector 9  IOConnectCallStructMethod OID query, 0x9d4 byte in/out frame
 *   selector 10 IOConnectCallStructMethod OID set,   0x9d4 byte in/out frame
 *
 * Both the 0x9d4 OID frame and the 0x640 survey record were recovered by
 * static analysis of RtWlanHelper and vendor utility (see
 * REALTEK-APP-COMPAT-PROTOCOL.md). This layer maps that protocol onto the
 * fixed-layout RTWControl v1 ABI (RTWControl.h), which stays byte-identical.
 *
 * Privilege model: the Realtek GUI runs as the logged-in user while its
 * RtWlanHelper daemon runs as root. Read-only queries (survey records,
 * version, channel, scan-busy flag) and the scan trigger are allowed for any
 * authenticated caller; state-changing OIDs (association, keys) still require
 * kIOClientPrivilegeAdministrator, and the association itself is performed by
 * the root helper exactly as with the original driver.
 */

#include "RTWControl.h"

/* Selector numbers used by the Realtek utility. */
#define RTWLANUC_SEL_SURVEY_RECORD   0u
#define RTWLANUC_SEL_DISABLED_COMP   4u
#define RTWLANUC_SEL_DISABLED_LEVL   6u
#define RTWLANUC_SEL_SEND_PACKET     7u
#define RTWLANUC_SEL_OID_QUERY       9u
#define RTWLANUC_SEL_OID_SET         10u

/* Fixed frame sizes recovered from the utility binaries. */
#define RTWLANUC_OID_FRAME_SIZE   0x9D4u  /* 2516: 16-byte header + 2500 data */
#define RTWLANUC_OID_DATA_OFFSET  0x10u
#define RTWLANUC_OID_DATA_MAX     0x9C4u  /* 2500 */
#define RTWLANUC_SURVEY_RECORD    0x640u  /* 1600 bytes per BSS record */
#define RTWLANUC_SURVEY_MAX       0x200u  /* 512 records */

/* OID frame header (little-endian, fixed layout):
 *   +0x00 oid       QID/SID constant (0xff......)
 *   +0x04 length    query: buffer capacity 0x9C4; set: payload length
 *   +0x08 retLen    driver writes back bytes present in data[]
 *   +0x0C status    driver writes back IOReturn-style code (0 = OK)
 *   +0x10 data[2500] */
typedef struct RTWlanUOidFrame {
    uint32_t oid;
    uint32_t length;
    uint32_t retLen;
    uint32_t status;
    uint8_t data[RTWLANUC_OID_DATA_MAX];
} RTWlanUOidFrame;

/* Realtek OID constants observed in the utility (query = 0xff01....,
 * set = 0xff07...., locked = 0xff03....). Only the ones the utility uses in
 * its main station flows are implemented; the rest answer unsupported so the
 * UI hides the corresponding optional features. */
#define RTWLANU_QID_PERMANENT_ADDRESS  0x01010102u
/* NDIS permanent-address readback, sent with the standard-format OID space
 * (no 0xff01 prefix): GetPermanentAddress: memsets its buffer, queries this
 * OID and hex-formats the first six returned bytes into the 12-character
 * adapter key used by every per-adapter settings file (<key>rfoff.rtl). */
#define RTWLANU_QID_CHANNEL            0xFF010182u
#define RTWLANU_QID_AUTH_MODE          0xFF010194u
#define RTWLANU_QID_HARDWARE_TYPE      0xFF0101B1u
#define RTWLANU_QID_WIRELESS_MODE      0xFF0101BBu
#define RTWLANU_QID_SCAN_BUSY          0xFF0101BDu
#define RTWLANU_QID_JOIN_CONDITION     0xFF0101BCu
#define RTWLANU_QID_DRIVER_VERSION     0xFF010416u
#define RTWLANU_QID_IF_STATUS          0xFF010418u
#define RTWLANU_QID_SURVEY_COUNT       0xFF010419u
#define RTWLANU_QID_ENCRYPTION_STATUS  0xFF01041Au
#define RTWLANU_QID_CURRENT_SSID       0xFF01041Bu
#define RTWLANU_QID_HW_RF_OFF          0xFF818081u
/* Field round 11 (2026-11-08, disassembly of NicIFLinkStatusWatchdog
 * @0x10000d650): the watchdog's FIRST gate is
 * -[WLANClientUtilityModel GetConnectionStatus] (GUI @0x10001cb10), which is
 * a plain QueryInformation(0x00010114) returning a dword, and the watchdog
 * requires dword == 1 else it paints BarImageMode_Disconnect and climbs the
 * case2 reconnect ladder - 0xFF819048/0xFF819053 are only polled AFTER that
 * gate. 0x00010114 (OID_RT_NETWORK_STATUS) was never mapped, so every poll
 * returned unsupported/0 and the utility stayed red forever. 1 = linked.
 *
 * Link-state readbacks also polled by NicIFLinkStatusWatchdog (decoded from
 * the utility binary, field round 7): GetConnectionStatus(0xFF819048) returns a
 * dword whose FIRST BYTE is the supplicant outcome - 1 = linked (4-way
 * success), 3 = 4-way failure, anything else falls back to the signal
 * check. GetConnectionStatus(0xFF819053) returns a byte read as a plain
 * "associated" flag (non-zero sends the GUI into bSetIPAddress +
 * UpdateAssociatedNetwork). 0x0D010108 is the standard NDIS
 * OID_802_11_INFRASTRUCTURE_MODE query issued right after the
 * Disconnected-to-Connected transition; 0x0D010119 is OID_802_11_BSSID. */
#define RTWLANU_QID_NETWORK_STATUS     0x00010114u
/* 1.0.8 (RTLWiFiBar 8.7.14): honest join progress for the app's join
 * monitor - dword0 = the net80211 station state (0 INIT, 1 SCAN, 2 AUTH,
 * 3 ASSOC, 4 RUN), dword1 = ms since the last beacon of the associated
 * BSS (0 = none). The stock utility never issues this OID; the answer is
 * read-only state, so the vendor wire contract stays untouched. */
#define RTWLANU_QID_JOIN_STATE         0x00010115u
#define RTWLANU_QID_LINK_STATUS        0xFF819048u
#define RTWLANU_QID_ASSOC_STATE        0xFF819053u
#define RTWLANU_QID_INFRA_MODE         0x0D010108u
#define RTWLANU_QID_BSSID              0x0D010119u
/* Field round 12 (2026-11-08, disassembly of the menubar signal path):
 * -[WLANClientUtilityModel getSignalStrength] (GUI @0x10001c7a0) is a plain
 * QueryInformation(0x0D010206 = NDIS OID_802_11_RSSI) returning one dword,
 * and it is the ONLY caller chain: NicIFLinkStatusWatchdog (GUI
 * @0x10000d650, linked branch tail @0x10000dd52) feeds the LOW BYTE of the
 * answer into -[WiFiStatusBarDelegate SetStatusBarImage:param:] (GUI
 * @0x10000bdc0), whose mode-0 ladder is a PERCENT scale: >=0x50 bar5,
 * >=0x3c bar4, >=0x28 bar3, >=0x14 bar2, >=1 bar1, and 0 paints
 * BarSignal_0.icns - the grey icon the field saw while the same
 * watchdog's linked gate printed "connect". Unmapped, every read fell
 * into the unsupported default and returned 0. Answer the live dBm from
 * RTW_CONTROL_STATUS rescaled with the survey percent formula
 * ((dBm + 100) * 2, clamped 0..100) - the same scale the utility already
 * consumes from the survey record SignalStrength byte. */
#define RTWLANU_QID_RSSI               0x0D010206u
#define RTWLANU_SID_NETWORK_NAME       0xFF070102u
#define RTWLANU_SID_WEP_KEY            0xFF070113u
#define RTWLANU_SID_SCAN_TRIGGER       0xFF07011Au
#define RTWLANU_SID_WPA_KEY            0xFF010305u
#define RTWLANU_SID_ASSOC_TRIGGER      0xFF030004u
#define RTWLANU_SID_HIBERNATE_MODE     0xFF01041Cu
/* 1.1.2 (RTLWiFiBar "Disconnect USB WiFi"): the NDIS OID_802_11_DISASSOCIATE
 * value. Leaves the network (DISASSOC to the AP), stops the station and
 * erases the profile - RTW_CONTROL_LEAVE. A later join or scan brings the
 * station back up. No payload. */
#define RTWLANU_SID_DISASSOCIATE       0x0D010115u
/* mejoras: optional desired BSSID for the pending join (6 bytes,
 * all zero = none). Sent after the SSID set (which resets the join state)
 * and before auth/key. Older kexts accept and ignore it, so the app can
 * always send it: the join then simply goes by SSID. */
#define RTWLANU_SID_DESIRED_BSSID      0x0D01011Au
/* WirelessAssociate stores the join target through the very constant it
 * queries (decoded §8: 0xFF01041B is used query+set on the association
 * path, unlike the UI-level 0xFF070102 SetNetworkName path). */
#define RTWLANU_SID_CURRENT_SSID       RTWLANU_QID_CURRENT_SSID

/* Survey record layout — decoded from the REAL GUI parser
 * (StatusBar_GetAvailableNetworks, GUI @0x10000f47d; plist-key mapping
 * proven against the utility's own /private/tmp scan-cache dump,
 * field round 8, 2026-11-08). Total size fixed at 0x640; unused bytes
 * stay zero.
 *
 * Field round 9 correction (join-dialog classifier disassembled,
 * fieldCtrl_encryTypeSetting:preferrAuth:AKMsuit:NetType: @0x100051ea0):
 * +0x22C "Site_Encry" is NOT the NDIS encryption-status enum — it is
 * the utility's SITE_ capability ladder: 0 = open/none, 1 = WEP,
 * 2..3 = WPA-TKIP class, 4..0x1f = WPA-AES class, 0x20..0x3f =
 * WPA2-TKIP class, >= 0x40 = WPA2-AES class ("WPA2-PSK AES" box),
 * 0xff = WPA-None special case. +0x230 "AKMsuit" is the utility's AKM
 * capability bitmask (0x1 WPA-1X, 0x2 WPA-PSK, 0x10 WPA2-1X,
 * 0x20 WPA2-PSK, 0x1000 SAE), not an 802.11 suite selector.
 * Field round 8 correction: the earlier notes called +0x230 "RSSI" —
 * wrong. The GUI maps +0x22C to its "Site_Encry" property, +0x230 to
 * "AKMsuit" and +0x234 to "SignalStrength", and feeds the join
 * classifier from Site_Encry: a zero there makes EVERY network join as
 * open/none ("setApplyProfileInfo::[Case 1][OPEN_NONE]"), which is why
 * WPA2 APs never linked. The +0x2A word is the radio channel (GUI
 * @0xf9f1); the +0x23 byte is the NDIS auth mode (the GUI's plist key
 * for it is confusingly named "Channel"). */
#define RTWLANU_REC_SSID_OFFSET        0x00u  /* 33 bytes SSID area */
#define RTWLANU_REC_SSIDLEN_OFFSET     0x21u  /* byte  SSID length  */
#define RTWLANU_REC_NETTYPE_OFFSET     0x22u  /* byte  0=infrastructure 1=adhoc */
#define RTWLANU_REC_AUTH_OFFSET        0x23u  /* byte  NDIS auth mode (GUI plist key "Channel") */
#define RTWLANU_REC_BSSID_OFFSET       0x24u  /* 6 bytes BSSID            */
#define RTWLANU_REC_CHANNEL_OFFSET     0x2Au  /* word  radio channel      */
#define RTWLANU_REC_SITE_ENCRY_OFFSET  0x22Cu /* byte  utility SITE_ ladder:
                                                 0 open, 1 WEP, 2..3 WPA-TKIP,
                                                 4..0x1f WPA-AES,
                                                 0x20..0x3f WPA2-TKIP,
                                                 >=0x40 WPA2-AES, 0xff WPA-None */
#define RTWLANU_REC_AKMSUIT_OFFSET     0x230u /* dword utility AKM bitmask:
                                                 0x1 WPA-1X, 0x2 WPA-PSK,
                                                 0x10 WPA2-1X, 0x20 WPA2-PSK,
                                                 0x1000 SAE */
#define RTWLANU_REC_SIGNALSTRENGTH_OFFSET 0x234u /* byte  signal strength, percent */

/* Field round 10 (2026-11-08): the utility's OWN auth enum, decoded from
 * vendor utility _CmdAkm (GUI @0x10003c770) — NOT the NDIS enum and NOT the
 * Windows RT_802_11 enum. _CmdAkm writes these numbers to 0xFF010194 and
 * its readback jump table (indexed by the raw value) validates exactly
 * 0..6. The join-dialog dropdown tags are the same numbers
 * (WPA2-PSK TKIP tag 5, WPA2-PSK AES tag 6). */
#define RTWLANU_AUTH_OPEN              0u /* open system */
#define RTWLANU_AUTH_SHARED            1u /* 1/2 shared-key WEP */
#define RTWLANU_AUTH_WPAPSK            3u /* 3/4 WPA-PSK (TKIP/AES) */
#define RTWLANU_AUTH_WPAPSK_AES        4u
#define RTWLANU_AUTH_WPA2PSK_TKIP      5u
#define RTWLANU_AUTH_WPA2PSK           6u /* 5/6 WPA2-PSK (TKIP/AES) */
#define RTWLANU_AUTH_PSK_MAX           7u /* join PSK range cap (7 = the
                                             build<=59 legacy constant,
                                             kept for cached profiles) */

/* Fixed driver version string surfaced in the utility About panel; also
 * published as the registry property "RtWlanDriverVersion". */
#define RTWLANU_DRIVER_VERSION "1.1.2-tr.8"

#endif /* RTWLANU_COMPAT_H */
