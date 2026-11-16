# Technical notes

Findings behind the design, for anyone working on the driver. Tested on a Dell Wyse 3040 thin
client (Intel Cherry Trail, 8086:2295 SDIO host controller), Windows 11 26200, sdbus.sys 10.0.26100.7019.

## Source layout

| File | Role |
|------|------|
| `src/driver.c` | KMDF entry, PnP/power, boot re-enumeration, crash guard, diagnostics in the registry |
| `src/sdio.c` | sdbus interface: attach, CMD52/CMD53, block size, bus settings, card interrupt |
| `src/card.c` | mwifiex port: firmware download, host commands, scan, TX (TxPD) / RX (RxPD), events |
| `src/wifi.cpp` | WiFiCx/NetAdapterCx client: capabilities, WDI command handling, data path |
| `src/log.c` | in-memory log ring, flushed to `Parameters\Log` once a second |
| `src/mrvl8897.inf` | Net-class INF (Native 802.11), firmware copy, boot trace AutoLogger |

## Windows SD bus driver (sdbus) behaviour

### The OCR mismatch

`SdbusInitializeCardWorker` has two compare paths for the card's identity:

- **Detection** (work packet 0x0D, used at enumeration): if card type / OCR differ from what was
  saved (or the saved info was cleared), the new values are saved. No failure.
- **Function start** (packet 0x0C, INITIALIZE_CARD, used when the function driver's START reaches
  sdbus): strict compare of the saved values (`socket+1180h` = OCR) with the fresh CMD5 response;
  a mismatch fails the start with `0xC0000001` ("different card").

The 88W8897 answers the very first CMD5 after power-on with OCR **0x800000**, every later one with
**0xFF8000**. So after a cold boot the saved OCR never matches and the first START always fails;
the failed re-init also leaves the card unresponsive until the next power-off.

No `OverrideFlags` bit affects that compare. The per-slot bit **0x200** (controller
`Device Parameters\<slot>\OverrideFlags`) only skips a different check in `SdbusSetPowerWorker`
(card OCR AND slot voltage mask must be non-zero), which the stale 0x800000 also failed. It is not
needed once the card has been re-detected (confirmed on the Wyse 3040 with the flag removed). Never change the global sdbus parameters
(`Services\sdbus\Parameters`): the same driver runs the boot eMMC, and `DisableRuntimePower` there
made the machine unbootable.

### The real cause of failed boots: UHS tuning

An sdbus trace of a failed boot (`mrvl8897-sdbus.etl`) shows that the OCR mismatch at that point
is a consequence, not the cause. The function START after the re-detection first succeeds (CMD5
returns 0xFF8000, matching), then sdbus switches the card to SDR104 (CMD52 write 0x06 to CCCR
0x13, clock 200 MHz). The tuning that follows (CMD19, 11 attempts) fails, the next CMD52 times out
and the step fails with `0xC0000185`. sdbus then re-runs the whole initialisation without a real
power cycle; the card is still set to the voltage selected moments before and now answers CMD5
with OCR 0x040000, which no longer matches the saved 0xFF8000 -> `0xC0000001`. The card stays in
that state until it loses power.

Per-slot `OverrideFlags` (controller `Device Parameters\<slot>`, stored at socket+48h, default =
the global hack flags) bit **0x40** makes `SdbusHighSpeedDetectionWorker` skip the UHS modes.
With it the card runs at 25 MHz, 4 bit (SDP_BUS_CLOCK reports 25000 instead of 200000) and boots
reliably; measured throughput about 45 Mbit/s. Bit 0x40000000 is tested in `SdhcExecuteRetuning`
(not tried). The card supports high speed (CCCR 0x13 = 0x01, SHS), so 50 MHz is a possible later
step.

### Re-enumeration

sdbus implements `GUID_REENUMERATE_SELF_INTERFACE_STANDARD` on the function PDO
(`SdbusPdoGetReenumerationInterface`, Size >= 0x28, Version 1). Its
`SurpriseRemoveAndReenumerateSelf` clears the saved card info (`socket+3Ch = 0`), flags the
socket as card-changed and re-detects the card through the 0x0D path (saving the current OCR),
then re-creates the PDO.

The driver uses it like this:

1. `EvtDeviceAdd` of the first instance in a boot (volatile registry value
   `Parameters\ThisBoot\Reenumerated` not set): no card I/O, just `WdfFdoQueryForInterface`.
2. Its `IRP_MN_START_DEVICE` is caught in a WDM preprocess callback: set the volatile value, call
   the interface, complete the START with `STATUS_DEVICE_NOT_READY` (it never reaches sdbus).
3. The re-created device starts normally; the firmware download runs in `EvtDevicePrepareHardware`.

After the firmware has run, any further sdbus re-init (disable/enable, driver update) fails until
a real power-off; the D0Exit path sends `FUNC_SHUTDOWN` and an SDIO I/O reset, which is not enough.
A normal restart works (the platform power-cycles the card); resume from sleep/hibernate is
expected to fail for the same reason (D0Entry does no re-initialisation).

### No software power switch

The BIOS (DSDT) has an ACPI node for SDIO function 1 under the SDIO controller,
`\_SB.PCI0.SDHB.BRC1` (copied from Intel's Broadcom reference design), whose `_PS3`/`_PS0` drive
GPIO `WFD3` (`\_SB.GPO3` = SOUTHEAST community, pin 75) low/high. On the Wyse 3040 it is useless:
Windows does not link the node to the sdbus function PDO (no location path / BIOS name), and pad
75 is not in GPIO mode at all (PADCTRL0 0x00910301: native function 1, GPIO disabled; in the
Cherry View pin list pin 75 is `SDMMC3_1P8_EN`). So a card whose start failed can only be
recovered by a real power loss (unplugging; a normal shutdown keeps standby power on the card).

Re-detecting the card through the re-enumerate interface *after* a failed start makes it worse:
the card does not answer, and sdbus removes the device until the next real power loss. The driver
therefore only logs a failed START (completion routine on the START IRP).

### Firmware download timing

The download (mwifiex `prog_fw_w_helper`, 16-byte header chunks + up to 2 KB data chunks, polled
via `CARD_IO_READY | DN_LD_CARD_RDY` at register 0x50, length at 0x60/0x61) hangs after the first
16-byte chunk unless sdbus's WPP tracing is enabled. Delays in our own code (5 ms around chunks)
do not help, so the difference is inside sdbus (it logs from its interrupt DPC). The INF keeps an
AutoLogger session on both sdbus WPP providers (`{3B9E3DA4-70B8-46D3-9EF2-3DDF128BDED8}`,
`{9C205A39-1250-487D-ABD7-E831C6290539}`, level 0xFF, all flags). The AutoLogger `FileName` must be
a `REG_SZ` full path; `REG_EXPAND_SZ` with `%SystemRoot%` makes the session fail with status 0x57.

## mwifiex details used

- SD8897 register set: `start_wr_port 0`, `data_port_mask 0xffffffff`, write bitmap at 0x08–0x0b,
  read bitmap 0x04–0x07, read lengths from 0x0c, host int status 0x03, CMD53 "new mode", command
  port `ioport | 0x8000`, data ports `ioport + n`.
- Key API: HW-spec TLV 0x1C7 (api id 1) selects `KEY_MATERIAL` v1 or v2; the driver logs the
  version it found and supports both for CCMP keys.
- RX `rx_pkt_type`: this firmware uses **2** for ordinary data; only 0xE5 (management) and 0xE7
  (block-ack request) are not data. Frames are 802.3 + SNAP or Ethernet II.
- Association: `HostCmd_CMD_802_11_ASSOCIATE` (0x12) with SSID, DS, CF, rates, auth type
  (0x11F, open), channel list and an RSN IE TLV (group cipher from the AP, pairwise CCMP, AKM PSK
  or 802.1X). The response is the IEEE association response (cap, status, AID, IEs).

## 802.11n

- `GET_HW_SPEC` gives `fw_cap_info` (bit 11 = 11n), `dot_11n_dev_cap` and `dev_mcs_support`
  (offsets 34, 38, 42); `11N_CFG` (0xCD) enables HT TX at start.
- ASSOCIATE adds an HT capabilities TLV (45), the AP's HT operation TLV (61, secondary channel
  cleared for 20 MHz), a WMM IE (221, 00:50:F2:02) and sets the secondary-channel offset in the
  channel-list TLV (bits 4-5 of the radio type). 40 MHz is only used on 5 GHz.
- RX type 0xE6 = A-MSDU (subframes DA/SA/length + SNAP, padded to 4).
- Block-ack: `EVENT_ADDBA` (0x33, body = the AP's ADDBA request) is answered with
  `11N_ADDBA_RSP` (0xCF): accept, window 64, no A-MSDU in A-MPDU. `EVENT_BA_STREAM_TIMEOUT` (0x37)
  -> `11N_DELBA` (0xD0). Events arrive with the I/O lock held, so the answers are sent from a work
  item. There is no host reorder buffer yet.

## WiFiCx notes

- WiFiCx 1.0 / NetAdapterCx 2.1 / KMDF 1.33 (WDK defaults for WiFiCx 1.0); TLV generator/parser
  from `km/wlan/2.0` (`WificxTLVGenParse.lib`), compiled as C++17; the driver provides
  `operator new/delete` for it.
- Capabilities are set in `EvtDevicePrepareHardware` (after the firmware is up); WiFiCx then calls
  `EvtWifiDeviceCreateAdapter`. The station adapter needs the MAC address at that point.
- Messages seen at start: `SET_ADAPTER_CONFIGURATION` (85), `DOT11_RESET` (7), `GET_STATISTICS`
  (34, not supported), `SCAN` (4), 75 (not supported), `SET_LOCATION_PRIVACY` (138).
- Data path: the driver treats TX packets as native 802.11 data frames (ToDS) and falls back to
  passing them through if they already look like Ethernet II; RX frames are indicated as 802.11
  (FromDS). Conversion to/from Ethernet II uses RFC1042 / bridge-tunnel SNAP headers. The first
  four TX and RX frames are hex-dumped to the log.
