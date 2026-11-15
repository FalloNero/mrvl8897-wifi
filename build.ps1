# Builds the mrvl8897 driver package (x64, KMDF 1.33 + NetAdapterCx 2.1 + WiFiCx 1.0).
#
# Needs: Visual Studio 2022 with the C++ x64 tools, and the Windows Driver Kit 10.0.26100.
# Run:   pwsh -NoProfile -File build.ps1
# Output: out\driver\  (mrvl8897.inf / .sys / .cat + firmware), test-signed
#         out\mrvl8897-test.cer  (the test certificate to trust on the target PC)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src  = Join-Path $root 'src'
$out  = Join-Path $root 'out'
$obj  = Join-Path $out 'obj'
$pkg  = Join-Path $out 'driver'
New-Item -ItemType Directory -Force $obj, $pkg | Out-Null

$kits   = 'C:\Program Files (x86)\Windows Kits\10'
$ver    = '10.0.26100.0'
$kmdf   = '1.33'
$netcx  = '2.1'
$wificx = '1.0'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { $vs = & $vswhere -latest -products * -property installationPath }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$inc = @(
  "$kits\Include\$ver\km", "$kits\Include\$ver\km\crt", "$kits\Include\$ver\shared",
  "$kits\Include\wdf\kmdf\$kmdf",
  "$kits\Include\$ver\km\netcx\kmdf\adapter\$netcx", "$kits\Include\$ver\shared\netcx\shared\1.0",
  "$kits\Include\$ver\km\wificx\$wificx", "$kits\Include\$ver\km\wlan\2.0"
) | ForEach-Object { "/I`"$_`"" }

$cflags = @('/nologo','/c','/kernel','/W4','/WX','/O2','/Oi','/GS','/Gy','/Zi','/Zc:wchar_t-','/GR-',
  '/D_AMD64_','/DAMD64','/D_WIN64','/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00',
  '/DWINVER=0x0A00','/DPOOL_NX_OPTIN=1','/DPOOL_ZERO_DOWN_LEVEL_SUPPORT',
  '/DKMDF_VERSION_MAJOR=1','/DKMDF_VERSION_MINOR=33',
  '/DNET_VERSION_MAJOR=2','/DNET_VERSION_MINOR=1','/DWIFI_VERSION_MAJOR=1','/DWIFI_VERSION_MINOR=0',
  '/wd4201','/wd4214','/wd4324',"/Fd`"$obj\mrvl8897.pdb`"","/Fo`"$obj\\`"")

$libs = @("$kits\Lib\$ver\km\x64\ntoskrnl.lib","$kits\Lib\$ver\km\x64\hal.lib",
  "$kits\Lib\$ver\km\x64\sdbus.lib","$kits\Lib\$ver\km\x64\BufferOverflowFastFailK.lib",
  "$kits\Lib\wdf\kmdf\x64\$kmdf\WdfDriverEntry.lib","$kits\Lib\wdf\kmdf\x64\$kmdf\WdfLdr.lib",
  "$kits\Lib\$ver\km\x64\netcx\kmdf\adapter\$netcx\netadaptercxstub.lib",
  "$kits\Lib\$ver\km\x64\wificx\$wificx\wificxstub.lib",
  "$kits\Lib\$ver\km\x64\wlan\2.0\WificxTLVGenParse.lib",
  "$kits\Lib\$ver\km\x64\ndis.lib") | ForEach-Object { "`"$_`"" }

$lflags = @('/nologo','/DRIVER','/SUBSYSTEM:NATIVE,10.00','/ENTRY:FxDriverEntry','/NODEFAULTLIB',
  '/MACHINE:X64','/DEBUG','/OPT:REF','/OPT:ICF','/INCREMENTAL:NO','/MERGE:_TEXT=.text;_PAGE=PAGE',
  '/SECTION:INIT,d','/RELEASE','/GUARD:CF',
  "/OUT:`"$pkg\mrvl8897.sys`"","/PDB:`"$out\mrvl8897.pdb`"")

$csrc = 'driver.c','sdio.c','card.c','log.c' | ForEach-Object { "`"$src\$_`"" }
$cpp  = 'wifi.cpp' | ForEach-Object { "`"$src\$_`"" }
$objs = 'driver.obj','sdio.obj','card.obj','log.obj','wifi.obj' | ForEach-Object { "`"$obj\$_`"" }

$cmd = "call `"$vcvars`" >nul && " +
       "cl $($cflags -join ' ') $($inc -join ' ') /guard:cf $($csrc -join ' ') && " +
       "cl $($cflags -join ' ') /std:c++17 /wd4471 $($inc -join ' ') /guard:cf $($cpp -join ' ') && " +
       "link $($lflags -join ' ') $($objs -join ' ') $($libs -join ' ')"
cmd /c $cmd 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "compile/link failed ($LASTEXITCODE)" }

Copy-Item "$src\mrvl8897.inf" $pkg -Force
Copy-Item "$root\firmware\sd8897_uapsta.bin" $pkg -Force

# ---- catalog + test signing ----
$bin = "$kits\bin\$ver"
& "$bin\x86\Inf2Cat.exe" /driver:"$pkg" /os:10_X64,10_RS5_X64,10_VB_X64,10_CO_X64,10_NI_X64 /uselocaltime | Out-Host
if ($LASTEXITCODE -ne 0) { throw "inf2cat failed" }

$subject = 'CN=mrvl8897 Test Signing'
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq $subject | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
          -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $cert -FilePath "$out\mrvl8897-test.cer" | Out-Null

$signtool = "$bin\x64\signtool.exe"
foreach ($f in "$pkg\mrvl8897.sys", "$pkg\mrvl8897.cat") {
  & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /s My $f | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f" }
}
Write-Host "`nDriver package: $pkg"
Write-Host "Test certificate: $out\mrvl8897-test.cer"
