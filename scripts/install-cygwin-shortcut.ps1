param(
    [string]$InstallDirectory = $PSScriptRoot,
    [string]$WorkingDirectory = (Join-Path $env:USERPROFILE 'Work')
)

$ErrorActionPreference = 'Stop'
$install = (Resolve-Path -LiteralPath $InstallDirectory).Path
$launcher = Join-Path $install 'morph.cmd'
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
    throw "Morph launcher is missing: $launcher"
}

if (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
    $WorkingDirectory = $env:USERPROFILE
}

$desktop = [Environment]::GetFolderPath('Desktop')
if (-not $desktop -or -not (Test-Path -LiteralPath $desktop -PathType Container)) {
    throw 'Windows desktop directory is unavailable.'
}

$icon = Join-Path $install 'morph.ico'
if (-not (Test-Path -LiteralPath $icon -PathType Leaf)) {
    $icon = Join-Path $install 'bin\morph.exe'
}

$shell = New-Object -ComObject WScript.Shell
$shortcutPath = Join-Path $desktop 'Morph.lnk'
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $env:ComSpec
$shortcut.Arguments = '/d /k ""{0}""' -f $launcher
$shortcut.WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
$shortcut.IconLocation = "$icon,0"
$shortcut.Description = 'Morph interactive CLI'
$shortcut.WindowStyle = 1
$shortcut.Save()

[pscustomobject]@{
    Shortcut = $shortcutPath
    Target = $shortcut.TargetPath
    Arguments = $shortcut.Arguments
    WorkingDirectory = $shortcut.WorkingDirectory
    Icon = $shortcut.IconLocation
} | ConvertTo-Json
