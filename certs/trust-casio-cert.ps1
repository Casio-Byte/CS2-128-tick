# Trust the "Casio" code-signing certificate for YOUR Windows user only.
# After this, cs2_128tick.exe shows "Casio" as a verified publisher on this PC.
# Windows will show a security prompt for the Root store - check it says "Casio" and click Yes.
#
# Run:  right-click this file > Run with PowerShell
#   or: powershell -ExecutionPolicy Bypass -File .\trust-casio-cert.ps1
# Undo: powershell -ExecutionPolicy Bypass -File .\trust-casio-cert.ps1 -Remove

param([switch]$Remove)
$cer = Join-Path $PSScriptRoot "casio.cer"
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cer)
Write-Host "Certificate: $($cert.Subject)   thumbprint $($cert.Thumbprint)   expires $($cert.NotAfter)"

foreach ($store in "Root", "TrustedPublisher") {
    $path = "Cert:\CurrentUser\$store\$($cert.Thumbprint)"
    if ($Remove) {
        if (Test-Path $path) { Remove-Item $path; Write-Host "Removed from CurrentUser\$store" }
    } else {
        if (-not (Test-Path $path)) { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\CurrentUser\$store" | Out-Null }
        Write-Host "Trusted in CurrentUser\$store"
    }
}
Read-Host "Done - press Enter to close"
