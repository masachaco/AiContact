# Installs the AI Contact skill for Claude Code (%USERPROFILE%\.claude\skills\aicontact).
$src = Join-Path $PSScriptRoot "aicontact"
$dest = Join-Path $env:USERPROFILE ".claude\skills\aicontact"
New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Copy-Item -Recurse $src $dest
Write-Host "installed: $dest"
