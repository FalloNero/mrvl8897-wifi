/*
 * mrvl8897 - WiFiCx / NetAdapterCx glue.
 *
 * Control path (WDI over WiFiCx): EvtWifiDeviceSendCommand gets an M1;
 * WifiRequestComplete sends the M3; tasks then send an M4 indication with the
 * same TransactionId via WifiDeviceReceiveIndication; unsolicited indications
 * use TransactionId 0. Slow commands (connect, keys, disconnect, reset) run on
 * a work item at PASSIVE_LEVEL, one at a time, in arrival order.
 *
 * Data path: Windows (nwifi) exchanges native 802.11 data frames with us; the
 * firmware wants/gives Ethernet II (+TxPD/RxPD, done in card.c). TX packets
 * are converted and queued for a sender thread (card writes block); RX frames
 * from the card are converted to 802.11 and handed to the RX queue.
 *
 * Security: Windows runs the WPA2 4-way handshake itself over the data path
 * and installs keys with WDI_SET_ADD_CIPHER_KEYS -> HostCmd KEY_MATERIAL.
 *
 * GPL v2 (part of the mwifiex port).
 */
#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <netiodef.h>
#include <net/virtualaddress.h>
#include <wificx.h>
#include <TlvGeneratorParser.hpp>

#include "mrvl8897.h"

/* ------------------------------------------------------------------ */
/* C++ runtime bits the TLV generator/parser library needs (no CRT)   */
/* ------------------------------------------------------------------ */

inline void *operator new(size_t, void *p) noexcept { return p; }
inline void operator delete(void *, void *) noexcept {}

static void *MrvlAlloc(size_t n)
{
    if (n == 0) n = 1;
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, n, 'lTrM');   /* zeroed */
}
static void MrvlFree(void *p)
{
    if (p) ExFreePoolWithTag(p, 'lTrM');
}

/* AllocationContext is always 0 (we never hand the library a placement buffer). */
void *__cdecl operator new(size_t n, ULONG_PTR) noexcept { return MrvlAlloc(n); }
void *__cdecl operator new[](size_t n, ULONG_PTR) noexcept { return MrvlAlloc(n); }
void *__cdecl operator new(size_t n) { return MrvlAlloc(n); }
void *__cdecl operator new[](size_t n) { return MrvlAlloc(n); }
void __cdecl operator delete(void *p) noexcept { MrvlFree(p); }
void __cdecl operator delete[](void *p) noexcept { MrvlFree(p); }
/* (ULONG_PTR == size_t on x64, so these also serve the library's placement deletes) */
void __cdecl operator delete(void *p, size_t) noexcept { MrvlFree(p); }
void __cdecl operator delete[](void *p, size_t) noexcept { MrvlFree(p); }

/* ------------------------------------------------------------------ */
/* Firmware command / event constants (mwifiex fw.h)                  */
/* ------------------------------------------------------------------ */

#define HostCmd_CMD_802_11_ASSOCIATE      0x0012
#define HostCmd_CMD_802_11_DEAUTHENTICATE 0x0024
#define HostCmd_CMD_802_11_KEY_MATERIAL   0x005e
#define HostCmd_ACT_GEN_SET               0x0001
#define TLV_TYPE_KEY_MATERIAL             0x0100
#define TLV_TYPE_CHANLIST                 0x0101
#define TLV_TYPE_AUTH_TYPE                0x011F
#define TLV_TYPE_KEY_PARAM_V2             0x019C
#define KEY_TYPE_ID_AES                   2
#define KEY_MCAST                         0x0001
#define KEY_UNICAST                       0x0002
#define KEY_ENABLED                       0x0004
#define KEY_DEFAULT                       0x0008
#define KEY_TX_KEY                        0x0010
#define KEY_RX_KEY                        0x0020
#define EVENT_LINK_LOST                   0x0003
#define EVENT_DEAUTHENTICATED             0x0008
#define EVENT_DISASSOCIATED               0x0009
#define CAPINFO_MASK                      0x25FF   /* ~(BIT15|BIT14|BIT12|BIT11|BIT9) */

/* ------------------------------------------------------------------ */
/* Contexts                                                           */
/* ------------------------------------------------------------------ */

#define FRAME_MAX       2304
#define TX_SLOTS        32
#define RX_SLOTS        32

typedef struct _FRAME_SLOT {
    USHORT Len;
    UCHAR  Prio;
    UCHAR  Data[FRAME_MAX];
} FRAME_SLOT;

typedef struct _CMD_ITEM {
    LIST_ENTRY  Link;
    WIFIREQUEST Request;
    UINT16      Id;
    ULONG       InLen;
    UCHAR       Data[1];                 /* copy of the M1 (header + TLVs) */
} CMD_ITEM, *PCMD_ITEM;

typedef struct _WIFI_CTX {
    TLV_CONTEXT         Tlv;
    NETADAPTER          Adapter;
    WDFDEVICE           Device;
    UCHAR               RadioOn;

    /* scan */
    WDFWORKITEM         ScanWork;
    volatile LONG       ScanBusy;
    WDI_MESSAGE_HEADER  ScanHdr;
    UCHAR               ScanSsid[32];
    ULONG               ScanSsidLen;

    /* serialised slow commands */
    KSPIN_LOCK          CmdListLock;
    LIST_ENTRY          CmdList;
    WDFWORKITEM         CmdWork;

    /* firmware events (link lost etc.) */
    WDFWORKITEM         EventWork;
    volatile LONG       PendingEvent;

    /* connection */
    volatile LONG       Connected;
    UCHAR               Bssid[6];
    UCHAR               Channel;
    UCHAR               Is5GHz;
    LONG                Rssi;
    WDI_MESSAGE_HEADER  ConnHdr;          /* header of the connect M1 (port id for indications) */
    BOOLEAN             GtkSet;
    BOOLEAN             Ht;               /* 802.11n negotiated */
    BOOLEAN             Use40;            /* 40 MHz */
    BOOLEAN             Sgi;              /* short guard interval */
    BOOLEAN             Wmm;
    UCHAR               Streams;

    /* TX: Advance -> slots -> sender thread -> card */
    FRAME_SLOT         *TxSlots;
    volatile LONG       TxHead, TxTail;   /* producer: Advance, consumer: thread */
    KSPIN_LOCK          TxLock;
    KEVENT              TxEvent;
    volatile LONG       TxStop;
    PVOID               TxThread;
    ULONG               TxLogged;
    LONG64              TxOk, TxDrop;

    /* RX: card -> slots -> RX queue */
    FRAME_SLOT         *RxSlots;
    ULONG               RxHead, RxTail;
    KSPIN_LOCK          RxLock;
    NETPACKETQUEUE      RxQueue;
    volatile LONG       RxNotify;
    ULONG               RxLogged;
    LONG64              RxOk, RxDrop;
} WIFI_CTX, *PWIFI_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WIFI_CTX, GetWifi)

typedef struct _QUEUE_CTX {
    NET_RING_COLLECTION const *Rings;
    NET_EXTENSION              Va;
    PWIFI_CTX                  W;
    PUCHAR                     Scratch;   /* TX: one gathered 802.11 frame */
} QUEUE_CTX, *PQUEUE_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CTX, GetQueue)

static EVT_WIFI_DEVICE_SEND_COMMAND EvtWifiSendCommand;
static EVT_WIFI_DEVICE_CREATE_ADAPTER EvtWifiCreateAdapter;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE EvtWifiCreateWfdDevice;
static EVT_WDF_WORKITEM ScanWorker;
static EVT_WDF_WORKITEM CmdWorker;
static EVT_WDF_WORKITEM EventWorker;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP WifiCtxCleanup;
static EVT_NET_ADAPTER_CREATE_TXQUEUE EvtCreateTxQueue;
static EVT_NET_ADAPTER_CREATE_RXQUEUE EvtCreateRxQueue;
static EVT_PACKET_QUEUE_ADVANCE EvtTxAdvance;
static EVT_PACKET_QUEUE_ADVANCE EvtRxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtTxSetNotify;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtRxSetNotify;
static EVT_PACKET_QUEUE_CANCEL EvtTxCancel;
static EVT_PACKET_QUEUE_CANCEL EvtRxCancel;
static KSTART_ROUTINE TxThreadMain;

static void PutLe16(PUCHAR p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }
static USHORT GetLe16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }

/* ------------------------------------------------------------------ */
/* Indications                                                        */
/* ------------------------------------------------------------------ */

static void Indicate(WDFDEVICE Device, UINT16 MessageId, const WDI_MESSAGE_HEADER *Ref, UINT32 TransactionId,
                     NDIS_STATUS Status, const UINT8 *Tlv, ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDFMEMORY mem;
    PUCHAR buf;
    PWDI_MESSAGE_HEADER h;
    size_t size = sizeof(WDI_MESSAGE_HEADER) + TlvLen;

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&attr, NonPagedPoolNx, MRVL_TAG, size, &mem, reinterpret_cast<PVOID *>(&buf)))) {
        LOG("wdi: no memory for indication %u", MessageId);
        return;
    }
    RtlZeroMemory(buf, size);
    h = reinterpret_cast<PWDI_MESSAGE_HEADER>(buf);
    h->PortId = Ref ? Ref->PortId : 0;
    h->Reserved = Ref ? Ref->Reserved : 0;
    h->IhvSpecificId = Ref ? Ref->IhvSpecificId : 0;
    h->TransactionId = TransactionId;
    h->Status = Status;
    if (Tlv && TlvLen)
        RtlCopyMemory(buf + sizeof(WDI_MESSAGE_HEADER), Tlv, TlvLen);
    WifiDeviceReceiveIndication(Device, MessageId, mem);
    WdfObjectDelete(mem);
}

static void CompleteM3(WIFIREQUEST Request, NTSTATUS Status)
{
    WifiRequestComplete(Request, Status, sizeof(WDI_MESSAGE_HEADER));
}

static void CompleteTrivialTask(WDFDEVICE Device, WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, UINT16 M4Id)
{
    CompleteM3(Request, STATUS_SUCCESS);
    Indicate(Device, M4Id, Hdr, Hdr->TransactionId, NDIS_STATUS_SUCCESS, nullptr, 0);
}

/* ------------------------------------------------------------------ */
/* IE helpers                                                         */
/* ------------------------------------------------------------------ */

/* Finds IE Id in a beacon/probe-response body (12 fixed bytes, then IEs). */
static const UCHAR *FindIe(const UCHAR *Body, ULONG BodyLen, UCHAR Id)
{
    ULONG p = 12;
    while (p + 2 <= BodyLen) {
        UCHAR l = Body[p + 1];
        if (p + 2 + l > BodyLen) break;
        if (Body[p] == Id) return Body + p;
        p += 2 + l;
    }
    return nullptr;
}

/* WMM information/parameter element: vendor IE 221, OUI 00:50:F2, type 2 */
static const UCHAR *FindWmmIe(const UCHAR *Body, ULONG BodyLen)
{
    ULONG p = 12;
    while (p + 2 <= BodyLen) {
        UCHAR l = Body[p + 1];
        if (p + 2 + l > BodyLen) break;
        if (Body[p] == 221 && l >= 5 && Body[p + 2] == 0x00 && Body[p + 3] == 0x50 &&
            Body[p + 4] == 0xF2 && Body[p + 5] == 0x02)
            return Body + p;
        p += 2 + l;
    }
    return nullptr;
}

static ULONG LinkQualityFromRssi(LONG Rssi)
{
    LONG q = (Rssi + 100) * 2;      /* -100 dBm -> 0, -50 dBm -> 100 */
    return (ULONG)(q < 0 ? 0 : q > 100 ? 100 : q);
}

/* ------------------------------------------------------------------ */
/* Scan                                                               */
/* ------------------------------------------------------------------ */

static void IndicateBssList(WDFDEVICE Device, PDEVICE_CONTEXT Ctx, PWIFI_CTX W)
{
    ULONG n = Ctx->ScanBssCount, i;
    WDI_BSS_ENTRY_CONTAINER *arr;
    UINT8 *out = nullptr;
    ULONG outLen = 0;
    NDIS_STATUS ns;

    if (n == 0)
        return;
    arr = static_cast<WDI_BSS_ENTRY_CONTAINER *>(MrvlAlloc(sizeof(WDI_BSS_ENTRY_CONTAINER) * n));
    if (!arr)
        return;
    for (i = 0; i < n; i++) {
        PMRVL_BSS b = &Ctx->ScanResults[i];
        WDI_BSS_ENTRY_CONTAINER *e = new (&arr[i]) WDI_BSS_ENTRY_CONTAINER();
        RtlCopyMemory(e->BSSID.Address, b->Bssid, 6);
        e->Optional.ProbeResponseFrame_IsPresent = 1;
        e->ProbeResponseFrame.SimpleAssign(b->Body, b->BodyLen);
        e->SignalInfo.RSSI = b->Rssi;
        e->SignalInfo.LinkQuality = LinkQualityFromRssi(b->Rssi);
        e->ChannelInfo.ChannelNumber = b->Channel;
        e->ChannelInfo.BandId = b->Is5GHz ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    }
    {
        WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS prm;
        prm.Optional.DeviceDescriptor_IsPresent = 1;
        prm.DeviceDescriptor.SimpleAssign(arr, n);
        ns = GenerateWdiIndicationBssEntryList(&prm, 0, &W->Tlv, &outLen, &out);
        if (ns == NDIS_STATUS_SUCCESS && out) {
            Indicate(Device, WDI_INDICATION_BSS_ENTRY_LIST, &W->ScanHdr, 0, NDIS_STATUS_SUCCESS, out, outLen);
            LOG("wdi: indicated %u BSS entries (%u bytes)", n, outLen);
        } else {
            LOG("wdi: generating BSS list failed 0x%08X", ns);
        }
        if (out)
            FreeGenerated(out);
        prm.DeviceDescriptor.SimpleAssign(nullptr, 0);   /* not owned */
    }
    for (i = 0; i < n; i++)
        arr[i].ProbeResponseFrame.SimpleAssign(nullptr, 0);
    MrvlFree(arr);
}

static VOID ScanWorker(WDFWORKITEM Item)
{
    WDFDEVICE dev = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(Item));
    PDEVICE_CONTEXT ctx = GetCtx(dev);
    PWIFI_CTX w = GetWifi(dev);
    NTSTATUS st = STATUS_SUCCESS;

    /* While connected the firmware would leave the channel; skip until background
     * scanning is supported (Windows keeps the connected network listed anyway). */
    if (w->RadioOn && !w->Connected) {
        st = CardScan(ctx, w->ScanSsid, w->ScanSsidLen);
        if (NT_SUCCESS(st))
            IndicateBssList(dev, ctx, w);
    }
    Indicate(dev, WDI_INDICATION_SCAN_COMPLETE, &w->ScanHdr, w->ScanHdr.TransactionId,
             NT_SUCCESS(st) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE, nullptr, 0);
    InterlockedExchange(&w->ScanBusy, 0);
}

static void HandleScan(WDFDEVICE Device, WIFIREQUEST Request, PWDI_MESSAGE_HEADER Hdr, UINT InLen)
{
    PWIFI_CTX w = GetWifi(Device);
    WDI_SCAN_PARAMETERS prm;
    NDIS_STATUS ns;

    if (InterlockedCompareExchange(&w->ScanBusy, 1, 0) != 0) {
        CompleteM3(Request, STATUS_DEVICE_BUSY);
        return;
    }
    w->ScanSsidLen = 0;
    ns = ParseWdiTaskScan(InLen - sizeof(WDI_MESSAGE_HEADER), reinterpret_cast<UINT8 *>(Hdr + 1), &w->Tlv, &prm);
    if (ns == NDIS_STATUS_SUCCESS) {
        UINT32 k;
        for (k = 0; k < prm.SSIDList.ElementCount; k++) {
            WDI_SSID &s = prm.SSIDList.pElements[k];
            if (s.ElementCount > 0 && s.ElementCount <= 32) {
                RtlCopyMemory(w->ScanSsid, s.pElements, s.ElementCount);
                w->ScanSsidLen = s.ElementCount;
                break;
            }
        }
        CleanupParsedWdiTaskScan(&prm);
    }
    w->ScanHdr = *Hdr;
    CompleteM3(Request, STATUS_SUCCESS);
    WdfWorkItemEnqueue(w->ScanWork);
}

/* ------------------------------------------------------------------ */
/* Connection                                                         */
/* ------------------------------------------------------------------ */

static void IndicateLinkState(PWIFI_CTX W)
{
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS prm;
    WDI_LINK_INFO_CONTAINER link;
    UINT8 *out = nullptr;
    ULONG outLen = 0;
    PDEVICE_CONTEXT ctx = GetCtx(W->Device);

    /* nominal top rate, kbps: HT MCS7 per stream = 65 / 72.2 (SGI) at 20 MHz, 135 / 150 at 40 MHz */
    ULONG kbps = 54000;
    if (W->Ht)
        kbps = W->Streams * (W->Use40 ? (W->Sgi ? 150000u : 135000u) : (W->Sgi ? 72200u : 65000u));
    RtlCopyMemory(&prm.LinkStateChangeParameters.PeerMACAddress, W->Bssid, 6);
    prm.LinkStateChangeParameters.TxLinkSpeed = kbps;
    prm.LinkStateChangeParameters.RxLinkSpeed = kbps;
    prm.LinkStateChangeParameters.LinkQuality = (UINT8)LinkQualityFromRssi(W->Rssi);
    link.LinkID = 0;
    RtlCopyMemory(&link.LocalLinkMACAddress, ctx->Mac, 6);
    RtlCopyMemory(&link.PeerLinkMACAddress, W->Bssid, 6);
    link.ChannelNumber = W->Channel;
    link.BandId = W->Is5GHz ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
    link.RSSI = W->Rssi;
    link.Bandwidth = W->Use40 ? 40 : 20;
    link.TxMCS = W->Ht ? (W->Streams == 2 ? 15 : 7) : 0;
    link.RxMCS = link.TxMCS;
    prm.LinkInfo.SimpleAssign(&link, 1);
    if (GenerateWdiIndicationLinkStateChange(&prm, 0, &W->Tlv, &outLen, &out) == NDIS_STATUS_SUCCESS && out) {
        Indicate(W->Device, WDI_INDICATION_LINK_STATE_CHANGE, &W->ConnHdr, 0, NDIS_STATUS_SUCCESS, out, outLen);
        FreeGenerated(out);
    } else {
        LOG("wdi: link state generate failed");
    }
    prm.LinkInfo.SimpleAssign(nullptr, 0);
}

static void IndicateDisassociation(PWIFI_CTX W, WDI_ASSOC_STATUS Reason)
{
    WDI_INDICATION_DISASSOCIATION_PARAMETERS prm;
    UINT8 *out = nullptr;
    ULONG outLen = 0;

    RtlCopyMemory(&prm.DisconnectIndicationParameters.MacAddress, W->Bssid, 6);
    prm.DisconnectIndicationParameters.DisassociationWABIReason = Reason;
    prm.Optional.NeedPeerStateCleanup_IsPresent = 1;
    prm.NeedPeerStateCleanup = 0;
    if (GenerateWdiIndicationDisassociation(&prm, 0, &W->Tlv, &outLen, &out) == NDIS_STATUS_SUCCESS && out) {
        Indicate(W->Device, WDI_INDICATION_DISASSOCIATION, &W->ConnHdr, 0, NDIS_STATUS_SUCCESS, out, outLen);
        FreeGenerated(out);
    }
    LOG("wifi: disassociated (reason %u)", (ULONG)Reason);
}

static void Deauthenticate(PDEVICE_CONTEXT Ctx, PWIFI_CTX W)
{
    UCHAR p[8];
    NTSTATUS st;
    RtlCopyMemory(p, W->Bssid, 6);
    PutLe16(p + 6, 3);                      /* reason 3: STA leaving */
    st = CardCommand(Ctx, HostCmd_CMD_802_11_DEAUTHENTICATE, p, sizeof(p), nullptr, 0, nullptr);
    LOG("wifi: deauthenticate 0x%08X", st);
    InterlockedExchange(&W->Connected, 0);
    W->GtkSet = FALSE;
}

static WDI_CIPHER_ALGORITHM CipherFromSuite(const UCHAR *Suite)
{
    if (Suite[0] == 0x00 && Suite[1] == 0x0F && Suite[2] == 0xAC) {
        switch (Suite[3]) {
        case 1: return WDI_CIPHER_ALGO_WEP40;
        case 2: return WDI_CIPHER_ALGO_TKIP;
        case 4: return WDI_CIPHER_ALGO_CCMP;
        case 5: return WDI_CIPHER_ALGO_WEP104;
        }
    }
    return WDI_CIPHER_ALGO_NONE;
}

/*
 * WDI_TASK_CONNECT: M3 now, then build HostCmd_CMD_802_11_ASSOCIATE from the
 * first preferred BSS (mwifiex join.c), report WDI_INDICATION_ASSOCIATION_RESULT
 * and finish with the M4 (WDI_INDICATION_CONNECT_COMPLETE).
 */
static void DoConnect(WDFDEVICE Dev, WIFIREQUEST Req, PWDI_MESSAGE_HEADER Hdr, ULONG InLen)
{
    PDEVICE_CONTEXT ctx = GetCtx(Dev);
    PWIFI_CTX w = GetWifi(Dev);
    WDI_TASK_CONNECT_PARAMETERS prm;
    NDIS_STATUS ns;
    const UCHAR *body = nullptr, *ssid = nullptr, *ie;
    ULONG bodyLen = 0, ssidLen = 0, i, p = 0, reqIeStart, reqLen = 0;
    UCHAR bssid[6] = { 0 }, chan = 0, is5 = 0;
    WDI_AUTH_ALGORITHM auth = WDI_AUTH_ALGO_UNKNOWN;
    WDI_CIPHER_ALGORITHM ucast = WDI_CIPHER_ALGO_NONE, mcast = WDI_CIPHER_ALGO_NONE;
    LONG rssi = -60;
    PUCHAR cmd = nullptr, rsp = nullptr, reqFrame = nullptr;
    ULONG rspLen = 0;
    USHORT cap, status = 0xFFFF, rcap = 0xFFFF;
    UCHAR rsnIe[2 + 20];
    ULONG rsnLen = 0;
    NTSTATUS st;
    WDI_ASSOC_STATUS assoc = WDI_ASSOC_STATUS_FAILURE;

    ns = ParseWdiTaskConnect(InLen - sizeof(WDI_MESSAGE_HEADER), reinterpret_cast<UINT8 *>(Hdr + 1), &w->Tlv, &prm);
    if (ns != NDIS_STATUS_SUCCESS) {
        LOG("wifi: connect parameters parse failed 0x%08X", ns);
        CompleteM3(Req, STATUS_INVALID_PARAMETER);
        return;
    }
    CompleteM3(Req, STATUS_SUCCESS);
    w->ConnHdr = *Hdr;
    if (w->Connected)
        Deauthenticate(ctx, w);

    cmd = static_cast<PUCHAR>(MrvlAlloc(1024));
    rsp = static_cast<PUCHAR>(MrvlAlloc(1024));
    reqFrame = static_cast<PUCHAR>(MrvlAlloc(512));
    if (!cmd || !rsp || !reqFrame || prm.PreferredBSSEntryList.ElementCount == 0 ||
        prm.ConnectParameters.SSIDList.ElementCount == 0) {
        LOG("wifi: connect: no memory / no BSS / no SSID");
        goto report;
    }

    /* target BSS */
    {
        WDI_CONNECT_BSS_ENTRY_CONTAINER &b = prm.PreferredBSSEntryList.pElements[0];
        RtlCopyMemory(bssid, b.BSSID.Address, 6);
        if (b.Optional.ProbeResponseFrame_IsPresent) {
            body = b.ProbeResponseFrame.pElements; bodyLen = b.ProbeResponseFrame.ElementCount;
        } else if (b.Optional.BeaconFrame_IsPresent) {
            body = b.BeaconFrame.pElements; bodyLen = b.BeaconFrame.ElementCount;
        }
        chan = (UCHAR)b.ChannelInfo.ChannelNumber;
        is5 = b.ChannelInfo.BandId == WDI_BAND_ID_5000;
        rssi = b.SignalInfo.RSSI ? b.SignalInfo.RSSI : rssi;
    }
    if (!body || bodyLen < 12) {
        for (i = 0; i < ctx->ScanBssCount; i++)
            if (RtlEqualMemory(ctx->ScanResults[i].Bssid, bssid, 6)) {
                body = ctx->ScanResults[i].Body; bodyLen = ctx->ScanResults[i].BodyLen;
                if (!chan) { chan = ctx->ScanResults[i].Channel; is5 = ctx->ScanResults[i].Is5GHz; }
                break;
            }
    }
    if (!body || bodyLen < 12) {
        LOG("wifi: connect: no beacon/probe response for the BSS");
        goto report;
    }
    if (!chan && (ie = FindIe(body, bodyLen, 3)) != nullptr && ie[1] >= 1)
        chan = ie[2];
    ssid = prm.ConnectParameters.SSIDList.pElements[0].pElements;
    ssidLen = prm.ConnectParameters.SSIDList.pElements[0].ElementCount;
    if (ssidLen > 32) ssidLen = 32;

    /* security: prefer WPA2-PSK, then WPA2-802.1X, then open */
    for (i = 0; i < prm.ConnectParameters.AuthenticationAlgorithms.ElementCount; i++) {
        WDI_AUTH_ALGORITHM a = prm.ConnectParameters.AuthenticationAlgorithms.pElements[i];
        if (a == WDI_AUTH_ALGO_RSNA_PSK) { auth = a; break; }
        if (a == WDI_AUTH_ALGO_RSNA && auth != WDI_AUTH_ALGO_RSNA_PSK) auth = a;
        if (a == WDI_AUTH_ALGO_80211_OPEN && auth == WDI_AUTH_ALGO_UNKNOWN) auth = a;
    }
    for (i = 0; i < prm.ConnectParameters.UnicastCipherAlgorithms.ElementCount; i++)
        if (prm.ConnectParameters.UnicastCipherAlgorithms.pElements[i] == WDI_CIPHER_ALGO_CCMP)
            ucast = WDI_CIPHER_ALGO_CCMP;
    if (ucast == WDI_CIPHER_ALGO_NONE && prm.ConnectParameters.UnicastCipherAlgorithms.ElementCount)
        ucast = prm.ConnectParameters.UnicastCipherAlgorithms.pElements[0];
    if (prm.ConnectParameters.MulticastCipherAlgorithms.ElementCount)
        mcast = prm.ConnectParameters.MulticastCipherAlgorithms.pElements[0];

    if (auth == WDI_AUTH_ALGO_RSNA_PSK || auth == WDI_AUTH_ALGO_RSNA) {
        const UCHAR *apRsn = FindIe(body, bodyLen, 48);
        if (apRsn && apRsn[1] >= 6)
            mcast = CipherFromSuite(apRsn + 4);          /* AP's group cipher */
        if (ucast != WDI_CIPHER_ALGO_CCMP) {
            LOG("wifi: connect: only CCMP pairwise is supported (asked for %u)", (ULONG)ucast);
            goto report;
        }
        /* our RSN IE: v1, group = AP's, pairwise CCMP, AKM PSK or 802.1X, caps 0 */
        rsnIe[0] = 48; rsnIe[1] = 20;
        rsnIe[2] = 1; rsnIe[3] = 0;
        rsnIe[4] = 0x00; rsnIe[5] = 0x0F; rsnIe[6] = 0xAC;
        rsnIe[7] = mcast == WDI_CIPHER_ALGO_TKIP ? 2 : 4;
        rsnIe[8] = 1; rsnIe[9] = 0;
        rsnIe[10] = 0x00; rsnIe[11] = 0x0F; rsnIe[12] = 0xAC; rsnIe[13] = 4;
        rsnIe[14] = 1; rsnIe[15] = 0;
        rsnIe[16] = 0x00; rsnIe[17] = 0x0F; rsnIe[18] = 0xAC; rsnIe[19] = auth == WDI_AUTH_ALGO_RSNA_PSK ? 2 : 1;
        rsnIe[20] = 0; rsnIe[21] = 0;
        rsnLen = 22;
    } else if (auth != WDI_AUTH_ALGO_80211_OPEN || ucast != WDI_CIPHER_ALGO_NONE) {
        LOG("wifi: connect: unsupported security (auth %u, cipher %u)", (ULONG)auth, (ULONG)ucast);
        goto report;
    }

    cap = (USHORT)(GetLe16(body + 10) & CAPINFO_MASK);
    LOG("wifi: connecting to %02X:%02X:%02X:%02X:%02X:%02X ch%u auth %u ucast %u mcast %u",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], chan, (ULONG)auth, (ULONG)ucast, (ULONG)mcast);

    /* host_cmd_ds_802_11_associate + TLVs */
    RtlCopyMemory(cmd, bssid, 6); p = 6;
    PutLe16(cmd + p, cap); p += 2;
    PutLe16(cmd + p, 10); p += 2;                        /* listen interval */
    PutLe16(cmd + p, GetLe16(body + 8)); p += 2;         /* beacon period */
    cmd[p++] = 0;                                        /* dtim period */
    /* the association request body we report = cap, listen interval, IEs */
    PutLe16(reqFrame, cap); PutLe16(reqFrame + 2, 10); reqLen = 4;
    reqIeStart = reqLen;
    UNREFERENCED_PARAMETER(reqIeStart);

    PutLe16(cmd + p, 0); PutLe16(cmd + p + 2, (USHORT)ssidLen); p += 4;      /* SSID */
    RtlCopyMemory(cmd + p, ssid, ssidLen); p += ssidLen;
    reqFrame[reqLen++] = 0; reqFrame[reqLen++] = (UCHAR)ssidLen;
    RtlCopyMemory(reqFrame + reqLen, ssid, ssidLen); reqLen += ssidLen;

    PutLe16(cmd + p, 3); PutLe16(cmd + p + 2, 1); p += 4; cmd[p++] = chan;  /* DS params */
    PutLe16(cmd + p, 4); PutLe16(cmd + p + 2, 6); p += 4;                     /* CF params */
    RtlZeroMemory(cmd + p, 6); p += 6;
    {
        /* rates: AP supported + extended rates, as advertised */
        UCHAR rates[32];
        ULONG nr = 0;
        const UCHAR *r1 = FindIe(body, bodyLen, 1), *r2 = FindIe(body, bodyLen, 50);
        if (r1) { RtlCopyMemory(rates, r1 + 2, min(r1[1], 16)); nr = min(r1[1], 16); }
        if (r2) { ULONG k = min(r2[1], (ULONG)(sizeof(rates) - nr)); RtlCopyMemory(rates + nr, r2 + 2, k); nr += k; }
        if (nr == 0) { static const UCHAR def[] = { 0x82, 0x84, 0x8b, 0x96 }; RtlCopyMemory(rates, def, 4); nr = 4; }
        PutLe16(cmd + p, 1); PutLe16(cmd + p + 2, (USHORT)nr); p += 4;
        RtlCopyMemory(cmd + p, rates, nr); p += nr;
        reqFrame[reqLen++] = 1; reqFrame[reqLen++] = (UCHAR)min(nr, 8);
        RtlCopyMemory(reqFrame + reqLen, rates, min(nr, 8)); reqLen += min(nr, 8);
        if (nr > 8) {
            reqFrame[reqLen++] = 50; reqFrame[reqLen++] = (UCHAR)(nr - 8);
            RtlCopyMemory(reqFrame + reqLen, rates + 8, nr - 8); reqLen += nr - 8;
        }
    }
    PutLe16(cmd + p, TLV_TYPE_AUTH_TYPE); PutLe16(cmd + p + 2, 2); p += 4;   /* open system auth */
    PutLe16(cmd + p, 0); p += 2;
    {
        /*
         * 802.11n + WMM (mwifiex 11n.c mwifiex_cmd_append_11n_tlv, wmm.c): only with an
         * HT-capable AP and CCMP/open (HT forbids TKIP/WEP). 40 MHz only on 5 GHz and
         * only if the AP runs 40 MHz; the secondary channel offset goes into the
         * channel TLV's radio-type byte (bits 4-5: 1 = above, 3 = below).
         */
        const UCHAR *apHtCap = FindIe(body, bodyLen, 45), *apHtOp = FindIe(body, bodyLen, 61);
        const UCHAR *apWmm = FindWmmIe(body, bodyLen);
        ULONG dc = ctx->Dot11nDevCap;
        BOOLEAN ht = !(ctx->Options & 0x800) &&            /* SdioFlags 0x800: 802.11n off */
                     (ctx->FwCapInfo & (1u << 11)) && apHtCap && apHtOp && apHtOp[1] >= 22 &&
                     (ucast == WDI_CIPHER_ALGO_CCMP || ucast == WDI_CIPHER_ALGO_NONE);
        UCHAR secOff = 0;
        w->Use40 = FALSE;
        w->Sgi = FALSE;
        w->Streams = 1;
        if (ht && is5 && (dc & (1u << 17)) && (apHtOp[3] & 0x04))
            secOff = apHtOp[3] & 0x03;
        if (secOff != 1 && secOff != 3)
            secOff = 0;

        PutLe16(cmd + p, TLV_TYPE_CHANLIST); PutLe16(cmd + p + 2, 7); p += 4;    /* the AP's channel */
        cmd[p++] = (UCHAR)((is5 ? 1 : 0) | (secOff << 4)); cmd[p++] = chan; cmd[p++] = 0;
        PutLe16(cmd + p, 0); p += 2; PutLe16(cmd + p, 0); p += 2;

        if (ht) {
            UCHAR htc[26];
            USHORT capInfo = 0x000C | 0x1000;                 /* SM power save off, DSSS/CCK in 40 MHz */
            RtlZeroMemory(htc, sizeof(htc));
            if (is5 && (dc & (1u << 17))) capInfo |= 0x0002;  /* 20/40 MHz */
            if (dc & (1u << 23)) capInfo |= 0x0020;           /* short GI 20 */
            if ((dc & (1u << 24)) && (capInfo & 0x0002)) capInfo |= 0x0040;   /* short GI 40 */
            if (dc & (1u << 25)) capInfo |= 0x0080;           /* TX STBC */
            if (dc & (1u << 26)) capInfo |= 0x0100;           /* RX STBC, 1 stream */
            if (dc & (1u << 22)) capInfo |= 0x0001;           /* LDPC */
            PutLe16(htc, capInfo);
            htc[2] = 0x03;                                    /* A-MPDU 64 KB, no spacing */
            htc[3] = 0xFF;                                    /* MCS 0-7 */
            if ((ctx->DevMcsSupport & 0x0F) >= 2) htc[4] = 0xFF;   /* MCS 8-15 (2 streams) */
            if (capInfo & 0x0002) htc[7] |= 0x01;             /* MCS 32 */
            htc[15] = 0x01;                                   /* TX MCS set defined */
            PutLe16(cmd + p, 45); PutLe16(cmd + p + 2, sizeof(htc)); p += 4;
            RtlCopyMemory(cmd + p, htc, sizeof(htc)); p += sizeof(htc);
            reqFrame[reqLen++] = 45; reqFrame[reqLen++] = sizeof(htc);
            RtlCopyMemory(reqFrame + reqLen, htc, sizeof(htc)); reqLen += sizeof(htc);
            PutLe16(cmd + p, 61); PutLe16(cmd + p + 2, 22); p += 4;    /* HT operation, as the AP sent it */
            RtlCopyMemory(cmd + p, apHtOp + 2, 22);
            if (!secOff) cmd[p + 1] &= (UCHAR)~0x07;           /* we stay at 20 MHz */
            p += 22;
            w->Use40 = secOff != 0;
            w->Sgi = (capInfo & (w->Use40 ? 0x0040 : 0x0020)) != 0;
            w->Streams = (ctx->DevMcsSupport & 0x0F) >= 2 ? 2 : 1;
        }
        if (apWmm) {
            static const UCHAR wmm[7] = { 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00 };   /* WMM info, no U-APSD */
            PutLe16(cmd + p, 221); PutLe16(cmd + p + 2, sizeof(wmm)); p += 4;
            RtlCopyMemory(cmd + p, wmm, sizeof(wmm)); p += sizeof(wmm);
            reqFrame[reqLen++] = 221; reqFrame[reqLen++] = sizeof(wmm);
            RtlCopyMemory(reqFrame + reqLen, wmm, sizeof(wmm)); reqLen += sizeof(wmm);
        }
        w->Ht = ht;
        w->Wmm = apWmm != nullptr;
        LOG("wifi: 802.11n %s%s, WMM %s", ht ? "on" : "off", ht ? (secOff ? " 40 MHz" : " 20 MHz") : "",
            apWmm ? "on" : "off");
    }
    if (rsnLen) {
        PutLe16(cmd + p, 48); PutLe16(cmd + p + 2, (USHORT)(rsnLen - 2)); p += 4;   /* RSN IE TLV */
        RtlCopyMemory(cmd + p, rsnIe + 2, rsnLen - 2); p += rsnLen - 2;
        RtlCopyMemory(reqFrame + reqLen, rsnIe, rsnLen); reqLen += rsnLen;
    }

    st = CardCommand(ctx, HostCmd_CMD_802_11_ASSOCIATE, cmd, (USHORT)p, rsp, 1024, &rspLen);
    if (rspLen >= 6) {
        rcap = GetLe16(rsp);
        status = GetLe16(rsp + 2);
    }
    LOG("wifi: associate: 0x%08X, cap 0x%04X status %u aid 0x%04X (%u bytes)",
        st, rcap, status, rspLen >= 6 ? GetLe16(rsp + 4) : 0, rspLen);
    if (NT_SUCCESS(st) && rspLen >= 6 && status == 0 && rcap != 0xFFFF && rcap != 0xFFFE &&
        rcap != 0xFFFD && rcap != 0xFFFC) {
        assoc = WDI_ASSOC_STATUS_SUCCESS;
        RtlCopyMemory(w->Bssid, bssid, 6);
        w->Channel = chan;
        w->Is5GHz = is5;
        w->Rssi = rssi;
        w->GtkSet = FALSE;
        ctx->WrBitmap = 0;            /* re-read the free data ports before the first frame */
        InterlockedExchange(&w->Connected, 1);
    } else if (rcap == 0xFFFC) {
        assoc = WDI_ASSOC_STATUS_NO_ASSOC_RESPONSE;
    } else if (status != 0 && status != 0xFFFF) {
        assoc = WDI_ASSOC_STATUS_ASSOC_FAILED_BY_PEER;
    }

report:
    {
        WDI_INDICATION_ASSOCIATION_RESULT_LIST lst;
        WDI_ASSOCIATION_RESULT_CONTAINER res;
        WDI_PHY_TYPE phy = (assoc == WDI_ASSOC_STATUS_SUCCESS && w->Ht) ? WDI_PHY_TYPE_HT :
                           is5 ? WDI_PHY_TYPE_OFDM : WDI_PHY_TYPE_ERP;
        UINT8 *out = nullptr;
        ULONG outLen = 0;

        RtlCopyMemory(res.BSSID.Address, prm.PreferredBSSEntryList.ElementCount ?
                      prm.PreferredBSSEntryList.pElements[0].BSSID.Address : bssid, 6);
        res.AssociationResultParameters.AssociationStatus = assoc;
        res.AssociationResultParameters.StatusCode = status == 0xFFFF ? 1 : status;
        res.AssociationResultParameters.ReAssociation = FALSE;
        res.AssociationResultParameters.AuthAlgorithm = auth == WDI_AUTH_ALGO_UNKNOWN ? WDI_AUTH_ALGO_80211_OPEN : auth;
        res.AssociationResultParameters.UnicastCipherAlgorithm = ucast;
        res.AssociationResultParameters.MulticastDataCipherAlgorithm = mcast;
        res.AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
        res.AssociationResultParameters.FourAddressSupported = FALSE;
        res.AssociationResultParameters.PortAuthorized = FALSE;
        res.AssociationResultParameters.WMMQoSEnabled = (assoc == WDI_ASSOC_STATUS_SUCCESS && w->Wmm) ? TRUE : FALSE;
        res.AssociationResultParameters.DSInfo = WDI_DS_UNKNOWN;
        res.AssociationResultParameters.BandID = is5 ? WDI_BAND_ID_5000 : WDI_BAND_ID_2400;
        if (assoc == WDI_ASSOC_STATUS_SUCCESS) {
            res.Optional.AssociationRequestFrame_IsPresent = 1;
            res.AssociationRequestFrame.SimpleAssign(reqFrame, reqLen);
            res.Optional.AssociationResponseFrame_IsPresent = 1;
            res.AssociationResponseFrame.SimpleAssign(rsp, rspLen);
            res.Optional.BeaconProbeResponse_IsPresent = 1;
            res.BeaconProbeResponse.SimpleAssign(const_cast<UCHAR *>(body), bodyLen);
        }
        res.ActivePhyTypeList.SimpleAssign(&phy, 1);
        lst.AssociationResults.SimpleAssign(&res, 1);
        ns = GenerateWdiIndicationAssociationResult(&lst, 0, &w->Tlv, &outLen, &out);
        if (ns == NDIS_STATUS_SUCCESS && out) {
            Indicate(Dev, WDI_INDICATION_ASSOCIATION_RESULT, Hdr, 0, NDIS_STATUS_SUCCESS, out, outLen);
            FreeGenerated(out);
        } else {
            LOG("wdi: association result generate failed 0x%08X", ns);
        }
        res.AssociationRequestFrame.SimpleAssign(nullptr, 0);
        res.AssociationResponseFrame.SimpleAssign(nullptr, 0);
        res.BeaconProbeResponse.SimpleAssign(nullptr, 0);
        res.ActivePhyTypeList.SimpleAssign(nullptr, 0);
        lst.AssociationResults.SimpleAssign(nullptr, 0);
    }
    if (assoc == WDI_ASSOC_STATUS_SUCCESS)
        IndicateLinkState(w);
    Indicate(Dev, WDI_INDICATION_CONNECT_COMPLETE, Hdr, Hdr->TransactionId, NDIS_STATUS_SUCCESS, nullptr, 0);
    LOG("wifi: connect task done, association %s (%u)", assoc == WDI_ASSOC_STATUS_SUCCESS ? "OK" : "FAILED", (ULONG)assoc);
    CleanupParsedWdiTaskConnect(&prm);
    MrvlFree(cmd);
    MrvlFree(rsp);
    MrvlFree(reqFrame);
}

/* KEY_MATERIAL for one CCMP key (v1 or v2 format, per the firmware's key API). */
static NTSTATUS SetCcmpKey(PDEVICE_CONTEXT Ctx, PWIFI_CTX W, BOOLEAN Pairwise, UCHAR KeyIdx,
                           const UCHAR *Key, const UCHAR *Rsc)
{
    UCHAR c[64];
    USHORT n;
    RtlZeroMemory(c, sizeof(c));
    PutLe16(c, HostCmd_ACT_GEN_SET);
    if (Ctx->KeyApiMajor >= 2) {
        USHORT info = KEY_ENABLED;
        info |= Pairwise ? (KEY_UNICAST | KEY_TX_KEY | KEY_RX_KEY) : (KEY_MCAST | KEY_RX_KEY);
        if (!W->GtkSet) info |= KEY_DEFAULT;
        PutLe16(c + 2, TLV_TYPE_KEY_PARAM_V2);
        PutLe16(c + 4, 10 + 42);
        if (Pairwise) RtlCopyMemory(c + 6, W->Bssid, 6); else RtlFillMemory(c + 6, 6, 0xFF);
        c[12] = KeyIdx & 0xF;
        c[13] = KEY_TYPE_ID_AES;
        PutLe16(c + 14, info);
        if (Rsc) RtlCopyMemory(c + 16, Rsc, 6);           /* aes.pn[8] */
        PutLe16(c + 24, 16);                              /* aes.key_len */
        RtlCopyMemory(c + 26, Key, 16);                   /* aes.key[32] */
        n = 2 + 4 + 52;
    } else {
        PutLe16(c + 2, TLV_TYPE_KEY_MATERIAL);
        PutLe16(c + 4, 6 + 16);
        PutLe16(c + 6, KEY_TYPE_ID_AES);
        PutLe16(c + 8, (USHORT)(KEY_ENABLED | (Pairwise ? KEY_UNICAST : KEY_MCAST)));
        PutLe16(c + 10, 16);
        RtlCopyMemory(c + 12, Key, 16);
        n = 2 + 4 + 22;
    }
    return CardCommand(Ctx, HostCmd_CMD_802_11_KEY_MATERIAL, c, n, nullptr, 0, nullptr);
}

static void DoAddKeys(WDFDEVICE Dev, WIFIREQUEST Req, PWDI_MESSAGE_HEADER Hdr, ULONG InLen)
{
    PDEVICE_CONTEXT ctx = GetCtx(Dev);
    PWIFI_CTX w = GetWifi(Dev);
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS prm;
    NTSTATUS st = STATUS_SUCCESS;
    UINT32 i;

    if (ParseWdiSetAddCipherKeys(InLen - sizeof(WDI_MESSAGE_HEADER), reinterpret_cast<UINT8 *>(Hdr + 1),
                                 &w->Tlv, &prm) != NDIS_STATUS_SUCCESS) {
        LOG("wifi: add keys: parse failed");
        CompleteM3(Req, STATUS_INVALID_PARAMETER);
        return;
    }
    for (i = 0; i < prm.SetCipherKey.ElementCount; i++) {
        WDI_SET_ADD_CIPHER_KEYS_CONTAINER &k = prm.SetCipherKey.pElements[i];
        WDI_CIPHER_KEY_TYPE type = k.CipherKeyTypeInfo.KeyType;
        WDI_CIPHER_ALGORITHM alg = k.CipherKeyTypeInfo.CipherAlgorithm;
        UCHAR idx = k.Optional.CipherKeyID_IsPresent ? (UCHAR)k.CipherKeyID.CipherKeyID : 0;
        const UCHAR *rsc = k.Optional.ReceiveSequenceCount_IsPresent ? k.ReceiveSequenceCount.ReceiveSequenceCount : nullptr;
        NTSTATUS ks;
        if (alg == WDI_CIPHER_ALGO_CCMP && k.Optional.CCMPKey_IsPresent && k.CCMPKey.ElementCount >= 16 &&
            (type == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY || type == WDI_CIPHER_KEY_TYPE_GROUP_KEY)) {
            ks = SetCcmpKey(ctx, w, type == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY, idx, k.CCMPKey.pElements, rsc);
            if (type == WDI_CIPHER_KEY_TYPE_GROUP_KEY && NT_SUCCESS(ks))
                w->GtkSet = TRUE;
            LOG("wifi: %s key idx %u (CCMP, api v%u): 0x%08X",
                type == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY ? "pairwise" : "group", idx, ctx->KeyApiMajor, ks);
            if (!NT_SUCCESS(ks)) st = ks;
        } else {
            LOG("wifi: key type %u alg %u not supported - ignored", (ULONG)type, (ULONG)alg);
        }
    }
    CleanupParsedWdiSetAddCipherKeys(&prm);
    CompleteM3(Req, st);
}

/* ------------------------------------------------------------------ */
/* Serialised command worker                                           */
/* ------------------------------------------------------------------ */

static VOID CmdWorker(WDFWORKITEM Item)
{
    WDFDEVICE dev = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(Item));
    PDEVICE_CONTEXT ctx = GetCtx(dev);
    PWIFI_CTX w = GetWifi(dev);

    for (;;) {
        PCMD_ITEM it = nullptr;
        KIRQL irql;
        PWDI_MESSAGE_HEADER hdr;

        KeAcquireSpinLock(&w->CmdListLock, &irql);
        if (!IsListEmpty(&w->CmdList))
            it = CONTAINING_RECORD(RemoveHeadList(&w->CmdList), CMD_ITEM, Link);
        KeReleaseSpinLock(&w->CmdListLock, irql);
        if (!it)
            break;
        hdr = reinterpret_cast<PWDI_MESSAGE_HEADER>(it->Data);

        switch (it->Id) {
        case WDI_TASK_CONNECT:
            DoConnect(dev, it->Request, hdr, it->InLen);
            break;
        case WDI_TASK_DISCONNECT:
            CompleteM3(it->Request, STATUS_SUCCESS);
            if (w->Connected) {
                Deauthenticate(ctx, w);
                IndicateDisassociation(w, WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST);
            }
            Indicate(dev, WDI_INDICATION_DISCONNECT_COMPLETE, hdr, hdr->TransactionId, NDIS_STATUS_SUCCESS, nullptr, 0);
            break;
        case WDI_TASK_DOT11_RESET:
            CompleteM3(it->Request, STATUS_SUCCESS);
            if (w->Connected)
                Deauthenticate(ctx, w);
            Indicate(dev, WDI_INDICATION_DOT11_RESET_COMPLETE, hdr, hdr->TransactionId, NDIS_STATUS_SUCCESS, nullptr, 0);
            break;
        case WDI_SET_ADD_CIPHER_KEYS:
            DoAddKeys(dev, it->Request, hdr, it->InLen);
            break;
        default:
            CompleteM3(it->Request, STATUS_SUCCESS);
            break;
        }
        MrvlFree(it);
    }
}

static void QueueCommand(WDFDEVICE Device, WIFIREQUEST Request, UINT16 Id, PVOID Buf, UINT InLen)
{
    PWIFI_CTX w = GetWifi(Device);
    PCMD_ITEM it = static_cast<PCMD_ITEM>(MrvlAlloc(sizeof(CMD_ITEM) + InLen));
    KIRQL irql;
    if (!it) {
        CompleteM3(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    it->Request = Request;
    it->Id = Id;
    it->InLen = InLen;
    RtlCopyMemory(it->Data, Buf, InLen);
    KeAcquireSpinLock(&w->CmdListLock, &irql);
    InsertTailList(&w->CmdList, &it->Link);
    KeReleaseSpinLock(&w->CmdListLock, irql);
    WdfWorkItemEnqueue(w->CmdWork);
}

/* ------------------------------------------------------------------ */
/* Firmware events                                                    */
/* ------------------------------------------------------------------ */

extern "C" VOID WifiFwEvent(PDEVICE_CONTEXT Ctx, ULONG Event)
{
    PWIFI_CTX w = GetWifi(Ctx->Device);
    if (!w || !w->EventWork)
        return;
    if ((Event == EVENT_LINK_LOST || Event == EVENT_DEAUTHENTICATED || Event == EVENT_DISASSOCIATED) && w->Connected) {
        InterlockedExchange(&w->PendingEvent, (LONG)Event);
        WdfWorkItemEnqueue(w->EventWork);
    }
}

static VOID EventWorker(WDFWORKITEM Item)
{
    WDFDEVICE dev = static_cast<WDFDEVICE>(WdfWorkItemGetParentObject(Item));
    PWIFI_CTX w = GetWifi(dev);
    LONG ev = InterlockedExchange(&w->PendingEvent, 0);
    if (!ev || !InterlockedExchange(&w->Connected, 0))
        return;
    w->GtkSet = FALSE;
    IndicateDisassociation(w, ev == EVENT_LINK_LOST ? WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN :
                              ev == EVENT_DEAUTHENTICATED ? WDI_ASSOC_STATUS_PEER_DEAUTHENTICATED :
                              WDI_ASSOC_STATUS_PEER_DISASSOCIATED);
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                   */
/* ------------------------------------------------------------------ */

static bool IsSetProperty(UINT16 Id)
{
    switch (Id) {
    case WDI_SET_HOST_DETECT_ERROR: case WDI_SET_POWER_STATE: case WDI_SET_OPERATION_MODE:
    case WDI_SET_P2P_ADDITIONAL_IE: case WDI_SET_P2P_LISTEN_STATE: case WDI_SET_PRIVACY_EXEMPTION_LIST:
    case WDI_SET_DEFAULT_KEY_ID: case WDI_SET_CONNECTION_QUALITY: case WDI_SET_RECEIVE_PACKET_FILTER:
    case WDI_SET_NETWORK_LIST_OFFLOAD: case WDI_SET_RECEIVE_COALESCING: case WDI_SET_AUTO_POWER_SAVE:
    case WDI_SET_ADD_WOL_PATTERN: case WDI_SET_REMOVE_WOL_PATTERN: case WDI_SET_MULTICAST_LIST:
    case WDI_SET_ADD_PM_PROTOCOL_OFFLOAD: case WDI_SET_REMOVE_PM_PROTOCOL_OFFLOAD:
    case WDI_SET_ADAPTER_CONFIGURATION: case WDI_SET_CLEAR_RECEIVE_COALESCING:
    case WDI_SET_ADVERTISEMENT_INFORMATION: case WDI_SET_FLUSH_BSS_ENTRY:
    case WDI_SET_TCP_OFFLOAD_PARAMETERS: case WDI_SET_ENCAPSULATION_OFFLOAD: case WDI_SET_END_DWELL_TIME:
    case WDI_SET_NEIGHBOR_REPORT_ENTRIES: case WDI_SET_LOCATION_PRIVACY: case WDI_SET_DELETE_CIPHER_KEYS:
        return true;
    default:
        return false;
    }
}

static VOID EvtWifiSendCommand(WDFDEVICE Device, WIFIREQUEST Request)
{
    UINT inLen = 0, outLen = 0;
    PVOID buf = WifiRequestGetInOutBuffer(Request, &inLen, &outLen);
    UINT16 id = WifiRequestGetMessageId(Request);
    PWDI_MESSAGE_HEADER hdr = static_cast<PWDI_MESSAGE_HEADER>(buf);
    PWIFI_CTX w = GetWifi(Device);

    if (!buf || inLen < sizeof(WDI_MESSAGE_HEADER)) {
        CompleteM3(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    if (id != WDI_TASK_SCAN)
        LOG("wdi: msg %u port %u tx %u in %u", id, hdr->PortId, hdr->TransactionId, inLen);

    switch (id) {
    case WDI_TASK_SET_RADIO_STATE: {
        WDI_SET_RADIO_STATE_PARAMETERS prm;
        UCHAR on = w->RadioOn;
        if (ParseWdiTaskSetRadioState(inLen - sizeof(WDI_MESSAGE_HEADER), reinterpret_cast<UINT8 *>(hdr + 1),
                                      &w->Tlv, &prm) == NDIS_STATUS_SUCCESS) {
            on = prm.SoftwareRadioState ? 1 : 0;
            CleanupParsedWdiTaskSetRadioState(&prm);
        }
        CompleteM3(Request, STATUS_SUCCESS);
        if (on != w->RadioOn) {
            WDI_INDICATION_RADIO_STATUS_PARAMETERS rs;
            UINT8 *out = nullptr;
            ULONG outLen2 = 0;
            w->RadioOn = on;
            rs.RadioState.HardwareState = TRUE;
            rs.RadioState.SoftwareState = on;
            if (GenerateWdiIndicationRadioStatus(&rs, 0, &w->Tlv, &outLen2, &out) == NDIS_STATUS_SUCCESS) {
                Indicate(Device, WDI_INDICATION_RADIO_STATUS, hdr, 0, NDIS_STATUS_SUCCESS, out, outLen2);
                FreeGenerated(out);
            }
        }
        LOG("wdi: radio %s", on ? "on" : "off");
        Indicate(Device, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, hdr, hdr->TransactionId, NDIS_STATUS_SUCCESS, nullptr, 0);
        return;
    }
    case WDI_TASK_SET_READY_TO_RECEIVE_INDICATIONS:
        CompleteTrivialTask(Device, Request, hdr, WDI_INDICATION_SET_READY_TO_RECEIVE_INDICATIONS_COMPLETE);
        return;
    case WDI_TASK_SCAN:
        HandleScan(Device, Request, hdr, inLen);
        return;
    case WDI_TASK_CONNECT:
    case WDI_TASK_DISCONNECT:
    case WDI_TASK_DOT11_RESET:
    case WDI_SET_ADD_CIPHER_KEYS:
        QueueCommand(Device, Request, id, buf, inLen);
        return;
    default:
        if (IsSetProperty(id)) {
            CompleteM3(Request, STATUS_SUCCESS);          /* accepted, ignored for now */
        } else {
            LOG("wdi: msg %u not supported", id);
            CompleteM3(Request, STATUS_NOT_SUPPORTED);
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Frame conversion                                                   */
/* ------------------------------------------------------------------ */

static const UCHAR g_Rfc1042[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
static const UCHAR g_Tunnel[6]  = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8 };

/* 802.11 data frame (no FCS) -> Ethernet II. Returns the Ethernet length or 0. */
static ULONG Dot11ToEth(const UCHAR *In, ULONG InLen, PUCHAR Out, PUCHAR Prio)
{
    UCHAR fc0 = In[0], fc1 = In[1];
    ULONG hdr = 24;
    const UCHAR *da, *sa, *pl;
    ULONG plLen;

    *Prio = 0;
    if (InLen < 24 || ((fc0 >> 2) & 3) != 2)
        return 0;
    if ((fc1 & 3) == 3) hdr += 6;                  /* 4-address */
    if (fc0 & 0x80) {                              /* QoS data */
        *Prio = In[hdr] & 7;
        hdr += 2;
        if (fc1 & 0x80) hdr += 4;                  /* HT control */
    }
    if (InLen < hdr + 8)
        return 0;
    if (fc1 & 1) {                                 /* ToDS: A1 = BSSID, A2 = SA, A3 = DA */
        da = In + 16; sa = In + 10;
    } else {                                       /* A1 = DA, A2 = SA */
        da = In + 4; sa = In + 10;
    }
    pl = In + hdr;
    plLen = InLen - hdr;
    RtlCopyMemory(Out, da, 6);
    RtlCopyMemory(Out + 6, sa, 6);
    if (RtlEqualMemory(pl, g_Rfc1042, 6) || RtlEqualMemory(pl, g_Tunnel, 6)) {
        RtlCopyMemory(Out + 12, pl + 6, 2 + (plLen - 8));     /* ethertype + payload */
        return 12 + 2 + (plLen - 8);
    }
    PutLe16(Out + 12, 0);                          /* 802.3 length frame (big endian) */
    Out[12] = (UCHAR)(plLen >> 8); Out[13] = (UCHAR)plLen;
    RtlCopyMemory(Out + 14, pl, plLen);
    return 14 + plLen;
}

/* Ethernet II -> 802.11 data frame from the AP (FromDS). Returns the length or 0. */
static ULONG EthToDot11(const UCHAR *Eth, ULONG Len, const UCHAR *Bssid, PUCHAR Out)
{
    USHORT type;
    if (Len < 14 || Len - 14 + 24 + 8 > FRAME_MAX)
        return 0;
    type = (USHORT)((Eth[12] << 8) | Eth[13]);
    RtlZeroMemory(Out, 24);
    Out[0] = 0x08;                                 /* data */
    Out[1] = 0x02;                                 /* FromDS */
    RtlCopyMemory(Out + 4, Eth, 6);                /* A1 = DA */
    RtlCopyMemory(Out + 10, Bssid, 6);             /* A2 = BSSID */
    RtlCopyMemory(Out + 16, Eth + 6, 6);           /* A3 = SA */
    if (type < 0x0600) {                           /* 802.3 length frame: payload is already LLC */
        RtlCopyMemory(Out + 24, Eth + 14, Len - 14);
        return 24 + Len - 14;
    }
    RtlCopyMemory(Out + 24, (type == 0x80F3 || type == 0x8137) ? g_Tunnel : g_Rfc1042, 6);
    RtlCopyMemory(Out + 30, Eth + 12, Len - 12);   /* ethertype + payload */
    return 30 + Len - 12;
}

static void LogBytes(PCSTR What, const UCHAR *b, ULONG n)
{
    CHAR hex[3 * 32 + 1];
    ULONG i;
    if (n > 32) n = 32;
    for (i = 0; i < n; i++) {
        static const char d[] = "0123456789ABCDEF";
        hex[3 * i] = d[b[i] >> 4]; hex[3 * i + 1] = d[b[i] & 15]; hex[3 * i + 2] = ' ';
    }
    hex[3 * n] = 0;
    LOG("%s: %s", What, hex);
}

/* ------------------------------------------------------------------ */
/* Data path: TX                                                      */
/* ------------------------------------------------------------------ */

static VOID TxThreadMain(PVOID Context)
{
    PWIFI_CTX w = static_cast<PWIFI_CTX>(Context);
    PDEVICE_CONTEXT ctx = GetCtx(w->Device);

    while (!w->TxStop) {
        KeWaitForSingleObject(&w->TxEvent, Executive, KernelMode, FALSE, nullptr);
        while (!w->TxStop && w->TxTail != w->TxHead) {
            FRAME_SLOT *s = &w->TxSlots[w->TxTail % TX_SLOTS];
            if (w->Connected) {
                NTSTATUS st = CardTxFrame(ctx, s->Data, s->Len, s->Prio);
                if (NT_SUCCESS(st)) w->TxOk++; else w->TxDrop++;
            } else {
                w->TxDrop++;
            }
            InterlockedIncrement(&w->TxTail);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID EvtTxAdvance(NETPACKETQUEUE Queue)
{
    PQUEUE_CTX q = GetQueue(Queue);
    PWIFI_CTX w = q->W;
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    bool queued = false;

    for (UINT32 i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i)) {
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, i);
        ULONG len = 0, elen;
        UINT32 fi = pkt->FragmentIndex;
        UCHAR prio;
        if (pkt->Ignore)
            continue;
        for (UINT32 k = 0; k < pkt->FragmentCount; k++, fi = NetRingIncrementIndex(fr, fi)) {
            NET_FRAGMENT *f = NetRingGetFragmentAtIndex(fr, fi);
            PUCHAR va = static_cast<PUCHAR>(NetExtensionGetFragmentVirtualAddress(&q->Va, fi)->VirtualAddress);
            ULONG n = (ULONG)f->ValidLength;
            if (len + n > FRAME_MAX) { len = 0; break; }
            RtlCopyMemory(q->Scratch + len, va + f->Offset, n);
            len += n;
        }
        if (w->TxLogged < 4 && len) {
            w->TxLogged++;
            LogBytes("tx frame from Windows", q->Scratch, len);
        }
        if (!len || !w->Connected || (w->TxHead - w->TxTail) >= TX_SLOTS) {
            w->TxDrop++;
            continue;
        }
        {
            FRAME_SLOT *s = &w->TxSlots[w->TxHead % TX_SLOTS];
            elen = Dot11ToEth(q->Scratch, len, s->Data, &prio);
            if (!elen) {
                /* not an 802.11 data frame: maybe already Ethernet */
                if (len >= 14 && len <= FRAME_MAX && ((q->Scratch[12] << 8) | q->Scratch[13]) >= 0x0600) {
                    RtlCopyMemory(s->Data, q->Scratch, len);
                    elen = len;
                    prio = 0;
                } else {
                    w->TxDrop++;
                    continue;
                }
            }
            s->Len = (USHORT)elen;
            s->Prio = prio;
            InterlockedIncrement(&w->TxHead);
            queued = true;
        }
    }
    pr->BeginIndex = pr->EndIndex;     /* everything copied: complete the packets */
    fr->BeginIndex = fr->EndIndex;
    if (queued)
        KeSetEvent(&w->TxEvent, IO_NETWORK_INCREMENT, FALSE);
}

static VOID EvtTxCancel(NETPACKETQUEUE Queue)
{
    PQUEUE_CTX q = GetQueue(Queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    pr->BeginIndex = pr->EndIndex;
    fr->BeginIndex = fr->EndIndex;
}

static VOID EvtTxSetNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);      /* TX completes synchronously in Advance */
}

/* ------------------------------------------------------------------ */
/* Data path: RX                                                      */
/* ------------------------------------------------------------------ */

extern "C" VOID WifiRxFrame(PDEVICE_CONTEXT Ctx, PUCHAR Eth, ULONG Len)
{
    PWIFI_CTX w = GetWifi(Ctx->Device);
    KIRQL irql;
    bool notify = false;

    if (!w || !w->RxSlots || !w->Connected) {
        if (w) w->RxDrop++;
        return;
    }
    if (w->RxLogged < 4) {
        w->RxLogged++;
        LogBytes("rx frame from card (eth)", Eth, Len);
    }
    KeAcquireSpinLock(&w->RxLock, &irql);
    if (w->RxHead - w->RxTail < RX_SLOTS) {
        FRAME_SLOT *s = &w->RxSlots[w->RxHead % RX_SLOTS];
        ULONG n = EthToDot11(Eth, Len, w->Bssid, s->Data);
        if (n) {
            s->Len = (USHORT)n;
            w->RxHead++;
            w->RxOk++;
        } else {
            w->RxDrop++;
        }
    } else {
        w->RxDrop++;
    }
    KeReleaseSpinLock(&w->RxLock, irql);
    if (w->RxQueue && InterlockedExchange(&w->RxNotify, 0))
        notify = true;
    if (notify)
        NetRxQueueNotifyMoreReceivedPacketsAvailable(w->RxQueue);
}

static VOID EvtRxAdvance(NETPACKETQUEUE Queue)
{
    PQUEUE_CTX q = GetQueue(Queue);
    PWIFI_CTX w = q->W;
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    KIRQL irql;

    KeAcquireSpinLock(&w->RxLock, &irql);
    while (w->RxTail != w->RxHead && pr->BeginIndex != pr->EndIndex && fr->BeginIndex != fr->EndIndex) {
        FRAME_SLOT *s = &w->RxSlots[w->RxTail % RX_SLOTS];
        UINT32 fi = fr->BeginIndex;
        NET_FRAGMENT *f = NetRingGetFragmentAtIndex(fr, fi);
        PUCHAR va = static_cast<PUCHAR>(NetExtensionGetFragmentVirtualAddress(&q->Va, fi)->VirtualAddress);
        NET_PACKET *pkt = NetRingGetPacketAtIndex(pr, pr->BeginIndex);
        ULONG n = s->Len;
        if (n > f->Capacity) n = (ULONG)f->Capacity;
        RtlCopyMemory(va, s->Data, n);
        f->ValidLength = n;
        f->Offset = 0;
        pkt->FragmentIndex = fi;
        pkt->FragmentCount = 1;
        pkt->Layout = {};
        fr->BeginIndex = NetRingIncrementIndex(fr, fr->BeginIndex);
        pr->BeginIndex = NetRingIncrementIndex(pr, pr->BeginIndex);
        w->RxTail++;
    }
    KeReleaseSpinLock(&w->RxLock, irql);
}

static VOID EvtRxCancel(NETPACKETQUEUE Queue)
{
    PQUEUE_CTX q = GetQueue(Queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(q->Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(q->Rings);
    for (UINT32 i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i))
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    pr->BeginIndex = pr->EndIndex;
    fr->BeginIndex = fr->EndIndex;
}

static VOID EvtRxSetNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    PQUEUE_CTX q = GetQueue(Queue);
    PWIFI_CTX w = q->W;
    InterlockedExchange(&w->RxNotify, Enabled ? 1 : 0);
    if (Enabled && w->RxTail != w->RxHead && InterlockedExchange(&w->RxNotify, 0))
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
}

static NTSTATUS QueryVa(NETPACKETQUEUE Q, bool Tx, NET_EXTENSION *Ext)
{
    NET_EXTENSION_QUERY qq;
    NET_EXTENSION_QUERY_INIT(&qq, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    if (Tx) NetTxQueueGetExtension(Q, &qq, Ext); else NetRxQueueGetExtension(Q, &qq, Ext);
    return Ext->Enabled ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}


typedef struct _ADAPTER_CTX {
    PWIFI_CTX W;
} ADAPTER_CTX, *PADAPTER_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ADAPTER_CTX, GetAdapterCtx)

static NTSTATUS EvtCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    PWIFI_CTX w = GetAdapterCtx(Adapter)->W;
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NETPACKETQUEUE q;
    PQUEUE_CTX qc;
    NTSTATUS st;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtTxAdvance, EvtTxSetNotify, EvtTxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, QUEUE_CTX);
    st = NetTxQueueCreate(Init, &attr, &cfg, &q);
    if (!NT_SUCCESS(st)) {
        LOG("NetTxQueueCreate failed 0x%08X", st);
        return st;
    }
    qc = GetQueue(q);
    qc->W = w;
    qc->Rings = NetTxQueueGetRingCollection(q);
    qc->Scratch = static_cast<PUCHAR>(MrvlAlloc(FRAME_MAX));
    if (!qc->Scratch)
        return STATUS_INSUFFICIENT_RESOURCES;
    st = QueryVa(q, true, &qc->Va);
    if (!NT_SUCCESS(st)) {
        LOG("tx queue: no virtual address extension");
        return st;
    }
    LOG("tx queue created");
    return STATUS_SUCCESS;
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    PWIFI_CTX w = GetAdapterCtx(Adapter)->W;
    NET_PACKET_QUEUE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    NETPACKETQUEUE q;
    PQUEUE_CTX qc;
    NTSTATUS st;

    NET_PACKET_QUEUE_CONFIG_INIT(&cfg, EvtRxAdvance, EvtRxSetNotify, EvtRxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, QUEUE_CTX);
    st = NetRxQueueCreate(Init, &attr, &cfg, &q);
    if (!NT_SUCCESS(st)) {
        LOG("NetRxQueueCreate failed 0x%08X", st);
        return st;
    }
    qc = GetQueue(q);
    qc->W = w;
    qc->Rings = NetRxQueueGetRingCollection(q);
    st = QueryVa(q, false, &qc->Va);
    if (!NT_SUCCESS(st)) {
        LOG("rx queue: no virtual address extension");
        return st;
    }
    w->RxQueue = q;
    LOG("rx queue created");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Adapter (the station "port")                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS EvtWifiCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    PDEVICE_CONTEXT ctx = GetCtx(Device);
    PWIFI_CTX w = GetWifi(Device);
    NET_ADAPTER_DATAPATH_CALLBACKS dp;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES ll;
    NET_ADAPTER_TX_CAPABILITIES tx;
    NET_ADAPTER_RX_CAPABILITIES rx;
    NET_ADAPTER_LINK_LAYER_ADDRESS mac;
    WDF_OBJECT_ATTRIBUTES attr;
    NETADAPTER adapter;
    NTSTATUS st;
    WIFI_ADAPTER_TYPE type = WifiAdapterInitGetType(AdapterInit);

    LOG("wifi: create adapter, type %d", (int)type);
    if (type != WIFI_ADAPTER_EXTENSIBLE_STATION)
        return STATUS_NOT_SUPPORTED;

    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&dp, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &dp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, ADAPTER_CTX);
    st = NetAdapterCreate(AdapterInit, &attr, &adapter);
    if (!NT_SUCCESS(st)) {
        LOG("NetAdapterCreate failed 0x%08X", st);
        return st;
    }
    GetAdapterCtx(adapter)->W = w;
    st = WifiAdapterInitialize(adapter);
    if (!NT_SUCCESS(st)) {
        LOG("WifiAdapterInitialize failed 0x%08X", st);
        return st;
    }

    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&ll, 300000000ULL, 300000000ULL);
    NetAdapterSetLinkLayerCapabilities(adapter, &ll);
    NetAdapterSetLinkLayerMtuSize(adapter, 1500);
    NET_ADAPTER_TX_CAPABILITIES_INIT(&tx, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rx, FRAME_MAX, 1);
    NetAdapterSetDataPathCapabilities(adapter, &tx, &rx);
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&mac, 6, ctx->Mac);
    NetAdapterSetPermanentLinkLayerAddress(adapter, &mac);
    NetAdapterSetCurrentLinkLayerAddress(adapter, &mac);

    st = NetAdapterStart(adapter);
    if (!NT_SUCCESS(st)) {
        LOG("NetAdapterStart failed 0x%08X", st);
        return st;
    }
    w->Adapter = adapter;
    LOG("wifi: station adapter started, port %u", (ULONG)WifiAdapterGetPortId(adapter));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtWifiCreateWfdDevice(WDFDEVICE Device, WIFIDIRECT_DEVICE_INIT *Init)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Init);
    return STATUS_NOT_SUPPORTED;    /* no Wi-Fi Direct */
}

/* ------------------------------------------------------------------ */
/* Device init + capabilities                                         */
/* ------------------------------------------------------------------ */

extern "C" NTSTATUS WifiDeviceInitPre(PWDFDEVICE_INIT Init)
{
    NTSTATUS st = NetDeviceInitConfig(Init);
    if (!NT_SUCCESS(st)) {
        LOG("NetDeviceInitConfig failed 0x%08X", st);
        return st;
    }
    st = WifiDeviceInitConfig(Init);
    if (!NT_SUCCESS(st))
        LOG("WifiDeviceInitConfig failed 0x%08X", st);
    return st;
}

static VOID WifiCtxCleanup(WDFOBJECT Object)
{
    PWIFI_CTX w = GetWifi(static_cast<WDFDEVICE>(Object));
    if (!w)
        return;
    if (w->TxThread) {
        InterlockedExchange(&w->TxStop, 1);
        KeSetEvent(&w->TxEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(w->TxThread, Executive, KernelMode, FALSE, nullptr);
        ObDereferenceObject(w->TxThread);
        w->TxThread = nullptr;
    }
    MrvlFree(w->TxSlots); w->TxSlots = nullptr;
    MrvlFree(w->RxSlots); w->RxSlots = nullptr;
    {
        KIRQL irql;
        KeAcquireSpinLock(&w->CmdListLock, &irql);
        while (!IsListEmpty(&w->CmdList))
            MrvlFree(CONTAINING_RECORD(RemoveHeadList(&w->CmdList), CMD_ITEM, Link));
        KeReleaseSpinLock(&w->CmdListLock, irql);
    }
}

static NTSTATUS MakeWorkItem(WDFDEVICE Device, PFN_WDF_WORKITEM Fn, WDFWORKITEM *Out)
{
    WDF_WORKITEM_CONFIG wcfg;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_WORKITEM_CONFIG_INIT(&wcfg, Fn);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Device;
    return WdfWorkItemCreate(&wcfg, &attr, Out);
}

extern "C" NTSTATUS WifiDeviceInitPost(WDFDEVICE Device)
{
    WIFI_DEVICE_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    PVOID mem;
    PWIFI_CTX w;
    NTSTATUS st;
    HANDLE th;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, WIFI_CTX);
    attr.EvtCleanupCallback = WifiCtxCleanup;
    st = WdfObjectAllocateContext(Device, &attr, &mem);
    if (!NT_SUCCESS(st))
        return st;
    w = static_cast<PWIFI_CTX>(mem);
    w->Device = Device;
    w->RadioOn = 1;
    KeInitializeSpinLock(&w->CmdListLock);
    InitializeListHead(&w->CmdList);
    KeInitializeSpinLock(&w->TxLock);
    KeInitializeSpinLock(&w->RxLock);
    KeInitializeEvent(&w->TxEvent, SynchronizationEvent, FALSE);
    w->TxSlots = static_cast<FRAME_SLOT *>(MrvlAlloc(sizeof(FRAME_SLOT) * TX_SLOTS));
    w->RxSlots = static_cast<FRAME_SLOT *>(MrvlAlloc(sizeof(FRAME_SLOT) * RX_SLOTS));
    if (!w->TxSlots || !w->RxSlots)
        return STATUS_INSUFFICIENT_RESOURCES;

    WIFI_DEVICE_CONFIG_INIT(&cfg, WDI_VERSION_LATEST, EvtWifiSendCommand, EvtWifiCreateAdapter, EvtWifiCreateWfdDevice);
    st = WifiDeviceInitialize(Device, &cfg);
    if (!NT_SUCCESS(st)) {
        LOG("WifiDeviceInitialize failed 0x%08X", st);
        return st;
    }
    w->Tlv.AllocationContext = 0;
    w->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(Device);
    LOG("wifi: WiFiCx initialised, OS WDI version 0x%X", w->Tlv.PeerVersion);

    if (!NT_SUCCESS(st = MakeWorkItem(Device, ScanWorker, &w->ScanWork)) ||
        !NT_SUCCESS(st = MakeWorkItem(Device, CmdWorker, &w->CmdWork)) ||
        !NT_SUCCESS(st = MakeWorkItem(Device, EventWorker, &w->EventWork))) {
        LOG("work item create failed 0x%08X", st);
        return st;
    }

    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, TxThreadMain, w);
    if (!NT_SUCCESS(st)) {
        LOG("tx thread create failed 0x%08X", st);
        return st;
    }
    (VOID)ObReferenceObjectByHandle(th, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &w->TxThread, nullptr);
    ZwClose(th);
    return STATUS_SUCCESS;
}

static const WDI_CHANNEL_MAPPING_ENTRY g_Chan24[] = {
    {1, 2412}, {2, 2417}, {3, 2422}, {4, 2427}, {5, 2432}, {6, 2437}, {7, 2442},
    {8, 2447}, {9, 2452}, {10, 2457}, {11, 2462}, {12, 2467}, {13, 2472},
};
static const WDI_CHANNEL_MAPPING_ENTRY g_Chan5[] = {
    {36, 5180}, {40, 5200}, {44, 5220}, {48, 5240},
    {149, 5745}, {153, 5765}, {157, 5785}, {161, 5805}, {165, 5825},
};

extern "C" NTSTATUS WifiReportCapabilities(PDEVICE_CONTEXT Ctx)
{
    WDFDEVICE dev = Ctx->Device;
    NTSTATUS st;

    /* --- device --- */
    {
        WIFI_DEVICE_CAPABILITIES c;
        size_t n = strlen(Ctx->FwVersion);
        WIFI_DEVICE_CAPABILITIES_INIT(&c);
        c.HardwareRadioState = TRUE;
        c.SoftwareRadioState = TRUE;
        if (n >= MAX_FIRMWARE_VERSION_LENGTH) n = MAX_FIRMWARE_VERSION_LENGTH - 1;
        RtlCopyMemory(c.FirmwareVersion, Ctx->FwVersion, n);
        c.ActionFramesSupported = FALSE;
        c.NumRxStreams = 2;               /* 88W8897: 2x2 */
        c.NumTxStreams = 2;
        c.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
        st = WifiDeviceSetDeviceCapabilities(dev, &c);
        if (!NT_SUCCESS(st)) { LOG("WifiDeviceSetDeviceCapabilities 0x%08X", st); return st; }
    }

    /* --- station: what Connect actually implements (open, WPA2-PSK/802.1X with CCMP) --- */
    {
        static DOT11_AUTH_CIPHER_PAIR uni[] = {
            {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
            {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP},
            {DOT11_AUTH_ALGO_RSNA, DOT11_CIPHER_ALGO_CCMP},
        };
        static DOT11_AUTH_CIPHER_PAIR mcast[] = {
            {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
            {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP},
            {DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_TKIP},
            {DOT11_AUTH_ALGO_RSNA, DOT11_CIPHER_ALGO_CCMP},
            {DOT11_AUTH_ALGO_RSNA, DOT11_CIPHER_ALGO_TKIP},
        };
        static DOT11_AUTH_CIPHER_PAIR mcastMgmt[] = {
            {DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE},
        };
        WIFI_STATION_CAPABILITIES c;
        WIFI_STATION_CAPABILITIES_INIT(&c);
        c.ScanSSIDListSize = 1;
        c.DesiredSSIDListSize = 1;
        c.PrivacyExemptionListSize = 1;
        c.KeyMappingTableSize = 4;
        c.DefaultKeyTableSize = 4;
        c.WEPKeyValueMaxLength = 13;
        c.MaxNumPerSTA = 4;
        c.SupportedQOSFlags = 1;          /* WMM */
        c.HostFIPSModeImplemented = FALSE;
        c.MFPCapable = FALSE;
        c.AutoPowerSaveMode = FALSE;
        c.BSSListCachemanagement = FALSE;
        c.ConnectBSSSelectionOverride = FALSE;
        c.NumSupportedUnicastAlgorithms = ARRAYSIZE(uni);
        c.UnicastAlgorithmsList = uni;
        c.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(mcast);
        c.MulticastDataAlgorithmsList = mcast;
        c.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(mcastMgmt);
        c.MulticastMgmtAlgorithmsList = mcastMgmt;
        st = WifiDeviceSetStationCapabilities(dev, &c);
        if (!NT_SUCCESS(st)) { LOG("WifiDeviceSetStationCapabilities 0x%08X", st); return st; }
    }

    /* --- bands --- */
    {
        static WDI_PHY_TYPE phy24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
        static WDI_PHY_TYPE phy5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT };
        static UINT32 w20[] = { 20 };
        static UINT32 w40[] = { 20, 40 };
        WIFI_BAND_INFO bands[2] = {};
        WIFI_BAND_CAPABILITIES c;
        RtlZeroMemory(&c, sizeof(c));
        c.Size = sizeof(c);
        bands[0].BandID = WDI_BAND_ID_2400;
        bands[0].BandState = TRUE;
        bands[0].NumValidPhyTypes = ARRAYSIZE(phy24);
        bands[0].ValidPhyTypeList = phy24;
        bands[0].NumValidChannelTypes = ARRAYSIZE(g_Chan24);
        bands[0].ValidChannelTypes = const_cast<WDI_CHANNEL_MAPPING_ENTRY *>(g_Chan24);
        bands[0].NumChannelWidths = ARRAYSIZE(w20);
        bands[0].ChannelWidthList = w20;
        bands[1].BandID = WDI_BAND_ID_5000;
        bands[1].BandState = TRUE;
        bands[1].NumValidPhyTypes = ARRAYSIZE(phy5);
        bands[1].ValidPhyTypeList = phy5;
        bands[1].NumValidChannelTypes = ARRAYSIZE(g_Chan5);
        bands[1].ValidChannelTypes = const_cast<WDI_CHANNEL_MAPPING_ENTRY *>(g_Chan5);
        bands[1].NumChannelWidths = ARRAYSIZE(w40);
        bands[1].ChannelWidthList = w40;
        c.NumBands = 2;
        c.BandInfoList = bands;
        st = WifiDeviceSetBandCapabilities(dev, &c);
        if (!NT_SUCCESS(st)) { LOG("WifiDeviceSetBandCapabilities 0x%08X", st); return st; }
    }

    /* --- PHYs (rates in 500 kbps units) --- */
    {
        static const UINT16 rB[] = { 2, 4, 11, 22 };
        static const UINT16 rOfdm[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
        /* HT MCS 0-15, 20 MHz long GI, plus the 2-stream SGI / 40 MHz tops (500 kbps units) */
        static const UINT16 rHt[] = { 13, 26, 39, 52, 78, 104, 117, 130, 156, 208, 234, 260, 289, 540, 600 };
        struct { WDI_PHY_TYPE t; const UINT16 *r; ULONG n; } def[] = {
            { WDI_PHY_TYPE_HRDSSS, rB, ARRAYSIZE(rB) },
            { WDI_PHY_TYPE_ERP, rOfdm, ARRAYSIZE(rOfdm) },
            { WDI_PHY_TYPE_OFDM, rOfdm, ARRAYSIZE(rOfdm) },
            { WDI_PHY_TYPE_HT, rHt, ARRAYSIZE(rHt) },
        };
        WIFI_PHY_INFO *info = static_cast<WIFI_PHY_INFO *>(MrvlAlloc(sizeof(WIFI_PHY_INFO) * ARRAYSIZE(def)));
        WIFI_PHY_CAPABILITIES c;
        ULONG i, j;
        if (!info)
            return STATUS_INSUFFICIENT_RESOURCES;
        for (i = 0; i < ARRAYSIZE(def); i++) {
            info[i].PhyType = def[i].t;
            info[i].NumberDataRateEntries = def[i].n;
            for (j = 0; j < def[i].n; j++) {
                info[i].DataRateList[j].DataRateFlag = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
                info[i].DataRateList[j].DataRateValue = def[i].r[j];
            }
        }
        RtlZeroMemory(&c, sizeof(c));
        c.Size = sizeof(c);
        c.NumPhyTypes = ARRAYSIZE(def);
        c.PhyInfoList = info;
        st = WifiDeviceSetPhyCapabilities(dev, &c);
        MrvlFree(info);
        if (!NT_SUCCESS(st)) { LOG("WifiDeviceSetPhyCapabilities 0x%08X", st); return st; }
    }

    LOG("wifi: capabilities reported (MAC %02X:%02X:%02X:%02X:%02X:%02X, key api v%u)",
        Ctx->Mac[0], Ctx->Mac[1], Ctx->Mac[2], Ctx->Mac[3], Ctx->Mac[4], Ctx->Mac[5], Ctx->KeyApiMajor);
    return STATUS_SUCCESS;
}
