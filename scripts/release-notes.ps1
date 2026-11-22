# Writes a release's notes: its section of CHANGELOG.md, the link comparing it with the release before, and the file's
# checksum and signature.
#
# Usage: pwsh -File scripts\release-notes.ps1 -Changelog CHANGELOG.md -Section <X.Y.Z|Unreleased> -Repository owner/name
#          -Head <tag> [-Base <tag>] -Asset <file> [-Signature <file>] -Output <file>
param(
  [Parameter(Mandatory = $true)][string]$Changelog,
  [Parameter(Mandatory = $true)][string]$Section,
  [Parameter(Mandatory = $true)][string]$Repository,
  [Parameter(Mandatory = $true)][string]$Head,
  [string]$Base,
  [Parameter(Mandatory = $true)][string]$Asset,
  [string]$Signature,
  [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = 'Stop'

# The lines under "## <Section>", up to the next section, without the blank lines around them.
$body = New-Object System.Collections.Generic.List[string]
$inside = $false
foreach ($line in Get-Content -Path $Changelog -Encoding UTF8) {
  if ($line -match '^## (.+?)\s*$') {
    if ($inside) { break }
    $inside = $Matches[1] -eq $Section
    continue
  }
  if ($inside) { $body.Add($line) }
}
while ($body.Count -gt 0 -and $body[0].Trim() -eq '') { $body.RemoveAt(0) }
while ($body.Count -gt 0 -and $body[$body.Count - 1].Trim() -eq '') { $body.RemoveAt($body.Count - 1) }
if ($body.Count -eq 0) { $body.Add('Nothing is recorded in the changelog for this build yet.') }

$site = "https://github.com/$Repository"
if ($Base) {
  $compare = "**Compare:** [$Base...$Head]($site/compare/$Base...$Head)"
} else {
  $compare = "**Commits:** [$Head]($site/commits/$Head)"
}
$name = Split-Path $Asset -Leaf
$hash = (Get-FileHash -Path $Asset -Algorithm SHA256).Hash.ToLowerInvariant()

$notes = New-Object System.Collections.Generic.List[string]
$notes.Add('Pre-production beta.')
$notes.Add('')
$notes.AddRange($body)
$notes.Add('')
$notes.Add('---')
$notes.Add('')
$notes.Add($compare)
$notes.Add('')
$notes.Add("``$name`` SHA-256: ``$hash``")
if ($Signature -and (Test-Path $Signature)) {
  $notes.Add('')
  $notes.Add((Get-Content -Path $Signature -Raw -Encoding UTF8).Trim())
}
Set-Content -Path $Output -Value $notes -Encoding UTF8
