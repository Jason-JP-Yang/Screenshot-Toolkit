# Signs a program with a self-signed code signing certificate and a timestamp.
#
# The certificate comes from the environment: SIGNING_PFX holds a .pfx file in base64 and SIGNING_PASSWORD its
# password, as scripts\new-signing-cert.ps1 makes them. Without them a certificate is made for this run alone, so a
# build is always signed. Windows does not trust a self-signed certificate: the signature shows the file is unchanged
# and who built it, but SmartScreen still treats the publisher as unknown.
#
# Usage: powershell -File scripts\sign.ps1 -Path <file> [-Summary <file>]
param(
  [Parameter(Mandatory = $true)][string]$Path,
  [string]$Summary
)

$ErrorActionPreference = 'Stop'
$subject = 'CN=Jason-JP-Yang (self-signed)'
$timestamp = 'http://timestamp.digicert.com'
$pfx = Join-Path ([IO.Path]::GetTempPath()) ("signing-" + [guid]::NewGuid().ToString('N') + '.pfx')

try {
  if ($env:SIGNING_PFX) {
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:SIGNING_PFX))
    $password = $env:SIGNING_PASSWORD
    $kind = 'the project certificate'
  } else {
    $made = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyAlgorithm RSA -KeyLength 3072 `
      -HashAlgorithm SHA256 -KeyExportPolicy Exportable -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(1)
    $password = [guid]::NewGuid().ToString('N')
    Export-PfxCertificate -Cert $made -FilePath $pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
    Remove-Item "Cert:\CurrentUser\My\$($made.Thumbprint)"
    $kind = 'a certificate made for this build'
  }
  $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($pfx, $password)

  $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
  if (-not $signtool) {
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
      Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
  }
  if (-not $signtool) { throw 'signtool.exe was not found.' }

  & $signtool sign /fd SHA256 /f $pfx /p $password /d 'Screenshot Toolkit' /tr $timestamp /td SHA256 $Path
  if ($LASTEXITCODE -ne 0) {
    Write-Warning 'The timestamp server did not answer; signing without a timestamp.'
    & $signtool sign /fd SHA256 /f $pfx /p $password /d 'Screenshot Toolkit' $Path
  }
  if ($LASTEXITCODE -ne 0) { throw "signtool failed with $LASTEXITCODE." }

  if ($Summary) {
    "Signed with $kind, $($certificate.Subject), SHA-1 thumbprint ``$($certificate.Thumbprint)``." |
      Set-Content -Path $Summary -Encoding UTF8
  }
} finally {
  if (Test-Path $pfx) { Remove-Item $pfx -Force }
}
