/*
 * In-memory log ring, mirrored to DbgPrint and flushed to the registry
 * (Services\mrvl8897\Parameters, REG_MULTI_SZ "Log") once a second by a
 * system thread, so it survives a hung call. GPL v2.
 */
#include "mrvl8897.h"
#include <ntstrsafe.h>
#include <stdarg.h>

#define LOG_LINES     128
#define LOG_LINE_LEN  200

static CHAR       g_Log[LOG_LINES][LOG_LINE_LEN];
static volatile LONG g_LogNext;   /* total lines ever written */
static KSPIN_LOCK g_LogLock;
static ULONGLONG  g_LogT0;

static WDFDRIVER  g_FlushDriver;
static KEVENT     g_FlushStop;
static PVOID      g_FlushThread;  /* referenced thread object */
static FAST_MUTEX g_DumpLock;

VOID MrvlLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    ExInitializeFastMutex(&g_DumpLock);
    g_LogT0 = KeQueryInterruptTime();
}

VOID MrvlLog(PCSTR Fmt, ...)
{
    CHAR line[LOG_LINE_LEN];
    CHAR body[LOG_LINE_LEN - 24];
    va_list ap;
    KIRQL irql;
    ULONG ms = (ULONG)((KeQueryInterruptTime() - g_LogT0) / 10000);

    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfA(body, sizeof(body), Fmt, ap);
    va_end(ap);

    (VOID)RtlStringCbPrintfA(line, sizeof(line), "%7u.%03u %s", ms / 1000, ms % 1000, body);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "mrvl8897: %s\n", line);

    KeAcquireSpinLock(&g_LogLock, &irql);
    RtlCopyMemory(g_Log[g_LogNext % LOG_LINES], line, sizeof(line));
    g_LogNext++;
    KeReleaseSpinLock(&g_LogLock, irql);
}

/* PASSIVE_LEVEL only */
VOID MrvlLogDumpToKey(WDFKEY Key)
{
    DECLARE_CONST_UNICODE_STRING(vLog, L"Log");
    ULONG first, count, i, j, n = 0;
    KIRQL irql;
    PCHAR snap;
    PWCHAR buf;
    SIZE_T cb = (SIZE_T)LOG_LINES * LOG_LINE_LEN * sizeof(WCHAR) + 2 * sizeof(WCHAR);

    snap = (PCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)LOG_LINES * LOG_LINE_LEN, MRVL_TAG);
    buf = (PWCHAR)ExAllocatePool2(POOL_FLAG_PAGED, cb, MRVL_TAG);
    if (!snap || !buf)
        goto out;

    KeAcquireSpinLock(&g_LogLock, &irql);
    count = min((ULONG)g_LogNext, LOG_LINES);
    first = (ULONG)g_LogNext - count;
    for (i = 0; i < count; i++)
        RtlCopyMemory(snap + i * LOG_LINE_LEN, g_Log[(first + i) % LOG_LINES], LOG_LINE_LEN);
    KeReleaseSpinLock(&g_LogLock, irql);

    for (i = 0; i < count; i++) {
        PCHAR l = snap + i * LOG_LINE_LEN;
        for (j = 0; j < LOG_LINE_LEN - 1 && l[j]; j++)
            buf[n++] = (WCHAR)(UCHAR)l[j];
        buf[n++] = L'\0';
    }
    if (n == 0)
        buf[n++] = L'\0';
    buf[n++] = L'\0';

    ExAcquireFastMutex(&g_DumpLock);
    (VOID)WdfRegistryAssignValue(Key, &vLog, REG_MULTI_SZ, n * sizeof(WCHAR), buf);
    ExReleaseFastMutex(&g_DumpLock);
out:
    if (snap) ExFreePoolWithTag(snap, MRVL_TAG);
    if (buf) ExFreePoolWithTag(buf, MRVL_TAG);
}

static KSTART_ROUTINE FlushThread;

static VOID FlushThread(PVOID Context)
{
    LONG last = -1;
    LARGE_INTEGER t;
    NTSTATUS w;
    WDFKEY key;
    UNREFERENCED_PARAMETER(Context);

    t.QuadPart = -10000LL * 1000;   /* 1 s */
    for (;;) {
        w = KeWaitForSingleObject(&g_FlushStop, Executive, KernelMode, FALSE, &t);
        if (g_LogNext != last) {
            last = g_LogNext;
            if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_FlushDriver, KEY_WRITE,
                                                              WDF_NO_OBJECT_ATTRIBUTES, &key))) {
                MrvlLogDumpToKey(key);
                WdfRegistryClose(key);
            }
        }
        if (w == STATUS_SUCCESS)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID MrvlLogStartFlusher(WDFDRIVER Driver)
{
    HANDLE h;
    g_FlushDriver = Driver;
    KeInitializeEvent(&g_FlushStop, NotificationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, FlushThread, NULL))) {
        (VOID)ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, &g_FlushThread, NULL);
        ZwClose(h);
    }
}

VOID MrvlLogStopFlusher(VOID)
{
    if (g_FlushThread) {
        KeSetEvent(&g_FlushStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(g_FlushThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_FlushThread);
        g_FlushThread = NULL;
    }
}
