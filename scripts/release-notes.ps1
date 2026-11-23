# Writes a release's notes: its section of CHANGELOG.md, the link comparing it with the release before, the file's
# checksum and signature, and a hidden line the program's updater reads: the build number, commit, file, and SHA-256.
#
# Usage: pwsh -File scripts\release-notes.ps1 -Changelog CHANGELOG.md -Section <X.Y.Z|Unreleased> -Repository owner/name
#          -Head <tag> [-Base <tag>] -Asset <file> [-Signature <file>] -Name <build name> -Build <number> -Commit <sha>
#          -Output <file>
param(
  [Parameter(Mandatory = $true)][string]$Changelog,
  [Parameter(Mandatory = $true)][string]$Section,
  [Parameter(Mandatory = $true)][string]$Repository,
  [Parameter(Mandatory = $true)][string]$Head,
  [string]$Base,
  [Parameter(Mandatory = $true)][string]$Asset,
  [string]$Signature,
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][int]$Build,
  [Parameter(Mandatory = $true)][string]$Commit,
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
$file = Split-Path $Asset -Leaf
$hash = (Get-FileHash -Path $Asset -Algorithm SHA256).Hash.ToLowerInvariant()

$notes = New-Object System.Collections.Generic.List[string]
if ($Name -match '-beta$') {
  $notes.Add('Pre-production beta.')
  $notes.Add('')
} elseif ($Name -match '-dev-') {
  $notes.Add('Development build from main, ahead of the next release.')
  $notes.Add('')
}
$notes.AddRange($body)
$notes.Add('')
$notes.Add('---')
$notes.Add('')
$notes.Add($compare)
$notes.Add('')
$notes.Add("``$file`` SHA-256: ``$hash``")
if ($Signature -and (Test-Path $Signature)) {
  $notes.Add('')
  $notes.Add((Get-Content -Path $Signature -Raw -Encoding UTF8).Trim())
}
$update = [ordered]@{ build = $Build; name = $Name; commit = $Commit; asset = $file; sha256 = $hash } | ConvertTo-Json -Compress
$notes.Add('')
$notes.Add("<!-- sstk-update $update -->")
Set-Content -Path $Output -Value $notes -Encoding UTF8
