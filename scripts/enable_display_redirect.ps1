param(
    [string]$ManifestPath = "C:\Program Files (x86)\Steam\steamapps\common\PlayStation VR2 App\SteamVR_Plug-In\driver.vrdrivermanifest",
    [switch]$Disable
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    throw "Manifest not found: $ManifestPath"
}

$backupPath = "$ManifestPath.before-display-redirect"
if (-not (Test-Path -LiteralPath $backupPath)) {
    Copy-Item -LiteralPath $ManifestPath -Destination $backupPath
    Write-Host "Backup created: $backupPath"
}

$json = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$value = -not $Disable

if ($json.PSObject.Properties.Name -contains "redirectsDisplay") {
    $json.redirectsDisplay = $value
} else {
    $json | Add-Member -NotePropertyName redirectsDisplay -NotePropertyValue $value
}

$json | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8

$check = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
Write-Host "redirectsDisplay = $($check.redirectsDisplay)"
Write-Host "Manifest: $ManifestPath"

if ($Disable) {
    Write-Host "Display redirect disabled in manifest."
} else {
    Write-Host "Display redirect enabled in manifest. Restart SteamVR completely before testing."
}
