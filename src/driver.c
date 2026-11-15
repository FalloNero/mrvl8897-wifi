/*
 * mrvl8897 KMDF driver entry / PnP / power.
 * GPL v2.
 */
#include "mrvl8897.h"
#include <ntstrsafe.h>

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtDeviceCleanup;
static EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;

#pragma alloc_text(INIT, DriverEntry)

#define DRIVER_BUILD L"1.0 " _CRT_WIDE(__DATE__) L" " _CRT_WIDE(__TIME__)

/* HKLM\SYSTEM\CurrentControlSet\Services\mrvl8897\Parameters\SdioFlags (DWORD)
 *   bit 0 = SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE (driver enables the function itself)
 *   bit 1 = SDIO_FLAG_SDIO_ENABLE_POLLING
 *   bit 2 (4)  = experiment: attempt full firmware bring-up after a failed start
 *   bit 3 (8)  = don't ask sdbus for card interrupts (poll instead)
 *   bit 4 (16) = don't attach to sdbus in AddDevice; attach in PrepareHardware
 *   bit 5 (32) = raw start: forward START to sdbus ourselves, record its status, probe + bring-up, then fail start
 * Re-read on every AddDevice, so "pnputil /restart-device" picks up changes. */
#define OPT_EXPERIMENT     0x04
#define OPT_NO_INTERRUPTS  0x08
#define OPT_LATE_ATTACH    0x10
#define OPT_RAW_START      0x20
/* bit 6 (64)  = on stop, don't send FUNC_SHUTDOWN (leave firmware running)
 * bit 7 (128) = on stop, don't send the SDIO I/O reset
 * Bits 6/7 are re-read at stop time (D0Exit), so retest.cmd's value applies to the
 * instance being stopped. */
#define OPT_STOP_NO_SHUTDOWN 0x40
#define OPT_STOP_NO_IORESET  0x80
static ULONG  g_Experiment;

static ULONG ReadOptions(VOID)
{
    WDFKEY key;
    ULONG flags = 0;
    DECLARE_CONST_UNICODE_STRING(vFlags, L"SdioFlags");
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (VOID)WdfRegistryQueryULong(key, &vFlags, &flags);
        WdfRegistryClose(key);
    }
    return flags;
}

/*
 * One-shot arming switch (crash safety). Bring-up only runs if
 * Parameters\ArmBringUp = 1, and the value is deleted and flushed to disk
 * BEFORE any card I/O. If anything in bring-up crashes the machine, the next
 * boot finds no flag and the driver refuses the device (AddDevice fails, so
 * no START and no card I/O): no boot loop. The boot task (autostart.ps1),
 * arm.cmd and retest.cmd set the flag.
 */
static BOOLEAN ConsumeArm(VOID)
{
    WDFKEY key;
    ULONG v = 0;
    DECLARE_CONST_UNICODE_STRING(vArm, L"ArmBringUp");
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key)))
        return FALSE;
    if (NT_SUCCESS(WdfRegistryQueryULong(key, &vArm, &v))) {
        (VOID)WdfRegistryRemoveValue(key, &vArm);
        (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    }
    WdfRegistryClose(key);
    return v == 1;
}

/*
 * Boot without a helper task.
 *
 * sdbus records the card's OCR when it first detects the card at boot (0x800000
 * right after power-on) and refuses the function's START when the re-init it does
 * then sees a different OCR (the card answers 0xFF8000 from then on): "different
 * card", and the failed attempt wedges the card until the next power-off.
 * sdbus also exposes GUID_REENUMERATE_SELF_INTERFACE_STANDARD on our PDO: calling it
 * forgets the saved card info, re-detects the card (the detection path saves the new
 * OCR) and re-creates our device. So on the first START of each boot we don't let
 * the START reach sdbus; we call the interface instead and the re-created device
 * starts normally. A volatile registry value (gone after reboot) records that the
 * re-enumeration was done this boot.
 */
static const GUID MrvlGuidReenumerateSelf =
    { 0x2aeb0243, 0x6a6e, 0x486b, { 0x82, 0xfc, 0xd8, 0x15, 0xf6, 0xb9, 0x70, 0x06 } };

static NTSTATUS OpenVolatileKey(WDFKEY *Key)
{
    WDFKEY par;
    NTSTATUS st;
    DECLARE_CONST_UNICODE_STRING(name, L"ThisBoot");
    st = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &par);
    if (!NT_SUCCESS(st))
        return st;
    st = WdfRegistryCreateKey(par, &name, KEY_READ | KEY_WRITE, REG_OPTION_VOLATILE, NULL,
                              WDF_NO_OBJECT_ATTRIBUTES, Key);
    WdfRegistryClose(par);
    return st;
}

static BOOLEAN ReenumDoneThisBoot(VOID)
{
    WDFKEY key;
    ULONG v = 0;
    DECLARE_CONST_UNICODE_STRING(val, L"Reenumerated");
    if (NT_SUCCESS(OpenVolatileKey(&key))) {
        (VOID)WdfRegistryQueryULong(key, &val, &v);
        WdfRegistryClose(key);
    }
    return v != 0;
}

static VOID SetReenumDoneThisBoot(VOID)
{
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(val, L"Reenumerated");
    if (NT_SUCCESS(OpenVolatileKey(&key))) {
        (VOID)WdfRegistryAssignULong(key, &val, 1);
        WdfRegistryClose(key);
    }
}

/*
 * Crash guard for automatic bring-up: AutoAttempts is raised (and flushed) before
 * a bring-up and cleared when PrepareHardware finishes, whatever the result. Two
 * attempts that never finished (the machine crashed) -> stop starting automatically;
 * start-wifi.cmd (arms the driver) still works and clears it.
 */
static ULONG AutoAttempts(LONG Delta, BOOLEAN Reset)
{
    WDFKEY key;
    ULONG v = 0;
    DECLARE_CONST_UNICODE_STRING(val, L"AutoAttempts");
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_WRITE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key)))
        return 0;
    (VOID)WdfRegistryQueryULong(key, &val, &v);
    if (Reset || Delta) {
        v = Reset ? 0 : v + Delta;
        (VOID)WdfRegistryAssignULong(key, &val, v);
        (VOID)ZwFlushKey(WdfRegistryWdmGetHandle(key));
    }
    WdfRegistryClose(key);
    return v;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    WDFDRIVER drv;
    WDFKEY key;
    NTSTATUS st;
    DECLARE_CONST_UNICODE_STRING(vRan, L"DriverEntryRan");
    UNICODE_STRING build;

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    MrvlLogInit();
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    cfg.EvtDriverUnload = EvtDriverUnload;
    LOG("DriverEntry, build %ws", DRIVER_BUILD);

    st = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &drv);
    if (!NT_SUCCESS(st)) {
        LOG("WdfDriverCreate failed 0x%08X", st);
        return st;
    }

    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(drv, KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        /* keep the previous driver load's log (the boot instance unloads before the real start) */
        DECLARE_CONST_UNICODE_STRING(vLog, L"Log");
        DECLARE_CONST_UNICODE_STRING(vPrev, L"LogPrev");
        ULONG cb = 0, type = 0;
        PVOID lb = ExAllocatePool2(POOL_FLAG_PAGED, 65536, MRVL_TAG);
        if (lb) {
            if (NT_SUCCESS(WdfRegistryQueryValue(key, &vLog, 65536, lb, &cb, &type)) && type == REG_MULTI_SZ && cb)
                (VOID)WdfRegistryAssignValue(key, &vPrev, REG_MULTI_SZ, cb, lb);
            ExFreePoolWithTag(lb, MRVL_TAG);
        }
        RtlInitUnicodeString(&build, DRIVER_BUILD);
        (VOID)WdfRegistryAssignUnicodeString(key, &vRan, &build);
        WdfRegistryClose(key);
    }
    MrvlLogStartFlusher(drv);
    return STATUS_SUCCESS;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LOG("driver unload");
    MrvlLogStopFlusher();
}

/*
 * Diagnostics are written to two places so at least one always works:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\<instance>\Device Parameters
 *   HKLM\SYSTEM\CurrentControlSet\Services\mrvl8897\Parameters
 */
static VOID WriteDiagToKey(WDFKEY key, PDEVICE_CONTEXT Ctx, PCSTR Stage, NTSTATUS Status)
{
    DECLARE_CONST_UNICODE_STRING(vStage, L"BringUpStage");
    DECLARE_CONST_UNICODE_STRING(vStatus, L"BringUpStatus");
    DECLARE_CONST_UNICODE_STRING(vFunc, L"SdioFunction");
    DECLARE_CONST_UNICODE_STRING(vBlk, L"SdioBlockLength");
    DECLARE_CONST_UNICODE_STRING(vBlkSt, L"SdioSetBlockLengthStatus");
    DECLARE_CONST_UNICODE_STRING(vBusVer, L"SdBusDriverVersion");
    DECLARE_CONST_UNICODE_STRING(vWidth, L"SdBusWidth");
    DECLARE_CONST_UNICODE_STRING(vClock, L"SdBusClock");
    DECLARE_CONST_UNICODE_STRING(vMac, L"MacAddress");
    DECLARE_CONST_UNICODE_STRING(vRel, L"FwRelease");
    DECLARE_CONST_UNICODE_STRING(vVer, L"FwVersion");
    DECLARE_CONST_UNICODE_STRING(vInts, L"SdioInterruptCallbacks");
    DECLARE_CONST_UNICODE_STRING(vEvts, L"FwEvents");
    DECLARE_CONST_UNICODE_STRING(vBuild, L"DriverBuild");
    DECLARE_CONST_UNICODE_STRING(vFlags, L"SdioFlagsUsed");
    DECLARE_CONST_UNICODE_STRING(vIoe, L"IoEnableStatus");
    DECLARE_CONST_UNICODE_STRING(vPw, L"ProbeWhen");
    DECLARE_CONST_UNICODE_STRING(vPfs, L"ProbeFuncStatus");
    DECLARE_CONST_UNICODE_STRING(vCs, L"CccrReadStatus");
    DECLARE_CONST_UNICODE_STRING(vCccr, L"Cccr");
    DECLARE_CONST_UNICODE_STRING(vF1s, L"Fn1ReadStatus");
    DECLARE_CONST_UNICODE_STRING(vFws, L"FwStatusReg");
    DECLARE_CONST_UNICODE_STRING(vPoll, L"Fn1PollReg");
    DECLARE_CONST_UNICODE_STRING(vLss, L"LowerStartStatus");
    DECLARE_CONST_UNICODE_STRING(vXs, L"ExperimentStage");
    DECLARE_CONST_UNICODE_STRING(vXst, L"ExperimentStatus");
    WCHAR wbuf[160];
    UNICODE_STRING us;
    ANSI_STRING as;

    RtlInitAnsiString(&as, Stage);
    us.Buffer = wbuf; us.Length = 0; us.MaximumLength = sizeof(wbuf);
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&us, &as, FALSE)))
        (VOID)WdfRegistryAssignUnicodeString(key, &vStage, &us);

    (VOID)WdfRegistryAssignULong(key, &vStatus, (ULONG)Status);
    (VOID)WdfRegistryAssignULong(key, &vFunc, Ctx->Func);
    (VOID)WdfRegistryAssignULong(key, &vBlk, Ctx->BlockLen);
    (VOID)WdfRegistryAssignULong(key, &vBlkSt, (ULONG)Ctx->SetBlockLenStatus);
    (VOID)WdfRegistryAssignULong(key, &vBusVer, Ctx->BusDriverVersion);
    (VOID)WdfRegistryAssignULong(key, &vWidth, Ctx->BusWidth);
    (VOID)WdfRegistryAssignULong(key, &vClock, Ctx->BusClock);
    (VOID)WdfRegistryAssignULong(key, &vRel, Ctx->FwRelease);
    (VOID)WdfRegistryAssignULong(key, &vInts, (ULONG)Ctx->IntCallbacks);
    (VOID)WdfRegistryAssignULong(key, &vEvts, (ULONG)Ctx->Events);
    {
        DECLARE_CONST_UNICODE_STRING(vScan, L"ScanNetworksFound");
        DECLARE_CONST_UNICODE_STRING(vAtt, L"AutoAttempts");
        (VOID)WdfRegistryAssignULong(key, &vScan, Ctx->ScanBssCount);
        if (strcmp(Stage, "ok") == 0)
            (VOID)WdfRegistryAssignULong(key, &vAtt, 0);   /* boot task's crash guard: success resets it */
    }

    if (NT_SUCCESS(RtlStringCbPrintfW(wbuf, sizeof(wbuf), L"%02X-%02X-%02X-%02X-%02X-%02X",
                                      Ctx->Mac[0], Ctx->Mac[1], Ctx->Mac[2],
                                      Ctx->Mac[3], Ctx->Mac[4], Ctx->Mac[5]))) {
        RtlInitUnicodeString(&us, wbuf);
        (VOID)WdfRegistryAssignUnicodeString(key, &vMac, &us);
    }

    RtlInitAnsiString(&as, Ctx->FwVersion);
    us.Buffer = wbuf; us.Length = 0; us.MaximumLength = sizeof(wbuf);
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&us, &as, FALSE)))
        (VOID)WdfRegistryAssignUnicodeString(key, &vVer, &us);

    (VOID)WdfRegistryAssignULong(key, &vFlags, Ctx->SdioFlags);
    (VOID)WdfRegistryAssignULong(key, &vIoe, (ULONG)Ctx->IoEnableStatus);
    (VOID)WdfRegistryAssignULong(key, &vPfs, (ULONG)Ctx->ProbeFuncStatus);
    (VOID)WdfRegistryAssignULong(key, &vCs, (ULONG)Ctx->CccrReadStatus);
    (VOID)WdfRegistryAssignValue(key, &vCccr, REG_BINARY, sizeof(Ctx->Cccr), Ctx->Cccr);
    (VOID)WdfRegistryAssignULong(key, &vF1s, (ULONG)Ctx->Fn1ReadStatus);
    (VOID)WdfRegistryAssignULong(key, &vFws, Ctx->FwStatusReg);
    (VOID)WdfRegistryAssignULong(key, &vPoll, Ctx->Fn1PollReg);
    (VOID)WdfRegistryAssignULong(key, &vLss, (ULONG)Ctx->LowerStartStatus);
    (VOID)WdfRegistryAssignULong(key, &vXst, (ULONG)Ctx->ExpStatus);

    RtlInitAnsiString(&as, Ctx->ProbeWhen);
    us.Buffer = wbuf; us.Length = 0; us.MaximumLength = sizeof(wbuf);
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&us, &as, FALSE)))
        (VOID)WdfRegistryAssignUnicodeString(key, &vPw, &us);
    RtlInitAnsiString(&as, Ctx->ExpStage);
    us.Buffer = wbuf; us.Length = 0; us.MaximumLength = sizeof(wbuf);
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&us, &as, FALSE)))
        (VOID)WdfRegistryAssignUnicodeString(key, &vXs, &us);

    RtlInitUnicodeString(&us, DRIVER_BUILD);
    (VOID)WdfRegistryAssignUnicodeString(key, &vBuild, &us);
}

static VOID WriteDiag(PDEVICE_CONTEXT Ctx, PCSTR Stage, NTSTATUS Status)
{
    WDFKEY key;
    NTSTATUS st;

    LOG("diag: stage=%s status=0x%08X", Stage, Status);

    st = WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                  WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(st)) {
        WriteDiagToKey(key, Ctx, Stage, Status);
        MrvlLogDumpToKey(key);
        WdfRegistryClose(key);
    } else {
        LOG("open device registry key failed 0x%08X", st);
    }

    st = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(st)) {
        WriteDiagToKey(key, Ctx, Stage, Status);
        MrvlLogDumpToKey(key);
        WdfRegistryClose(key);
    } else {
        LOG("open driver parameters key failed 0x%08X", st);
    }
}

/*
 * Experiment (SdioFlags bit 2): run the whole mwifiex bring-up directly,
 * outside the PnP start path, to find out whether the hardware works even
 * though sdbus refuses to start the function.
 */
static VOID RunExperiment(PDEVICE_CONTEXT Ctx, PCSTR When)
{
    PCSTR stage = "start";
    NTSTATUS st;

    if (!g_Experiment || (Ctx->ExpStatus == STATUS_SUCCESS && Ctx->ExpStage[0]))
        return;

    if (Ctx->SdioFlags & SDIO_FLAG_DO_NOT_MANAGE_IO_ENABLE) {
        stage = "manual-io-enable";
        st = SdioEnableFunctionManually(Ctx);
        Ctx->IoEnableStatus = st;
        if (!NT_SUCCESS(st))
            goto out;
    }
    st = CardBringUp(Ctx, &stage);
out:
    RtlStringCbPrintfA(Ctx->ExpStage, sizeof(Ctx->ExpStage), "%s@%s", stage, When);
    Ctx->ExpStatus = st;
    LOG("experiment (%s): stage %s status 0x%08X", When, stage, st);
}

static EVT_WDFDEVICE_WDM_IRP_PREPROCESS EvtPreprocessStart;

static NTSTATUS EvtPreprocessStart(WDFDEVICE Device, PIRP Irp)
{
    PDEVICE_CONTEXT ctx = GetCtx(Device);
    NTSTATUS st;

    LOG("IRP_MN_START_DEVICE received");

    if (ctx->NeedReenum) {
        LARGE_INTEGER d;
        SetReenumDoneThisBoot();
        if (ctx->ReenumValid && ctx->Reenum.SurpriseRemoveAndReenumerateSelf) {
            d.QuadPart = -10000LL * 500;               /* let the boot-time detection settle */
            KeDelayExecutionThread(KernelMode, FALSE, &d);
            LOG("first start this boot: asking sdbus to re-detect the card (saves its current OCR)");
            ctx->Reenum.SurpriseRemoveAndReenumerateSelf(ctx->Reenum.Context);
            WriteDiag(ctx, "reenumerating", STATUS_PENDING);
        } else {
            LOG("first start this boot, but sdbus gave no re-enumerate interface");
            WriteDiag(ctx, "no-reenumerate-interface", STATUS_NOT_SUPPORTED);
        }
        /* never let this START reach sdbus: it would fail and wedge the card */
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    if (ctx->Options & OPT_RAW_START) {
        /*
         * Diagnostic: bypass the framework and hand the start straight to
         * sdbus, synchronously, so we see sdbus's own answer. If sdbus says
         * yes, the card is started at the bus level: probe it and try the
         * full firmware bring-up, then fail the start ourselves (KMDF never
         * saw it, so PnP's follow-up REMOVE is handled from the Added state).
         */
        st = IoForwardIrpSynchronously(WdfDeviceWdmGetAttachedDevice(Device), Irp)
                 ? Irp->IoStatus.Status : STATUS_UNSUCCESSFUL;
        ctx->LowerStartStatus = st;
        LOG("raw start: sdbus returned 0x%08X (Information 0x%IX)", st, Irp->IoStatus.Information);
        if (NT_SUCCESS(st) && ctx->BusOpen) {
            PCSTR stage = "raw-configure";
            NTSTATUS cst = SdioConfigure(ctx, &stage);
            LOG("raw start: SdioConfigure 0x%08X at %s", cst, stage);
            SdioProbe(ctx, "after-raw-start");
            g_Experiment = TRUE;
            RunExperiment(ctx, "raw-start");
            if (ctx->FwReady) {
                (VOID)SdioWriteReg(ctx, R8897_HOST_INT_MASK, 0);
                ctx->FwReady = FALSE;
            }
            if (ctx->IntEnabled)
                (VOID)SdioSetIntEnable(ctx, FALSE);
        }
        WriteDiag(ctx, NT_SUCCESS(st) ? "raw-start-sdbus-ok" : "raw-start-sdbus-failed", st);
        Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;   /* diagnostic mode never stays started */
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_UNSUCCESSFUL;
    }
    /*
     * Required by KMDF: the framework has already advanced the stack
     * location before calling a preprocess callback. Without this skip the
     * framework misreads the IRP and the start never reaches sdbus or
     * EvtDevicePrepareHardware (v0.4-v0.6 bug).
     */
    IoSkipCurrentIrpStackLocation(Irp);
    st = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);   /* Irp is gone after this */
    LOG("IRP_MN_START_DEVICE dispatch returned 0x%08X", st);

    if (st != STATUS_PENDING && !NT_SUCCESS(st) && ctx->BusOpen) {
        ctx->LowerStartStatus = st;
        LOG("IRP_MN_START_DEVICE failed 0x%08X - probing card anyway", st);
        SdioProbe(ctx, "after-failed-start");
        RunExperiment(ctx, "after-failed-start");
        WriteDiag(ctx, "start-failed-below-driver", st);
    }
    return st;
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    UCHAR startMinor[] = { IRP_MN_START_DEVICE };
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE dev;
    PDEVICE_CONTEXT ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);

    /*
     * Boot gate + crash guard. sdbus saves the card's OCR at enumeration; after a
     * cold power-on that OCR (0x800000) never matches what the card reports on
     * the re-init sdbus does at our START (0xFF8000), so that START fails AND
     * wedges the card until the next power-off. So unless the boot task (or
     * retest/arm) has just armed us right before restarting the SD controller,
     * refuse the device: no START is sent and the card stays clean. The flag is
     * one-shot and deleted+flushed before any card I/O (crash safety).
     */
    BOOLEAN armed = ConsumeArm();          /* start-wifi.cmd: the controller was just restarted */
    BOOLEAN needReenum = !armed && !ReenumDoneThisBoot();

    if (armed) {
        (VOID)AutoAttempts(0, TRUE);
    } else if (!needReenum && AutoAttempts(0, FALSE) >= 2) {
        LOG("automatic start disabled: the last 2 bring-ups never finished (crash?). Run start-wifi.cmd.");
        return STATUS_DEVICE_NOT_READY;
    }

    /* WiFiCx client: NetAdapterCx + WiFiCx device init must precede WdfDeviceCreate. */
    st = WifiDeviceInitPre(Init);
    if (!NT_SUCCESS(st)) {
        LOG("WiFiCx device init config failed 0x%08X", st);
        return st;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);

    st = WdfDeviceInitAssignWdmIrpPreprocessCallback(Init, EvtPreprocessStart, IRP_MJ_PNP,
                                                    startMinor, ARRAYSIZE(startMinor));
    if (!NT_SUCCESS(st))
        return st;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    attr.EvtCleanupCallback = EvtDeviceCleanup;
    st = WdfDeviceCreate(&Init, &attr, &dev);
    if (!NT_SUCCESS(st))
        return st;

    ctx = GetCtx(dev);
    ctx->WdfTriageInfoPtr = WdfGetTriageInfo();
    ctx->NeedReenum = needReenum;
    ctx->Device = dev;
    KeInitializeEvent(&ctx->CmdDone, NotificationEvent, FALSE);

    st = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->IoLock);
    if (!NT_SUCCESS(st))
        return st;
    st = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->CmdLock);
    if (!NT_SUCCESS(st))
        return st;

    st = WifiDeviceInitPost(dev);
    if (!NT_SUCCESS(st)) {
        LOG("WifiDeviceInitialize failed 0x%08X", st);
        return st;
    }

    ctx->MpRegs = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, MRVL_TAG);
    ctx->TxBuf  = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, MRVL_TAG);
    ctx->RxBuf  = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, MRVL_TAG);
    ctx->ScanResults = (PMRVL_BSS)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MRVL_BSS) * SCAN_MAX_BSS, MRVL_TAG);
    ctx->DataTxBuf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, MRVL_TAG);
    if (!ctx->MpRegs || !ctx->TxBuf || !ctx->RxBuf || !ctx->ScanResults || !ctx->DataTxBuf)
        return STATUS_INSUFFICIENT_RESOURCES;   /* freed in EvtDeviceCleanup */

    ctx->Options = ReadOptions();
    g_Experiment = ctx->Options & OPT_EXPERIMENT;
    LOG("EvtDeviceAdd: SdioFlags option value 0x%X", ctx->Options);

    if (needReenum) {
        /* Don't touch the card in this instance; just get sdbus's re-enumerate interface. */
        ctx->Reenum.Size = sizeof(ctx->Reenum);
        ctx->Reenum.Version = 1;
        st = WdfFdoQueryForInterface(dev, &MrvlGuidReenumerateSelf, (PINTERFACE)&ctx->Reenum,
                                     sizeof(ctx->Reenum), 1, NULL);
        ctx->ReenumValid = NT_SUCCESS(st);
        LOG("boot instance: re-enumerate interface 0x%08X; the card is re-detected at START", st);
        WriteDiag(ctx, "boot-waiting-for-start", st);
        return STATUS_SUCCESS;
    }

    ctx->CountedAttempt = !armed;
    if (ctx->CountedAttempt)
        (VOID)AutoAttempts(1, FALSE);
    LOG("%s: doing full bring-up", armed ? "armed by start-wifi" : "automatic start");

    if (ctx->Options & OPT_LATE_ATTACH) {
        WriteDiag(ctx, "device-add-ok (late attach)", STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    {
        PCSTR stage = "device-add";
        st = SdioAttach(ctx, (USHORT)(ctx->Options & 0x3), !(ctx->Options & OPT_NO_INTERRUPTS), &stage);
        if (!NT_SUCCESS(st)) {
            LOG("SD bus attach in AddDevice failed at %s: 0x%08X", stage, st);
            ctx->SdioFailed = TRUE;
        }
        WriteDiag(ctx, NT_SUCCESS(st) ? "device-add-ok" : stage, st);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetCtx(Device);
    PCSTR stage = "prepare-hardware";
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    LOG("EvtPrepareHardware enter");
    WriteDiag(ctx, "prepare-hardware", STATUS_PENDING);
    if (ctx->SdioFailed)
        return STATUS_DEVICE_NOT_CONNECTED;   /* AddDevice diagnostics stand */

    if (!ctx->BusOpen) {   /* OPT_LATE_ATTACH */
        st = SdioAttach(ctx, (USHORT)(ctx->Options & 0x3), !(ctx->Options & OPT_NO_INTERRUPTS), &stage);
        if (!NT_SUCCESS(st)) {
            ctx->SdioFailed = TRUE;
            WriteDiag(ctx, stage, st);
            return st;
        }
    }

    st = SdioConfigure(ctx, &stage);
    if (!NT_SUCCESS(st)) {
        LOG("SD function configure failed at %s: 0x%08X", stage, st);
        ctx->SdioFailed = TRUE;
        WriteDiag(ctx, stage, st);
        return st;
    }
    SdioProbe(ctx, "prepare-hardware");
    WriteDiag(ctx, "sdbus-attached", STATUS_SUCCESS);

    /*
     * Firmware bring-up happens here (not in D0Entry): WiFiCx creates the
     * station NetAdapter right after PrepareHardware, and the adapter needs
     * the MAC address and the Wi-Fi capabilities, which come from the firmware.
     */
    WriteDiag(ctx, "bringup-running", STATUS_PENDING);
    stage = "start";
    st = CardBringUp(ctx, &stage);
    if (ctx->CountedAttempt) {
        (VOID)AutoAttempts(0, TRUE);      /* got through the bring-up without crashing */
        ctx->CountedAttempt = FALSE;
    }
    if (!NT_SUCCESS(st)) {
        LOG("bring-up failed at %s: 0x%08X", stage, st);
        WriteDiag(ctx, stage, st);
        return st;
    }
    LOG("bring-up complete");

    st = WifiReportCapabilities(ctx);
    if (!NT_SUCCESS(st)) {
        LOG("reporting Wi-Fi capabilities failed 0x%08X", st);
        WriteDiag(ctx, "wifi-capabilities", st);
        return st;
    }
    WriteDiag(ctx, "ok", STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    LOG("EvtReleaseHardware");
    SdioStop(GetCtx(Device));
    return STATUS_SUCCESS;
}

static VOID EvtDeviceCleanup(WDFOBJECT Object)
{
    PDEVICE_CONTEXT ctx = GetCtx((WDFDEVICE)Object);
    LOG("EvtDeviceCleanup");
    if (ctx->ReenumValid && ctx->Reenum.InterfaceDereference) {
        ctx->Reenum.InterfaceDereference(ctx->Reenum.Context);
        ctx->ReenumValid = FALSE;
    }
    if (ctx->CountedAttempt) {             /* removed before the bring-up ran: not a crash */
        (VOID)AutoAttempts(0, TRUE);
        ctx->CountedAttempt = FALSE;
    }
    if (ctx->BusOpen && ctx->FwReady) {
        (VOID)SdioWriteReg(ctx, R8897_HOST_INT_MASK, 0);   /* quiet the card before dropping the interface */
        ctx->FwReady = FALSE;
    }
    SdioStop(ctx);
    SdioDetach(ctx);
    if (ctx->MpRegs) { ExFreePoolWithTag(ctx->MpRegs, MRVL_TAG); ctx->MpRegs = NULL; }
    if (ctx->TxBuf)  { ExFreePoolWithTag(ctx->TxBuf, MRVL_TAG);  ctx->TxBuf = NULL; }
    if (ctx->RxBuf)  { ExFreePoolWithTag(ctx->RxBuf, MRVL_TAG);  ctx->RxBuf = NULL; }
    if (ctx->ScanResults) { ExFreePoolWithTag(ctx->ScanResults, MRVL_TAG); ctx->ScanResults = NULL; }
    if (ctx->DataTxBuf) { ExFreePoolWithTag(ctx->DataTxBuf, MRVL_TAG); ctx->DataTxBuf = NULL; }
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    PDEVICE_CONTEXT ctx = GetCtx(Device);

    UNREFERENCED_PARAMETER(ctx);
    LOG("D0Entry (from D%d)", Previous - WdfPowerDeviceD0);
    /* Bring-up already ran in PrepareHardware. Coming back from a low-power state
     * would need a card re-init, which sdbus can't do without a power cycle. */
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PDEVICE_CONTEXT ctx = GetCtx(Device);
    LOG("D0Exit (to D%d)", Target - WdfPowerDeviceD0);
    if (ctx->SdioFailed)
        return STATUS_SUCCESS;
    {
        ULONG opt = ReadOptions();
        NTSTATUS shut, rst = STATUS_NOT_SUPPORTED;   /* NOT_SUPPORTED = skipped by option */
        WDFKEY key;
        DECLARE_CONST_UNICODE_STRING(vShut, L"LastFuncShutdownStatus");
        DECLARE_CONST_UNICODE_STRING(vRst, L"LastIoResetStatus");
        LOG("stop: SdioFlags now 0x%X (no-shutdown %u, no-ioreset %u)", opt,
            !!(opt & OPT_STOP_NO_SHUTDOWN), !!(opt & OPT_STOP_NO_IORESET));
        if (opt & OPT_STOP_NO_SHUTDOWN) {
            /* Leave the firmware running; just quiet the card's interrupts. */
            shut = STATUS_NOT_SUPPORTED;
            if (ctx->FwReady) {
                (VOID)SdioWriteReg(ctx, R8897_HOST_INT_MASK, 0);
                ctx->FwReady = FALSE;
            }
        } else {
            shut = CardShutdown(ctx);
        }
        if (ctx->IntEnabled)
            (VOID)SdioSetIntEnable(ctx, FALSE);
        if (!(opt & OPT_STOP_NO_IORESET))
            rst = SdioIoReset(ctx);
        /* Service key so the result survives this driver instance being unloaded. */
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
            (VOID)WdfRegistryAssignULong(key, &vShut, (ULONG)shut);
            (VOID)WdfRegistryAssignULong(key, &vRst, (ULONG)rst);
            WdfRegistryClose(key);
        }
    }
    return STATUS_SUCCESS;
}
