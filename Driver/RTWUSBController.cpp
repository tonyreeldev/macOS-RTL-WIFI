#include "RTWUSBController.hpp"
#include "rtw/NetworkSecurity.h"
#include "RTWUserClient.hpp"
#include "RTWNet80211Globals.hpp"
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IONetworkData.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostFamily.h>
#include <IOKit/usb/StandardUSB.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/OSAtomic.h>
#include <libkern/version.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <sys/kpi_mbuf.h>
#include <net/kpi_interface.h>
#include <net80211/ieee80211_priv.h>
#include "rtw/RTL8192EU.h"
#include "rtw/USBPackets.h"
#include "RTL8192EUFirmware.inc"
#include "LegacyUSBWiFiFirmware.inc"
#include "RTL8814AUFirmware.inc"
#include "RTL8192DUFirmware.inc"
#include "rtw/DiagnosticPublishPolicy.hpp"
#include <libkern/c++/OSNumber.h>
#include "RTWBandCapabilities.hpp"
#include "RTWPerformanceDiagnostics.inc"
#include "RTWHTDataPath.inc"
#include "RTWFamilyDataPath.inc"

#define super IOEthernetController
OSDefineMetaClassAndStructors(RTWUSBController, IOEthernetController)
static IOReturn fromErrno(int error) {
    switch (error) {
    case 0: return kIOReturnSuccess;
    case EINVAL: return kIOReturnBadArgument;
    case ENOMEM: case ENOBUFS: return kIOReturnNoMemory;
    case EBUSY: return kIOReturnBusy;
    case ENODEV: case ENXIO: return kIOReturnNoDevice;
    case ENETDOWN: return kIOReturnNotReady;
    case ETIMEDOUT: return kIOReturnTimeout;
    default: return kIOReturnIOError;
    }
}
/* 1.0.5 software re-plug budget, per boot across every controller instance
 * (re-enumeration spawns a fresh instance, so the per-attach cap alone
 * would not bound a truly dead dongle). Six fires per boot is generous:
 * the field wedge needed exactly one. */
static uint32_t g_softwareReplugsSinceBoot = 0;
static uint64_t g_lastSoftwareReplugMS = 0;
/* 1.0.6 boot-rescue window: 12 minutes after each attach. The reboot-2
 * wedge shows up in the first minutes; past the window the rescue is
 * disarmed so a working session is never replugged from behind. */
static const uint64_t RTW_BOOT_RESCUE_WINDOW_MS = 12ull * 60 * 1000;
RTWUSBController *RTWUSBController::owner(void *p) { return static_cast<RTWUSBController *>(p); }
IOWorkLoop *RTWUSBController::getWorkLoop() const { return loop_ ? loop_ : super::getWorkLoop(); }
IOOutputQueue *RTWUSBController::createOutputQueue() { return IOGatedOutputQueue::withTarget(this, loop_, 256); }

/* ---- IONetworkController vtable ABI bridge (R28-monterey-vtable) ----
 * Rationale lives next to the declarations in RTWUSBController.hpp: with the
 * SDK 26.x headers these six slots are what macOS 12 (Monterey) dispatches as
 * outputStart / setInputPacketPollingEnable / pollInputPackets /
 * networkInterfaceNotification (private-SPI layout) plus the two 26.x additions.
 * The bodies reproduce the parent-class defaults exactly so every caller sees
 * the value the family would have produced without the bridge.
 * The parent declares the four _RESERVED slots as `virtual void`, but
 * Monterey's drainOutputQueue() loops on slot 2 as
 * `IOReturn outputStart(interface, 0)` and only stops on a non-Success
 * status, and the polling transition on slot 3 expects kIOReturnUnsupported
 * to stay on the push model. An uninitialised eax would read as garbage
 * (possibly kIOReturnSuccess) and hang TX or flip RX into a dead polled mode,
 * so each bridge writes kIOReturnUnsupported into eax before returning. */
static mbuf_t rtw_bridge_get_packet(UInt32 size, UInt32 how, UInt32 smask, UInt32 lmask) {
    /* Same allocation logic as IONetworkController's file-local getPacket():
     * over-request so the alignment masks can be fulfilled, force a cluster
     * for sizes above the mbuf header capacity, then align every chain
     * element. Self-contained so the bridge imports nothing that is absent
     * on older kernels. */
    const UInt32 reqSize = size + smask + lmask;
    const uint32_t minSize = mbuf_get_minclsize();
    UInt32 want = reqSize;
    if (reqSize > mbuf_get_mhlen() && reqSize <= minSize) want = minSize + 1;
    mbuf_t packet = nullptr;
    if (mbuf_allocpacket(how, want, nullptr, &packet)) return nullptr;
    mbuf_pkthdr_setlen(packet, size);
    mbuf_t m = packet;
    UInt32 left = size;
    while (left && m) {
        const uintptr_t originalStart = (uintptr_t)mbuf_data(m);
        const uintptr_t alignedStart = (originalStart + smask) & ~((uintptr_t)smask);
        mbuf_setdata(m, (caddr_t)alignedStart,
                     (mbuf_maxlen(m) - (alignedStart - originalStart)) & ~((uintptr_t)lmask));
        if (mbuf_len(m) > left) mbuf_setlen(m, left);
        left -= mbuf_len(m);
        m = mbuf_next(m);
    }
    return packet;
}
void RTWUSBController::_RESERVEDIONetworkController2() {
#if defined(__x86_64__)
    asm volatile("movl %0, %%eax" : : "i"((UInt32)kIOReturnUnsupported) : "ax");
#else
    asm volatile("" ::: "memory");
#endif
}
void RTWUSBController::_RESERVEDIONetworkController3() {
#if defined(__x86_64__)
    asm volatile("movl %0, %%eax" : : "i"((UInt32)kIOReturnUnsupported) : "ax");
#else
    asm volatile("" ::: "memory");
#endif
}
void RTWUSBController::_RESERVEDIONetworkController4() {
    /* Monterey dispatches this slot as `void pollInputPackets(...)` - a plain
     * no-op is the exact parent default. */
}
void RTWUSBController::_RESERVEDIONetworkController5() {
#if defined(__x86_64__)
    asm volatile("movl %0, %%eax" : : "i"((UInt32)kIOReturnUnsupported) : "ax");
#else
    asm volatile("" ::: "memory");
#endif
}
mbuf_t RTWUSBController::allocatePacketNoWait(UInt32 size) {
    /* Real allocation (the parent default on the 26.x layout) - never a stub,
     * so a caller on any kernel sees the documented behaviour. The parent
     * keeps the alignment masks in private ivars, so recompute them from the
     * public getPacketBufferConstraints() query exactly like the parent does. */
    IOPacketBufferConstraints constraints{};
    getPacketBufferConstraints(&constraints);
    if (constraints.alignStart < constraints.alignLength)
        constraints.alignStart = constraints.alignLength;
    const UInt32 startMask = constraints.alignStart ? constraints.alignStart - 1 : 0;
    const UInt32 lengthMask = constraints.alignLength ? constraints.alignLength - 1 : 0;
    return rtw_bridge_get_packet(size, MBUF_DONTWAIT, startMask, lengthMask);
}
IOReturn RTWUSBController::setHardwareAssists(UInt32, UInt32) {
    /* Parent default: this driver takes no hardware-assist changes. */
    return kIOReturnUnsupported;
}

UInt32 RTWUSBController::getFeatures() const { return 0; }
IOReturn RTWUSBController::getPacketFilters(const OSSymbol *group,UInt32 *filters) const {
    if(!filters)return kIOReturnBadArgument;
    *filters=0;
    if(group!=gIONetworkFilterGroup)return kIOReturnUnsupported;
    *filters=kIOPacketFilterUnicast|kIOPacketFilterBroadcast|kIOPacketFilterMulticast;
    return kIOReturnSuccess;
}
IOReturn RTWUSBController::setMulticastMode(bool) {
    /* The pinned MAC profile receives multicast with MAR all ones. The stack
     * applies station/BSSID, RSN, MIC and controlled-port checks. */
    return kIOReturnSuccess;
}
IOReturn RTWUSBController::setMulticastList(IOEthernetAddress *addresses,UInt32 count) {
    return !addresses&&count ? kIOReturnBadArgument : kIOReturnSuccess;
}
IOReturn RTWUSBController::selectMedium(const IONetworkMedium *medium) {
    return medium && setSelectedMedium(medium) ? kIOReturnSuccess : kIOReturnBadArgument;
}
bool RTWUSBController::authorized() const {
    return attached_ && running_ && radioReady_ && joinRequested_ && present_ &&
        !sleeping_ && !dfsTXBlocked() && usbError_ == kIOReturnSuccess && !core_.core_error &&
        core_.sc_ic.ic_state == IEEE80211_S_RUN && core_.sc_ic.ic_bss &&
        (family_!=RTW_WIFI_RTL8814AU||(!acCore_.io_error&&acCore_.phy_initialized&&uses8814FirmwareRate()))&&
        (!is8812Family()||uses8812FirmwareRate())&&(!is88Family()||uses88FirmwareRate())&&(family_!=RTW_WIFI_RTL8192DU||uses8192DUFirmwareRate())&&(!is8192FUFamily()||uses8192FUFirmwareRate())&&(!is8710BUFamily()||uses8710BUFirmwareRate())&&(!is8723Family()||uses8723FirmwareRate())&&core_.sc_ic.ic_bss->ni_port_valid;
}
bool RTWUSBController::setLinkStatus(UInt32 state, const IONetworkMedium *medium, UInt64 speed, OSData *data) {
    if ((state & kIONetworkLinkActive) && !authorized()) {
        state = kIONetworkLinkValid; medium = nullptr; speed = 0;
    }
    bool result=super::setLinkStatus(state, medium, speed, data);
    if((state&kIONetworkLinkActive)&&authorized()){
        /* 1.0.6: the first live carrier of this attach proves the hardware
         * and the boot are good - the boot rescue is disarmed from here on,
         * so no re-plug can ever disturb a session the user has seen. */
        bootEverAuthorized_=true;
        cancelWpaRecovery();wpaRecovery_.state=WPARecoveryAuthorized;
        if(auto*queue=getOutputQueue())queue->start();
    }
    return result;
}
bool RTWUSBController::configureInterface(IONetworkInterface *interface) {
    if (!super::configureInterface(interface)) return false;
    IONetworkData *data = interface->getParameter(kIONetworkStatsKey);
    IOEthernetInterface *ethernet = OSDynamicCast(IOEthernetInterface, interface);
    if (!data || !ethernet || !(stats_ = const_cast<IONetworkStats *>(static_cast<const IONetworkStats *>(data->getBuffer())))) return false;
    core_.sc_ic.ic_if.netStat = stats_;
    ether_ifattach(&core_.sc_ic.ic_if, ethernet);
    return true;
}
IOReturn RTWUSBController::getHardwareAddress(IOEthernetAddress *address) {
    if (!address) return kIOReturnBadArgument;
    if (!macReady_) return kIOReturnNotReady;
    *address = mac_; return kIOReturnSuccess;
}
void RTWUSBController::fault(IOReturn error, uint32_t site, uint32_t detail) {
    if (error == kIOReturnSuccess) return;
    if (usbError_ == kIOReturnSuccess) {
        usbError_ = error;
        /* 1.0.34: remember WHERE the first USB fault happened (published by
         * publishDiagnostics, no registry write here: this can run in a
         * completion context). Sites: 1/2 vendor read/write (detail=reg),
         * 3 TX submit, 4 RX recovery, 5-10 RX completion, 11 terminate. */
        faultCode_ = (uint32_t)error; faultSite_ = site; faultDetail_ = detail;
        faultAfterAttachMS_ = bootAttachCompleteMS_ ? nowMS() - bootAttachCompleteMS_ : 0;
        faultIcState_ = (uint32_t)core_.sc_ic.ic_state;
        faultRunning_ = running_ ? 1 : 0;
    }
    clear88Discovery(true);
    resetDFSStation();
    radioReady_ = false;
    if (attached_) core_.sc_ic.ic_if.if_link_state = LINK_STATE_DOWN;
    linkActivePosted_=false;
    super::setLinkStatus(kIONetworkLinkValid);
}
int RTWUSBController::vendorRequest(bool input, uint16_t address, uint8_t *bytes, size_t length) {
    if (!present_ || !usbOpened_ || !usb_ || sleeping_ || usbError_ != kIOReturnSuccess) return ENXIO;
    if (!bytes || !length || length > 254) return EINVAL;
    if(!input)rtw_dfs_txpause_guard(dfsTXHold_,family_==RTW_WIFI_RTL8192DU,address,bytes,length);
    StandardUSB::DeviceRequest request{};
    request.bmRequestType = input ? 0xc0 : 0x40;
    request.bRequest = 0x05; request.wValue = OSSwapHostToLittleInt16(address);
    request.wIndex = 0; request.wLength = OSSwapHostToLittleInt16((uint16_t)length);
    uint32_t actual = 0;
    IOReturn result = usb_->deviceRequest(request, bytes, actual, 1000);
    if (result != kIOReturnSuccess || actual != length) {
        fault(result == kIOReturnSuccess ? kIOReturnUnderrun : result, input ? 1 : 2, address); return EIO;
    }
    return 0;
}
uint32_t RTWUSBController::readRegister(uint16_t address, uint8_t width) {
    uint8_t bytes[4]{};
    if ((width != 1 && width != 2 && width != 4) || vendorRequest(true,address,bytes,width)) return 0xffffffffU;
    uint32_t value = bytes[0];
    if (width >= 2) value |= (uint32_t)bytes[1] << 8;
    if (width == 4) value |= (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return value;
}
int RTWUSBController::writeRegister(uint16_t address, uint8_t width, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
    if (width != 1 && width != 2 && width != 4) return EINVAL;
    return vendorRequest(false,address,bytes,width);
}
uint8_t RTWUSBController::read1(void *p,uint16_t a) {
    auto*s=owner(p);
    const bool h2cPoll=s->calibDiag_.active&&a==R92C_HMETFR;
    const uint64_t started=h2cPoll?nowMS():0;
    uint8_t value=(uint8_t)s->readRegister(a,1);
    if(h2cPoll){
        s->calibDiag_.lastH2CPolls++;
        s->calibDiag_.totalH2CPolls++;
        s->calibDiag_.lastH2CReadMS+=nowMS()-started;
    }
    return value;
}
uint16_t RTWUSBController::read2(void *p,uint16_t a) { return (uint16_t)owner(p)->readRegister(a,2); }
uint32_t RTWUSBController::read4(void *p,uint16_t a) { return owner(p)->readRegister(a,4); }
void RTWUSBController::write1(void*p,uint16_t a,uint8_t v) { (void)owner(p)->writeRegister(a,1,v); }
void RTWUSBController::write2(void*p,uint16_t a,uint16_t v) { (void)owner(p)->writeRegister(a,2,v); }
void RTWUSBController::write4(void*p,uint16_t a,uint32_t v) { (void)owner(p)->writeRegister(a,4,v); }
void RTWUSBController::delayUS(void*,uint32_t us) { if (us >= 1000) IOSleep((us+999)/1000); else IODelay(us); }
int RTWUSBController::writeRegion(void*p,uint16_t a,const uint8_t*b,size_t n) {
    if (!b || !n || n > 254) return EINVAL;
    uint8_t copy[254]; memcpy(copy,b,n); return owner(p)->vendorRequest(false,a,copy,n);
}
int RTWUSBController::ioError(void*p) {
    auto *s=owner(p); return !s->present_ || s->sleeping_ || s->usbError_ != kIOReturnSuccess ? EIO : 0;
}
int RTWUSBController::busPower(void*p) { return rtw92eu_power_on(&owner(p)->core_); }
int RTWUSBController::busDMA(void*p) { auto*s=owner(p); return rtw92eu_dma_init(&s->core_,s->endpoints_.num_bulk_out); }
int RTWUSBController::busFirmware(void*p,int page,uint8_t*b,int n) { return rtw92eu_fw_loadpage(&owner(p)->core_,page,b,n); }
int RTWUSBController::loadFirmware(void*p,u_char **bytes,size_t *length) {
    if (!bytes || !length) return EINVAL;
    *bytes=nullptr;*length=0;
    auto*s=owner(p);const uint8_t*image=nullptr;size_t count=0;
    switch(s->family_){
    case RTW_WIFI_RTL8192EU:image=RTW8192EUFirmware;count=sizeof(RTW8192EUFirmware);break;
    case RTW_WIFI_RTL8188EU:image=rtl8188eu;count=sizeof(rtl8188eu);break;
    case RTW_WIFI_RTL8192CU:
        if((s->core_.chip&(RTWN_CHIP_UMC_A_CUT|RTWN_CHIP_92C))==RTWN_CHIP_UMC_A_CUT){image=rtl8192cU;count=sizeof(rtl8192cU);}
        else {image=rtl8192cT;count=sizeof(rtl8192cT);}
        break;
    default:return ENODEV;
    }
    rtw_8192eu_fw_info info{};
    if(!rtw_usb_wifi_inspect_firmware(s->family_,image,count,&info))return EINVAL;
    *bytes=static_cast<u_char *>(::malloc(count,0,0));
    if (!*bytes) return ENOMEM;
    memcpy(*bytes,image,count);*length=count;return 0;
}
void RTWUSBController::busMAC(void*p) { auto*s=owner(p);int e=rtw92eu_mac_init(&s->core_);if(e&&!s->core_.core_error)s->core_.core_error=e; }
void RTWUSBController::busBB(void*p) { auto*s=owner(p);int e=rtw92eu_bb_init(&s->core_);if(e&&!s->core_.core_error)s->core_.core_error=e; }
void RTWUSBController::busAggregation(void*p) { auto*s=owner(p);int e=rtw92eu_aggr_init(&s->core_,s->rxPipe_->getSpeed()==kUSBHostConnectionSpeedHigh);if(e&&!s->core_.core_error)s->core_.core_error=e; }
int RTWUSBController::busAllocate(void*p) { return owner(p)->allocateTransfers(); }
int RTWUSBController::busInitialize(void*p) {
    auto*s=owner(p);
    if (!s->core_.sc_ic.ic_if.iface || !s->stats_ || ioError(p)) return ENXIO;
    if(s->family_==RTW_WIFI_RTL8192EU){write1(p,R92C_ACLK_MON,0);write1(p,R92C_USB_HRPWM,0);}
    if (ioError(p)) return EIO;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(s->completionLock_);
    bool restart=rtw_transfer_restart(&s->ledger_);IOSimpleLockUnlockEnableInterrupt(s->completionLock_,lock);
    if(!restart)return EBUSY;
    s->running_=true;
    for(unsigned i=0;i<RXSlots;i++) if(s->submit(i,s->rxPipe_,rtw_usb_rx_capacity(s->family_))) {s->abortTransfers();return EIO;}
    /* rtwn_init already rebuilt DMA and reloaded firmware. Successful RX
     * submission certifies a fresh transport for untagged CCX feedback. */
    if(s->family_==RTW_WIFI_RTL8188EU){
        write1(p,R88E_TX_RPT_CTRL,read1(p,R88E_TX_RPT_CTRL)|R88E_TX_RPT_CTRL_EN);
        if(ioError(p)){s->abortTransfers();return EIO;}
    }
    s->rateTransportFresh_=true;
    s->watchdogTimer_->setTimeoutMS(1000);return 0;
}
void RTWUSBController::busStop(void*p) {
    auto*s=owner(p);s->abortTransfers();
    if(s->present_&&!s->sleeping_&&s->usbError_==kIOReturnSuccess){
        write1(p,R92C_RF_CTRL,0);
        uint16_t control=read2(p,R92C_CR);
        write2(p,R92C_CR,control&~(R92C_CR_MACTXEN|R92C_CR_MACRXEN));
    }
    s->freeTransfers();
}
int RTWUSBController::busBusy(void*p) { return owner(p)->hasFreeTX()?0:1; }
void RTWUSBController::nextScan(void*p) { auto*s=owner(p);if(s->running_&&!s->stopping_)s->scanTimer_->setTimeoutMS(200); }
void RTWUSBController::cancelScan(void*p) { auto*s=owner(p);if(s->scanTimer_)s->scanTimer_->cancelTimeout(); }
void RTWUSBController::nextCalibration(void*p) { auto*s=owner(p);if(s->running_&&!s->stopping_)s->calibTimer_->setTimeoutMS(2000); }
void RTWUSBController::cancelCalibration(void*p) { auto*s=owner(p);if(s->calibTimer_)s->calibTimer_->cancelTimeout(); }
void RTWUSBController::waitAsync(void*) { /* All rtwn work executes on this same serialized workloop. */ }
int RTWUSBController::transmit(void*p,mbuf_t m,ieee80211_node*n) { return owner(p)->sendFrame(m,n); }
static const char* rtwStateName(ieee80211_state s) {
    switch(s){
    case IEEE80211_S_INIT:return "INIT";
    case IEEE80211_S_SCAN:return "SCAN";
    case IEEE80211_S_AUTH:return "AUTH";
    case IEEE80211_S_ASSOC:return "ASSOC";
    case IEEE80211_S_RUN:return "RUN";
    default:return "?";
    }
}
void RTWUSBController::snapshotTx88(const char*tag) {
    char b[420];char k[24];snprintf(k,sizeof k,"RTWTx88E_%s",tag);
    const uint32_t cr=read2(this,0x100),dma=read4(this,0x10c),pause=read1(this,0x522),rcr=read4(this,0x608),bcn=read1(this,0x550),msr=read1(this,0x102),
        bssid0=read4(this,0x618),bssid1=read2(this,0x61c),mac0=read4(this,0x610),mac1=read2(this,0x614),txdma=read4(this,0x210),ac=read4(this,0x508);
    snprintf(b,sizeof b,"nOut=%u mps=%u,%u,%u map=%u,%u,%u,%u ch=%u CR=%04x TRXDMA=%08x TXPAUSE=%02x RCR=%08x BCNCTRL=%02x MSR=%02x BSSID=%08x%04x MAC=%08x%04x TXDMASTAT=%08x r508=%08x rxAgg=%u ioerr=%d",
        (unsigned)endpoints_.num_bulk_out,(unsigned)txMPS_[0],(unsigned)txMPS_[1],(unsigned)txMPS_[2],(unsigned)core_.ac2idx[0],(unsigned)core_.ac2idx[1],(unsigned)core_.ac2idx[2],(unsigned)core_.ac2idx[3],
        core_.sc_ic.ic_bss&&core_.sc_ic.ic_bss->ni_chan?(unsigned)ieee80211_chan2ieee(&core_.sc_ic,core_.sc_ic.ic_bss->ni_chan):0U,
        cr,dma,pause,rcr,bcn,msr,bssid0,bssid1,mac0,mac1,txdma,ac,(unsigned)rxDiag_.aggregates,rtwn_io_error(&core_));
    setProperty(k,b);IOLog("[macOS-RTL-WIFI 1.0.21] %s: %s\n",k,b);
}
/* 1.0.26: encrypt a known frame with the same software CCMP code the TX path uses,
 * decrypt it with the RX code (proven against the AP's own group frames) and
 * publish the verdict as RTWDiagCCMPSelfTest = 100 + code (100 = pass). */
void RTWUSBController::ccmpSelfTest() {
    auto&ic=core_.sc_ic;int code=0;
    ieee80211_key k{};k.k_cipher=IEEE80211_CIPHER_CCMP;k.k_len=16;k.k_id=0;
    for(unsigned i=0;i<16;i++)k.k_key[i]=(uint8_t)(i*7+3);
    uint8_t original[64]{};ieee80211_frame*wh=reinterpret_cast<ieee80211_frame*>(original);
    wh->i_fc[0]=IEEE80211_FC0_VERSION_0|IEEE80211_FC0_TYPE_DATA|IEEE80211_FC0_SUBTYPE_DATA;
    wh->i_fc[1]=IEEE80211_FC1_DIR_TODS|IEEE80211_FC1_PROTECTED;
    for(unsigned i=0;i<6;i++){wh->i_addr1[i]=(uint8_t)(0x10+i);wh->i_addr2[i]=ic.ic_myaddr[i];wh->i_addr3[i]=0xff;}
    wh->i_seq[0]=0x30;wh->i_seq[1]=0x01;
    for(unsigned i=24;i<64;i++)original[i]=(uint8_t)(i*5+1);
    mbuf_t m=nullptr;mbuf_t enc=nullptr;mbuf_t dec=nullptr;
    if(ieee80211_ccmp_set_key(&ic,&k))code=1;
    else if(mbuf_allocpacket(MBUF_DONTWAIT,sizeof original,nullptr,&m)||!m||mbuf_copyback(m,0,sizeof original,original,MBUF_DONTWAIT))code=2;
    else if(!(enc=ieee80211_ccmp_encrypt(&ic,m,&k)))code=3;
    else if(mbuf_pkthdr_len(enc)!=sizeof original+IEEE80211_CCMP_HDRLEN+IEEE80211_CCMP_MICLEN)code=4;
    else{
        uint8_t hdr[32]{};
        if(mbuf_copydata(enc,0,32,hdr)||hdr[24]!=(uint8_t)k.k_tsc||hdr[27]!=(uint8_t)((k.k_id<<6)|IEEE80211_WEP_EXTIV))code=5;
        else if(!(dec=ieee80211_ccmp_decrypt(&ic,enc,&k))){code=6;enc=nullptr;}
        else{
            enc=nullptr;
            uint8_t back[64]{};
            if(mbuf_pkthdr_len(dec)!=sizeof original||mbuf_copydata(dec,0,sizeof back,back))code=8;
            else{back[1]|=IEEE80211_FC1_PROTECTED;if(memcmp(back,original,sizeof original))code=7;}
        }
    }
    if(dec)mbuf_freem(dec);
    if(k.k_priv)ieee80211_ccmp_delete_key(&ic,&k);
    setProperty("RTWDiagCCMPSelfTest",(unsigned long long)(100+code),32);
    IOLog("[macOS-RTL-WIFI 1.0.26] CCMP self-test code=%d\n",code);
}
int RTWUSBController::stateTransition(ieee80211com *ic,ieee80211_state state,int argument) {
    auto*core=static_cast<rtwn_softc *>(ic->ic_softc);
    auto*s=owner(core->sc_ops.cookie);
    /* 1.0.7: the 1.0.6 WPA-lockout gate that rejected AUTH/ASSOC/RUN while
     * WPARecoveryFailed was latched is GONE - the latch itself is gone
     * (advanceWpaRecovery now stands down instead of freezing the station
     * until a kext reload). */
    if (!s->joinRequested_ && (state==IEEE80211_S_AUTH || state==IEEE80211_S_ASSOC || state==IEEE80211_S_RUN))
    {
        IOLog("RtWlanUCompat: GATE no-join-requested rejected %s -> %s\n",
              rtwStateName(ic->ic_state),rtwStateName(state));
        ieee80211_begin_scan(&ic->ic_if);
        return rtwn_io_error(core);
    }
    if(state!=ic->ic_state||ic->ic_state==IEEE80211_S_RUN)s->resetRateControl();
    const auto previous=ic->ic_state;
    unsigned requestedChannel=0;
    if(ic->ic_bss&&ic->ic_bss->ni_chan&&ic->ic_bss->ni_chan!=IEEE80211_CHAN_ANYC)
        requestedChannel=ieee80211_chan2ieee(ic,ic->ic_bss->ni_chan);
    if(!s->dfsStateAllowed(state,requestedChannel)){if(state==IEEE80211_S_AUTH&&!s->dfsTXBlocked())s->stopDFSStation();IOLog("RtWlanUCompat: GATE dfs rejected %s -> %s (ch %u)\n",rtwStateName(ic->ic_state),rtwStateName(state),requestedChannel);return EACCES;}
    if(state==IEEE80211_S_AUTH){const int power=s->prepareDFSPower(requestedChannel);if(power){s->stopDFSStation();IOLog("RtWlanUCompat: GATE dfs-power rejected %s -> %s (ch %u)\n",rtwStateName(ic->ic_state),rtwStateName(state),requestedChannel);return power;}}
    rtw_signal_reset(&s->signal_);
    /* 1.0.14 shipped a pre-auth DEAUTH "ghost flush" here; removed in
     * 1.0.15 after field testing showed no cure - the blocker is not an
     * AP-side ghost but a cold firmware auth engine (see the cold-attach
     * readiness gate in handleControl/considerColdGate). */
    int result=s->is8723Family()?s->state8723(ic,state,argument):s->is8710BUFamily()?s->state8710BU(ic,state,argument):s->is8192FUFamily()?s->state8192FU(ic,state,argument):s->family_==RTW_WIFI_RTL8192DU?s->state8192DU(ic,state,argument):s->is88Family()?s->state88(ic,state,argument):s->is8812Family()?s->state8812(ic,state,argument):s->family_==RTW_WIFI_RTL8814AU?s->state8814(ic,state,argument):rtwn_newstate(ic,state,argument);
    /* Some legacy register callbacks report a latched I/O error separately
     * from the backend return. It must not become a successful visit claim;
     * the diagnostic check leaves the transition's return value unchanged. */
    s->dfsStateChanged(state,requestedChannel,result?result:rtwn_io_error(core));
    s->observeScanTransition(previous,state,requestedChannel,result?result:rtwn_io_error(core));
    if(!result&&state==IEEE80211_S_RUN){s->core_.sc_ic.ic_bad_valid=0;s->core_.sc_ic.ic_avoid5=0;s->core_.sc_ic.ic_join_alt24=0;s->lastBeaconMs_=nowMS();s->lastRunMs_=nowMS();s->resetRateControl();}
    else if(state==IEEE80211_S_SCAN||state==IEEE80211_S_INIT)s->lastBeaconMs_=0;
    /* 1.0.10: AUTH was actually reached and accepted by the state machine -
     * the network exists and was visible. The boot-rescue auth-cycle
     * detector keys on this so a merely absent network never re-plugs. */
    if(!result&&state==IEEE80211_S_AUTH)s->everReachedAuth_=true;
    if(!result)s->wpaStateChanged(previous);
    /* 1.0.30: how long after the join request each state was first entered. */
    if(!result&&s->lastJoinRequestedMS_&&state>=IEEE80211_S_SCAN&&state<=IEEE80211_S_RUN&&!s->joinStateMS_[state]){
        const uint64_t dt=nowMS()-s->lastJoinRequestedMS_;s->joinStateMS_[state]=dt?dt:1;
        static const char*const names[5]={"","RTWDiagJoinScanMS","RTWDiagJoinAuthMS","RTWDiagJoinAssocMS","RTWDiagJoinRunMS"};
        s->setProperty(names[state],(unsigned long long)s->joinStateMS_[state],64);
    }
    return result;
}
void RTWUSBController::observeScanTransition(ieee80211_state previous,ieee80211_state requested,unsigned channel,int result) {
    const auto transition=requested==IEEE80211_S_SCAN?rtw::ScanDiagnosticTransition::Scan:
        requested==IEEE80211_S_AUTH?rtw::ScanDiagnosticTransition::Auth:
        requested==IEEE80211_S_INIT?rtw::ScanDiagnosticTransition::Init:rtw::ScanDiagnosticTransition::Other;
    rtw::scanDiagnosticsTransition(scanDiag_,previous==IEEE80211_S_SCAN,transition,channel,result);
    publishScanDiagnostics();
}

bool RTWUSBController::read88IdentityDescriptor(rtw_chip_identity_descriptor*out) const {
    if(!out||!usb_)return false;
    auto*device=usb_->getDevice();const auto*d=device?device->getDeviceDescriptor():nullptr;
    const auto*i=usb_->getInterfaceDescriptor();const auto*c=usb_->getConfigurationDescriptor();
    if(!d||!i||!c)return false;
    *out={USBToHost16(d->idVendor),USBToHost16(d->idProduct),USBToHost16(d->bcdDevice),
        c->bConfigurationValue,i->bInterfaceNumber,i->bInterfaceClass,i->bInterfaceSubClass,i->bInterfaceProtocol,i->bAlternateSetting};
    return true;
}
bool RTWUSBController::matches88Discovery() const {
    if(!r88ResolvedIdentity_)return true;
    rtw_chip_identity_descriptor descriptor{};
    return (const void*)r88ResolvedIdentity_==(const void*)r88IdentityStorage_&&
        usb_==r88DiscoveryProvider_&&profile_==rtw_chip_identity_profile(r88ResolvedIdentity_)&&
        profile_&&profile_->family==family_&&read88IdentityDescriptor(&descriptor)&&
        rtw_chip_identity_candidate(r88DiscoveryProfile_,&descriptor)&&
        rtw_chip_identity_descriptor_matches(r88ResolvedIdentity_,&descriptor);
}
bool RTWUSBController::permits88Transport(bool discoveryRead) const {
    if(!r88DiscoveryProvider_)return true;
    if(usb_!=r88DiscoveryProvider_)return false;
    if(!r88ResolvedIdentity_)return discoveryRead&&family_==RTW_WIFI_UNKNOWN&&profile_==r88DiscoveryProfile_;
    rtw_chip_identity_descriptor descriptor{};
    return (const void*)r88ResolvedIdentity_==(const void*)r88IdentityStorage_&&
        profile_==rtw_chip_identity_profile(r88ResolvedIdentity_)&&profile_&&profile_->family==family_&&
        read88IdentityDescriptor(&descriptor)&&rtw_chip_identity_descriptor_matches(r88ResolvedIdentity_,&descriptor);
}
bool RTWUSBController::owns88Discovery() const {
    if(!r88ResolvedIdentity_)return true;
    return (const void*)r88ResolvedIdentity_==(const void*)r88IdentityStorage_&&
        usb_==r88DiscoveryProvider_&&profile_==rtw_chip_identity_profile(r88ResolvedIdentity_)&&
        profile_&&profile_->family==family_&&rtw_chip_identity_bound_to(r88ResolvedIdentity_,&r88Core_)&&
        rtw_chip_identity_valid(r88ResolvedIdentity_,profile_,&r88Core_.raw_bus);
}
void RTWUSBController::clear88Discovery(bool forget) {
    if(!r88ResolvedIdentity_)return;
    removeProperty("RTWChipIdentityVerified");removeProperty("RTWHardwareChipCode");
    if(forget&&(const void*)r88ResolvedIdentity_==(const void*)r88IdentityStorage_)rtw_chip_identity_forget(r88ResolvedIdentity_);
    /* Keep the pointer after revocation: a failed discovery must never fall
     * back to the ordinary immutable-profile path on this controller. */
}
int RTWUSBController::prepare88Identity(const rtw88_fw_bus&bus) {
    if(!r88ResolvedIdentity_)return rtw88_boot_init(&r88Core_,profile_,&bus);
    if(!matches88Discovery()||!rtw_chip_identity_valid(r88ResolvedIdentity_,profile_,&bus)){
        clear88Discovery(true);return ENODEV;
    }
    int e=rtw88_boot_init_resolved(&r88Core_,r88ResolvedIdentity_,&bus);
    if(e)clear88Discovery(true);return e;
}
int RTWUSBController::recheck88Identity() {
    if(!r88ResolvedIdentity_)return 0;
    if(!matches88Discovery()||!owns88Discovery()){clear88Discovery(true);return ENODEV;}
    int e=rtw88_boot_revalidate_identity(&r88Core_);
    if(!e&&(!matches88Discovery()||!owns88Discovery()))e=ENODEV;
    if(e)clear88Discovery(true);return e;
}
int RTWUSBController::publish88Identity() {
    if(!r88ResolvedIdentity_)return 0;
    int e=recheck88Identity();if(e)return e;
    setProperty("RTWSiliconFamily",rtw_usb_wifi_family_name(family_));
    setProperty("RTWHardwareChipCode",(unsigned long long)rtw_chip_identity_code(r88ResolvedIdentity_),8);
    setProperty("RTWChipIdentityVerified",true);return 0;
}
bool RTWUSBController::discoverUSB() {
    auto why=[this](uint32_t code,uint32_t a=0,uint32_t b=0)->bool{if(usb_){usb_->setProperty("RTWDiscoveryFail",(unsigned long long)code,32);usb_->setProperty("RTWDiscoveryFailA",(unsigned long long)a,32);usb_->setProperty("RTWDiscoveryFailB",(unsigned long long)b,32);}return false;};
    IOUSBHostDevice *device=usb_->getDevice();
    const auto*d=device?device->getDeviceDescriptor():nullptr;
    const auto*i=usb_->getInterfaceDescriptor();const auto*c=usb_->getConfigurationDescriptor();
    if(!d||!i||!c)return why(1);
    profile_=rtw_usb_wifi_lookup_descriptor(USBToHost16(d->idVendor),USBToHost16(d->idProduct),
        USBToHost16(d->bcdDevice),c->bConfigurationValue,i->bInterfaceNumber,
        i->bInterfaceClass,i->bInterfaceSubClass,i->bInterfaceProtocol);
    r88DiscoveryProfile_=profile_;family_=rtw_usb_wifi_station_backend(profile_);
    if(family_==RTW_WIFI_UNKNOWN){
        rtw_chip_identity_descriptor descriptor{};
        if(!read88IdentityDescriptor(&descriptor)||!rtw_chip_identity_candidate(profile_,&descriptor))return why(2,(uint32_t)(uintptr_t)profile_!=0);
        const rtw88_fw_bus bus{this,r88Read,r88Write,r88Bulk,delayUS,nullptr,nullptr};
        r88DiscoveryProvider_=usb_;
        int e=rtw_chip_identity_init(r88IdentityStorage_,sizeof(r88IdentityStorage_),profile_,&descriptor,&bus,&r88ResolvedIdentity_);
        if(e||usb_!=r88DiscoveryProvider_){clear88Discovery(true);return false;}
        profile_=rtw_chip_identity_profile(r88ResolvedIdentity_);
        if(!profile_)return why(3);family_=profile_->family;
        if(!matches88Discovery()){clear88Discovery(true);return why(4);}
    }
    /* Shared family registry covers every catalog chipset, but only backends
     * with an integrated station HAL may bind to an IOEthernetInterface.
     * A validated USB identity is not authorization to transmit. */
    const rtw_family_backend *backend=rtw_backend_for_family(family_);
    if(family_==RTW_WIFI_UNKNOWN||!backend||
       !(backend->capabilities&RTW_BACKEND_CAP_KEXT_STATION))return why(5,(uint32_t)family_,backend?backend->capabilities:0);
    size_t length=USBToHost16(c->wTotalLength);
    if(length<9||length>4096||rtw_find_usb_bulk_endpoints((const uint8_t*)c,length,i->bInterfaceNumber,i->bAlternateSetting,&endpoints_)!=RTW_DESC_OK||endpoints_.num_bulk_in!=1||endpoints_.num_bulk_out<1||endpoints_.num_bulk_out>(family_==RTW_WIFI_RTL8814AU||is8812Family()||is88Family()||family_==RTW_WIFI_RTL8723BU?4:3))return why(6,endpoints_.num_bulk_in,endpoints_.num_bulk_out);
    /* RTL8723BU combo (Edimax EW-7611UCB) exposes 4 bulk-out endpoints; the station path uses the first 3. */
    if(family_==RTW_WIFI_RTL8723BU&&endpoints_.num_bulk_out>3)endpoints_.num_bulk_out=3;
    if((family_==RTW_WIFI_RTL8814AU||is8812Family()||is88Family())&&endpoints_.num_bulk_out<2)return why(7);
    rxPipe_=usb_->copyPipe(endpoints_.bulk_in[0]);if(!rxPipe_)return why(8);
    for(unsigned p=0;p<endpoints_.num_bulk_out;p++) {
        txPipes_[p]=usb_->copyPipe(endpoints_.bulk_out[p]);if(!txPipes_[p])return why(9,p);
        const auto*e=txPipes_[p]->getEndpointDescriptor();if(!e)return why(10,p);
        txMPS_[p]=USBToHost16(e->wMaxPacketSize)&0x7ff;
        if(txMPS_[p]!=64&&txMPS_[p]!=512&&!((family_==RTW_WIFI_RTL8814AU||is8812Family()||is88Family())&&txMPS_[p]==1024))return why(11,p,txMPS_[p]);
    }
    core_.ac2idx[EDCA_AC_BK]=core_.ac2idx[EDCA_AC_BE]=endpoints_.num_bulk_out>=3?2:endpoints_.num_bulk_out==2?1:0;
    core_.ac2idx[EDCA_AC_VI]=endpoints_.num_bulk_out>=3?1:0;core_.ac2idx[EDCA_AC_VO]=0;
    return true;
}
IOReturn RTWUSBController::initializePlatform() {
    reportStartStage("usb-discovery");
    if(!discoverUSB()){if(usb_&&!usb_->getProperty("RTWDiscoveryFail"))usb_->setProperty("RTWDiscoveryFail",(unsigned long long)99,32);reportStartStage("usb-discovery",kIOReturnUnsupported);return kIOReturnUnsupported;}
    core_.chip=RTWN_CHIP_USB;core_.sc_flags=0;
    if(family_==RTW_WIFI_RTL8192EU){core_.chip|=RTWN_CHIP_92E;core_.sc_flags=RTWN_FLAG_EXT_HDR;}
    else if(family_==RTW_WIFI_RTL8188EU)core_.chip|=RTWN_CHIP_88E;
    else if(family_==RTW_WIFI_RTL8192CU)core_.chip|=RTWN_CHIP_92C|RTWN_CHIP_88C;
    else if(family_!=RTW_WIFI_RTL8814AU&&!is8812Family()&&!is88Family()&&family_!=RTW_WIFI_RTL8192DU&&family_!=RTW_WIFI_RTL8192FU&&family_!=RTW_WIFI_RTL8710BU&&family_!=RTW_WIFI_RTL8723BU)return kIOReturnUnsupported;
    setProperty("RTWSiliconFamily",rtw_usb_wifi_family_name(family_));
    rtwn_ops&o=core_.sc_ops;o.cookie=this;o.delay_us=delayUS;o.write_region=writeRegion;o.io_error=ioError;
    o.read_1=read1;o.read_2=read2;o.read_4=read4;o.write_1=write1;o.write_2=write2;o.write_4=write4;
    o.tx=transmit;o.power_on=busPower;o.dma_init=busDMA;o.fw_loadpage=busFirmware;o.load_firmware=loadFirmware;
    o.mac_init=busMAC;o.bb_init=busBB;o.aggr_init=busAggregation;o.alloc_buffers=busAllocate;o.init=busInitialize;
    o.stop=busStop;o.is_oactive=busBusy;o.next_scan=nextScan;o.cancel_scan=cancelScan;
    o.next_calib=nextCalibration;o.cancel_calib=cancelCalibration;o.wait_async=waitAsync;
    device_.provider=usb_;device_._data=this;strlcpy(device_.dv_xname,rtw_usb_wifi_family_name(family_),sizeof(device_.dv_xname));
    reportStartStage("rtwn-attach");
    int result=is8723Family()?attach8723():is8710BUFamily()?attach8710BU():is8192FUFamily()?attach8192FU():family_==RTW_WIFI_RTL8192DU?attach8192DU():is88Family()?attach88():is8812Family()?attach8812():family_==RTW_WIFI_RTL8814AU?attach8814():rtwn_attach(&device_,&core_,this);
    if(result){reportStartStage("rtwn-attach",fromErrno(result));return fromErrno(result);}
    attached_=true;
    if(family_==RTW_WIFI_RTL8188EU){
        const struct r88e_rom&r=core_.sc_r88e_rom;char b[220];
        snprintf(b,sizeof b,"id=%04x xtal=%02x cck=%02x,%02x,%02x,%02x,%02x,%02x ht40=%02x,%02x,%02x,%02x,%02x diff=%02x chplan=%02x therm=%02x board=%02x feat=%02x ver=%02x cust=%02x vid=%04x pid=%04x mac=%02x:%02x:%02x:%02x:%02x:%02x",
            (unsigned)r.id,r.xtal,r.txpwr.cck_tx_pwr[0],r.txpwr.cck_tx_pwr[1],r.txpwr.cck_tx_pwr[2],r.txpwr.cck_tx_pwr[3],r.txpwr.cck_tx_pwr[4],r.txpwr.cck_tx_pwr[5],
            r.txpwr.ht40_tx_pwr[0],r.txpwr.ht40_tx_pwr[1],r.txpwr.ht40_tx_pwr[2],r.txpwr.ht40_tx_pwr[3],r.txpwr.ht40_tx_pwr[4],r.txpwr.ht20_ofdm_tx_pwr_diff,
            r.channel_plan,r.thermal_meter,r.rf_board_opt,r.rf_feature_opt,r.version,r.customer_id,(unsigned)r.r88eu_rom.vid,(unsigned)r.r88eu_rom.pid,
            r.r88eu_rom.macaddr[0],r.r88eu_rom.macaddr[1],r.r88eu_rom.macaddr[2],r.r88eu_rom.macaddr[3],r.r88eu_rom.macaddr[4],r.r88eu_rom.macaddr[5]);
        setProperty("RTWEfuse88E",b);IOLog("[macOS-RTL-WIFI 1.0.20] 8188EU efuse: %s\n",b);
        ccmpSelfTest();
    }
    if(family_==RTW_WIFI_RTL8192EU){
        /* 1.0.33: the TL-WN821N (2357:0107) sees networks but its AUTH frames are never answered; publish the ROM it runs on. */
        const struct r92e_rom&r=core_.sc_r92e_rom;char b[300];
        snprintf(b,sizeof b,"id=%04x xtal=%02x chplan=%02x therm=%02x pa=%02x lna2=%02x board=%02x feat=%02x ant=%02x ver=%02x cust=%02x vid=%04x pid=%04x txchains=%d A:cck=%02x,%02x,%02x,%02x,%02x,%02x ht40=%02x,%02x,%02x,%02x,%02x diff=%02x B:cck=%02x,%02x,%02x,%02x,%02x,%02x ht40=%02x,%02x,%02x,%02x,%02x diff=%02x",
            (unsigned)r.id,r.xtal_k,r.channel_plan,r.thermal_meter,r.pa_type,r.lna_type_2g,r.rf_board_opt,r.rf_feature_opt,r.rf_antenna_option,r.eeprom_version,r.eeprom_customer_id,(unsigned)r.vid,(unsigned)r.pid,(int)core_.ntxchains,
            r.txpwr_a.cck_tx_pwr[0],r.txpwr_a.cck_tx_pwr[1],r.txpwr_a.cck_tx_pwr[2],r.txpwr_a.cck_tx_pwr[3],r.txpwr_a.cck_tx_pwr[4],r.txpwr_a.cck_tx_pwr[5],
            r.txpwr_a.ht40_tx_pwr[0],r.txpwr_a.ht40_tx_pwr[1],r.txpwr_a.ht40_tx_pwr[2],r.txpwr_a.ht40_tx_pwr[3],r.txpwr_a.ht40_tx_pwr[4],r.txpwr_a.ht20_ofdm_tx_pwr_diff,
            r.txpwr_b.cck_tx_pwr[0],r.txpwr_b.cck_tx_pwr[1],r.txpwr_b.cck_tx_pwr[2],r.txpwr_b.cck_tx_pwr[3],r.txpwr_b.cck_tx_pwr[4],r.txpwr_b.cck_tx_pwr[5],
            r.txpwr_b.ht40_tx_pwr[0],r.txpwr_b.ht40_tx_pwr[1],r.txpwr_b.ht40_tx_pwr[2],r.txpwr_b.ht40_tx_pwr[3],r.txpwr_b.ht40_tx_pwr[4],r.txpwr_b.ht20_ofdm_tx_pwr_diff);
        setProperty("RTWEfuse92E",b);IOLog("[macOS-RTL-WIFI 1.0.33] 8192EU efuse: %s\n",b);
    }
    bool nonzero=false;for(unsigned n=0;n<6;n++)nonzero|=core_.sc_ic.ic_myaddr[n]!=0;
    if(!nonzero||(core_.sc_ic.ic_myaddr[0]&1)||rtwn_io_error(&core_)){reportStartStage("efuse-mac",kIOReturnIOError);return kIOReturnIOError;}
    memcpy(mac_.bytes,core_.sc_ic.ic_myaddr,6);macReady_=true;
    core_.sc_ic.ic_newstate=stateTransition;core_.sc_ic.ic_max_rssi=100;
    core_.sc_ic.ic_ampdu_rx_start=family_==RTW_WIFI_RTL8192DU?ampdu8192DURXStart:ampduRXStart;core_.sc_ic.ic_ampdu_rx_stop=family_==RTW_WIFI_RTL8192DU?ampdu8192DURXStop:ampduRXStop;
    core_.sc_ic.ic_ampdu_tx_start=ampduTXStart;core_.sc_ic.ic_ampdu_tx_stop=ampduTXStop;
    core_.sc_ic.ic_flags&=~IEEE80211_F_AUTO_JOIN;
    super::setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}
int RTWUSBController::allocateTransfers() {
    if(!completionLock_)return ENXIO;
    for(unsigned i=0;i<RTW_TRANSFER_SLOTS;i++) {
        Transfer&t=transfers_[i];t.owner=this;t.index=i;
        const uint32_t capacity=i<RXSlots?rtw_usb_rx_capacity(family_):TXCapacity;
        if(!t.memory)t.memory=IOBufferMemoryDescriptor::withCapacity(capacity,i<RXSlots?kIODirectionIn:kIODirectionOut);
        if(!t.memory||t.memory->getCapacity()<capacity||!t.memory->getBytesNoCopy()){freeTransfers();return ENOMEM;}
    }
    return 0;
}
void RTWUSBController::freeTransfers() {
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    bool drained=rtw_transfer_drained(&ledger_);IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!drained)return;
    for(auto&t:transfers_) {if(t.prepared&&t.memory){t.memory->complete();t.prepared=false;}if(t.memory){t.memory->release();t.memory=nullptr;}t.pipe=nullptr;}
}
bool RTWUSBController::hasFreeTX() const {
    if(!running_||stopping_||!present_||usbError_!=kIOReturnSuccess)return false;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);bool available=false;
    for(unsigned i=RXSlots;i<RTW_TRANSFER_SLOTS;i++)if(!(ledger_.pending&(1U<<i))){available=true;break;}
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);return available;
}
int RTWUSBController::submit(unsigned index,IOUSBHostPipe*pipe,uint32_t length) {
    if(index>=RTW_TRANSFER_SLOTS||!running_||stopping_||!present_||sleeping_||usbError_!=kIOReturnSuccess||!pipe)return ENXIO;
    if(index<RXSlots&&rxRecovery_.pending&&!rxRecovery_.refilling)return EBUSY;
    Transfer&t=transfers_[index];if(!t.memory||length>t.memory->getCapacity())return EINVAL;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    bool accepted=rtw_transfer_begin(&ledger_,index);
    if(accepted&&++t.generation==0)++t.generation;
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!accepted)return EBUSY;
    t.pipe=pipe;t.submitted=length;t.actual=0;t.result=kIOReturnNotReady;
    retain(); /* released only after completion is recorded and reclaimed */
    IOReturn result=t.memory->prepare();
    bool transientRXRejection=false;
    if(result==kIOReturnSuccess){
        t.prepared=true;IOUSBHostCompletion completion{this,usbCompletion,(void*)(((uintptr_t)t.generation<<4)|index)};
        result=pipe->io(t.memory,length,&completion,index<RXSlots?0:1000);
        transientRXRejection=index<RXSlots&&result==kIOReturnNotResponding;
    }
    if(result!=kIOReturnSuccess){
        /* A failed recovery must retain the original transaction error,
         * rather than replacing it with the refill's secondary error. */
        if(index<RXSlots&&rxRecovery_.refilling)rxRecovery_.lastError=result;
        else if(!transientRXRejection)fault(result,3,(uint32_t)index);
        /* A halted pipe can reject io() before accepting a callback. Record
         * that zero-byte result just like an asynchronous RX completion. A
         * prepare() failure never enters this transport recovery path. */
        recordCompletion(&t,result,0,t.generation);return transientRXRejection?EAGAIN:EIO;
    }
    return 0;
}
bool RTWUSBController::beginRXRecovery(IOReturn error) {
    const auto*backend=rtw_backend_for_family(family_);
    if(!backend||!(backend->capabilities&RTW_BACKEND_CAP_KEXT_STATION)||error!=kIOReturnNotResponding||
       !attached_||!running_||stopping_||sleeping_||closing_||!present_||
       usbError_!=kIOReturnSuccess||core_.core_error||!rxPipe_||!watchdogTimer_||rxRecovery_.pending)return false;
    auto&r=rxRecovery_;
    ++r.errors;r.lastError=error;r.originalError=error;
    if(!r.endpoint)r.endpoint=endpoints_.bulk_in[0];
    if(r.consecutive>=3){++r.failures;++r.exhausted;r.failureStage=RXRecovery::Idle;r.stage=RXRecovery::Failed;return false;}
    ++r.consecutive;++r.attempts;r.startedMS=nowMS();
    r.nextAttemptMS=0;r.clearAttempt=0;r.refillSlot=RXSlots;
    r.clearResult=r.refillResult=kIOReturnSuccess;r.failureStage=RXRecovery::Idle;r.stage=RXRecovery::Clear;
    r.pending=true;r.cleared=false;r.refilling=false;
    watchdogTimer_->setTimeoutMS(100);
    return true;
}
void RTWUSBController::failRXRecovery(bool exhausted) {
    auto&r=rxRecovery_;
    ++r.failures;if(exhausted)++r.exhausted;
    r.failureStage=r.stage;r.stage=RXRecovery::Failed;r.nextAttemptMS=0;
    r.pending=r.cleared=r.refilling=false;
    fault(r.originalError,4,0);
    if(attached_&&!stopping_)quiesce(false);
}
void RTWUSBController::advanceRXRecovery() {
    auto&r=rxRecovery_;
    if(!r.pending)return;
    if(!running_||stopping_||sleeping_||closing_||!present_||usbError_!=kIOReturnSuccess||core_.core_error){
        r.pending=r.cleared=r.refilling=false;r.stage=RXRecovery::Cancelled;r.nextAttemptMS=0;return;
    }
    if(nowMS()-r.startedMS>=2000){r.lastError=kIOReturnTimeout;failRXRecovery();return;}
    if(r.nextAttemptMS&&nowMS()<r.nextAttemptMS)return;
    r.nextAttemptMS=0;
    auto retryTransient=[&]{
        r.refilling=false;
        if(nowMS()-r.startedMS>=2000){r.lastError=kIOReturnTimeout;failRXRecovery();return;}
        if(r.clearAttempt>=3){++r.transportExhausted;failRXRecovery();return;}
        ++r.transportRetries;r.cleared=false;r.stage=RXRecovery::RetryWait;
        r.nextAttemptMS=nowMS()+100;
        watchdogTimer_->setTimeoutMS(100);
    };
    if(!r.cleared){
        /* IOUSBHostPipe halts after a non-timeout bulk error. clearStall(true)
         * synchronizes device/host toggles and aborts the other old RX reads.
         * Their callbacks only record completions; never refill in this call. */
        r.stage=RXRecovery::Clear;++r.clearAttempt;++r.clearAttempts;
        IOReturn result=rxPipe_->clearStall(true);
        r.clearResult=result;
        if(result!=kIOReturnSuccess){
            ++r.clearFailures;r.lastError=result;
            if(result==kIOReturnNotResponding)retryTransient();else failRXRecovery();
            return;
        }
        if(nowMS()-r.startedMS>=2000){r.lastError=kIOReturnTimeout;failRXRecovery();return;}
        r.cleared=true;
    }
    r.stage=RXRecovery::Drain;
    const uint32_t rxMask=(1U<<RXSlots)-1U;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    bool drained=(ledger_.pending&rxMask)==0;
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!drained)return;
    r.refilling=true;r.stage=RXRecovery::Refill;
    for(unsigned i=0;i<RXSlots;i++){
        r.refillSlot=i;
        int error=submit(i,rxPipe_,rtw_usb_rx_capacity(family_));
        if(error){
            ++r.refillFailures;r.refillResult=r.lastError;r.refilling=false;
            /* Some reads in this refill may already be accepted or complete.
             * A fresh clear/drain must discard the entire partial epoch before
             * any slot is reused; the whole recovery retains its deadline. */
            if(error==EAGAIN)retryTransient();else failRXRecovery();
            return;
        }
        r.refillResult=kIOReturnSuccess;
        if(nowMS()-r.startedMS>=2000){r.lastError=kIOReturnTimeout;r.refilling=false;failRXRecovery();return;}
    }
    r.pending=r.cleared=r.refilling=false;r.stage=RXRecovery::Complete;++r.completed;
}
void RTWUSBController::publishRXRecoveryDiagnostics() {
    auto publish=[this](const char*name,uint64_t value){publishDiagnosticCounter(name,value);};
    const auto&r=rxRecovery_;
    publish("RTWDiagRXRecoveryErrors",r.errors);publish("RTWDiagRXRecoveryAttempts",r.attempts);
    publish("RTWDiagRXRecoveryCompleted",r.completed);publish("RTWDiagRXRecoveryFailures",r.failures);
    publish("RTWDiagRXRecoveryExhausted",r.exhausted);publish("RTWDiagRXRecoveryPending",r.pending?1:0);
    publish("RTWDiagRXRecoveryConsecutive",r.consecutive);publish("RTWDiagRXRecoveryEndpoint",r.endpoint);
    publish("RTWDiagRXRecoveryOriginalError",(uint32_t)r.originalError);
    publish("RTWDiagRXRecoveryLastError",(uint32_t)r.lastError);
    publish("RTWDiagRXRecoveryStage",r.stage);publish("RTWDiagRXRecoveryFailureStage",r.failureStage);
    publish("RTWDiagRXRecoveryClearResult",(uint32_t)r.clearResult);
    publish("RTWDiagRXRecoveryRefillResult",(uint32_t)r.refillResult);
    publish("RTWDiagRXRecoveryClearAttempts",r.clearAttempts);publish("RTWDiagRXRecoveryClearAttempt",r.clearAttempt);
    publish("RTWDiagRXRecoveryClearFailures",r.clearFailures);publish("RTWDiagRXRecoveryRefillFailures",r.refillFailures);
    publish("RTWDiagRXRecoveryTransportRetries",r.transportRetries);publish("RTWDiagRXRecoveryTransportExhausted",r.transportExhausted);
    publish("RTWDiagRXRecoveryRefillSlot",r.refillSlot);
}
void RTWUSBController::recordCompletion(Transfer*t,IOReturn result,uint32_t length,uint32_t generation) {
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    if(t->generation==generation&&rtw_transfer_complete(&ledger_,t->index)){t->result=result;t->actual=length;}
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(completionEvent_)completionEvent_->interruptOccurred(nullptr,nullptr,0);
}
void RTWUSBController::usbCompletion(void*owner,void*parameter,IOReturn result,uint32_t length) {
    auto*s=static_cast<RTWUSBController*>(owner);
    uintptr_t token=(uintptr_t)parameter;unsigned index=(unsigned)(token&15);
    if(s&&index<RTW_TRANSFER_SLOTS)s->recordCompletion(&s->transfers_[index],result,length,(uint32_t)(token>>4));
}
void RTWUSBController::completionAction(OSObject*owner,IOInterruptEventSource*,int) { static_cast<RTWUSBController*>(owner)->drainCompletions(true); }
void RTWUSBController::serviceTXOutputQueue() {
    if(closing_||stopping_||!authorized())return;
    auto*pending=core_.sc_ic.ic_if.if_snd.queue;
    if(!pending||pending->getSize()>=pending->getCapacity())return;
    // The completion path holds the workloop gate. Defer the outer queue's
    // retry rather than reenter its dequeue/locking path synchronously.
    if(auto*queue=getOutputQueue())queue->service(IOBasicOutputQueue::kServiceAsync);
}
void RTWUSBController::drainCompletions(bool process) {
    retain();bool failure=false,rxRecoveryFatal=false;
    /* A watchdog-queued common-core reinit also finishes with IFF_RUNNING.
     * This handler cannot interleave with that gate-held initialization. */
    if(running_&&(core_.sc_ic.ic_if.if_flags&IFF_RUNNING)&&!rtwn_io_error(&core_))radioReady_=enabled_=true;
    if(process&&running_&&!rxRecovery_.pending){
        /* An endpoint error can complete behind an older RX callback hole.
         * Inspect only completion metadata here: waiting for ordered payload
         * delivery could otherwise prevent both halt clearing and its timer. */
        bool transactionError=false;
        IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
        for(unsigned i=0;i<RXSlots;i++)if((ledger_.completed&(1U<<i))&&
           transfers_[i].result==kIOReturnNotResponding&&!transfers_[i].actual){transactionError=true;break;}
        IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
        if(transactionError){
            ++rxErrors_;
            if(!beginRXRecovery(kIOReturnNotResponding)){
                failure=rxRecoveryFatal=true;fault(kIOReturnNotResponding,5,0);
            }
        }
    }
    auto reclaim=[&](unsigned i){
        Transfer&t=transfers_[i];
        if(t.prepared){t.memory->complete();t.prepared=false;}
        bool success=t.result==kIOReturnSuccess&&t.actual<=t.submitted;
        if(i>=RXSlots)success=success&&t.actual==t.submitted;
        if(i<RXSlots){
            /* Clearing a halt may cancel the other old reads. Only those
             * cancellations and the same zero-byte transaction error belong
             * to this recovery; never hide a different coalesced RX fault. */
            if(process&&running_&&rxRecovery_.pending&&!success&&t.result!=kIOReturnAborted&&
               !(t.result==kIOReturnNotResponding&&!t.actual)){
                ++rxErrors_;failure=true;fault(t.result==kIOReturnSuccess?kIOReturnUnderrun:t.result,6,(uint32_t)t.actual);
            }
            if(process&&running_&&!failure&&!rxRecoveryFatal&&!rxRecovery_.pending&&success&&t.actual){
                const uint64_t before=rxPackets_;
                receive((const uint8_t*)t.memory->getBytesNoCopy(),t.actual);
                /* Empty/malformed transfers cannot replenish the retry budget. */
                if(rxPackets_>before)rxRecovery_.consecutive=0;
            }
            if(process&&running_&&!rxRecoveryFatal&&!rxRecovery_.pending&&!success&&t.result!=kIOReturnAborted){
                rxErrors_++;
                if(t.actual||!beginRXRecovery(t.result)){failure=true;fault(t.result==kIOReturnSuccess?kIOReturnUnderrun:t.result,7,(uint32_t)t.actual);}
            }
        }else{
            if(success){txPackets_++;if(stats_)stats_->outputPackets++;core_.sc_tx_timer=0;}
            else if(process&&running_&&t.result!=kIOReturnAborted){
                txErrors_++;txUSBTransferErrors_++;if(stats_)stats_->outputErrors++;
                failure=true;fault(t.result==kIOReturnSuccess?kIOReturnUnderrun:t.result,8,(uint32_t)t.actual);
            }
        }
        t.pipe=nullptr;
        if(i<RXSlots&&process&&running_&&!stopping_&&!failure&&!rxRecovery_.pending&&usbError_==kIOReturnSuccess){
            int error=submit(i,rxPipe_,rtw_usb_rx_capacity(family_));
            if(error==EAGAIN){
                ++rxErrors_;
                if(!beginRXRecovery(kIOReturnNotResponding)){failure=true;fault(kIOReturnNotResponding,9,0);}
            }else if(error){failure=true;fault(kIOReturnIOError,10,0);}
        }
        release();
    };
    const uint32_t rxMask=(1U<<RXSlots)-1U;
    unsigned rxProcessed=0;
    if(process&&!rxRecovery_.pending){
        /* A recycled low-numbered slot follows older endpoint reads. Retain
         * submission order even when callbacks are delayed or coalesced. */
        while(rxProcessed<RXSlots){
            unsigned i=0;
            IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
            bool done=rtw_transfer_take_oldest(&ledger_,rxMask,&i);
            IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
            if(!done)break;
            ++rxProcessed;reclaim(i);
            if(rxRecovery_.pending)break;
        }
    }else{
        /* Abort cleanup must reclaim completed reads despite a callback hole. */
        for(unsigned i=0;i<RXSlots;i++){
            IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
            bool done=rtw_transfer_take(&ledger_,i);
            IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
            if(done)reclaim(i);
        }
    }
    for(unsigned i=RXSlots;i<RTW_TRANSFER_SLOTS;i++){
        IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
        bool done=rtw_transfer_take(&ledger_,i);
        IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
        if(done)reclaim(i);
    }
    if(process&&!rxRecovery_.pending&&rxProcessed==RXSlots&&running_&&!stopping_&&!failure){
        IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
        bool ready=rtw_transfer_oldest_ready(&ledger_,rxMask);
        IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
        if(ready&&completionEvent_)completionEvent_->interruptOccurred(nullptr,nullptr,0);
    }
    if(failure&&attached_&&!stopping_){
        if(rxRecovery_.pending){rxRecovery_.lastError=usbError_;failRXRecovery();}
        else quiesce(false);
    }
    if(process&&rxRecovery_.pending)advanceRXRecovery();
    if(process&&running_&&!stopping_&&hasFreeTX()){
        ifq_clr_oactive(&core_.sc_ic.ic_if.if_snd);rtwn_start(&core_.sc_ic.ic_if);
        serviceTXOutputQueue();
    }
    scheduleDeferredCleanup();
    release();
}
void RTWUSBController::abortTransfers() {
    running_=false;radioReady_=false;rateTransportFresh_=false;
    if(rxRecovery_.pending)rxRecovery_.stage=RXRecovery::Cancelled;
    rxRecovery_.nextAttemptMS=0;
    rxRecovery_.pending=rxRecovery_.cleared=rxRecovery_.refilling=false;
    cancelWpaRecovery();
    if(scanTimer_)scanTimer_->cancelTimeout();if(calibTimer_)calibTimer_->cancelTimeout();if(watchdogTimer_)watchdogTimer_->cancelTimeout();
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);rtw_transfer_stop(&ledger_);IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(rxPipe_)rxPipe_->abort(IOUSBHostIOSource::kAbortSynchronous);
    for(auto*p:txPipes_)if(p)p->abort(IOUSBHostIOSource::kAbortSynchronous);
    drainCompletions(false);
}
void RTWUSBController::resetRateControl() {
    auto&r=rateControl_;
    /* CCX has no JOIN generation or 802.11 sequence/tag. Once this
     * association requested a report, only a complete firmware/DMA/USB
     * restart can certify an empty previous transport epoch. */
    if(r.requestedSinceReset)rateTransportFresh_=false;
    r.requestedSinceReset=false;r.pendingReports=0;r.active=false;r.seenSequence=false;
    eapolReportPending_=0;
    dhcpReportPending_=0;/* R24: a new station epoch starts the probe bookkeeping over. */
    r.policy.amrr_min_success_threshold=1;r.policy.amrr_max_success_threshold=15;
    ieee80211_amrr_node_init(&r.policy,&r.node);++r.resets;
    memset(r.bssid,0,sizeof(r.bssid));r.finalRate=0;
    auto*node=core_.sc_ic.ic_bss;
    if(node){
        if(family_!=RTW_WIFI_RTL8192CU&&family_!=RTW_WIFI_RTL8814AU&&!is8812Family()&&!is88Family()&&family_!=RTW_WIFI_RTL8192DU&&family_!=RTW_WIFI_RTL8192FU&&family_!=RTW_WIFI_RTL8710BU&&family_!=RTW_WIFI_RTL8723BU)node->ni_txrate=0;
        IEEE80211_ADDR_COPY(r.bssid,node->ni_bssid);
        r.active=family_!=RTW_WIFI_RTL8192CU&&family_!=RTW_WIFI_RTL8814AU&&!is8812Family()&&!is88Family()&&family_!=RTW_WIFI_RTL8192DU&&family_!=RTW_WIFI_RTL8192FU&&family_!=RTW_WIFI_RTL8710BU&&family_!=RTW_WIFI_RTL8723BU&&running_&&!stopping_&&rateTransportFresh_&&core_.sc_ic.ic_state==IEEE80211_S_RUN;
    }
}
void RTWUSBController::handleTxReport(const uint8_t*bytes,size_t length) {
    ++txReportCalls_;txReportLastLen_=length;
    /* R24: log every 88E TX report while a DHCP probe is outstanding.
     * With the lease dead no other host-rate frame requests reports, so
     * each verdict names the firmware's own outcome for the DISCOVER:
     * txok=0/retries=0 with no flags = transmitted (broadcast, no ACK
     * expected), a lifetime/retry flag = dropped inside the firmware,
     * silence = never attempted. This is logged before the rate-control
     * gate because DHCP probes are not AMRR samples. */
    if(family_==RTW_WIFI_RTL8188EU&&dhcpReportPending_){
        rtw_88e_tx_report probe{};
        if(rtw_88e_parse_tx_report(bytes,length,&probe)){
            ++dhcpTxReports_;--dhcpReportPending_;
            if(probe.packet_ok)++dhcpRepOK_;if(probe.report.retry_over)++dhcpRepRetryOver_;if(probe.report.lifetime_expired)++dhcpRepLifetime_;dhcpRepRetries_+=probe.report.retries;dhcpRepRate_=probe.report.final_rate;
            IOLog("RtWlanUCompat: dhcp TX report: txok=%u retries=%u rate=%u lifetime_over=%u retry_over=%u (pending=%u)\n",
                  (unsigned)probe.packet_ok,(unsigned)probe.report.retries,
                  (unsigned)probe.report.final_rate,(unsigned)probe.report.lifetime_expired,
                  (unsigned)probe.report.retry_over,dhcpReportPending_);
        }
    }
    else if(family_==RTW_WIFI_RTL8188EU&&eapolReportPending_){
        rtw_88e_tx_report probe{};
        if(rtw_88e_parse_tx_report(bytes,length,&probe)){
            --eapolReportPending_;++eapolReports_;
            if(probe.packet_ok)++eapolRepOK_;if(probe.report.retry_over)++eapolRepRetryOver_;if(probe.report.lifetime_expired)++eapolRepLifetime_;
            eapolRepRetries_+=probe.report.retries;eapolRepRate_=probe.report.final_rate;
        }
    }
    auto&r=rateControl_;auto*node=core_.sc_ic.ic_bss;
    if(!running_||stopping_||!authorized()||core_.sc_ic.ic_state!=IEEE80211_S_RUN||
       !node||!r.active||!rateTransportFresh_||!IEEE80211_ADDR_EQ(r.bssid,node->ni_bssid)||
       !r.pendingReports){++r.ignored;return;}
    rtw_92e_tx_report report{};bool packetOK=true;
    if(family_==RTW_WIFI_RTL8188EU){
        rtw_88e_tx_report legacy{};
        if(!rtw_88e_parse_tx_report(bytes,length,&legacy)){++r.invalid;return;}
        report=legacy.report;packetOK=legacy.packet_ok;
    }else if(family_==RTW_WIFI_RTL8192EU){
        if(bytes&&length&&bytes[0]!=3U){++r.ignored;return;}
        if(!rtw_92e_parse_tx_report(bytes,length,&report)){++r.invalid;return;}
    }else {++r.ignored;return;}
    /* The C2H sequence is not a per-MPDU tag. Repeated values are legal
     * feedback candidates, bounded by this epoch's pending SPE requests. */
    if(family_==RTW_WIFI_RTL8192EU&&r.seenSequence&&r.lastSequence==report.event_sequence)++r.repeatedSequences;
    static const uint8_t values[12]={2,4,11,22,12,18,24,36,48,72,96,108};
    bool negotiated=false;
    if(usesFirmwareRate())negotiated=(core_.rate_config.rate_mask & (1U<<report.final_rate))!=0;
    if(report.final_rate<12 && node->ni_rates.rs_nrates<=nitems(node->ni_rates.rs_rates))
        for(unsigned i=0;i<node->ni_rates.rs_nrates;i++)
            if((node->ni_rates.rs_rates[i]&IEEE80211_RATE_VAL)==values[report.final_rate])negotiated=true;
    if(!negotiated){++r.invalid;return;}
    r.seenSequence=true;r.lastSequence=report.event_sequence;r.finalRate=report.final_rate;
    --r.pendingReports;++r.reports;
    bool failed=!packetOK||report.lifetime_expired||report.retry_over;
    if(failed)++r.failures;
    /* Count RF report outcomes, never USB completions. A failed packet with
     * zero retries is still a failed AMRR sample, not a clean success. */
    if(report.retries||failed)++r.retried;
    if(!usesFirmwareRate()){
        if(r.node.amn_txcnt<~0U)++r.node.amn_txcnt;
        if((report.retries||failed)&&r.node.amn_retrycnt<~0U)++r.node.amn_retrycnt;
    }
}
void RTWUSBController::chooseRateControl() {
    auto&r=rateControl_;auto*node=core_.sc_ic.ic_bss;
    if(!usesFirmwareRate()&&!usesLegacyFirmwareRate()&&!uses8188FirmwareRate()&&!uses8814FirmwareRate()&&!is8812Family()&&!is88Family()&&family_!=RTW_WIFI_RTL8192DU&&family_!=RTW_WIFI_RTL8192FU&&family_!=RTW_WIFI_RTL8710BU&&family_!=RTW_WIFI_RTL8723BU&&running_&&!stopping_&&authorized()&&core_.sc_ic.ic_state==IEEE80211_S_RUN&&
       node&&r.active&&rateTransportFresh_&&IEEE80211_ADDR_EQ(r.bssid,node->ni_bssid)&&
       node->ni_rates.rs_nrates&&node->ni_rates.rs_nrates<=nitems(node->ni_rates.rs_rates)&&
       node->ni_txrate<node->ni_rates.rs_nrates){
        uint8_t previous=node->ni_txrate;
        ieee80211_amrr_choose(&r.policy,node,&r.node);
        if(node->ni_txrate!=previous)++r.changes;
    }
    publishRateControlDiagnostics();
}
void RTWUSBController::publishRateControlDiagnostics() {
    /* Observer only. STATUS must never run AMRR, touch USB or select a rate.
     * The native backends return from calibAction before its AMRR branch. */
    const auto&r=rateControl_;const auto*node=core_.sc_ic.ic_bss;
    const bool associated=running_&&!stopping_&&core_.sc_ic.ic_state==IEEE80211_S_RUN&&node;
    uint64_t selected=0;
    if(associated&&node->ni_rates.rs_nrates<=nitems(node->ni_rates.rs_rates)&&node->ni_txrate<node->ni_rates.rs_nrates)
        selected=node->ni_rates.rs_rates[node->ni_txrate]&IEEE80211_RATE_VAL;
    if(usesFirmwareRate()||usesLegacyFirmwareRate()||uses8188FirmwareRate()||uses8814FirmwareRate()||uses8812FirmwareRate()||uses88FirmwareRate()||uses8192DUFirmwareRate()||uses8192FUFirmwareRate()||uses8710BUFirmwareRate()||uses8723FirmwareRate())selected=0;
    publishDiagnosticCounter("RTWDiagFirmwareRateControl",(uint64_t)(usesFirmwareRate()||usesLegacyFirmwareRate()||uses8188FirmwareRate()||uses8814FirmwareRate()||uses8812FirmwareRate()||uses88FirmwareRate()||uses8192DUFirmwareRate()||uses8192FUFirmwareRate()||uses8710BUFirmwareRate()||uses8723FirmwareRate()));
    publishDiagnosticCounter("RTWDiagSiliconFamily",(uint64_t)family_);
    publishDiagnosticCounter("RTWDiagTXRate500Kbps",selected);
    const auto* hwRate=uses88FirmwareRate()?&r88Rates_.rates:uses8814FirmwareRate()?&acRateConfig_:nullptr;
    const bool acHT=hwRate&&hwRate->ht,auHT=uses8812HighThroughput();
    const bool auVHT=auHT&&auRateConfig_.vht;
    const bool duHT=uses8192DUFirmwareRate()&&duMAC_.rates.ht,fuHT=uses8192FUHighThroughput(),buHT=uses8710BUHighThroughput(),r23HT=uses8723FirmwareRate()&&(r23Rates_.mask&0xff000U);
    const bool htNegotiated=usesFirmwareRate()||uses8188FirmwareRate()||acHT||auHT||duHT||fuHT||buHT||r23HT;
    publishDiagnosticCounter("RTWDiagHTNegotiated",(uint64_t)htNegotiated);
    publishDiagnosticCounter("RTWDiagVHTNegotiated",(uint64_t)auVHT);
    publishDiagnosticCounter("RTWDiagHTRateMask",htNegotiated?(uint64_t)(r23HT?r23Rates_.mask:buHT?buRate_.mask:fuHT?fuRate_.mask:duHT?duMAC_.rates.mask:auHT?auRateConfig_.mask:acHT?hwRate->mask:core_.rate_config.rate_mask):0);
    publishDiagnosticCounter("RTWDiagHTSGI20Negotiated",(uint64_t)((usesFirmwareRate()&&core_.rate_config.sgi20)||(uses8188FirmwareRate()&&core_.rate_config.sgi20)||(r23HT&&r23Rates_.sgi)||(buHT&&buRate_.width_mhz==20&&buRate_.sgi)||(fuHT&&fuRate_.width_mhz==20&&fuRate_.sgi)||(duHT&&duMAC_.rates.sgi20)||(acHT&&hwRate->sgi20)||(auHT&&auRateConfig_.bandwidth_mhz==20&&auRateConfig_.short_gi)));
    publishDiagnosticCounter("RTWDiagHTSGI40Negotiated",(uint64_t)((usesFirmwareRate()&&core_.rate_config.sgi40)||(buHT&&buRate_.width_mhz==40&&buRate_.sgi)||(fuHT&&fuRate_.width_mhz==40&&fuRate_.sgi)||(auHT&&auRateConfig_.bandwidth_mhz==40&&auRateConfig_.short_gi)));
    publishDiagnosticCounter("RTWDiagVHTSGI80Negotiated",(uint64_t)(auVHT&&auRateConfig_.bandwidth_mhz==80&&auRateConfig_.short_gi));
    publishDiagnosticCounter("RTWDiagHTChannelWidthMHz",(uint64_t)(r23HT?20:buHT?buRate_.width_mhz:fuHT?fuRate_.width_mhz:duHT?20:auHT?auRateConfig_.bandwidth_mhz:acHT?20:uses8188FirmwareRate()?20:usesFirmwareRate()?(core_.rate_config.bw40?40:20):0));
    publishDiagnosticCounter("RTWDiagHTTXStreams",htNegotiated?(uint64_t)(r23HT||buHT?1:fuHT?((fuRate_.mask&0x0ff00000U)?2:1):duHT?((duMAC_.rates.mask&0x0ff00000U)?2:1):auHT?auRateConfig_.streams:acHT?hwRate->tx_streams:core_.rate_config.tx_streams):0);
    publishDiagnosticCounter("RTWDiagHTRAID",htNegotiated?(uint64_t)(r23HT?r23Rates_.raid:buHT?buRate_.rate_id:fuHT?fuRate_.rate_id:duHT?duMAC_.rates.raid:auHT?auRateConfig_.rate_id:acHT?hwRate->rate_id:core_.rate_config.raid):0);
    publishDiagnosticCounter("RTWDiagHTFWRateSubmitted",txHTPackets_);
    publishDiagnosticCounter("RTWDiagHTTXAMPDURequested",txAMPDUPackets_);
    publishDiagnosticCounter("RTWDiagHTRXBARPackets",rxBARPackets_);
    uint64_t txBA=0,rxBA=0;
    if(associated)for(unsigned tid=0;tid<16;++tid){
        if(node->ni_tx_ba[tid].ba_state==IEEE80211_BA_AGREED)txBA|=1U<<tid;
        if(node->ni_rx_ba[tid].ba_state==IEEE80211_BA_AGREED)rxBA|=1U<<tid;
    }
    publishDiagnosticCounter("RTWDiagHTTXBATIDs",txBA);publishDiagnosticCounter("RTWDiagHTRXBATIDs",rxBA);
    publishDiagnosticCounter("RTWDiagTXRateFeedbackActive",(uint64_t)(r.active&&rateTransportFresh_&&running_&&!stopping_&&
        authorized()&&core_.sc_ic.ic_state==IEEE80211_S_RUN&&node&&IEEE80211_ADDR_EQ(r.bssid,node->ni_bssid)));
    publishDiagnosticCounter("RTWDiagTXRateRequests",r.requests);publishDiagnosticCounter("RTWDiagTXRatePending",r.pendingReports);
    publishDiagnosticCounter("RTWDiagTXRateReports",r.reports);publishDiagnosticCounter("RTWDiagTXRateFailures",r.failures);
    publishDiagnosticCounter("RTWDiagTXRateRetried",r.retried);publishDiagnosticCounter("RTWDiagTXRateInvalid",r.invalid);
    publishDiagnosticCounter("RTWDiagTXRateIgnored",r.ignored);publishDiagnosticCounter("RTWDiagTXRateRepeatedSequence",r.repeatedSequences);
    publishDiagnosticCounter("RTWDiagTXRateChanges",r.changes);publishDiagnosticCounter("RTWDiagTXRateResets",r.resets);
    publishDiagnosticCounter("RTWDiagTXFinalHardwareRate",(uint64_t)r.finalRate);
    publishDiagnosticCounter("RTWDiagTXDhcpSeen",txDhcpSeen_);
    publishDiagnosticCounter("RTWDiagTXDhcpSent",txDhcpSent_);
    publishDiagnosticCounter("RTWDiagTXDhcpDrops",txDhcpDrops_);
    publishDiagnosticCounter("RTWDiagTXBuilderOtherDrops",txOtherDrops_);
    publishDiagnosticCounter("RTWDiagTXDhcpTxReports",dhcpTxReports_);
    publishDiagnosticCounter("RTWDiagTXDhcpRepOK",dhcpRepOK_);
    publishDiagnosticCounter("RTWDiagTXDhcpRepRetryOver",dhcpRepRetryOver_);
    publishDiagnosticCounter("RTWDiagTXDhcpRepLifetime",dhcpRepLifetime_);
    publishDiagnosticCounter("RTWDiagTXDhcpRepRetries",dhcpRepRetries_);
    publishDiagnosticCounter("RTWDiagTXDhcpRepRate",dhcpRepRate_);
    publishDiagnosticCounter("RTWDiagTXEapolReports",eapolReports_);
    publishDiagnosticCounter("RTWDiagTXEapolPending",eapolReportPending_);
    publishDiagnosticCounter("RTWDiagTXEapolRepOK",eapolRepOK_);
    publishDiagnosticCounter("RTWDiagTXEapolRepRetryOver",eapolRepRetryOver_);
    publishDiagnosticCounter("RTWDiagTXEapolRepLifetime",eapolRepLifetime_);
    publishDiagnosticCounter("RTWDiagTXEapolRepRetries",eapolRepRetries_);
    publishDiagnosticCounter("RTWDiagTXEapolRepRate",eapolRepRate_);
    for(unsigned i=0;i<txProtHexCount_&&i<3;i++){char key[24];snprintf(key,sizeof key,"RTWDiagTXProtHex%u",i);setProperty(key,txProtHex_[i]);}
    publishDiagnosticCounter("RTWDiagTXPipe0Frames",txPipeFrames_[0]);
    publishDiagnosticCounter("RTWDiagTXPipe1Frames",txPipeFrames_[1]);
    publishDiagnosticCounter("RTWDiagTXMgmtFrames",txMgmtFrames_);
    publishDiagnosticCounter("RTWDiagTXDataFrames",txDataFrames_);
    publishDiagnosticCounter("RTWDiagTXEapolSent",txEapolSent_);
    publishDiagnosticCounter("RTWDiagTXEapolLastPipe",lastEapolPipe_);
    publishDiagnosticCounter("RTWDiagTXEapolLastQsel",lastEapolQsel_);
    publishDiagnosticCounter("RTWDiagTXEapolLastRate",lastEapolRate_);
    publishDiagnosticCounter("RTWDiagTXEapolLastLen",lastEapolLen_);
    publishDiagnosticCounter("RTWDiagTXEapolLastQoS",lastEapolQos_);
    publishDiagnosticCounter("RTWDiagTXReportCalls",txReportCalls_);
    publishDiagnosticCounter("RTWDiagDisableIgnored",disableIgnored_);
    publishDiagnosticCounter("RTWDiagTXReportLastLen",txReportLastLen_);
    publishDiagnosticCounter("RTWDiagRXMgmtFrames",rxMgmtFrames_);
    publishDiagnosticCounter("RTWDiagRXDataFrames",rxDataFrames_);
    publishDiagnosticCounter("RTWDiagRXDataToUs",rxDataToUs_);
    publishDiagnosticCounter("RTWDiagRXDataBcast",rxDataBcast_);
    publishDiagnosticCounter("RTWDiagDhcpEpisodeReplies",episodeReplies_);
    publishDiagnosticCounter("RTWDiagDhcpEpisodeDiscovers",episodeDiscovers_);
    publishDiagnosticCounter("RTWDiagDhcpDiscoversTotal",dhcpDiscovers_);
    publishDiagnosticCounter("RTWDiagTXDhcpReportPending",dhcpReportPending_);
    publishDiagnosticCounter("RTWDiagTXDhcpReportMissing",dhcpReportWaits_);
}
int RTWUSBController::sendFrame(mbuf_t packet,ieee80211_node*node) {
    if(!dfsFrameAllowed(packet,node)){if(packet)mbuf_freem(packet);return EACCES;}
    if(is8723Family())return send8723Frame(packet,node);
    if(is8710BUFamily())return send8710BUFrame(packet,node);
    if(is8192FUFamily())return send8192FUFrame(packet,node);
    if(family_==RTW_WIFI_RTL8192DU)return send8192DUFrame(packet,node);
    if(is88Family())return send88Frame(packet,node);
    if(is8812Family())return send8812Frame(packet,node);
    if(!packet)return EINVAL;
    if(!running_||stopping_||!present_||usbError_!=kIOReturnSuccess||!node){mbuf_freem(packet);++txOtherDrops_;return ENETDOWN;}
    if(family_==RTW_WIFI_RTL8814AU&&(!acCore_.phy_initialized||acCore_.io_error)){mbuf_freem(packet);++txOtherDrops_;return ENETDOWN;}
    ieee80211_frame header{};size_t length=mbuf_pkthdr_len(packet);
    if(length<sizeof(header)||length>4096||mbuf_copydata(packet,0,sizeof(header),&header)){mbuf_freem(packet);++txOtherDrops_;return EINVAL;}
    auto&ic=core_.sc_ic;
    const bool data=(header.i_fc[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_DATA;
    const bool multicast=(header.i_addr1[0]&1)!=0;
    const bool qos=data&&(header.i_fc[0]&IEEE80211_FC0_SUBTYPE_QOS);
    /* R25: on to-DS frames the real destination lives in addr3; addr1 is
     * always the AP's unicast BSSID, so `multicast` above never fires in
     * infrastructure mode and every broadcast - configd's DISCOVER, ARP -
     * silently rode the unicast firmware rate-adaptation path. Field
     * captures proved the server never answers it (sent=21/19 drops=0,
     * replies=0 on every build). R23 tried to reroute broadcasts to the
     * host basic-rate path but compared against DIR_DSTODS - the
     * 4-address WDS direction - so the check NEVER fired on
     * infrastructure frames (they are DIR_TODS): the DISCOVERs stayed
     * firmware-rate, and rtw24 (2026-11-10) caught R24's CCX probe dying
     * reason=17 on exactly those frames (USE_RATE is cleared on
     * firmware-rate descriptors, so the report request is invalid there).
     * Fixed: infrastructure STA frames are DIR_TODS. Broadcasts now truly
     * take the host-rate basic-rate path - the same profile as the EAPOL
     * frames the AP demonstrably receives and answers. */
    const bool bcastDA=((header.i_fc[1]&IEEE80211_FC1_DIR_MASK)==IEEE80211_FC1_DIR_TODS)&&
        (header.i_addr3[0]&1)!=0;
    /* 1.0.20: DHCP TX verdict accounting. Field capture 2026-11-10 proved
     * the lease dies inside the TX builder: every configd DISCOVER was
     * dropped with errno=22 and sent=0 while RX and the association stayed
     * healthy. Every failure exit below now names itself with a reason tag
     * so the next capture points at the exact gate. */
    bool dhcpTx=false;unsigned tid=0;
    auto txDrop=[&](unsigned reason,const char*detail){
        if(dhcpTx){
            ++txDhcpDrops_;
            const uint64_t n=nowMS();
            if(!lastTxDhcpLogMs_||n-lastTxDhcpLogMs_>=10000){
                lastTxDhcpLogMs_=n;
                IOLog("RtWlanUCompat: dhcp client frame DROPPED in TX builder, reason=%u %s (seen=%llu sent=%llu drops=%llu) qos=%u tid=%u mcast=%u len=%u\n",
                      reason,detail?detail:"",txDhcpSeen_,txDhcpSent_,txDhcpDrops_,(unsigned)qos,tid,(unsigned)multicast,(unsigned)length);
            }
        }else{
            ++txOtherDrops_;
            const uint64_t n=nowMS();
            if(!lastTxOtherDropLogMs_||n-lastTxOtherDropLogMs_>=30000){
                lastTxOtherDropLogMs_=n;
                IOLog("RtWlanUCompat: TX builder drop reason=%u non-dhcp total=%llu qos=%u tid=%u mcast=%u len=%u\n",
                      reason,(unsigned long long)txOtherDrops_,(unsigned)qos,tid,(unsigned)multicast,(unsigned)length);
            }
        }
    };
    /* 1.0.20: mark DHCP client frames (UDP 68->67) while still cleartext,
     * BEFORE any gate can drop them. LLC/SNAP IPv4 with the minimal
     * 20-byte IP header configd always sends. */
    {
        const size_t hl0=qos?sizeof(ieee80211_frame)+2:sizeof(ieee80211_frame);
        uint8_t x[32]{};
        if(data&&length>=hl0+32&&!mbuf_copydata(packet,hl0,32,x)&&
           x[0]==0xaa&&x[1]==0xaa&&x[2]==3&&x[6]==0x08&&x[7]==0x00&&
           (x[8]>>4)==4&&(x[8]&0x0f)==5&&x[17]==17){
            const unsigned sp=((unsigned)x[28]<<8)|x[29],dp=((unsigned)x[30]<<8)|x[31];
            if(sp==68&&dp==67){dhcpTx=true;++txDhcpSeen_;}
        }
    }
    /* R21: THE root cause of the dead lease is fixed here. The original
     * gate dropped every QoS data frame on this builder, and this builder
     * serves the 8188EU. Field capture rtw21 (2026-11-10) caught configd's
     * DISCOVERs arriving qos=1 and dying 15/15 and 19/19 (sent=0) while RX
     * and the association stayed healthy - the server never heard a single
     * discover, so there was never an offer to receive. Everything below
     * this point is QoS-complete: the 92C descriptor validates qsel<=7 and
     * carries the TID, AMPDU and the 8188 firmware-rate path are keyed on
     * exactly these frames. The author's own 8188 firmware-rate feature
     * requires qos=1 here and could never have executed with this gate in
     * place. The frames are no longer dropped; they are built, encrypted
     * and transmitted. */
    const size_t headerLength=qos?sizeof(header)+2:sizeof(header);
    uint8_t qosBytes[2]{},llc[8]{};
    if((header.i_fc[1]&IEEE80211_FC1_DIR_MASK)==IEEE80211_FC1_DIR_DSTODS ||
       length<headerLength || (qos&&mbuf_copydata(packet,sizeof(header),2,qosBytes))){mbuf_freem(packet);txDrop(6,"dir-len-qosbytes");return EINVAL;}
    tid=qos?(qosBytes[0]&IEEE80211_QOS_TID):0;
    if(tid>7){/* 1.0.20: degrade to best effort instead of dropping. */txDrop(7,"tid-clamped");tid=0;}
    /* Inspect SNAP before CCMP encryption, including protected group rekeys. */
    const bool eapol=data&&length>=headerLength+sizeof(llc)&&
        !mbuf_copydata(packet,headerLength,sizeof(llc),llc)&&
        llc[0]==0xaa&&llc[1]==0xaa&&llc[2]==3&&llc[3]==0&&llc[4]==0&&llc[5]==0&&llc[6]==0x88&&llc[7]==0x8e;
    if(header.i_fc[1]&IEEE80211_FC1_PROTECTED){
        auto*key=ieee80211_get_txkey(&ic,&header,node);
        if(!key||key->k_cipher!=IEEE80211_CIPHER_CCMP){mbuf_freem(packet);txDrop(8,"no-ccmp-txkey");return EINVAL;}
        packet=ieee80211_encrypt(&ic,packet,key);
        if(!packet){txDrop(9,"encrypt-nombuf");return ENOBUFS;}
        length=mbuf_pkthdr_len(packet);
    }
    unsigned index=RTW_TRANSFER_SLOTS;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    for(unsigned i=RXSlots;i<RTW_TRANSFER_SLOTS;i++)if(!(ledger_.pending&(1U<<i))){index=i;break;}
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(index==RTW_TRANSFER_SLOTS||length>4096){mbuf_freem(packet);txDrop(10,index==RTW_TRANSFER_SLOTS?"no-tx-slot":"frame-too-big");return ENOBUFS;}
    const bool firmware=qos&&!multicast&&!bcastDA&&!eapol&&node==ic.ic_bss&&
        (usesFirmwareRate()||(family_==RTW_WIFI_RTL8188EU&&uses8188FirmwareRate()));
    const bool legacyFirmware=data&&!multicast&&!bcastDA&&!eapol&&node==ic.ic_bss&&usesLegacyFirmwareRate();
    const bool gen2=family_==RTW_WIFI_RTL8192EU;
    const bool ac8814=family_==RTW_WIFI_RTL8814AU;
    const bool acFirmware=data&&!multicast&&!bcastDA&&!eapol&&node==ic.ic_bss&&uses8814FirmwareRate()&&(!acRateConfig_.ht||qos);
    const size_t descriptorLength=(gen2||ac8814)?40U:32U;
    unsigned ac=data?(qos?ieee80211_up_to_ac(&ic,tid):EDCA_AC_BE):EDCA_AC_VO;
    unsigned pipeIndex=ac8814&&multicast?0:core_.ac2idx[ac];
    if(pipeIndex>=endpoints_.num_bulk_out){
        /* 1.0.20: fall back to the proven best-effort data pipe instead of
         * dropping the frame. */
        txDrop(11,"pipe-fallback");pipeIndex=core_.ac2idx[EDCA_AC_BE];
        if(pipeIndex>=endpoints_.num_bulk_out){mbuf_freem(packet);txDrop(11,"pipe-invalid");return EINVAL;}
    }
    static const uint8_t values[12]={2,4,11,22,12,18,24,36,48,72,96,108};
    const bool band5g=ac8814&&node->ni_chan&&node->ni_chan!=IEEE80211_CHAN_ANYC&&(node->ni_chan->ic_flags&IEEE80211_CHAN_5GHZ)!=0;
    uint8_t rate=band5g?4:0;uint8_t raid=band5g?7:8;
    if(node->ni_rates.rs_nrates>nitems(node->ni_rates.rs_rates)){mbuf_freem(packet);txDrop(12,"rates-overflow");return EINVAL;}
    if(node->ni_rates.rs_nrates){
        unsigned selected=node->ni_txrate<node->ni_rates.rs_nrates?node->ni_txrate:0;
        if(!data||multicast||bcastDA||eapol){
            selected=0;unsigned lowest=256;bool basic=false;
            for(unsigned r=0;r<node->ni_rates.rs_nrates;r++){
                const unsigned v=node->ni_rates.rs_rates[r]&IEEE80211_RATE_VAL;
                const bool b=(node->ni_rates.rs_rates[r]&IEEE80211_RATE_BASIC)!=0;
                if(band5g&&(v==2||v==4||v==11||v==22))continue;
                if((b&&!basic)||(b==basic&&v<lowest)){selected=r;lowest=v;basic=b;}
            }
        }
        const uint8_t value=node->ni_rates.rs_rates[selected]&IEEE80211_RATE_VAL;
        bool found=false;for(uint8_t hw=0;hw<12;hw++)if(values[hw]==value){rate=hw;found=true;break;}
        if(!found||(band5g&&rate<4)){
            /* 1.0.20: fall back to the 1 Mb/s basic rate instead of
             * dropping; 1 Mb/s exists in every 2.4 GHz basic set. */
            if(band5g){mbuf_freem(packet);txDrop(13,"rate-5ghz-invalid");return EINVAL;}
            txDrop(13,"rate-fallback-1mb");rate=0;
        }
        uint16_t mask=0;for(unsigned r=0;r<node->ni_rates.rs_nrates;r++)for(unsigned hw=0;hw<12;hw++)
            if((node->ni_rates.rs_rates[r]&IEEE80211_RATE_VAL)==values[hw])mask|=(uint16_t)(1U<<hw);
        raid=(mask&0xff0U)?((mask&15U)?6:7):8;
    }
    rtw_92e_tx_options options{};
    options.data=data&&!multicast;options.multicast=multicast;options.raid=firmware?core_.rate_config.raid:raid;
    options.rate=firmware?core_.rate_config.initial_rate:rate;options.firmware_rate=firmware||legacyFirmware;
    /* Gen2 DW5 describes each packet's width and primary subchannel.
     * Legacy/EAPOL/management stay at 20MHz on an HT40 radio as well. */
    if(gen2&&core_.channel_bw40){
        const unsigned sco=ic.ic_bss?ic.ic_bss->ni_htop0&IEEE80211_HTOP0_SCO_MASK:0;
        if(sco!=IEEE80211_HTOP0_SCO_SCA&&sco!=IEEE80211_HTOP0_SCO_SCB){mbuf_freem(packet);txDrop(14,"ht40-sco");return EINVAL;}
        options.channel_bw40=1;
        options.primary_above=(sco==IEEE80211_HTOP0_SCO_SCB);
    }
    if(gen2&&firmware&&qos){
        options.bw40=core_.rate_config.bw40;
        options.short_gi=options.bw40?core_.rate_config.sgi40:core_.rate_config.sgi20;
    }
    if(!gen2&&!(family_==RTW_WIFI_RTL8188EU&&firmware))options.raid=raid==6?4:raid==7?5:6;
    /* Preserve header TID and its sequence, while honoring ACM queue downgrade. */
    static const uint8_t queues[4]={0,2,5,7};
    const unsigned nativeAC=(tid==1||tid==2)?EDCA_AC_BK:(tid==4||tid==5)?EDCA_AC_VI:tid>=6?EDCA_AC_VO:EDCA_AC_BE;
    options.qsel=options.data?(qos&&ac==nativeAC?(uint8_t)tid:queues[ac]):0x12;
    options.sequence=((uint16_t)header.i_seq[0]|(uint16_t)header.i_seq[1]<<8)>>4;
    options.short_preamble=(ic.ic_flags&IEEE80211_F_SHPREAMBLE)&&options.rate>=1&&options.rate<=3&&!firmware;
    if(firmware&&qos&&(qosBytes[0]&IEEE80211_QOS_ACK_POLICY_MASK)==IEEE80211_QOS_ACK_POLICY_NORMAL&&
       node->ni_tx_ba[tid].ba_state==IEEE80211_BA_AGREED){
        options.ampdu=1;options.density=(node->ni_ampdu_param&IEEE80211_AMPDU_PARAM_SS)>>2;
        options.max_agg=(uint8_t)MIN(16,node->ni_tx_ba[tid].ba_winsize);
    }
    size_t total=0;
    if(rtw_tx_total_transfer_size_for_descriptor(descriptorLength,length,txMPS_[pipeIndex],&total)!=RTW_PACKETS_OK||total>TXCapacity){mbuf_freem(packet);txDrop(15,"transfer-size");return EINVAL;}
    auto*t=&transfers_[index];auto*bytes=static_cast<uint8_t*>(t->memory->getBytesNoCopy());
    bzero(bytes,total);
    if(!ac8814&&!(gen2?rtw_92e_make_tx_descriptor_ex(length,&options,bytes):rtw_92c_make_tx_descriptor(length,&options,bytes))){mbuf_freem(packet);txDrop(16,"make-descriptor");return EINVAL;}
    unsigned protection=0;
    if(options.data){
        unsigned htprotection=node->ni_htop1&IEEE80211_HTOP1_PROT_MASK;
        /* BA aggregation alone does not mandate RTS/CTS on each MPDU.
         * Honor the RTS threshold, mixed HT protection and 20MHz-only
         * stations when using HT40, as required by the AP operation IE. */
        if(length+4>(unsigned)ic.ic_rtsthreshold||
           ((firmware||(acFirmware&&acRateConfig_.ht))&&(htprotection==1||htprotection==3||
              (htprotection==2&&core_.rate_config.bw40))))protection=1;
        else if(ic.ic_flags&IEEE80211_F_USEPROT)protection=ic.ic_protmode==IEEE80211_PROT_CTSONLY?2:ic.ic_protmode==IEEE80211_PROT_RTSCTS?1:0;
    }
    /* 1.0.24: ask the 8188EU firmware for a verdict on unicast EAPOL (handshake M2): unlike broadcast DHCP it needs an ACK, so retry_over proves whether the AP hears us. */
    const bool eapolProbe=eapol&&family_==RTW_WIFI_RTL8188EU&&!firmware&&options.data&&!bcastDA;
    bool requestReport=options.data&&!bcastDA&&(!eapol||eapolProbe)&&!legacyFirmware&&
        /* 88E firmware-rate packets clear USE_RATE; the 88E CCX report
         * request is only valid on host-selected rates. */
        !(family_==RTW_WIFI_RTL8188EU&&firmware)&&authorized()&&ic.ic_state==IEEE80211_S_RUN&&
        node==ic.ic_bss&&rateControl_.active&&rateTransportFresh_&&
        IEEE80211_ADDR_EQ(rateControl_.bssid,node->ni_bssid)&&
        (!firmware||((++rateControl_.htSampleCounter&31U)==0));
    /* R24: DHCP frames carry an 88E CCX report request when they are
     * host-rate, so the firmware's own completion report witnesses the
     * over-the-air attempt. R25 guard: NEVER on a firmware-rate frame -
     * rtw24 (2026-11-10) proved that combination dies reason=17 because
     * firmware-rate descriptors clear USE_RATE by design. With the R25
     * TODS fix the DISCOVER is genuinely host-rate and the request is
     * valid; this guard keeps the probe from ever regressing the path. */
    const bool dhcpReport=dhcpTx&&family_==RTW_WIFI_RTL8188EU&&!firmware;
    if(dhcpReport)requestReport=true;
    bool descriptorOK=false;
    if(ac8814){
        rtw_8814au_tx_options acOptions{};
        acOptions.data=data;acOptions.multicast=multicast;acOptions.eapol=eapol;
        acOptions.qsel=data?options.qsel:0x12;acOptions.rate=acFirmware?acRateConfig_.initial_rate:rate;
        acOptions.firmware_rate=acFirmware;acOptions.sequence=options.sequence;
        acOptions.short_gi=acFirmware&&acRateConfig_.ht&&acRateConfig_.sgi20;
        acOptions.short_preamble=(ic.ic_flags&IEEE80211_F_SHPREAMBLE)&&acOptions.rate>=1&&acOptions.rate<=3;
        /* Use RTS/CTS when protection is requested; this codec has no
         * verified standalone CTS-to-self encoding. */
        acOptions.protection=protection?1:0;
        acOptions.qos=qos;
        acOptions.ack_expected=qos&&(qosBytes[0]&IEEE80211_QOS_ACK_POLICY_MASK)==IEEE80211_QOS_ACK_POLICY_NORMAL&&
            !(ic.ic_tid_noack&(1U<<tid));
        acOptions.fragment=(header.i_fc[1]&IEEE80211_FC1_MORE_FRAG)||(header.i_seq[0]&0x0fU);
        const auto&ba=node->ni_tx_ba[tid];
        if(acFirmware&&acRateConfig_.ht&&authorized()&&qos&&!(header.i_fc[0]&IEEE80211_FC0_SUBTYPE_NODATA)&&
           acOptions.ack_expected&&!acOptions.fragment&&
           (node->ni_flags&IEEE80211_NODE_HT)&&acOptions.qsel==tid&&
           ba.ba_state==IEEE80211_BA_AGREED&&ba.ba_winsize>=2&&ba.ba_winsize<=IEEE80211_BA_MAX_WINSZ){
            acOptions.ampdu=1;acOptions.density=(node->ni_ampdu_param&IEEE80211_AMPDU_PARAM_SS)>>2;
            /* Linux get_tx_ampdu_factor: DW3 counts pairs of MPDUs.
             * Bound both the peer HT factor and the live receive window. */
            const unsigned peerPairs=(4U<<(node->ni_ampdu_param&IEEE80211_AMPDU_PARAM_LE))-1U;
            acOptions.max_agg=(uint8_t)MIN(16U,MIN(peerPairs,(unsigned)ba.ba_winsize/2U));
        }
        options.ampdu=acOptions.ampdu;
        descriptorOK=rtw_8814au_make_tx_descriptor(length,&acOptions,acFirmware?&acRateConfig_:nullptr,bytes);
        requestReport=false;
        /* Thermal tracking retains the effective rate from accepted C2H. */
    }else if(gen2){
        /* R26: option encoding is best-effort. The rate-report request is
         * a diagnostic aid - an unencodable request (rtw19/20/24 killed
         * DHCP frames exactly here, reason=17) must degrade to a frame
         * without the request, never to a dropped frame. The pass-through
         * call (requested=0) restamps the descriptor checksum. */
        if(!rtw_92e_tx_protection(protection,bytes))txDrop(18,"protection-encode-skipped");
        if(requestReport&&!rtw_92e_tx_report_request(1,bytes)){
            txDrop(19,"report-request-skipped");
            rtw_92e_tx_report_request(0,bytes);
        }
        descriptorOK=true;
    }else{
        if(!rtw_92c_tx_protection(protection,bytes))txDrop(18,"protection-encode-skipped");
        if(requestReport&&!rtw_92c_tx_report_request(family_==RTW_WIFI_RTL8188EU,1,bytes)){
            txDrop(19,"report-request-skipped");
            rtw_92c_tx_report_request(family_==RTW_WIFI_RTL8188EU,0,bytes);
        }else if(!requestReport&&family_==RTW_WIFI_RTL8188EU){
            /* 1.0.27: on the 88E, DW1 bit6 is not AGG_BREAK (that is DW2 bit16). Every non-report data frame left it set, so the chip saw an invalid MACID 64. Pass-through call moves the bit. */
            rtw_92c_tx_report_request(1,0,bytes);
        }
        descriptorOK=true;
    }
    if(!descriptorOK||mbuf_copydata(packet,0,length,bytes+descriptorLength)){mbuf_freem(packet);txDrop(17,descriptorOK?"copydata-failed":"descriptor-build");return EINVAL;}
    mbuf_freem(packet);
    if(family_==RTW_WIFI_RTL8188EU&&(header.i_fc[1]&IEEE80211_FC1_PROTECTED)&&!eapol&&txProtHexCount_<3){
        char*h=txProtHex_[txProtHexCount_];size_t o=0;
        const size_t dl=descriptorLength<32?descriptorLength:32,fl=length<48?length:48;
        for(size_t i=0;i<dl&&o+3<sizeof(txProtHex_[0]);i++)o+=snprintf(h+o,sizeof(txProtHex_[0])-o,"%02x",bytes[i]);
        if(o+2<sizeof(txProtHex_[0])){h[o++]='|';h[o]=0;}
        for(size_t i=0;i<fl&&o+3<sizeof(txProtHex_[0]);i++)o+=snprintf(h+o,sizeof(txProtHex_[0])-o,"%02x",bytes[descriptorLength+i]);
        ++txProtHexCount_;
    }
    int error=submit(index,txPipes_[pipeIndex],(uint32_t)total);
    if(!error){
        /* R24: the DHCP probe report is consumed by the probe logger only;
         * it must never count as an AMRR sample (broadcast has no ACK, so
         * txok=0 would falsely degrade the station's rate). */
        if(eapolProbe&&requestReport)++eapolReportPending_;
        ++txPipeFrames_[pipeIndex&3U];if(data)++txDataFrames_;else ++txMgmtFrames_;
        if(eapol){++txEapolSent_;lastEapolPipe_=pipeIndex;lastEapolQsel_=options.qsel;lastEapolRate_=options.rate;lastEapolLen_=length;lastEapolQos_=qos?1:0;}
        if(requestReport&&!dhcpTx&&!eapolProbe){++rateControl_.requests;++rateControl_.pendingReports;rateControl_.requestedSinceReset=true;}
        if(firmware||(acFirmware&&acRateConfig_.ht))++txHTPackets_;
        if(options.ampdu){++txAMPDUPackets_;auto&ba=node->ni_tx_ba[tid];if(ba.ba_timeout_val)timeout_add_usec(&ba.ba_to,ba.ba_timeout_val);}
        ieee80211_release_node(&ic,node);
        if(dhcpTx){
            ++txDhcpSent_;
            if(dhcpReport)++dhcpReportPending_;
            const uint64_t n=nowMS();
            if(!lastTxDhcpLogMs_||n-lastTxDhcpLogMs_>=10000){
                lastTxDhcpLogMs_=n;
                IOLog("RtWlanUCompat: dhcp client frame SENT to USB (seen=%llu sent=%llu drops=%llu)\n",
                      txDhcpSeen_,txDhcpSent_,txDhcpDrops_);
            }
        }
    }else if(dhcpTx)txDrop(20,"usb-submit");
    return error;
}
void RTWUSBController::receive(const uint8_t*bytes,size_t length) {
    if(!attached_||!running_||!netif_||!stats_||!core_.sc_ic.ic_if.iface)return;
    ++rxDiag_.aggregates;rxDiag_.bytes+=length;
    rxDiag_.lastReadBytes=length;
    if(length>rxDiag_.maxReadBytes)rxDiag_.maxReadBytes=length;
    /* Decode everything structurally before any delivery. If a later RX
     * descriptor is corrupt, salvage ONLY an already validated prefix;
     * no malformed bytes ever reach net80211. All chipset codecs share this
     * preflight, including 8814AU's FCS length and C2H validation. */
    auto decode=[](rtw_rx_iterator*it,void*context)->rtw_packet_error{
        auto*self=static_cast<RTWUSBController*>(context);
        rtw_8192eu_rx_frame checked{};uint8_t type=0;
        return self->decodeUSBFrame(it,&checked,&type);
    };
    rtw_rx_preflight_result prefix{};
    rtw_packet_error result=rtw_rx_preflight_transfer(bytes,length,decode,this,&prefix);
    if(result!=RTW_PACKETS_DONE){
        ++rxDiag_.aggregateErrors;
        rxDiag_.lastAggregateErrorReadBytes=length;
        switch(result){
        case RTW_PACKETS_TRUNCATED: ++rxDiag_.aggregateTruncated;break;
        case RTW_PACKETS_MALFORMED: ++rxDiag_.aggregateMalformed;break;
        case RTW_PACKETS_TOO_MANY: ++rxDiag_.aggregateTooMany;break;
        default: ++rxDiag_.aggregateBadArgument;break;
        }
        rxDiag_.lastAggregateErrorOffset=prefix.error_offset;
        rxDiag_.lastAggregateErrorFrames=prefix.error_frames;
        rxErrors_++;stats_->inputErrors++;
        if(!prefix.valid_frames)return; /* invalid first frame: reject all */
        ++rxDiag_.partialAggregates;
        rxDiag_.partialPrefixFrames+=prefix.valid_frames;
        rxDiag_.partialPrefixBytes+=prefix.valid_bytes;
    }
    /* A second parse is deliberately bounded to the validated prefix.
     * Never decode (or deliver) any part of the invalid USB suffix. */
    rtw_rx_iterator iterator;rtw_rx_iterator_init(&iterator,bytes,prefix.valid_bytes);
    rtw_8192eu_rx_frame frame{};uint8_t reportType=0;
    mbuf_list list=MBUF_LIST_INITIALIZER();
    while(decodeUSBFrame(&iterator,&frame,&reportType)==RTW_PACKETS_OK){
        const auto&p=frame.payload;
        if(reportType){++rxDiag_.c2h;if(reportType==7)handle8723C2H(p.mpdu,p.mpdu_length);else if(reportType==6)handle88C2H(p.mpdu,p.mpdu_length);else if(reportType==5){if(is8192FUFamily())handle8192FUC2H(p.mpdu,p.mpdu_length);else if(is8710BUFamily())handle8710BUC2H(p.mpdu,p.mpdu_length);else handle8812C2H(p.mpdu,p.mpdu_length);}else if(reportType==4)handle8814C2H(p.mpdu,p.mpdu_length);else if(reportType==1)handleTxReport(p.mpdu,p.mpdu_length);continue;}
        uint64_t*drop=nullptr;
        if((is8723Family()&&(p.rate_index>=20||(p.rate_index>=12&&(!uses8723FirmwareRate()||!(r23Rates_.mask&0xff000U)||p.bandwidth_code))))||(is8710BUFamily()&&!accepts8710BUReceiveRate(p))||(is8192FUFamily()&&!accepts8192FUReceiveRate(p))||(family_==RTW_WIFI_RTL8192DU&&p.rate_index>=12U+8U*core_.nrxchains)||(is8812Family()&&!accepts8812ReceiveRate(p))||(is88Family()&&p.rate_index>=12U+8U*core_.nrxchains)||(family_==RTW_WIFI_RTL8188EU&&p.rate_index>=12U+8U*core_.nrxchains)||(family_==RTW_WIFI_RTL8814AU&&p.rate_index>27)||
           (family_!=RTW_WIFI_RTL8192EU&&family_!=RTW_WIFI_RTL8188EU&&family_!=RTW_WIFI_RTL8814AU&&!is88Family()&&!is8812Family()&&family_!=RTW_WIFI_RTL8192DU&&family_!=RTW_WIFI_RTL8192FU&&family_!=RTW_WIFI_RTL8710BU&&family_!=RTW_WIFI_RTL8723BU&&p.rate_index>=12))drop=&rxDiag_.invalidFrameDrops;
        else if(p.crc_error)drop=&rxDiag_.crcDrops;
        else if(p.icv_error)drop=&rxDiag_.icvDrops;
        else if(!rtw_92e_rx_frame_allowed(p.mpdu,p.mpdu_length,authorized()&&(usesFirmwareRate()||(uses8814FirmwareRate()&&acRateConfig_.ht)||(uses88FirmwareRate()&&r88Rates_.rates.ht)||(uses8188FirmwareRate()&&core_.rate_config.ht)||uses8812HighThroughput()||uses8192FUHighThroughput()||uses8710BUHighThroughput()||(uses8723FirmwareRate()&&(r23Rates_.mask&0xff000U))||(uses8192DUFirmwareRate()&&duMAC_.rates.ht)),
                core_.sc_ic.ic_myaddr,core_.sc_ic.ic_bss?core_.sc_ic.ic_bss->ni_bssid:nullptr))drop=&rxDiag_.invalidFrameDrops;
        else if(frame.security_code&&!frame.sw_decrypt_required)drop=&rxDiag_.hardwareDecryptDrops;
        if(drop){++*drop;rxErrors_++;stats_->inputErrors++;continue;}
        if((p.mpdu[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_CTL)++rxBARPackets_;
        observeReceivedRate(p);
        if(p.mpdu[1]&IEEE80211_FC1_PROTECTED)++rxDiag_.protectedMPDUs;
        if((p.mpdu[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_MGT)++rxMgmtFrames_;
        else if((p.mpdu[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_DATA&&p.mpdu_length>=24){
            ++rxDataFrames_;
            if(!memcmp(p.mpdu+4,core_.sc_ic.ic_myaddr,6))++rxDataToUs_;
            else if(p.mpdu[4]==0xff&&p.mpdu[5]==0xff&&p.mpdu[6]==0xff&&p.mpdu[7]==0xff&&p.mpdu[8]==0xff&&p.mpdu[9]==0xff)++rxDataBcast_;
        }
        /* Beacons establish RF freshness throughout WPA2 key exchange, before
         * net80211 opens the controlled port for Ethernet traffic. */
        if(present_&&joinRequested_&&core_.sc_ic.ic_state==IEEE80211_S_RUN&&
           associatedBeacon(p.mpdu,p.mpdu_length))lastBeaconMs_=nowMS();
        ieee80211_rxinfo info{};
        int measuredRSSI=0;
        unsigned scanPhysicalChannel=0;
        if(family_==RTW_WIFI_RTL8814AU&&p.phy_status_present){
            rtw_8814au_rx_frame acFrame{};acFrame.packet=p;rtw_8814au_phy_status phy{};
            if(rtw_8814au_parse_phy(&acFrame,acCore_.board.antenna_rx,&phy)){
                measuredRSSI=phy.signal_dbm;
                scanPhysicalChannel=phy.channel;
                if(rtw_signal_valid_dbm(measuredRSSI)){
                    acRSSI_=phy.rssi;
                    info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                    core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
                }
            }
        }else if(is8723Family()){
            measuredRSSI=sample8723RSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(is8710BUFamily()){
            measuredRSSI=sample8710BURSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(is8192FUFamily()){
            measuredRSSI=sample8192FURSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(family_==RTW_WIFI_RTL8192DU){
            measuredRSSI=sample8192DURSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(is88Family()){
            measuredRSSI=sample88RSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(is8812Family()){
            measuredRSSI=sample8812RSSI(p);
            if(rtw_signal_valid_dbm(measuredRSSI)){
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
                core_.avg_pwdb=core_.avg_pwdb<0?info.rxi_rssi:(core_.avg_pwdb*19+info.rxi_rssi)/20;
            }
        }else if(p.phy_status_present&&p.phy_info_length>=sizeof(r92c_rx_phystat)){
            measuredRSSI=rtwn_get_rssi(&core_,p.rate_index,const_cast<uint8_t*>(p.phy_info));
            if(rtw_signal_valid_dbm(measuredRSSI)){
                rtwn_update_avgrssi(&core_,p.rate_index,(int8_t)measuredRSSI);
                info.rxi_rssi=rtw_signal_node_value(measuredRSSI);
            }
        }
        info.rxi_tstamp=p.timestamp;
        auto*channel=core_.sc_ic.ic_bss?core_.sc_ic.ic_bss->ni_chan:nullptr;
        if(channel&&channel!=IEEE80211_CHAN_ANYC)info.rxi_chan=(uint8_t)ieee80211_chan2ieee(&core_.sc_ic,channel);
        /* Only decoded AU PHY status supplies physical-channel evidence. A
         * missing/zero PHY channel stays unknown; never copy the software
         * scan channel into it. Mismatches do not affect packet delivery. */
        observeDFSFrame(p.mpdu,p.mpdu_length,scanPhysicalChannel);
        rtw::scanDiagnosticsFrame(scanDiag_,core_.sc_ic.ic_state==IEEE80211_S_SCAN,
            p.mpdu,p.mpdu_length,info.rxi_chan,scanPhysicalChannel);
        /* DFS quiet/power admission above must run even under mbuf pressure. */
        mbuf_t packet=nullptr;unsigned chunks=1;
        if(mbuf_allocpacket(MBUF_DONTWAIT,p.mpdu_length,&chunks,&packet)||!packet){++rxDiag_.allocationDrops;rxErrors_++;stats_->inputErrors++;continue;}
        if(mbuf_copyback(packet,0,p.mpdu_length,p.mpdu,MBUF_DONTWAIT)){++rxDiag_.copyDrops;mbuf_freem(packet);rxErrors_++;continue;}
        auto*header=static_cast<ieee80211_frame*>(mbuf_data(packet));
        auto*node=ieee80211_find_rxnode(&core_.sc_ic,header);
        if(!node){++rxDiag_.noNodeDrops;mbuf_freem(packet);rxErrors_++;continue;}
        /* Missing PHY is not a zero-strength measurement. Preserve only this
         * transmitter's node value; never borrow RSSI from a different AP. */
        if(!info.rxi_rssi&&rtw_signal_from_ap(p.mpdu,p.mpdu_length,node->ni_bssid))
            info.rxi_rssi=node->ni_rssi;
        if(core_.sc_ic.ic_state==IEEE80211_S_RUN&&core_.sc_ic.ic_bss)
            rtw_signal_observe(&signal_,core_.sc_ic.ic_bss->ni_bssid,info.rxi_chan,
                p.mpdu,p.mpdu_length,p.phy_status_present,measuredRSSI,nowMS());
        ieee80211_inputm(&core_.sc_ic.ic_if,packet,node,&info,&list);
        ieee80211_release_node(&core_.sc_ic,node);rxPackets_++;
    }
    /* 1.0.17: sniff the decapped ethernet queue for DHCP server replies
     * before the stack consumes it - the one bit the lease watchdog needs
     * (beacons prove management, replies prove the IPv4 data path). */
    for(mbuf_t m=list.ml_head;m;m=mbuf_next(m)){
        noteRxDhcpReply(m);
        /* 1.0.18: broadcast probe. A first-time DHCP OFFER arrives as a
         * broadcast frame; if NO broadcast data frame ever reaches this
         * queue while the AP is strong, the loss is in our RX filter or
         * power save - not the server. */
        if(mbuf_len(m)>=1&&(*(const uint8_t*)mbuf_data(m)&0x01))++rxBcast_;
        /* R23: unicast probe. The DHCP OFFER can also arrive unicast; if
         * bcast frames arrive but unicast never does, our RX drops the
         * one direction the lease needs. */
        else ++rxUcast_;
    }
    rxDiag_.ethernetQueued+=list.ml_len;
    if(if_input(&core_.sc_ic.ic_if,&list)!=kIOReturnSuccess)++rxDiag_.handoffErrors;
}
void RTWUSBController::scanAction(OSObject*o,IOTimerEventSource*) {auto*s=static_cast<RTWUSBController*>(o);if(s->running_&&!s->stopping_){if(s->rxRecovery_.pending||s->dfsTXBlocked()){s->scanTimer_->setTimeoutMS(100);return;}rtwn_next_scan(&s->core_);if(rtwn_io_error(&s->core_))s->quiesce(false);}}
void RTWUSBController::calibAction(OSObject*o,IOTimerEventSource*) {
    auto*s=static_cast<RTWUSBController*>(o);
    if(s->dfsTXBlocked())return;
    if(!s->running_||s->stopping_)return;
    if(s->rxRecovery_.pending){s->calibTimer_->setTimeoutMS(100);return;}
    if(s->is8723Family()){
        int e=s->tick8723();
        if(e){s->core_.core_error=e;s->quiesce(false);}
        else if(s->core_.sc_ic.ic_state==IEEE80211_S_RUN)s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->is8710BUFamily()){
        int e=s->tick8710BU();
        if(e){s->core_.core_error=e;s->quiesce(false);}
        else if(s->core_.sc_ic.ic_state==IEEE80211_S_RUN)s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->is8192FUFamily()){
        int e=s->tick8192FU();
        if(e){s->core_.core_error=e;s->quiesce(false);}
        else if(s->core_.sc_ic.ic_state==IEEE80211_S_RUN)s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->family_==RTW_WIFI_RTL8192DU){
        int e=s->tick8192DU();
        if(e){s->core_.core_error=e;s->quiesce(false);}
        else if(s->core_.sc_ic.ic_state==IEEE80211_S_RUN)s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->is88Family()){
        int e=s->tick88();
        if(e){s->core_.core_error=e;s->quiesce(false);}else s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->is8812Family()){
        int e=s->tick8812();
        if(e){s->core_.core_error=e;s->quiesce(false);}else s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    if(s->family_==RTW_WIFI_RTL8814AU){
        int e=rtw_8814au_phy_tick(&s->acCore_);uint8_t command[8]{};
        if(!e&&s->uses8814FirmwareRate()&&rtw_8814au_make_rssi_monitor(s->acRSSI_,command))e=rtw_8814au_h2c_mailbox(&s->acCore_,command);
        if(e){s->core_.core_error=e;s->quiesce(false);}else s->calibTimer_->setTimeoutMS(2000);
        return;
    }
    auto&d=s->calibDiag_;
    d.calls++;
    d.lastStartMS=nowMS();
    d.lastH2CPolls=d.lastH2CReadMS=0;
    d.thermalBefore=(uint32_t)s->core_.thcal_lctemp;
    d.thermalState=(uint32_t)s->core_.thcal_state;
    d.active=true;
    rtwn_calib(&s->core_);
    s->chooseRateControl();
    d.active=false;
    d.lastMS=nowMS()-d.lastStartMS;
    if(d.lastMS>d.maxMS)d.maxMS=d.lastMS;
    if(d.lastH2CPolls>d.maxH2CPolls)d.maxH2CPolls=d.lastH2CPolls;
    if(d.lastH2CReadMS>d.maxH2CReadMS)d.maxH2CReadMS=d.lastH2CReadMS;
    d.thermalAfter=(uint32_t)s->core_.thcal_lctemp;
    if(d.thermalBefore!=d.thermalAfter){
        d.thermalChanges++;
        /* A zero baseline is only initialized. A changed nonzero baseline
         * is recorded after rtwn_temp_calib runs both IQ and LC. */
        if(d.thermalBefore)d.thermalIQs++;
    }
    d.generation++;
    if(rtwn_io_error(&s->core_))s->quiesce(false);
}
uint64_t RTWUSBController::nowMS() {
    uint64_t absolute=0,nanoseconds=0;clock_get_uptime(&absolute);
    absolutetime_to_nanoseconds(absolute,&nanoseconds);return nanoseconds/1000000;
}
void RTWUSBController::publishCalibrationDiagnostics() {
    auto publish=[this](const char*name,uint64_t value){publishDiagnosticCounter(name,value);};
    const auto&d=calibDiag_;
    publish("RTWDiagCalibCalls",d.calls);
    publish("RTWDiagCalibGeneration",d.generation);
    publish("RTWDiagCalibLastStartMS",d.lastStartMS);
    publish("RTWDiagCalibLastMS",d.lastMS);
    publish("RTWDiagCalibMaxMS",d.maxMS);
    publish("RTWDiagCalibThermalChanges",d.thermalChanges);
    publish("RTWDiagCalibThermalIQs",d.thermalIQs);
    publish("RTWDiagCalibLastH2CPolls",d.lastH2CPolls);
    publish("RTWDiagCalibMaxH2CPolls",d.maxH2CPolls);
    publish("RTWDiagCalibTotalH2CPolls",d.totalH2CPolls);
    publish("RTWDiagCalibLastH2CReadMS",d.lastH2CReadMS);
    publish("RTWDiagCalibMaxH2CReadMS",d.maxH2CReadMS);
    publish("RTWDiagCalibThermalBefore",d.thermalBefore);
    publish("RTWDiagCalibThermalAfter",d.thermalAfter);
    publish("RTWDiagCalibThermalState",d.thermalState);
}
#include "RTWDFSRegulatory.inc"
#include "RTWDFSStation.inc"
bool RTWUSBController::associatedBeacon(const uint8_t *bytes,size_t length) const {
    if(length<36||!core_.sc_ic.ic_bss)return false;
    const auto*header=reinterpret_cast<const ieee80211_frame*>(bytes);
    if(header->i_fc[0]!=(IEEE80211_FC0_TYPE_MGT|IEEE80211_FC0_SUBTYPE_BEACON)||
       (header->i_fc[1]&(IEEE80211_FC1_DIR_MASK|IEEE80211_FC1_PROTECTED))||
       memcmp(header->i_addr2,core_.sc_ic.ic_bss->ni_bssid,6)||
       memcmp(header->i_addr3,core_.sc_ic.ic_bss->ni_bssid,6))return false;
    for(unsigned i=0;i<6;i++)if(header->i_addr1[i]!=0xff)return false;
    unsigned interval=(unsigned)bytes[32]|((unsigned)bytes[33]<<8);
    if(!interval||!(bytes[34]&IEEE80211_CAPINFO_ESS))return false;
    size_t offset=36;bool ssid=false;
    while(offset<length){
        if(length-offset<2)return false;
        unsigned id=bytes[offset],size=bytes[offset+1];offset+=2;
        if(size>length-offset)return false;
        if(id==IEEE80211_ELEMID_SSID){
            if(size>32||(size&&(size!=(unsigned)core_.sc_ic.ic_des_esslen||memcmp(bytes+offset,core_.sc_ic.ic_des_essid,size))))return false;
            ssid=true;
        }
        offset+=size;
    }
    return ssid;
}
void RTWUSBController::watchdogAction(OSObject*o,IOTimerEventSource*) {
    auto*s=static_cast<RTWUSBController*>(o);
    if(!s->running_||s->stopping_)return;
    s->advanceDFSRecovery();
    if(!s->running_||s->stopping_||s->dfsTXBlocked())return;
    s->advanceRXRecovery();
    if(!s->running_||s->stopping_)return;
    if(s->rxRecovery_.pending){s->watchdogTimer_->setTimeoutMS(100);return;}
    s->advanceWpaRecovery();
    auto&ic=s->core_.sc_ic;
    if(s->authorized()&&rtw_92e_beacon_expired(nowMS(),s->lastBeaconMs_,ic.ic_bss->ni_intval)){
        ic.ic_bss->ni_port_valid=0;
        if(auto*queue=s->getOutputQueue()){queue->stop();queue->flush();}
        ieee80211_set_link_state(&ic,LINK_STATE_DOWN);
        (void)stateTransition(&ic,IEEE80211_S_SCAN,-1);
    }
    /* 1.0.15: the watchdog owns the self-heal - no user click required. If
     * configd keeps asking for a lease while the station sits in RUN, the
     * data path died under the association; a fresh station init is the
     * only proven cure (it is exactly what a reboot does). */
    s->autoHealStuckDhcp();
    s->autoHealSilentLease();
    /* R24: name the silence. A DHCP probe that requested an 88E CCX
     * report but produced none within 10 s proves the firmware never
     * completed the frame - it died before the radio, it was not the
     * server ignoring us. The pending count is cleared so each new
     * DISCOVER burst starts its own complaint window. */
    if(s->dhcpReportPending_&&nowMS()-s->lastDhcpReportCheckMs_>=10000){
        s->lastDhcpReportCheckMs_=nowMS();
        ++s->dhcpReportWaits_;
        IOLog("RtWlanUCompat: dhcp TX report MISSING: %u probe(s) outstanding over 10 s - firmware never completed the frame\n",s->dhcpReportPending_);
        s->dhcpReportPending_=0;
    }
    /* 1.0.18: 30 s RX tally, only while the lease is missing. bcast>0
     * with replies==0 means the server is silent; 0 means our RX path
     * drops broadcast frames (filter or power save). R23: unicast==0
     * while bcast>0 means unicast data RX is dead - the OFFER direction. */
    if(s->authorized()&&!s->episodeReplies_&&nowMS()-s->lastBcastLogMs_>=30000){
        s->lastBcastLogMs_=nowMS();
        IOLog("RtWlanUCompat: RX tally: %u bcast, %u unicast in 30 s, dhcp "
              "replies=%u\n",s->rxBcast_,s->rxUcast_,s->episodeReplies_);
        s->rxBcast_=0;s->rxUcast_=0;
    }
    s->refreshLinkEvent();
    ic.ic_if.if_watchdog(&ic.ic_if);
    if(rtwn_io_error(&s->core_))s->quiesce(false);else s->watchdogTimer_->setTimeoutMS(1000);
}
void RTWUSBController::resetWpaRecovery() { wpaRecovery_=WPARecovery{}; }
void RTWUSBController::cancelWpaRecovery() {
    auto&r=wpaRecovery_;
    r.active=false;r.remainingMS=0;r.lastMS=0;
    if(r.state!=WPARecoveryFailed)r.state=WPARecoveryIdle;
}
void RTWUSBController::wpaStateChanged(ieee80211_state previous) {
    auto&r=wpaRecovery_;const auto&ic=core_.sc_ic;
    /* 1.0.7: a spent budget never re-arms itself - a fresh join request
     * (RTW_CONTROL_JOIN) clears wpaStandDown_ and buys a new one. */
    if(wpaStandDown_)return;
    if(!joinRequested_||!ic.ic_des_esslen||
       (ic.ic_flags&(IEEE80211_F_RSNON|IEEE80211_F_PSK))!=(IEEE80211_F_RSNON|IEEE80211_F_PSK)){
        cancelWpaRecovery();return;
    }
    if(ic.ic_state!=IEEE80211_S_RUN||previous==IEEE80211_S_RUN||authorized()||r.state==WPARecoveryFailed)return;
    /* The imported association path suggests a 5 s RSN management timer but
     * leaves it disabled. Allow three times that interval for a complete
     * WPA2 exchange, independently of beacons and status polling. */
    if(!r.active){r.retries=0;r.error=0;}
    else if(r.state!=WPARecoverySearching)return;
    r.active=true;r.state=WPARecoveryHandshake;r.remainingMS=family_==RTW_WIFI_RTL8188EU?30000:15000;r.lastMS=nowMS(); /* 1.0.22: 8188EU field logs show associations dropped by this 15 s watchdog before the 4-way handshake finished */
}
void RTWUSBController::advanceWpaRecovery() {
    auto&r=wpaRecovery_;auto&ic=core_.sc_ic;
    if(!attached_||!running_||!radioReady_||!enabled_||stopping_||!present_||sleeping_||
       usbError_!=kIOReturnSuccess||core_.core_error||!joinRequested_||!ic.ic_des_esslen||
       (ic.ic_flags&(IEEE80211_F_RSNON|IEEE80211_F_PSK))!=(IEEE80211_F_RSNON|IEEE80211_F_PSK)){
        cancelWpaRecovery();return;
    }
    if(authorized()){
        cancelWpaRecovery();r.state=WPARecoveryAuthorized;return;
    }
    if(!r.active)return;
    const uint64_t now=nowMS(),elapsed=now-r.lastMS;r.lastMS=now;
    /* Uptime is monotonic. Modular subtraction tolerates wrap; a backwards
     * sample rebases the clock without expiring or replenishing the budget. */
    if(elapsed>(~uint64_t(0)>>1))return;
    if(elapsed<r.remainingMS){r.remainingMS-=elapsed;return;}
    r.timeouts++;r.lastSupplicantState=ic.ic_bss?ic.ic_bss->ni_rsn_supp_state:0;
    if(ic.ic_bss)ic.ic_bss->ni_port_valid=0;
    if(auto*queue=getOutputQueue()){queue->stop();queue->flush();}
    ifq_flush(&ic.ic_if.if_snd);
    ieee80211_set_link_state(&ic,LINK_STATE_DOWN);
    /* Only the serialized watchdog runs this teardown. INIT clears host
     * keys and management queues; it neither stops USB nor reloads firmware. */
    core_.sc_tx_timer=0;
    (void)stateTransition(&ic,IEEE80211_S_INIT,-1);
    if(r.retries==3){
        /* 1.0.7 field fix (RTLWiFiBar.log 2026-11-18: both joins accepted,
         * no link, silence until the app quit): 1.0.6 latched
         * WPARecoveryFailed here, cancelled the scan and calibration
         * timers and left the state machine frozen - the state gate then
         * rejected EVERY later auth/assoc/run until the kext reloaded.
         * A spent handshake budget must never brick the station. Stand
         * down instead: release the WPA state, keep the timers running,
         * return the station to its normal scan cycle, and wait for the
         * user's next join request (which clears wpaStandDown_ and buys
         * a fresh four-window budget). No teardown loop either: while
         * standing down a RUN never re-arms the recovery (see
         * wpaStateChanged). */
        IOLog("RtWlanUCompat: WPA handshake failed %u times on %s - standing "
              "down; scans continue, pick the network again to retry\n",
              (unsigned)r.retries+1,
              ic.ic_des_esslen?(const char*)ic.ic_des_essid:"?");
        wpaStandDown_=true;
        r.active=false;r.state=WPARecoveryIdle;r.error=ETIMEDOUT;r.remainingMS=0;
        (void)stateTransition(&ic,IEEE80211_S_SCAN,-1);
        return;
    }
    r.retries++;r.state=WPARecoverySearching;r.remainingMS=15000;r.lastMS=now;
    /* Scan/auth/assoc also has a finite budget if the AP disappears. A new
     * RUN gets one handshake window; repeated RUN notifications do not. */
    (void)stateTransition(&ic,IEEE80211_S_SCAN,-1);
}
void RTWUSBController::publishWpaRecoveryDiagnostics() {
    auto publish=[this](const char*name,uint64_t value){publishDiagnosticCounter(name,value);};
    const auto&r=wpaRecovery_;
    publish("RTWDiagWPARecoveryState",r.state);
    publish("RTWDiagWPARecoveryActive",r.active?1:0);
    publish("RTWDiagWPARecoveryRetries",r.retries);
    publish("RTWDiagWPARecoveryTimeouts",r.timeouts);
    publish("RTWDiagWPARecoveryError",r.error);
    publish("RTWDiagWPARecoveryRemainingMS",r.remainingMS);
    publish("RTWDiagWPARecoveryLastSupplicantState",r.lastSupplicantState);
}
/* 1.0.14: macOS configd only (re)runs DHCP on a carrier change. The
 * watchdog beacon-expiry path posts carrier DOWN, and after an RF-off
 * window the station recovers on its own (scan -> auth -> assoc -> RUN ->
 * 4-way) WITHOUT anybody re-posting kIONetworkLinkActive - so configd
 * kept the dropped-lease picture (yellow dot) until a manual click
 * bounced the interface. Mirror every authorization change to the OS
 * carrier exactly once per change. */
void RTWUSBController::refreshLinkEvent() {
    const bool up=authorized();
    if(up==linkActivePosted_)return;
    linkActivePosted_=up;
    core_.sc_ic.ic_if.if_link_state=up?LINK_STATE_UP:LINK_STATE_DOWN;
    if(up){
        dhcpHeals_=0; /* fresh link episode: the auto-heal budget resets */
        /* 1.0.16: a fresh UP edge opens a new lease episode. On a
         * healthy DHCP host configd asks for a lease within seconds
         * of this edge, so an episode that ages out with zero
         * exchanges can only be a dead lease path. */
        episodeStartMs_=nowMS();episodeDiscovers_=0;episodeReplies_=0;leaseKicks_=0;
    }
    IOLog("RtWlanUCompat: link event -> %s%s\n",up?"UP":"DOWN",
          up?" (kIONetworkLinkActive posted, configd will (re)run DHCP)":"");
    if(up)setLinkStatus(kIONetworkLinkActive|kIONetworkLinkValid,getCurrentMedium());
    else setLinkStatus(kIONetworkLinkValid);
}
/* 1.0.15: recognize the stack's DHCP DISCOVERs on the TX path. The frame
 * is a full Ethernet head from the IP stack; DHCP DISCOVER is always
 * broadcast IPv4/UDP from port 68 to port 67. Only the leading 42 bytes
 * are inspected - the probe can never misread a frame. */
void RTWUSBController::noteTxDhcpDiscover(mbuf_t packet) {
    if(!packet)return;
    /* Headers may straddle mbufs on bpf-originated frames; gather the
     * first 42 bytes across the chain before judging. */
    uint8_t head[42];size_t have=0;
    for(mbuf_t m=packet;m&&have<sizeof(head);m=mbuf_next(m)){
        size_t n=mbuf_len(m);if(n>sizeof(head)-have)n=sizeof(head)-have;
        if(!n)break;
        memcpy(head+have,mbuf_data(m),n);have+=n;
    }
    if(have<sizeof(head))return;
    const uint8_t*b=head;
    if(b[0]!=0xff||b[1]!=0xff||b[2]!=0xff||b[3]!=0xff||b[4]!=0xff||b[5]!=0xff)return;
    if(b[12]!=0x08||b[13]!=0x00)return;
    if((b[14]>>4)!=4||(b[14]&0x0f)!=5||b[23]!=17)return;
    const unsigned sport=((unsigned)b[34]<<8)|b[35],dport=((unsigned)b[36]<<8)|b[37];
    if(sport!=68||dport!=67)return;
    const uint64_t now=nowMS();
    const bool announce=!lastDhcpDiscoverMs_||now-lastDhcpDiscoverMs_>=10000;
    ++dhcpDiscovers_;
    ++episodeDiscovers_;
    lastDhcpDiscoverMs_=now;
    everSawDhcp_=true; /* 1.0.16: this host uses DHCP - watchdog allowed */
    /* 1.0.17: sparse per-discover log - makes configd's begging visible in
     * the dmesg ring without flooding it. */
    if(announce)IOLog("RtWlanUCompat: dhcp discover #%llu on TX (episode "
          "discovers=%u replies=%u)\n",(unsigned long long)dhcpDiscovers_,
          episodeDiscovers_,episodeReplies_);
}
/* 1.0.17: the lease completes only when the server's reply (OFFER/ACK,
 * UDP 67->68) comes back through the RX data path. Beacons prove nothing
 * about data frames; this sniff is the single bit the lease watchdog
 * needs: a reply means the data path is alive, none means it is dead. */
void RTWUSBController::noteRxDhcpReply(mbuf_t packet) {
    if(!packet)return;
    uint8_t head[42];size_t have=0;
    for(mbuf_t m=packet;m&&have<sizeof(head);m=mbuf_next(m)){
        size_t n=mbuf_len(m);if(n>sizeof(head)-have)n=sizeof(head)-have;
        if(!n)break;
        memcpy(head+have,mbuf_data(m),n);have+=n;
    }
    if(have<sizeof(head))return;
    const uint8_t*b=head;
    if(b[12]!=0x08||b[13]!=0x00)return;
    if((b[14]>>4)!=4||(b[14]&0x0f)!=5||b[23]!=17)return;
    const unsigned sport=((unsigned)b[34]<<8)|b[35],dport=((unsigned)b[36]<<8)|b[37];
    if(sport!=67||dport!=68)return;
    ++episodeReplies_;everSawDhcp_=true;dhcpHealMisses_=0; /* reply: reset backoff ladder */
    const uint64_t now=nowMS();
    if(!lastDhcpReplyMs_||now-lastDhcpReplyMs_>=5000){
        lastDhcpReplyMs_=now;
        IOLog("RtWlanUCompat: dhcp server reply on RX - data path alive "
              "(episode replies=%u)\n",episodeReplies_);
    }
}
/* configd sends DISCOVER only while it holds no lease. A healthy exchange
 * answers within a few seconds and the burst stops; four DISCOVERs inside
 * a 60 s window mean the exchange never completes - the data path is dead
 * under a live-looking association. */
bool RTWUSBController::dhcpStuck() const {
    if(!lastDhcpDiscoverMs_||dhcpDiscovers_<4)return false;
    return nowMS()-lastDhcpDiscoverMs_<60000;
}
/* Watchdog-driven self-heal: no user click required. Budget three rebuilds
 * per link episode with a 90 s cooldown, so a genuinely broken radio can
 * never turn into a rebuild loop; a healthy association never reaches the
 * trigger because its DHCP exchanges finish and the burst decays. */
void RTWUSBController::autoHealStuckDhcp() {
    const uint64_t now=nowMS();
    if(lastDhcpDiscoverMs_&&now-lastDhcpDiscoverMs_>120000){dhcpDiscovers_=0;return;}
    if(!dhcpStuck()||!linkActivePosted_||!authorized())return;
    /* 1.0.18 field fix: log ONLY when the rebuild actually runs -
     * rebuildLink owns the budget and may refuse; a refused attempt must
     * stay silent or it floods the dmesg ring (the 1.0.17 bug). */
    if(!rebuildLink("auto-heal"))return;
    const unsigned long long seen=dhcpDiscovers_;
    ++dhcpHeals_;lastDhcpHealMs_=now;dhcpDiscovers_=0;
    IOLog("RtWlanUCompat: %llu DHCP discovers went unanswered while RUN - "
          "rebuilding link (auto-heal)\n",seen);
}
/* 1.0.16 backstop: configd keeps the DISCOVER burst going for roughly
 * two minutes after a loss, then gives up. Once the burst decays,
 * dhcpStuck() disarms and the idempotent-join guard swallows re-joins
 * again while the lease is still missing - the persistent yellow dot.
 * A RUN episode that reaches 45 s without a single DHCP server reply
 * cannot happen on a healthy DHCP network (configd always asks right
 * after the carrier UP edge and the answer comes back in seconds), so
 * rebuild the link to hand configd the carrier edge it needs. Keyed on
 * REPLIES seen on the RX data path, not on discovers: configd begging
 * with unanswered discovers is exactly the broken case. Budget three
 * rebuilds per 15 min with a 5 min cooldown; static-IP hosts never
 * spoke DHCP and are exempt via everSawDhcp_. */
void RTWUSBController::autoHealSilentLease() {
    const uint64_t now=nowMS();
    if(!authorized()||!linkActivePosted_||!everSawDhcp_||!episodeStartMs_)return;
    if(now-episodeStartMs_<45000)return;
    /* 1.0.18: configd must actually be begging - exempts static-IP hosts
     * and a configd that already gave up its DISCOVER burst. */
    if(episodeReplies_||!episodeDiscovers_)return;
    /* 1.0.18 field fix: this logged EVERY watchdog tick while rebuildLink
     * silently refused on budget - 60 lines/min that flushed the dmesg
     * ring. Log only when the rebuild actually runs. */
    if(!rebuildLink("lease-silence"))return;
    ++leaseKicks_;
    IOLog("RtWlanUCompat: RUN %llu s without a single DHCP server reply - "
          "rebuilding link so configd re-acquires the lease\n",
          (unsigned long long)((now-episodeStartMs_)/1000));
}
/* The proven cure for a dead data path is exactly what a reboot does:
 * stop the station, clear a possibly halted RX pipe, restart it.
 * Build 1.0.17: the restart must ERASE the profile (quiesce(true)) - the
 * same starting state a reboot and a manual click begin from. Field
 * evidence 2026-11-09 07:16: a profile-preserving rebuild re-associated
 * perfectly (RUN, fresh beacons, carrier UP) yet the DHCP exchange STILL
 * never completed - the stale profile state itself is the poison. The
 * utility's profile watchdog re-seeds the join within ~3 s of seeing the
 * disconnect (capture 07:16: disconnect 461 -> key set 464), so the
 * erase-rebuild self-completes into the clean association that actually
 * restores the lease. One shared budget for every caller: max 2 rebuilds
 * per 15 min, at least 4 min apart, so a broken radio can never become a
 * visible bounce loop. */
bool RTWUSBController::rebuildLink(const char *why) {
    const uint64_t now=nowMS();
    if(lastHealMs_&&now-lastHealMs_>900000)heals_=0;
    /* 1.0.18: a rebuild that still ends without a DHCP reply did not help
     * - escalate the cooldown 4 -> 8 -> 15 min so a dead lease path can
     * never become a visible bounce loop. One RX reply resets the ladder
     * (dhcpHealMisses_ cleared in noteRxDhcpReply). */
    uint64_t cooldown=240000;
    if(dhcpHealMisses_>=2)cooldown=900000;
    else if(dhcpHealMisses_==1)cooldown=480000;
    if(lastHealMs_&&now-lastHealMs_<cooldown)return false;
    if(heals_>=2)return false;
    if(lastHealMs_&&lastDhcpReplyMs_<lastHealMs_)++dhcpHealMisses_;else dhcpHealMisses_=0;
    ++heals_;lastHealMs_=now;
    quiesce(true);
    if(rxPipe_&&!rxRecovery_.pending){
        IOReturn unstuck=rxPipe_->clearStall(true);
        IOLog("RtWlanUCompat: %s RX pipe unstick -> 0x%08x\n",why,(unsigned)unstuck);
    }
    IOReturn result=enableStation();
    IOLog("RtWlanUCompat: link rebuild (%s) -> 0x%08x\n",why,(unsigned)result);
    if(result!=kIOReturnSuccess)quiesce(true);
    return true;
}
void RTWUSBController::eraseProfile() {
    resetDFSStation();
    resetWpaRecovery();
    joinRequested_=false;auto&ic=core_.sc_ic;
    explicit_bzero(ic.ic_psk,sizeof(ic.ic_psk));explicit_bzero(ic.ic_des_essid,sizeof(ic.ic_des_essid));ic.ic_des_esslen=0;
    ic.ic_flags&=~(IEEE80211_F_PSK|IEEE80211_F_RSNON|IEEE80211_F_AUTO_JOIN|IEEE80211_F_DESBSSID); /* mejoras: DESBSSID */
}
void RTWUSBController::quiesce(bool erase) {
    if(stopping_)return;
    /* 1.0.9 field fix (RTLWiFiBar.log 2026-11-18 07:42+07:54, driver 1.0.8:
     * "it not reconnect after another reboot even if I try Forget network
     * and change the USB Port"). Every teardown used to leave the AP with
     * a ghost association: the stop path transitioned INIT with arg -1,
     * and this port's net80211 treats -1 as "send no management frame" -
     * and by the time that transition ran, the stopping_/running_ TX gates
     * were closed anyway. The AP keeps such a ghost entry for its idle
     * timeout (minutes); the next AUTH from this same MAC after a reboot
     * is then simply ignored and net80211 cycles SCAN->AUTH forever -
     * exactly the field shape (join progress 1 -> 2 -> 1 -> 2, never 3;
     * Forget and a USB port change cannot fix the AP's table). Say goodbye
     * BEFORE any teardown: the CORE newstate INIT transition emits
     * DISASSOC (from RUN) or DEAUTH (from AUTH/ASSOC) through the same
     * mgmt path every join uses, with the chip still alive and stopping_
     * still false so the output gates pass. Must carry a real reason arg,
     * never -1. Best effort: a chip that is already USB-dead just fails
     * the TX and the teardown proceeds regardless. */
    if(attached_&&present_&&!sleeping_&&running_&&
       usbError_==kIOReturnSuccess&&!core_.core_error&&
       core_.sc_newstate&&core_.sc_ic.ic_bss){
        const int st=core_.sc_ic.ic_state;
        if(st==IEEE80211_S_RUN||st==IEEE80211_S_ASSOC||st==IEEE80211_S_AUTH)
            (void)core_.sc_newstate(&core_.sc_ic,IEEE80211_S_INIT,
                IEEE80211_REASON_ASSOC_LEAVE);
    }
    stopping_=true;radioReady_=false;enabled_=false;lastBeaconMs_=0;
    rtw_signal_reset(&signal_);
    cancelWpaRecovery();
    resetRateControl();
    if(auto*queue=getOutputQueue()){queue->stop();queue->flush();}
    super::setLinkStatus(kIONetworkLinkValid);
    if(attached_){core_.sc_ic.ic_if.if_flags&=~IFF_UP;if(is8723Family())stop8723();else if(is8710BUFamily())stop8710BU();else if(is8192FUFamily())stop8192FU();else if(family_==RTW_WIFI_RTL8192DU)stop8192DU();else if(is88Family())stop88();else if(is8812Family())stop8812();else if(family_==RTW_WIFI_RTL8814AU)stop8814();else rtwn_stop(&core_.sc_ic.ic_if);ifq_flush(&core_.sc_ic.ic_if.if_snd);}
    else if(completionLock_)abortTransfers();
    if(scanDiag_.state==rtw::ScanDiagnosticState::Active){
        rtw::scanDiagnosticsTransition(scanDiag_,true,rtw::ScanDiagnosticTransition::Init,0,0);
        publishScanDiagnostics();
    }
    resetDFSStation(true);
    if(erase)eraseProfile();
    stopping_=false;
    /* 1.0.15: keep the carrier mirror consistent. quiesce() posts the
     * carrier DOWN directly through setLinkStatus(); without resetting the
     * mirror bookkeeping the next recovery could early-return out of
     * refreshLinkEvent() and never re-post kIONetworkLinkActive. */
    linkActivePosted_=false;
    core_.sc_ic.ic_if.if_link_state=LINK_STATE_DOWN;
}
/* 1.1.2 "Disconnect USB WiFi" (RTLWiFiBar): leave the network WITHOUT
 * powering the chip down. Field log 2026-11-22 15:04 (MAC 80:af:ca:d3:e4:08):
 * the first version called quiesce(true), which stops the MAC and leaves
 * calibTimer_ running; the next 2 s tick then touched the stopped chip, the
 * tick failed (RTWDiagCoreError=6) and latched core_error, so every later
 * call - the Reconnect join included - answered NotReady (0xe00002d8) until
 * a session rescue re-enumerated the dongle. A real join never hits this
 * because executeJoinTail runs quiesce() and enableStation() back to back.
 * Here the chip stays up: say goodbye to the AP (same INIT transition
 * quiesce() uses, real reason, chip alive), drop the carrier, erase the
 * profile so nothing auto-joins; the station idles in INIT until a join or
 * scan arrives. */
void RTWUSBController::softLeave() {
    if(stopping_)return;
    if(attached_&&present_&&!sleeping_&&running_&&
       usbError_==kIOReturnSuccess&&!core_.core_error&&
       core_.sc_newstate&&core_.sc_ic.ic_bss){
        const int st=core_.sc_ic.ic_state;
        if(st==IEEE80211_S_RUN||st==IEEE80211_S_ASSOC||st==IEEE80211_S_AUTH)
            (void)core_.sc_newstate(&core_.sc_ic,IEEE80211_S_INIT,
                IEEE80211_REASON_ASSOC_LEAVE);
    }
    cancelWpaRecovery();
    resetRateControl();
    if(auto*queue=getOutputQueue())queue->flush();
    super::setLinkStatus(kIONetworkLinkValid);
    if(attached_)ifq_flush(&core_.sc_ic.ic_if.if_snd);
    resetDFSStation(true);
    eraseProfile();
    linkActivePosted_=false;
    core_.sc_ic.ic_if.if_link_state=LINK_STATE_DOWN;
}
IOReturn RTWUSBController::enableStation() {
    if(!attached_||!present_||sleeping_||usbError_!=kIOReturnSuccess||core_.core_error||!netif_||!stats_){
        /* 1.0.6 reboot-2 wedge cure: after the first failed bring-up the
         * transport fault is latched, so every later bring-up attempt exits
         * HERE and the failure ladder never reaches its 4th count - in the
         * field the software re-plug never fired at all. A latched
         * usbError_/core_error before the first successful bring-up (and
         * after at least one real attempt reached the chip) is the wedge
         * signature; count it so the ladder and the boot rescue can fire. */
        if(bootRescueArmed_&&!sleeping_&&bringUpAttempts_&&!bringUpEverSucceeded_&&
           (usbError_!=kIOReturnSuccess||core_.core_error)){
            ++wedgedBringUps_;
            scheduleSoftwareReplug("transport fault latched before first bring-up");
        }
        return kIOReturnNotReady;
    }
    ++bringUpAttempts_;
    if(enabled_&&running_)return kIOReturnSuccess;
    core_.sc_ic.ic_if.if_flags|=IFF_UP;
    int result=is8723Family()?start8723():is8710BUFamily()?start8710BU():is8192FUFamily()?start8192FU():family_==RTW_WIFI_RTL8192DU?start8192DU():is88Family()?start88():is8812Family()?start8812():family_==RTW_WIFI_RTL8814AU?start8814():rtwn_init(&core_.sc_ic.ic_if);
    /* 1.0.43: any bring-up (first scan, join, rescan) can time out inside
     * start88 on the 8821CU (RTWDiagStart88FailStage=2, Sub=9 = firmware
     * download, ETIMEDOUT) and latch core_error + firmware.last_error, after
     * which every call answers NotReady until the dongle is moved. The chip
     * is fine: power it down (start88 already did), clear the latches and
     * try again up to 3 times, 300 ms apart. */
    if(result&&(family_==RTW_WIFI_RTL8821CU||family_==RTW_WIFI_RTL8821AU||family_==RTW_WIFI_RTL8812AU)){
        for(unsigned retry=0;retry<3&&result&&!ioError(this)&&usbError_==kIOReturnSuccess&&!stopping_&&!closing_&&!sleeping_;++retry){
            IOSleep(300);
            core_.core_error=0;
            r88Core_.firmware.last_error=0;
            ++joinBringUpRetries_;
            core_.sc_ic.ic_if.if_flags|=IFF_UP;
            result=is8812Family()?start8812():start88();
        }
        publishDiagnosticCounter("RTWDiagJoinBringUpRetries",joinBringUpRetries_);
    }
    if(result){quiesce(false);/* 1.0.5/1.0.6: persistent bring-up failure with the
        station never up is the boot-wedge signature; count it and let the
        software re-plug decide. */
        if(!bringUpEverSucceeded_){wedgedBringUps_++;scheduleSoftwareReplug("chip bring-up");}
        return fromErrno(result);}
    enabled_=radioReady_=true;wedgedBringUps_=0;bringUpEverSucceeded_=true;
    /* Sequence completion is not a claim that the physical radio has been
     * validated; the stable register-result chain mask remains observable. */
    setProperty("IQCalibrationMask",(unsigned long long)core_.iq_calibration_mask,8);
    return kIOReturnSuccess;
}
uint32_t RTWUSBController::supported5GHzFlags() const {
    if(!attached_)return 0;
    return rtw_station_band_flags([this](uint8_t channel){
        return (core_.sc_ic.ic_channels[channel].ic_flags&(channel<=14?IEEE80211_CHAN_2GHZ:IEEE80211_CHAN_5GHZ))!=0;
    });
}
void RTWUSBController::status(RTWControlStatus*out) {
    bzero(out,sizeof(*out));out->version=RTW_CONTROL_VERSION;
    out->flags|=RTW_CONTROL_SIGNAL_TRACKED;
    out->flags|=supported5GHzFlags();
    if(present_)out->flags|=RTW_CONTROL_DEVICE;
    if(radioReady_)out->flags|=RTW_CONTROL_RADIO;
    if(running_)out->flags|=RTW_CONTROL_RUNNING;
    if(authorized())out->flags|=RTW_CONTROL_AUTHORIZED;
    auto&ic=core_.sc_ic;out->state=ic.ic_state;
    /* 1.0.12: beacon age lets the compat layer (and the logs) tell a
     * healthy RUN from a zombie RUN without any ABI growth. */
    out->beaconAgeMs=(ic.ic_state==IEEE80211_S_RUN&&lastBeaconMs_)
        ?(uint32_t)(nowMS()-lastBeaconMs_):0;
    if(ic.ic_state==IEEE80211_S_SCAN)out->flags|=RTW_CONTROL_SCANNING;
    out->usbError=usbError_;memcpy(out->mac,mac_.bytes,6);
    if(attached_&&ic.ic_bss){
        auto*n=ic.ic_bss;memcpy(out->bssid,n->ni_bssid,6);
        out->ssidLength=n->ni_esslen<=32?n->ni_esslen:0;memcpy(out->ssid,n->ni_essid,out->ssidLength);
        if(n->ni_chan&&n->ni_chan!=IEEE80211_CHAN_ANYC)out->channel=ieee80211_chan2ieee(&ic,n->ni_chan);
        if(ic.ic_state==IEEE80211_S_RUN){
            const uint64_t now=nowMS();
            out->rssi=rtw_signal_value(&signal_,n->ni_bssid,(uint16_t)out->channel,now);
            if(out->rssi&&now-signal_.sampled_ms>=RTW_SIGNAL_HELD_AGE_MS)
                out->flags|=RTW_CONTROL_SIGNAL_HELD;
        }
    }
    out->txPackets=txPackets_;out->rxPackets=rxPackets_;out->txErrors=txErrors_;out->rxErrors=rxErrors_;
    refreshLinkEvent();
    publishDiagnostics();
}
void RTWUSBController::publishDiagnosticCounter(const char *name, uint64_t value) {
    // Called while holding the controller's gate. Registry mutations can
    // allocate OSNumber objects; avoid them when the published value is stable.
    const auto *existing=OSDynamicCast(OSNumber,getProperty(name));
    if(rtw::diagnosticCounterNeedsWrite(existing!=nullptr,
        existing?existing->numberOfBits():0,
        existing?existing->unsigned64BitValue():0,value))
        setProperty(name,(unsigned long long)value,64);
}
void RTWUSBController::publishDiagnostics() {
    /* Control status is polled under the driver's gate. Publish counters here,
     * outside the RX hot path; no packet contents or key state are exported. */
    auto publish=[this](const char*name,uint64_t value){
        publishDiagnosticCounter(name,value);
    };
    /* Odd means a publication is in progress. The next even value commits
     * this device's snapshot; registry readers still need bracketing reads
     * before claiming an atomic snapshot. All writers run under our gate. */
    publish("RTWDiagPublicationGeneration",++diagnosticPublicationGeneration_);
    publish("RTWDiagCoreError",(uint32_t)core_.core_error);
    if(r88StartFails_){publish("RTWDiagStart88FailStage",r88StartFailStage_);publish("RTWDiagStart88FailErr",r88StartFailErr_);publish("RTWDiagStart88Fails",r88StartFails_);publish("RTWDiagStart88FailSub",r88StartFailSub_);publish("RTWDiagStart88FailDetail",r88StartFailDetail_);}
    if(coexErr_){
        publish("RTWDiagCoexErr",coexErr_);publish("RTWDiagCoexErrSite",coexErrSiteLatched_);
        publish("RTWDiagCoexErrAfterAttachMS",coexErrAfterAttachMS_);publish("RTWDiagCoexTickErrors",coexTickErrors_);
    }
    if(faultCode_){
        publish("RTWDiagFaultCode",faultCode_);publish("RTWDiagFaultSite",faultSite_);
        publish("RTWDiagFaultDetail",faultDetail_);publish("RTWDiagFaultAfterAttachMS",faultAfterAttachMS_);
        publish("RTWDiagFaultIcState",faultIcState_);publish("RTWDiagFaultRunning",faultRunning_);
    }
    publishRateControlDiagnostics();
    /* No backend currently caches a complete, association-fresh effective
     * TX PHY tuple (rate, bandwidth and GI). Negotiated capability and RX MCS
     * are not an effective TX speed. Keep Ethernet metadata unknown. */
    publish("RTWDiagLinkSpeedKnown",0);
    publish("RTWDiagLinkSpeedBps",0);
    publish("SignalValidSamples",signal_.valid_samples);
    publish("SignalMissingPHY",signal_.missing_phy);
    publish("SignalInvalidPHY",signal_.invalid_phy);
    publish("SignalForeignFrames",signal_.foreign_frames);
    const uint64_t signalNow=nowMS();
    const bool samplePresent=signal_.valid&&signalNow>=signal_.sampled_ms;
    publish("SignalSampleAgeMS",samplePresent?signalNow-signal_.sampled_ms:UINT64_MAX);
    publish("SignalLastDBmMagnitude",samplePresent?(uint64_t)(-signal_.dbm):0);
    // Previously TX errors mixed USB faults and full software queues.
    // Keep the existing cumulative counter/ABI; publish separate causes.
    publish("RTWDiagTXQueueDrops",txQueueDrops_);
    publish("RTWDiagTXUSBTransferErrors",txUSBTransferErrors_);
    publish("RTWDiagTXLinkDrops",txLinkDrops_);
    publish("RTWDiagRXAggregates",rxDiag_.aggregates);
    publish("RTWDiagRXBytes",rxDiag_.bytes);
    publish("RTWDiagRXLastReadBytes",rxDiag_.lastReadBytes);
    publish("RTWDiagRXMaxReadBytes",rxDiag_.maxReadBytes);
    publish("RTWDiagRXAggregateLastErrorReadBytes",rxDiag_.lastAggregateErrorReadBytes);
    publish("RTWDiagRXAggregateErrors",rxDiag_.aggregateErrors);
    publish("RTWDiagRXAggregateTruncated",rxDiag_.aggregateTruncated);
    publish("RTWDiagRXAggregateMalformed",rxDiag_.aggregateMalformed);
    publish("RTWDiagRXAggregateTooMany",rxDiag_.aggregateTooMany);
    publish("RTWDiagRXAggregateBadArgument",rxDiag_.aggregateBadArgument);
    publish("RTWDiagRXAggregateLastErrorOffset",rxDiag_.lastAggregateErrorOffset);
    publish("RTWDiagRXAggregateLastErrorFrames",rxDiag_.lastAggregateErrorFrames);
    publish("RTWDiagRXPartialAggregates",rxDiag_.partialAggregates);
    publish("RTWDiagRXPartialPrefixFrames",rxDiag_.partialPrefixFrames);
    publish("RTWDiagRXPartialPrefixBytes",rxDiag_.partialPrefixBytes);
    publish("RTWDiagRXC2HReports",rxDiag_.c2h);
    publish("RTWDiagRXCRCDrops",rxDiag_.crcDrops);
    publish("RTWDiagRXICVDrops",rxDiag_.icvDrops);
    publish("RTWDiagRXInvalidFrameDrops",rxDiag_.invalidFrameDrops);
    publish("RTWDiagRXHardwareDecryptDrops",rxDiag_.hardwareDecryptDrops);
    publish("RTWDiagRXAllocationDrops",rxDiag_.allocationDrops);
    publish("RTWDiagRXCopyDrops",rxDiag_.copyDrops);
    publish("RTWDiagRXNoNodeDrops",rxDiag_.noNodeDrops);
    publish("RTWDiagRXProtectedMPDUs",rxDiag_.protectedMPDUs);
    publish("RTWDiagRXMPDUsToNet80211",rxPackets_);
    publish("RTWDiagRXEthernetQueued",rxDiag_.ethernetQueued);
    publish("RTWDiagRXHandoffErrors",rxDiag_.handoffErrors);
    publish("RTWDiagNet80211InputPackets",stats_?stats_->inputPackets:0);
    publish("RTWDiagNet80211InputErrors",stats_?stats_->inputErrors:0);
    const auto&input=core_.sc_ic.ic_if.if_input_diag;
    publish("RTWDiagRXInputCalls",input.calls);
    publish("RTWDiagRXInputOfferedPackets",input.offeredPackets);
    publish("RTWDiagRXInputOfferedBytes",input.offeredBytes);
    publish("RTWDiagRXInputImmediateReturns",input.immediateReturns);
    publish("RTWDiagRXInputFlushCalls",input.flushCalls);
    publish("RTWDiagRXInputFlushPackets",input.flushPackets);
    publish("RTWDiagRXInputSubmissionMismatches",input.submissionMismatches);
    publish("RTWDiagRXInputNoIfnetCalls",input.noIfnetCalls);
    /* The IOKit statistics buffer and the BSD interface counters are distinct
     * observations. Read the interface KPI; never adjust either counter to
     * make the two agree. getIfnet() is valid only while the child is attached. */
    ifnet_stats_param nativeStats{};
    const auto nativeIfnet=attached_&&netif_&&!closing_?netif_->getIfnet():nullptr;
    const int nativeStatsError=nativeIfnet?ifnet_stat(nativeIfnet,&nativeStats):ENXIO;
    const bool nativeStatsValid=nativeStatsError==0;
    publish("RTWDiagNativeIfnetIndex",nativeIfnet?ifnet_index(nativeIfnet):0);
    publish("RTWDiagNativeIfnetStatsValid",nativeStatsValid?1:0);
    publish("RTWDiagNativeIfnetStatsError",(uint64_t)nativeStatsError);
    publish("RTWDiagNativeIfnetInputPackets",nativeStatsValid?nativeStats.packets_in:0);
    publish("RTWDiagNativeIfnetInputBytes",nativeStatsValid?nativeStats.bytes_in:0);
    publish("RTWDiagNativeIfnetInputErrors",nativeStatsValid?nativeStats.errors_in:0);
    const auto&s=core_.sc_ic.ic_stats;
    publish("RTWDiagNet80211TooShort",s.is_rx_tooshort);
    publish("RTWDiagNet80211NoBuffer",s.is_rx_nombuf);
    publish("RTWDiagNet80211WrongBSS",s.is_rx_wrongbss);
    publish("RTWDiagNet80211WrongDirection",s.is_rx_wrongdir);
    publish("RTWDiagNet80211Duplicates",s.is_rx_dup);
    publish("RTWDiagNet80211Unencrypted",s.is_rx_unencrypted);
    publish("RTWDiagNet80211ProtectionOff",s.is_rx_nowep);
    publish("RTWDiagNet80211DecryptFailures",s.is_rx_wepfail);
    publish("RTWDiagNet80211DecapFailures",s.is_rx_decap);
    publish("RTWDiagNet80211Unauthorized",s.is_rx_unauth);
    publish("RTWDiagNet80211CCMPReplays",s.is_ccmp_replays);
    publish("RTWDiagNet80211CCMPMICFailures",s.is_ccmp_dec_errs);
    /* Export existing stack events verbatim. no_buf includes both a missing
     * reorder buffer and an occupied duplicate slot, not just allocation.
     * Window events are not a one-to-one decomposition of inputErrors. */
    publish("RTWDiagNet80211RXBABelowWindow",s.is_ht_rx_frame_below_ba_winstart);
    publish("RTWDiagNet80211RXBAAboveWindow",s.is_ht_rx_frame_above_ba_winend);
    publish("RTWDiagNet80211RXBAWindowSlides",s.is_ht_rx_ba_window_slide);
    publish("RTWDiagNet80211RXBAWindowJumps",s.is_ht_rx_ba_window_jump);
    publish("RTWDiagNet80211RXBANoBuffer",s.is_ht_rx_ba_no_buf);
    publish("RTWDiagNet80211RXBAFramesLost",s.is_ht_rx_ba_frame_lost);
    publish("RTWDiagNet80211RXBAWindowGapTimeouts",s.is_ht_rx_ba_window_gap_timeout);
    publish("RTWDiagNet80211RXBATimeouts",s.is_ht_rx_ba_timeout);
    publish("RTWDiagNet80211TXBATimeouts",s.is_ht_tx_ba_timeout);
    publishCalibrationDiagnostics();
    publishScanDiagnostics();
    publishRXRecoveryDiagnostics();
    publishWpaDiagnostics();
    publishWpaRecoveryDiagnostics();
    publishPerformanceDiagnostics();
    const uint64_t publicationNow=nowMS();
    if(publicationNow>diagnosticPublicationMonotonicMS_)diagnosticPublicationMonotonicMS_=publicationNow;
    publish("RTWDiagPublicationMonotonicMS",diagnosticPublicationMonotonicMS_);
    publish("RTWDiagPublicationGeneration",++diagnosticPublicationGeneration_);
}
void RTWUSBController::publishScanDiagnostics() {
    auto publish=[this](const char*name,uint64_t value){publishDiagnosticCounter(name,value);};
    rtw::publishScanDiagnostics(scanDiag_,publish);
    auto&ic=core_.sc_ic;unsigned channel=0;
    if(attached_&&ic.ic_bss&&ic.ic_bss->ni_chan&&ic.ic_bss->ni_chan!=IEEE80211_CHAN_ANYC)
        channel=ieee80211_chan2ieee(&ic,ic.ic_bss->ni_chan);
    publish("RTWDiagCurrentChannel",rtw::scanDiagnosticChannelInRange(channel)?channel:0);
    publish("RTWDiagSupportedBandFlags",supported5GHzFlags());
    /* These are cached BSS records, distinct from the repeated beacon/probe
     * frame observations above. Bound the traversal independently of the
     * client's scan-results page size, without exporting any identity. */
    uint64_t bss24=0,bss5=0,bssUnknown=0;unsigned visited=0;
    auto*node=attached_?RB_MIN(ieee80211_tree,&ic.ic_tree):nullptr;
    while(node&&visited<1024){
        ++visited;unsigned cachedChannel=0;
        if(node->ni_chan&&node->ni_chan!=IEEE80211_CHAN_ANYC)
            cachedChannel=ieee80211_chan2ieee(&ic,node->ni_chan);
        const unsigned band=rtw::scanDiagnosticBand(cachedChannel);
        if(band==24)++bss24;else if(band==5)++bss5;else ++bssUnknown;
        node=RB_NEXT(ieee80211_tree,&ic.ic_tree,node);
    }
    publish("RTWDiagScanCacheBSS24",bss24);publish("RTWDiagScanCacheBSS5",bss5);
    publish("RTWDiagScanCacheBSSUnknown",bssUnknown);publish("RTWDiagScanCacheBSSVisited",visited);
    publish("RTWDiagScanCacheBSSTruncated",node?1:0);
}
void RTWUSBController::publishWpaDiagnostics() {
    /* Called under the control gate. Publish scalar progress only: no SSID,
     * key bytes, nonces, replay-counter values or packet contents. */
    auto publish=[this](const char*name,uint64_t value){publishDiagnosticCounter(name,value);};
    const auto&ic=core_.sc_ic;
    const auto*node=ic.ic_bss;
    publish("RTWDiagWPAProfileActive",joinRequested_?1:0);
    publish("RTWDiagWPARSNEnabled",(ic.ic_flags&IEEE80211_F_RSNON)?1:0);
    publish("RTWDiagWPAPSKConfigured",(ic.ic_flags&IEEE80211_F_PSK)?1:0);
    publish("RTWDiagWPAEAPOLReceived",ic.ic_stats.is_rx_eapol_key);
    publish("RTWDiagWPAEAPOLReplays",ic.ic_stats.is_rx_eapol_replay);
    publish("RTWDiagWPAEAPOLBadMIC",ic.ic_stats.is_rx_eapol_badmic);
    publish("RTWDiagWPASupplicantState",node?node->ni_rsn_supp_state:0);
    publish("RTWDiagWPAPortValid",node?node->ni_port_valid:0);
    publish("RTWDiagWPAProtocol",node?node->ni_rsnprotos:0);
    publish("RTWDiagWPACipher",node?node->ni_rsncipher:0);
    publish("RTWDiagWPAAssocStatus",ic.ic_assoc_status);
    publish("RTWDiagWPADeauthReason",ic.ic_deauth_reason);
    publish("RTWDiagWPAProtectionMode",ic.ic_protmode);
}
void RTWUSBController::channelMap(RTWControlChannelMap*out) const {
    bzero(out,sizeof(*out));out->version=RTW_CONTROL_VERSION;
    if(!attached_)return;
    for(unsigned channel=1;channel<256;++channel) {
        const auto flags=core_.sc_ic.ic_channels[channel].ic_flags;
        if(flags&(channel<=14?IEEE80211_CHAN_2GHZ:IEEE80211_CHAN_5GHZ))
            out->channels[channel/8U]|=(uint8_t)(1U<<(channel%8U));
    }
}
void RTWUSBController::scanResults(RTWControlScanResults*out) {
    bzero(out,sizeof(*out));out->version=RTW_CONTROL_VERSION;if(!attached_)return;
    ieee80211_node*node=RB_MIN(ieee80211_tree,&core_.sc_ic.ic_tree);
    unsigned visited=0;
    while(node&&visited++<1024){
        if(out->count==RTW_CONTROL_MAX_NETWORKS){out->truncated=1;break;}
        if(node->ni_esslen<=32&&node->ni_chan&&node->ni_chan!=IEEE80211_CHAN_ANYC){
            auto&r=out->records[out->count++];r.ssidLength=node->ni_esslen;memcpy(r.ssid,node->ni_essid,r.ssidLength);memcpy(r.bssid,node->ni_bssid,6);
            r.channel=(uint8_t)ieee80211_chan2ieee(&core_.sc_ic,node->ni_chan);r.rssi=node->ni_rssi?(int16_t)node->ni_rssi-100:0;
            /* A CCMP *pairwise* cipher alone does not prove this station can
             * connect: group TKIP and mandatory PMF are unsupported by the
             * selected WPA2-PSK/CCMP configuration. Only report a compatible
             * network when the complete RSN policy is satisfied. */
            const rtw_network_security capabilities{
                static_cast<uint8_t>((node->ni_capinfo&IEEE80211_CAPINFO_PRIVACY)!=0),
                static_cast<uint8_t>((node->ni_rsnprotos&IEEE80211_PROTO_RSN)!=0),
                static_cast<uint8_t>((node->ni_rsnakms&IEEE80211_AKM_PSK)!=0),
                static_cast<uint8_t>((node->ni_rsnciphers&IEEE80211_CIPHER_CCMP)!=0),
                static_cast<uint8_t>(node->ni_rsngroupcipher==IEEE80211_CIPHER_CCMP),
                static_cast<uint8_t>((node->ni_rsncaps&IEEE80211_RSNCAP_MFPR)!=0)
            };
            if(capabilities.privacy)r.securityFlags|=RTW_CONTROL_SECURITY_PRIVACY;
            if(rtw_network_security_classify(&capabilities)==RTW_NETWORK_WPA2_PSK_CCMP)
                r.securityFlags|=RTW_CONTROL_SECURITY_WPA2_PSK|RTW_CONTROL_SECURITY_CCMP;
            else {
                r.securityFlags|=RTW_CONTROL_SECURITY_UNSUPPORTED;
                /* Field round 9: refine the display class of non-joinable
                 * networks. The station still cannot connect to them, but
                 * the utility's join dialog then shows the true mode
                 * ("WPA-PSK TKIP" / "WPA2-PSK TKIP" / WEP) instead of
                 * mislabeling everything WEP. Pure 802.1X (enterprise)
                 * networks stay UNSUPPORTED without refinement - the
                 * utility refuses 1X joins anyway. */
                if(capabilities.rsn&&capabilities.psk)
                    r.securityFlags|=RTW_CONTROL_SECURITY_WPA2_PSK_TKIP;
                else if(!capabilities.rsn&&(node->ni_rsnprotos&IEEE80211_PROTO_WPA)&&capabilities.psk)
                    r.securityFlags|=(node->ni_rsnciphers&IEEE80211_CIPHER_CCMP)
                        ?RTW_CONTROL_SECURITY_WPA_PSK_CCMP
                        :RTW_CONTROL_SECURITY_WPA_PSK;
                else if(capabilities.privacy&&!capabilities.rsn&&!(node->ni_rsnprotos&IEEE80211_PROTO_WPA))
                    r.securityFlags|=RTW_CONTROL_SECURITY_WEP;
            }
        }
        node=RB_NEXT(ieee80211_tree,&core_.sc_ic.ic_tree,node);
    }
    if(node&&visited>=1024)out->truncated=1;
}
/* Printable-prefix length of an SSID field. Fixed-width profile fields are
 * padded past the real name with NUL or control bytes (field capture:
 * "Network-Wifi\x01"). Same rule as the compat layer and the utility. */
static size_t printableSsidLen(const uint8_t* p,size_t n) {
    size_t len=0;
    if(!p)return 0;
    while(len<n&&p[len]>=0x20&&p[len]<=0x7e)++len;
    return len;
}
/* 1.0.15: the association tail of every JOIN - teardown, RX unstick,
 * profile seeding and station start. Behavior-identical to the original
 * inline block; kept as a method for one source of truth. */
IOReturn RTWUSBController::executeJoinTail(const RTWControlJoin *join) {
    /* 1.0.31: remember the target's channel from the live survey BEFORE the
     * quiesce below empties the node tree (strongest signal wins). */
    unsigned hintChannel=0;int hintRssi=-100000;unsigned alt24=0;int alt24Rssi=-1;
    /* mejoras: optional desired BSSID (RTWControlJoin.reserved). It is
     * only honored when the survey has seen that AP; otherwise the join goes
     * by SSID as before, so a vanished AP can never leave us scanning. */
    const uint8_t*wantBssid=reinterpret_cast<const uint8_t*>(join->reserved);
    const bool bssidRequested=wantBssid[6]==RTW_CONTROL_JOIN_BSSID_SET;
    unsigned desiredChannel=0;
    {
        unsigned visited=0;
        for(ieee80211_node*hn=RB_MIN(ieee80211_tree,&core_.sc_ic.ic_tree);hn&&visited++<1024;hn=RB_NEXT(ieee80211_tree,&core_.sc_ic.ic_tree,hn)){
            if(hn->ni_esslen!=join->ssidLength||!hn->ni_esslen||memcmp(hn->ni_essid,join->ssid,hn->ni_esslen))continue;
            if(!hn->ni_chan||hn->ni_chan==IEEE80211_CHAN_ANYC)continue;
            const unsigned c=(unsigned)ieee80211_chan2ieee(&core_.sc_ic,hn->ni_chan);
            if(bssidRequested&&c>=1&&c<255&&!memcmp(hn->ni_bssid,wantBssid,6))desiredChannel=c;
            {int score=(int)hn->ni_rssi-((core_.sc_ic.ic_bad_valid&&!memcmp(hn->ni_bssid,core_.sc_ic.ic_bad_bssid,6))?1000:0);if(c>=1&&c<255&&score>hintRssi){hintRssi=score;hintChannel=c;}
            if(c>=1&&c<255&&IEEE80211_IS_CHAN_2GHZ(hn->ni_chan)&&(int)hn->ni_rssi>alt24Rssi){alt24Rssi=(int)hn->ni_rssi;alt24=c;}}
        }
    }
    /* a re-submit after a failed join sees only the one channel that was scanned, so keep the earlier 2.4 GHz choice */
    if(alt24)core_.sc_ic.ic_join_alt24=(uint8_t)alt24;
    const bool pinBssid=bssidRequested&&desiredChannel;
    if(pinBssid)hintChannel=desiredChannel; /* mejoras: the AP the user picked */
    else if(core_.sc_ic.ic_avoid5&&core_.sc_ic.ic_join_alt24)hintChannel=core_.sc_ic.ic_join_alt24;
    if(bssidRequested)IOLog("RTWUSBController: join pinned to bssid %02x:%02x:%02x:%02x:%02x:%02x -> %s (ch %u)\n",
        wantBssid[0],wantBssid[1],wantBssid[2],wantBssid[3],wantBssid[4],wantBssid[5],
        pinBssid?"pinned":"not in survey, joining by SSID",desiredChannel);
    setProperty("RTWDiagJoinPinnedBssid",(unsigned long long)(pinBssid?1:0),32);
    setProperty("RTWDiagJoinHintChannel",(unsigned long long)hintChannel,32);
    setProperty("RTWDiagJoinAlt24Channel",(unsigned long long)alt24,32);
    const uint64_t tailStartMS=nowMS();
    for(auto&v:joinStateMS_)v=0;
    quiesce(true);
    setProperty("RTWDiagJoinQuiesceMS",(unsigned long long)(nowMS()-tailStartMS),64);
    /* 1.0.11: a re-join that lands after an off/on window inherits a
     * halted USB RX pipe; the fresh association then sees no beacons
     * and no EAPOL and dies quietly. Clear the halt before the station
     * restarts. */
    if(rxPipe_&&!rxRecovery_.pending){
        IOReturn unstuck=rxPipe_->clearStall(true);
        IOLog("RtWlanUCompat: fresh re-join RX pipe unstick -> 0x%08x\n",(unsigned)unstuck);
    }
    /* A zero PMK is the compat layer's encoding of an open/none association
     * (PBKDF2 output is never all-zero); an open join must actively DISARM
     * RSN so no stale WPA config survives. */
    bool openJoin=true;
    for(unsigned i=0;i<sizeof(join->pmk);++i)if(join->pmk[i]){openJoin=false;break;}
    if(openJoin){
        ieee80211_wpaparams off{};off.i_enabled=0;
        int error=ieee80211_ioctl_setwpaparms(&core_.sc_ic,&off);
        if(error&&error!=ENETRESET)return fromErrno(error);
        memset(core_.sc_ic.ic_psk,0,sizeof(core_.sc_ic.ic_psk));
        core_.sc_ic.ic_flags&=~IEEE80211_F_PSK;
    }else{
        ieee80211_wpaparams parameters{};parameters.i_enabled=1;parameters.i_protos=IEEE80211_WPA_PROTO_WPA2;parameters.i_akms=IEEE80211_WPA_AKM_PSK;parameters.i_ciphers=IEEE80211_WPA_CIPHER_CCMP;parameters.i_groupcipher=IEEE80211_WPA_CIPHER_CCMP;
        int error=ieee80211_ioctl_setwpaparms(&core_.sc_ic,&parameters);
        if(error&&error!=ENETRESET)return fromErrno(error);
        memcpy(core_.sc_ic.ic_psk,join->pmk,32);core_.sc_ic.ic_flags|=IEEE80211_F_PSK;
    }
    core_.sc_ic.ic_des_esslen=join->ssidLength;memcpy(core_.sc_ic.ic_des_essid,join->ssid,join->ssidLength);
    if(pinBssid){core_.sc_ic.ic_flags|=IEEE80211_F_DESBSSID;IEEE80211_ADDR_COPY(core_.sc_ic.ic_des_bssid,wantBssid);}
    else core_.sc_ic.ic_flags&=~(u_int32_t)IEEE80211_F_DESBSSID; /* mejoras */
    if(!joinRequested_)firstJoinRequestedMS_=nowMS();
    joinRequested_=true;
    lastJoinRequestedMS_=nowMS();
    wpaStandDown_=false;
    core_.sc_ic.ic_join_chan_hint=(uint8_t)hintChannel;
    const uint64_t enableStartMS=nowMS();
    IOReturn result=enableStation();
    setProperty("RTWDiagJoinEnableMS",(unsigned long long)(nowMS()-enableStartMS),64);if(result!=kIOReturnSuccess)quiesce(true);return result;
}
/* 1.0.16: the 1.0.15 cold-attach gate (considerColdGate + held-join
 * deferral) is REMOVED - it deadlocked with the app's scan suppression
 * (a held join suppressed the very sweeps the gate itself waited for) and
 * broke ultra-compatible dongles into endless scanning. */
IOReturn RTWUSBController::handleControl(ControlCall*call) {
    if(!call||rtw_control_validate_request(call->selector,call->input,call->length)!=RTW_CONTROL_VALID||!call->outputLength||!call->output||*call->outputLength<rtw_control_output_size(call->selector))return kIOReturnBadArgument;
    size_t size=rtw_control_output_size(call->selector);bzero(call->output,size);*call->outputLength=size;
    if(call->selector==RTW_CONTROL_STATUS){status(static_cast<RTWControlStatus*>(call->output));return kIOReturnSuccess;}
    if(call->selector==RTW_CONTROL_SCAN_RESULTS){scanResults(static_cast<RTWControlScanResults*>(call->output));return kIOReturnSuccess;}
    if(call->selector==RTW_CONTROL_CHANNEL_MAP){channelMap(static_cast<RTWControlChannelMap*>(call->output));return kIOReturnSuccess;}
    auto*reply=static_cast<RTWControlReply*>(call->output);reply->version=RTW_CONTROL_VERSION;
    if(call->selector==RTW_CONTROL_LEAVE){softLeave();return kIOReturnSuccess;}
    if(!attached_||!present_||sleeping_||usbError_!=kIOReturnSuccess||core_.core_error){
        considerSessionRescue();/* 1.0.9: a transport latched dead re-plugs itself */
        return kIOReturnNotReady;
    }
    if(call->selector==RTW_CONTROL_JOIN){
        const auto*join=static_cast<const RTWControlJoin*>(call->input);
        /* 1.0.15 shipped a cold-attach gate here that HELD the first join
         * until two sweeps were served; rolled back in 1.0.16 - it deadlocked
         * with the app's own "no scans while a join is in flight" rule (the
         * held join suppressed the very scans the gate was waiting for) and
         * left ultra-compatible dongles scanning forever. */
        /* Build 1.0.8: an idempotent re-join must not tear down a healthy
         * link. The utility re-sends the whole profile (its watchdog, a
         * saved-profile refresh, a second click); quiesce() below stopped
         * the station and erased the profile EVERY time, so a live RUN
         * association died on every re-send - the green -> rescan loop in
         * every field capture. Field evidence 2026-11-08 18:10: the dongle
         * sat in RUN on Network-Wifi (en2 held its DHCP lease) while the
         * re-join kept restarting it. While the station is already
         * authorized to the requested SSID (RUN, port valid, this exact
         * network), answer success and leave the link untouched. A
         * different SSID, an unauthorized station or a fresh join still
         * takes the full rejoin path below.
         * Build 1.0.12: "authorized" is judged against beacon freshness.
         * Beacons arrive ~100 ms apart and keep lastBeaconMs_ current; a
         * zombie RUN (link dead after an off/on, no beacon for seconds)
         * must fall through to the full rejoin instead of being swallowed.
         * Build 1.0.14: after an RF-off window the watchdog posts carrier
         * DOWN and macOS drops the DHCP lease; the station then recovers
         * on its own (scan -> auth -> assoc -> RUN -> 4-way), but nothing
         * re-posts kIONetworkLinkActive, so configd never re-runs DHCP and
         * Network preferences keeps the yellow dot until a manual click.
         * An idempotent join with the carrier still DOWN now posts the
         * link-up event instead of doing nothing. */
        if(authorized()){
            auto*live=core_.sc_ic.ic_bss;
            const size_t wantLen=printableSsidLen(join->ssid,
                join->ssidLength<=32?join->ssidLength:0);
            const size_t haveLen=printableSsidLen(live->ni_essid,
                live->ni_esslen<=32?live->ni_esslen:0);
            /* mejoras: a join pinned to ANOTHER AP of the same SSID
             * is a real move, never an idempotent re-join. */
            const uint8_t*pinRsv=reinterpret_cast<const uint8_t*>(join->reserved);
            const bool otherAp=pinRsv[6]==RTW_CONTROL_JOIN_BSSID_SET&&memcmp(live->ni_bssid,pinRsv,6)!=0;
            if(!otherAp&&wantLen&&wantLen==haveLen&&
               memcmp(live->ni_essid,join->ssid,wantLen)==0){
                char name[33]{};
                memcpy(name,join->ssid,wantLen<32?wantLen:32);
                const bool beaconsFresh=lastBeaconMs_&&nowMS()-lastBeaconMs_<8000;
                if(beaconsFresh&&core_.sc_ic.ic_if.if_link_state==LINK_STATE_UP&&dhcpStuck()){
                    /* 1.0.15: fresh beacons only prove the AP is alive, not
                     * that our data path is. A repeating unanswered DISCOVER
                     * burst means configd still has no lease; rebuild the
                     * link instead of swallowing the join. */
                    IOLog("RtWlanUCompat: join submit on %s - %llu DHCP "
                          "discovers unanswered while RUN, IPv4 data path "
                          "stuck - rebuilding link\n",name,
                          (unsigned long long)dhcpDiscovers_);
                    dhcpDiscovers_=0;
                }else if(beaconsFresh&&core_.sc_ic.ic_if.if_link_state==LINK_STATE_UP){
                    IOLog("RtWlanUCompat: join submit idempotent, already "
                          "authorized to %s - link healthy, untouched\n",name);
                    return kIOReturnSuccess;
                }else if(beaconsFresh){
                    IOLog("RtWlanUCompat: join submit idempotent on %s - "
                          "carrier was DOWN, posting link UP so configd "
                          "re-runs DHCP\n",name);
                    linkActivePosted_=false;
                    refreshLinkEvent();
                    return kIOReturnSuccess;
                }
                if(!beaconsFresh)IOLog("RtWlanUCompat: zombie RUN on %s - rebuilding link\n",name);
            }
        }
        return executeJoinTail(join);
    }
    if(call->selector==RTW_CONTROL_SCAN){
        if(rxRecovery_.pending)return kIOReturnBusy;
        /* Field round 12: a sweep while associated tears the RUN state down
         * (4 -> 1). The capture rtw-capture-20261108-190047 shows the cycle:
         * scan trigger -> link answer disconnected -> configd detaches the
         * interface -> rejoin ~2 s later, and the Realtek utility rescans
         * every few seconds with its menu open, so every click on the icon
         * dropped the connection. While linked, answer success and serve the
         * cached survey instead of sweeping; the radio only leaves the AP
         * when the association is already gone. */
        if(core_.sc_ic.ic_state==IEEE80211_S_RUN)return kIOReturnSuccess;
        /* 1.0.8 (field log 2026-11-18 07:03-07:05, driver 1.0.7 + app
         * 8.7.13): a scan trigger while a join request is still working
         * runs ieee80211_begin_scan below and restarts the whole STA
         * sweep, erasing any AUTH/ASSOC progress - the app's own rescan
         * landed 4 ms and 0.8 s after the join submit and the association
         * never converged ("join submit -> ok", then nothing, forever).
         * While a join is inside its working window, answer success and
         * serve the cached survey exactly like the RUN case: the join's
         * own site survey keeps the node tree current. A genuinely dead
         * join is retired by the app's bounded monitor, and scans flow
         * again once the 60 s window passes or a fresh join re-anchors
         * the clock. One line per boot marks guarded triggers. */
        if(joinRequested_&&lastJoinRequestedMS_&&
           nowMS()-lastJoinRequestedMS_<60000u){
            static bool scanGuardLogged=false;
            if(!scanGuardLogged){
                scanGuardLogged=true;
                IOLog("[macOS-RTL-WIFI 1.0.16] scan trigger %llu ms into a live join - cached survey served, the join owns the radio\n",
                    (unsigned long long)(nowMS()-lastJoinRequestedMS_));}
            return kIOReturnSuccess;
        }
        if(!running_)return enableStation();
        ieee80211_begin_scan(&core_.sc_ic.ic_if);
        return rtwn_io_error(&core_)?kIOReturnIOError:kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}
IOReturn RTWUSBController::gated(OSObject*object,void*operation,void*a,void*,void*) {
    auto*s=static_cast<RTWUSBController*>(object);
    if(s->closing_&&(uintptr_t)operation!=Shutdown&&(uintptr_t)operation!=TeardownReady){
        if((uintptr_t)operation==Output){auto*call=static_cast<OutputCall*>(a);call->consumed=true;if(call->packet)mbuf_freem(call->packet);return kIOReturnOutputDropped;}
        if((uintptr_t)operation==Disable||(uintptr_t)operation==Terminated)return kIOReturnSuccess;
        return kIOReturnNotReady;
    }
    switch((uintptr_t)operation){
    case Initialize:return s->initializePlatform();
    case Shutdown:s->shutdownPlatform();return kIOReturnSuccess;
    case TeardownReady:s->teardownReady_=true;s->scheduleDeferredCleanup();return kIOReturnSuccess;
    case SoftwareReplug:{
        /* Runs under the gate only to validate state and capture the device
         * provider; reset() itself must stay off-gate because it synchronously
         * terminates the device (and us with it - closeUSBProvider's reentry
         * rule). */
        auto*out=static_cast<IOUSBHostDevice**>(a);
        s->reenumQueued_=false;
        if(out)*out=nullptr;
        /* 1.0.6: normally only a never-started station qualifies (!running_),
         * but the boot rescue may also fire while the station runs as a
         * zombie - up, never authorized, inside the boot-rescue window. */
        bool rescueLive=s->bootRescueArmed_&&!s->bootEverAuthorized_;
        /* 1.0.9: a session transport fault (sessionFaultSinceMS_) also
         * qualifies even while the station bookkeeping still says running_. */
        if(out&&s->present_&&!s->stopping_&&!s->closing_&&s->usb_&&s->usb_->getDevice()&&
           (!s->running_||rescueLive||s->sessionFaultSinceMS_))
            if(auto*dev=OSDynamicCast(IOUSBHostDevice,s->usb_->getDevice())){dev->retain();*out=dev;}
        return kIOReturnSuccess;
    }
    case Enable:return s->enableStation();
    case Disable:
        /* 1.0.28: opening System Settings > Network makes macOS disable/enable the interface. A full quiesce tore down a working link (LED blink, app 'ready, not connected', Forget needed). While associated and awake, ignore it; enable() is a no-op when already running. */
        /* 1.1.1: also ignore it while a join is still in progress (scan/auth/assoc/4-way): opening System Settings right after connecting reached quiesce(true) and dropped the link. */
        if(s->attached_&&s->enabled_&&s->running_&&!s->sleeping_&&(s->core_.sc_ic.ic_state==IEEE80211_S_RUN||s->joinRequested_)){++s->disableIgnored_;return kIOReturnSuccess;}
        s->quiesce(true);return kIOReturnSuccess;
    case Output:{
        auto*call=static_cast<OutputCall*>(a);mbuf_t packet=call->packet;
        call->consumed=true;
        /* 1.0.15: sniff DHCP DISCOVERs before the drop gates. configd only
         * keeps re-sending DISCOVER while it has no lease, so a repeating
         * burst proves the IPv4 data path is stuck even when the 802.11
         * layer looks perfectly healthy (field capture 2026-11-09 06:11:
         * RUN + fresh beacons + en5 parked at 169.254.245.61). */
        if(packet)s->noteTxDhcpDiscover(packet);
        if(s->stopping_||!s->authorized()||!packet){
            if(packet){
                ++s->txLinkDrops_;
                if(!s->authorized()){ /* 1.0.17: silent blackholes must show */
                    const uint64_t n=nowMS();
                    if(!s->lastTxGateLogMs_||n-s->lastTxGateLogMs_>=10000){
                        s->lastTxGateLogMs_=n;
                        IOLog("RtWlanUCompat: output dropped - station not "
                              "authorized (total %llu)\n",
                              (unsigned long long)s->txLinkDrops_);
                    }
                }
                mbuf_freem(packet);
            }
            return kIOReturnOutputDropped;
        }
        auto*queue=s->core_.sc_ic.ic_if.if_snd.queue;
        if(!queue){
            mbuf_freem(packet);++s->txErrors_;++s->txQueueDrops_;
            if(s->stats_)++s->stats_->outputErrors;
            return kIOReturnOutputDropped;
        }
        if(!queue->lockEnqueue(packet)){
            // Retry returns this untouched mbuf to IOGatedOutputQueue. The
            // completion path services that queue after inner capacity opens.
            call->consumed=false;return kIOReturnOutputStall;
        }
        rtwn_start(&s->core_.sc_ic.ic_if);return kIOReturnOutputSuccess;
    }
    case Control:return s->handleControl(static_cast<ControlCall*>(a));
    case Terminated:
        // Reject new work before quiescing; stop() may follow synchronously
        // when our provider is closed by the termination notification.
        s->closing_=true;s->present_=false;
        s->fault(kIOReturnNoDevice,11,0);s->quiesce(true);
        return kIOReturnSuccess;
    case Sleep:s->quiesce(true);s->sleeping_=true;return kIOReturnSuccess;
    case Wake:s->sleeping_=false;/* The app restores a saved profile from the user's keychain. */return kIOReturnSuccess;
    default:return kIOReturnBadArgument;
    }
}
IOCommandGate *RTWUSBController::copyGate(IOWorkLoop **loop) {
    *loop=nullptr;if(!lifecycleLock_)return nullptr;
    IOLockLock(lifecycleLock_);IOCommandGate*gate=gate_;
    if(gate&&loop_){gate->retain();*loop=loop_;(*loop)->retain();}else gate=nullptr;
    IOLockUnlock(lifecycleLock_);return gate;
}
IOReturn RTWUSBController::enable(IONetworkInterface*interface) {
    IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);if(!gate||interface!=netif_){if(gate)gate->release();if(lease)lease->release();return kIOReturnNotReady;}
    IOReturn result=gate->runAction(gated,(void*)Enable);gate->release();lease->release();return result;
}
IOReturn RTWUSBController::disable(IONetworkInterface*) {
    IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);if(!gate)return kIOReturnNotReady;
    IOReturn result=gate->runAction(gated,(void*)Disable);gate->release();lease->release();return result;
}
UInt32 RTWUSBController::outputPacket(mbuf_t packet,void*) {
    IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);if(!gate){if(packet)mbuf_freem(packet);return kIOReturnOutputDropped;}
    OutputCall call{packet,false};
    IOReturn result=gate->runAction(gated,(void*)Output,&call);gate->release();lease->release();
    if(result==kIOReturnOutputStall&&!call.consumed)return kIOReturnOutputStall;
    if(!call.consumed){if(packet)mbuf_freem(packet);return kIOReturnOutputDropped;}
    return result==kIOReturnOutputSuccess?kIOReturnOutputSuccess:kIOReturnOutputDropped;
}
IOReturn RTWUSBController::controlRequest(uint32_t selector,const void*input,size_t length,void*output,size_t*outputLength) {
    IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);if(!gate||isInactive()){if(gate)gate->release();if(lease)lease->release();return kIOReturnNotReady;}
    ControlCall call{selector,input,length,output,outputLength};IOReturn result=gate->runAction(gated,(void*)Control,&call);gate->release();lease->release();return result;
}

IOReturn RTWUSBController::newUserClient(task_t task,void*security,UInt32 type,OSDictionary*properties,IOUserClient**handler) {
    if(!handler)return kIOReturnBadArgument;*handler=nullptr;if(isInactive()||!attached_)return kIOReturnNotReady;
    auto*client=new RTWUserClient;if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnNotPrivileged;}
    if(!client->attach(this)){client->release();return kIOReturnError;}
    if(!client->start(this)){client->detach(this);client->release();return kIOReturnError;}
    *handler=client;return kIOReturnSuccess;
}
IOReturn RTWUSBController::message(UInt32 type,IOService*provider,void*argument) {
    if((type==kIOMessageServiceIsTerminated||type==kIOMessageServiceIsRequestingClose)&&(provider==usb_||is8192DUUSBProvider(provider))){
        // IOService does not call stop() on an opened client until that client
        // closes the terminated provider. Keep ourselves alive across close():
        // it can synchronously invoke stop() and trigger last-reference teardown.
        retain();
        IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);
        bool quiesced=false;
        if(gate){
            quiesced=gate->runAction(gated,(void*)Terminated)==kIOReturnSuccess;
            gate->release();lease->release();
        }
        IOReturn result=super::message(type,provider,argument);
        // Never close the provider under our workloop gate: close() may run
        // stop(), whose shutdown path must acquire that same gate.
        if(quiesced){closeUSBProvider();close8192DUProviders();}
        // When the gate is unavailable, releasePlatform() owns the fallback
        // close. Do not close before outstanding work has been quiesced.
        release();
        return result;
    }
    return super::message(type,provider,argument);
}
IOReturn RTWUSBController::setPowerState(unsigned long state,IOService*) {
    IOWorkLoop*lease=nullptr;auto*gate=copyGate(&lease);if(gate){gate->runAction(gated,(void*)(state?Wake:Sleep));gate->release();lease->release();}return kIOPMAckImplied;
}
void RTWUSBController::reportStartStage(const char *stage, IOReturn result) {
    // Preserve the last start stage on the USB provider even if start fails
    // and IOKit destroys this controller. No SSID or key material is logged.
    setProperty("RTWStartStage",stage);
    setProperty("RTWStartResult",(unsigned long long)(uint32_t)result,32);
    if(auto *provider=getProvider()){
        provider->setProperty("RTWStartStage",stage);
        provider->setProperty("RTWStartResult",(unsigned long long)(uint32_t)result,32);
    }
    IOLog("[macOS-RTL-WIFI 1.0.16] start stage=%s result=0x%08x\n",stage,(unsigned)result);
}
bool RTWUSBController::start(IOService*provider) {
    IOLog("RtWlanUCompat: compat shim build R28-monterey-vtable (2026-11-11) active, Darwin %d.%d\n",version_major,version_minor);
    reportStartStage("entry");
    IOReturn runtimeResult=RTWNet80211Acquire(&loop_);
    if(runtimeResult!=kIOReturnSuccess){reportStartStage("stack-runtime",runtimeResult);return false;}runtimeLease_=true;
    lifecycleLock_=IOLockAlloc();completionLock_=IOSimpleLockAlloc();
    // The priority-only allocator is exported by the private KPI on macOS 26.
    // The public Mach allocator preserves the same priority and zero options.
    cleanupCall_=thread_call_allocate_with_options(deferredCleanup,this,THREAD_CALL_PRIORITY_KERNEL,0);
    reenumCall_=thread_call_allocate_with_options(reenumerateCall,this,THREAD_CALL_PRIORITY_KERNEL,0);
    if(!lifecycleLock_||!loop_||!completionLock_||!cleanupCall_||!reenumCall_){reportStartStage("locks-workloop",kIOReturnNoMemory);releasePlatform();return false;}
    if(!super::start(provider)){reportStartStage("network-super-start",kIOReturnError);releasePlatform();return false;}
    usb_=OSDynamicCast(IOUSBHostInterface,provider);
    if(!usb_||!usb_->open(this)){reportStartStage("usb-open",kIOReturnNotOpen);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    usbOpened_=present_=true;
    gate_=IOCommandGate::commandGate(this);completionEvent_=IOInterruptEventSource::interruptEventSource(this,completionAction);
    scanTimer_=IOTimerEventSource::timerEventSource(this,scanAction);calibTimer_=IOTimerEventSource::timerEventSource(this,calibAction);watchdogTimer_=IOTimerEventSource::timerEventSource(this,watchdogAction);
    rescueTimer_=IOTimerEventSource::timerEventSource(this,bootRescueTick);
    if(!gate_||!completionEvent_||!scanTimer_||!calibTimer_||!watchdogTimer_||!rescueTimer_||loop_->addEventSource(gate_)!=kIOReturnSuccess||loop_->addEventSource(completionEvent_)!=kIOReturnSuccess||loop_->addEventSource(scanTimer_)!=kIOReturnSuccess||loop_->addEventSource(calibTimer_)!=kIOReturnSuccess||loop_->addEventSource(watchdogTimer_)!=kIOReturnSuccess||loop_->addEventSource(rescueTimer_)!=kIOReturnSuccess){reportStartStage("event-sources",kIOReturnNoResources);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    completionEvent_->enable();rtw_transfer_reset(&ledger_);
    if(!taskq_is_initialized(systq)){reportStartStage("taskqueue",kIOReturnNoResources);gate_->runAction(gated,(void*)Shutdown);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    if(gate_->runAction(gated,(void*)Initialize)!=kIOReturnSuccess){gate_->runAction(gated,(void*)Shutdown);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    OSDictionary*media=OSDictionary::withCapacity(1);IONetworkMedium*automatic=IONetworkMedium::medium(kIOMediumEthernetAuto,0);
    bool mediumOK=media&&automatic&&IONetworkMedium::addMedium(media,automatic)&&publishMediumDictionary(media)&&setSelectedMedium(automatic);
    if(automatic)automatic->release();if(media)media->release();
    IONetworkInterface*interface=nullptr;
    if(!mediumOK||!attachInterface(&interface,false)){reportStartStage("ethernet-attach",kIOReturnError);gate_->runAction(gated,(void*)Shutdown);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    netif_=OSDynamicCast(IOEthernetInterface,interface);
    if(!netif_){reportStartStage("ethernet-type",kIOReturnUnsupported);detachInterface(interface,true);interface->release();gate_->runAction(gated,(void*)Shutdown);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    static IOPMPowerState states[2]{};states[0].version=1;states[1].version=1;states[1].capabilityFlags=kIOPMPowerOn|kIOPMDeviceUsable;states[1].inputPowerRequirement=kIOPMPowerOn;states[1].outputPowerCharacter=kIOPMPowerOn;
    PMinit();powerManaged_=true;provider->joinPMtree(this);
    if(registerPowerDriver(this,states,2)!=kIOReturnSuccess){reportStartStage("power-register",kIOReturnError);PMstop();powerManaged_=false;gate_->runAction(gated,(void*)Shutdown);releasePlatform();super::stop(provider);markTeardownReady();return false;}
    super::setLinkStatus(kIONetworkLinkValid);reportStartStage("ready");registerService();netif_->registerService();
    /* 1.0.6 boot rescue: from here the attach has 12 minutes to prove
     * itself. The first tick waits 15 s so a clean boot (configd enabling
     * the interface + a fast association) always beats the rescue. */
    bootAttachCompleteMS_=nowMS();bootRescueArmed_=true;
    rescueTimer_->setTimeoutMS(15000);
    return true;
}
void RTWUSBController::shutdownPlatform() {
    closing_=true;quiesce(true);
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);bool drained=rtw_transfer_drained(&ledger_);IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!drained){if(!retainedForPending_)retain();retainedForPending_=true;IOLog("[macOS-RTL-WIFI] USB abort has not drained; retaining driver resources to prevent unsafe teardown\n");return;}
    if(attached_){rtwn_detach(&core_,0);attached_=false;ether_ifdetach(&core_.sc_ic.ic_if);}
    /* Detach the Ethernet child outside our gate: its synchronous disable
     * callback may run on another networking thread and acquire this gate. */
    macReady_=false;
}
void RTWUSBController::closeUSBProvider() {
    IOUSBHostInterface*provider=nullptr;
    // close() may synchronously reenter stop(). Claim the open under a lock
    // *before* calling it, so releasePlatform() cannot close it again.
    if(lifecycleLock_)IOLockLock(lifecycleLock_);
    if(usb_&&usbOpened_){
        provider=usb_;
        usbOpened_=false;
        provider->retain();
    }
    if(lifecycleLock_)IOLockUnlock(lifecycleLock_);
    if(provider){provider->close(this);provider->release();}
}
void RTWUSBController::releasePlatform() {
    if(retainedForPending_)return;
    if(completionLock_)freeTransfers();
    if(cleanupCall_){thread_call_free(cleanupCall_);cleanupCall_=nullptr;}
    /* 1.0.6: a queued re-plug call is either cancelled before it fires or,
     * if already running, held alive by the schedule retain until its own
     * release balances it; the body exits on a null gate after teardown. */
    if(reenumCall_){thread_call_cancel(reenumCall_);thread_call_free(reenumCall_);reenumCall_=nullptr;}
    if(netif_){detachInterface(netif_,true);netif_->release();netif_=nullptr;}stats_=nullptr;
    IOCommandGate *oldGate=gate_;
    if(lifecycleLock_){IOLockLock(lifecycleLock_);oldGate=gate_;gate_=nullptr;IOLockUnlock(lifecycleLock_);}
    if(loop_){
        IOEventSource*events[]={scanTimer_,calibTimer_,watchdogTimer_,rescueTimer_,completionEvent_,oldGate};
        for(auto*event:events)if(event){if(event!=oldGate)event->disable();if(event->getWorkLoop()==loop_)loop_->removeEventSource(event);event->release();}
    }
    scanTimer_=calibTimer_=watchdogTimer_=nullptr;rescueTimer_=nullptr;completionEvent_=nullptr;gate_=nullptr;
    if(rxPipe_){rxPipe_->release();rxPipe_=nullptr;}for(auto*&p:txPipes_)if(p){p->release();p=nullptr;}
    closeUSBProvider();
    if(lifecycleLock_)IOLockLock(lifecycleLock_);
    usb_=nullptr;usbOpened_=present_=false;
    if(lifecycleLock_)IOLockUnlock(lifecycleLock_);
    if(completionLock_){IOSimpleLockFree(completionLock_);completionLock_=nullptr;}
    if(loop_){loop_->release();loop_=nullptr;}
    if(acPHYStorage_){IOFree(acPHYStorage_,acPHYSize_);acPHYStorage_=nullptr;acPHYSize_=0;acCore_.phy=nullptr;}
    release8723Storage();
    release8710BUStorage();
    release8192FUStorage();
    release8192DUStorage();
    release88Storage();
    release8812Storage();
    if(runtimeLease_){runtimeLease_=false;RTWNet80211Release();}
}
void RTWUSBController::stop(IOService*provider) {
    if(powerManaged_){PMstop();powerManaged_=false;}
    if(gate_)gate_->runAction(gated,(void*)Shutdown);
    releasePlatform();super::stop(provider);markTeardownReady();
}
void RTWUSBController::free() {releasePlatform();if(lifecycleLock_){IOLockFree(lifecycleLock_);lifecycleLock_=nullptr;}super::free();}
#include "RTW8814Bus.inc"
#include "RTW8814Backend.inc"
#include "RTW8812Backend.inc"
#include "RTW88Backend.inc"
#include "RTW8192DUBackend.inc"
#include "RTW8192FUBackend.inc"
#include "RTW8710BUBackend.inc"
#include "RTW8723BUBackend.inc"

// Called while the shared WorkLoop is gated. Never free a WorkLoop, an
// Ethernet child or a USB descriptor from its own completion callback.
void RTWUSBController::scheduleDeferredCleanup() {
    if(!retainedForPending_||!teardownReady_||cleanupQueued_||!cleanupCall_||!completionLock_)return;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    const bool drained=rtw_transfer_drained(&ledger_);
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!drained)return;
    cleanupQueued_=true;
    thread_call_enter(cleanupCall_);
}
void RTWUSBController::markTeardownReady() {
    IOWorkLoop* lease=nullptr;auto* gate=copyGate(&lease);
    if(gate){gate->runAction(gated,(void*)TeardownReady);gate->release();lease->release();}
}
void RTWUSBController::deferredCleanup(thread_call_param_t argument,thread_call_param_t) {
    auto* controller=static_cast<RTWUSBController*>(argument);
    IOWorkLoop* lease=nullptr;auto* gate=controller->copyGate(&lease);
    if(!gate)return; // The deferred retain keeps all resources until a gate exists.
    gate->runAction(gated,(void*)Shutdown);
    // The worker cannot run until drainCompletions has returned. No new
    // submissions are possible after closing_, and schedule checked drained.
    controller->retainedForPending_=false;
    gate->release();lease->release();
    controller->releasePlatform();
    controller->release(); // Balance shutdownPlatform's deferred retain exactly once.
}
/* ---- 1.0.5/1.0.6 software re-plug (the unplug/replug cure, in software) ----
 * 1.0.6 re-gating: the 1.0.5 ladder only counted failures that reached the
 * chip-start path, and disarmed itself once a bring-up "succeeded". The
 * reboot-2 field wedge defeats both: the first failed bring-up latches
 * usbError_ so every later attempt exits before counting, and a station
 * that comes up as a zombie never authorizes - the cure stayed silent.
 * The guards below now key on the boot-rescue state instead: armed at
 * start(), disarmed by the first live carrier (bootEverAuthorized_) and
 * by the 12-minute window, with the same budgets as before. */
void RTWUSBController::scheduleSoftwareReplug(const char *why,bool timeBased) {
    if(!reenumCall_||!completionLock_||closing_||stopping_)return;
    /* 1.0.34: the 8821CU family never re-enumerates itself. The old kexts do
     * not reset these dongles and they stay on the bus after a reboot; our
     * software re-plug made them vanish ("no Realtek dongle visible"). */
    if(family_==RTW_WIFI_RTL8821CU)return;
    /* 1.0.6: never replug a session the user has seen working, and never
     * outside the boot-rescue window. */
    if(bootEverAuthorized_||!bootRescueArmed_)return;
    if(nowMS()-bootAttachCompleteMS_>=RTW_BOOT_RESCUE_WINDOW_MS)return;
    if(reenumFires_>=3)return;              /* per-attach budget */
    if(g_softwareReplugsSinceBoot>=6)return;/* per-boot budget */
    if(!timeBased){const uint32_t need=4u<<reenumFires_;if(wedgedBringUps_<need)return;} /* 4th, 8th, 16th failure */
    if(g_lastSoftwareReplugMS&&nowMS()-g_lastSoftwareReplugMS<20000)return;
    bool fire=false;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    if(!reenumQueued_){reenumQueued_=true;fire=true;}
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!fire)return;
    g_lastSoftwareReplugMS=nowMS();
    ++g_softwareReplugsSinceBoot;
    ++reenumFires_;
    IOLog("[macOS-RTL-WIFI 1.0.16] boot rescue (%s): software re-plug %u/3 (USB re-enumeration, %u/6 used this boot)\n",
          why,(unsigned)reenumFires_,(unsigned)g_softwareReplugsSinceBoot);
    retain(); /* reenumerateCall releases exactly once. */
    thread_call_enter(reenumCall_);
}
/* 1.0.9 session rescue (RTLWiFiBar.log 2026-11-18 07:42-07:48, driver
 * 1.0.8): the DHCP auto-heal's restart latched usbError_ mid-session, and
 * the boot rescue refused to fire because this session had already seen a
 * working link (bootEverAuthorized_) - the dongle sat dead, 0xe00002d8 on
 * every scan, client reopens useless, for 6+ minutes until a physical
 * replug cured it. The app's 1 Hz status poll lands in the NotReady guard
 * of handleControl while the transport is latched: this tick measures how
 * long the fault has persisted and re-plugs on its own. Budget: 20 s of a
 * confirmed dead transport, max 2 per attach, and the boot rescue's global
 * per-boot ceiling and 20 s spacing still apply. The re-plug itself is the
 * proven boot path (IOUSBHostDevice::reset() off-gate); the app already
 * rides out the re-enumeration with its client-reopen logic. */
void RTWUSBController::considerSessionRescue() {
    if(!reenumCall_||!completionLock_)return;
    if(family_==RTW_WIFI_RTL8821CU)return; /* 1.0.34: no self re-plug on 8821CU */
    if(!attached_||!present_||sleeping_||stopping_||closing_)return;
    if(usbError_==kIOReturnSuccess){sessionFaultSinceMS_=0;return;}
    const uint64_t now=nowMS();
    if(!sessionFaultSinceMS_){
        sessionFaultSinceMS_=now;
        IOLog("[macOS-RTL-WIFI 1.0.16] USB transport fault latched - session rescue armed (re-plug after 20 s dead, max 2)\n");
        return;
    }
    if(now-sessionFaultSinceMS_<20000)return;   /* the fault must persist */
    if(sessionReplugFires_>=2)return;           /* per-attach budget */
    if(g_softwareReplugsSinceBoot>=10)return;   /* global per-boot ceiling */
    if(g_lastSoftwareReplugMS&&now-g_lastSoftwareReplugMS<20000)return;
    bool fire=false;
    IOInterruptState lock=IOSimpleLockLockDisableInterrupt(completionLock_);
    if(!reenumQueued_){reenumQueued_=true;fire=true;}
    IOSimpleLockUnlockEnableInterrupt(completionLock_,lock);
    if(!fire)return;
    ++sessionReplugFires_;
    g_lastSoftwareReplugMS=now;
    ++g_softwareReplugsSinceBoot;
    IOLog("[macOS-RTL-WIFI 1.0.16] session rescue (%u/2): USB transport dead %llu s - re-enumerating the dongle by itself\n",
          (unsigned)sessionReplugFires_,
          (unsigned long long)((now-sessionFaultSinceMS_)/1000));
    retain(); /* reenumerateCall releases exactly once. */
    thread_call_enter(reenumCall_);
}
/* 1.0.6 boot rescue: a watchdog that works even while the station never
 * started (watchdogAction exits early while !running_) and that also
 * catches the zombie shape (station up, radio dead, join never
 * authorized). Runs on the shared workloop; every field it reads is
 * written under the same workloop, so no extra locking is needed. */
void RTWUSBController::bootRescueTick(OSObject *o,IOTimerEventSource*) {
    auto*s=static_cast<RTWUSBController*>(o);
    if(!s->rescueTimer_||s->stopping_||s->closing_||s->sleeping_)return;
    const uint64_t now=nowMS();
    /* 1.0.11: do NOT cancel when bootEverAuthorized_ - a flappy association
     * (brief RUN then drop) must stay rescuable. The timer keeps ticking;
     * the flappy condition below handles the post-RUN drop. */
    if(now-s->bootAttachCompleteMS_>=RTW_BOOT_RESCUE_WINDOW_MS){s->rescueTimer_->cancelTimeout();return;}/* window over */
    const char*why=nullptr;
    if(s->bringUpAttempts_&&!s->bringUpEverSucceeded_&&now-s->bootAttachCompleteMS_>=45000)
        why="chip bring-up never succeeded";
    /* 1.0.13 unified fast rescue (field logs 2026-11-19 07:07 / 22:02 /
     * 22:39 / 23:01, drivers 1.0.9-1.0.12): the post-reboot AUTH cycle
     * comes in two shapes - "never authorized" and "brief RUN flaps" - and
     * the split detectors kept failing each other: 1.0.11's flappy clock
     * (90 s after lastRunMs_) SLID every time a re-submit reached another
     * 1-second RUN (field 23:05: fire at +184 s instead of +92 s), and the
     * 1.0.12 split re-created a deadlock between the branches and the
     * bootEverAuthorized_ guard. ONE detector, ONE clock, no sliding: a
     * join that reached AUTH at least once and is still unresolved 60 s
     * after the FIRST submit of this attach gets a software re-plug,
     * whether or not a brief RUN happened in between (a brief RUN during
     * an unresolved 60 s join is pathology, not progress). If a brief RUN
     * set bootEverAuthorized_, clear it so the re-plug guard passes. 60 s
     * chosen against the app's own watch: healthy joins land in 3-9 s and
     * the app re-submits at 46 s - by 60 s attempt 2 is already failing,
     * so the re-plug lands mid-watch and the app's own re-submit (or the
     * 8.7.18 post-rescue re-arm) connects on the fresh instance.
     * End-to-end recovery target: ~90-110 s. Budgets unchanged: 3 per
     * attach, 6 per boot, 20 s spacing, 12-minute window, never while RUN. */
    else if(s->bringUpEverSucceeded_&&s->joinRequested_&&s->everReachedAuth_&&
            s->firstJoinRequestedMS_&&now-s->firstJoinRequestedMS_>=60000&&
            s->core_.sc_ic.ic_state!=IEEE80211_S_RUN){
        const bool flapped=s->bootEverAuthorized_;
        s->bootEverAuthorized_=false; /* a brief RUN inside an unresolved join is not a proven-good session */
        why=flapped?"link flapped (brief RUN) and the join is still unresolved 60 s in"
                  :"station up but the requested join never authorized";
    }
    if(why&&!s->rescueWhyLogged_){s->rescueWhyLogged_=true;
        IOLog("[macOS-RTL-WIFI 1.0.16] boot rescue: %s - scheduling software re-plug\n",why);}
    if(why)s->scheduleSoftwareReplug(why,true);
    s->rescueTimer_->setTimeoutMS(5000);
}
void RTWUSBController::reenumerateCall(thread_call_param_t argument,thread_call_param_t) {
    auto* controller=static_cast<RTWUSBController*>(argument);
    IOUSBHostDevice* device=nullptr;
    IOWorkLoop* lease=nullptr;auto* gate=controller->copyGate(&lease);
    if(gate){gate->runAction(gated,(void*)SoftwareReplug,&device);gate->release();lease->release();}
    if(device){
        IOLog("[macOS-RTL-WIFI 1.0.16] re-enumerating the USB device now (same effect as unplug/replug)\n");
        /* IOUSBHostDevice::reset() - Apple's documented replacement for the
         * legacy IOUSBDevice::ReEnumerateDevice (SDK migration table):
         * terminate the device, reset the hub port, re-enumerate, publish a
         * fresh device for matching. Declared on every SDK from 10.11 through
         * macOS 26, and called off the USB port workloop here as required. */
        IOReturn r=device->reset();
        IOLog("[macOS-RTL-WIFI 1.0.16] software re-plug -> 0x%08x\n",(unsigned)r);
        device->release(); /* Balanced by the retain inside SoftwareReplug. */
    }
    controller->release(); /* Balanced by the schedule retain. */
}
