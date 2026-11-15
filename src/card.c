/*
 * 88W8897 card bring-up: SDIO new-mode init, firmware download, interrupt
 * status handling, host command channel.
 * Ported from Linux mwifiex (sdio.c, init.c, cmdevt.c). GPL v2.
 */
#include "mrvl8897.h"

static VOID SleepMs(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* Delay between register polls. KeDelayExecutionThread rounds up to the
 * ~15.6 ms timer tick, which made the firmware download take ~19 s, so
 * busy-wait 20 us for the first 250 polls (~5 ms, like Linux udelay), then
 * sleep 1 ms per poll so long waits don't burn the CPU. */
static VOID PollDelay(ULONG Try)
{
    if (Try < 250)
        KeStallExecutionProcessor(20);
    else
        SleepMs(1);
}

static USHORT GetLe16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }
static ULONG  GetLe32(const UCHAR *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }
static VOID   PutLe16(UCHAR *p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }

static NTSTATUS RegSetBits(PDEVICE_CONTEXT Ctx, ULONG Reg, UCHAR Bits)
{
    UCHAR v;
    NTSTATUS st = SdioReadReg(Ctx, Reg, &v);
    if (!NT_SUCCESS(st))
        return st;
    return SdioWriteReg(Ctx, Reg, v | Bits);
}

/* ------------------------------------------------------------------ */
/* Firmware file                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS LoadFirmware(PUCHAR *Image, PULONG Size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION fsi;
    HANDLE h;
    NTSTATUS st;
    PUCHAR buf;

    *Image = NULL;
    *Size = 0;
    RtlInitUnicodeString(&path, MRVL_FW_PATH);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    st = ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                      NULL, 0);
    if (!NT_SUCCESS(st)) {
        LOG("cannot open firmware %wZ: 0x%08X", &path, st);
        return st;
    }

    st = ZwQueryInformationFile(h, &iosb, &fsi, sizeof(fsi), FileStandardInformation);
    if (!NT_SUCCESS(st))
        goto out;
    if (fsi.EndOfFile.QuadPart == 0 || fsi.EndOfFile.QuadPart > 4 * 1024 * 1024) {
        st = STATUS_INVALID_IMAGE_FORMAT;
        goto out;
    }

    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (SIZE_T)fsi.EndOfFile.QuadPart, MRVL_TAG);
    if (!buf) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    st = ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, (ULONG)fsi.EndOfFile.QuadPart, NULL, NULL);
    if (!NT_SUCCESS(st) || iosb.Information != (ULONG_PTR)fsi.EndOfFile.QuadPart) {
        ExFreePoolWithTag(buf, MRVL_TAG);
        if (NT_SUCCESS(st))
            st = STATUS_END_OF_FILE;
        goto out;
    }
    *Image = buf;
    *Size = (ULONG)fsi.EndOfFile.QuadPart;
out:
    ZwClose(h);
    return st;
}

/* ------------------------------------------------------------------ */
/* mwifiex_init_sdio / mwifiex_init_sdio_ioport (new mode)             */
/* ------------------------------------------------------------------ */

static NTSTATUS InitSdioIoPort(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS st;
    UCHAR dummy;

    /* ACK the first interrupt from the bootloader */
    (VOID)SdioReadReg(Ctx, R8897_HOST_INT_STATUS, &dummy);

    Ctx->IoPort = MEM_PORT;
    if (!NT_SUCCESS(st = RegSetBits(Ctx, R8897_CARD_CFG_2_1, CMD53_NEW_MODE))) return st;
    if (!NT_SUCCESS(st = RegSetBits(Ctx, R8897_CMD_CFG_0, CMD_PORT_RD_LEN_EN))) return st;
    if (!NT_SUCCESS(st = RegSetBits(Ctx, R8897_CMD_CFG_1, CMD_PORT_AUTO_EN))) return st;

    /* host interrupt status: reset-on-read */
    if (!NT_SUCCESS(st = RegSetBits(Ctx, R8897_HOST_INT_RSR, R8897_SDIO_INT_MASK))) return st;
    /* Dnld/Upld ready set to auto reset */
    if (!NT_SUCCESS(st = RegSetBits(Ctx, R8897_CARD_MISC_CFG, AUTO_RE_ENABLE_INT))) return st;

    LOG("SDIO new mode enabled, ioport 0x%X", Ctx->IoPort);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Firmware download (mwifiex_prog_fw_w_helper / mwifiex_dnld_fw)      */
/* ------------------------------------------------------------------ */

static NTSTATUS ReadFwStatus(PDEVICE_CONTEXT Ctx, PUSHORT Stat)
{
    UCHAR s0, s1;
    NTSTATUS st;
    if (!NT_SUCCESS(st = SdioReadReg(Ctx, R8897_STATUS_0, &s0))) return st;
    if (!NT_SUCCESS(st = SdioReadReg(Ctx, R8897_STATUS_1, &s1))) return st;
    *Stat = (USHORT)((s1 << 8) | s0);
    return STATUS_SUCCESS;
}

static BOOLEAN CheckFwReady(PDEVICE_CONTEXT Ctx, ULONG Polls)
{
    USHORT s = 0;
    ULONG i;
    for (i = 0; i < Polls; i++) {
        if (NT_SUCCESS(ReadFwStatus(Ctx, &s)) && s == FIRMWARE_READY_SDIO)
            return TRUE;
        if (i + 1 < Polls)
            SleepMs(100);
    }
    LOG("firmware status 0x%04X (want 0x%04X)", s, FIRMWARE_READY_SDIO);
    return FALSE;
}

static NTSTATUS PollCardStatus(PDEVICE_CONTEXT Ctx, UCHAR Bits)
{
    ULONG tries;
    UCHAR cs;
    for (tries = 0; tries < MAX_POLL_TRIES * 10; tries++) {
        if (!NT_SUCCESS(SdioReadReg(Ctx, R8897_POLL, &cs)))
            break;
        if ((cs & Bits) == Bits)
            return STATUS_SUCCESS;
        PollDelay(tries);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS ProgFirmware(PDEVICE_CONTEXT Ctx, const UCHAR *Fw, ULONG FwLen)
{
    ULONG offset = 0, txlen = 0, txBlocks = 0, retry = 0, tries, stall = 0, chunks = 0;
    UCHAR b0, b1;
    USHORT len = 0;
    NTSTATUS st;
    PUCHAR fwbuf = Ctx->TxBuf;    /* 4K nonpaged >= MWIFIEX_UPLD_SIZE rounded to blocks */

    LOG("downloading firmware, %u bytes", FwLen);

    for (;;) {
        st = PollCardStatus(Ctx, CARD_IO_READY | DN_LD_CARD_RDY);
        if (!NT_SUCCESS(st)) {
            UCHAR pr = 0, is = 0, l0 = 0, l1 = 0;
            (VOID)SdioReadReg(Ctx, R8897_POLL, &pr);
            (VOID)SdioReadReg(Ctx, R8897_HOST_INT_STATUS, &is);
            (VOID)SdioReadReg(Ctx, R8897_BASE_0, &l0);
            (VOID)SdioReadReg(Ctx, R8897_BASE_1, &l1);
            LOG("FW dnld: poll status timeout @ %u (poll 0x%02X, int-status 0x%02X, base len %u, last txlen %u)",
                offset, pr, is, (l1 << 8) | l0, txlen);
            /* Recovery: resend the previous chunk (as if the card had flagged a CRC error). */
            if (txBlocks && stall < 2) {
                stall++;
                LOG("FW dnld: resending the last chunk (try %u)", stall);
                st = SdioWriteBlocks(Ctx, Ctx->IoPort, fwbuf, txBlocks * SDIO_BLOCK_SIZE);
                LOG("FW dnld: resend write 0x%08X", st);
                continue;
            }
            return st;
        }
        stall = 0;
        if (offset >= FwLen)
            break;

        len = 0;
        for (tries = 0; tries < MAX_POLL_TRIES * 10; tries++) {   /* ~5 ms spin + ~750 ms sleep */
            if (!NT_SUCCESS(st = SdioReadReg(Ctx, R8897_BASE_0, &b0))) return st;
            if (!NT_SUCCESS(st = SdioReadReg(Ctx, R8897_BASE_1, &b1))) return st;
            len = (USHORT)((b1 << 8) | b0);
            if (len)
                break;
            PollDelay(tries);
        }

        if (!len) {
            break;
        } else if (len > MWIFIEX_UPLD_SIZE) {
            LOG("FW dnld failed @ %u, invalid length %u", offset, len);
            return STATUS_DEVICE_PROTOCOL_ERROR;
        }

        if (chunks++ < 4)
            LOG("FW dnld: card asks for %u bytes @ %u", len, offset);
        txlen = len;
        if (len & 1) {
            if (++retry > MAX_WRITE_IOMEM_RETRY) {
                LOG("FW dnld failed @ %u, CRC retries exhausted", offset);
                return STATUS_CRC_ERROR;
            }
            LOG("FW dnld: CRC error indicated, resending @ %u", offset);
            txlen = 0;   /* resend previous buffer, same tx_blocks */
        } else {
            retry = 0;
            if (FwLen - offset < txlen)
                txlen = FwLen - offset;
            txBlocks = (txlen + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE;
            RtlZeroMemory(fwbuf, txBlocks * SDIO_BLOCK_SIZE);
            RtlCopyMemory(fwbuf, Fw + offset, txlen);
        }

        if (txBlocks == 0)
            continue;   /* CRC flagged before anything was sent */
        /*
         * Pacing. The download only ever worked while the sdbus WPP trace was
         * running (it slows every bus request a little); without it the boot ROM
         * hangs right after the first 16-byte header chunk. Give the card some
         * breathing room around each chunk. SdioFlags 0x200 turns this off.
         */
        if (!(Ctx->Options & OPT_NO_FW_PACING)) {
            if (offset == 0)
                SleepMs(5);
            else
                KeStallExecutionProcessor(txlen <= 16 ? 200 : 20);
        }
        st = SdioWriteBlocks(Ctx, Ctx->IoPort, fwbuf, txBlocks * SDIO_BLOCK_SIZE);
        if (NT_SUCCESS(st) && !(Ctx->Options & OPT_NO_FW_PACING)) {
            if (offset == 0)
                SleepMs(5);
            else
                KeStallExecutionProcessor(txlen <= 16 ? 200 : 20);
        }
        if (!NT_SUCCESS(st)) {
            LOG("FW dnld: write iomem failed @ %u: 0x%08X", offset, st);
            (VOID)SdioWriteReg(Ctx, CONFIGURATION_REG, 0x04);
            return st;
        }
        offset += txlen;
    }

    LOG("FW download over, %u bytes", offset);
    return STATUS_SUCCESS;
}

static NTSTATUS DownloadFirmware(PDEVICE_CONTEXT Ctx)
{
    PUCHAR fw;
    ULONG fwLen;
    UCHAR winner;
    NTSTATUS st;

    if (CheckFwReady(Ctx, 1)) {
        LOG("WLAN firmware already running, skipping download");
        return STATUS_SUCCESS;
    }

    /* mwifiex_check_winner_status: status_reg_0 == 0 means we download */
    st = SdioReadReg(Ctx, R8897_STATUS_0, &winner);
    if (!NT_SUCCESS(st))
        return st;

    if (winner == 0) {
        st = LoadFirmware(&fw, &fwLen);
        if (!NT_SUCCESS(st))
            return st;
        st = ProgFirmware(Ctx, fw, fwLen);
        ExFreePoolWithTag(fw, MRVL_TAG);
        if (!NT_SUCCESS(st))
            return st;
    } else {
        LOG("not the download winner (another function is loading FW), waiting");
    }

    if (!CheckFwReady(Ctx, MAX_FIRMWARE_POLL_TRIES)) {
        LOG("firmware failed to become active in time");
        return STATUS_IO_TIMEOUT;
    }
    LOG("firmware is ready");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Interrupt status (mwifiex_interrupt_status + process_int_status)    */
/* Caller holds IoLock.                                                */
/* ------------------------------------------------------------------ */

static VOID HandleCmdPortUpload(PDEVICE_CONTEXT Ctx)
{
    ULONG rxLen, rxBlocks, pktLen, type;
    NTSTATUS st;

    rxLen = (Ctx->MpRegs[R8897_CMD_RD_LEN_1] << 8) | Ctx->MpRegs[R8897_CMD_RD_LEN_0];
    rxBlocks = (rxLen + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE;
    if (rxLen <= INTF_HEADER_LEN || rxBlocks * SDIO_BLOCK_SIZE > MWIFIEX_RX_DATA_BUF_SIZE) {
        LOG("cmd port: bad rx len %u", rxLen);
        return;
    }

    st = SdioReadBlocks(Ctx, Ctx->IoPort | CMD_PORT_SLCT, Ctx->RxBuf, rxBlocks * SDIO_BLOCK_SIZE);
    if (!NT_SUCCESS(st)) {
        UCHAR cr = 0;
        LOG("cmd port read failed 0x%08X, terminating cmd53", st);
        if (NT_SUCCESS(SdioReadReg(Ctx, CONFIGURATION_REG, &cr)))
            (VOID)SdioWriteReg(Ctx, CONFIGURATION_REG, cr | 0x04);
        return;
    }

    pktLen = GetLe16(Ctx->RxBuf);
    type = GetLe16(Ctx->RxBuf + 2);
    if (pktLen > rxBlocks * SDIO_BLOCK_SIZE || pktLen < INTF_HEADER_LEN) {
        LOG("cmd port: invalid packet len %u", pktLen);
        return;
    }

    if (type == MWIFIEX_TYPE_CMD) {
        ULONG n = pktLen - INTF_HEADER_LEN;
        if (n > CMD_RESP_MAX)
            n = CMD_RESP_MAX;
        RtlCopyMemory(Ctx->Resp, Ctx->RxBuf + INTF_HEADER_LEN, n);
        Ctx->RespLen = n;
        KeSetEvent(&Ctx->CmdDone, IO_NO_INCREMENT, FALSE);
    } else if (type == MWIFIEX_TYPE_EVENT) {
        if (pktLen >= INTF_HEADER_LEN + 4) {
            Ctx->LastEvent = GetLe32(Ctx->RxBuf + INTF_HEADER_LEN) & 0xffff;
            InterlockedIncrement(&Ctx->Events);
            LOG("firmware event 0x%04X", Ctx->LastEvent);
            WifiFwEvent(Ctx, Ctx->LastEvent);
        }
    } else {
        LOG("cmd port: unexpected packet type %u", type);
    }
}

/*
 * One received data packet (mwifiex sta_rx.c): SDIO header (len, type), then
 * RxPD { bss_type, bss_num, le16 rx_pkt_length, le16 rx_pkt_offset (from the
 * RxPD start), le16 rx_pkt_type, ... }, then the frame. The frame is 802.3
 * with an RFC1042/bridge-tunnel SNAP header, or already Ethernet II; either
 * way hand Ethernet II to the Wi-Fi layer.
 */
static VOID DeliverRxData(PDEVICE_CONTEXT Ctx, PUCHAR Pkt, ULONG PktLen)
{
    static const UCHAR rfc1042[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    static const UCHAR tunnel[6]  = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8 };
    PUCHAR rxpd = Pkt + INTF_HEADER_LEN, f;
    ULONG len, off, type;

    if (PktLen < INTF_HEADER_LEN + 20)
        return;
    len = GetLe16(rxpd + 2);
    off = GetLe16(rxpd + 4);
    type = GetLe16(rxpd + 6);
    if (INTF_HEADER_LEN + off + len > PktLen || len < 14) {
        LOG("rx: bad rxpd (len %u off %u pkt %u)", len, off, PktLen);
        return;
    }
    /* mwifiex treats everything except management (0xE5) and block-ack requests
     * (0xE7) as data; this firmware uses type 2 for ordinary data frames. */
    if (type == 0xE5 || type == 0xE7) {
        LOG("rx: packet type 0x%X ignored", type);
        return;
    }
    if (type == 0xE6)
        LOG("rx: A-MSDU (type 0xE6) not de-aggregated yet, passed as-is");
    f = rxpd + off;
    /* 802.3 + SNAP -> Ethernet II: keep DA/SA, drop length + the 6 SNAP bytes */
    if (len >= 22 && (RtlEqualMemory(f + 14, rfc1042, 6) || RtlEqualMemory(f + 14, tunnel, 6))) {
        RtlMoveMemory(f + 8, f, 12);       /* DA, SA */
        f += 8;
        len -= 8;
    }
    WifiRxFrame(Ctx, f, len);
}

static VOID DrainDataPorts(PDEVICE_CONTEXT Ctx)
{
    ULONG bitmap, port, rxLen, rxBlocks;
    PUCHAR r = Ctx->MpRegs;

    bitmap = r[R8897_RD_BITMAP_L] | (r[R8897_RD_BITMAP_U] << 8) |
             (r[R8897_RD_BITMAP_1L] << 16) | ((ULONG)r[R8897_RD_BITMAP_1U] << 24);

    for (port = 0; port < R8897_MAX_PORTS; port++) {
        if (!(bitmap & (1u << port)))
            continue;
        rxLen = r[R8897_RD_LEN_P0_L + (port << 1)] | (r[R8897_RD_LEN_P0_U + (port << 1)] << 8);
        rxBlocks = (rxLen + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE;
        if (rxLen <= INTF_HEADER_LEN || rxBlocks * SDIO_BLOCK_SIZE > MWIFIEX_RX_DATA_BUF_SIZE)
            continue;
        if (NT_SUCCESS(SdioReadBlocks(Ctx, Ctx->IoPort + port, Ctx->RxBuf, rxBlocks * SDIO_BLOCK_SIZE))) {
            ULONG plen = GetLe16(Ctx->RxBuf);
            ULONG type = GetLe16(Ctx->RxBuf + 2);
            if (plen > rxBlocks * SDIO_BLOCK_SIZE)
                plen = rxBlocks * SDIO_BLOCK_SIZE;
            if (type == MWIFIEX_TYPE_EVENT && plen >= 8) {
                Ctx->LastEvent = GetLe32(Ctx->RxBuf + 4) & 0xffff;
                InterlockedIncrement(&Ctx->Events);
                LOG("firmware event 0x%04X (data port %u)", Ctx->LastEvent, port);
                WifiFwEvent(Ctx, Ctx->LastEvent);
            } else if (type == MWIFIEX_TYPE_DATA) {
                DeliverRxData(Ctx, Ctx->RxBuf, plen);
            }
        }
    }
}

VOID CardProcessInterrupt(PDEVICE_CONTEXT Ctx)
{
    UCHAR ireg;
    NTSTATUS st;

    if (!Ctx->FwReady)
        return;

    /* read all MP registers (reset-on-read clears host int status) */
    st = SdioReadBytes(Ctx, REG_PORT, Ctx->MpRegs, R8897_MAX_MP_REGS);
    if (!NT_SUCCESS(st)) {
        LOG("read mp_regs failed 0x%08X", st);
        return;
    }

    ireg = Ctx->MpRegs[R8897_HOST_INT_STATUS];
    if (!ireg)
        return;

    if (ireg & DN_LD_HOST_INT_STATUS)
        Ctx->WrBitmap = Ctx->MpRegs[R8897_WR_BITMAP_L] | (Ctx->MpRegs[R8897_WR_BITMAP_U] << 8) |
                        (Ctx->MpRegs[R8897_WR_BITMAP_1L] << 16) | ((ULONG)Ctx->MpRegs[R8897_WR_BITMAP_1U] << 24);

    if (ireg & UP_LD_CMD_PORT_HOST_INT_STATUS)
        HandleCmdPortUpload(Ctx);

    if (ireg & UP_LD_HOST_INT_STATUS)
        DrainDataPorts(Ctx);
}

/* ------------------------------------------------------------------ */
/* Host commands                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS SendCmdLocked(PDEVICE_CONTEXT Ctx, USHORT Cmd, const VOID *Payload, USHORT PayloadLen,
                              PUCHAR *Resp, PULONG RespLen);

/* Serialised: the response buffer (Ctx->Resp) is shared, so one command at a time.
 * Callers that use *Resp must hold CmdLock themselves (CardScan does). */
static NTSTATUS SendCmd(PDEVICE_CONTEXT Ctx, USHORT Cmd, const VOID *Payload, USHORT PayloadLen,
                        PUCHAR *Resp, PULONG RespLen)
{
    NTSTATUS st;
    WdfWaitLockAcquire(Ctx->CmdLock, NULL);
    st = SendCmdLocked(Ctx, Cmd, Payload, PayloadLen, Resp, RespLen);
    WdfWaitLockRelease(Ctx->CmdLock);
    return st;
}

static NTSTATUS SendCmdLocked(PDEVICE_CONTEXT Ctx, USHORT Cmd, const VOID *Payload, USHORT PayloadLen,
                              PUCHAR *Resp, PULONG RespLen)
{
    ULONG total = INTF_HEADER_LEN + S_DS_GEN + PayloadLen;
    ULONG padded = ((total + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE) * SDIO_BLOCK_SIZE;
    PUCHAR b = Ctx->TxBuf;
    LARGE_INTEGER slice;
    NTSTATUS st;
    ULONG waited;
    USHORT seq;

    if (padded > PAGE_SIZE)
        return STATUS_BUFFER_OVERFLOW;

    WdfWaitLockAcquire(Ctx->IoLock, NULL);

    seq = ++Ctx->SeqNum & 0xff;
    RtlZeroMemory(b, padded);
    PutLe16(b + 0, (USHORT)total);
    PutLe16(b + 2, MWIFIEX_TYPE_CMD);
    PutLe16(b + 4, Cmd);
    PutLe16(b + 6, (USHORT)(S_DS_GEN + PayloadLen));
    PutLe16(b + 8, seq);
    PutLe16(b + 10, 0);
    if (PayloadLen)
        RtlCopyMemory(b + INTF_HEADER_LEN + S_DS_GEN, Payload, PayloadLen);

    Ctx->PendingCmd = Cmd;
    Ctx->RespLen = 0;
    KeClearEvent(&Ctx->CmdDone);

    st = SdioWriteBlocks(Ctx, Ctx->IoPort | CMD_PORT_SLCT, b, padded);
    WdfWaitLockRelease(Ctx->IoLock);
    if (!NT_SUCCESS(st)) {
        LOG("cmd 0x%04X: write to cmd port failed 0x%08X", Cmd, st);
        return st;
    }

    /*
     * Wait for the response. Card interrupts normally complete it; if the
     * host controller never delivers SDIO interrupts we poll the status
     * registers ourselves every 20 ms so bring-up still works.
     */
    slice.QuadPart = -10000LL * 20;
    for (waited = 0; waited < 5000; waited += 20) {
        if (KeWaitForSingleObject(&Ctx->CmdDone, Executive, KernelMode, FALSE, &slice) == STATUS_SUCCESS)
            break;
        WdfWaitLockAcquire(Ctx->IoLock, NULL);
        CardProcessInterrupt(Ctx);
        WdfWaitLockRelease(Ctx->IoLock);
    }

    if (Ctx->RespLen < S_DS_GEN) {
        LOG("cmd 0x%04X: no response (timeout)", Cmd);
        return STATUS_IO_TIMEOUT;
    }
    if (GetLe16(Ctx->Resp) != (Cmd | HostCmd_RET_BIT)) {
        LOG("cmd 0x%04X: unexpected response id 0x%04X", Cmd, GetLe16(Ctx->Resp));
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (GetLe16(Ctx->Resp + 6) != 0) {
        LOG("cmd 0x%04X: firmware returned result %u", Cmd, GetLe16(Ctx->Resp + 6));
        return STATUS_UNSUCCESSFUL;
    }
    if (Resp)
        *Resp = Ctx->Resp + S_DS_GEN;
    if (RespLen)
        *RespLen = Ctx->RespLen - S_DS_GEN;
    return STATUS_SUCCESS;
}

/* struct host_cmd_ds_get_hw_spec (fw.h), packed */
#define HWSPEC_SIZE             63
#define HWSPEC_OFF_PERM_ADDR    8
#define HWSPEC_OFF_FW_RELEASE   18

static NTSTATUS QueryFirmwareInfo(PDEVICE_CONTEXT Ctx)
{
    UCHAR req[129];
    PUCHAR resp;
    ULONG len;
    NTSTATUS st;

    WdfWaitLockAcquire(Ctx->CmdLock, NULL);
    RtlZeroMemory(req, sizeof(req));
    st = SendCmdLocked(Ctx, HostCmd_CMD_GET_HW_SPEC, req, HWSPEC_SIZE, &resp, &len);
    if (!NT_SUCCESS(st)) {
        WdfWaitLockRelease(Ctx->CmdLock);
        return st;
    }
    if (len >= HWSPEC_OFF_FW_RELEASE + 4) {
        RtlCopyMemory(Ctx->Mac, resp + HWSPEC_OFF_PERM_ADDR, 6);
        Ctx->FwRelease = GetLe32(resp + HWSPEC_OFF_FW_RELEASE);
        LOG("HW spec: MAC %02X:%02X:%02X:%02X:%02X:%02X fw_release 0x%08X",
            Ctx->Mac[0], Ctx->Mac[1], Ctx->Mac[2], Ctx->Mac[3], Ctx->Mac[4], Ctx->Mac[5],
            Ctx->FwRelease);
    }
    /* variable part: TLVs; TLV_TYPE_API_REV (0x1C7) { le16 api_id, u8 major, u8 minor } */
    if (len > HWSPEC_SIZE) {
        ULONG o = HWSPEC_SIZE;
        while (o + 4 <= len) {
            ULONG t = GetLe16(resp + o), l = GetLe16(resp + o + 2);
            if (o + 4 + l > len) break;
            if (t == 0x1C7 && l >= 4) {
                ULONG id = GetLe16(resp + o + 4);
                LOG("HW spec: API id %u version %u.%u", id, resp[o + 6], resp[o + 7]);
                if (id == 1)
                    Ctx->KeyApiMajor = resp[o + 6];
            }
            o += 4 + l;
        }
    }

    /* host_cmd_ds_version_ext: u8 sel + char[128] */
    RtlZeroMemory(req, sizeof(req));
    if (NT_SUCCESS(SendCmdLocked(Ctx, HostCmd_CMD_VERSION_EXT, req, sizeof(req), &resp, &len)) && len > 1) {
        ULONG n = min(len - 1, sizeof(Ctx->FwVersion) - 1);
        RtlCopyMemory(Ctx->FwVersion, resp + 1, n);
        Ctx->FwVersion[n] = 0;
        LOG("firmware version: %s", Ctx->FwVersion);
    }
    WdfWaitLockRelease(Ctx->CmdLock);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* First radio test: active scan, results logged (scan.c / fw.h)       */
/* ------------------------------------------------------------------ */

#define HostCmd_CMD_802_11_SCAN   0x0006
#define HostCmd_CMD_MAC_CONTROL   0x0028
#define HostCmd_BSS_MODE_ANY      3
#define TLV_TYPE_SSID             0x0000
#define TLV_TYPE_RATES            0x0001
#define TLV_TYPE_CHANLIST         0x0101
#define SCAN_CHAN_TIME_MS         100

static ULONG PutTlvHdr(PUCHAR p, USHORT Type, USHORT Len) { PutLe16(p, Type); PutLe16(p + 2, Len); return 4; }

/*
 * Parses one HostCmd_CMD_802_11_SCAN response (mwifiex scan.c): le16 desc size,
 * u8 number of sets, then per BSS: le16 size, BSSID[6], RSSI (-dBm), and the
 * frame body (timestamp[8], beacon interval[2], capability[2], IEs).
 * New BSSIDs are appended to Ctx->ScanResults; a repeat keeps the stronger copy.
 */
static VOID ParseScanResponse(PDEVICE_CONTEXT Ctx, const UCHAR *r, ULONG len)
{
    ULONG descSize, sets, i, off = 3;
    if (len < 3) return;
    descSize = GetLe16(r);
    sets = r[2];
    if (descSize > len - 3) descSize = len - 3;

    for (i = 0; i < sets && off + 2 <= 3 + descSize; i++) {
        ULONG bsz = GetLe16(r + off), p, end, k, bodyLen;
        const UCHAR *b = r + off + 2;
        CHAR ssid[33] = "<hidden>";
        UCHAR chan = 0;
        USHORT cap;
        PCSTR sec;
        BOOLEAN rsn = FALSE, wpa = FALSE;
        PMRVL_BSS e = NULL;

        off += 2;
        if (bsz < 6 + 1 + 12 || off + bsz > 3 + descSize) break;
        cap = GetLe16(b + 6 + 1 + 10);
        /* walk the IEs; also find where the body must be cut to fit SCAN_MAX_BODY */
        bodyLen = 12;
        for (p = 6 + 1 + 12, end = bsz; p + 2 <= end; p += 2 + b[p + 1]) {
            UCHAR id = b[p], l = b[p + 1];
            const UCHAR *v = b + p + 2;
            if (p + 2 + l > end) break;
            if (p + 2 + l - 7 <= SCAN_MAX_BODY)
                bodyLen = p + 2 + l - 7;
            if (id == 0 && l > 0 && l <= 32) {
                ULONG n;
                for (n = 0; n < l; n++) ssid[n] = (v[n] >= 0x20 && v[n] < 0x7f) ? (CHAR)v[n] : '?';
                ssid[l] = 0;
            } else if (id == 3 && l >= 1) {
                chan = v[0];
            } else if (id == 61 && l >= 1 && !chan) {
                chan = v[0];                          /* HT operation: primary channel (5 GHz) */
            } else if (id == 48) {
                rsn = TRUE;
            } else if (id == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1) {
                wpa = TRUE;
            }
        }
        sec = rsn ? "WPA2/3" : wpa ? "WPA" : (cap & 0x0010) ? "WEP" : "open";

        for (k = 0; k < Ctx->ScanBssCount; k++)
            if (RtlEqualMemory(Ctx->ScanResults[k].Bssid, b, 6)) { e = &Ctx->ScanResults[k]; break; }
        if (!e && Ctx->ScanBssCount < SCAN_MAX_BSS) {
            e = &Ctx->ScanResults[Ctx->ScanBssCount++];
            e->Rssi = -1000;
            LOG("  BSS %02X:%02X:%02X:%02X:%02X:%02X ch%-3u -%u dBm %-6s \"%s\"",
                b[0], b[1], b[2], b[3], b[4], b[5], chan, b[6], sec, ssid);
        }
        if (e && -(LONG)b[6] > e->Rssi) {
            RtlCopyMemory(e->Bssid, b, 6);
            e->Rssi = -(LONG)b[6];
            e->Channel = chan;
            e->Is5GHz = chan > 14;
            e->BodyLen = (USHORT)bodyLen;
            RtlCopyMemory(e->Body, b + 7, bodyLen);
        }
        off += bsz;
    }
}

/*
 * Active scan of 2.4 GHz 1-13 and 5 GHz 36-48/149-165, 4 channels per command,
 * wildcard SSID plus (optionally) one directed SSID for hidden networks.
 * Results land in Ctx->ScanResults / ScanBssCount. PASSIVE_LEVEL.
 */
NTSTATUS CardScan(PDEVICE_CONTEXT Ctx, const UCHAR *Ssid, ULONG SsidLen)
{
    static const UCHAR rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
    static const UCHAR ch24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    static const UCHAR ch5[]  = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
    ULONG band, i, ok = 0;
    NTSTATUS st;

    if (!Ctx->FwReady || !Ctx->ScanResults)
        return STATUS_DEVICE_NOT_READY;
    if (SsidLen > 32)
        SsidLen = 32;

    WdfWaitLockAcquire(Ctx->CmdLock, NULL);
    Ctx->ScanBssCount = 0;
    LOG("scanning (2.4 GHz ch1-13, 5 GHz 36-48/149-165)%s", SsidLen ? " + directed SSID" : "");
    for (band = 0; band < 2; band++) {
        const UCHAR *list = band ? ch5 : ch24;
        ULONG count = band ? sizeof(ch5) : sizeof(ch24);
        for (i = 0; i < count; i += 4) {
            UCHAR cmd[7 + 4 + 4 + 32 + 4 + sizeof(rates) + 4 + 4 * 7];
            ULONG n = min(4, count - i), j, p = 0;
            PUCHAR resp;
            ULONG respLen;

            RtlZeroMemory(cmd, sizeof(cmd));
            cmd[p++] = HostCmd_BSS_MODE_ANY;
            p += 6;                                        /* bssid: any */
            p += PutTlvHdr(cmd + p, TLV_TYPE_SSID, 0);     /* wildcard SSID */
            if (SsidLen) {
                p += PutTlvHdr(cmd + p, TLV_TYPE_SSID, (USHORT)SsidLen);
                RtlCopyMemory(cmd + p, Ssid, SsidLen); p += SsidLen;
            }
            p += PutTlvHdr(cmd + p, TLV_TYPE_RATES, sizeof(rates));
            RtlCopyMemory(cmd + p, rates, sizeof(rates)); p += sizeof(rates);
            p += PutTlvHdr(cmd + p, TLV_TYPE_CHANLIST, (USHORT)(7 * n));
            for (j = 0; j < n; j++) {
                cmd[p++] = (UCHAR)band;                    /* radio type: 0 = 2.4 GHz, 1 = 5 GHz */
                cmd[p++] = list[i + j];
                cmd[p++] = 0;                              /* active scan */
                PutLe16(cmd + p, 0); p += 2;               /* min scan time */
                PutLe16(cmd + p, SCAN_CHAN_TIME_MS); p += 2;
            }
            st = SendCmdLocked(Ctx, HostCmd_CMD_802_11_SCAN, cmd, (USHORT)p, &resp, &respLen);
            if (!NT_SUCCESS(st)) {
                LOG("scan %s ch%u..: 0x%08X", band ? "5G" : "2.4G", list[i], st);
                continue;
            }
            ok++;
            ParseScanResponse(Ctx, resp, respLen);
        }
    }
    LOG("scan done: %u networks found (%u scan commands ok)", Ctx->ScanBssCount, ok);
    WdfWaitLockRelease(Ctx->CmdLock);
    return ok ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

/* ------------------------------------------------------------------ */

NTSTATUS CardBringUp(PDEVICE_CONTEXT Ctx, PCSTR *FailStage)
{
    NTSTATUS st;

    *FailStage = "sdio-init";
    st = InitSdioIoPort(Ctx);
    if (!NT_SUCCESS(st))
        return st;

    *FailStage = "firmware-download";
    st = DownloadFirmware(Ctx);
    if (!NT_SUCCESS(st))
        return st;
    Ctx->FwReady = TRUE;

    /* mwifiex_sdio_enable_host_int */
    *FailStage = "enable-interrupts";
    if (Ctx->WantInterrupts)
        (VOID)SdioSetIntEnable(Ctx, TRUE);   /* polling fallback covers failure */
    st = SdioWriteReg(Ctx, R8897_HOST_INT_MASK, R8897_HOST_INT_ENABLE);
    if (!NT_SUCCESS(st))
        return st;

    *FailStage = "FUNC_INIT";
    st = SendCmd(Ctx, HostCmd_CMD_FUNC_INIT, NULL, 0, NULL, NULL);
    if (!NT_SUCCESS(st))
        return st;

    *FailStage = "GET_HW_SPEC";
    st = QueryFirmwareInfo(Ctx);
    if (!NT_SUCCESS(st))
        return st;

    {
        /* HostCmd_ACT_MAC_RX_ON | TX_ON | ETHERNETII_ENABLE, as mwifiex_sta_init_cmd does */
        UCHAR macCtl[4];
        PutLe16(macCtl, 0x0013); PutLe16(macCtl + 2, 0);
        st = SendCmd(Ctx, HostCmd_CMD_MAC_CONTROL, macCtl, sizeof(macCtl), NULL, NULL);
        LOG("MAC_CONTROL: 0x%08X", st);
    }

    *FailStage = "ok";
    return STATUS_SUCCESS;
}

NTSTATUS CardShutdown(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS st = STATUS_NOT_FOUND;   /* firmware wasn't running */
    if (Ctx->FwReady) {
        st = SendCmd(Ctx, HostCmd_CMD_FUNC_SHUTDOWN, NULL, 0, NULL, NULL);
        LOG("FUNC_SHUTDOWN: 0x%08X", st);
        (VOID)SdioWriteReg(Ctx, R8897_HOST_INT_MASK, 0);
        Ctx->FwReady = FALSE;
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Helpers for the Wi-Fi layer                                         */
/* ------------------------------------------------------------------ */

/* Host command with the response payload (after the 8-byte header) copied out. PASSIVE. */
NTSTATUS CardCommand(PDEVICE_CONTEXT Ctx, USHORT Cmd, const VOID *Payload, USHORT Len,
                     PUCHAR Out, ULONG OutMax, PULONG OutLen)
{
    PUCHAR resp = NULL;
    ULONG rlen = 0;
    NTSTATUS st;

    if (!Ctx->FwReady)
        return STATUS_DEVICE_NOT_READY;
    WdfWaitLockAcquire(Ctx->CmdLock, NULL);
    st = SendCmdLocked(Ctx, Cmd, Payload, Len, &resp, &rlen);
    if (OutLen)
        *OutLen = 0;
    if (resp && Out && OutMax) {
        ULONG n = min(rlen, OutMax);
        RtlCopyMemory(Out, resp, n);
        if (OutLen)
            *OutLen = n;
    }
    WdfWaitLockRelease(Ctx->CmdLock);
    return st;
}

/*
 * Send one Ethernet II frame (mwifiex sta_tx.c + sdio host_to_card): SDIO header
 * { le16 len, le16 type=DATA }, TxPD (16 bytes: bss_type 0, bss_num 0,
 * le16 tx_pkt_length, le16 tx_pkt_offset = 16, ...), frame; padded to whole
 * blocks and written to the next free data port. PASSIVE.
 */
NTSTATUS CardTxFrame(PDEVICE_CONTEXT Ctx, const UCHAR *Eth, ULONG Len, UCHAR Priority)
{
    ULONG total = INTF_HEADER_LEN + 16 + Len, padded, port, tries;
    PUCHAR b = Ctx->DataTxBuf;
    NTSTATUS st;

    if (!Ctx->FwReady || !b)
        return STATUS_DEVICE_NOT_READY;
    padded = ((total + SDIO_BLOCK_SIZE - 1) / SDIO_BLOCK_SIZE) * SDIO_BLOCK_SIZE;
    if (Len < 14 || padded > PAGE_SIZE)
        return STATUS_INVALID_BUFFER_SIZE;

    WdfWaitLockAcquire(Ctx->IoLock, NULL);
    /* need a free write port; refresh the bitmap from the card if we have none */
    for (tries = 0; !(Ctx->WrBitmap & (1u << Ctx->CurrWrPort)); tries++) {
        if (tries >= 20) {
            WdfWaitLockRelease(Ctx->IoLock);
            return STATUS_DEVICE_BUSY;
        }
        if (NT_SUCCESS(SdioReadBytes(Ctx, REG_PORT, Ctx->MpRegs, R8897_MAX_MP_REGS))) {
            PUCHAR r = Ctx->MpRegs;
            UCHAR ireg = r[R8897_HOST_INT_STATUS];
            Ctx->WrBitmap = r[R8897_WR_BITMAP_L] | (r[R8897_WR_BITMAP_U] << 8) |
                            (r[R8897_WR_BITMAP_1L] << 16) | ((ULONG)r[R8897_WR_BITMAP_1U] << 24);
            /* the read cleared the interrupt status: handle what it reported */
            if (ireg & UP_LD_CMD_PORT_HOST_INT_STATUS)
                HandleCmdPortUpload(Ctx);
            if (ireg & UP_LD_HOST_INT_STATUS)
                DrainDataPorts(Ctx);
        }
        if (!(Ctx->WrBitmap & (1u << Ctx->CurrWrPort)) && Ctx->WrBitmap) {
            /* current port busy but others free: move on */
            Ctx->CurrWrPort = (Ctx->CurrWrPort + 1) % R8897_MAX_PORTS;
            continue;
        }
        if (!(Ctx->WrBitmap & (1u << Ctx->CurrWrPort)))
            KeStallExecutionProcessor(100);
    }
    port = Ctx->CurrWrPort;
    Ctx->WrBitmap &= ~(1u << port);
    Ctx->CurrWrPort = (Ctx->CurrWrPort + 1) % R8897_MAX_PORTS;

    RtlZeroMemory(b, padded);
    PutLe16(b + 0, (USHORT)total);
    PutLe16(b + 2, MWIFIEX_TYPE_DATA);
    PutLe16(b + INTF_HEADER_LEN + 2, (USHORT)Len);      /* tx_pkt_length */
    PutLe16(b + INTF_HEADER_LEN + 4, 16);               /* tx_pkt_offset (from TxPD start) */
    b[INTF_HEADER_LEN + 12] = Priority;                 /* priority */
    RtlCopyMemory(b + INTF_HEADER_LEN + 16, Eth, Len);

    st = SdioWriteBlocks(Ctx, Ctx->IoPort + port, b, padded);
    if (!NT_SUCCESS(st)) {
        Ctx->WrBitmap |= (1u << port);
        LOG("tx: write to port %u failed 0x%08X", port, st);
    }
    WdfWaitLockRelease(Ctx->IoLock);
    return st;
}
