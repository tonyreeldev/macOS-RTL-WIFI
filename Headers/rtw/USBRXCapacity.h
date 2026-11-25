/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTW_USB_RX_CAPACITY_H
#define RTW_USB_RX_CAPACITY_H
#include "USBWiFiProfiles.h"

/* RTL8814A DMA threshold 0x2005 is20KiB, larger than the legacy16KiB read.
 * A read must hold the aggregate; the RX codec starts at a descriptor boundary.
 * Linux rtw88 uses32768-byte USB RX buffers with this same DMA threshold.
 * Primary pins:
 * aircrack-ng/rtl8814au 892641430c24308e7ae193d746b25b600ef227d8,
 * hal/rtl8814a/usb/usb_halinit.c:148 ("dmc agg th 20K").
 * torvalds/linux 0e135910deec43ffe97eca0b6af1f8f847285b59,
 * drivers/net/wireless/realtek/rtw88/usb.h:33,usb.c:884-902.
 * No USB timing, DMA register or retry-budget change is implied. */
static inline uint32_t rtw_usb_rx_capacity(rtw_wifi_family family)
{
    /* mejoras (1.1.2-tr.3): rtw88 chips (8821CU/8822BU/8822CU) are programmed
     * with the same 0x2005 threshold in RTW88MAC.c (REG_RXDMA_AGG_PG_TH), so a
     * 16 KiB read split aggregates >16 KiB: the read ended TRUNCATED and the
     * next one started mid-frame (MALFORMED at offset 0), dropping frames.
     * Linux rtw88 reads 32768 bytes (usb.h RTW_USB_MAX_RECVBUF_SZ). */
    return (family==RTW_WIFI_RTL8814AU||family==RTW_WIFI_RTL8821CU||
            family==RTW_WIFI_RTL8822BU||family==RTW_WIFI_RTL8822CU)?32768U:16384U;
}
#endif
