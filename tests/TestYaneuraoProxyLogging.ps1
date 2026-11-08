param(
    [Parameter(Mandatory=$true)][string]$TestExecutable,
    [Parameter(Mandatory=$true)][string]$ProxyDll,
    [Parameter(Mandatory=$true)][string]$FontFile,
    [switch]$ExpectLogging,
    [string]$SandboxParent = (Join-Path $env:LOCALAPPDATA 'Temp\opencode')
)

$ErrorActionPreference = 'Stop'
$TestExecutable = (Resolve-Path $TestExecutable).Path
$ProxyDll = (Resolve-Path $ProxyDll).Path
$FontFile = (Resolve-Path $FontFile).Path
$sandbox = Join-Path $SandboxParent ('yaneurao_proxy_' + [Guid]::NewGuid().ToString('N'))
$temporary = Join-Path $sandbox 'temp'
New-Item -ItemType Directory -Path $temporary -Force | Out-Null
Copy-Item $TestExecutable (Join-Path $sandbox 'FontOverrideTest.exe')
Copy-Item $ProxyDll (Join-Path $sandbox 'd3d9.dll')
Copy-Item $FontFile (Join-Path $sandbox 'MahoroCHS Gothic.ttf')
$log = Join-Path $sandbox 'yaneurao_v21455.log'
$tempLog = Join-Path $temporary 'yaneurao_v21455.log'
$oldTemp = $env:TEMP
$oldTmp = $env:TMP

try {
    $env:TEMP = $temporary
    $env:TMP = $temporary
    & (Join-Path $sandbox 'FontOverrideTest.exe') *> (Join-Path $sandbox 'first_run.txt')
    if ($LASTEXITCODE -ne 0) { throw "Font integration test failed; see $sandbox" }
    $result = Get-Content (Join-Path $sandbox 'first_run.txt') -Raw
    if ($result -notmatch 'RESULT failures=0') { throw 'Missing successful integration result' }

    if ($ExpectLogging) {
        if (-not (Test-Path $log)) { throw 'Diagnostic DLL did not create its log' }
        $contents = Get-Content $log -Raw
        if ($contents -notmatch 'custom_font_match=yes') { throw 'Diagnostic log lacks actual font match evidence' }
        [ordered]@{ status='passed'; logging=$true; sandbox=$sandbox; integration_result=$result } | ConvertTo-Json
    }
    else {
        if ((Test-Path $log) -or (Test-Path $tempLog)) { throw 'No-log DLL created a log file' }
        [IO.File]::WriteAllText($log, "Existing log sentinel: do not append.`r`n", [Text.UTF8Encoding]::new($false))
        $before = (Get-FileHash $log -Algorithm SHA256).Hash
        & (Join-Path $sandbox 'FontOverrideTest.exe') *> (Join-Path $sandbox 'existing_log_run.txt')
        if ($LASTEXITCODE -ne 0) { throw "Existing-log font integration test failed; see $sandbox" }
        if ((Get-FileHash $log -Algorithm SHA256).Hash -ne $before) { throw 'No-log DLL changed an existing log' }
        if (Test-Path $tempLog) { throw 'No-log DLL wrote to the temporary-directory fallback' }
        [ordered]@{ status='passed'; logging=$false; no_log_created=$true; existing_log_unchanged=$true; no_temp_log=$true; sandbox=$sandbox; integration_result=$result } | ConvertTo-Json
    }
}
finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
