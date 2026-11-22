# Makes the self-signed certificate that signs every release alike, and the two values the release workflow reads.
# Run it once, on your own machine, then add the values in GitHub under Settings > Secrets and variables > Actions:
#   SIGNING_PFX       the contents of signing.pfx.base64.txt
#   SIGNING_PASSWORD  the contents of password.txt
# Keep the folder private, or delete it once the secrets are saved. signing.cer is the public half: importing it into
# a machine's Trusted Root Certification Authorities makes that machine show the signature as valid. Do that only on
# your own machines.
#
# Usage: powershell -File scripts\new-signing-cert.ps1 [-Output <folder>]
param([string]$Output = (Join-Path $HOME 'ScreenshotToolkit-signing'))

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Output | Out-Null

$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Jason-JP-Yang (self-signed)' -KeyAlgorithm RSA `
  -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy Exportable -CertStoreLocation Cert:\CurrentUser\My `
  -NotAfter (Get-Date).AddYears(5)
$bytes = New-Object byte[] 24
[Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$password = [Convert]::ToBase64String($bytes)

$pfx = Join-Path $Output 'signing.pfx'
Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
Export-Certificate -Cert $certificate -FilePath (Join-Path $Output 'signing.cer') | Out-Null
Remove-Item "Cert:\CurrentUser\My\$($certificate.Thumbprint)"

[Convert]::ToBase64String([IO.File]::ReadAllBytes($pfx)) | Set-Content (Join-Path $Output 'signing.pfx.base64.txt') -Encoding ASCII
$password | Set-Content (Join-Path $Output 'password.txt') -Encoding ASCII

Write-Host "Made $($certificate.Subject), thumbprint $($certificate.Thumbprint), valid until $($certificate.NotAfter.ToString('yyyy-MM-dd'))."
Write-Host "Files are in $Output."
