/*
 * mrvl8897 - Windows KMDF SDIO function driver for Marvell 88W8897 (WLAN function)
 *
 * Register map, firmware download protocol and host-command framing are ported
 * from the Linux mwifiex driver (drivers/net/wireless/marvell/mwifiex/sdio.[ch],
 * fw.h, init.c), which is distributed under the GNU GPL v2. This file is
 * therefore also GPL v2.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntddsd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MRVL_TAG 'v8rM'

#define MRVL_FW_PATH L"\\SystemRoot\\System32\\drivers\\sd8897_uapsta.bin"

/* ---- mwifiex: decl.h / main.h / fw.h ---- */
#define SDIO_BLOCK_SIZE             256
#define MWIFIEX_UPLD_SIZE           2312
#define MWIFIEX_RX_DATA_BUF_SIZE    4096
#define INTF_HEADER_LEN             4
#define MAX_POLL_TRIES              100
#define MAX_FIRMWARE_POLL_TRIES     150
#define FIRMWARE_READY_SDIO         0xfedc
#define MAX_WRITE_IOMEM_RETRY       2

#define MWIFIEX_TYPE_DATA           0
#define MWIFIEX_TYPE_CMD            1
#define MWIFIEX_TYPE_EVENT          3

#define HostCmd_CMD_GET_HW_SPEC     0x0003
#define HostCmd_CMD_VERSION_EXT     0x0097
#define HostCmd_CMD_FUNC_INIT       0x00a9
#define HostCmd_CMD_FUNC_SHUTDOWN   0x00aa
#define HostCmd_RET_BIT             0x8000
#define S_DS_GEN                    8

/* ---- mwifiex: sdio.h ---- */
#define CONFIGURATION_REG           0x00
#define CMD53_NEW_MODE              (1u << 0)
#define CMD_PORT_RD_LEN_EN          (1u << 2)
#define CMD_PORT_AUTO_EN            (1u << 0)
#define CMD_PORT_SLCT               0x8000
#define MEM_PORT                    0x10000
#define REG_PORT                    0
#define UP_LD_HOST_INT_MASK         0x01
#define DN_LD_HOST_INT_MASK         0x02
#define CMD_PORT_UPLD_INT_MASK      0x40
#define CMD_PORT_DNLD_INT_MASK      0x80
#define UP_LD_HOST_INT_STATUS       0x01
#define DN_LD_HOST_INT_STATUS       0x02
#define UP_LD_CMD_PORT_HOST_INT_STATUS 0x40
#define DN_LD_CMD_PORT_HOST_INT_STATUS 0x80
#define AUTO_RE_ENABLE_INT          (1u << 4)
#define CARD_IO_READY               (1u << 3)
#define DN_LD_CARD_RDY              (1u << 0)

/* ---- mwifiex: sdio.c, mwifiex_reg_sd8897 / mwifiex_sdio_sd8897 ---- */
#define R8897_BASE_0                0x60
#define R8897_BASE_1                0x61
#define R8897_POLL                  0x50
#define R8897_HOST_INT_ENABLE       (UP_LD_HOST_INT_MASK | DN_LD_HOST_INT_MASK | \
                                     CMD_PORT_UPLD_INT_MASK | CMD_PORT_DNLD_INT_MASK)
#define R8897_HOST_INT_RSR          0x01
#define R8897_HOST_INT_STATUS       0x03
#define R8897_HOST_INT_MASK         0x02
#define R8897_STATUS_0              0xc0
#define R8897_STATUS_1              0xc1
#define R8897_SDIO_INT_MASK         0xff
#define R8897_MAX_MP_REGS           184
#define R8897_RD_BITMAP_L           0x04
#define R8897_RD_BITMAP_U           0x05
#define R8897_RD_BITMAP_1L          0x06
#define R8897_RD_BITMAP_1U          0x07
#define R8897_WR_BITMAP_L           0x08
#define R8897_WR_BITMAP_U           0x09
#define R8897_WR_BITMAP_1L          0x0a
#define R8897_WR_BITMAP_1U          0x0b
#define R8897_RD_LEN_P0_L           0x0c
#define R8897_RD_LEN_P0_U           0x0d
#define R8897_CARD_MISC_CFG         0xcc
#define R8897_CARD_CFG_2_1          0xcd
#define R8897_CMD_RD_LEN_0          0xb4
#define R8897_CMD_RD_LEN_1          0xb5
#define R8897_CMD_CFG_0             0xb8
#define R8897_CMD_CFG_1             0xb9
#define R8897_MAX_PORTS             32

#define OPT_NO_FW_PACING 0x200   /* SdioFlags: no delays around firmware chunks */
#define CMD_RESP_MAX                4096
#define BA_QUEUE_LEN                8
#define BA_RX_WIN_SIZE              64      /* mwifiex MWIFIEX_STA_AMPDU_DEF_RXWINSIZE */

/* One scan result: BSSID, signal, channel and the beacon/probe-response body
 * (timestamp, beacon interval, capability, IEs) as WDI wants it. */
#define SCAN_MAX_BSS                48
#define SCAN_MAX_BODY               1024
typedef struct _MRVL_BSS {
    UCHAR   Bssid[6];
    UCHAR   Channel;
    UCHAR   Is5GHz;
    LONG    Rssi;             /* dBm (negative) */
    USHORT  BodyLen;
    UCHAR   Body[SCAN_MAX_BODY];
} MRVL_BSS, *PMRVL_BSS;

typedef struct _DEVICE_CONTEXT {
    PVOID                    WdfTriageInfoPtr;   /* must be first (NetAdapterCx crash-dump carving) */
    WDFDEVICE                Device;
    SDBUS_INTERFACE_STANDARD Bus;
    BOOLEAN                  BusOpen;
    BOOLEAN                  IntEnabled;
    BOOLEAN                  FwReady;
    BOOLEAN                  SdioFailed;

    UCHAR                    Func;
    UCHAR                    BusWidth;
    USHORT                   BlockLen;
    USHORT                   BusDriverVersion;
    ULONG                    BusClock;
    NTSTATUS                 SetBlockLenStatus;
    USHORT                   SdioFlags;
    BOOLEAN                  WantInterrupts;
    ULONG                    Options;
    NTSTATUS                 IoEnableStatus;

    /* raw card probe (v0.4) */
    CHAR                     ProbeWhen[32];
    NTSTATUS                 ProbeFuncStatus;
    NTSTATUS                 CccrReadStatus;
    UCHAR                    Cccr[0x14];
    NTSTATUS                 Fn1ReadStatus;
    USHORT                   FwStatusReg;
    UCHAR                    Fn1PollReg;
    NTSTATUS                 LowerStartStatus;
    CHAR                     ExpStage[32];
    NTSTATUS                 ExpStatus;
    ULONG                    IoPort;

    WDFWAITLOCK              IoLock;      /* serialises card transactions */
    WDFWAITLOCK              CmdLock;     /* one host command in flight at a time */

    /* boot-time self re-enumeration (see driver.c) */
    BOOLEAN                  NeedReenum;
    BOOLEAN                  NeedRestart;   /* answer START with STATUS_PNP_REBOOT_REQUIRED */
    BOOLEAN                  ReenumValid;
    BOOLEAN                  CountedAttempt;
    BOOLEAN                  PrepareRan;  /* EvtDevicePrepareHardware was reached */
    ULONG                    ReenumCount; /* re-detections done this boot before this instance */
    REENUMERATE_SELF_INTERFACE_STANDARD Reenum;
    KEVENT                   CmdDone;

    PUCHAR                   MpRegs;      /* R8897_MAX_MP_REGS, nonpaged */
    PUCHAR                   TxBuf;       /* 4K, nonpaged */
    PUCHAR                   RxBuf;       /* 4K, nonpaged */

    USHORT                   SeqNum;
    USHORT                   PendingCmd;
    ULONG                    RespLen;
    ULONG                    ScanBssCount;
    PMRVL_BSS                ScanResults; /* SCAN_MAX_BSS entries, nonpaged */
    PUCHAR                   DataTxBuf;   /* 4K, nonpaged: one outgoing data frame */
    ULONG                    WrBitmap;    /* free card write (data) ports */
    ULONG                    CurrWrPort;
    UCHAR                    KeyApiMajor; /* 0/1 = KEY_MATERIAL v1, 2 = v2 */
    ULONG                    FwCapInfo;   /* GET_HW_SPEC: bit 11 = 11n, bit 13 = 11ac */
    ULONG                    Dot11nDevCap;
    UCHAR                    DevMcsSupport; /* low nibble = RX streams */
    ULONG                    Dot11acDevCap;
    UCHAR                    Resp[CMD_RESP_MAX];

    /* block-ack (802.11n aggregation) firmware events, answered from BaWork */
    KSPIN_LOCK               BaLock;
    WDFWORKITEM              BaWork;
    ULONG                    BaHead, BaTail;
    struct { ULONG Event; UCHAR Body[16]; } BaQueue[BA_QUEUE_LEN];
    volatile LONG            BaSessions;

    volatile LONG            IntCallbacks;
    volatile LONG            Events;
    ULONG                    LastEvent;

    UCHAR                    Mac[6];
    ULONG                    FwRelease;
    CHAR                     FwVersion[129];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetCtx)

VOID MrvlLogInit(VOID);
VOID MrvlLog(PCSTR Fmt, ...);
VOID MrvlLogDumpToKey(WDFKEY Key);
VOID MrvlLogStartFlusher(WDFDRIVER Driver);
VOID MrvlLogStopFlusher(VOID);
#define LOG(fmt, ...) MrvlLog(fmt, __VA_ARGS__)

/* sdio.c */
NTSTATUS SdioAttach(PDEVICE_CONTEXT Ctx, USHORT SdioFlags, BOOLEAN WantInterrupts, PCSTR *Stage);
NTSTATUS SdioConfigure(PDEVICE_CONTEXT Ctx, PCSTR *Stage);
VOID     SdioStop(PDEVICE_CONTEXT Ctx);
VOID     SdioDetach(PDEVICE_CONTEXT Ctx);
VOID     SdioProbe(PDEVICE_CONTEXT Ctx, PCSTR When);
NTSTATUS SdioEnableFunctionManually(PDEVICE_CONTEXT Ctx);
NTSTATUS SdioReadReg(PDEVICE_CONTEXT Ctx, ULONG Reg, PUCHAR Val);
NTSTATUS SdioWriteReg(PDEVICE_CONTEXT Ctx, ULONG Reg, UCHAR Val);
NTSTATUS SdioReadBytes(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len);
NTSTATUS SdioReadBlocks(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len);
NTSTATUS SdioWriteBlocks(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len);
NTSTATUS SdioSetIntEnable(PDEVICE_CONTEXT Ctx, BOOLEAN Enable);

/* card.c */
NTSTATUS CardBringUp(PDEVICE_CONTEXT Ctx, PCSTR *FailStage);
NTSTATUS CardShutdown(PDEVICE_CONTEXT Ctx);
NTSTATUS SdioIoReset(PDEVICE_CONTEXT Ctx);
VOID     CardProcessInterrupt(PDEVICE_CONTEXT Ctx);
NTSTATUS CardScan(PDEVICE_CONTEXT Ctx, const UCHAR *Ssid, ULONG SsidLen);   /* PASSIVE; fills ScanResults */
NTSTATUS CardCommand(PDEVICE_CONTEXT Ctx, USHORT Cmd, const VOID *Payload, USHORT Len,
                     PUCHAR Out, ULONG OutMax, PULONG OutLen);
VOID     CardBaWorker(WDFWORKITEM Item);   /* answers ADDBA / BA timeout events */
NTSTATUS CardTxFrame(PDEVICE_CONTEXT Ctx, const UCHAR *Eth, ULONG Len, UCHAR Priority);

/* wifi.cpp (WiFiCx / NetAdapterCx glue) */
NTSTATUS WifiDeviceInitPre(PWDFDEVICE_INIT Init);          /* before WdfDeviceCreate */
NTSTATUS WifiDeviceInitPost(WDFDEVICE Device);             /* after WdfDeviceCreate */
NTSTATUS WifiReportCapabilities(PDEVICE_CONTEXT Ctx);      /* after firmware is up */
VOID     WifiRxFrame(PDEVICE_CONTEXT Ctx, PUCHAR Eth, ULONG Len);   /* Ethernet II frame from the card; PASSIVE, IoLock held */
VOID     WifiFwEvent(PDEVICE_CONTEXT Ctx, ULONG Event);            /* firmware event; PASSIVE, IoLock held */

#ifdef __cplusplus
}
#endif
