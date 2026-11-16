# mrvl8897 — Marvell 88W8897 SDIO Wi-Fi driver for Windows 11

**Version 1.8**

A Windows 11 x64 driver for the **Marvell 88W8897 Wi-Fi chip on SDIO**, developed and tested on
the **Dell Wyse 3040** thin client (Intel Cherry Trail, SDIO host 8086:2295). Marvell only ever
shipped a Windows driver for the PCIe variant; on these machines the Wi-Fi shows up as an unknown
device:

```
SD\VID_02DF&PID_912C&FN_1      (function 1 = Wi-Fi; FN_2/FN_3 are Bluetooth)
```

The hardware side is ported from the Linux **mwifiex** driver (firmware download, SDIO register
map, host commands, TX/RX descriptors). The Windows side is a **WiFiCx / NetAdapterCx** client, so
the card is a normal Windows Wi-Fi adapter: networks in the taskbar, connect with a password, DHCP,
everything as usual.

## Status

Working:
- Firmware download and start at boot, without any helper task or script
- Scanning (2.4 GHz channels 1–13, 5 GHz 36–48 / 149–165), live network list in Windows
- Connecting to open and WPA2-Personal networks (CCMP); the WPA2 handshake is done by Windows
- Data in both directions (internet works)
- Disconnect, link loss and AP-initiated disconnect
- 802.11n (HT): up to 2 streams, short guard interval, 40 MHz on 5 GHz (link speed up to
  300 Mbps), WMM/QoS, A-MSDU receive, RX block-ack sessions (A-MPDU from the router)
- Warm restarts

Not yet:
- Other machines with the SDIO 88W8897 are untested (they should work if the SD host is
  handled by Windows' sdbus)
- 802.11ac; DFS channels 52-144; TX block-ack (upload aggregation); RX reorder buffer
- Real throughput is far below the link speed (about 45 Mbit/s measured with iperf): the SDIO
  bus runs at 25 MHz (see step 1.4 of the install) and every packet is a separate SDIO transfer
  (no SDIO multi-port aggregation yet)
- WPA3-only networks (WPA2/WPA3 mixed networks connect via WPA2), TKIP pairwise, WEP
- Sleep / hibernate / Fast Startup: Wi-Fi will probably not come back after resume (the card
  can't be re-initialised without a power cycle); use Shut down / Restart instead of sleep
- Scanning while connected (the list is frozen while connected)

## Install

The driver is **test-signed** (not signed by Microsoft), so the PC must allow test-signed drivers.

### 1. One-time preparation

1. Turn **Secure Boot off** in the BIOS/UEFI setup.
2. In an administrator command prompt (`mrvl8897-test.cer` comes from the build, see below):
   ```
   bcdedit /set testsigning on
   certutil -addstore Root mrvl8897-test.cer
   certutil -addstore TrustedPublisher mrvl8897-test.cer
   ```
3. Restart.
4. Limit the SDIO bus speed. Without this, Windows' SD bus driver switches the card to its 200 MHz
   UHS mode (SDR104), whose signal tuning fails on some boots and leaves the card dead until it
   loses power (the "flaky boot" with a yellow bang). In an administrator PowerShell:
   ```powershell
   $wifi = Get-PnpDevice | ? InstanceId -match '^SD\\VID_02DF&PID_912C&FN_1'
   $ctl  = (Get-PnpDeviceProperty -InstanceId $wifi.InstanceId -KeyName DEVPKEY_Device_Parent).Data
   foreach ($slot in 0, 1) { reg add "HKLM\SYSTEM\CurrentControlSet\Enum\$ctl\Device Parameters\$slot" /v OverrideFlags /t REG_DWORD /d 0x40 /f }
   ```
   This only affects the Wi-Fi's SDIO controller (per-slot setting under its own registry
   entry), not the eMMC the system boots from. It applies from the next boot.

### 2. Install the driver

Device Manager → the unknown `SD\VID_02DF&PID_912C&FN_1` device → **Update driver** →
**Browse my computer for drivers** → **Let me pick from a list** → **Have Disk…** →
select `mrvl8897.inf` → OK → Next.

### 3. Restart

Windows asks for a restart after the install; restart when asked. The device usually shows a
yellow bang (code 10) right after the install: Windows starts it immediately, and that start
fails. From then on Wi-Fi comes up by itself at boot, restarts included (with the occasional
failed boot listed under "Not yet").

Disabling and re-enabling the device in Device Manager has the same problem as starting it right
after an install; restart (or power off) instead.

If the device is missing or shows a yellow bang after the restart, shut down, **unplug the power
cord** for a few minutes and power on again: a "shut down" leaves standby power on the card, and
a card that failed to start only recovers once it really loses power. There is no software way
to power-cycle the card on the Wyse 3040 (see docs/TECHNICAL.md).

## Uninstall (remove every version)

In an administrator PowerShell:

```powershell
Get-WindowsDriver -Online | ? { $_.OriginalFileName -like '*\mrvl8897.inf' } | % { pnputil /delete-driver $_.Driver /uninstall /force }
Get-PnpDevice | ? InstanceId -match '^SD\\VID_02DF&PID_912[CD]&FN_1' | % { pnputil /remove-device $_.InstanceId }
reg delete HKLM\SYSTEM\CurrentControlSet\Control\WMI\Autologger\mrvl8897-sdbus /f
reg delete HKLM\SYSTEM\CurrentControlSet\Services\mrvl8897 /f
Remove-Item "$env:windir\System32\drivers\mrvl8897.sys","$env:windir\System32\drivers\sd8897_uapsta.bin" -ErrorAction SilentlyContinue
```

To also undo the bus-speed limit from install step 1.4, delete the `OverrideFlags` values again
(same paths as there, `reg delete ... /v OverrideFlags /f`).

This removes all mrvl8897 packages from the driver store, the device entry, the boot trace, the
driver's settings and logs, and the installed files. Restart afterwards; the card shows up as an
unknown device again.

## Build

Requirements: **Visual Studio 2022** (C++ x64 build tools) and the **Windows Driver Kit 10.0.26100**.

```
pwsh -NoProfile -File build.ps1
```

Output:
- `out\driver\` — `mrvl8897.inf`, `mrvl8897.sys`, `mrvl8897.cat`, `sd8897_uapsta.bin` (install this folder)
- `out\mrvl8897-test.cer` — the self-signed test certificate the package is signed with

The script creates the test certificate in your user certificate store on first run and reuses it after.

## How it works

See [docs/TECHNICAL.md](docs/TECHNICAL.md) for the details. In short:

- **Firmware**: the INF installs `sd8897_uapsta.bin` to `C:\Windows\System32\drivers\`; the driver
  downloads it to the card over SDIO on every start (mwifiex protocol), then runs `FUNC_INIT`,
  `GET_HW_SPEC` (MAC address) and enables the radio.
- **Boot fix**: right after power-on the card reports a different voltage window (OCR 0x800000)
  than on any later initialisation (0xFF8000). Windows' SD bus driver remembers the first value
  and rejects the function's start as a "different card". On the first start of each boot the
  driver therefore asks the SD bus driver to re-detect the card (the documented
  `GUID_REENUMERATE_SELF_INTERFACE_STANDARD` interface, which sdbus implements); the re-created
  device then starts normally.
- **Boot trace**: the firmware download only completes while the SD bus driver's WPP tracing is
  active (it changes sdbus's timing). The INF therefore registers a Windows boot trace
  ("AutoLogger") `mrvl8897-sdbus` with a 64 MB ring file in `C:\Windows\System32\LogFiles\WMI\`.
- **Wi-Fi**: WDI commands from WiFiCx (scan, connect, keys, disconnect, radio state) are mapped to
  mwifiex host commands (`802_11_SCAN`, `802_11_ASSOCIATE` with an RSN IE, `KEY_MATERIAL`,
  `DEAUTHENTICATE`). Data frames are converted between Windows' native 802.11 format and the
  Ethernet II frames (+ TxPD/RxPD) the firmware uses.
- **Crash guard**: if two automatic start attempts in a row never finish (the machine crashed),
  the driver stops starting automatically. Reset with
  `reg add HKLM\SYSTEM\CurrentControlSet\Services\mrvl8897\Parameters /v AutoAttempts /t REG_DWORD /d 0 /f`.

## Troubleshooting

The driver keeps an internal log in the registry (refreshed every second):

```
HKLM\SYSTEM\CurrentControlSet\Services\mrvl8897\Parameters
    Log           this driver load (REG_MULTI_SZ)
    LogPrev       the previous driver load (the boot instance unloads before the real start)
    BringUpStage  "ok" when the firmware is up, otherwise the failing step
    MacAddress, FwVersion, AutoAttempts
```

`SdioFlags` (DWORD, same key) holds debug options; 0 is the normal value. Bit `0x800` turns
802.11n off (connect as 802.11a/g), bit `0x200` disables the pacing delays around firmware
chunks.

If Wi-Fi doesn't come up after an install or update, do the cold power-cycle from step 3.

## Licence

- Driver: **GPL-2.0** (derived from the Linux mwifiex driver), see [LICENSE](LICENSE).
- Firmware `firmware/sd8897_uapsta.bin`: © Marvell, redistributed under
  [firmware/LICENCE.Marvell](firmware/LICENCE.Marvell) (from linux-firmware, `mrvl/sd8897_uapsta.bin`).

## Credits

- Linux **mwifiex** driver (Marvell and the Linux wireless community) — the hardware protocol.
- Microsoft's WiFiCx sample (`Windows-driver-samples/network/wlan/wificx`) — the WiFiCx structure.
