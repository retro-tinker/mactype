#Requires -Version 7
# T-01: 改造前（base）と改造後（head）の Core DLL で t01.exe を動かし、画素のハッシュを比べる。
#
# 1. それぞれの DLL を別のフォルダに置き、横に MacType.ini と Default.ini を置く。
#    MacType.ini は [General] AlternativeFile=<Default.ini の絶対パス> だけ。
#    MacType 本体は DLL と同じフォルダの MacType.ini を読み（settings.cpp の LoadSettings）、
#    AlternativeFile があればそちらを読み直す（LoadAppSettings）。実際の配置と同じ読み方になる。
# 2. 単スレッド（--repeat）と複数スレッド（--threads）で、それぞれ別のプロセスとして動かす。
# 3. 比べる:
#    - base と head の単スレッドのハッシュが、全ケースで同じ        → 違えば失敗
#    - head の複数スレッドのハッシュが、head の単スレッドと同じ      → 違えば失敗
#    - base の複数スレッド（改造前のコード）が落ちた・ずれた場合は、警告として残す（S-01 などの再現）
# 4. 速度: base と head を交互に数回動かし、中央値を出す（参考値）。
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

# ---- 単スレッド・複数スレッド
$st = @{}; $mt = @{}
foreach ($label in $dlls.Keys) {
    $dll = $dlls[$label]
    $tsv = Join-Path $OutDir "$label-st.tsv"
    $code = RunTool "$label-st" (@('--dll', $dll, '--out', $tsv, '--repeat', "$Repeat") + $FontArgs)
    if ($code -ne 0) { Bad ("{0}: single-thread run failed (exit 0x{1:x8}). See {0}-st.log" -f $label, $code) }
    elseif (Test-Path $tsv) { $st[$label] = ReadHashes $tsv }

    $tsv = Join-Path $OutDir "$label-mt.tsv"
    $code = RunTool "$label-mt" (@('--dll', $dll, '--out', $tsv, '--threads', "$Threads", '--rounds', "$Rounds") + $FontArgs)
    if ($code -ne 0) {
        $msg = ("{0}: multi-thread run failed (exit 0x{1:x8}). See {0}-mt.log" -f $label, $code)
        if ($label -eq 'base') { Warn "$msg  (pre-change code; kept as a warning)" } else { Bad $msg }
    } elseif (Test-Path $tsv) { $mt[$label] = ReadHashes $tsv }
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
    if ($st.ContainsKey($label) -and $mt.ContainsKey($label)) {
        $d = CompareHashes 'mt' $st[$label] $mt[$label] 'single' 'multi'
        if ($d.Count) {
            $msg = "${label}: multi-thread result differs from single-thread in $($d.Count) cases"
            if ($label -eq 'base') { Warn $msg } else { Bad $msg }
            $d | Select-Object -First 10 | ForEach-Object { Note "  - $_" }
        } else {
            Note "${label}: multi-thread ($Threads threads x $Rounds rounds) matches single-thread"
        }
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
        $code = RunTool $name (@('--dll', $dlls[$label], '--repeat', "$SpeedRepeat", '--no-gdi-check') + $FontArgs)
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

Note ""
Note ("result: {0}" -f ($(if ($failed) { 'FAIL' } else { 'OK' })))
$summary | Set-Content -LiteralPath (Join-Path $OutDir 'summary.md') -Encoding utf8
if ($env:GITHUB_STEP_SUMMARY) { $summary | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 }
if ($failed) { exit 1 }
exit 0
