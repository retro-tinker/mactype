#Requires -Version 7
# S-01 の確認用（使い捨て。test/s01-stress ブランチだけに置く。directwrite にはマージしない）。
# run-t01.ps1 が作った置き場所（<OutDir>\dll-base、<OutDir>\dll-head）の DLL で、
# t01.exe の複数スレッドのモードを、回数とラウンド数を増やして何度も回す。
#
#   A: base と head を同じ条件で（8 スレッド × 3 ラウンド。ふつうの T-01 と同じ）
#   B: A と同じだが、各スレッドの最初の描画だけ 1 本ずつにする（--serial-first-draw。S-03 の場所を重ねない）
#   C: head だけ、B の条件でラウンド数とスレッド数を増やす
#   D: 診断。ページヒープ（解放済みメモリに触った瞬間に止まる）を有効にして、落ちた場所の呼び出し履歴を出す
#
# これは数を数えるためのジョブなので、落ちても終了コードは 0 にする。1 回の実行ごとに 1 行、最後に集計の表を出す。
param(
    [Parameter(Mandatory)] [string] $Tool,
    [Parameter(Mandatory)] [string] $OutDir,
    [int] $SameRuns = 6,
    [int] $LongRounds = 20,
    [int[]] $LongThreads = @(4, 8, 16),
    [int] $PageHeapRuns = 4,
    [int] $TimeoutSec = 1200
)

$ErrorActionPreference = 'Stop'
$OutDir = [IO.Path]::GetFullPath($OutDir)
$Tool = [IO.Path]::GetFullPath($Tool)

function ReadHashes([string] $tsv) {
    $map = [ordered]@{}
    if (-not (Test-Path -LiteralPath $tsv)) { return $map }
    foreach ($line in [IO.File]::ReadAllLines($tsv, [Text.Encoding]::UTF8)) {
        if ($line.StartsWith('#') -or $line -eq '') { continue }
        $f = $line.Split("`t")
        $map[$f[0]] = $f[1]
    }
    return $map
}

$dlls = @{}
$st = @{}
foreach ($label in 'base', 'head') {
    $dll = Get-ChildItem -LiteralPath (Join-Path $OutDir "dll-$label") -Filter '*.Core.dll' | Select-Object -First 1
    if (-not $dll) { Write-Host "::error::no DLL in dll-$label"; exit 1 }
    $dlls[$label] = $dll.FullName
    $st[$label] = ReadHashes (Join-Path $OutDir "$label-st.tsv")
    Write-Host ("{0}: {1} ({2} bytes), single-thread hashes: {3} cases" -f $label, $dll.FullName, $dll.Length, $st[$label].Count)
}

$rows = [System.Collections.Generic.List[object]]::new()

# 1 回動かす。$set は集計の見出し、$exe は使う t01、$extra は追加の引数
function RunMt([string] $set, [string] $exe, [string] $label, [int] $threads, [int] $rounds, [int] $n, [string[]] $extra) {
    $name = "stress-$set-$label-t$threads-r$rounds-$n"
    $tsv = Join-Path $OutDir "$name.tsv"
    $log = Join-Path $OutDir "$name.log"
    $psi = [Diagnostics.ProcessStartInfo]::new($exe)
    foreach ($a in (@('--dll', $dlls[$label], '--out', $tsv, '--threads', "$threads", '--rounds', "$rounds") + $extra)) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) { $p.Kill($true) }
    $p.WaitForExit()
    $sec = $sw.Elapsed.TotalSeconds
    $text = $stdout.Result
    Set-Content -LiteralPath $log -Value $text -Encoding utf8

    $outcome = 'ok'
    $detail = ''
    if ($timedOut) { $outcome = 'timeout'; $detail = "no exit within $TimeoutSec s (hang?)" }
    elseif ($p.ExitCode -eq 1) { $outcome = 'mismatch'; $detail = 'check failed (exit 1)' }
    elseif ($p.ExitCode -ne 0) { $outcome = 'crash'; $detail = ('exit 0x{0:x8}' -f $p.ExitCode) }
    else {
        # 落ちなかったら、単スレッドの画素（同じ DLL）と比べる
        $mt = ReadHashes $tsv
        $ref = $st[$label]
        $diff = 0
        foreach ($k in $ref.Keys) { if (-not $mt.Contains($k) -or $mt[$k] -ne $ref[$k]) { $diff++ } }
        if ($ref.Count -eq 0) { $detail = 'no single-thread reference' }
        elseif ($mt.Count -ne $ref.Count -or $diff) { $outcome = 'mismatch'; $detail = "$diff of $($ref.Count) cases differ from single-thread" }
        else { $detail = "all $($ref.Count) cases same as single-thread" }
    }
    if ($set.StartsWith('D')) { $detail += $(if ($text -match 'NtGlobalFlag 0x[0-9a-f][2367abef][0-9a-f]{6}\b') { ' [page heap on]' } else { ' [page heap NOT on]' }) }
    $mtMs = ''
    if ($text -match 'TIME mt_ms=([\d.]+)') { $mtMs = $Matches[1] }
    Write-Host ("{0,-34} {1,-8} {2,7:N1} s  mt_ms={3,-9} {4}" -f $name, $outcome, $sec, $mtMs, $detail)
    if ($outcome -ne 'ok') {
        $text -split "`r?`n" | Where-Object { $_ -match '^(FAIL|CRASH|  access|  #\d\d )' } | Select-Object -First 16 |
            ForEach-Object { Write-Host "      $_" }
    }
    $rows.Add([pscustomobject]@{ set = $set; label = $label; threads = $threads; rounds = $rounds; outcome = $outcome })
}

Write-Host "==== A: same condition for base and head: 8 threads x 3 rounds, $SameRuns runs each (alternating)"
for ($i = 1; $i -le $SameRuns; $i++) {
    RunMt 'A' $Tool 'base' 8 3 $i @()
    RunMt 'A' $Tool 'head' 8 3 $i @()
}

Write-Host "==== B: same, but the first draw of each thread is serialized (S-03 site not overlapped)"
for ($i = 1; $i -le $SameRuns; $i++) {
    RunMt 'B' $Tool 'base' 8 3 $i @('--serial-first-draw')
    RunMt 'B' $Tool 'head' 8 3 $i @('--serial-first-draw')
}

Write-Host "==== C: head only, first draw serialized, longer runs ($LongRounds rounds)"
foreach ($t in $LongThreads) {
    RunMt 'C' $Tool 'head' $t $LongRounds 1 @('--serial-first-draw')
}

Write-Host "==== D: diagnostics with full page heap (t01ph.exe), head, 8 threads x 1 round, $PageHeapRuns runs"
$TimeoutSec = 600   # ページヒープは遅いので、1 回 10 分で打ち切る
$ph = Join-Path (Split-Path -Parent $Tool) 't01ph.exe'
Copy-Item -LiteralPath $Tool -Destination $ph -Force
foreach ($key in 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\t01ph.exe',
                 'HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\t01ph.exe') {
    & reg.exe add $key /v GlobalFlag /t REG_DWORD /d 0x02000000 /f | Out-Null
    & reg.exe add $key /v PageHeapFlags /t REG_DWORD /d 0x3 /f | Out-Null
}
for ($i = 1; $i -le $PageHeapRuns; $i++) {
    RunMt 'D-pageheap' $ph 'head' 8 1 $i @()
}
RunMt 'D-pageheap-serial' $ph 'head' 8 1 1 @('--serial-first-draw')
RunMt 'D-pageheap' $ph 'base' 8 1 1 @()

# ---- 集計
$summary = [System.Collections.Generic.List[string]]::new()
$summary.Add("| set | dll | threads | rounds | runs | ok | crash | timeout | mismatch |")
$summary.Add("|---|---|---:|---:|---:|---:|---:|---:|---:|")
foreach ($g in ($rows | Group-Object set, label, threads, rounds)) {
    $r = $g.Group
    $summary.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |" -f $r[0].set, $r[0].label, $r[0].threads, $r[0].rounds, $r.Count,
        @($r | Where-Object outcome -eq 'ok').Count, @($r | Where-Object outcome -eq 'crash').Count,
        @($r | Where-Object outcome -eq 'timeout').Count, @($r | Where-Object outcome -eq 'mismatch').Count))
}
$summary | ForEach-Object { Write-Host "STRESS $_" }
$summary | Set-Content -LiteralPath (Join-Path $OutDir 'stress-summary.md') -Encoding utf8
if ($env:GITHUB_STEP_SUMMARY) { $summary | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
exit 0
