# Pyokomin (ADR-0063): builds and runs the native unit tests of the fork.
# Needs the Visual Studio C++ tools (found with vswhere).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH"
$vs = & (Join-Path $installer "vswhere.exe") -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$out = Join-Path $env:TEMP "webview_windows_native_tests"
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $root "windows\test\child_focus_test.cc"
Push-Location $out
try {
  cmd /c "`"$vcvars`" >nul && cl /nologo /EHsc /std:c++17 /W4 /WX `"$src`" user32.lib >nul"
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  & (Join-Path $out "child_focus_test.exe")
  $code = $LASTEXITCODE
} finally {
  Pop-Location
}
exit $code
