/*
 * SDIO transport over the Windows SD bus driver (sdbus.sys).
 * Mirrors mwifiex's sdio_readb / sdio_writeb / sdio_readsb / sdio_writesb usage.
 * GPL v2 (derived from Linux mwifiex).
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>   /* instantiate GUID_SDBUS_INTERFACE_STANDARD here */
#include <ntddsd.h>
#include "mrvl8897.h"
#include <ntstrsafe.h>

static SDBUS_CALLBACK_ROUTINE SdioIntCallback;

static VOID SdioIntCallback(PVOID Context, ULONG InterruptType)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    UNREFERENCED_PARAMETER(InterruptType);

    InterlockedIncrement(&ctx->IntCallbacks);

    /* CallbackAtDpcLevel = FALSE, so we are at PASSIVE_LEVEL here */
    WdfWaitLockAcquire(ctx->IoLock, NULL);
    CardProcessInterrupt(ctx);
    WdfWaitLockRelease(ctx->IoLock);

    ctx->Bus.AcknowledgeInterrupt(ctx->Bus.Context);
}

static NTSTATUS SdioProperty(PDEVICE_CONTEXT Ctx, BOOLEAN Set, SDBUS_PROPERTY Prop, PVOID Buf, ULONG Len)
{
    SDBUS_REQUEST_PACKET rp;
    RtlZeroMemory(&rp, sizeof(rp));
    rp.RequestFunction = Set ? SDRF_SET_PROPERTY : SDRF_GET_PROPERTY;
    rp.Parameters.GetSetProperty.Property = Prop;
    rp.Parameters.GetSetProperty.Buffer = Buf;
    rp.Parameters.GetSetProperty.Length = Len;
    return SdBusSubmitRequest(Ctx->Bus.Context, &rp);
}

/*
 * Called from EvtDeviceAdd, before IRP_MN_START_DEVICE, as the SD bus
 * documentation requires ("should be called in the AddDevice routine").
 */
NTSTATUS SdioAttach(PDEVICE_CONTEXT Ctx, USHORT SdioFlags, BOOLEAN WantInterrupts, PCSTR *Stage)
{
    NTSTATUS st;
    SDBUS_INTERFACE_PARAMETERS ip;

    *Stage = "sdbus-open-interface";
    RtlZeroMemory(&Ctx->Bus, sizeof(Ctx->Bus));
    st = SdBusOpenInterface(WdfDeviceWdmGetPhysicalDevice(Ctx->Device),
                            &Ctx->Bus, sizeof(Ctx->Bus), SDBUS_INTERFACE_VERSION);
    if (!NT_SUCCESS(st)) {
        LOG("SdBusOpenInterface failed 0x%08X", st);
        return st;
    }
    Ctx->BusOpen = TRUE;

    *Stage = "sdbus-initialize-interface";
    RtlZeroMemory(&ip, sizeof(ip));
    ip.Size = sizeof(ip);
    ip.SdioFlags = SdioFlags;
    ip.TargetObject = WdfDeviceWdmGetAttachedDevice(Ctx->Device);
    ip.DeviceGeneratesInterrupts = WantInterrupts;
    ip.CallbackAtDpcLevel = FALSE;
    ip.CallbackRoutine = WantInterrupts ? SdioIntCallback : NULL;
    ip.CallbackRoutineContext = Ctx;
    LOG("InitializeInterface: SdioFlags=0x%X interrupts=%u", SdioFlags, WantInterrupts);
    st = Ctx->Bus.InitializeInterface(Ctx->Bus.Context, &ip);
    if (!NT_SUCCESS(st)) {
        LOG("InitializeInterface failed 0x%08X", st);
        return st;
    }
    Ctx->SdioFlags = SdioFlags;
    Ctx->WantInterrupts = WantInterrupts;
    *Stage = "sdbus-interface-ready";
    return STATUS_SUCCESS;
}

static NTSTATUS SdioCmd52Fn(PDEVICE_CONTEXT Ctx, UCHAR Fn, BOOLEAN Write, ULONG Reg, UCHAR In, PUCHAR Out);

/* Manual IO_ENABLE when sdbus was told not to manage it (SdioFlags bit 0). */
NTSTATUS SdioEnableFunctionManually(PDEVICE_CONTEXT Ctx)
{
    UCHAR ioe = 0, ior = 0, bit = (UCHAR)(1u << Ctx->Func);
    NTSTATUS st;
    ULONG i;
    LARGE_INTEGER t;

    st = SdioCmd52Fn(Ctx, 0, FALSE, 0x02, 0, &ioe);       /* CCCR I/O Enable */
    if (!NT_SUCCESS(st)) return st;
    if (!(ioe & bit)) {
        st = SdioCmd52Fn(Ctx, 0, TRUE, 0x02, ioe | bit, NULL);
        if (!NT_SUCCESS(st)) return st;
    }
    t.QuadPart = -10000LL * 10;
    for (i = 0; i < 300; i++) {                          /* up to 3 s */
        st = SdioCmd52Fn(Ctx, 0, FALSE, 0x03, 0, &ior);  /* CCCR I/O Ready */
        if (NT_SUCCESS(st) && (ior & bit))
            return STATUS_SUCCESS;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    LOG("function %u never reported I/O ready (IOE 0x%02X IOR 0x%02X)", Ctx->Func, ioe, ior);
    return STATUS_IO_TIMEOUT;
}

/* Called from EvtDevicePrepareHardware, after the bus has started the function. */
NTSTATUS SdioConfigure(PDEVICE_CONTEXT Ctx, PCSTR *Stage)
{
    NTSTATUS st;
    USHORT blk = SDIO_BLOCK_SIZE;

    /* informational, never fatal */
    (VOID)SdioProperty(Ctx, FALSE, SDP_BUS_DRIVER_VERSION, &Ctx->BusDriverVersion, sizeof(Ctx->BusDriverVersion));
    (VOID)SdioProperty(Ctx, FALSE, SDP_BUS_WIDTH, &Ctx->BusWidth, sizeof(Ctx->BusWidth));
    (VOID)SdioProperty(Ctx, FALSE, SDP_BUS_CLOCK, &Ctx->BusClock, sizeof(Ctx->BusClock));

    *Stage = "sdbus-get-function-number";
    st = SdioProperty(Ctx, FALSE, SDP_FUNCTION_NUMBER, &Ctx->Func, sizeof(Ctx->Func));
    if (!NT_SUCCESS(st)) {
        LOG("get SDP_FUNCTION_NUMBER failed 0x%08X", st);
        return st;
    }
    LOG("SDIO function %u, sdbus v%X, width %u, clock %u",
        Ctx->Func, Ctx->BusDriverVersion, Ctx->BusWidth, Ctx->BusClock);

    if (Ctx->Options & 0x100) {   /* SdioFlags 0x100: slow the bus to 50 MHz (signal-integrity test) */
        ULONG want = 50000;
        st = SdioProperty(Ctx, TRUE, SDP_BUS_CLOCK, &want, sizeof(want));
        (VOID)SdioProperty(Ctx, FALSE, SDP_BUS_CLOCK, &Ctx->BusClock, sizeof(Ctx->BusClock));
        LOG("set bus clock 50000 kHz: 0x%08X, now %u", st, Ctx->BusClock);
    }

    if (Ctx->SdioFlags & SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE) {
        *Stage = "manual-io-enable";
        Ctx->IoEnableStatus = SdioEnableFunctionManually(Ctx);
        if (!NT_SUCCESS(Ctx->IoEnableStatus))
            return Ctx->IoEnableStatus;
    }

    /* mwifiex_register_dev: sdio_set_block_size(func, 256) */
    Ctx->SetBlockLenStatus = SdioProperty(Ctx, TRUE, SDP_FUNCTION_BLOCK_LENGTH, &blk, sizeof(blk));
    (VOID)SdioProperty(Ctx, FALSE, SDP_FUNCTION_BLOCK_LENGTH, &Ctx->BlockLen, sizeof(Ctx->BlockLen));
    if (!NT_SUCCESS(Ctx->SetBlockLenStatus))
        LOG("set block length 256 failed 0x%08X (current %u)", Ctx->SetBlockLenStatus, Ctx->BlockLen);

    *Stage = "sdbus-attached";
    return STATUS_SUCCESS;
}

NTSTATUS SdioSetIntEnable(PDEVICE_CONTEXT Ctx, BOOLEAN Enable)
{
    NTSTATUS st = SdioProperty(Ctx, TRUE, SDP_FUNCTION_INT_ENABLE, &Enable, sizeof(Enable));
    if (NT_SUCCESS(st))
        Ctx->IntEnabled = Enable;
    else
        LOG("SDP_FUNCTION_INT_ENABLE(%u) failed 0x%08X", Enable, st);
    return st;
}

/* ReleaseHardware: quiesce, keep the interface (it belongs to the device object). */
VOID SdioStop(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->IntEnabled && Ctx->BusOpen)
        (VOID)SdioSetIntEnable(Ctx, FALSE);
}

/* Device cleanup: drop the interface reference taken in SdioAttach. */
VOID SdioDetach(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->BusOpen) {
        Ctx->Bus.InterfaceDereference(Ctx->Bus.Context);
        Ctx->BusOpen = FALSE;
    }
}

/* CMD52 */
static NTSTATUS SdioCmd52Fn(PDEVICE_CONTEXT Ctx, UCHAR Fn, BOOLEAN Write, ULONG Reg, UCHAR In, PUCHAR Out)
{
    SDBUS_REQUEST_PACKET rp;
    SD_RW_DIRECT_ARGUMENT arg;
    SDCMD_DESCRIPTOR desc = { SDCMD_IO_RW_DIRECT, SDCC_STANDARD,
                              Write ? SDTD_WRITE : SDTD_READ, SDTT_CMD_ONLY, SDRT_5 };
    NTSTATUS st;

    arg.u.AsULONG = 0;
    arg.u.bits.Function = Fn;
    arg.u.bits.Address = Reg;
    arg.u.bits.WriteToDevice = Write ? 1 : 0;
    arg.u.bits.Data = Write ? In : 0;

    RtlZeroMemory(&rp, sizeof(rp));
    rp.RequestFunction = SDRF_DEVICE_COMMAND;
    rp.Parameters.DeviceCommand.CmdDesc = desc;
    rp.Parameters.DeviceCommand.Argument = arg.u.AsULONG;

    st = SdBusSubmitRequest(Ctx->Bus.Context, &rp);
    if (NT_SUCCESS(st) && Out)
        *Out = rp.ResponseData.AsUCHAR[0];
    return st;
}

/*
 * SDIO I/O reset: CCCR 0x06 (I/O Abort) bit 3 = RES, like Linux sdio_reset().
 * Returns the card to its pre-CMD5 state. sdbus's own card init never does
 * this, and on this platform slot "power off" doesn't really cut the module's
 * power, so without it a card whose firmware is running never reports ready
 * to CMD5 again and disappears until a reboot. After this the card no longer
 * answers at its old RCA, so it must be the last thing we send.
 */
NTSTATUS SdioIoReset(PDEVICE_CONTEXT Ctx)
{
    UCHAR abort = 0;
    NTSTATUS st;
    if (!Ctx->BusOpen)
        return STATUS_DEVICE_NOT_READY;
    (VOID)SdioCmd52Fn(Ctx, 0, FALSE, 0x06, 0, &abort);
    st = SdioCmd52Fn(Ctx, 0, TRUE, 0x06, (UCHAR)(abort | 0x08), NULL);
    /* The card may reset before it answers, so a timeout here is expected. */
    LOG("SDIO I/O reset (CCCR 0x06 |= 0x08): 0x%08X", st);
    return st;
}

NTSTATUS SdioReadReg(PDEVICE_CONTEXT Ctx, ULONG Reg, PUCHAR Val)
{
    return SdioCmd52Fn(Ctx, Ctx->Func, FALSE, Reg, 0, Val);
}

NTSTATUS SdioWriteReg(PDEVICE_CONTEXT Ctx, ULONG Reg, UCHAR Val)
{
    return SdioCmd52Fn(Ctx, Ctx->Func, TRUE, Reg, Val, NULL);
}

/*
 * CMD53, fixed address (OpCode 0) exactly like Linux sdio_readsb/sdio_writesb.
 * Buf must be nonpaged.
 */
static NTSTATUS SdioCmd53(PDEVICE_CONTEXT Ctx, BOOLEAN Write, BOOLEAN Block, ULONG Port, PUCHAR Buf, ULONG Len)
{
    SDBUS_REQUEST_PACKET rp;
    SD_RW_EXTENDED_ARGUMENT arg;
    SDCMD_DESCRIPTOR desc = { SDCMD_IO_RW_EXTENDED, SDCC_STANDARD,
                              Write ? SDTD_WRITE : SDTD_READ,
                              Block ? SDTT_MULTI_BLOCK_NO_CMD12 : SDTT_SINGLE_BLOCK,
                              SDRT_5 };
    PMDL mdl;
    NTSTATUS st;
    ULONG count;

    if (Len == 0)
        return STATUS_INVALID_PARAMETER;
    if (Block) {
        if (Len % SDIO_BLOCK_SIZE)
            return STATUS_INVALID_PARAMETER;
        count = Len / SDIO_BLOCK_SIZE;
        if (count > 511)
            return STATUS_INVALID_PARAMETER;
    } else {
        if (Len > 512)
            return STATUS_INVALID_PARAMETER;
        count = (Len == 512) ? 0 : Len;
    }

    mdl = IoAllocateMdl(Buf, Len, FALSE, FALSE, NULL);
    if (!mdl)
        return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(mdl);

    arg.u.AsULONG = 0;
    arg.u.bits.Count = count;
    arg.u.bits.Address = Port & 0x1FFFF;
    arg.u.bits.OpCode = 0;
    arg.u.bits.BlockMode = Block ? 1 : 0;
    arg.u.bits.Function = Ctx->Func;
    arg.u.bits.WriteToDevice = Write ? 1 : 0;

    RtlZeroMemory(&rp, sizeof(rp));
    rp.RequestFunction = SDRF_DEVICE_COMMAND;
    rp.Parameters.DeviceCommand.CmdDesc = desc;
    rp.Parameters.DeviceCommand.Argument = arg.u.AsULONG;
    rp.Parameters.DeviceCommand.Mdl = mdl;
    rp.Parameters.DeviceCommand.Length = Len;

    st = SdBusSubmitRequest(Ctx->Bus.Context, &rp);
    IoFreeMdl(mdl);
    return st;
}

NTSTATUS SdioReadBytes(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len)
{
    return SdioCmd53(Ctx, FALSE, FALSE, Port, Buf, Len);
}

NTSTATUS SdioReadBlocks(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len)
{
    return SdioCmd53(Ctx, FALSE, TRUE, Port, Buf, Len);
}

NTSTATUS SdioWriteBlocks(PDEVICE_CONTEXT Ctx, ULONG Port, PUCHAR Buf, ULONG Len)
{
    return SdioCmd53(Ctx, TRUE, TRUE, Port, Buf, Len);
}

/*
 * Raw probe: can we talk to the card at all? Reads CCCR (function 0,
 * 0x00-0x13) and the 8897 firmware status / card status registers on
 * function 1. Read-only.
 */
VOID SdioProbe(PDEVICE_CONTEXT Ctx, PCSTR When)
{
    ULONG i;
    UCHAR s0 = 0, s1 = 0;
    NTSTATUS st;

    RtlStringCbCopyA(Ctx->ProbeWhen, sizeof(Ctx->ProbeWhen), When);

    if (Ctx->Func == 0) {
        Ctx->ProbeFuncStatus = SdioProperty(Ctx, FALSE, SDP_FUNCTION_NUMBER, &Ctx->Func, sizeof(Ctx->Func));
        if (!NT_SUCCESS(Ctx->ProbeFuncStatus) || Ctx->Func == 0)
            Ctx->Func = 1;   /* instance is FN_1 */
    }

    Ctx->CccrReadStatus = STATUS_SUCCESS;
    for (i = 0; i < sizeof(Ctx->Cccr); i++) {
        st = SdioCmd52Fn(Ctx, 0, FALSE, i, 0, &Ctx->Cccr[i]);
        if (!NT_SUCCESS(st)) {
            Ctx->CccrReadStatus = st;
            break;
        }
    }

    Ctx->Fn1ReadStatus = SdioReadReg(Ctx, R8897_STATUS_0, &s0);
    if (NT_SUCCESS(Ctx->Fn1ReadStatus))
        Ctx->Fn1ReadStatus = SdioReadReg(Ctx, R8897_STATUS_1, &s1);
    if (NT_SUCCESS(Ctx->Fn1ReadStatus))
        Ctx->Fn1ReadStatus = SdioReadReg(Ctx, R8897_POLL, &Ctx->Fn1PollReg);
    Ctx->FwStatusReg = (USHORT)((s1 << 8) | s0);

    LOG("probe(%s): func %u, CCCR st 0x%08X rev 0x%02X IOE 0x%02X IOR 0x%02X IEN 0x%02X; fn1 st 0x%08X fwstat 0x%04X poll 0x%02X",
        When, Ctx->Func, Ctx->CccrReadStatus, Ctx->Cccr[0], Ctx->Cccr[2], Ctx->Cccr[3], Ctx->Cccr[4],
        Ctx->Fn1ReadStatus, Ctx->FwStatusReg, Ctx->Fn1PollReg);
}
