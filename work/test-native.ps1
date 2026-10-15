$ErrorActionPreference = 'Stop'
$tvTestSdk = Join-Path $PSScriptRoot 'samsung-emscripten/emscripten-release-bundle/emsdk'
$env:EM_CONFIG = Join-Path $tvTestSdk '.emscripten'
$env:EM_CACHE = Join-Path $tvTestSdk '.emscripten_cache'
$tvTestFlags = @('-O1', '-pthread', '-s', 'WASM=1', '-s', 'USE_PTHREADS=1', '-s', 'USE_SDL=2', '-s', 'PROXY_TO_PTHREAD=1', '-s', 'PTHREAD_POOL_SIZE=4', '-s', 'WASM_ASYNC_COMPILATION=0', '-s', 'TOTAL_MEMORY=134217728')
$tvTestFlags += @('--pre-js', (Join-Path $PSScriptRoot 'test-pre.js'))
foreach ($tvTestInclude in @('ihslib-reference/include', 'ihslib-reference/src', 'protobuf-c', 'moonlight-platform/opus/include')) { $tvTestFlags += '-I' + (Join-Path $PSScriptRoot $tvTestInclude) }
# emcc warnings and Node notices both land on stderr; see build-ihs-core.ps1.
$ErrorActionPreference = 'Continue'
& (Join-Path $tvTestSdk 'python/2.7.13.1_64bit/python-2.7.13.amd64/python.exe') (Join-Path $tvTestSdk 'fastcomp/emscripten/emcc.py') (Join-Path $PSScriptRoot 'test-native.c') (Join-Path $PSScriptRoot 'test-media-append.cpp') (Join-Path $PSScriptRoot 'ihs-core.bc') (Join-Path $PSScriptRoot 'opus.bc') @tvTestFlags -o (Join-Path $PSScriptRoot 'test-native.js')
$tvTestBuildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($tvTestBuildExit -ne 0) { throw 'Native test build failed.' }
$ErrorActionPreference = 'Continue'
$tvTestOutput = & node (Join-Path $PSScriptRoot 'run-native.cjs') 2>&1
$tvTestRunExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
foreach ($tvTestLine in $tvTestOutput) { Write-Host $tvTestLine }
if ($tvTestRunExit -ne 0) { throw 'Native tests failed.' }
