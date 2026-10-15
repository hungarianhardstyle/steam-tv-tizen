$ErrorActionPreference = 'Stop'
$tvLinkWork = $PSScriptRoot
$tvLinkSdk = Join-Path $tvLinkWork 'samsung-emscripten/emscripten-release-bundle/emsdk'
$env:EM_CONFIG = Join-Path $tvLinkSdk '.emscripten'
$env:EM_CACHE = Join-Path $tvLinkSdk '.emscripten_cache'
$tvLinkPython = Join-Path $tvLinkSdk 'python/2.7.13.1_64bit/python-2.7.13.amd64/python.exe'
$tvLinkEmcc = Join-Path $tvLinkSdk 'fastcomp/emscripten/emcc.py'
$tvLinkSources = @(Get-ChildItem -LiteralPath (Join-Path $tvLinkWork 'ihslib-reference/src') -Filter '*.c' | ForEach-Object { $_.FullName })
foreach ($tvLinkDir in @('client', 'protobuf', 'session', 'hid')) {
    $tvLinkSources += Get-ChildItem -LiteralPath (Join-Path $tvLinkWork "ihslib-reference/src/$tvLinkDir") -Recurse -Filter '*.c' | Where-Object { $_.FullName -notmatch '[\\/]hid[\\/]sdl[\\/]' } | ForEach-Object { $_.FullName }
}
foreach ($tvLinkSource in @('platforms/ihs_ip_posix.c', 'platforms/ihs_udp_posix.c', 'platforms/ihs_thread_sdl.c', 'crypto/impl_mbedtls.c')) {
    $tvLinkSources += Join-Path $tvLinkWork "ihslib-reference/src/$tvLinkSource"
}
$tvLinkSources += Join-Path $tvLinkWork 'protobuf-c/protobuf-c/protobuf-c.c'
$tvLinkSources += Get-ChildItem -LiteralPath (Join-Path $tvLinkWork 'mbedtls/library') -Filter '*.c' | ForEach-Object { $_.FullName }
$tvLinkFlags = @('-O1', '-pthread', '-s', 'USE_PTHREADS=1', '-s', 'USE_SDL=2', '-s', 'ENVIRONMENT_MAY_BE_TIZEN=1')
foreach ($tvLinkInclude in @('ihslib-reference/include', 'ihslib-reference/src', 'protobuf-c', 'mbedtls/include')) {
    $tvLinkFlags += '-I' + (Join-Path $tvLinkWork $tvLinkInclude)
}
# emcc reports ordinary warnings on stderr. Windows PowerShell converts native stderr into an
# error record, which aborts the script under 'Stop', so relax it across the call and treat the
# exit code as the real gate.
$ErrorActionPreference = 'Continue'
& $tvLinkPython $tvLinkEmcc @tvLinkSources @tvLinkFlags -r -o (Join-Path $tvLinkWork 'ihs-core.bc')
$tvLinkCoreExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($tvLinkCoreExit -ne 0) { throw 'IHSlib WASM core compilation failed.' }
if (-not (Test-Path -LiteralPath (Join-Path $tvLinkWork 'ihs-core.bc'))) { throw 'No core build artifact.' }
