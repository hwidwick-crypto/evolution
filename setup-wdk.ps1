#
# Downloads the official WDK (Microsoft.Windows.WDK.x64 nuget package) and
# extracts it to wdk\nuget. No administrator rights or WDK installation needed -
# the driver project just points its include/lib paths into the extracted folder.
#
param(
    [string]$Version = "10.0.26100.6584"
)

$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$wdir  = Join-Path $root "wdk"
$stamp = Join-Path $wdir "nuget\.extracted"

if (Test-Path $stamp) {
    Write-Host "[+] WDK already present: $wdir\nuget"
    exit 0
}

New-Item -ItemType Directory -Force -Path $wdir | Out-Null

$pkg = Join-Path $wdir "wdk.nupkg"
$url = "https://www.nuget.org/api/v2/package/Microsoft.Windows.WDK.x64/$Version"

if (-not (Test-Path $pkg)) {
    Write-Host "[~] downloading WDK $Version (~106 MB)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $pkg -UseBasicParsing
}

$extract = Join-Path $wdir "nuget"
if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
Write-Host "[~] extracting..."
Expand-Archive -Path $pkg -DestinationPath $extract -Force

$probe = Join-Path $extract "c\Include\10.0.26100.0\km\wdm.h"
if (-not (Test-Path $probe)) {
    Write-Host "[!] WDK extraction failed, expected file not found: $probe"
    exit 1
}

Set-Content -Path $stamp -Value $Version -Encoding ascii
Write-Host "[+] WDK ready: $extract\c"
exit 0
