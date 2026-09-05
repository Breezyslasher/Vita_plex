<#
.SYNOPSIS
  Authenticode-sign Windows binaries, if a certificate is configured.

.DESCRIPTION
  Signing is what removes the SmartScreen "Unknown Publisher" warning. It needs
  a certificate issued against a verified identity, so it cannot be done for
  free or with a self-signed cert — SmartScreen does not trust those, and the
  warning stays.

  This script is deliberately inert without the secrets. Forks, pull requests
  and any build where WINDOWS_CERT_BASE64 is unset produce unsigned binaries
  exactly as before, and the step reports why rather than failing. That mirrors
  how UPDATE_SIGNING_KEY gates the in-app updater's own signatures.

  Two notes on what signing buys:

    * An OV certificate — including Azure Trusted Signing and the free
      SignPath Foundation programme — is reputation-based. The warning fades
      once enough people have downloaded a signed build, not immediately.
    * An EV certificate clears SmartScreen from the first download, and costs
      considerably more.

  Timestamping is not optional. Without it every signature stops verifying the
  day the certificate expires, including on builds already downloaded.

.PARAMETER Path
  Files to sign. Missing files are skipped, not treated as an error, so the
  caller can pass an installer that a runner without NSIS never produced.

.NOTES
  To use a different signing service instead of a PFX, replace the signtool
  invocation below:

    Azure Trusted Signing  azure/trusted-signing-action@v0
    SignPath Foundation    signpath/github-action-submit-signing-request@v1

  Both are GitHub Actions rather than local tools, so they would live in the
  workflow instead of here; the gating and ordering around them stay the same.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Path
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($env:WINDOWS_CERT_BASE64)) {
    Write-Host 'No WINDOWS_CERT_BASE64 secret — leaving binaries unsigned.'
    Write-Host 'Users will see the SmartScreen "Unknown Publisher" prompt; see sign.ps1.'
    exit 0
}

# signtool ships with the Windows SDK, whose version is in the path, so it is
# found rather than hardcoded. Newest SDK first: an older one may predate the
# /fd and /tr switches used below. Within an SDK, prefer the build's own
# architecture — the arm64 runner can run the x64 signtool under emulation,
# but there is no reason to.
$native = if ($env:PROCESSOR_ARCHITECTURE -match 'ARM64') { 'arm64' } else { 'x64' }
$signtool =
    Get-ChildItem -Path 'C:\Program Files (x86)\Windows Kits\10\bin' `
                  -Filter 'signtool.exe' -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\(x64|arm64)\\' } |
    Sort-Object -Property `
        @{ Expression = { $_.Directory.Parent.Name }; Descending = $true }, `
        @{ Expression = { $_.Directory.Name -eq $native }; Descending = $true } |
    Select-Object -First 1

if (-not $signtool) {
    Write-Host 'signtool.exe not found in the Windows SDK — cannot sign.'
    Write-Host 'Looked under C:\Program Files (x86)\Windows Kits\10\bin'
    exit 0
}
Write-Host "using $($signtool.FullName)"

# The certificate only ever exists as a file for the length of this step.
$pfx = Join-Path ([System.IO.Path]::GetTempPath()) ("vp-" + [guid]::NewGuid() + ".pfx")
try {
    [System.IO.File]::WriteAllBytes(
        $pfx, [System.Convert]::FromBase64String($env:WINDOWS_CERT_BASE64))

    $targets = @($Path | Where-Object { Test-Path $_ })
    if ($targets.Count -eq 0) {
        Write-Host 'Nothing to sign (none of the given paths exist).'
        exit 0
    }

    foreach ($f in $targets) {
        # Not $args: that is an automatic variable in PowerShell.
        $signArgs = @('sign', '/fd', 'SHA256', '/f', $pfx)
        if (-not [string]::IsNullOrWhiteSpace($env:WINDOWS_CERT_PASSWORD)) {
            $signArgs += @('/p', $env:WINDOWS_CERT_PASSWORD)
        }
        # RFC 3161 timestamp, so the signature outlives the certificate.
        $signArgs += @('/tr', 'http://timestamp.digicert.com', '/td', 'SHA256', $f)

        & $signtool.FullName @signArgs
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f (exit $LASTEXITCODE)" }

        & $signtool.FullName 'verify' '/pa' '/v' $f
        if ($LASTEXITCODE -ne 0) { throw "signature did not verify on $f" }
        Write-Host "signed $f"
    }
}
finally {
    Remove-Item $pfx -Force -ErrorAction SilentlyContinue
}
