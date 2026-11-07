# Installs the latest Lumo release on Windows. In PowerShell:
#   irm https://raw.githubusercontent.com/tunaseckin/lumo/main/install.ps1 | iex
# Lumo goes to %LOCALAPPDATA%\Lumo (change with $env:LUMO_HOME) and that folder is added to your PATH.
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # the progress bar makes downloads much slower
$repo = "tunaseckin/lumo"
$home_dir = if ($env:LUMO_HOME) { $env:LUMO_HOME } else { Join-Path $env:LOCALAPPDATA "Lumo" }

$tag = (Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest").tag_name
$exe = Join-Path $home_dir "lumo.exe"
if ((Test-Path $exe) -and ((& $exe --version) -eq "lumo $($tag.TrimStart('v'))")) {
  Write-Host "Lumo $($tag.TrimStart('v')) is already installed and up to date ($home_dir)"
  return
}
Write-Host "Downloading Lumo $tag for Windows..."
$zip = Join-Path ([IO.Path]::GetTempPath()) "lumo-$tag.zip"
Invoke-WebRequest "https://github.com/$repo/releases/download/$tag/lumo-$tag-windows-x64.zip" -OutFile $zip
if (Test-Path $home_dir) { Remove-Item -Recurse -Force $home_dir }
Expand-Archive $zip -DestinationPath $home_dir
Remove-Item $zip
Get-ChildItem -Recurse $home_dir | Unblock-File # no "downloaded from the internet" warnings

$path = [Environment]::GetEnvironmentVariable("Path", "User")
if (($path -split ";") -notcontains $home_dir) {
  $new = if ($path) { "$($path.TrimEnd(';'));$home_dir" } else { $home_dir }
  [Environment]::SetEnvironmentVariable("Path", $new, "User")
}
$env:Path = "$env:Path;$home_dir" # this window too
Write-Host "Installed $(& (Join-Path $home_dir "lumo.exe") --version) in $home_dir"
Write-Host "Try it: lumo `"$home_dir\examples\bricks.lu`"   (new windows find lumo by themselves)"
