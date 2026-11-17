#Requires -Version 7
# T-01: 改造前（base）と改造後（head）の Core DLL で t01.exe を動かし、画素のハッシュを比べる。
#
# 1. それぞれの DLL を別のフォルダに置き、横に MacType.ini と Default.ini を置く。
#    MacType.ini は [General] AlternativeFile=<Default.ini の絶対パス> だけ。
#    MacType 本体は DLL と同じフォルダの MacType.ini を読み（settings.cpp の LoadSettings）、
#    AlternativeFile があればそちらを読み直す（LoadAppSettings）。実際の配置と同じ読み方になる。
# 2. 単スレッド（--repeat）と複数スレッド（--threads）で、それぞれ別のプロセスとして動かす。
#    複数スレッドは、base と head を交互に MtRuns 回ずつ動かす。スレッドの開始時にだけ起きる競合（S-03 など）は、
#    1 回のプロセスにつき 1 度しか機会がないので、プロセスを立ち上げる回数で稼ぐ。
# 3. 比べる:
#    - base と head の単スレッドのハッシュが、全ケースで同じ        → 違えば失敗
#    - head の複数スレッドが 1 回でも「落ちる・固まる・スレッド間でずれる・単スレッドとずれる」→ 失敗
#      base がどうだったかには関係なく失敗にする（S-01 と S-03 を直した後は、base も head も通るはず）
#    - base の複数スレッドだけが失敗 → 警告（比較先のブランチ側の問題。それを直すブランチを失敗にしないため）
# 4. 速度: base と head を交互に数回動かし、中央値を出す（参考値）。複数スレッドの時間も、MtRuns 回の中央値を出す。
#
# 例: pwsh tools/t01/run-t01.ps1 -Tool t01.exe -BaseDll base\MacType64.Core.dll -HeadDll head\MacType64.Core.dll -OutDir out
param(
    [Parameter(Mandatory)] [string] $Tool,
    [Parameter(Mandatory)] [string] $BaseDll,
    [Parameter(Mandatory)] [string] $HeadDll,
    [Parameter(Mandatory)] [string] $OutDir,
    [string] $Ini = (Join-Path $PSScriptRoot 'Default.ini'),
    [int] $Repeat = 5,
    [int] $Threads = 8,
    [int] $Rounds = 3,
    [int] $MtRuns = 5,
    [int] $SpeedRuns = 3,
    [int] $SpeedRepeat = 20,
    [int] $TimeoutSec = 600,
    [string[]] $FontArgs = @()
)

$ErrorActionPreference = 'Stop'
$FontArgs = @($FontArgs | Where-Object { $_ })   # 空の配列が "" 1 つで渡ってくることがあるので取り除く
$OutDir = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Tool = [IO.Path]::GetFullPath($Tool)

$summary = [System.Collections.Generic.List[string]]::new()
$failed = $false
function Note([string] $s) { Write-Host $s; $summary.Add($s) }
function Bad([string] $s) { Write-Host "::error::$s"; $summary.Add("**FAIL**: $s"); $script:failed = $true }
function Warn([string] $s) { Write-Host "::warning::$s"; $summary.Add("**WARNING**: $s") }

$iniHash = (Get-FileHash -Algorithm SHA256 $Ini).Hash.ToLower()
Note "Default.ini: $Ini (sha256 $iniHash)"

# ---- DLL の置き場所を作る
function Prepare([string] $label, [string] $dll) {
    $dir = Join-Path $OutDir "dll-$label"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath $dll -Destination $dir -Force
    $pdb = [IO.Path]::ChangeExtension($dll, '.pdb')
    if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $dir -Force }
    $iniCopy = Join-Path $dir 'Default.ini'
    Copy-Item -LiteralPath $Ini -Destination $iniCopy -Force
    # UTF-16LE（BOM つき）で書く。IniParser は BOM を見て文字コードを決めるので、パスに日本語があっても読める
    $macIni = "[General]`r`nAlternativeFile=$iniCopy`r`n"
    [IO.File]::WriteAllText((Join-Path $dir 'MacType.ini'), $macIni, [Text.Encoding]::Unicode)
    return (Join-Path $dir (Split-Path -Leaf $dll))
}

# ---- t01.exe を 1 回動かす。戻り値は終了コード
function RunTool([string] $name, [string[]] $toolArgs) {
    $log = Join-Path $OutDir "$name.log"
    Write-Host "---- $name"
    Write-Host "> t01.exe $($toolArgs -join ' ')"
    $psi = [Diagnostics.ProcessStartInfo]::new($Tool)
    foreach ($a in $toolArgs) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $p = [Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $p.Kill($true)
        $p.WaitForExit()
        $text = $stdout.Result
        Set-Content -LiteralPath $log -Value $text -Encoding utf8
        Write-Host $text
        Write-Host "TIMEOUT after $TimeoutSec s (hang?)"
        return -1
    }
    $p.WaitForExit()
    $text = $stdout.Result
    Set-Content -LiteralPath $log -Value $text -Encoding utf8
    Write-Host $text
    $code = $p.ExitCode
    Write-Host ("exit code {0} (0x{0:x8})" -f $code)
    return $code
}

function ReadHashes([string] $tsv) {
    $map = [ordered]@{}
    foreach ($line in [IO.File]::ReadAllLines($tsv, [Text.Encoding]::UTF8)) {
        if ($line.StartsWith('#') -or $line -eq '') { continue }
        $f = $line.Split("`t")
        $map[$f[0]] = $f[1]
    }
    return $map
}

function CompareHashes([string] $what, $a, $b, [string] $la, [string] $lb) {
    $diff = @()
    foreach ($k in $a.Keys) {
        if (-not $b.Contains($k)) { $diff += "$k (missing in $lb)"; continue }
        if ($a[$k] -ne $b[$k]) { $diff += "$k ($la $($a[$k]) / $lb $($b[$k]))" }
    }
    foreach ($k in $b.Keys) { if (-not $a.Contains($k)) { $diff += "$k (missing in $la)" } }
    if ($a.Count -eq 0) { $diff += "no cases" }
    return ,$diff
}

$dlls = [ordered]@{ base = (Prepare 'base' $BaseDll); head = (Prepare 'head' $HeadDll) }

# ---- 単スレッド
$st = @{}
foreach ($label in $dlls.Keys) {
    $tsv = Join-Path $OutDir "$label-st.tsv"
    $code = RunTool "$label-st" (@('--dll', $dlls[$label], '--out', $tsv, '--repeat', "$Repeat") + $FontArgs)
    if ($code -ne 0) { Bad ("{0}: single-thread run failed (exit 0x{1:x8}). See {0}-st.log" -f $label, $code) }
    elseif (Test-Path $tsv) { $st[$label] = ReadHashes $tsv }
}

# ---- 複数スレッド（base と head を交互に、MtRuns 回ずつ）
$mtOk = @{ base = 0; head = 0 }
$mtBad = @{ base = @(); head = @() }      # 失敗した回の説明
$mtMs = @{ base = @(); head = @() }
for ($i = 1; $i -le $MtRuns; $i++) {
    foreach ($label in $dlls.Keys) {
        $name = "$label-mt-$i"
        $tsv = Join-Path $OutDir "$name.tsv"
        $code = RunTool $name (@('--dll', $dlls[$label], '--out', $tsv, '--threads', "$Threads", '--rounds', "$Rounds") + $FontArgs)
        $why = $null
        if ($code -eq -1) { $why = "no exit within $TimeoutSec s (hang?)" }
        elseif ($code -eq 1) { $why = 'check failed, for example threads or rounds disagree (exit 1)' }
        elseif ($code -ne 0) { $why = ('crashed (exit 0x{0:x8})' -f $code) }
        elseif (-not (Test-Path $tsv)) { $why = 'no output' }
        elseif ($st.ContainsKey($label)) {
            $d = CompareHashes 'mt' $st[$label] (ReadHashes $tsv) 'single' 'multi'
            if ($d.Count) { $why = "differs from single-thread in $($d.Count) cases: " + (($d | Select-Object -First 3) -join '; ') }
        }
        if ($why) {
            $mtBad[$label] += "run ${i}: $why. See $name.log"
            # 落ちた場所を要約にも残す
            $crash = @(Get-Content (Join-Path $OutDir "$name.log") | Where-Object { $_ -match '^(CRASH|  |# heap corruption)' } | Select-Object -First 24)
            $crash | ForEach-Object { $mtBad[$label] += "    $_" }
        } else {
            $mtOk[$label]++
            $line = Get-Content (Join-Path $OutDir "$name.log") | Where-Object { $_ -match '^# TIME mt_ms=' } | Select-Object -First 1
            if ($line -match 'mt_ms=([\d.]+)') { $mtMs[$label] += [double]$Matches[1] }
        }
    }
}

# ---- 比べる
if ($st.ContainsKey('base') -and $st.ContainsKey('head')) {
    $d = CompareHashes 'st' $st['base'] $st['head'] 'base' 'head'
    if ($d.Count) {
        Bad "base and head render differently in $($d.Count) cases"
        $d | Select-Object -First 30 | ForEach-Object { Note "  - $_" }
    } else {
        Note "base vs head (single thread): all $($st['head'].Count) cases identical"
    }
}
foreach ($label in 'base', 'head') {
    $what = "${label}: multi-thread ($Threads threads x $Rounds rounds): $($mtOk[$label]) of $MtRuns runs OK"
    if ($mtOk[$label] -eq $MtRuns) {
        Note "$what (no crash, all threads agree, same pixels as single-thread)"
    } elseif ($label -eq 'head') {
        # head が 1 回でも失敗したら、base に関係なく失敗
        Bad $what
        $mtBad[$label] | ForEach-Object { $summary.Add("  - $_"); Write-Host "  - $_" }
    } else {
        Warn "$what  (base = the branch compared against. Not caused by this change)"
        $mtBad[$label] | ForEach-Object { $summary.Add("  - $_"); Write-Host "  - $_" }
    }
}
foreach ($label in 'base', 'head') {
    $log = Join-Path $OutDir "$label-st.log"
    if (Test-Path $log) {
        Get-Content $log | Where-Object { $_ -match '^# (ExtTextOutW|IsRunAsUser|dwrite|effect|overall|sweep|font )' } |
            ForEach-Object { Note "${label}: $($_.Substring(2))" }
    }
}

# ---- 速度（参考値。共有ランナーなので揺れる）
$times = @{ base = @(); head = @() }
for ($i = 1; $i -le $SpeedRuns; $i++) {
    foreach ($label in 'base', 'head') {
        $name = "speed-$label-$i"
        # 素の GDI の 1 周（--no-gdi-check を付けない）を先に回し、GDI 側のフォント読み込みを済ませてから測る。
        # こうすると pass 1（cold）は、MacType 自身のキャッシュが空の状態の時間になる
        $code = RunTool $name (@('--dll', $dlls[$label], '--repeat', "$SpeedRepeat") + $FontArgs)
        if ($code -ne 0) { Bad "${name}: failed (exit $code)"; continue }
        $line = Get-Content (Join-Path $OutDir "$name.log") | Where-Object { $_ -match '^# TIME ' } | Select-Object -First 1
        if ($line -match 'cold_ms=([\d.]+) warm_ms_per_pass=([\d.]+)') {
            $times[$label] += [pscustomobject]@{ cold = [double]$Matches[1]; warm = [double]$Matches[2] }
        }
    }
}
function Median([double[]] $v) { if (-not $v.Count) { return [double]::NaN }; $s = $v | Sort-Object; return $s[[int][Math]::Floor(($s.Count - 1) / 2)] }
if ($times['base'].Count -and $times['head'].Count) {
    $bc = Median $times['base'].cold; $hc = Median $times['head'].cold
    $bw = Median $times['base'].warm; $hw = Median $times['head'].warm
    Note ""
    Note "| speed (median of $SpeedRuns runs) | base | head | head/base |"
    Note "|---|---:|---:|---:|"
    Note ("| pass 1, cold cache (ms) | {0:N1} | {1:N1} | {2:N3} |" -f $bc, $hc, ($hc / $bc))
    Note ("| pass 2+, warm, per pass (ms) | {0:N2} | {1:N2} | {2:N3} |" -f $bw, $hw, ($hw / $bw))
}
if ($mtMs['base'].Count -and $mtMs['head'].Count) {
    $bm = Median $mtMs['base']; $hm = Median $mtMs['head']
    Note ""
    Note "| multi-thread, $Threads threads x $Rounds rounds (median of the runs that passed) | base ($($mtMs['base'].Count) runs) | head ($($mtMs['head'].Count) runs) | head/base |"
    Note "|---|---:|---:|---:|"
    Note ("| whole run (ms) | {0:N1} | {1:N1} | {2:N3} |" -f $bm, $hm, ($hm / $bm))
}

Note ""
Note ("result: {0}" -f ($(if ($failed) { 'FAIL' } else { 'OK' })))
$summary | Set-Content -LiteralPath (Join-Path $OutDir 'summary.md') -Encoding utf8
if ($env:GITHUB_STEP_SUMMARY) { $summary | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
if ($failed) { exit 1 }
exit 0
