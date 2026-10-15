$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-ihs-core.ps1')
& (Join-Path $PSScriptRoot 'build-opus.ps1')
$tvLinkSdk = Join-Path $PSScriptRoot 'samsung-emscripten/emscripten-release-bundle/emsdk'
$env:EM_CONFIG = Join-Path $tvLinkSdk '.emscripten'
$env:EM_CACHE = Join-Path $tvLinkSdk '.emscripten_cache'
$tvLinkPython = Join-Path $tvLinkSdk 'python/2.7.13.1_64bit/python-2.7.13.amd64/python.exe'
$tvLinkEmcc = Join-Path $tvLinkSdk 'fastcomp/emscripten/emcc.py'
$tvLinkFlags = @('-O1', '-pthread', '-s', 'WASM=1', '-s', 'USE_PTHREADS=1', '-s', 'USE_SDL=2', '-s', 'ENVIRONMENT_MAY_BE_TIZEN=1', '-s', 'PROXY_TO_PTHREAD=1', '-s', 'PTHREAD_POOL_SIZE=12', '-s', 'TOTAL_MEMORY=134217728')
foreach ($tvLinkInclude in @('ihslib-reference/include', 'ihslib-reference/src', 'protobuf-c')) { $tvLinkFlags += '-I' + (Join-Path $PSScriptRoot $tvLinkInclude) }
$tvLinkFlags += '-I' + (Join-Path $PSScriptRoot 'moonlight-platform/opus/include')
# See build-ihs-core.ps1: emcc warnings on stderr must not abort the script.
$ErrorActionPreference = 'Continue'
& $tvLinkPython $tvLinkEmcc (Join-Path $PSScriptRoot 'steam-tv/probe.c') (Join-Path $PSScriptRoot 'steam-tv/media.cpp') (Join-Path $PSScriptRoot 'steam-tv/gamepad.c') (Join-Path $PSScriptRoot 'ihs-core.bc') (Join-Path $PSScriptRoot 'opus.bc') @tvLinkFlags -o (Join-Path $PSScriptRoot 'steam-tv/ihs.js')
$tvLinkLinkExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($tvLinkLinkExit -ne 0) { throw 'Steam TV probe linking failed.' }
