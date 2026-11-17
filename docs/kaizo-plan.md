# MacType 改造計画（調査結果）

目的は「安定性の向上」と「高速化」。この文書は、コードを変更する前の調査結果をまとめたもの。

| 項目 | 内容 |
|---|---|
| 対象 | retro-tinker/mactype の `directwrite` ブランチ、コミット `693ca92`（upstream snowie2000/mactype `directwrite` の `05052e8` に build.yml を足しただけの状態） |
| 調査日 | 2026-11-16（2026-11-17 に更新。別セッションの検証 `docs/kaizo-plan-review.md` と、けんじさんの回答を反映した。対応の一覧は 8 章） |
| 前提にする設定 | けんじさんの `Default.ini`。DirectWrite=1、HintingMode=1、AntiAliasMode=4、FontLoader=0、FontLink=2、FontSubstitutes=0 など。この設定で通る経路・通らない経路は 1.7 |
| 対応 OS | Vista 以降の API を使ってよい（けんじさんの回答） |
| 調査方法 | ソースを読む静的調査のみ。Windows 実機での実行・計測はしていない |
| 行番号 | すべて上記コミット時点のもの。コードを変えたらずれる |
| ビルド構成 | Actions が作る `Rel+Detours`（`USE_DETOURS` 定義）の Win32 / x64 を前提に読んだ |
| 依存ライブラリ | 挙動の確認に使ったのは microsoft/Detours `1dbd4ea`、snowie2000/freetype `ef77157`（どちらも調査日の最新。Actions は毎回最新を取るので、将来は変わりうる） |

**「確認済み」と「推測」の区別**

- **確認済み**: このリポジトリや依存ライブラリ（microsoft/Detours、snowie2000/freetype）のソースを読んで確かめたこと。再現コードで確かめたものはそう書く。
- **推測**: 実機で確かめていない見込み。特に「どれくらい頻繁に起きるか」「どれくらい速くなるか」は、ほぼすべて推測。

**評価の意味**

- 影響: 安定性は「起きたときの重さ × 起きやすさ」、高速化は「体感への効き」の見込み。大 / 中 / 小。
- 難しさ: 修正そのものの難しさ（差分の大きさ、設計の要否、描画を変えない保証のしやすさ）。低 / 中 / 高。
- 確信度: 「その問題がコード上に実在する」ことへの確信。高 / 中 / 低。効果の大きさへの確信ではない。

---

## 0. 要約

1. **一番大きい安定性リスクは、スレッド間の排他が足りないこと。** 描画フックは「所有者つきロック」で守られているように見えるが、このロックは描画スレッド同士を排他しない（1.6 参照）。その内側で、グリフキャッシュ（`std::map`）など複数の共有データがロックなしで読み書きされている（S-01, S-03〜S-06）。複数スレッドが同時に文字を描くプロセスで、まれにクラッシュしうる。
2. **グリフキャッシュ（S-01）は、間引きのときだけでなく、2 つのスレッドが同じ文字を同時に初めて描いたときにも壊れうる**（レビューの指摘。コードと再現コードで確かめた）。新しい文字が出るたびに機会があるので、着手順を前に出した（4 章）。
3. **所有者つきロック自体にも穴がある**。条件が重なると「描画」と「フォント削除」が同時に内側へ入れる（S-02。再現コードあり）。描画が途切れないと、フォント削除や設定反映がいつまでも入れない（S-22。再現コードあり）。
4. **速度面で一番効きそうなのは、アルファブレンドの画素ごとの割り算**（P-01）。シフトに置き換えても結果が同じになることを全域で確認済み。Linux/g++ での再現ベンチでは、このループが約 4.5 倍速くなった（MSVC での実測ではない）。けんじさんの設定（AntiAliasMode=4）は、この LCD 合成を通る。**→ 実装済み（2026-11-17）。T-01 で `ExtTextOutW` 全体を測ると、差は揺れ（±6%）の内側で、見えなかった。影響は「小」に下げた（3.2）。**
5. **次に効きそうなのは、空白 1 文字ごとに GDI の `ExtTextOutW` を 1 回呼んでいる点**（P-02）と、グリフキャッシュの間引きが「最近使った順」ではなく「キーの小さい順」になっている点（P-08、日本語で効く）。
6. **プロセス起動時の重さ**もある。アンロード対象のプロセスでも走る約 3.7 万回の `iswcntrl`（P-10）、D2D を使わないプロセスでも `d2d1.dll` / `dwrite.dll` を読み込む処理（P-11 / S-13）。DirectWrite=1 なので、P-11 はけんじさんの設定で実際に通る。
7. **DirectWrite=1 かつ HintingMode=1 で入る「変換行列への微小な傾き」は、高速化候補にしない**（P-19。見送り）。MacType 側の処理は COM 呼び出し 3 回だけで軽く、外すと描画が変わりうるため。
8. **FontSubstitutes=0 なので、置換フォントまわり（S-07、S-08、S-21、P-14）は、けんじさんの設定では通らない。** 後回しにした（1.7、4 章）。
9. 最初は T-01（描画を機械的に比べる仕組み）。コードの修正の 1 本目は、小さくて結果が同じと証明できる P-01 がおすすめ（4 章）。

---

## 1. 全体構造

### 1.1 ビルドされるもの

| 成果物 | プロジェクト | 中身 |
|---|---|---|
| `MacType.Core.dll`（Win32）/ `MacType64.Core.dll`（x64） | `gdipp.vcxproj` | 本体。各プロセスに読み込まれてフックを張る |
| `macloader.exe` / `macloader64.exe` | `macloader.vcxproj`（`run.cpp` だけ） | 指定したアプリを MacType 付きで起動するランチャー。`MacType.dll` / `MacType64.dll`（非公開のローダー）を読み込ませる（`run.cpp:66-71, 280`） |

- 本体に入るソース: `gdipp.vcxproj:818-842`（`cache.cpp` `common.cpp` `directwrite.cpp` `dll.cpp` `dynCodeHelper.cpp` `EventLogging.cpp` `expfunc.cpp` `ft.cpp` `ft2vert.c` `fteng.cpp` `ftref.c` `gdiPlusFlat2.cpp` `hash_list.cpp` `hook.cpp` `hookCounter.cpp` `misc.cpp` `override.cpp` `ownedcs.cpp` `settings.cpp`）。
- ビルドに入らないもの: `ft - non-ref.cpp`、`optimize/`（古い memcpy 最適化。`common.h:58` でコメントアウト済み）、`Makefile*`。
- エクスポート: `GetMsgProc @1`、`DllGetVersion @2`、`CreateControlCenter @4`、`ReloadConfig @5`（`expfunc.def`）。
- 依存ライブラリ（すべて静的リンク）: snowie2000/freetype（2.14.3 ベースの改造版）、IniParser、wow64ext（Win32 のみ）、Detours。

`Rel+Detours` のコンパイル設定（確認済み）:

| 設定 | Win32 | x64 | 根拠 |
|---|---|---|---|
| 例外 | `/EHsc`（C++ 例外のみ。アクセス違反などは捕まえない） | 同じ | `gdipp.vcxproj:498, 681` |
| `/GS`（スタック破壊検出） | 無効 | 無効 | `gdipp.vcxproj:500, 683` |
| 関数内 static のスレッド安全な初期化 | **無効**（`/Zc:threadSafeInit-`） | 有効 | `gdipp.vcxproj:509, 691` |
| CET 互換 | なし | あり | `gdipp.vcxproj:706` |

Win32 と x64 で「関数内 static の初期化がスレッド安全かどうか」が違う点は、S-09 に関係する。

### 1.2 DllMain → 初期化 → フック設置

**(0) DllMain より前（CRT の静的初期化。ローダーロック中）**

- `undocAPI.h:72, 91-96` の `static` 変数。`LoadLibrary("gdi32.dll")` と `GetProcAddress` を、このヘッダーを取り込む .cpp ごとに実行する（P-13）。
- `ControlIder CID`（`ft.cpp:62`、中身は `ft.h:146-165`）。`iswcntrl` を約 3.7 万回呼んで文字種の表を作る（P-10）。
- `s_AlphaBlendTable`（`ft.cpp:219`）、`IsUWP`（`hook.cpp:277`）、`bIsOS64`（`expfunc.cpp:1262`）。

**(1) `DllMain` の `DLL_PROCESS_ATTACH`（`hook.cpp:494-627`。ローダーロック中）**

| 順 | 処理 | 根拠 |
|---|---|---|
| 1 | ロック 20 個＋所有者つきロック 2 個、TLS を初期化 | `hook.cpp:540-546`、`common.h:145-149, 295-301` |
| 2 | 設定オブジェクトを作り `MacType.ini` を読む | `hook.cpp:558-559` → `settings.cpp:308-318, 506-792` |
| 3 | UnloadDLL 対象か判定。対象ならフックせず、ミューテックスを見て残るか抜けるか決める | `hook.cpp:563, 607-624` |
| 4 | `hook_initinternal`: `ORIG_xxx` に本物の API アドレスを入れる | `hook.cpp:567, 74-81` |
| 5 | ExcludeModule 対象でなければ FreeType を初期化（`FT_Init_FreeType`、キャッシュマネージャ作成） | `hook.cpp:569-576` → `ft.cpp:3435-3477` |
| 6 | `g_pFTEngine` を生成 | `hook.cpp:577` |
| 7 | `hook_init`: Detours のトランザクション 1 回で、常設フックをまとめて設置 | `hook.cpp:584, 86-107` |
| 8 | 対話ユーザーのプロセス かつ INI の `DirectWrite` が有効（既定は 0、`settings.cpp:631`）なら `HookD2DDll`（`d2d1.dll` / `dwrite.dll` を読み込み、4 つの入口関数をフック） | `hook.cpp:595-599` → `directwrite.cpp:1482-1527` |
| 9 | FontSubstitutes が有効なら `CreateFontIndirectW/ExW` をフック | `hook.cpp:601-603, 434-451` |

**(2) 遅延初期化 `DelayedInit`（`settings.cpp:156-306`）**

`g_pFTEngine` ができた後、最初に `CGdippSettings::GetInstance()` が呼ばれたスレッドで実行される（`settings.cpp:136-146`）。`CS_SETTING` を握ったまま、DPI 取得、ガンマ表作成、FontSubstitutes / Individual / Exclude / Include の解釈（フォント列挙あり）、FontLink のレジストリ読み込み、LCD フィルタ設定を行う。

**(3) スレッド終了時**: `DLL_THREAD_DETACH` でそのスレッドの TLS データを破棄（`hook.cpp:633-634` → `tlsdata.h:69-79`）。

**(4) `DLL_PROCESS_DETACH`（`hook.cpp:639-681`）**: プロセス終了時（`lpReserved != NULL`）は何もせず戻る（`hook.cpp:653`）。動的アンロード時だけ `hook_term`（フック解除 → フック内にいるスレッドを待つ）→ エンジン破棄 → FreeType 終了 → 設定破棄 → ロック破棄。

### 1.3 フック一覧

**仕組み（確認済み）**: `hooklist.h` の 1 行が 1 フック。`HOOK_DEFINE` は起動時に必ず設置、`HOOK_MANUALLY` は必要になったときに `hook_demand_xxx()` で設置。Detours 版では、各フックは `REF_xxx`（呼び出し中カウンタ `HCounter` を増減するだけの包み）→ `IMPL_xxx`（本体）→ `ORIG_xxx`（元の API）の順に呼ばれる（`hook.cpp:59-66`）。

**A. 起動時に必ず入る GDI フック（`hook_init`）**

| API | `hooklist.h` | 本体（`override.cpp`） | 役割 |
|---|---|---|---|
| `ExtTextOutW` | 66 | 1123 | **描画の本体**。FreeType で描いて DC に転送 |
| `ExtTextOutA` / `TextOutA` / `TextOutW` | 63 / 60 / 61 | 889 / 856 / 864 | `ExtTextOutW` 本体へ回す |
| `GetGlyphOutlineW/A` | 27 / 35 | 473 / 516 | ClipBoxFix（グリフの外接箱を少し広げて返す） |
| `GetObjectW/A` | 1 / 2 | 675 / 656 | 置換したフォントの名前を元の名前に見せる |
| `GetTextFaceW/A`、`GetTextFaceAliasW` | 5 / 6 / 3 | 560 / 579 / 437 | 同上 |
| `GetFontData` | 75 | 1725 | 置換フォントの `name` テーブルだけ元フォントのものを返す |
| `DeleteObject` | 4 | 760 | 置換フォントの記録を消す。代替 GUI フォントの削除を防ぐ |
| `GetStockObject` | 71 | 597 | `DEFAULT_GUI_FONT` を代替フォントに差し替え |
| `BeginPath` / `EndPath` / `AbortPath` | 72 / 73 / 74 | 623 / 634 / 645 | パス作成中の DC を覚えて、その間は描画に手を出さない |
| `RemoveFontResourceExW` | 69 | 843 | 削除されるフォントをエンジンとキャッシュから外す |

**B. 条件つきで入るフック**

| 対象 | 設置場所 | 条件 |
|---|---|---|
| `CreateFontIndirectW` / `CreateFontIndirectExW`（本体 `override.cpp:771, 689`） | `hook.cpp:434-451` | FontSubstitutes 有効時のみ、DllMain 内で |
| `DWriteCreateFactory`、`D2D1CreateFactory`、`D2D1CreateDevice`、`D2D1CreateDeviceContext` | `directwrite.cpp:1511-1526` | 対話ユーザーのプロセス かつ DirectWrite 設定有効、DllMain 内で |
| COM メソッド（下表） | `directwrite.cpp` の `HOOK` マクロ（28-35） | 上の入口関数が呼ばれ、実物のオブジェクトが手に入ったとき |

COM メソッドのフック（vtable の番号で関数アドレスを取り、関数本体を Detours でフック）:

| インターフェース | メソッド（vtable 番号） | 設置箇所 |
|---|---|---|
| `IDWriteFactory` | `CreateGlyphRunAnalysis`(23)、`GetGdiInterop`(17)、`CreateTextFormat`(15) | `directwrite.cpp:1381-1382, 1352` |
| `IDWriteFactory2` / `3` | `CreateGlyphRunAnalysis`(30) / (31) | `directwrite.cpp:1392, 1398` |
| `IDWriteGlyphRunAnalysis` | `GetAlphaBlendParams`(5) | `directwrite.cpp:625, 797, 881` |
| `IDWriteGdiInterop` | `CreateBitmapRenderTarget`(7) | `directwrite.cpp:640` |
| `IDWriteBitmapRenderTarget` | `DrawGlyphRun`(3) | `directwrite.cpp:665` |
| `IDWriteFont` / `IDWriteFont3` | `CreateFontFace`(13) / (19) | `directwrite.cpp:1363, 1366` |
| `ID2D1Factory` | `CreateWicBitmapRenderTarget`(13)、`CreateHwndRenderTarget`(14)、`CreateDCRenderTarget`(16) | `directwrite.cpp:293-296` |
| `ID2D1Factory1`〜`7` | `CreateDevice`(17, 27〜32) | `directwrite.cpp:302-344` |
| `ID2D1Device`〜`Device6` | `CreateDeviceContext`(4, 11, 12, 15, 16, 19, 20) | `directwrite.cpp:354-390` |
| `ID2D1RenderTarget` 系（3 系統） | `CreateCompatibleRenderTarget`(12)、`DrawText`(27)、`DrawTextLayout`(28)、`DrawGlyphRun`(29)、`SetTextAntialiasMode`(34)、`SetTextRenderingParams`(36)、`DrawGlyphRun`(82) | `directwrite.cpp:416-417, 442-522` |

DirectWrite / Direct2D 側は自前で描いていない。レンダリングパラメータ（ガンマ、コントラスト、ClearType レベル、レンダリングモード、アンチエイリアス、グリッドフィット）を差し替えるだけ（`directwrite.cpp:196-281, 525-528`）。HintingMode=1 のときはグリッドフィット無効になり（`directwrite.cpp:227`）、Direct2D の描画フックが、描画のたびに変換行列へ微小な傾き（1/0xFFFF）を足して元に戻す（`directwrite.cpp:1697-2107`）。詳しくは P-19。

**C. 定義はあるが設置されないフック（確認済み。`hook_demand_xxx` を呼ぶ箇所がない）**

- `CreateCompatibleDC` / `DeleteDC` / `GetDC` / `ReleaseDC`（`hooklist.h:8-11`、本体 `override.cpp:1834-1892`）。DisplayAffinity 用の未完成部分。
- `MySetProcessMitigationPolicy`（`hooklist.h:352`）、`CreateDxgiSurfaceRenderTarget`（`directwrite.cpp:295` でコメントアウト。05052e8 の WeCom クラッシュ対策）、`DWriteFontFaceReference_CreateFontFace*`（`directwrite.cpp:1599-1611` でコメントアウト）。

### 1.4 描画の流れ（GDI の `ExtTextOutW`）

`IMPL_ExtTextOutW`（`override.cpp:1123-1693`）:

1. 素通しの判定（`1128-1154`）: 引数が無効、描画オフ、8192 文字超、空白 1 文字、パス作成中の DC、複雑スクリプト（アラビア文字など）、DisplayAffinity の対象外。
2. 文字ごとの送り幅を GDI に問い合わせる（`1156-1174`。`GetTextExtentExPointW/I`）。
3. 再入チェック（`1196-1198`）、座標変換の確認（`1200-1219`。回転などは素通し、拡大縮小だけ対応）。
4. 所有者つきロック `OCS_DC` を owner=1 で取得（`1252`）。
5. 作業配列の確保（`1262-1265`）、DC・フォントの検査（`1284-1319`。`GetOutlineTextMetrics` 2 回、サイズ制限、除外フォント、TrueType/OpenType 以外は除外）。
6. スレッド専用の作業用メモリ DC とフォント情報の準備（`1322-1423`）。
7. `FreeTypeGetGlyph`（`ft.cpp:3037-3064`）: `OCS_FREETYPE` を owner=1 で取得 → `FreeTypePrepare`（`ft.cpp:1401-1654`。フォント検索、リンクフォント、サイズ別キャッシュ取得、ヒンティング・AA モード決定）→ `ForEachGetGlyphFT`（`ft.cpp:1753-2270`。文字ごとにキャッシュを引き、なければ FreeType でグリフを取り出して太字化・斜体化・ラスタライズし、キャッシュに入れる）。
8. 描画範囲の計算（`override.cpp:1470-1556`）、作業用 DIB の準備（`1534`）、背景の用意（`1558-1606`。不透明なら塗りつぶし、透明なら描画先から `BitBlt` で写し取る）。
9. `FreeTypeTextOut`（`ft.cpp:2823-3035`）: グリフごとに `TextOutCallback`（`ft.cpp:2695-2748`）→ ピクセル形式別の合成（`ft.cpp:537-1048`。LCD / グレー / モノクロ / カラー絵文字）。下線・取り消し線も自前で引く。
10. 作業用 DIB を描画先へ `BitBlt`（`override.cpp:1634-1643`）、後始末（`1654-1685`）。途中で失敗したら元の `ExtTextOutW` に任せる（`1690`）。

フォントファイルの中身は、GDI の `GetFontData` で取り出して FreeType に渡している（`fteng.cpp:837-1070`）。UseMapping 設定が有効なら、フォント全体をメモリにコピーして持つ（`fteng.cpp:930-942`）。

### 1.5 キャッシュ

| キャッシュ | 場所 | キー → 値 | 上限・破棄 | ロック |
|---|---|---|---|---|
| FreeType のキャッシュマネージャ（フェイス、サイズ、文字→グリフ番号、グリフ輪郭） | `ft.cpp:3454-3471` | FreeType 内部 | 既定はフェイス 64（これが下限）、サイズ 1200、10MB（`settings.cpp:646-649`）。けんじさんの設定は 256 / 6140 / 約 20MB | `CS_MANAGER` |
| フォント表 `m_mfontMap` / `m_mfullMap` / `m_mfontList` | `fteng.h:531-540` | (名前, 太さ, 斜体) / 一意なフルネーム / フェイス ID → `FreeTypeFontInfo` | `RemoveFont` 以外では減らない | `CS_FONTMAP`、`CS_FONTENG` |
| サイズ別キャッシュ表 `FreeTypeFontInfo::m_cache` | `fteng.h:315-316, 372`、`fteng.cpp:441-478` | (px, 幅, 太さ, 斜体) のハッシュ → `FreeTypeFontCache` | CacheMaxSizes 超でキーの小さい順に破棄（`fteng.cpp:373-381`） | `CS_FONTCACHE` |
| **グリフキャッシュ `FreeTypeFontCache::m_GlyphCache`** | `fteng.h:204, 218, 223-240` | 文字コード、または −(グリフ番号) → `FreeTypeCharData`（描画済みビットマップへの参照、GDI 幅、AA モード） | 2048 回追加するごとに 1024 個まで間引く。**キーの小さい順に捨てる**（`fteng.cpp:16-18, 123-136, 254-264, 285-287`） | 間引きだけ `CS_FONTCACHE`（`fteng.cpp:126`）。**検索と追加は取らない**ので、守れていない（S-01） |
| 参照カウントつきグリフ | `ftref.h`、`ftref.c` | — | 参照が 0 で解放 | カウンタは Interlocked |
| 作業用メモリ DC と DIB（スレッドごと） | `cache.h:66-151`、`cache.cpp:7-62` | — | 256 回ごとに縮小判定（`cache.cpp:4, 22-33`） | TLS なので不要（例外: S-17） |
| リンクフォント配列 | `fteng.h:318-321, 482-497`、`fteng.cpp:383-413` | フォント → フェイス ID の並び | 設定再読み込みで作り直し | `CS_FONTLINK` |
| 置換フォント記録 `FontCache` | `override.cpp:46, 399-435` | `HFONT` → (元の名前, 置換後の名前) | `DeleteObject` で削除 | 書き込みだけ `CS_CACHEDFONT`（S-07） |
| フォント名キャッシュ `FontNameCache` | `settings.cpp:88`、`hash_list.cpp` | 名前 → ローカライズ名 | 減らない | **なし**（S-06） |
| 除外フォント集合など設定の集合 | `settings.h:331-341` | — | 再読み込みで作り直し | **なし**（S-05, S-11） |
| DirectWrite 用パラメータ | `directwrite.cpp:237-281` | — | 一度作ったら更新されない（設定を再読み込みしても変わらない） | 関数内 static |

### 1.6 スレッドとロック

**スレッド**: フックは、アプリのどのスレッドからでも呼ばれる。MacType が自分で作るスレッドは、`CreateMessageWnd` が呼ばれたときのメッセージ窓スレッドだけ（`settings.h:1013-1050`）。スレッドごとのデータは TLS（`tlsdata.h`、`override.h:60-119`）に置き、作業用ビットマップと再入フラグを持つ。

**普通のロック**（`CRITICAL_SECTION` の配列。定義は `common.h:118-135`）:

| 名前 | 守っているもの | 取得箇所 |
|---|---|---|
| `CS_LIBRARY`（既定） | ラスタライズ（`FT_Glyph_To_Bitmap`）、グリフ参照のコピー、FreeType の初期化・終了、設定反映 | `ft.cpp:1865, 2143, 2185, 2411, 2565, 2600, 3436, 3480`、`fteng.h:623` |
| `CS_MANAGER` | FreeType キャッシュマネージャの呼び出し全般 | `ft.h:362`、`fteng.h:339, 354`、`fteng.cpp:421, 551`、`ft.cpp:1908, 2123` |
| `CS_FONTMAP` | フォント表の検索・削除 | `fteng.cpp:495, 527, 761, 784, 810` |
| `CS_FONTENG` | フォントの追加 | `fteng.cpp:484, 566, 599, 674` |
| `CS_FONTCACHE` | サイズ別キャッシュ表、間引き処理 | `fteng.cpp:126, 146, 443`、`fteng.h:517` |
| `CS_FONTLINK` | リンクフォント配列の作成 | `fteng.h:484, 492` |
| `CS_SETTING` | 設定インスタンスの取得・生成・読み込み | `settings.cpp:112, 128, 138, 150, 310` |
| `CS_CACHEDFONT` | 置換フォント記録の書き込み | `override.cpp:420, 428` |
| `CS_DWRITE` | レンダーターゲットのメソッドフック設置 | `directwrite.cpp:484, 499, 514` |
| `CS_VIRTMEM` | 動的コード生成ポリシーの取得 | `dynCodeHelper.cpp:36` |
| `CS_DCRELATION` | DC とウィンドウの対応表 | `override.cpp:1091`（ほかは未設置フック内） |
| `CS_MAIN`、`CS_CREATEFONT`、`CS_OWNEDCS` | 未使用 | — |

**所有者つきロック**（`ownedcs.cpp`、`common.h:272-309`）: 「スレッド」ではなく「番号（owner）」で排他する独自ロック。動きは次のとおり（確認済み。初版の「読み手/書き手ロックに近い」は不正確だったので、レビュー 1.1 の指摘で書き直した）。

- すでに誰かが持っていて、その owner が自分と同じなら、待たずに合流する（`ownedcs.cpp:27-30`）。何スレッドでも合流できる。
- それ以外（空いている、または別の owner が持っている）は、順番待ちに入る（`ownedcs.cpp:34-37`）。待ちを起こすイベントは自動リセット（`ownedcs.cpp:13`）で、解放 1 回につき 1 スレッドしか起きない（`ownedcs.cpp:54-55`）。だから、**いったん待ちに入った同じ owner どうしは、1 本ずつしか入れない**。「読み手は全員同時に入れる」読み手/書き手ロックとは違う。
- 後から来た同じ owner は、待っているスレッドを追い越して合流する。描画（owner=1）が途切れずに重なると、owner=2 はいつまでも入れない（S-22）。
- owner=2 どうしも、番号が同じなので合流できる。実際には、フォント削除は続けて `CS_MANAGER`（`fteng.cpp:551`）を、設定反映は `CS_LIBRARY`（`fteng.h:623`）を取るので、それぞれ直列になる。
- `OCS_DC` と `OCS_FREETYPE` は別々のロック。フォント削除（`OCS_DC` の owner=2）と設定反映（`OCS_FREETYPE` の owner=2）は、互いに排他されない（S-20）。

| ロック | owner=1 | owner=2 |
|---|---|---|
| `OCS_DC` | `ExtTextOutW` の描画全体（`override.cpp:1252`） | フォント削除（`fteng.cpp:550`） |
| `OCS_FREETYPE` | グリフ取得 `FreeTypeGetGlyph` の中だけ（`ft.cpp:3046-3064`） | 設定反映 `ReloadAll`（`fteng.h:622`） |

**確認できたロックの取得順**（左を持ったまま右を取る。レビュー 1.2 の指摘で、抜けていた組を足した）:

- 描画（グリフ取得中）: `OCS_DC(1)` → `OCS_FREETYPE(1)` → 次のどれか
  - `CS_FONTMAP` → `CS_FONTENG` → `CS_SETTING`（フォント検索 → 追加 → `fteng.h:382`）
  - `CS_FONTLINK` → `CS_SETTING`（`fteng.h:484-486` → `fteng.cpp:394`）、`CS_FONTLINK` → `CS_FONTMAP` → `CS_FONTENG`（`fteng.cpp:404`）
  - `CS_MANAGER` → `CS_FONTMAP`（`ft.cpp:3154`）、`CS_MANAGER` → `CS_SETTING`（`ft.cpp:3162` → `fteng.cpp:852`）、`CS_MANAGER` → `CS_CACHEDFONT`（フェイスを閉じるときの `DeleteFont`。`fteng.h:689`、`fteng.cpp:1008` → `override.cpp:766` → `428`）
  - `CS_FONTCACHE`、`CS_LIBRARY`、`CS_SETTING`（それぞれ単独）
- 描画（合成中）: `OCS_DC(1)` → `CS_SETTING`（`ft.cpp:2851, 2712`）。`OCS_FREETYPE(1)` は `FreeTypeGetGlyph` を出た時点で離している。合成の `FreeTypeTextOut`（`override.cpp:1618` → `ft.cpp:2823-3035`）は、`OCS_DC(1)` だけで走る。
- フォント削除: `OCS_DC(2)` → `CS_MANAGER` → 次のどれか
  - `CS_FONTMAP` → `CS_FONTCACHE`（`fteng.cpp:495, 521` → `fteng.h:433, 517`）、`CS_FONTMAP` → `CS_CACHEDFONT`（`fteng.h:434` → `override.cpp:766`）
  - `CS_FONTENG`（`fteng.cpp:566`）
- 設定反映: `OCS_FREETYPE(2)` → `CS_LIBRARY` →（`CS_SETTING`／`CS_FONTCACHE`）（`fteng.h:622-624, 640`）
- 遅延初期化: `CS_SETTING` → `CS_CACHEDFONT`（`settings.cpp:143, 1468`。FontSubstitutes 有効時だけ）
- FontSubstitutes=2 のときだけ: `CS_FONTENG` → `CS_SETTING`、`CS_FONTENG` → `CS_CACHEDFONT`（`fteng.h:391` → `override.cpp:712, 753` → `420`。レビューは 3 つを 1 本の鎖として書いているが、`override.cpp:712` の `CS_SETTING` は `GetInstance()` から戻った時点で離しているので、`CS_CACHEDFONT` を取るときには持っていない）

読んだ範囲では、これらのロック同士で順序が逆になる組は見つからなかった（確認済み。レビューも同じ結論。ただし全経路の網羅を保証するものではない）。また、`CS_SETTING`、`CS_FONTCACHE`、`CS_CACHEDFONT` を持ったまま別のロックを取る箇所は、上に書いた FontSubstitutes 有効時の組だけだった（同じく、読んだ範囲で）。ローダーロックとの関係は別問題（S-13, S-14）。

補足（レビュー 1.2 (4) と違う点）: 設定の再読み込みで遅延初期化がやり直されるのは、`ReloadAll` の中ではなく、その手前の `RefreshAlphaTable()` の中（`settings.h:1007-1008` → `ft.cpp:3493` → `ft.cpp:267` → `settings.cpp:142-143`）。ここで持っているのは `CS_SETTING` だけ。`ReloadAll` の `GetInstance()`（`fteng.h:624`）に来た時点では、もう済んでいる。`CS_LIBRARY` → `CS_SETTING` の順そのものは実在する。

**ロックで守られていない共有データ**（2 章の各項目へ）: グリフキャッシュ（S-01）、TLS 一覧と `TLSDCArray`（S-03）、`DCArray`（S-04）、除外フォント集合（S-05）、`FontNameCache`（S-06）、`FontCache` の読み取り（S-07）、設定全般（S-11）、フォント表の走査（設定反映側。S-20）。

### 1.7 けんじさんの設定で通る経路・通らない経路

添付の `Default.ini` の値を、コードの分岐と突き合わせた（すべて確認済み。値は INI の `[General]` など）。2 章・3 章の一覧の「今の設定」列は、この表から付けている。

| 設定（値） | コード上の意味 | 通る項目 | 通らない項目 |
|---|---|---|---|
| FontLoader=0 | FreeType でグリフを取り出す（`ft.cpp:3054-3056`） | `ForEachGetGlyphFT` 全体。S-01、P-05、P-08、P-20 | GDI の `GetGlyphOutline` で取り出す側（`ForEachGetGlyphGGO`。5 章の `CTempMem` の件も含む） |
| AntiAliasMode=4 | 軽いヒンティング向けの読み込みフラグ＋ LCD で描く（`ft.cpp:1616-1620`） | LCD 合成 → P-01 | — |
| HintingMode=1 | GDI 側: ヒンティングなしで読み込む（`ft.cpp:1583-1585`）。DirectWrite 側: グリッドフィット無効（`directwrite.cpp:227`）→ Direct2D の描画フックが毎回、変換行列に微小な傾きを足す | P-19（見送り） | — |
| DirectWrite=1 | 対話ユーザーのプロセスで、`DllMain` の中から `HookD2DDll` を呼ぶ（`hook.cpp:595-597`） | P-11、S-13 の `LoadLibrary`、S-09、S-18 c / d、S-23、1.3 B の COM フック全部 | P-12（順序を入れ替えても、`IsRunAsUser()` は結局必要） |
| `[DirectWrite]` RenderingMode=6 | Direct2D 側は 6 のまま（Windows SDK の定義では `OUTLINE` = 輪郭で描くモード）、DirectWrite 側は `NATURAL_SYMMETRIC` に置き換え（`directwrite.cpp:221, 233, 247-250`） | — | — |
| FontLink=2 | リンクフォントの判定を GDI の `GetGlyphIndices` で行う（`ft.cpp:1760, 1894-1905`。キャッシュにない文字だけ）。遅延初期化でレジストリを読む（`settings.cpp:223-225`） | S-19 c、P-15 | FreeType 側でのリンク判定（`ft.cpp:1906-1911`） |
| FontSubstitutes=0 | フォント作成フックを設置しない（`hook.cpp:601-603`）。置換表は空のまま（`settings.cpp:1714-1720`）。`FontCache` も常に空 | P-09（空の表のために毎回ロックを取る） | S-07、S-08、S-21、P-14、S-18 e。`[FontSubstitutes]` の 6 件は、読むだけで使われない |
| UseMapping=0 | FreeType がフォントファイルを読むたびに、GDI の `GetFontData` を呼ぶ（`fteng.cpp:1034`） | P-20 | フォント全体をメモリに写す側（`fteng.cpp:930-942`） |
| CacheMaxFaces=256 | FreeType が開いたままにするフェイスの数（`ft.cpp:3455`） | — | S-16 が起きるのは、257 書体以上を使うプロセスだけになる |
| CacheMaxSizes=6140 | フォントごとのサイズ別キャッシュ表の上限（`fteng.h:383`）と、FreeType のサイズ数（`ft.cpp:3456`） | — | サイズ別キャッシュの破棄（`fteng.cpp:445-446`）は、まず起きない |
| CacheMaxBytes=20485760 | FreeType の輪郭キャッシュ 約 20MB（`ft.cpp:3457`。プロセスごと） | — | — |
| ColorFont（未設定 = 0） | カラーフォント用の経路を使わない（`settings.cpp:663`、`ft.cpp:2186`） | — | S-15 |
| MaxBitmap（未設定 = 0） | 埋め込みビットマップを使わない（`settings.cpp:629`、`ft.cpp:1884, 1984`） | — | S-18 b |
| Shadow（未設定） | 影を描かない（`override.cpp:1397`） | — | 影の合成 |
| ClipBoxFix（未設定 = 1） | `GetGlyphOutline` フックが外接箱を広げる（`settings.cpp:662`、`override.cpp:477, 526`） | P-16 | — |
| `[Individual]` 16 件 | 遅延初期化で、1 件ごとにフォント名を引く（`settings.cpp:912`。未登録の名前はフォント列挙になる、`fteng.cpp:43-52`） | P-15 | — |
| `[UnloadDll]` 24 件 | 対象のプロセスでは、INI を読んだ後、フックせずに戻る（`hook.cpp:558-569, 607-624`） | 静的初期化（P-10、P-13）と INI の読み込みだけ | それ以外すべて |
| HookChildProcesses=1、LoadOnDemand=1 | Core では効かない。読むのは使われていないコードだけ（`supinfo.h:54, 107, 247`、`ft.cpp:2689` の空の `if`）。ローダー側の設定と思われる（推測） | — | — |

読み取れること:

- 置換フォントまわり（S-07、S-08、S-21、P-14）は、けんじさんの PC では通らない。upstream に出す価値は残るが、着手順は後ろに下げた。
- DirectWrite=1 なので、起動時の `d2d1.dll` / `dwrite.dll` の読み込み（P-11、S-13）と COM フックの競合（S-09）は、実際に通る。
- `[UnloadDll]` のプロセス（`SearchProtocolHost.exe` など、何度も起動されるもの）でも、DLL の静的初期化と INI の読み込みは毎回走る。P-10 は、こういうプロセスにいちばん効く。

---

## 2. 安定性リスク

### 2.1 一覧

| ID | 内容 | 根拠 | 影響 | 難しさ | 確信度 | 今の設定 |
|---|---|---|---|---|---|---|
| S-01 | グリフキャッシュ（`std::map`）を、複数スレッドがロックなしで読み書きする。間引きのときと、同じ文字が同時に追加されたときに、解放済みのグリフを読みうる | `fteng.h:223-240, 258-276`、`fteng.cpp:215-230, 280-369`、`ftref.c:7-27`、`ft.cpp:1857-1866, 2243-2251, 3046` | 大 | 中 | 高（上書きの経路は再現コードあり） | 通る |
| S-02 | 所有者つきロックで、所有者とカウントの更新がロックの外にあり、排他が破れる。破れた後は、抜ける側が相手のカウントを減らすので連鎖する | `ownedcs.cpp:24-44, 46-61` | 中 | 中 | 高（再現コードあり） | 通る |
| S-03 | TLS オブジェクト一覧と `TLSDCArray` を、スレッドの初回描画・終了時にロックなしで更新する | `tlsdata.h:29-31, 73-75`、`override.h:74, 78`、`fteng.cpp:575-580` | 中 | 低 | 高 | 通る |
| S-04 | パス作成中 DC の集合 `DCArray` を、ロックなしで追加・削除・検索する | `override.cpp:47, 629, 640, 651, 1139` | 中 | 低 | 高 | 通る |
| S-05 | 除外フォント集合に、描画中のスレッドがロックなしで追加する（他スレッドは同時に検索） | `settings.cpp:1056-1071`、`override.cpp:1311, 1317`、`ft.cpp:1445, 1517` | 中 | 低〜中 | 高 | 通る |
| S-06 | フォント名キャッシュ `FontNameCache` を、別々のロックの下（またはロックなし）から読み書きする | `hash_list.cpp:5-37`、`fteng.cpp:43, 52`、`settings.cpp:1358, 1421, 1745-1766` | 中 | 低 | 高 | 通る |
| S-07 | 置換フォント記録 `FontCache` の読み取りがロックなしで、中の文字列ポインタをそのまま返す（FontSubstitutes 有効時のみ） | `override.cpp:399-415, 417-435, 444, 563, 582, 661, 680` | 中 | 低 | 高 | 通らない |
| S-08 | `GetTextFaceA` / `GetTextFaceAliasW` のフックが、呼び出し元のバッファ長を無視して最大 32 文字書く（FontSubstitutes 有効時のみ） | `override.cpp:437-451, 579-595`（正しい例は `560-577`） | 中 | 低 | 高 | 通らない |
| S-09 | COM メソッドのフック設置（`HOOK` マクロ）が直列化されていない。`ORIG_xxx` を上書きして無限再帰、またはフックの取りこぼし | `directwrite.cpp:28-35, 625, 797, 881`、`hook.cpp:111-123`、`gdipp.vcxproj:509` | 中 | 低 | 中 | 通る |
| S-10 | 可変フォントの名前比較: ヒープの読み越し、例外の送出、メモリリーク | `ft.cpp:1320-1399, 3210`、`common.cpp:3-20` | 中 | 低〜中 | 高（例外の発生条件は中） | 通る |
| S-11 | 設定の再読み込みが、描画スレッドと排他されていない | `settings.h:978-1012`、`settings.cpp:747-752, 1218-1235`、`fteng.h:619-647` | 中 | 高 | 高 | 通る（再読み込み時） |
| S-12 | `WM_COPYDATA` の処理で JSON の例外を捕まえていない | `settings.h:1065-1098` | 小〜中 | 低 | 中 | 不明（ローダー次第） |
| S-13 | `DllMain`（ローダーロック中）で、ファイル読み込み・FreeType 初期化・`LoadLibrary` などの重い処理をしている | `hook.cpp:558-559, 573, 584, 595-599`、`directwrite.cpp:1500-1506`、`settings.cpp:535-538` | 中 | 中〜高 | 高（実害は推測） | 通る |
| S-14 | 動的アンロード時、フック内に残ったスレッドを待ち切れないと、そのまま資源を解放する | `hook.cpp:134-147, 639-681`、`hookCounter.h:18-23` | 中 | 高 | 中 | 通る（アンロード時） |
| S-15 | 同じ `FT_Face` を、別々のロック（`CS_MANAGER` と `CS_LIBRARY`）の下で使う（ColorFont 有効時） | `ft.cpp:2123-2136, 2185-2193` | 小 | 中 | 中 | 通らない |
| S-16 | キャッシュマネージャから借りた `FT_Face` を、ロックを離した後も使い続ける | `ft.h:358-377`、`ft.cpp:2076, 2113, 2186` | 小〜中 | 高 | 中 | 条件つき（257 書体以上） |
| S-17 | `RemoveFontResourceEx` フックが重く、副作用もある（未登録なら一度 `AddFontResource` する、他スレッドの作業用ビットマップのデストラクタを直接呼ぶ） | `override.cpp:843-847`、`fteng.cpp:59-99, 542-583` | 小〜中 | 高 | 中 | 通る（アプリが呼べば） |
| S-18 | NULL・失敗チェック漏れ（a〜h。2.2 に内訳） | 2.2 参照 | 小 | 低 | 高 | b・e は通らない。ほかは通る |
| S-19 | 配列の境界・バッファ長の小さな誤り（a〜d。2.2 に内訳） | 2.2 参照 | 小 | 低 | 高 | 通る |
| S-20 | フォント削除と設定反映が、互いに排他されない。設定反映はフォント表を `CS_FONTMAP` なしで走査し、その間に削除側が同じ表から消して `delete` しうる | `fteng.cpp:493-523, 550-551`、`fteng.h:619-647` | 小 | 低〜中 | 高（頻度は推測） | 通る（まれ） |
| S-21 | `GetTextFaceA` / `GetObjectA` のフックが、名前の変換に CRT の `wcstombs` を使う。この DLL の CRT は常に "C" ロケールなので、日本語などの名前は必ず変換に失敗する（FontSubstitutes 有効時のみ） | `override.cpp:585-592, 664-668`、`gdipp.vcxproj:499, 682`、`expfunc.cpp:190`、`settings.cpp:665-675` | 小 | 低 | 高 | 通らない |
| S-22 | 所有者つきロックの飢餓。描画が途切れずに重なると、フォント削除・設定反映がいつまでも入れない。待ちに入った描画どうしは 1 本ずつしか進まない | `ownedcs.cpp:13, 27-30, 34-37, 54-55` | 小 | 中 | 高（再現コードあり。実機での頻度は推測） | 通る |
| S-23 | Win32 ビルドのみ: DirectWrite 用パラメータを持つ関数内 static が、初期化の途中で別スレッドから使われうる（既定値のまま 1 回描く、パラメータを二重に作る） | `directwrite.cpp:237-281`、`gdipp.vcxproj:509` | 小 | 低 | 中 | Win32 のプロセスだけ |

### 2.2 各項目の説明

#### S-01 グリフキャッシュがロックなし

- 確認済み（全体）:
  - グリフキャッシュの実体は `std::map<int, FreeTypeCharData*>`（`fteng.h:204, 218`）。検索（`FindChar` / `FindGlyphIndex`、`fteng.h:258-276`）も、追加（`AddCharData` / `AddGlyphData`、`fteng.cpp:280-369`）も、ロックを取らない。間引き（`CompactMap`）だけは `CS_FONTCACHE` を取る（`fteng.cpp:126`）が、相手の検索・追加が取らないので、守れていない。
  - 呼び出し元が取っているのは `OCS_DC(1)` と `OCS_FREETYPE(1)` だけで、これは描画スレッド同士を排他しない（`override.cpp:1252`、`ft.cpp:3046`、`ownedcs.cpp:27-30`）。
  - キャッシュヒット時の `CS_LIBRARY`（`ft.cpp:1865`）は、検索の後で取っている。しかも、グリフを手放す側（下の `SetGlyph`、`~FreeTypeCharData` の `fteng.cpp:203-208`）は `CS_LIBRARY` を取らない。だから、下の 2 つの壊れ方のどちらも防がない。
- 確認済み（壊れ方 1: 間引き）: 追加の途中で間引き（`Compact`）が走ると、ほかのスレッドが検索で得たばかりの `FreeTypeCharData` を解放しうる（`fteng.cpp:285-287, 123-136`）。検索した側は、そのポインタから幅やグリフを読む（`ft.cpp:1863-1866`）。
- 確認済み（壊れ方 2: 同じ文字の上書き。レビューの指摘を、コードと再現コードで確かめた）:
  - `AddCharData` / `AddGlyphData` は、同じキーがすでにあると、新しい項目を作らずに `SetGlyph` で中のグリフを差し替える（`fteng.cpp:297-307, 343-354`）。
  - `SetGlyph` は、古いグリフを `FT_Done_Ref_Glyph` で手放してから、新しいグリフを入れる（`fteng.cpp:218-222`）。`FT_Done_Ref_Glyph` は、参照が 0 になればその場で解放し、どちらの場合もポインタを NULL にする（`ftref.c:18-27`）。つまり差し替えの間、`m_glyph` は一瞬 NULL になる。
  - この追加は、どのロックの中でもない（`ft.cpp:2243-2251`。ラスタライズ用の `CS_LIBRARY` は `ft.cpp:2200` で離した後）。
  - キャッシュヒット側は、`m_glyph` を読んでそのまま `FT_Glyph_Ref_Copy` に渡す（`ft.cpp:1866`）。NULL の検査はない。`FT_Glyph_Ref_Copy` は、カウントを増やす前に、渡されたグリフの中身を読む（`ftref.c:9-13`）。差し替えと重なると、解放済みのグリフを読んでカウントを増やす（後で二重解放になる）か、NULL を読んで落ちる。
  - 起きる条件: 2 つのスレッドが、同じフォント・同じサイズの「まだキャッシュにない同じ文字」をほぼ同時に描く。両方が取りこぼして両方がラスタライズし、後から追加した方が上書きする。その上書きの瞬間に、別のスレッドか、先に描き終えたスレッドの次の描画が、同じ文字をキャッシュから引く。レビューは「3 本目のスレッド」と書いているが、スレッドは 2 本でも足りる。
  - モノクロと非モノクロが混ざる場合（S-18 f）にも、同じ差し替えが起きる。
- 確認済み（再現コード）: `ftref.c` と `SetGlyph` のロジックを Linux に写し、上書きと読み取りを別スレッドで回した。AddressSanitizer は、`FT_Glyph_Ref_Copy` に当たる行で「解放済みメモリの読み取り」を報告した。検査なしのビルドは、3 回動かして 3 回とも、3 秒以内に異常終了した（ヒープ破壊の検出が 2 回、不正なメモリアクセスが 1 回）。ただし、これは上書きを連続で起こしたときの確認で、実機での頻度の再現ではない。コードは付録 A。
- 確認済み（Windows 上での再現。T-01 の複数スレッドのモード、2026-11-17）: Actions のランナーで、改造前のコードが `ft.cpp:1866`（NULL グリフの読み取り）と `fteng.cpp:220`（`SetGlyph` での差し替え、`AddCharData` の `fteng.cpp:303` から）で落ちた。Win32 / x64 とも。ただし T-01 は 8 スレッドが同じ文字を同時に描く極端な条件で、実際のアプリでの頻度を示すものではない。詳しくは 4 章の T-01。
- 推測: 同じフォント・同じサイズの文字を、2 つ以上のスレッドが同時に GDI で描くプロセス（複数の UI スレッドを持つアプリなど）で、まれにクラッシュやハングになる。間引きは 2048 回の追加に 1 回だが、上書きは、新しい文字が初めて出るたびに機会がある。初版の見込みより起きやすいかもしれない（レビューの推測。同意）。一方、けんじさんの PC では落ちるアプリが特にないので、実際の頻度は低いと思われる。
- 直し方の方向:
  - 囲む範囲: 「検索〜参照コピー」（`ft.cpp:1857-1866`）と、「追加・上書き・間引き」（`AddCharData` / `AddGlyphData` の全体）を、同じロックで囲む。
  - ロックは新しく作らず、`CS_FONTCACHE` を使う（レビューの提案。同意）。間引きがすでに取っているので、再入で済む。ラスタライズ中に握られる `CS_LIBRARY` とは別なので、待たされにくい。
  - 取得順: 今ある順は `CS_LIBRARY` → `CS_FONTCACHE`（設定反映、1.6）。逆を作らないよう、`CS_FONTCACHE` を持ったまま `CS_LIBRARY` や `CS_MANAGER` を取らない。つまり、ラスタライズとグリフの読み込みは、今までどおりロックの外に置く。
  - キャッシュヒット時の `CS_LIBRARY` と置き換えれば、ロック回数は増えない（P-05 と一緒に設計する。ブランチは分ける）。
  - 確かめ方: T-01 に「複数スレッドから同じ文字列を同時に描く」モードを足し、修正の前後で回す（4 章）。
  - サイズ別キャッシュ自体（`FreeTypeFontCache`）が破棄される経路（`fteng.cpp:373-381`）も同じ問題を持つが、既定値（1200 サイズ）ではまず起きない。けんじさんの設定（6140）では、さらに起きにくい。

#### S-02 所有者つきロックの排他破れ

- 確認済み: `EnterOwnedCritialSection` は、内部ロックを離した後で `nOwner` と `nRecursiveCount` を書く（`ownedcs.cpp:36-42`）。この 2 つの代入の間に、同じ owner の別スレッドが入るとカウントが 1 つ失われる。先に入ったスレッドが抜けた時点でロックが「空き」と見なされ、owner=2（フォント削除、設定反映）が、まだ中にいる描画スレッドと同時に入れる。
- 確認済み（レビューの補足）: 抜ける側の `LeaveOwnedCritialSection` は、`nOwner` が自分の owner と違うとき、相手のカウントを 1 減らす（`ownedcs.cpp:58-59`）。排他が破れた後、取り残された描画スレッドが抜けると、owner=2 側のカウントが減る。owner=2 側に合流しているスレッドがいれば、まだ中にいるのに「空き」になる。破れが連鎖する。
- 確認済み（再現コード）: ロジックをそのまま Linux に写したテストで確かめた。写しそのままでは 5 秒 × 3 回で違反 0 件（窓が非常に狭い）。2 つの代入の間に関数呼び出し 1 つ分の遅れを入れると、約 2900 万回中 17 件、`yield` を入れると 60〜90 件の「同時進入」が出た。レビューは、同じ流れを状態の表で 1 行ずつ追って確かめている（再現コードの再実行はしていない）。
- 推測: 実害が出るのは、ちょうどその瞬間にフォント削除や設定の再読み込みが重なったときだけ。頻度は低い。
- 直し方の方向（レビューの提案。同意）:
  - 待たずに入る経路は、内部ロックを離す（`ownedcs.cpp:40`）前に、`nOwner` と `nRecursiveCount` を書く。
  - 待つ経路は、起きた後で内部ロックを取り直してから書く。起きた時点では `nOwner` は −1 なので、まだ誰も合流していない。
  - こうすれば、`ownedcs.cpp:58-59` の分岐は通らなくなる。動作は変わらないが、全描画が通るロックなので慎重に。飢餓（S-22）は、この修正では直らない。
- 別案: Vista 以降の API を使ってよいので、SRW ロック（共有 = 描画、排他 = 削除・設定反映）に置き換える手もある。ただし「owner=2 どうしが合流できる」「同じスレッドの再入」など、今の動きとの違いを先に洗う必要があり、差分も大きい。まずは上の最小の修正にする。

#### S-03 TLS 一覧と `TLSDCArray`

- 確認済み: スレッドが初めて描画すると `CTlsData::GetPtr` が `pArray->Add`（`tlsdata.h:29-31`）を、`CThreadLocalInfo` のコンストラクタが `TLSDCArray.insert`（`override.h:74`）を、どちらもロックなしで行う。スレッド終了時の削除も同じ（`tlsdata.h:73-75`、`override.h:78`）。`RemoveFont` は `TLSDCArray` を走査する（`fteng.cpp:575-580`）。
- 推測: 複数のスレッドが同時に初回描画する、またはスレッドの開始と終了が重なると、配列や集合が壊れる。壊れた後の追加・削除・スレッド終了時にクラッシュする。
- 直し方の方向: 追加・削除・走査をロックで囲む。スレッドの初回と終了時だけなので、速度への影響はない。
- 修正時の注意（レビューの補足をもとに、コードで確かめた）: 削除側（`DLL_THREAD_DETACH`、`hook.cpp:633-634`）はローダーロックの中なので、「ローダーロック → 新しいロック」の順ができる。新しいロックを持ったまま、`LoadLibrary` などローダーロックを待つものを呼ばない。
  - 囲むのは、配列・集合への追加と削除だけにする（`tlsdata.h:29-31, 73-75`、`override.h:74, 78`）。
  - 削除側の `delete pT`（`tlsdata.h:76`）は、ロックの外に置く。作業用ビットマップのデストラクタが GDI を呼ぶため（`cache.h:95-104`。`DeleteObject` はフック済みなので、中で `CS_CACHEDFONT` も取る）。
  - レビューは「追加側の `new T` が GDI を呼びうる」と書いているが、コンストラクタはメンバーの初期化と `TLSDCArray.insert` だけで、GDI は呼ばない（`cache.h:82-93`、`override.h:71-75`）。GDI を呼ぶのは削除側。

#### S-04 `DCArray`

- 確認済み: `BeginPath` / `EndPath` / `AbortPath` のフックが `std::set<HDC>` をロックなしで更新し（`override.cpp:629, 640, 651`）、`ExtTextOutW` のフックが毎回ロックなしで検索する（`override.cpp:1139`）。
- 推測: パスを使うスレッドと文字を描くスレッドが別にあるアプリで、まれに壊れる。
- 直し方の方向: ロックで囲む。`ExtTextOutW` は毎回通るので、「集合が空ならロックを取らない」形（個数をアトミックに持つ）にして、速度を落とさない。
- 補足（レビューの指摘。コードで確認）: 集合から消すのは `EndPath` / `AbortPath` が成功したときだけ（`override.cpp:640, 651`）。パス作成中のまま DC を消すと、その `HDC` の値が集合に残り続ける。同じ値の `HDC` が再利用されると、その DC では MacType が描かない（`override.cpp:1139-1140`）。集合が空にならないので、「空ならロックを取らない」近道も効かなくなる。ただし、ここを直すと描画が変わるので、S-04 では触らない（5 章）。

#### S-05 除外フォント集合

- 確認済み: `IsFontExcluded` は `std::set<wstring>` をロックなしで検索する（`settings.cpp:1056-1065`）。`AddFontExclude` はロックなしで追加する（`settings.cpp:1067-1071`）。追加は、描画の途中で「TrueType/OpenType ではない」「フェイスを開けない」とわかったときに起きる（`override.cpp:1317`、`ft.cpp:1445, 1517`）。
- 推測: ビットマップフォントなどを初めて描く瞬間に、別スレッドの検索と重なると壊れる。

#### S-06 `FontNameCache`

- 確認済み: `CHashedStringList` は中で `std::map` を使い、ロックを持たない（`hash_list.h`、`hash_list.cpp:5-37`）。呼び出し元のロックはばらばら。`FreeTypeFontInfo` のコンストラクタ（`CS_FONTENG` の下、`fteng.h:412`）、リンクフォント作成（`CS_FONTLINK` の下、`settings.cpp:1790`）、遅延初期化（`CS_SETTING` の下、`settings.cpp:216`）、フォント作成フック（ロックなし、`override.cpp:724` → `settings.cpp:1745-1766`）。
- 直し方の方向: `CHashedStringList` の中にロックを 1 つ持たせる。

#### S-07 / S-08 置換フォントまわり（FontSubstitutes 有効時のみ）

- S-07 確認済み: 書き込み側（`AddToCachedFont` / `DeleteCachedFont`）は `CS_CACHEDFONT` を取るが、読み取り側（`GetCachedFont` / `GetCachedFontLocale`、`override.cpp:399-415`）は取らない。しかも中の文字列へのポインタを返すので、別スレッドが同じフォントを `DeleteObject` すると、解放済みメモリを読む。
- S-08 確認済み: `IMPL_GetTextFaceAliasW` は `nLen` を見ずに `StringCchCopy(lpAliasW, LF_FACESIZE, ...)`（`override.cpp:447`）、`IMPL_GetTextFaceA` は `c` を見ずに `StringCchCopyA(lpName, LF_FACESIZE, ...)`（`override.cpp:590`）。W 版（`override.cpp:567-568`）は長さを見ている。
- S-08 の起きる条件（レビューの補足で正確にした）: 書き込むのは「名前の長さ + 1」文字まで（最大 32）。あふれるのは、それがアプリのバッファ長より大きいとき。初版の「32 文字より小さいバッファなら必ず」は言いすぎだった。
- FontSubstitutes を使っていなければ `FontCache` は常に空なので、どちらも起きない。けんじさんの設定（FontSubstitutes=0）では通らない。
- A 版の名前変換そのものの不具合は、別の ID にした（S-21）。

#### S-09 COM フック設置の競合

- 確認済み:
  - `HOOK` マクロは「未フックか確認 → `ORIG_xxx` に vtable の関数アドレスを代入 → `hook_demand_xxx`」の 3 段階で、全体を守るロックがない（`directwrite.cpp:28-35`）。別スレッドが同じフックを先に完了させた直後に代入が走ると、`ORIG_xxx`（Detours がトランポリンに書き換え済み）が、フック済みの関数アドレスで上書きされる。以後 `IMPL_xxx` → `ORIG_xxx` → `REF_xxx` → `IMPL_xxx` の無限再帰になる。
  - `GetAlphaBlendParams` は 3 箇所から設置されうる（`directwrite.cpp:625, 797, 881`）。それぞれ別の `static bool loaded` で守られているだけなので、3 箇所の間では排他されない。Win32 ビルドは `static` の初期化自体がスレッド安全でない（`gdipp.vcxproj:509`）。
  - `hook_demand_xxx` は `DetourTransactionBegin` の戻り値を見ず、`DetourAttach` が失敗しても `IsHooked_xxx = true` にする（`hook.cpp:113-118`）。Detours は、他スレッドのトランザクション中だと `Begin` も `Attach` も `ERROR_INVALID_OPERATION` を返す（Detours の `detours.cpp:1873-1881, 2319-2321` で確認）。つまり競合すると、そのフックは「設置済み」と記録されたまま、設置されない。
- 推測: 窓はごく狭い。ただし無限再帰は一度起きると、そのプロセスではそのメソッドを呼ぶたびにスタックオーバーフローする。過去の 0522b1f（スタックオーバーフロー修正）、b9f9e05（フックの競合修正）と同じ系統。
- 直し方の方向: `HOOK` マクロの中身全体を 1 つのロック（既存の `CS_DWRITE`）で囲む。`COM_METHOD_HOOKER`（`directwrite.cpp:46-57`）も中で `HOOK` を使うので、一緒に守られる。レンダーターゲットの設置側は、すでに `CS_DWRITE` を持っている（`directwrite.cpp:484, 499, 514`）が、`CRITICAL_SECTION` は同じスレッドなら再入できるので問題ない（レビューの補足。同意）。
- 補足: `hook_demand_xxx` の `&ORIG_##name`（`hook.cpp:117`）は、グローバル変数のアドレスなので常に真。検査として働いていない（動作には影響しない）。
- DirectWrite=1 なので、けんじさんの設定で通る。

#### S-10 可変フォントの名前比較

Windows 11 の Segoe UI Variable など、名前つきインスタンスを持つ可変フォントを開くたびに通る（`face_requester` → `VarFontByAlias` → `IsSFNTNameMatch`、`ft.cpp:3210, 1380-1399, 1320-1378`）。

- 確認済み (a) 読み越し: `FT_SfntName.string_len` はバイト数だが、`wstring(ポインタ, string_len)` と文字数として使っている（`ft.cpp:1325, 1336`）。FreeType はこの文字列をちょうど `string_len` バイトで確保する（freetype の `src/base/ftsnames.c:68-70` で確認）ので、確保した領域の後ろを同じバイト数だけ読む。
- 確認済み (b) 例外: Mac プラットフォームなどの名前を `to_wide_string` に渡す（`ft.cpp:1329, 1339`）。中身は `std::wstring_convert`（`common.cpp:16-20`）で、UTF-8 として不正なバイト列だと `std::range_error` を投げる。途中に `catch` はなく、FreeType（C のコード）を突き抜けてアプリまで届く。
- 確認済み (c) リーク: 名前つきインスタンスが 0 個の可変フォントでは、`FT_Done_MM_Var` を呼ばずに戻る（`ft.cpp:1386`）。
- 推測: (a) は読むだけなので、ほぼ表面化しない。(b) は、Mac プラットフォームの名前にアクセント記号などを持つ可変フォントがあると、そのフォントを使うアプリが確実に落ちる。そういうフォントが実在するかは未確認。
- 注意: (a) を直すとき、Windows プラットフォームの名前（`ft.cpp:1336`）は結果を変えずに直せる。Unicode プラットフォーム（`ft.cpp:1325`）は、今は壊れた文字列になっていて一致しない。正しく直すと一致するようになり、選ばれるインスタンスが変わりうる（Windows 用の名前を持たないフォントに限る）。
- レビューの補足（3 点とも、コードで確かめて同意）:
  1. (b) の後遺症: 例外で `IMPL_ExtTextOutW` を抜けると、`InExtTextOut(true)`（`override.cpp:1270`）が戻されない（戻すのは `override.cpp:1687, 1691` だけ）。`ETO_TRY` / `ETO_CATCH` は名前だけで、`try` / `catch` ではない（`override.cpp:1001-1003`）。アプリが例外を捕まえて動き続けた場合、そのスレッドは以後ずっと再入扱い（`override.cpp:1196-1198`）になり、MacType が効かなくなる。作業配列（`override.cpp:1262-1263`）もリークする。
  2. (a) を直すときの注意: Windows 名前の変換 `to_utf16le` は、`char` を符号つきのまま `char16_t` に広げている（`common.cpp:9-10`）。下位バイトが 0x80 以上だと、上位バイトが 0xFF になる。つまり、非 ASCII の名前は今も正しく変換されていない。上の「結果を変えずに直せる」は、**読み越しだけを直し、この変換はそのまま残す**場合に正しい。変換まで直すと、非 ASCII のインスタンス名を持つフォントで、選ばれるインスタンスが変わりうる（5 章）。
  3. 起きる条件の絞り込み: 名前を変換するのは、名前 ID がインスタンスの ID と一致するレコードだけ（`ft.cpp:1367`）。Mac プラットフォームのその名前が ASCII だけなら、(b) は起きない。

#### S-11 設定の再読み込み

- 確認済み: `ReloadConfig`（エクスポート）と `CControlCenter::LoadSetting` は、Individual 配列、置換表、リンクフォント表、除外集合を、ロックなしで空にして作り直す（`settings.h:1002-1006, 981-985`、`settings.cpp:747-752`）。`ReloadConfig` の先頭のロックはコメントアウトされている（`settings.h:999`）。描画スレッドはこれらをロックなしで読む（例: `FindIndividual`、`settings.cpp:1218-1235`）。`ReloadAll` の排他（`fteng.h:622-623`）は、フォントキャッシュの作り直しの部分だけ。
- 推測: プロファイルを切り替えた瞬間に描画中だったアプリが、まれに落ちる。
- 直し方の方向: 設定全体を「読み手多数・書き手 1 人」で守る必要がある。DirectWrite 側も設定を読むので範囲が広い。大きな変更になるため後回し。

#### S-12 `WM_COPYDATA` の JSON

- 確認済み: `json::parse` と `get<std::string>()` は、不正な入力で例外を投げる（`settings.h:1074-1078`）。`catch` はない。ウィンドウプロシージャから C++ 例外が漏れる。
- 未確認: このメッセージ窓が実際に作られているか（`CreateMessageWnd` を呼ぶのは非公開のローダーや Tuner 側）。
- 直し方の方向: `try` / `catch` で囲んで、失敗したら何もしない。

#### S-13 `DllMain` 内の重い処理

- 確認済み: ローダーロックを持ったまま、INI ファイルの読み込みと解析（`settings.cpp:535-538`）、FreeType の初期化（`hook.cpp:573`）、Detours のトランザクション（`hook.cpp:584`）、トークン情報の取得（`hook.cpp:595`）、`LoadLibrary("d2d1.dll")` / `LoadLibrary("dwrite.dll")`（`directwrite.cpp:1503-1506`。INI の `DirectWrite` が有効なときだけ）、ミューテックスのオープン（`hook.cpp:609-615`。UnloadDLL 対象のときだけ）を行う。`DllMain` の `try` / `catch(...)` は C++ 例外しか捕まえない（`hook.cpp:496, 685-687`）。
- 過去の関連修正: 8a7f497（DirectWrite 有効時に Windows 7 が起動で固まる）、5320ef4（起動時に固まるのを防ぐための対話プロセス判定）、e35c7a1 / 8c7214a（#192 のプロセス起動遅延）。
- 推測: 現状で固まる具体的な経路は見つけていない。ただし `DllMain` 内の `LoadLibrary` は、ほかの DLL の初期化順に影響する典型的な危険箇所。
- 直し方の方向: P-11（`d2d1` / `dwrite` の遅延）が、安定性にも効く。
- けんじさんの設定（DirectWrite=1）では、`LoadLibrary` の分も実際に通る（1.7）。

#### S-14 動的アンロード

- 確認済み: `hook_term` はフックを外した後、フック内にいるスレッド数が 0 になるのを最大 3000 回（1 回 1ms 以上のスリープ）待つ（`hook.cpp:146`、`hookCounter.h:18-23`）。これは `DllMain`（ローダーロック中）で行われる。待ち切れなくても、そのままエンジンや設定、ロックを破棄する（`hook.cpp:656-677`）。
- 推測: フック内でローダーロックを待っているスレッドがいると、待ちは必ずタイムアウトし、その後そのスレッドが解放済みの資源に触れて落ちる。
- 直し方の方向: 根本的には「アンロードしない」しかない。ローダー側の仕様が絡むので、後回し。

#### S-15 / S-16 `FT_Face` の扱い

- S-15 確認済み: ColorFont 設定が有効で、フェイスがカラーを持つとき、`FT_Glyph_To_BitmapEx` にフェイスを渡す（`ft.cpp:2186-2188`、`CS_LIBRARY` の下）。改造版 FreeType はここでフェイスにグリフスロットを足し、`FT_Load_Glyph` を呼ぶ（freetype の `src/base/ftglyph.c:930-934`、`src/base/ftobjs.c:4744-4806` で確認）。同じフェイスを、別スレッドは `CS_MANAGER` の下で使う（`ft.cpp:2123-2136`）。
- S-16 確認済み: `FreeTypeDrawInfo::GetFace` は `CS_MANAGER` の中で得たフェイスのポインタを保存し、ロックを離した後に使う（`ft.h:358-377`。使用例は `ft.cpp:2076` のカーニング、`2113` のスタイル判定）。
- S-16 確認済み（追加）: 合成の `FreeTypeTextOut` も、保存しておいたフェイス（`ft.cpp:2840`）を、下線・取り消し線の太さの計算で読む（`ft.cpp:2906-2925`）。ここは `OCS_FREETYPE` も `CS_MANAGER` も持っていない（1.6）。
- S-16 推測: キャッシュマネージャは、フェイス数が上限（既定 64 以上）を超えると古いフェイスを閉じる。別スレッドの検索でそれが起きると、解放済みのフェイスを読む。上限を超える数の書体を使い、かつ複数スレッドで描くプロセスに限られる。けんじさんの設定（CacheMaxFaces=256）では、257 書体以上。

#### S-17 `RemoveFontResourceEx` フック

- 確認済み: アプリが `RemoveFontResourceEx` を呼ぶたびに、(1) フォントファイルの中の書体名を調べ、情報が取れなければ `AddFontResourceW` で一度登録してから調べて外す（`fteng.cpp:59-99`）、(2) `OCS_DC(2)` で全描画を止める（`fteng.cpp:550`）、(3) 使用済みのフォントなら、全スレッドの作業用ビットマップ `CBitmapCache` のデストラクタを直接呼ぶ（`fteng.cpp:575-580`。オブジェクト自体は残り、後でもう一度デストラクタが走る）。
- 推測: 埋め込みフォントを頻繁に出し入れするアプリで、重さや不安定さの原因になる。

#### S-18 NULL・失敗チェック漏れ（それぞれ別ブランチ）

| 枝番 | 内容 | 根拠 | 起きる条件 |
|---|---|---|---|
| a | `GetOS2Table()` が NULL でも `os2_table->xAvgCharWidth` を読む | `ft.cpp:1472-1474` | OS/2 テーブルのないフォント かつ 幅指定つき、または拡大縮小 DC |
| b | `FindFont(int)` が NULL を返した後、`pfi->EmbeddedBmpExist` を呼ぶ | `ft.cpp:1930-1931, 1984` | リンク先フォントが削除済み かつ MaxBitmap > 0 |
| c | `DWriteCreateFactory` が失敗しても、結果のポインタを使う | `directwrite.cpp:120-128` | ファクトリ作成に失敗する環境 |
| d | `GetGdiInterop` の結果を確かめずに `*gdiInterop` を使う | `directwrite.cpp:637-642` | 元の呼び出しが失敗したとき |
| e | フォント置換で `Release` した後の再作成が失敗しても、`S_OK` のまま返す | `directwrite.cpp:1622-1623` | FontSubstitutes 有効 かつ 再作成失敗 |
| f | キャッシュにあるグリフが要求モード（モノクロ／それ以外）のものと違うと、NULL を `FT_Glyph_Ref_Copy` に渡す | `ft.cpp:1866, 2412`、`ftref.c:9` | 1 つの文字列の中で、リンクフォントの AA 設定が親と違う（モノクロと非モノクロ） |
| g | `GetGlyphOutline` が失敗したとき、または `lpgm` が NULL のときも、`lpgm` を読み書きする | `override.cpp:476-477, 488, 520, 526, 532` | アプリが無効な引数を渡したとき |
| h | `malloc` / `FT_Glyph_Copy` の失敗を確かめない | `override.cpp:1294-1296`、`ft.cpp:2134-2135` | メモリ不足時 |

#### S-19 配列の境界・バッファ長（それぞれ別ブランチ）

| 枝番 | 内容 | 根拠 |
|---|---|---|
| a | `ControlIder` の表が 0xFFFF 要素で、U+FFFF が範囲外（読み取りは描画のたび。1 バイトの読み越し） | `ft.h:148, 172, 176, 182` |
| b | `GetAppDir`: `MAX_PATH` 文字のバッファに `MAX_PATH + 1` を渡す | `settings.cpp:77-79` |
| c | `CFontLinkInfo::init`: `RegEnumValue` の名前長に「バイト数」（文字数の 2 倍）を渡す、空の名前で `buf[-1]` を読む、途中の `return` でバッファ 3 本（計 48KB）をリーク、レジストリから読んだ番号を検査せず配列の添字に使う | `settings.cpp:1332-1337, 1342, 1349, 1355, 1367, 1478, 1503, 1507, 1516-1518` |
| d | `new[]` で確保して `delete` で解放（未定義動作。MSVC の `int` などでは実害なし） | `ft.h:168, 345-346`、`ft.cpp:2266-2267, 2671-2672` |

#### S-20 フォント削除と設定反映が排他されない（レビューの案を、コードで確かめて採用）

- 確認済み:
  - フォント削除 `RemoveFont` は、`OCS_DC` の owner=2 と `CS_MANAGER` を取り（`fteng.cpp:550-551`）、その中で `CS_FONTMAP` を取って、フォント表 `m_mfullMap` から項目を消し、`FreeTypeFontInfo` を `delete` する（`fteng.cpp:495, 508-521`）。残った全フォントの `UpdateFontSetting` も呼ぶ（`fteng.cpp:516`）。
  - 設定反映 `ReloadAll` は、`OCS_FREETYPE` の owner=2 と `CS_LIBRARY` を取り（`fteng.h:622-623`）、`CS_FONTMAP` を取らずに同じ `m_mfullMap` を走査して、各フォントの `Erase` / `SetFontSettings` / `UpdateFontSetting` を呼ぶ（`fteng.h:626-645`）。
  - 2 つが取るロックに、共通のものがない（`OCS_DC` と `OCS_FREETYPE` は別のロック、1.6）。同時に走ると、設定反映側が、消されたばかりの項目や解放済みの `FreeTypeFontInfo` を触る。
- 推測: 起きるのは、アプリが `RemoveFontResourceEx` を呼んでいる最中に、設定の再読み込み（プロファイルの切り替えなど）が重なったときだけ。まれ。
- 直し方の方向: `ReloadAll` の走査を `CS_FONTMAP` で囲む。`CS_LIBRARY` → `CS_FONTMAP` という新しい順ができるので、逆順（`CS_FONTMAP` を持ったまま `CS_LIBRARY`）がないことを、先に確かめる（読んだ範囲では見つかっていないが、この目的では洗っていない）。
- 関連: 設定そのものの読み書きが排他されていない件は S-11。

#### S-21 `GetTextFaceA` / `GetObjectA` の名前変換（FontSubstitutes 有効時のみ）

- 確認済み:
  - `IMPL_GetTextFaceA` は、置換前の名前を CRT の `wcstombs` でバイト列に変換する（`override.cpp:585-592`）。`IMPL_GetObjectA` も同じ（`override.cpp:664-668`。レビューにはないが、同じ書き方）。
  - この DLL は CRT を静的リンクしている（`gdipp.vcxproj:499, 682`）。DLL の中で `setlocale` を呼ぶのは `WstringToString`（`expfunc.cpp:190`）だけで、その呼び出し元は `#ifdef INFINALITY` の中にしかない（`settings.cpp:665-675`）。`Rel+Detours` は `INFINALITY` を定義していない（`gdipp.vcxproj:497, 680`）。つまり、この DLL の CRT は、ずっと既定の "C" ロケールのまま。
  - "C" ロケールの `wcstombs` は、255 を超える文字を変換できずに −1 を返す（MSVC の CRT の仕様。実機では未確認）。その場合、`GetTextFaceA` の戻り値は 0 になり（`override.cpp:592`）、バッファには、変換できた先頭部分だけが入る。`GetObjectA` は、`lfFaceName` が先頭部分だけになる。
- レビューと違う点: レビューは「アプリが UTF-8 ロケールにしていると、バッファが足りず終端なしで切れる」とも書いている。しかし CRT が静的リンクなので、アプリ側の `setlocale` はこの DLL に届かない。この壊れ方は、今のビルドでは起きない。逆に、日本語などの名前は「たまに」ではなく、必ず失敗する。
- 影響: 置換したフォントの名前が日本語などのとき、A 版の API でアプリに返る名前が空や途中までになる。描画には関係しない。
- 直し方の方向: `WideCharToMultiByte(CP_ACP, ...)` に置き換え、S-08 と同じくバッファ長を守る。アプリに返る値が変わるので、S-08 とは別のブランチにする。
- けんじさんの設定（FontSubstitutes=0）では通らない。

#### S-22 所有者つきロックの飢餓（レビューの案を、コードと再現コードで確かめて採用）

- 確認済み: 仕組みは 1.6 のとおり。同じ owner は、後から来ても無条件に合流する（`ownedcs.cpp:27-30`）。別の owner は、中が空になるまで待つ（`ownedcs.cpp:34-37`）。待っている owner=2 がいても、新しく来た owner=1 を止める仕組みがない。
- 確認済み（再現コード。ロジックを Linux に写したもの、2 コアの環境。数字は実行ごとに少し変わる。コードは付録 B）:
  - 描画役（owner=1）が、休みなしで「入る → 300 マイクロ秒持つ → 抜ける」を繰り返す場合。描画役 2 本で、owner=2 は約 0.7 秒待たされた。4 本以上では、3 秒待っても入れなかった。
  - 描画役が、抜けるたびに 30 マイクロ秒休む場合。8 本でも、owner=2 は約 70 ミリ秒で入れた。4 本以下なら数ミリ秒以内。
  - 待ちに入った owner=1 の 3 本（それぞれ 50 ミリ秒持つ）は、owner=2 が抜けた後、同時には入れず、1 本ずつ進んだ（全部で約 150 ミリ秒）。
- 推測: 実機では、描画と描画の間に隙間があるのがふつうなので、長く止まることは少ない。効くとすれば、多くのスレッドが休みなく GDI で描いている最中の、フォント削除（アプリの `RemoveFontResourceEx` が戻ってこない）と、設定の反映（切り替えがなかなか効かない）。レビューは影響を「小〜中」としているが、ここでは「小」にした。落ちるのではなく待たされるだけで、条件も狭いため。
- 直し方の方向: 「owner=2 が待っている間は、新しい owner=1 を合流させずに並ばせる」形にする。待ちに入った同じ owner をまとめて起こす工夫も要る。S-02 を直した後で、別のブランチにする。SRW ロックへの置き換え（S-02 の別案）でも軽くなる見込みだが、未確認。

#### S-23 DirectWrite 用パラメータの関数内 static（Win32 のみ。今回の調査で追加）

- 確認済み: `GetD2DParams` / `GetDWParams` は、パラメータを関数内 static に持つ（`directwrite.cpp:237-255`）。`GetD2DRenderingParams` / `GetDWRenderingParams` は、「作成済み」の印を関数内 static に持つ（`directwrite.cpp:257-281`）。Win32 ビルドは `/Zc:threadSafeInit-`（`gdipp.vcxproj:509`）なので、これらの初期化はスレッド安全でない。x64 は安全。
- 推測: Win32 のプロセスで、2 つのスレッドが同時に初めて Direct2D / DirectWrite で描くと、片方が初期化途中の値（ゼロのまま = 既定のパラメータ）で 1 回描く。または、レンダリングパラメータを二重に作って片方をリークする。落ちる経路は見つけていない。MSVC がこの初期化をどの順で行うかは、確かめていない。
- 直し方の方向: S-09 と同じく `CS_DWRITE` で守るか、Vista 以降の `InitOnceExecuteOnce` を使う。描画のたびに通る場所なので、初期化済みならロックを取らない形にする。
- 別案: Win32 の `/Zc:threadSafeInit-` は XP 向けの設定と思われる（推測）。Vista 以降だけを対象にするなら外せて、Win32 も x64 と同じ動きになる。ただしコンパイルオプションの変更は DLL 全体に効くので、やるなら別の ID を付けて、けんじさんに相談してから（7 章）。
- S-09 と原因が近いが、直す場所が違うので、別の ID にした。


---

## 3. 高速化候補

### 3.1 一覧

| ID | 内容 | 根拠 | 影響 | 難しさ | 確信度 | 今の設定 |
|---|---|---|---|---|---|---|
| P-01 | アルファブレンドの表引き `conv2` が、画素の色成分ごとに実行時の割り算をする。シフトで置き換えられる。**状態（2026-11-17）: 実装済み。ブランチ `perf/p01-conv2-shift`。`Build MacType` と T-01 が Win32 / x64 とも成功。画素は全 553 ケースで同じ。速度の差は T-01 の揺れ（±6%）の内側で、確かめられなかった。`directwrite` にマージ済み。詳しくは 3.2** | `ft.cpp:208-210, 388-395, 450-460` | 小（初版は「大」。T-01 で差が見えなかったので下げた） | 低 | 高 | 通る |
| P-02 | 空白など「絵のないグリフ」1 文字ごとに、GDI の `ExtTextOutW` を 1 回呼ぶ | `ft.cpp:2702-2707` | 中〜大 | 低〜中 | 中 | 通る |
| P-03 | グリフ 1 個ごとに `CGdippSettings::GetInstance()`（グローバルロック）を呼ぶ | `ft.cpp:2712`、`settings.cpp:136-146` | 小〜中 | 低 | 高 | 通る |
| P-04 | `GetInstance()` が、初期化済みでも毎回 `CS_SETTING` を取る | `settings.cpp:136-146`、`settings.h:262-271` | 中 | 中 | 中 | 通る |
| P-05 | キャッシュヒット時に、文字ごとに `CS_LIBRARY` を取る（参照カウントはすでに Interlocked） | `ft.cpp:1865-1866, 2411-2412`、`ftref.c:7-16` | 中 | 低（S-01 と同時） | 高 | 通る |
| P-06 | `ExtTextOutW` 1 回あたりのヒープ確保が多い（読んで数えた概算で 15〜20 回） | `override.cpp:1156-1158, 1262-1263, 1294, 1299`、`ft.h:223-229, 336-338`、`expfunc.cpp:199-202`、`ft.cpp:1798, 2880-2881`、`settings.cpp:1058` | 中 | 低〜中 | 高 | 通る |
| P-07 | `GetOutlineTextMetrics` を毎回 2 回呼ぶ（サイズ取得と本取得） | `override.cpp:1288, 1294-1300` | 中 | 低〜中 | 中 | 通る |
| P-08 | グリフキャッシュの間引きがキーの小さい順。使用頻度を見ない（MRU カウンタは更新だけされて使われていない） | `fteng.cpp:123-136, 254-264, 285-287`、`fteng.h:125-140, 228, 237, 263, 273` | 中（日本語環境） | 中 | 高 | 通る |
| P-09 | `DeleteObject` フックが、成功のたびにグローバルロックを取って表を検索する（置換フォントがなければ表は常に空） | `override.cpp:760-768, 425-435` | 小〜中 | 低 | 高 | 通る（表は常に空） |
| P-10 | 起動時: `ControlIder` の静的初期化で `iswcntrl` を 36,863 回呼ぶ。除外・アンロード対象のプロセスでも走る | `ft.h:146-165`、`ft.cpp:62` | 中 | 低〜中 | 中 | 通る |
| P-11 | 起動時: D2D / DirectWrite を使わないプロセスでも、`d2d1.dll` と `dwrite.dll` を読み込む | `directwrite.cpp:1500-1506`、`hook.cpp:595-599` | 中〜大 | 中 | 高（効果は推測） | 通る |
| P-12 | 起動時: DirectWrite 設定が無効でも、先に `IsRunAsUser()`（トークン情報の取得）を実行する | `hook.cpp:595, 279-409` | 小 | 低 | 高 | 効果なし |
| P-13 | 起動時: `undocAPI.h` の `static` 変数が、.cpp ごとに `LoadLibrary` / `GetProcAddress` を実行する | `undocAPI.h:72, 91-96`、`common.h:28` | 小 | 低 | 高 | 通る |
| P-14 | FontSubstitutes 有効時: 存在しないフォント名でフォントを作るたびに、フォント列挙が走る（見つからなかった結果を覚えない） | `fteng.cpp:38-57`、`settings.cpp:1745-1768` | 小〜中 | 中 | 中 | 通らない |
| P-15 | 遅延初期化が、最初のフック呼び出しの中で `CS_SETTING` を握ったまま、レジストリ列挙とフォント列挙を大量に行う | `settings.cpp:136-146, 156-306, 1314-1525, 1596-1628, 1643-1672` | 中 | 高 | 中 | 通る |
| P-16 | `GetGlyphOutline` フックが、寸法だけの問い合わせのたびに `GetTextMetrics` を呼ぶ | `override.cpp:477-487, 526-531` | 小〜中 | 中 | 中 | 通る |
| P-17 | 作業用 DIB を 256 回ごとに「そのときの要求サイズ」へ縮め、直後にまた広げ直す | `cache.cpp:4, 22-41` | 小 | 低〜中 | 中 | 通る |
| P-18 | すべてのフック呼び出しが、プロセス共通のアトミックカウンタを増減する | `hook.cpp:60-66`、`hookCounter.h:10-16` | 小 | 中 | 中 | 通る |
| P-19 | DirectWrite=1 かつ HintingMode=1 のとき、Direct2D の描画フックが毎回、変換行列を読む → 微小な傾きを足して設定 → 描画 → 元に戻す。**調べた結果、候補にしない（見送り）** | `directwrite.cpp:227, 1697-2107` | 小（推測） | 高（外すと描画が変わりうる） | 高（処理があること） | 通る |
| P-20 | UseMapping=0（既定）のとき、FreeType がフォントファイルを読むたびに、GDI の `GetFontData` を 1 回呼ぶ。グリフ 1 個の読み込みで 3 回以上。しかもフック済みの方を呼んでいる（a / b。3.2 に内訳） | `fteng.cpp:1016-1041`、`override.cpp:1731-1732`、`ft.cpp:2123-2135` | 小〜中（推測） | a: 低、b: 中 | 高 | 通る |

### 3.2 各項目の説明

#### P-01 `conv2` の割り算

- 確認済み:
  - `conv2` は `tbl2[n / (BASE * BASE / (tbl2.size() - 1))]`（`ft.cpp:208-210`）。`tbl2.size()` は実行時の値なので、コンパイラは割り算を消せない。`CAlphaBlendColorOne::doAB`（`ft.cpp:388-395`）から、文字のある画素の R / G / B それぞれで呼ばれる。
  - `tbl2` は 4097 要素（`ft.cpp:185`）、`BASE` は 0x4000（`ft.cpp:217`）なので、割る数は常に 65536。`n` は 0 以上 `BASE * BASE` 以下。よって `n >> 16` と完全に同じ値になる。0 から `BASE * BASE` までの全値で一致することをプログラムで確認した（不一致 0 件）。
  - 再現ベンチ（合成ループを写したもの。Linux、g++ -O2）: 割り算版 約 13 ns/画素、シフト版 約 2.8 ns/画素。約 4.5 倍。
- 推測: MSVC でも差は出る見込み。ただし割り算は `size_t` で行われるので、x64 は 64 ビットの割り算、Win32 は 32 ビットの割り算になる。Win32 の方が差は小さいはず（レビューの指摘。同意）。描画全体に占める割合は未計測。文字の画素数に比例する部分なので、文字の多い画面ほど効く。影響は、初版では「大」と見込んでいた（下の「状態」のとおり、T-01 の結果を受けて「小」に下げた）。
- 描画結果: 変わらない（同値）。
- レビューの補足（コードで確かめて同意）:
  - `conv2` を実際に呼ぶのは、`CAlphaBlendColorOne::doAB`（`ft.cpp:392-393`）だけ。もう 1 つの呼び出し元 `CAlphaBlend::doAB`（`ft.cpp:450-460`）は、`DoAlphaBlend` → `AlphaBlendColorLCD` / `AlphaBlendColor`（`ft.cpp:462-522`）からしか呼ばれず、この 2 つはどこからも呼ばれていない。
  - `n` が 0〜`BASE * BASE` に収まるのは、設定値が正常な範囲のとき。INI から読む値は範囲を絞っている（`settings.cpp:602-604`）が、`SetFloatAttribute`（`settings.h:816-832`）は範囲を見ない。また、補正表を作るときの `Bound(0, ..., BASE)`（`ft.cpp:329-341`）は、引数の順が (値, 最小, 最大)（`common.h:373`）なので、実際には「0 以上にする」だけで、上限は効いていない。異常な値を入れると、今のコードでも表の範囲外を読む。
  - レビューは、設定値 400 通り × 全組み合わせ（約 57.6 億回）で不一致 0 件を確かめている（レビュー付録 A。こちらでは再実行していない）。
- 書き方（レビューの案を 1 点だけ変える）: レビューの案は `tbl2[(unsigned)n >> 16]`。範囲内の `n` なら今と同じ。ただし x64 では、今の割り算は `n` を 64 ビットの符号なしに広げてから行うので、`n` が負になった場合（異常な設定のとき）の添字まで今と同じにするなら、`tbl2[(size_t)n >> 16]` の方がよい。これなら Win32 でも x64 でも、すべての `n` で今と同じ添字になる。割る数が 65536 であることは、表のサイズを定数にして `static_assert` で固定する（レビューの案どおり）。
- **状態（2026-11-17）**: 実装済み。ブランチ `perf/p01-conv2-shift`（コミット `bac1ad7`。`directwrite` の `f50cdba` から切った）。`directwrite` にマージ済み（早送り。`directwrite` は `070eb32`）。
  - 変更: `ft.cpp` だけ、+8 行 / −2 行。`conv2()` を `tbl2[(size_t)n >> CONV2_SHIFT]` にした。表の最後の添字を定数 `TBL2_LAST`（4096）にして、`static_assert` で `BASE * BASE / TBL2_LAST == 1 << CONV2_SHIFT`（= 65536）を固定した。`BASE` か表の大きさを変えて前提が崩れると、コンパイルが止まる。表の大きさ（4097 要素）と中身は変えていない。
  - 書き方: 上の「書き方」のとおり `(size_t)n >> 16`。確認済み（Linux / g++ -O2。Windows での確認ではない）: `int` の全 2^32 通りを、32 ビットの `size_t`（Win32）と 64 ビットの `size_t`（x64）の両方で、元の式と比べた。`(size_t)n >> 16` は、どちらも不一致 0。レビューの案 `(unsigned)n >> 16` は、32 ビットでは不一致 0、64 ビットでは負の `n` の 2^31 通りすべてが不一致。Win32 では `size_t` と `unsigned` は同じ 32 ビットの符号なしなので、レビューの言う「Win32 の今の動作とすべての値で同じ」はそのまま成り立ち、x64 でも成り立つ。負の `n` は異常な設定のときだけで、そのときは修正の前も後も、同じ添字で表の範囲外を読む。
  - ビルド（確認済み）: `Build MacType` 成功（run 44010921532。Win32 / x64）。T-01 の中のビルドで見たかぎり、警告の種類と数は base と同じ。
  - 画素（確認済み）: `T-01 render check` 成功（run 44010921492）。単スレッドの 553 ケースすべてで、base（`f50cdba`）と head のハッシュが同じ。Win32 / x64 とも。DLL は base と別物になっている（Win32 で 1,024 バイト、x64 で 512 バイト小さい）。
  - 複数スレッドのモード: base も head も落ちた（S-01。改造前からの問題なので、警告のまま）。Win32 の head が落ちた場所は `ft.cpp:1872` で、改造前の `ft.cpp:1866` と同じ行（下の行番号のずれ）。P-01 で良くも悪くもなっていない。
  - 速度（確認済み。T-01 の計測値。描画部分だけの時間、base と head を交互に 3 回ずつ動かした中央値、ミリ秒）:

    | | base | head | head / base |
    |---|---:|---:|---:|
    | Win32 1 周目（キャッシュが空） | 1,976.4 | 1,978.9 | 1.001 |
    | Win32 2 周目以降（1 周あたり） | 524.87 | 506.09 | 0.964 |
    | x64 1 周目（キャッシュが空） | 2,155.6 | 2,150.2 | 0.997 |
    | x64 2 周目以降（1 周あたり） | 609.34 | 602.46 | 0.989 |

    **4 つとも、差は ±6% の揺れより小さい。T-01 では、速くなったとも遅くなったとも言えない。** 3 回の生の値も重なっている（Win32 の 2 周目以降: base 571.4 / 524.9 / 509.0、head 502.7 / 511.8 / 506.1。x64: base 607.6 / 644.2 / 609.3、head 610.0 / 601.8 / 602.5）。ほぼ同じ時刻に走った `directwrite` 自身の T-01（base と head が同じソース、run 44010715845）は、head / base が 0.996〜1.012。こちらは別のランナーで、絶対値が 4〜6 割ほど遅かった（2 周目以降が約 840〜850 ミリ秒）。違う実行どうしの絶対値は比べられない。
  - 読み取り（推測）: 3.1 の影響「大」は、合成ループだけを取り出したベンチ（約 4.5 倍）からの見込みだった。T-01 の 1 ケース（約 1 ミリ秒）の大半は、合成以外（GDI への問い合わせ、`BitBlt`、ロック、ヒープ確保など）と思われ、割り算の分は揺れに埋もれている。`ExtTextOutW` 全体への効きは、あっても数 % まで（T-01 で言えるのはここまで）。この結果を受けて、3.1 の「影響」を「大」から「小」に下げた（けんじさんの決定、2026-11-17）。合成ループそのものが速くなったことは変わらないが、描画 1 回の時間としては見えない、という意味。
  - 行番号のずれ: このブランチでは、`ft.cpp` の元の 182 行目以降が +2、210 行目以降が +4、218 行目以降が +6。この文書の `ft.cpp` の行番号は `693ca92` 時点のまま。

#### P-02 空白ごとの GDI 呼び出し

- 確認済み: `TextOutCallback` は、ビットマップのバッファが NULL のグリフに対して、元の `ExtTextOutW` を 1 文字分呼ぶ（`ft.cpp:2702-2707`）。FreeType は、輪郭が空のグリフ（空白など）ではバッファを確保しない（freetype の `src/smooth/ftsmooth.c:635-636` で確認）。空白はキャッシュに「空のグリフ」として入るので、文字列中の空白 1 個ごとに、この GDI 呼び出しが起きる。
- 推測: 欧文では文字の 15〜20% が空白なので、1 回の描画に占める割合が大きい。日本語では小さい。
- 描画結果を変えないための条件（要確認）: 呼び出し先は背景透明モード（`override.cpp:1615`）なので、空白なら GDI は何も描かないはず。ただし下線・取り消し線つきフォントでは GDI が線を引く。省略するのは「グリフ番号指定でない」「文字が空白」「下線も取り消し線もない」ときに限るのが安全。実機で画素を比べてから採用する。

#### P-03 / P-04 設定取得のロック

- 確認済み: `GetInstance()` は毎回 `CS_SETTING` を取る（`settings.cpp:138`）。`ExtTextOutW` 1 回の中で、`override.cpp:1144`、`ft.cpp:1431, 1755, 2851, 3052` の 5 回に加えて、グリフごとに 1 回（`ft.cpp:2712`）呼ぶ。キャッシュにない文字を太らせるときは、さらに `ft.cpp:3229` でも呼ぶ。
- P-03: `ft.cpp:2712` の分は、ループの外（`FreeTypeTextOut`、`ft.cpp:2851`）で取ったものを渡せば済む。動作は変わらない。`TextOutCallback` を呼ぶのは `ft.cpp:2945` のループだけ（レビューでも確認）。渡し方は、グリフごとの情報 `FreeTypeGlyphInfo`（`ft.cpp:524-533`）に入れるのが差分が小さい。
- P-04: 初期化済みならロックなしで返す。ただし初期化済みフラグ `m_bDelayedInit` はビットフィールド（`settings.h:271`）で、同じバイトのほかのフラグと一緒に書き換えられる。再読み込みのたびにこのフラグを戻して、遅延初期化をやり直す作りでもある（`settings.h:986, 1007`）。S-11 との関係も整理が必要。
- 推測: ロックが競合しない限り、1 回あたりは数十ナノ秒。複数スレッドで描くと、全スレッドがここで直列になる。

#### P-05 キャッシュヒット時の `CS_LIBRARY`

- 確認済み: `FT_Glyph_Ref_Copy` は `InterlockedIncrement` でカウントを増やすだけ（`ftref.c:7-16`）。それをプロセス全体で 1 つの `CS_LIBRARY` で囲んでいる（`ft.cpp:1865-1866`）。`CS_LIBRARY` はラスタライズ中（`ft.cpp:2185-2199`）も握られるので、別スレッドが新しい文字をラスタライズしている間、キャッシュヒットだけのスレッドも待たされる。
- 補足（レビューの指摘。同意）: 今のこの `CS_LIBRARY` は、実質なにも守っていない。グリフを手放す側（`SetGlyph` の `fteng.cpp:220`、デストラクタの `fteng.cpp:203-208`）が `CS_LIBRARY` を取らないため。設定反映は `CS_LIBRARY` の中でキャッシュを消す（`fteng.h:623, 640`）が、読み手は `CS_LIBRARY` を取る前に項目を手に入れているので、これも防げていない。
- S-01 の修正で「検索〜参照コピー」を `CS_FONTCACHE` で囲めば、ここは不要になる。S-01 と同じ設計の中で扱う（ブランチは分ける）。

#### P-06 ヒープ確保の多さ

- 確認済み（箇所）: 送り幅配列（`override.cpp:1158`）、グリフ配列と状態配列（`1262-1263`）、`OUTLINETEXTMETRIC`（`1294`）、ファミリー名（`1299, 1302`）、`FREETYPE_PARAMS` の文字列 3 本と `MakeUniqueFontName`（`ft.h:223-229`、`expfunc.cpp:199-202`。引数が値渡しなのでコピー 3 回＋連結）、`Dx` / `Dy` / `AAModes`（`ft.h:336-338`）、グリフ番号配列（`ft.cpp:1798`）、合成器 2 個（`ft.cpp:2880-2881`）、除外フォント検索の一時文字列（`settings.cpp:1058`）。
- 直し方の方向: 短い文字列用のスタックバッファ、参照渡し、スタック上のオブジェクトへの置き換え。1 箇所ずつ別ブランチにできる。動作は変わらない。
- 根本的には、`HFONT` ごとにフォント情報を覚えておけば、名前の組み立てと検索（`fteng.cpp:758-773`）ごと省ける。ただし `HFONT` の値は再利用されるので、無効化の設計が要る。大物として保留。

#### P-07 `GetOutlineTextMetrics` の 2 回呼び出し

- 確認済み: サイズを聞く呼び出し（`override.cpp:1288`）と、確保して取得する呼び出し（`1300`）が、毎回ある。
- 直し方の方向: スタック上のバッファで先に 1 回試し、足りなければ今の手順に戻る。
- 未確認: バッファが足りないときの戻り値の挙動（実機で確かめる必要がある）。

#### P-08 グリフキャッシュの間引き

- 確認済み: 追加が 2048 回に達すると `CompactMap` が走り、`std::map` の先頭（キーの小さい方）から、1024 個になるまで捨てる（`fteng.cpp:123-136, 285-287`）。キーは文字コード（正）または −グリフ番号（負）（`fteng.h:228, 237`）。つまり、グリフ番号の大きいものから先に捨てられ、次に文字コードの小さいもの（ASCII）が捨てられる。使用頻度は見ない。MRU カウンタ（`fteng.h:125-140`）は検索のたびに更新されるが、今のビルド（`_USE_ARRAY` 未定義）の間引きでは使われない。
- 推測: 1 つのフォントサイズで 1024 種類を超える文字を使う場面（日本語の長文）では、よく使う漢字でも、グリフ番号が大きければ毎回捨てられ、再ラスタライズになる。
- 直し方の方向: 捨てる対象を「最近使っていない順」にする。描画結果は変わらない（キャッシュの当たり方だけ変わる）。
- 関連（メモリ）: キャッシュ全体のバイト数に上限はない。使用量の集計（`fteng.h:599-608`）は、制限に使われていない。

#### P-09 `DeleteObject` フック

- 確認済み: `IMPL_DeleteObject` は、削除が成功するたびに `DeleteCachedFont` を呼ぶ。中で `CS_CACHEDFONT` を取り、`FontCache` を検索する（`override.cpp:766, 428-434`）。ペン、ブラシ、ビットマップなど、すべての GDI オブジェクトの削除で通る。`FontCache` に追加するのはフォント作成フックだけ（`override.cpp:753`）で、FontSubstitutes が無効なら、このフックは設置されない（`hook.cpp:601-603`）。
- 直し方の方向: 表の個数をアトミックに持ち、0 ならロックも検索もしない。

#### P-10 `ControlIder` の初期化

- 確認済み: コンストラクタが `iswcntrl` を 0〜0x2FFF と 0xA000〜0xFFFE の計 36,863 回呼ぶ（`ft.h:151-154`）。グローバル変数なので、DLL が読み込まれた時点で必ず走る（`ft.cpp:62`）。除外・アンロード対象の判定（`hook.cpp:563-569`）より前。
- 推測: MSVC の `iswcntrl` は、256 以上の文字では 1 文字ごとに Windows の文字種 API を呼ぶ実装のはずで、合計で 1〜数ミリ秒かかる見込み（未計測）。全プロセスの起動に乗る。
- 直し方の方向: 最初に使うとき（最初の `ExtTextOutW`）まで遅らせる。表の中身は同じになる。Win32 ビルドは関数内 static がスレッド安全でないので、既存のロックか、Vista 以降の `InitOnceExecuteOnce` で守る。
- けんじさんの設定では、`[UnloadDll]` の 24 プロセスにも効く（1.7）。対象のプロセスは、この初期化をした直後に DLL を手放すので、まるごと無駄になっている。

#### P-11 `d2d1.dll` / `dwrite.dll` の読み込み

- 確認済み: `HookD2DDll` は、どちらの DLL も未ロードなら `LoadLibrary` する（`directwrite.cpp:1500-1506`）。条件は「対話ユーザーのプロセス かつ INI の `DirectWrite` が有効」（`hook.cpp:595`）だけで、そのプロセスが D2D を使うかどうかは見ない。`DirectWrite` の既定値は 0（`settings.cpp:631`）なので、INI で有効にしていなければ、この項目は関係ない。
- 推測: コンソールアプリや、短時間で終わるツールでも 2 つの DLL の読み込みが乗り、起動時間とメモリが増える。
- 直し方の方向: すでにロード済みなら今すぐフック。未ロードなら、DLL のロード通知（`LdrRegisterDllNotification`、Vista 以降）を受けて、その時点でフックする。フックの中身は変えない。ローダーロック中の `LoadLibrary` がなくなるので、S-13 にも効く。通知の登録・解除が要る。Vista 以降の API を使ってよいことになったので、古い OS 向けの代替は不要になり、難しさは「中」に下げた。通知が呼ばれる時点（ローダーロックの中）でフックを張って問題ないかは、実機で確かめる必要がある。
- けんじさんの設定（DirectWrite=1）では、対話ユーザーの全プロセス（`[UnloadDll]` と除外を除く）でこの読み込みが起きている。優先度は上がる。

#### P-12 / P-13 起動時の小さな無駄

- P-12: `IsRunAsUser() && bEnableDW && ...` の順（`hook.cpp:595`）を入れ替えるだけで、DirectWrite 設定が無効のときのトークン取得（`hook.cpp:279-409`）を省ける。けんじさんの設定（DirectWrite=1）では、入れ替えても `IsRunAsUser()` は呼ばれるので、効果はない。
- P-13: `undocAPI.h` の `static` 変数は、取り込む .cpp ごとに別々の実体になる。それぞれが `LoadLibrary("gdi32.dll")` を 4 回と `GetProcAddress` を実行する（`undocAPI.h:72, 91-92, 96`）。1 箇所の定義にまとめれば 1 回で済む。

#### P-14〜P-18

- P-14 確認済み: `GetFontLocalName` は、見つかったときだけキャッシュする（`fteng.cpp:51-52`）。見つからない名前は、毎回 `GetDC` → `EnumFontFamiliesEx` → `ReleaseDC`（`fteng.cpp:47-50`）。`lookup` 側の予備キャッシュのキーは、`LOGFONT` の先頭 28 バイト（数値部分）だけから作られ、フォント名を含まない（`settings.cpp:1722-1730, 1755-1756`）。ここを直すと置換の結果が変わりうるので、触るのは「見つからなかった結果のキャッシュ」だけにする。
- P-15: 遅延初期化の中身を「使うときに作る」形へ分ければ、最初の描画の待ちが減る。ただし初期化順に依存する箇所が多く（da2d7f1 の無限ループ修正など）、難しい。
- P-16: ClipBoxFix（既定で有効、`settings.cpp:662`）のために、寸法だけの `GetGlyphOutline` のたびに `GetTextMetrics` を呼ぶ。キャッシュするには「同じ DC・同じフォント」の判定が要る。
- P-17 確認済み: 256 回目の呼び出しで、要求サイズが今の DIB と違えば、要求サイズちょうどで作り直す（`cache.cpp:22-33`）。次に大きい要求が来ると、また作り直す（`cache.cpp:34-41`）。
- P-18: 効果は小さい見込み。アンロードの安全待ち（S-14）に使っているので、外すなら代わりの仕組みが要る。

#### P-19 Direct2D の描画フックの「微小な傾き」（調べた結果: 見送り）

けんじさんの質問「DirectWrite=1 かつ HintingMode=1 のとき、描画のたびに変換行列へ微小な回転を足す処理は、高速化候補になるか」への答え。**今の方針では、候補にしない。**

- 確認済み（何をしているか）:
  - HintingMode=1 だと、DirectWrite 用パラメータのグリッドフィットが「無効」になる（`directwrite.cpp:223-232`）。
  - このとき、Direct2D の描画フック 9 個（`DrawGlyphRun` 6 種、`DrawText` 2 種、`DrawTextLayout`。`directwrite.cpp:1697-1915, 1986-2107`）は、呼ばれるたびに次をする。今の変換行列を読む（`GetTransform`）→ `m12` と `m21` に 1/65535 を足して設定する（`SetTransform`）→ 元の描画を呼ぶ → 行列を元に戻す（`SetTransform`）。例: `directwrite.cpp:1705-1722`。
  - 厳密には回転ではない。`m12` と `m21` に同じ符号の値を足すので、ごくわずかな「斜めの歪み」になる（変数名は `rotate`）。行列を軸に平行でなくして、DirectWrite にグリッドフィット（ヒンティング）をさせないための仕掛けと思われる（推測。グリッドフィットを直接指定できない、古い Windows 向けの手）。
  - GDI 連携のビットマップ描画（`IDWriteBitmapRenderTarget::DrawGlyphRun`、`directwrite.cpp:1916-1983`）も、同じことを `GetCurrentTransform` / `SetCurrentTransform` で行う。
  - グリフ解析の作成（`CreateGlyphRunAnalysis` 3 種）は違う。Windows 8.1 以降の版では、グリッドフィット無効を引数でそのまま渡し（`directwrite.cpp:737-751, 821-832`）、傾きを足すのは、それが失敗したときの予備だけ（`directwrite.cpp:752-765, 834-848`）。いちばん古い版は、新しい版が使えるなら先にそちらへ回す（`directwrite.cpp:545-577`）。
- 確認済み（MacType 側の重さ）: 描画呼び出し 1 回につき、COM の仮想呼び出しが 3 回（読む 1 回、設定 2 回）と、24 バイトの行列のコピー。ロックもヒープ確保もない。文字ごとではなく、グリフの並び（ラン）ごと。
- 推測（Direct2D 側の重さ）: Direct2D の中身は非公開なので、コードからは確かめられない。変換行列を切り替えると、Direct2D の内部で何か（描画をまとめる処理の区切りなど）が起きる可能性はある。ただし足す値は毎回同じなので、Direct2D が持つグリフのキャッシュの当たり方は変わらないはず。
- 候補にしない理由:
  1. MacType 側で削れる処理がない。「読む → 変える → 戻す」は、この仕掛けの最小の形。
  2. 仕掛けそのものを外すと、**描画が変わりうる**。Windows 8.1 以降は、レンダリングパラメータでもグリッドフィット無効を渡している（`directwrite.cpp:128-162, 525-528`）ので、ヒンティングを止める役目は重複しているかもしれない。しかし傾きは、文字の位置と形にも掛かる（2000 ピクセル先で約 0.03 ピクセルずれる）。けんじさんの設定（`[DirectWrite]` RenderingMode=6 = 輪郭で描くモード）では、輪郭にそのまま掛かる。作業ルール 3 に当たる。
  3. 外して速くなるかどうかが、実機で測らないとわからない。
- もし将来やるなら: 実機で、(1) Direct2D で文字を多く描くアプリの描画時間を、傾きあり・なしで測る、(2) 画面の画素を比べる、の 2 つをしてから、けんじさんと相談する。T-01（GDI の描画比較）では、この経路は確かめられない。
- 未確認（実機でしかわからない）: `DrawText` / `DrawTextLayout` のフックの中で、Direct2D が内部から `DrawGlyphRun` の実装を呼んでいるなら、フックが入れ子で走り、傾きが 2 回足される（フックは関数本体に張っているので、内部からの呼び出しも拾う）。起きていたとしても、直すと描画が変わるので、今は触らない。

#### P-20 フォントファイルの読み取りが、1 回ごとに `GetFontData`（今回の調査で追加）

- 確認済み:
  - UseMapping=0（既定、`settings.cpp:599`）のとき、FreeType に渡している読み取り関数 `IoFunc` は、呼ばれるたびに GDI の `GetFontData` を 1 回呼ぶ（`fteng.cpp:1016-1041`）。読んだ内容をためておく仕組みはない。
  - FreeType は、読み取り関数つきのストリームでは、2 バイトの値 1 個を読むのにも読み取り関数を 1 回呼ぶ（freetype の `src/base/ftstream.c:501-515`）。位置を動かすだけのときは長さ 0 で呼び（`src/base/ftstream.c:56-64`）、`IoFunc` はすぐ戻る（`fteng.cpp:1022-1024`）。
  - TrueType のグリフ 1 個を読み込むとき、少なくとも、送り幅の表から 2 バイトを 2 回（`src/sfnt/ttmtx.c:274-276`）、グリフ本体を 1 回（`src/truetype/ttgload.c:292`）読む。つまり `GetFontData` が 3 回以上。グリフの位置の表は、フォントを開いたときにメモリへ読み込み済み（`src/truetype/ttpload.c:172`）。
  - 読みに行くのは、FreeType の輪郭キャッシュ（`ft.cpp:2123-2135`）にないグリフだけ。MacType 自身のグリフキャッシュに当たれば、そこまで行かない。
  - `IoFunc` が呼んでいるのは `ORIG_GetFontData` ではなく、フック済みの `GetFontData`（`fteng.cpp:1034`）。フック本体は、`name` テーブル以外ならすぐ元の関数へ回す（`override.cpp:1731-1732`）ので、結果は同じ。ただし、フックの包み（呼び出し中カウンタの増減、`hook.cpp:60-66`）を毎回通る。CLAUDE.md の「フック関数の中から、フック対象の API をそのまま呼ばない」にも合っていない。同じファイルの別の箇所は `ORIG_GetFontData` を使っている（`fteng.cpp:923, 940`）。
- 推測: `GetFontData` はカーネルまで行く呼び出しなので、1 回あたりは軽くない。効くのは、初めて出る文字を描くとき（ウィンドウを開いた直後、日本語の長い文章など）。全体に占める割合は未計測。
- 枝番:
  - a: `IoFunc` の `GetFontData` を `ORIG_GetFontData` にする。1 行。ここで読むのは `name` テーブルではないので、結果は同じ。
  - b: `IoFunc` に小さな読み取りバッファを持たせ、近い位置の読み取りをまとめる。フォントファイルの中身は変わらないので、描画結果は同じ。フェイスごとのメモリと、読み取りがどのロックの下で起きるか（`CS_MANAGER` だけとは限らない）の確認が要る。
- 設定での回避（参考）: UseMapping=1 にすると、フォント全体をメモリに写すので、この読み取りはなくなる（`fteng.cpp:930-942, 1030-1032`）。ただし、使うフォントごと・プロセスごとにファイル全体の写しを持つ。日本語フォントではメモリが大きく増えるので、すすめない。


---

## 4. 着手順の提案

「1 ブランチ 1 修正」なので、上の ID 1 つ（枝番があるものは枝番 1 つ）を 1 ブランチにする。

**2026-11-17 の見直しで変えた点**

- T-01 を最初にする順番は、変えていない（けんじさんの指示）。
- S-01 を、段階 2 の 4 番目（全体の 9 番目）から、S-03 / S-04 のすぐ後へ上げた。レビューの提案を採用した（検討の中身は、この章の最後）。
- けんじさんの設定で通らない項目（S-07、S-08、S-21、P-14、S-15、S-18 b / e）と、効果のない P-12 を、番号つきの列から外して後ろへ回した。
- DirectWrite=1 で実際に通る S-09 は、段階 2 に残した。P-11 / S-13 は段階 4 の先頭に置いた（Vista 以降の API を使えるので、難しさを「中」に下げた）。
- 新しい項目を入れた。P-20 a は段階 1、P-20 b は段階 3、S-20・S-22 は段階 4、S-23 は残り。P-19 は見送り。

**2026-11-17 の 2 回目の見直しで変えた点（P-01 と T-01 の結果を受けて。けんじさんの決定）**

- S-01 を、6 番目から、P-01 のすぐ次（2 番目）へ上げた。続きの P-05 も 3 番目へ上げた。段階 2 から段階 1 へ移した。
- P-03、S-03、S-04 は、その後（4〜6 番目）にした。並びは変えていない。
- 8 番目以降（S-02、S-10、S-09、…）の番号と並びは、変えていない。
- P-01 の影響を「大」から「小」に下げた（3.1、3.2）。
- 理由は、この章の最後（「S-01 をさらに前に出した件」）。

**段階 0: 確かめる手段を用意する（提案）**

- **T-01（最初にやる）**: 「描画結果は変えない」を、目視ではなく機械的に確かめたい。案: 小さなテスト用 EXE を Actions で一緒にビルドする。EXE は `MacType64.Core.dll` を直接 `LoadLibrary` し、決まった文字列をメモリ DC に `ExtTextOutW` で描き、画素のハッシュを出力する。修正前後のハッシュが同じなら、描画は変わっていない。同じ EXE で繰り返し回数を増やせば、速度の比較にも使える。
  - 推測: Core DLL は、ローダーなしで直接読み込んでもフックを張れる（`DllMain` がすべて自前で行うため）。Actions のランナー上で動くかどうかは未確認。
  - テスト用の INI は、けんじさんの `Default.ini` に合わせる（AntiAliasMode=4、HintingMode=1、FontLoader=0、FontLink=2 など）。普段と同じ経路を比べるため。
  - 追加の提案: 「複数のスレッドから、同じフォント・同じ文字列を同時に描く」モードを付ける。S-01、S-02、S-03 は、1 スレッドの画素比較では確かめられない。修正の前後でこのモードを長めに回し、落ちないことと、画素が同じことを見る。
  - T-01 で確かめられないもの: DirectWrite / Direct2D 側（S-09、S-23、P-11、P-19）と、実際のアプリでの動き。
  - **状態（2026-11-17）**: ブランチ `test/t01-render-check`。`tools/t01/`（`t01.cpp`、`run-t01.ps1`、`Default.ini`）と `.github/workflows/t01.yml` を追加。Actions の `T-01 render check` が Win32 / x64 とも成功（push の run 44007924291、フォントを差し替えた手動実行 44007934799）。`Build MacType` も成功。本体のコードは変えていない。
    - 仕組み: base（既定は `directwrite`）と head の Core DLL を、同じ依存ライブラリで同じランナー上にビルドし、それぞれ別プロセスで t01.exe に読ませる。`MacType.ini` は `AlternativeFile=` で `Default.ini`（けんじさんの添付とバイト単位で同じ）を指す。単スレッドのハッシュが base と head で 1 ケースでも違えば、ジョブを失敗させる。結果は Artifacts `T01-<Win32|x64>-<SHA>`。
    - ケース: Segoe UI / Arial / Times New Roman / Consolas / Yu Gothic / MS Gothic × 標準・太字・斜体 × 11〜32px の 5 段階 × 英語・記号・日本語 × 白地に黒・模様の上に色文字。ほかに送り幅つき、拡大した DC、幅指定フォント、漢字 2400 字（グリフキャッシュの間引きを起こす）。計 553 ケース。
    - 確かめたこと（Actions 上）: ① `ExtTextOutW` の先頭が Detours の `jmp` に書き換わる。② フォント自身が全文字を持つ 430 ケースすべてで、素の GDI と画素が違う。白地に黒の 220 ケースすべてに LCD の色にじみが出る（AntiAliasMode=4 が効いている = INI が読めている）。③ 2〜5 周目（キャッシュに載った状態）も 1 周目と同じ画素。④ base と head（同じソース）で 553 ケースすべて同じハッシュ。Win32 と x64 も同じハッシュだった。⑤ `IsRunAsUser` は真で、`dwrite.dll` / `d2d1.dll` が読み込まれ `DWriteCreateFactory` の先頭も `jmp` になる（DirectWrite のフックが張られる経路は通る。描画結果は見ていない）。
    - ランナーの日本語フォント: windows-2022 には Yu Gothic と MS Gothic がある（Meiryo、MS Mincho はない）。なければ、google/fonts の固定コミットから M PLUS 1p（SIL OFL 1.1）を取り、SHA-256 を確かめて `AddFontResourceEx(FR_PRIVATE)` で読ませる。手動実行の `force_font_fallback` で、この経路も通ることを確かめた（643 ケース、すべて MacType が効いた）。
    - **複数スレッドのモードで、改造前のコードが落ちる（S-01 を Windows 上で再現）**: 8 スレッドが同じケースを同時に描くと、アクセス違反かヒープ破壊（0xC0000374）で落ちる。これまでの 12 回（3 回の実行 × Win32 / x64 × base / head）すべてで落ちた。落ちた場所は PDB で確かめた: `ft.cpp:1866`（キャッシュヒット側の `FT_Glyph_Ref_Copy`。NULL グリフを読む）と `fteng.cpp:220`（`SetGlyph` の差し替え。`AddCharData` の `fteng.cpp:303` から）。S-01 の壊れ方 2 の 2 か所そのもの。今は base も head も落ちるので、ワークフローは「base でも落ちた = 改造前からの問題」として警告にとどめている。base が通って head だけ落ちたら失敗にする。S-01 を直したブランチでは、head が通るはず。
    - フォントリンクのケース: Arial、Times New Roman、Consolas はランナーのレジストリに SystemLink がなく、日本語を描くと MacType が素の GDI に任せるケースがある（123 ケース中 44）。これは失敗にせず、数だけ出す。
    - 速度: 描画部分（`ExtTextOutW` と `GdiFlush`）だけを足した時間を、base / head 交互に 3 回ずつ測って中央値を出す。同じソースどうしでも ±6% ほど揺れたので、それより小さい差は判断に使えない。
    - T-01 で確かめられないこと（変わらず）: DirectWrite / Direct2D の描画結果（S-09、S-23、P-11、P-19）、GDI の `ExtTextOutW` 以外の入口（`TextOutW`、`ExtTextOutA`、`DrawText` 経由など。後者は内部で `ExtTextOutW` を通ると思われるが未確認）、設定の再読み込み（S-11、S-20、S-22）、フォント削除（S-17）、`Default.ini` の `[Individual]` が効く経路（ランナーに MS Mincho などがない）、FontSubstitutes を使う経路、けんじさんの PC との差（DPI、フォントの版、ClearType 設定、日本語版 Windows の SystemLink）、実際のアプリでの動き。
- **T-02（提案）**: 実機での試し方を、1 回決める。けんじさんは公式版を普段使いしていて、改造した DLL はまだ試していない。決めること: (1) 普段使いの公式版を壊さずに、改造版をどう当てるか、(2) どのアプリで見るか、(3) 元に戻す手順。Core だけ差し替えて動くかは未確認（7 章）。段階 1 の最初の 1 本（P-01）のビルドができた時点で、一緒に試して固めるのがよい。

**段階 1: 最初の 1 本、グリフキャッシュの排他、そのあと小さい修正**

1. P-01 `conv2` をシフトに（**済み**。`directwrite` にマージ済み。画素は同じ。速度の差は T-01 では見えなかった）
2. S-01 グリフキャッシュのロック（`CS_FONTCACHE` を使う。T-01 の複数スレッドのモードで「base は落ちる、head は落ちない」を確かめる）
3. P-05 キャッシュヒット時の `CS_LIBRARY` を外す（S-01 と同じ設計の続き。S-01 だけだと、キャッシュヒット時のロックが一時的に 2 回になる。速度は P-05 の後で見る）
4. P-03 グリフごとの `GetInstance()` をループの外へ
5. S-03 TLS 一覧と `TLSDCArray` にロック
6. S-04 `DCArray` にロック（空ならロックを取らない形）
7. P-09、P-20 a、S-19 b、S-18 のうち単純なもの（a、g、h）

**段階 2: 効果の大きい安定性修正（残り）**

8. S-02 所有者つきロックの修正
9. S-10 可変フォントの名前比較（(b) 例外 → (c) リーク → (a) 読み越しの順。Windows 11 で日常的に通る経路）
10. S-09 `HOOK` マクロの直列化（DirectWrite=1 なので通る。確認は実機）
11. S-05、S-06

**段階 3: 効果の大きい高速化**

12. P-02 空白の GDI 呼び出し省略（画素比較ができてから）
13. P-08 間引きを使用頻度順に
14. P-10 `ControlIder` の遅延初期化（`[UnloadDll]` のプロセスにも効く）
15. P-20 b フォントファイルの読み取りをまとめる
16. P-06 を 1 箇所ずつ、P-07（実機で戻り値を確かめてから）

**段階 4: 大きめの変更（設計してから）**

17. P-11 / S-13 `d2d1` / `dwrite` の遅延フック（DirectWrite=1 なので、ほぼ全プロセスに効く。T-02 が決まってから）
18. P-04 `GetInstance()` のロックなし経路
19. S-11 設定再読み込みの排他
20. S-22 所有者つきロックの飢餓（S-02 の後）、S-20 フォント削除と設定反映の排他
21. 残り（S-12、S-14、S-16、S-17、S-18 c / d / f、S-19 a / c / d、S-23、P-13、P-15〜P-18）

**けんじさんの設定では通らないので、後回しにしたもの**: S-07、S-08、S-21、P-14、S-15、S-18 b / e。効果のない P-12 も同じ扱い。自分の PC では直った効果を確かめられない。upstream に出す価値は残る。

**見送り**: P-19（Direct2D の微小な傾き）。

upstream に Issue / PR を出すなら、再現条件を説明しやすく差分も小さい S-10、S-09、S-08、S-03、S-04、P-01 が候補。S-01 も、ロックの範囲が小さくまとまれば候補になる。出すかどうかは、けんじさんが決める。

**S-01 を前に出す件の検討（けんじさんの依頼）**

- 上げる理由:
  - 壊れ方 2（同じ文字の上書き）は、新しい文字が出るたびに機会がある。2 章の中で、起きたときの重さ × 起きやすさがいちばん大きい。
  - 使うロック（`CS_FONTCACHE`）と取得順が決まり、「先に設計が要る」という保留の理由がなくなった。
  - 続けて P-05 が片づく（ロックの回数は増えず、ラスタライズ待ちが減る）。
- 上げすぎない理由:
  - すべての描画が通る場所なので、確かめる手段（T-01 と、その複数スレッドのモード）が先に要る。
  - S-03 / S-04 は、同じ「ロックを足す」修正の小さい版。先にやって、ブランチ → Actions → 確認の流れを固めておく方が、S-01 で失敗しにくい。
- 結論: レビューの提案どおり、S-03 / S-04 の次（6 番目）に置く。初版で先にしていた S-10、S-09、S-08 より前にした。（→ その後、さらに前へ出した。すぐ下）

**S-01 をさらに前に出した件（2026-11-17。P-01 と T-01 の結果を受けて）**

- わかったこと（確認済み）: T-01 の複数スレッドのモードで、改造前のコードが S-01 の 2 か所で毎回落ちる（通算 20 回中 20 回。段階 0 の T-01）。
- S-01 を P-01 のすぐ次にした理由:
  - 今の T-01 は、S-03 / S-04 を確かめられない。base が S-01 で必ず落ちるので、head が複数スレッドで落ちても固まっても、「改造前からの問題」として警告で通る。S-03 / S-04 でデッドロックを入れてしまっても、Actions は成功のままになる。
  - S-01 を直して base が通るようになれば、「base は通る、head だけ落ちる」を失敗にできる。S-03、S-04、S-02 など、スレッドまわりの修正は、その後の方が安全に確かめられる。
  - S-01 自身は、今いちばん確かめやすい。毎回落ちるので、「base は落ちる、head は落ちない」で、直ったことを示せる。
  - 上の「上げすぎない理由」の 2 つは、なくなった。確かめる手段（T-01）はできた。「先に小さいもので流れを固める」は、P-01 で済んだ（1 回の push で `Build MacType` と T-01 が通った）。
- P-03 を S-01 / P-05 の後にした理由: 単スレッドの画素比較だけで確かめられるので、S-01 に依存しない。どちらが先でもよい。ただ、効果は小さく、P-01 でわかったとおり T-01 の速度計測（±6% の揺れ）では見えないので、急がない。
- 気をつける点:
  - S-01 は、全描画が通る場所に初めてロックを足す修正で、P-01 より重い。S-01 だけだと、キャッシュヒット時のロックが一時的に 2 回になる。P-05 を続けて行い、速度は P-05 の後で見る。
  - S-01 を直しても、複数スレッドのモードが S-02 や S-03 で落ちる可能性は残る（推測）。その場合は、落ちた場所を見て、次の順番を決め直す。
  - T-01 で見られるのは GDI の経路だけ。実機での試し方（T-02）は、S-01 を普段使いに入れる前に決める。

---

## 5. 触らないもの・注意点

**直すと描画結果が変わるので、今回の方針では触らないもの（確認済みの不具合）**

- 演算子の優先順位の誤り: `flags & FT_LOAD_TARGET_LCD_V == FT_LOAD_TARGET_LCD_V`（`ft.cpp:2101, 2520, 2532`）。`==` が先に評価されるので、意図した判定になっていない。
- `CTempMem::init` の条件が逆（`ft.cpp:1302`）。大きいサイズを要求しても、バッファが大きくならない。メモリ破壊にはならない（呼び出し側が実サイズを渡している、`ft.cpp:2543-2555`）。FontLoader=WIN32 のときだけ通る。
- カーニングが、フェイスの「そのとき有効なサイズ」で計算される（`ft.cpp:2073-2082`）。
- 存在しないフォント名の予備キャッシュのキーに、フォント名が入っていない（P-14 参照）。
- DisplayAffinity の集合が、再読み込みで増えるだけで消えない（`settings.cpp:652-659`）。
- Direct2D の描画フックが足す微小な傾き（P-19）。外すと、文字の位置と形がわずかに変わりうる。
- Windows 名前の変換 `to_utf16le` の符号拡張（`common.cpp:9-10`。S-10 の補足 2）。直すと、非 ASCII のインスタンス名を持つ可変フォントで、選ばれるインスタンスが変わりうる。
- パス作成中のまま消された DC が、`DCArray` に残り続ける件（S-04 の補足）。直すと、その `HDC` の値を再利用した DC で、MacType が描くようになる。

**使われていないコード（触らない。有効化するなら要注意）**

- 未設置フックの `IMPL_CreateCompatibleDC` は、中でフック対象の `CreateCompatibleDC` 自身を呼ぶ（`override.cpp:1835`）。Detours 版には EasyHook のような再入防止がないので、このフックを設置すると無限再帰になる。`IMPL_DeleteDC` などが使う `ORIG_xxx` も、初期化されていない。
- `EmBoldVariableFont`（`ft.cpp:83-124`。呼び出しなし。中にリークあり）、`CFontSetCache`（`fteng.h:26-53`。`realloc` のサイズ指定が誤り）、`SafeUnload` と `expfunc.cpp` 側の `hook_term`（`expfunc.cpp:89-105, 145-175`。エクスポートされていない）、子プロセス向けのコード（`expfunc.cpp:215-1598` の大半と、`supinfo.h` の `_CreateProcess...` テンプレート。エクスポートも呼び出しもない）、`dll.cpp` の `CMemLoadDll`（`expfunc.cpp:268-271` からのみ使用）。

**外部との約束（変えない）**

- エクスポート 4 つと `IControlCenter` の仮想関数の並び（`settings.h:228-249`）。非公開のローダーや Tuner が使っている可能性がある。
- `MacType.ini` の項目名と意味。

**調べて、候補から外したもの**

- 縦書き用の `ft2vert_init`（フェイスを開くたびに呼ばれる、`ft.cpp:3205`）: GSUB テーブルの検証が重いのではと疑ったが、改造版 FreeType には otvalid モジュールが入っておらず（freetype の `include/freetype/config/ftmodule.h`、`builds/windows/vc2010/freetype.vcxproj` で確認）、検証はすぐ「未実装」で戻る。重くない。
- `FindFont(int faceid)` の「存在 bug！！！」コメント（`fteng.cpp:814`）: フェイス ID と配列の位置は、`CS_FONTENG` の下で一致するように増減されている（`fteng.cpp:585-594, 606-636`）。0 を渡す呼び出し元も見つからなかった。
- Direct2D の微小な傾き（P-19）: 高速化候補になるか調べたが、MacType 側の処理は軽く、外すと描画が変わりうるので見送り。理由は P-19。
- `CreateGlyphRunAnalysis` フックが毎回 `QueryInterface` をして、新しい版のフックへ回す件（`directwrite.cpp:545-577, 718-735`）: フックが 2〜3 段の入れ子になるが、グリフ解析の作成そのものに比べて十分軽いと見て、候補にしなかった（推測。未計測）。
- HookChildProcesses と LoadOnDemand: 高速化や安定性に関係するかと思って調べたが、Core では効いていない（1.7）。

**以前のブランチとの関係**

- `fix/thread-safety-and-buffers`（2 コミット）は、S-03、S-04 と、S-19 b / c ほかに当たる修正を、まとめて入れている。「1 ブランチ 1 修正」に合わせるなら、ここから 1 件ずつ切り出して作り直す。
- `test-patched` は、同じ 2 コミットを build.yml の上に載せたもの。

---

## 6. 過去のクラッシュ・ハング修正（参考）

| コミット | 日付 | 内容 | 今回の関連 |
|---|---|---|---|
| 05052e8 | 2026-04-29 | `CreateDxgiSurfaceRenderTarget` のフックを外す（WeCom のビデオ会議でクラッシュ） | 1.3 C |
| f2369ce | 2024-10-08 | レンダーターゲットのフック設置判定を修正 | S-09 |
| 0bbafc1 | 2024-09-09 | EasyHook 静的版でのプロセス終了処理 | S-14 |
| 20e2b10 | 2023-08-22 | Detours 版の修正。`REF_xxx` と `HCounter` を導入 | S-14、P-18 |
| fc3491b | 2023-05-30 | `ID2D1Device5/6` の vtable 番号ずれでクラッシュ | 1.3 B（番号は OS 更新でずれうる） |
| ed7adf9 | 2019-07-09 | `CAlphaBlend` の配列を `std::vector` に（スタックオーバーフロー対策） | P-01（このとき割り算が実行時の値になった） |
| da2d7f1 / 424b230 | 2019-07-25 | 遅延初期化の無限ループ（#583） | P-15 |
| 737f7e2 / d5999f7 | 2019-07-30 | `CHashedStringList` を `std::map` で作り直し、不具合修正 | S-06 |
| fa1baca | 2019-06-24 | 初期化失敗時のアンロードでクラッシュ | S-13 |
| b9f9e05 | 2018-10-06 | DirectWrite フックの競合を修正 | S-09 |
| 4cedd13 | 2018-09-25 | `ExtTextOut` に不正な文字数（8192 超）でクラッシュ | 1.4 の 1 |
| 15c6c09 | 2018-07-17 | 最初の文字が未対応文字のときクラッシュ | S-18 |
| 77cbc66 / d72e361 | 2016-09 | Firefox 対策の巻き戻しとクラッシュ修正（#191） | — |
| e35c7a1 / 8c7214a | 2016-09-27 | プロセス起動が遅くなる問題（#192） | S-13、P-10〜P-13 |
| 5320ef4 | 2016-09-02 | 起動時に固まるのを防ぐための対話プロセス判定 | S-13、P-12 |
| 8a7f497 | 2016-09-01 | DirectWrite 有効時に Windows 7 が起動で固まる、Edge が重い・落ちる | S-13 |
| 893ba52 | 2016-08-19 | 特定条件でプロセスが固まる | S-13 |
| 0522b1f | 2016-08-15 | DirectWrite / D2D フックでスタックオーバーフロー | S-09 |

---

## 7. 未確認のこと・決めておきたいこと

**けんじさんの回答で決まったこと（2026-11-17）**

| 初版の質問 | 回答 | 計画への反映 |
|---|---|---|
| よく使う設定 | 添付の `Default.ini` | 1.7 に、通る経路・通らない経路を整理した。2 章・3 章の一覧に「今の設定」の列を足し、着手順を見直した（4 章） |
| 実機での確かめ方 | 公式版を普段使い中。改造した DLL は、まだ試していない | 試し方を決める作業を T-02 として提案（4 章）。具体的な手順は未定（下の 1） |
| 実際に落ちるアプリ | 特になし | 2 章は「今起きている不具合の修正」ではなく「予防」になる。起きる頻度の見込みは、低めに読む。クラッシュダンプから優先順位を付け直す手は、今は使えない |
| 対応 OS の範囲 | Vista 以降の API は使ってよい | SRW ロック、`InitOnceExecuteOnce`、`LdrRegisterDllNotification` を使える。P-11 の難しさを「中」に下げた。S-02、S-22、S-23、P-04、P-10 の直し方の選択肢が増えた |

**まだ決まっていないこと・確かめられていないこと**

1. **実機での試し方の手順**（T-02）。非公開のローダー（`MacType64.dll`）が Core DLL をどこから読むか、Core だけ差し替えて動くかは、コードからはわからない。`macloader64.exe アプリ名` で特定のアプリだけに当てる方法が安全そうだが、未確認。公式版が動いている PC で試すと、同じプロセスに公式版と改造版の両方が入るおそれもある（推測）。試すアプリを公式版の `[UnloadDll]` に入れておく、などの手当てが要るかもしれない。
2. **速度の実測**。3 章の効果は、P-01 の再現ベンチ以外すべて推測。T-01 のテスト用 EXE か、実機でのプロファイル（ETW など）で確かめたい。特に、P-20（`GetFontData` の回数と時間）と P-11（起動時の DLL 読み込み）は、測れば効果がすぐわかる。
3. **Direct2D の微小な傾き（P-19）**。外したときに速くなるか、画素が変わるか、`DrawText` の中でフックが入れ子になっているか。どれも実機でしか確かめられない。今は見送り。
4. **Win32 ビルドの `/Zc:threadSafeInit-` を外すか**。XP 向けの設定と思われ、Vista 以降だけが対象なら外せる。外すと、S-23 と、S-09 のうち `static` の初期化に関わる部分が、Win32 でも x64 と同じになる。コンパイルオプションは DLL 全体に効くので、やるなら ID を付けて相談してから。
5. **`CreateMessageWnd` が呼ばれているか**（S-12）、**動的アンロードをどれくらい使うか**（S-14）、**設定の再読み込みが、どのスレッドから、どれくらいの頻度で来るか**（S-11、S-20、S-22）。どれも、ローダーやトレイの使い方による。
6. **"C" ロケールの `wcstombs` の動き**（S-21）。MSVC の CRT の仕様として書いたが、実機では確かめていない。FontSubstitutes を使わない限り通らないので、急がない。

---

## 8. レビュー（`docs/kaizo-plan-review.md`）への対応

別セッションの検証結果を 1 件ずつコードと突き合わせた。行番号はすべて `693ca92` で確かめ直した。

| レビューの項目 | 判定 | この文書での対応 |
|---|---|---|
| 1.1 所有者つきロックの説明が違う | 納得 | 1.6 を書き直した。飢餓は S-22 として追加 |
| 1.2 取得順の抜け (1)〜(3)、(5) | 納得 | 1.6 の取得順に足した。(5) は S-20 |
| 1.2 取得順の抜け (4) | 一部、納得できない | 下の 1 |
| 1.5 の表の「ロック: なし」は厳密には違う | 納得 | 1.5 の表を直した |
| P-01 | 納得（書き方を 1 点だけ変えた） | P-01 に補足。下の 3 |
| P-03、P-05 | 納得 | それぞれに補足 |
| S-03 | 納得（1 点だけ違う） | S-03 に修正時の注意。下の 4 |
| S-04、S-09、S-10 | 納得 | それぞれに補足。描画が変わる点は 5 章へ |
| S-08 | 納得 | 起きる条件を正確にした。A 版の変換の件は S-21 へ |
| S-01 と、追加の壊れ方 | 納得（もう少し強い形で） | S-01 を書き直した。再現コードで確認（付録 A）。下の 5 |
| S-02 | 納得 | 抜ける側の動きと、直し方を足した |
| S-20（案） | 納得。コードで確かめて採用 | S-20 |
| S-21（案） | 一部、納得できない | S-21。下の 2 |
| S-22（案） | 納得。コードと再現コードで確かめて採用（付録 B）。影響は「小」にした | S-22。下の 6 |
| 着手順（S-01 を前へ） | 納得 | 4 章 |

**納得できなかった点・変えた点と、その理由**

1. **レビュー 1.2 (4)「`ReloadConfig` の後、`ReloadAll` の中の `GetInstance()` で遅延初期化が走る」**。`m_bDelayedInit` を戻した（`settings.h:1007`）直後に呼ばれるのは `RefreshAlphaTable()`（`settings.h:1008`）で、これが `CAlphaBlend::init` の先頭の `GetInstance()`（`ft.cpp:267`）を通る。遅延初期化はここで走る。持っているのは `CS_SETTING` だけ。`ReloadAll` の `GetInstance()`（`fteng.h:624`）の時点では、もう済んでいる。`LoadSetting` も同じ並び（`settings.h:986, 988`）。`CS_LIBRARY` → `CS_SETTING` の順が実在する、という点は正しい。`CS_LIBRARY` の下で遅延初期化まで走るのは、その間に別のスレッドがフラグをもう一度戻したときだけ。
2. **S-21 のうち「アプリが UTF-8 ロケールにしていると、終端なしで切れる」**。CRT は静的リンク（`gdipp.vcxproj:499, 682`）なので、アプリの `setlocale` はこの DLL に届かない。DLL 自身も `setlocale` を呼ばない（呼ぶのは `INFINALITY` 定義時だけ、`settings.cpp:665-675`）。この壊れ方は起きない。一方、「日本語名で失敗する」の方は、"C" ロケール固定なので必ず起きる。同じ書き方の `IMPL_GetObjectA`（`override.cpp:664-668`）も対象に足した。
3. **P-01 の書き方 `tbl2[(unsigned)n >> 16]`**。範囲内の `n` なら今と同じ。範囲外（設定値が異常で `n` が負）まで今と同じ添字にするなら、`(size_t)n >> 16` の方が正確（x64 で差が出る）。どちらにしても範囲外読みなので実害の差はないが、「今と同じ」と言い切れる方を選ぶ。
4. **S-03 の「`new T` は GDI を呼びうる」**。コンストラクタは GDI を呼ばない（`cache.h:82-93`、`override.h:71-75`）。GDI を呼ぶのは、削除側のデストラクタ（`cache.h:95-104`）。ロックの外に置くべきなのは、削除側の `delete`。
5. **S-01 の「3 本目のスレッドが」**。3 本目でなくても、先に描き終えたスレッドの次の描画で足りる（スレッドは 2 本でよい）。また、解放済みのグリフを読む場合のほかに、差し替えの途中の NULL を読む場合もある（`ftref.c:26`、`fteng.cpp:219-222`）。どちらも、レビューの結論を強める方向の違い。
6. **S-22 の影響「小〜中」**。「小」にした。落ちるのではなく待たされるだけで、再現コードでも、描画の間に少しでも隙間があれば短時間で入れたため。
7. **レビュー 1.2 (2) の「`CS_FONTENG` → `CS_SETTING` → `CS_CACHEDFONT`」という 1 本の鎖**。順としては `CS_FONTENG` → `CS_SETTING` と `CS_FONTENG` → `CS_CACHEDFONT` の 2 つ（1.6）。`CS_SETTING` を持ったまま `CS_CACHEDFONT` を取るのは、遅延初期化の中だけ。逆順がない、という結論は変わらない。

**レビューが確かめていて、こちらでは再実行していないもの**: P-01 の、設定値 400 通り × 全組み合わせの比較（レビュー付録 A）。

---

## 付録: 今回足した再現コード

どちらも、MacType のロジックを Linux の部品に写したもの。g++ 13.3（x86-64 Linux、2 コア）で実行した。Windows 実機での確認ではない。

### 付録 A: S-01 の「同じ文字の上書き」

`g++ -O1 -g -pthread -fsanitize=address` でビルドすると、`RefCopy`（`FT_Glyph_Ref_Copy` に当たる）の中で `heap-use-after-free` が報告される。`g++ -O2 -pthread` では、3 回動かして 3 回とも異常終了した。

<details>
<summary>glyph_replace.cpp</summary>

```cpp
// S-01 の「同じ文字の上書き」の確認。
// ftref.c (FT_Glyph_Ref_Copy / FT_Done_Ref_Glyph) と FreeTypeCharData::SetGlyph (fteng.cpp:215-230) を写し、
//   書き手: すでにキャッシュにある文字への AddCharData = SetGlyph (古いグリフを手放す → 新しいグリフを入れる)
//   読み手: キャッシュヒット (m_glyph を読む → Ref_Copy → 使う → 手放す)
// を、本物と同じくロックなしで同時に回す。上書きを連続で起こしているので、頻度の再現ではない。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
struct RefGlyph { void* ft_glyph; std::atomic<int> refcount; };
static int RefCopy(RefGlyph* source, RefGlyph** target) {       // ftref.c:7-16
  if (source->refcount < 0) return 1;
  ++source->refcount; *target = source; return 0; }
static void Done(RefGlyph** g) {                                 // ftref.c:18-27
  if (--(*g)->refcount == 0) { free((*g)->ft_glyph); (*g)->~RefGlyph(); free(*g); }
  *g = NULL; }
static RefGlyph* NewGlyph() { RefGlyph* g = (RefGlyph*)malloc(sizeof(RefGlyph)); new (g) RefGlyph; g->ft_glyph = malloc(64); g->refcount = 1; return g; }
static RefGlyph* volatile m_glyph = NULL;                        // FreeTypeCharData::m_glyph (ただのポインタ)
static void SetGlyph(RefGlyph* glyph) {                          // fteng.cpp:215-230
  RefGlyph* gl = m_glyph;
  if (gl) { RefGlyph* tmp = gl; Done(&tmp); m_glyph = NULL; }    // FT_Done_Ref_Glyph(&gl) は m_glyph を NULL にする
  RefGlyph* t = NULL; RefCopy(glyph, &t); m_glyph = t; }
int main() {
  std::atomic<bool> stop{false}; std::atomic<long> nullSeen{0}, hits{0}, replaces{0};
  { RefGlyph* g = NewGlyph(); SetGlyph(g); Done(&g); }
  std::thread w([&] { while (!stop) { RefGlyph* g = NewGlyph(); SetGlyph(g); Done(&g); ++replaces; } });
  std::thread r([&] { while (!stop) { RefGlyph* p = m_glyph;     // chData->GetGlyph(render_mode) (ft.cpp:1866)
      if (!p) { ++nullSeen; continue; }                           // 本物は、ここで NULL を FT_Glyph_Ref_Copy に渡して落ちる
      RefGlyph* mine = NULL; if (RefCopy(p, &mine) == 0) { volatile char c = *(char*)mine->ft_glyph; (void)c; Done(&mine); } ++hits; } });
  std::this_thread::sleep_for(std::chrono::seconds(3)); stop = true; w.join(); r.join();
  printf("replaces=%ld hits=%ld null_seen=%ld\n", replaces.load(), hits.load(), nullSeen.load());
  return 0; }
```

</details>

### 付録 B: S-22 の飢餓

`g++ -O2 -pthread` でビルド。

<details>
<summary>ownedcs_starve.cpp と実行結果</summary>

```cpp
// ownedcs.cpp の Enter / Leave を、そのまま Linux の部品に写したもの。
//  (A) 描画役(owner=1)が重なり続けると、owner=2 が入れない
//  (B) 待ちに入った owner=1 どうしは、1 本ずつしか入れない
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono;
// 自動リセットイベント。SetEvent の時点で待っているスレッドがいれば、いちばん古い 1 本に渡す
struct Event {
  std::mutex m; std::condition_variable cv; bool signaled = false; unsigned long nextSeq = 0, granted = 0;
  void Set() { std::lock_guard<std::mutex> l(m); if (granted < nextSeq) { ++granted; cv.notify_all(); } else signaled = true; }
  void Wait() { std::unique_lock<std::mutex> l(m); if (signaled) { signaled = false; return; }
                unsigned long my = nextSeq++; cv.wait(l, [&] { return my < granted; }); }
};
struct OCS { std::atomic<int> nOwner{-1}, nRequests{-1}, nRecursiveCount{0}; Event ev; std::mutex threadLock; };
void Enter(OCS* cs, int Owner) {                      // ownedcs.cpp:24-44
  cs->threadLock.lock();
  if (cs->nOwner == Owner) { ++cs->nRecursiveCount; cs->threadLock.unlock(); }
  else {
    if (++cs->nRequests > 0) { cs->threadLock.unlock(); cs->ev.Wait(); }
    else cs->threadLock.unlock();
    cs->nOwner = Owner;
    cs->nRecursiveCount = 1;
  }
}
void Leave(OCS* cs, int Owner) {                      // ownedcs.cpp:46-61
  cs->threadLock.lock();
  if (cs->nOwner == Owner) {
    if (--cs->nRecursiveCount <= 0) { cs->nOwner = -1; if (--cs->nRequests >= 0) cs->ev.Set(); }
  } else --cs->nRecursiveCount;
  cs->threadLock.unlock();
}
static double testA(int nDrawers, int holdUs, int gapUs, double capSec) {
  OCS cs; std::atomic<bool> stop{false}, got{false};
  std::vector<std::thread> th;
  for (int i = 0; i < nDrawers; i++) th.emplace_back([&, i] {
    std::this_thread::sleep_for(microseconds(i * holdUs / nDrawers));      // ずらして始める
    while (!stop) { Enter(&cs, 1); std::this_thread::sleep_for(microseconds(holdUs)); Leave(&cs, 1);
                    if (gapUs) std::this_thread::sleep_for(microseconds(gapUs)); } });
  std::this_thread::sleep_for(milliseconds(200));
  auto t0 = steady_clock::now(); double waited = -1;
  std::thread w([&] { Enter(&cs, 2); waited = duration<double>(steady_clock::now() - t0).count(); got = true; Leave(&cs, 2); });
  while (!got && duration<double>(steady_clock::now() - t0).count() < capSec) std::this_thread::sleep_for(milliseconds(5));
  bool ok = got; stop = true;
  for (auto& t : th) t.join();
  w.join();
  return ok ? waited : -1;
}
static void testB() {
  OCS cs; std::atomic<int> in{0}, maxIn{0};
  Enter(&cs, 2);
  std::vector<std::thread> th;
  for (int i = 0; i < 3; i++) th.emplace_back([&] { Enter(&cs, 1); int n = ++in; int m = maxIn; while (n > m && !maxIn.compare_exchange_weak(m, n)) {}
      std::this_thread::sleep_for(milliseconds(50)); --in; Leave(&cs, 1); });
  std::this_thread::sleep_for(milliseconds(100));     // 3 本とも待ちに入るのを待つ
  auto t1 = steady_clock::now();
  Leave(&cs, 2);
  for (auto& t : th) t.join();
  printf("(B) waiting owner=1 x3 (50ms each): max inside at once=%d, total=%.0f ms\n",
         maxIn.load(), duration<double>(steady_clock::now() - t1).count() * 1000);
}
int main() {
  for (int gap : {30, 0})
    for (int n : {1, 2, 4, 8}) {
      double w = testA(n, 300, gap, 3.0);
      if (w < 0) printf("(A) drawers=%d hold=300us gap=%dus: owner=2 did not get in within 3.0 s\n", n, gap);
      else       printf("(A) drawers=%d hold=300us gap=%dus: owner=2 waited %.2f ms\n", n, gap, w * 1000);
      fflush(stdout);
    }
  testB();
  return 0;
}
```

```
(A) drawers=1 hold=300us gap=30us: owner=2 waited 0.22 ms
(A) drawers=2 hold=300us gap=30us: owner=2 waited 0.20 ms
(A) drawers=4 hold=300us gap=30us: owner=2 waited 0.61 ms
(A) drawers=8 hold=300us gap=30us: owner=2 waited 70.39 ms
(A) drawers=1 hold=300us gap=0us: owner=2 waited 0.38 ms
(A) drawers=2 hold=300us gap=0us: owner=2 waited 701.08 ms
(A) drawers=4 hold=300us gap=0us: owner=2 did not get in within 3.0 s
(A) drawers=8 hold=300us gap=0us: owner=2 did not get in within 3.0 s
(B) waiting owner=1 x3 (50ms each): max inside at once=1, total=151 ms
```

</details>
