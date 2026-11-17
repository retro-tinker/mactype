# CLAUDE.md

retro-tinker/mactype（snowie2000/mactype のフォーク）で作業するときの決まり。

## このリポジトリについて

- MacType は、Windows の文字描画を FreeType に置き換えるソフト。各プロセスに DLL を読み込ませ、GDI / DirectWrite / Direct2D の API をフックする。
- 改造の目的は「安定性の向上」と「高速化」の 2 つだけ。
- 基準ブランチは `directwrite`（upstream の `directwrite` に build.yml を足したもの）。
- ここでビルドされるのは本体の `MacType.Core.dll` / `MacType64.Core.dll`（`gdipp.vcxproj`）と `macloader`（`run.cpp`）。ローダー（`MacType64.dll`）と Tuner は非公開で、ソースはない。
- 調査結果と修正候補の一覧は `docs/kaizo-plan.md`。作業はこの文書の ID（S-01、P-01 など）を単位に進める。
- 優先度は、けんじさんが使っている設定で通る経路を先にする。通る経路・通らない経路は `docs/kaizo-plan.md` の 1.7。

## 作業ルール

1. **1 ブランチ 1 修正。** `docs/kaizo-plan.md` の ID 1 つ（枝番があれば枝番 1 つ）だけを直す。ついでの修正や整理は入れない。気づいたことは計画書に書き足す。
2. **差分は最小限。** 関係ない行の整形、改名、コメント修正、改行コードの変更をしない。
3. **描画結果は変えない。** 画面に出る画素が 1 つでも変わりうる変更は入れない。必要だと思ったら、手を付ける前にけんじさんに相談する。変わらないと言える理由は、コミットメッセージか計画書に書く。
4. **Actions でビルドが通るまでを 1 単位にする。** push して `Build MacType` が Win32 / x64 とも成功するまでは「終わった」と言わない。失敗したら同じブランチで直す。

## 進め方

- ブランチは `directwrite` から切る。名前は `fix/s03-tls-array-lock`、`perf/p01-conv2-shift` のように、ID と内容を入れる。
- コミットメッセージの先頭に ID を書く（例: `P-01: conv2 の割り算をシフトに置き換え`）。
- 直したら `docs/kaizo-plan.md` の該当行に、状態（ブランチ名、ビルド結果）を書き足す。行番号は `693ca92` 時点のものなので、コードを変えたらずれる。根拠を引くときは現在の行を確かめる。
- 「確認済み」と「推測」を分けて報告する。実機で確かめていないことを、確かめたように書かない。
- 計画書にない問題を見つけたら、直す前に ID を付けて計画書に足す。

## ビルド

- ローカルに Visual Studio はない。ビルドは GitHub Actions（`.github/workflows/build.yml`）だけ。
- どのブランチでも push で走る。ただし `**.md` と `doc/**` だけの変更では走らない。手動実行（workflow_dispatch）もできる。
- 構成は `Rel+Detours` の Win32 と x64。成果物は実行結果ページの Artifacts（`MacType-<コミットSHA>`）。
- 依存ライブラリ（snowie2000/freetype、snowie2000/IniParser、snowie2000/rewolf-wow64ext、microsoft/Detours）は、毎回それぞれの最新をソースからビルドする。こちらが何も変えていなくても、依存側の更新でビルド結果が変わりうる。
- 結果は `gh run list --branch <ブランチ>` と `gh run view <ID> --log-failed` で確かめる。

## コードを触るときの注意

- **使われる構成は Detours 版。** `USE_DETOURS` が定義される。`hook.cpp` などの `#ifdef USE_DETOURS` の側を読む。EasyHook 側（`#else`）はビルドされない。
- **フック関数の中から、フック対象の API をそのまま呼ばない。** Detours 版には再入防止がない。元の API を呼ぶときは `ORIG_xxx` を使う。
- **Win32 と x64 で違う点がある。** Win32 は `/Zc:threadSafeInit-`（関数内 static の初期化がスレッド安全でない）。x64 は有効。両方で正しく動く書き方にする。
- **対応 OS は Vista 以降。** SRW ロック、`InitOnceExecuteOnce`、`LdrRegisterDllNotification` など、Vista 以降の API は使ってよい（けんじさんの決定、2026-11-17）。
- **例外は `/EHsc`。** `catch(...)` はアクセス違反を捕まえない。フック関数から C++ 例外を外へ漏らさない。
- **`DllMain` の中（ローダーロック中）に処理を足さない。** `LoadLibrary`、スレッド作成、待ち合わせは特に避ける。
- **ロックを足すときは、取得順を `docs/kaizo-plan.md` の 1.6 と照らす。** 逆順の組を作らない。描画のたびに通る場所では、ロックの回数を増やさない工夫を先に考える。
- **外部との約束を変えない。** エクスポート（`expfunc.def`）、`IControlCenter` の仮想関数の並び（`settings.h`）、`MacType.ini` の項目名と意味。非公開のローダーや Tuner が使っている可能性がある。
- **改行コードはファイルごとに違う。** CRLF のファイルと LF のファイルがあり、`settings.h` は 1 つのファイルの中で混ざっている。編集した行の前後に合わせ、ファイル全体の改行を変えない。コミット前に `git diff --stat` で、行数が想定どおりか確かめる。
- **文字コードは BOM なしの UTF-8。** 文字化けしたコメントが多いが、直さない（差分最小）。
- **使われていないコードは触らない。** `ft - non-ref.cpp`、`optimize/`、未設置のフックなど（計画書の 5 章）。

## upstream に出すとき

- Issue や PR は、けんじさんが出すと決めたものだけ。勝手に出さない。
- PR 用のブランチは upstream の `directwrite` から切り、修正のコミットだけを載せる（build.yml、CLAUDE.md、docs/ を含めない）。
