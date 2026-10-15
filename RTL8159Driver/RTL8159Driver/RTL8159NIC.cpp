// RTL8159NIC.cpp
// Personality B: USB Ethernet NIC
//
// Provider is IOUSBHostInterface (Config 1, bInterfaceClass=255, bInterfaceNumber=0).
// Config 1 is already pinned by Personality A (RTL8159Driver).
//
// Config 1 endpoints (USB 3.2 Gen 2, 10 Gbps):
//   EP1 IN  Bulk      0x81  1024B  — RX (bMaxBurst=6, alt-setting 0)
//   EP2 OUT Bulk      0x02  1024B  — TX (bMaxBurst=6, alt-setting 0)
//   EP3 IN  Interrupt 0x83  16B    — link status (wBytesPerInterval=2)
//
// Config 1 has 4 alternate settings (0–3) varying bMaxBurst/MaxStreams on bulk EPs.
// AltSetting 0 (bMaxBurst=6, no streams) is used for the initial scaffold.
//
// Interrupt pipe (EP3): wMaxPacketSize=16B, wBytesPerInterval=2.
// Allocate 16-byte buffer; only bytes [0:1] carry meaningful payload.
// Bit 0 = INTR_LINK. Actual speed/duplex read from OCP PHYSTATUS after interrupt.
//
// Register access: OCP model — bRequest=0x05, wValue=reg addr, wIndex=MCU type.
//   PLA (packet/MAC/PHY): MCU_TYPE_PLA = 0x0100
//   USB (USB controller): MCU_TYPE_USB = 0x0000
// See Common/RTLOcp.h.
//
// Init: stub — implement from Linux r8152.c r8156_init() / r8159_hw_phy_cfg().
// TX/RX framing: rtl rx_desc/tx_desc structs — see Common/RTLDesc.h.

#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>
#include <NetworkingDriverKit/NetworkingDriverKit.h>

#include "RTL8159NIC.h"
#include "RTLOcp.h"
#include "RTLDesc.h"
#include "RTLRegs.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "RTL8159-NIC [" __DATE__ " " __TIME__ "] " fmt, ##__VA_ARGS__)

// Config 1 endpoint addresses
#define EP_RX   0x81   // EP1 IN  Bulk 1024B      — receive
#define EP_TX   0x02   // EP2 OUT Bulk 1024B      — transmit
#define EP_ITR  0x83   // EP3 IN  Interrupt 16B   — link status

// RX ring — outstanding USB bulk IN transfers, each 64KB
#define RX_SLOTS        10
#define RX_BUF_SIZE     0x10000

// Interrupt pipe buffer (EP3 wMaxPacketSize=16; only bytes [0:1] meaningful)
#define ITR_BUF_SIZE    16

// INTR_LINK bit in the 2-byte interrupt status word (Linux r8152.c)
#define INTR_LINK       (1u << 0)

struct RTL8159NIC_IVars {
    IODispatchQueue                    *queue;
    IODispatchQueue                    *asyncQueue;
    IOUserNetworkPacketBufferPool      *pool;
    IOUserNetworkTxSubmissionQueue     *txsQueue;
    IOUserNetworkTxCompletionQueue     *txcQueue;
    IOUserNetworkRxSubmissionQueue     *rxsQueue;
    IOUserNetworkRxCompletionQueue     *rxcQueue;
    IOUSBHostDevice                    *device;
    IOUSBHostInterface                 *interface;
    IOUSBHostPipe                      *pipeRx;
    IOUSBHostPipe                      *pipeTx;
    IOUSBHostPipe                      *pipeItr;
    IOBufferMemoryDescriptor           *rxBufs[RX_SLOTS];
    OSAction                           *rxActions[RX_SLOTS];
    IOBufferMemoryDescriptor           *itrBuf;
    OSAction                           *itrAction;
    IOBufferMemoryDescriptor           *txBuf;
    OSAction                           *txPacketAction;
    OSAction                           *txCompleteAction;
    IOUserNetworkPacket                *txInFlight;
    IOTimerDispatchSource              *timerTest;
    OSAction                           *timerAction;
    IOUserNetworkMACAddress             macAddress;
    bool                                lastLinkUp;
    bool                                interfaceEnabled;
    bool                                rxStarted;
    bool                                ioArmed;
    bool                                txBusy;
};

// ---------------------------------------------------------------------------
// OCP helpers

static kern_return_t
ocpRead(IOUSBHostInterface *iface, uint16_t mcuType, uint16_t regAddr, void *data, uint16_t len)
{
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, len, 0, &buf);
    if (ret != kIOReturnSuccess) return ret;
    buf->SetLength(len);

    uint16_t transferred = 0;
    ret = iface->DeviceRequest(0xC0, RTL_REQ_GET_REGS, regAddr, mcuType, len, buf, &transferred, 500);
    if (ret == kIOReturnSuccess && transferred == len) {
        IOAddressSegment range;
        buf->GetAddressRange(&range);
        memcpy(data, (const void *)range.address, len);
    }
    OSSafeReleaseNULL(buf);
    return ret;
}

static kern_return_t
ocpWrite(IOUSBHostInterface *iface, uint16_t mcuType, uint16_t regAddr, const void *data, uint16_t len)
{
    IOBufferMemoryDescriptor *buf = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, len, 0, &buf);
    if (ret != kIOReturnSuccess) return ret;

    IOAddressSegment range;
    buf->GetAddressRange(&range);
    memcpy((void *)range.address, data, len);
    buf->SetLength(len);

    uint16_t transferred = 0;
    ret = iface->DeviceRequest(0x40, RTL_REQ_SET_REGS, regAddr, mcuType, len, buf, &transferred, 500);
    OSSafeReleaseNULL(buf);
    return ret;
}

static kern_return_t
rtlReadMACAddress(IOUSBHostInterface *iface, IOUserNetworkMACAddress *out)
{
    uint32_t lo = 0;
    uint16_t hi = 0;
    kern_return_t ret;

    ret = ocpRead(iface, MCU_TYPE_PLA, PLA_IDR, &lo, 4);
    if (ret != kIOReturnSuccess) return ret;
    ret = ocpRead(iface, MCU_TYPE_PLA, PLA_IDR + 4, &hi, 2);
    if (ret != kIOReturnSuccess) return ret;

    out->octet[0] = (lo >>  0) & 0xFF;
    out->octet[1] = (lo >>  8) & 0xFF;
    out->octet[2] = (lo >> 16) & 0xFF;
    out->octet[3] = (lo >> 24) & 0xFF;
    out->octet[4] = (hi >>  0) & 0xFF;
    out->octet[5] = (hi >>  8) & 0xFF;
    return kIOReturnSuccess;
}

static bool
rtlQueryLinkState(IOUSBHostInterface *iface, IOUserNetworkMediaType *outMedia)
{
    // STUB — read PHYSTATUS via OCP to determine link up/speed.
    // Linux: r8152_get_speed() reads PHYSTATUS via OCP_REG_PHYSTATUS.
    (void)iface;
    *outMedia = kIOUserNetworkMediaEthernetNone;
    Log("rtlQueryLinkState: STUB — returning link down");
    return false;
}

static void
hwInit(IOUSBHostInterface *iface)
{
    // TODO: implement from Linux r8152.c r8156_init() / r8159_hw_phy_cfg():
    //   1. DisableU1U2 / DisableLTM (BOS confirms LTM supported — device accepts disable)
    //   2. Wait for autoload
    //   3. Clear BMCR power-down
    //   4. Wait for PHY_STAT_LAN_ON
    //   5. USB queue / power config (USB_TX_AGG, USB_RX_BUF_TH, etc.)
    //   6. Flow control setup (PLA_MISC_1 RXDY_EN)
    //   7. r8159_hw_phy_cfg: PHY config (no patch blobs for RTL8159)
    //   8. Set medium (PLA_CR), enable RX path
    (void)iface;
    Log("hwInit: STUB — OCP init not yet implemented");
}

static void
hwDisable(IOUSBHostInterface *iface)
{
    // TODO: implement from Linux r8156_disable() / r8156_down()
    (void)iface;
    Log("hwDisable: STUB");
}

static void txDrainOne(RTL8159NIC_IVars *ivars);

// ---------------------------------------------------------------------------

bool
RTL8159NIC::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(RTL8159NIC_IVars, 1);
    return ivars != nullptr;
}

void
RTL8159NIC::free()
{
    IOSafeDeleteNULL(ivars, RTL8159NIC_IVars, 1);
    super::free();
}

kern_return_t
IMPL(RTL8159NIC, Start)
{
    kern_return_t ret;
    IOUserNetworkPacketQueue *queues[4];
    struct IOUserNetworkPacketBufferPoolOptions poolOptions;
    IOUserNetworkMACAddress macAddress = {};

    Log("Start: provider=%p", provider);

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("Start: super failed: 0x%x", ret);
        return ret;
    }

    ret = IODispatchQueue::Create("RTL8159.async", 0, 0, &ivars->asyncQueue);
    if (ret != kIOReturnSuccess) { Log("Start: asyncQueue failed: 0x%x", ret); goto fail; }

    ret = SetDispatchQueue("RxDispatchQueue", ivars->asyncQueue);
    if (ret != kIOReturnSuccess) { Log("Start: SetDispatchQueue(Rx) failed: 0x%x", ret); goto fail; }

    ret = SetDispatchQueue("TxDispatchQueue", ivars->asyncQueue);
    if (ret != kIOReturnSuccess) { Log("Start: SetDispatchQueue(Tx) failed: 0x%x", ret); goto fail; }

    ivars->interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (ivars->interface == nullptr) {
        Log("Start: provider is not IOUSBHostInterface");
        goto fail;
    }

    ret = ivars->interface->CopyDevice(&ivars->device);
    if (ret != kIOReturnSuccess || ivars->device == nullptr) {
        Log("Start: CopyDevice failed: 0x%x", ret);
        goto fail;
    }

    ret = ivars->interface->Open(this, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        Log("Start: interface Open failed: 0x%x", ret);
        goto fail;
    }

    // Diagnostic: isolate the USB port reset from the first vendor OCP request.
    // Restore rtlReadMACAddress once configuration 1 remains stable.
    macAddress = { .octet = { 0x02, 0x8A, 0x15, 0x90, 0x00, 0x01 } };
    ivars->macAddress = macAddress;
    Log("Start: diagnostic: skipping OCP MAC read");

    hwInit(ivars->interface);

    // Log matched interface endpoints
    {
        const IOUSBConfigurationDescriptor *cd = ivars->interface->CopyConfigurationDescriptor();
        if (cd) {
            const IOUSBInterfaceDescriptor *id = ivars->interface->GetInterfaceDescriptor(cd);
            if (id) {
                Log("Start: matched if bNum=%u bClass=%u bAlt=%u nEP=%u",
                    id->bInterfaceNumber, id->bInterfaceClass,
                    id->bAlternateSetting, id->bNumEndpoints);
                const IOUSBEndpointDescriptor *ed =
                    IOUSBGetNextEndpointDescriptor(cd, id, nullptr);
                while (ed) {
                    Log("Start:   ep addr=0x%02x attr=0x%02x maxPkt=%u",
                        ed->bEndpointAddress, ed->bmAttributes,
                        USBToHost16(ed->wMaxPacketSize));
                    ed = IOUSBGetNextEndpointDescriptor(cd, id,
                             reinterpret_cast<const IOUSBDescriptorHeader *>(ed));
                }
            }
            IOFree(const_cast<IOUSBConfigurationDescriptor *>(cd),
                   USBToHost16(cd->wTotalLength));
        }
    }

    ret = ivars->interface->CopyPipe(EP_RX, &ivars->pipeRx);
    Log("Start: CopyPipe(RX  0x%02x) -> 0x%x pipe=%p", EP_RX, ret, ivars->pipeRx);
    if (ret != kIOReturnSuccess || ivars->pipeRx == nullptr) goto fail;

    ret = ivars->interface->CopyPipe(EP_TX, &ivars->pipeTx);
    Log("Start: CopyPipe(TX  0x%02x) -> 0x%x pipe=%p", EP_TX, ret, ivars->pipeTx);
    if (ret != kIOReturnSuccess || ivars->pipeTx == nullptr) goto fail;

    ret = ivars->interface->CopyPipe(EP_ITR, &ivars->pipeItr);
    Log("Start: CopyPipe(ITR 0x%02x) -> 0x%x pipe=%p", EP_ITR, ret, ivars->pipeItr);
    if (ret != kIOReturnSuccess || ivars->pipeItr == nullptr) goto fail;

    ivars->pipeRx->ClearStall(false);
    ivars->pipeItr->ClearStall(false);

    ret = CopyDispatchQueue("Default", &ivars->queue);
    if (ret != kIOReturnSuccess) { Log("Start: CopyDispatchQueue failed: 0x%x", ret); goto fail; }

    poolOptions.packetCount = 64;
    poolOptions.bufferCount = 64;
    poolOptions.bufferSize  = 2048;
    poolOptions.maxBuffersPerPacket = 1;
    poolOptions.memorySegmentSize = 0;
    poolOptions.poolFlags = PoolFlagMapToDext;
    poolOptions.dmaSpecification.maxAddressBits = 64;
    ret = IOUserNetworkPacketBufferPool::CreateWithOptions(this, "RTL8159", &poolOptions, &ivars->pool);
    if (ret != kIOReturnSuccess) { Log("Start: pool failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkTxSubmissionQueue::Create(ivars->pool, this, 16, 0, ivars->queue, &ivars->txsQueue);
    if (ret != kIOReturnSuccess) { Log("Start: txsQueue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkTxCompletionQueue::Create(ivars->pool, this, 16, 0, ivars->queue, &ivars->txcQueue);
    if (ret != kIOReturnSuccess) { Log("Start: txcQueue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxSubmissionQueue::Create(ivars->pool, this, 16, 0, ivars->queue, &ivars->rxsQueue);
    if (ret != kIOReturnSuccess) { Log("Start: rxsQueue failed: 0x%x", ret); goto fail; }

    ret = IOUserNetworkRxCompletionQueue::Create(ivars->pool, this, 16, 0, ivars->queue, &ivars->rxcQueue);
    if (ret != kIOReturnSuccess) { Log("Start: rxcQueue failed: 0x%x", ret); goto fail; }

    queues[0] = ivars->txsQueue;
    queues[1] = ivars->txcQueue;
    queues[2] = ivars->rxsQueue;
    queues[3] = ivars->rxcQueue;

    ret = SetTxPacketHeadroom(8);
    Log("Start: SetTxPacketHeadroom(8) -> 0x%x", ret);

    {
        const uint32_t opts = kIOUserNetworkMediaOptionFullDuplex |
                              kIOUserNetworkMediaOptionFlowControl;
        IOUserNetworkMediaType mediaTable[] = {
            kIOUserNetworkMediaEthernetNone,
            kIOUserNetworkMediaEthernetAuto,
            kIOUserNetworkMediaEthernet100BaseTX | opts,
            kIOUserNetworkMediaEthernet1000BaseT | opts,
            kIOUserNetworkMediaEthernet10GBaseT  | opts,
        };
        uint32_t mediaCount = sizeof(mediaTable) / sizeof(mediaTable[0]);
        ret = ReportAvailableMediaTypes(mediaTable, mediaCount);
        Log("Start: ReportAvailableMediaTypes(%u) -> 0x%x", mediaCount, ret);
        if (ret != kIOReturnSuccess) goto fail;
    }

    ret = SelectMediaType(kIOUserNetworkMediaEthernetNone, nullptr);
    if (ret != kIOReturnSuccess) { Log("Start: SelectMediaType failed: 0x%x", ret); goto fail; }

    ret = RegisterEthernetInterface(macAddress, ivars->pool, queues, 4);
    if (ret != kIOReturnSuccess) { Log("Start: RegisterEthernetInterface failed: 0x%x", ret); goto fail; }
    Log("Start: RegisterEthernetInterface OK");

    ret = CreateActionTxPacketAvailable(0, &ivars->txPacketAction);
    if (ret != kIOReturnSuccess) { Log("Start: CreateActionTxPacketAvailable failed: 0x%x", ret); goto fail; }

    {
        IODataQueueDispatchSource *txdq = nullptr;
        ret = ivars->txsQueue->CopyDataQueue(&txdq);
        if (ret == kIOReturnSuccess && txdq != nullptr) {
            ret = txdq->SetDataAvailableHandler(ivars->txPacketAction);
            OSSafeReleaseNULL(txdq);
        }
        if (ret != kIOReturnSuccess) { Log("Start: TX queue handler failed: 0x%x", ret); goto fail; }
    }

    ret = CreateActionOnTxComplete(0, &ivars->txCompleteAction);
    if (ret != kIOReturnSuccess) { Log("Start: CreateActionOnTxComplete failed: 0x%x", ret); goto fail; }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, 8 + 1518, 0, &ivars->txBuf);
    if (ret != kIOReturnSuccess) { Log("Start: txBuf failed: 0x%x", ret); goto fail; }

    for (int i = 0; i < RX_SLOTS; i++) {
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, RX_BUF_SIZE, 0, &ivars->rxBufs[i]);
        if (ret != kIOReturnSuccess) { Log("Start: rxBuf[%d] failed: 0x%x", i, ret); goto fail; }
        ret = CreateActionOnRxComplete(sizeof(uint32_t), &ivars->rxActions[i]);
        if (ret != kIOReturnSuccess) { Log("Start: rxAction[%d] failed: 0x%x", i, ret); goto fail; }
        *(uint32_t *)ivars->rxActions[i]->GetReference() = (uint32_t)i;
        ret = ivars->pipeRx->AsyncIO(ivars->rxBufs[i], RX_BUF_SIZE, ivars->rxActions[i], 0);
        if (ret != kIOReturnSuccess) { Log("Start: rxAsyncIO[%d] failed: 0x%x", i, ret); goto fail; }
    }
    Log("Start: %d RX transfers posted", RX_SLOTS);

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, ITR_BUF_SIZE, 0, &ivars->itrBuf);
    if (ret != kIOReturnSuccess) { Log("Start: itrBuf failed: 0x%x", ret); goto fail; }
    ret = CreateActionOnItrComplete(0, &ivars->itrAction);
    if (ret != kIOReturnSuccess) { Log("Start: itrAction failed: 0x%x", ret); goto fail; }
    ret = ivars->pipeItr->AsyncIO(ivars->itrBuf, ITR_BUF_SIZE, ivars->itrAction, 0);
    if (ret != kIOReturnSuccess) { Log("Start: itrAsyncIO failed: 0x%x", ret); goto fail; }
    ivars->ioArmed = true;
    Log("Start: ITR transfer posted (16B buffer)");

    ret = RegisterService();
    Log("Start: RegisterService -> 0x%x", ret);

    {
        kern_return_t tr = IOTimerDispatchSource::Create(ivars->queue, &ivars->timerTest);
        if (tr == kIOReturnSuccess) tr = CreateActionOnTimerFired(0, &ivars->timerAction);
        if (tr == kIOReturnSuccess) tr = ivars->timerTest->SetHandler(ivars->timerAction);
        if (tr == kIOReturnSuccess) tr = ivars->timerTest->SetEnable(true);
        if (tr == kIOReturnSuccess) {
            uint64_t fireAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 3ULL * 1000000000ULL;
            ivars->timerTest->WakeAtTime(kIOTimerClockUptimeRaw, fireAt, 0);
            Log("Start: diagnostic timer armed (~3s)");
        }
    }

    return ret;

fail:
    Stop(provider, SUPERDISPATCH);
    return kIOReturnError;
}

kern_return_t
IMPL(RTL8159NIC, Stop)
{
    Log("Stop: enter");
    ivars->interfaceEnabled = false;
    ivars->ioArmed = false;

    if (ivars->timerTest != nullptr) ivars->timerTest->Cancel(nullptr);

    if (ivars->pipeItr != nullptr) ivars->pipeItr->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
    if (ivars->pipeRx  != nullptr) ivars->pipeRx ->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
    if (ivars->pipeTx  != nullptr) ivars->pipeTx ->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);

    if (ivars->interface != nullptr) ivars->interface->Close(this, 0);

    OSSafeReleaseNULL(ivars->timerAction);
    OSSafeReleaseNULL(ivars->timerTest);
    OSSafeReleaseNULL(ivars->itrAction);
    OSSafeReleaseNULL(ivars->itrBuf);
    for (int i = 0; i < RX_SLOTS; i++) {
        OSSafeReleaseNULL(ivars->rxActions[i]);
        OSSafeReleaseNULL(ivars->rxBufs[i]);
    }
    OSSafeReleaseNULL(ivars->txCompleteAction);
    OSSafeReleaseNULL(ivars->txPacketAction);
    OSSafeReleaseNULL(ivars->txBuf);
    OSSafeReleaseNULL(ivars->rxcQueue);
    OSSafeReleaseNULL(ivars->rxsQueue);
    OSSafeReleaseNULL(ivars->txcQueue);
    OSSafeReleaseNULL(ivars->txsQueue);
    OSSafeReleaseNULL(ivars->pool);
    OSSafeReleaseNULL(ivars->pipeItr);
    OSSafeReleaseNULL(ivars->pipeRx);
    OSSafeReleaseNULL(ivars->pipeTx);
    // Start's interface provider is system-owned; OSDynamicCast did not retain it.
    ivars->interface = nullptr;
    OSSafeReleaseNULL(ivars->device);
    OSSafeReleaseNULL(ivars->asyncQueue);
    OSSafeReleaseNULL(ivars->queue);

    return Stop(provider, SUPERDISPATCH);
}

// ---------------------------------------------------------------------------
// OSAction diagnostic

void
IMPL(RTL8159NIC, OnTimerFired)
{
    Log("OnTimerFired: OSAction dispatch CONFIRMED WORKING");
}

// ---------------------------------------------------------------------------
// TX path

static void
txDrainOne(RTL8159NIC_IVars *ivars)
{
    if (ivars->txBusy || !ivars->interfaceEnabled || !ivars->ioArmed) return;

    IOUserNetworkPacket *packets[1];
    uint32_t count = ivars->txsQueue->DequeuePackets(packets, 1);
    if (count == 0) return;

    IOUserNetworkPacket *pkt = packets[0];
    uint32_t dataLen = pkt->getDataLength();

    if (dataLen == 0 || dataLen > 1514) {
        pkt->setCompletionStatus(kIOReturnError);
        ivars->txcQueue->EnqueuePacket(pkt);
        return;
    }

    const uint8_t *frame = (const uint8_t *)(uintptr_t)pkt->getDataVirtualAddress()
                         + pkt->getDataOffset();

    IOAddressSegment range;
    ivars->txBuf->GetAddressRange(&range);
    uint8_t *txp = (uint8_t *)range.address;

    rtl_tx_desc desc = {};
    desc.opts1 = rtl_tx_opts1(dataLen);
    memcpy(txp, &desc, sizeof(desc));
    memcpy(txp + sizeof(desc), frame, dataLen);
    uint32_t txLen = (uint32_t)sizeof(desc) + dataLen;
    ivars->txBuf->SetLength(txLen);

    Log("txDrainOne: len=%u", dataLen);

    ivars->txInFlight = pkt;
    ivars->txBusy     = true;

    kern_return_t r = ivars->pipeTx->AsyncIO(ivars->txBuf, txLen, ivars->txCompleteAction, 0);
    if (r != kIOReturnSuccess) {
        ivars->txBusy     = false;
        ivars->txInFlight = nullptr;
        pkt->setCompletionStatus(r);
        ivars->txcQueue->EnqueuePacket(pkt);
    }
}

void
IMPL(RTL8159NIC, TxPacketAvailable)
{
    txDrainOne(ivars);
}

void
IMPL(RTL8159NIC, OnTxComplete)
{
    Log("OnTxComplete: status=0x%x bytes=%u", status, actualByteCount);
    IOUserNetworkPacket *pkt = ivars->txInFlight;
    ivars->txInFlight = nullptr;
    ivars->txBusy     = false;
    if (pkt != nullptr) {
        pkt->setCompletionStatus(status == kIOReturnSuccess ? kIOReturnSuccess : kIOReturnError);
        ivars->txcQueue->EnqueuePacket(pkt);
    }
    txDrainOne(ivars);
}

// ---------------------------------------------------------------------------
// RX path

void
IMPL(RTL8159NIC, OnRxComplete)
{
    uint32_t slot = *(uint32_t *)action->GetReference();
    Log("OnRxComplete: slot=%u status=0x%x bytes=%u", slot, status, actualByteCount);

    if (status == kIOReturnAborted || status == kIOReturnNotAttached) return;
    if (!ivars->interfaceEnabled || !ivars->ioArmed) return;

    if (status == kUSBHostReturnPipeStalled) {
        ivars->pipeRx->ClearStall(false);
        ivars->pipeRx->AsyncIO(ivars->rxBufs[slot], RX_BUF_SIZE, ivars->rxActions[slot], 0);
        return;
    }
    if (status != kIOReturnSuccess) {
        Log("RX[%u] terminal error 0x%x — not reposting", slot, status);
        return;
    }

    // TODO: parse rtl_rx_desc aggregate format and deliver frames to Skywalk.
    Log("RX[%u] %u bytes — aggregate parser TODO", slot, actualByteCount);

    ivars->pipeRx->AsyncIO(ivars->rxBufs[slot], RX_BUF_SIZE, ivars->rxActions[slot], 0);
}

// ---------------------------------------------------------------------------
// ITR (interrupt IN) path — link status
//
// EP3 delivers a 16-byte packet; only bytes [0:1] are meaningful (wBytesPerInterval=2).
// Bit 0 (INTR_LINK) signals a link change. OCP PHYSTATUS read required for speed/duplex.

void
IMPL(RTL8159NIC, OnItrComplete)
{
    Log("OnItrComplete: status=0x%x bytes=%u", status, actualByteCount);

    if (status == kIOReturnAborted || status == kIOReturnNotAttached) return;
    if (!ivars->ioArmed) return;

    if (status == kUSBHostReturnPipeStalled) {
        ivars->pipeItr->ClearStall(false);
        ivars->pipeItr->AsyncIO(ivars->itrBuf, ITR_BUF_SIZE, ivars->itrAction, 0);
        return;
    }

    if (status == kIOReturnSuccess && actualByteCount >= 2) {
        IOAddressSegment range;
        ivars->itrBuf->GetAddressRange(&range);
        uint16_t istat = *(const uint16_t *)range.address;
        Log("OnItrComplete: istat=0x%04x", istat);

        if (istat & INTR_LINK) {
            IOUserNetworkMediaType media = kIOUserNetworkMediaEthernetNone;
            bool linkUp = rtlQueryLinkState(ivars->interface, &media);
            ivars->lastLinkUp = linkUp;

            if (ivars->interfaceEnabled) {
                LinkStatus ls = linkUp ? kIOUserNetworkLinkStatusActive
                                       : kIOUserNetworkLinkStatusInactive;
                IOReturn lsRet = reportLinkStatus(ls, media);
                Log("OnItrComplete: linkUp=%d media=0x%x reportLinkStatus -> 0x%x",
                    (int)linkUp, media, lsRet);
            }
        }
    }

    ivars->pipeItr->AsyncIO(ivars->itrBuf, ITR_BUF_SIZE, ivars->itrAction, 0);
}

// ---------------------------------------------------------------------------
// Dispatched overrides

kern_return_t
IMPL(RTL8159NIC, SetInterfaceEnable)
{
    Log("SetInterfaceEnable: %d", isEnable);
    if (isEnable) {
        ivars->interfaceEnabled = true;
        ivars->txsQueue->SetEnable(true);
        ivars->txcQueue->SetEnable(true);
        ivars->rxsQueue->SetEnable(true);
        ivars->rxcQueue->SetEnable(true);
        hwInit(ivars->interface);
        if (ivars->ioArmed) {
            ivars->pipeItr->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, nullptr);
            ivars->pipeItr->AsyncIO(ivars->itrBuf, ITR_BUF_SIZE, ivars->itrAction, 0);
        }
    } else {
        ivars->interfaceEnabled = false;
        hwDisable(ivars->interface);
        ivars->rxcQueue->SetEnable(false);
        ivars->rxsQueue->SetEnable(false);
        ivars->txcQueue->SetEnable(false);
        ivars->txsQueue->SetEnable(false);
        reportLinkStatus(kIOUserNetworkLinkStatusInactive, kIOUserNetworkMediaEthernetAuto);
    }
    return kIOReturnSuccess;
}

kern_return_t IMPL(RTL8159NIC, SetPromiscuousModeEnable)    { Log("SetPromiscuousModeEnable: %d", enable); return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, SetAllMulticastModeEnable)   { Log("SetAllMulticastModeEnable: %d", enable); return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, SetMulticastAddresses)       { Log("SetMulticastAddresses: count=%u", count); return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, SelectMediaType)             { Log("SelectMediaType: 0x%x", mediaType); return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, SetWakeOnMagicPacketEnable)  { Log("SetWakeOnMagicPacketEnable: %d", enable); return kIOReturnSuccess; }

// ---------------------------------------------------------------------------
// LOCAL overrides

kern_return_t IMPL(RTL8159NIC, SetMTU)             { Log("SetMTU: %u", mtu); return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, GetMaxTransferUnit) { *mtu = 1500; return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, SetHardwareAssists) { return kIOReturnSuccess; }
kern_return_t IMPL(RTL8159NIC, GetHardwareAssists) { *hardwareAssists = 0; return kIOReturnSuccess; }

// ---------------------------------------------------------------------------
// Media support

kern_return_t
RTL8159NIC::getSupportedMediaArray(MediaWord *mediaArray, uint32_t *mediaCount)
{
    static const uint32_t opts = kIOUserNetworkMediaOptionFullDuplex |
                                 kIOUserNetworkMediaOptionFlowControl;
    static const MediaWord kMedia[] = {
        kIOUserNetworkMediaEthernetAuto,
        kIOUserNetworkMediaEthernet100BaseTX | opts,
        kIOUserNetworkMediaEthernet1000BaseT | opts,
        kIOUserNetworkMediaEthernet10GBaseT  | opts,
    };
    const uint32_t count = sizeof(kMedia) / sizeof(kMedia[0]);
    for (uint32_t i = 0; i < count; i++) mediaArray[i] = kMedia[i];
    *mediaCount = count;
    return kIOReturnSuccess;
}

kern_return_t RTL8159NIC::handleChosenMedia(MediaWord chosenMedia) { Log("handleChosenMedia: 0x%x", chosenMedia); return kIOReturnSuccess; }
MediaWord     RTL8159NIC::getInitialMedia()                        { return kIOUserNetworkMediaEthernetAuto; }
