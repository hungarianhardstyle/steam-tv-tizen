$ErrorActionPreference = 'Stop'
$tvOpusRoot = Join-Path $PSScriptRoot 'moonlight-platform/opus'
$tvOpusSdk = Join-Path $PSScriptRoot 'samsung-emscripten/emscripten-release-bundle/emsdk'
$env:EM_CONFIG = Join-Path $tvOpusSdk '.emscripten'
$env:EM_CACHE = Join-Path $tvOpusSdk '.emscripten_cache'
$tvOpusSources = @()
foreach ($tvOpusFile in @('opus_sources.mk', 'celt_sources.mk', 'silk_sources.mk')) {
    $tvOpusMake = Get-Content -Raw -LiteralPath (Join-Path $tvOpusRoot $tvOpusFile)
    foreach ($tvOpusMatch in [regex]::Matches($tvOpusMake, '(?m)^(?:OPUS_SOURCES|OPUS_SOURCES_FLOAT|CELT_SOURCES|SILK_SOURCES|SILK_SOURCES_FLOAT)\s*=\s*((?:[^\r\n]*\\\r?\n)*[^\r\n]*)')) {
        foreach ($tvOpusSource in [regex]::Matches($tvOpusMatch.Groups[1].Value, '[\w/]+\.c')) {
            $tvOpusSources += Join-Path $tvOpusRoot $tvOpusSource.Value
        }
    }
}
if ($tvOpusSources.Count -lt 100) { throw 'Incomplete portable Opus source list.' }
$tvOpusFlags = @('-O2', '-pthread', '-DOPUS_BUILD', '-DUSE_ALLOCA', '-DHAVE_LRINT', '-DHAVE_LRINTF')
foreach ($tvOpusInclude in @('include', 'celt', 'silk', 'silk/float')) { $tvOpusFlags += '-I' + (Join-Path $tvOpusRoot $tvOpusInclude) }
# See build-ihs-core.ps1: emcc warnings on stderr must not abort the script.
$ErrorActionPreference = 'Continue'
& (Join-Path $tvOpusSdk 'python/2.7.13.1_64bit/python-2.7.13.amd64/python.exe') (Join-Path $tvOpusSdk 'fastcomp/emscripten/emcc.py') @tvOpusSources @tvOpusFlags -r -o (Join-Path $PSScriptRoot 'opus.bc')
$tvOpusExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($tvOpusExit -ne 0) { throw 'Opus compilation failed.' }
