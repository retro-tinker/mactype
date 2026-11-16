# MacType 改造計画（調査結果）

目的は「安定性の向上」と「高速化」。この文書は、コードを変更する前の調査結果をまとめたもの。

| 項目 | 内容 |
|---|---|
| 対象 | retro-tinker/mactype の `directwrite` ブランチ、コミット `693ca92`（upstream snowie2000/mactype `directwrite` の `05052e8` に build.yml を足しただけの状態） |
| 調査日 | 2026-11-16 |
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

1. **一番大きい安定性リスクは、スレッド間の排他が足りないこと。** 描画フックは「所有者つきロック」で守られているように見えるが、このロックは描画スレッド同士を排他しない（1.6 参照）。その内側で、グリフキャッシュ（`std::map`）など複数の共有データがロックなしで読み書きされている（S-01, S-03〜S-07）。複数スレッドが同時に文字を描くプロセスで、まれにクラッシュしうる。
2. **所有者つきロック自体にも穴がある**（S-02）。再現コードで、条件が重なると「描画」と「フォント削除」が同時に内側へ入れることを確認した。
3. **速度面で一番効きそうなのは、アルファブレンドの画素ごとの割り算**（P-01）。シフトに置き換えても結果が完全に同じになることを全域で確認済み。Linux/g++ での再現ベンチでは、このループが約 4.5 倍速くなった（MSVC での実測ではない）。
4. **次に効きそうなのは、空白 1 文字ごとに GDI の `ExtTextOutW` を 1 回呼んでいる点**（P-02）と、グリフキャッシュの間引きが「最近使った順」ではなく「キーの小さい順」になっている点（P-08、日本語で効く）。
5. **プロセス起動時の重さ**もある。除外プロセスでも走る約 3.7 万回の `iswcntrl`（P-10）、D2D を使わないプロセスでも `d2d1.dll` / `dwrite.dll` を読み込む処理（P-11 / S-13）。
6. 最初の 1 本は、小さくて結果が同じと証明できる P-01 がおすすめ（4 章）。

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

DirectWrite / Direct2D 側は自前で描いていない。レンダリングパラメータ（ガンマ、コントラスト、ClearType レベル、レンダリングモード、アンチエイリアス）を差し替えるだけ（`directwrite.cpp:196-281, 525-528`）。ヒンティング無効の設定では、描画のたびに変換行列へ微小な回転（1/0xFFFF）を足して元に戻す（`directwrite.cpp:1697-2107`）。

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
| FreeType のキャッシュマネージャ（フェイス、サイズ、文字→グリフ番号、グリフ輪郭） | `ft.cpp:3454-3471` | FreeType 内部 | フェイス 64 以上、サイズ 1200、10MB（`settings.cpp:646-649`） | `CS_MANAGER` |
| フォント表 `m_mfontMap` / `m_mfullMap` / `m_mfontList` | `fteng.h:531-540` | (名前, 太さ, 斜体) / 一意なフルネーム / フェイス ID → `FreeTypeFontInfo` | `RemoveFont` 以外では減らない | `CS_FONTMAP`、`CS_FONTENG` |
| サイズ別キャッシュ表 `FreeTypeFontInfo::m_cache` | `fteng.h:315-316, 372`、`fteng.cpp:441-478` | (px, 幅, 太さ, 斜体) のハッシュ → `FreeTypeFontCache` | CacheMaxSizes 超でキーの小さい順に破棄（`fteng.cpp:373-381`） | `CS_FONTCACHE` |
| **グリフキャッシュ `FreeTypeFontCache::m_GlyphCache`** | `fteng.h:204, 218, 223-240` | 文字コード、または −(グリフ番号) → `FreeTypeCharData`（描画済みビットマップへの参照、GDI 幅、AA モード） | 2048 回追加するごとに 1024 個まで間引く。**キーの小さい順に捨てる**（`fteng.cpp:16-18, 123-136, 254-264, 285-287`） | **なし**（S-01） |
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

**所有者つきロック**（`ownedcs.cpp`、`common.h:272-309`）: 「スレッド」ではなく「番号（owner）」で排他する独自ロック。**同じ owner 同士は何スレッドでも同時に入れる**（`ownedcs.cpp:27-31`）。つまり読み手/書き手ロックに近い。

| ロック | owner=1（同時に入れる側） | owner=2（単独で入る側） |
|---|---|---|
| `OCS_DC` | `ExtTextOutW` の描画（`override.cpp:1252`） | フォント削除（`fteng.cpp:550`） |
| `OCS_FREETYPE` | グリフ取得（`ft.cpp:3046`） | 設定反映 `ReloadAll`（`fteng.h:622`） |

**確認できたロックの取得順**（左を持ったまま右を取る）:

- 描画: `OCS_DC(1)` → `OCS_FREETYPE(1)` →（`CS_FONTMAP` → `CS_FONTENG` → `CS_SETTING`）／（`CS_FONTLINK` → `CS_FONTMAP` → `CS_FONTENG`）／（`CS_MANAGER` → `CS_FONTMAP`）／`CS_FONTCACHE`／`CS_LIBRARY`
- フォント削除: `OCS_DC(2)` → `CS_MANAGER` → `CS_FONTMAP` → `CS_FONTENG`、`CS_FONTMAP` → `CS_FONTCACHE`
- 設定反映: `OCS_FREETYPE(2)` → `CS_LIBRARY` →（`CS_SETTING`／`CS_FONTCACHE`）
- 初回の設定取得: `CS_SETTING` → `CS_CACHEDFONT`

読んだ範囲では、これらのロック同士で順序が逆になる組は見つからなかった（確認済み。ただし全経路の網羅を保証するものではない）。ローダーロックとの関係は別問題（S-13, S-14）。

**ロックで守られていない共有データ**（2 章の各項目へ）: グリフキャッシュ（S-01）、TLS 一覧と `TLSDCArray`（S-03）、`DCArray`（S-04）、除外フォント集合（S-05）、`FontNameCache`（S-06）、`FontCache` の読み取り（S-07）、設定全般（S-11）。

---

## 2. 安定性リスク

### 2.1 一覧

| ID | 内容 | 根拠 | 影響 | 難しさ | 確信度 |
|---|---|---|---|---|---|
| S-01 | グリフキャッシュ（`std::map`）を、複数スレッドがロックなしで読み書きする | `fteng.h:223-240, 258-276`、`fteng.cpp:280-369`、`ft.cpp:1857-1866, 3046` | 大 | 中 | 高 |
| S-02 | 所有者つきロックで、所有者とカウントの更新がロックの外にあり、排他が破れる | `ownedcs.cpp:24-44, 46-61` | 中 | 中 | 高（再現コードあり） |
| S-03 | TLS オブジェクト一覧と `TLSDCArray` を、スレッドの初回描画・終了時にロックなしで更新する | `tlsdata.h:29-31, 73-75`、`override.h:74, 78`、`fteng.cpp:575-580` | 中 | 低 | 高 |
| S-04 | パス作成中 DC の集合 `DCArray` を、ロックなしで追加・削除・検索する | `override.cpp:47, 629, 640, 651, 1139` | 中 | 低 | 高 |
| S-05 | 除外フォント集合に、描画中のスレッドがロックなしで追加する（他スレッドは同時に検索） | `settings.cpp:1056-1071`、`override.cpp:1311, 1317`、`ft.cpp:1445, 1517` | 中 | 低〜中 | 高 |
| S-06 | フォント名キャッシュ `FontNameCache` を、別々のロックの下（またはロックなし）から読み書きする | `hash_list.cpp:5-37`、`fteng.cpp:43, 52`、`settings.cpp:1358, 1421, 1745-1766` | 中 | 低 | 高 |
| S-07 | 置換フォント記録 `FontCache` の読み取りがロックなしで、中の文字列ポインタをそのまま返す（FontSubstitutes 有効時のみ） | `override.cpp:399-415, 417-435, 444, 563, 582, 661, 680` | 中 | 低 | 高 |
| S-08 | `GetTextFaceA` / `GetTextFaceAliasW` のフックが、呼び出し元のバッファ長を無視して最大 32 文字書く（FontSubstitutes 有効時のみ） | `override.cpp:437-451, 579-595`（正しい例は `560-577`） | 中 | 低 | 高 |
| S-09 | COM メソッドのフック設置（`HOOK` マクロ）が直列化されていない。`ORIG_xxx` を上書きして無限再帰、またはフックの取りこぼし | `directwrite.cpp:28-35, 625, 797, 881`、`hook.cpp:111-123`、`gdipp.vcxproj:509` | 中 | 低 | 中 |
| S-10 | 可変フォントの名前比較: ヒープの読み越し、例外の送出、メモリリーク | `ft.cpp:1320-1399, 3210`、`common.cpp:3-20` | 中 | 低〜中 | 高（例外の発生条件は中） |
| S-11 | 設定の再読み込みが、描画スレッドと排他されていない | `settings.h:978-1012`、`settings.cpp:747-752, 1218-1235`、`fteng.h:619-647` | 中 | 高 | 高 |
| S-12 | `WM_COPYDATA` の処理で JSON の例外を捕まえていない | `settings.h:1065-1098` | 小〜中 | 低 | 中 |
| S-13 | `DllMain`（ローダーロック中）で、ファイル読み込み・FreeType 初期化・`LoadLibrary` などの重い処理をしている | `hook.cpp:558-559, 573, 584, 595-599`、`directwrite.cpp:1500-1506`、`settings.cpp:535-538` | 中 | 中〜高 | 高（実害は推測） |
| S-14 | 動的アンロード時、フック内に残ったスレッドを待ち切れないと、そのまま資源を解放する | `hook.cpp:134-147, 639-681`、`hookCounter.h:18-23` | 中 | 高 | 中 |
| S-15 | 同じ `FT_Face` を、別々のロック（`CS_MANAGER` と `CS_LIBRARY`）の下で使う（ColorFont 有効時） | `ft.cpp:2123-2136, 2185-2193` | 小 | 中 | 中 |
| S-16 | キャッシュマネージャから借りた `FT_Face` を、ロックを離した後も使い続ける | `ft.h:358-377`、`ft.cpp:2076, 2113, 2186` | 小〜中 | 高 | 中 |
| S-17 | `RemoveFontResourceEx` フックが重く、副作用もある（未登録なら一度 `AddFontResource` する、他スレッドの作業用ビットマップのデストラクタを直接呼ぶ） | `override.cpp:843-847`、`fteng.cpp:59-99, 542-583` | 小〜中 | 高 | 中 |
| S-18 | NULL・失敗チェック漏れ（a〜h。2.2 に内訳） | 2.2 参照 | 小 | 低 | 高 |
| S-19 | 配列の境界・バッファ長の小さな誤り（a〜d。2.2 に内訳） | 2.2 参照 | 小 | 低 | 高 |

### 2.2 各項目の説明

#### S-01 グリフキャッシュがロックなし

- 確認済み:
  - グリフキャッシュの実体は `std::map<int, FreeTypeCharData*>`（`fteng.h:204, 218`）。検索（`FindChar` / `FindGlyphIndex`、`fteng.h:258-276`）も、追加（`AddCharData` / `AddGlyphData`、`fteng.cpp:280-369`）も、ロックを取らない。
  - 追加の途中で間引き（`Compact`）が走ると、ほかのスレッドが検索で得たばかりの `FreeTypeCharData` を解放しうる（`fteng.cpp:285-287, 123-136`）。検索した側は、そのポインタから幅やグリフを読む（`ft.cpp:1863-1866`）。
  - 呼び出し元が取っているのは `OCS_DC(1)` と `OCS_FREETYPE(1)` だけで、これは描画スレッド同士を排他しない（`override.cpp:1252`、`ft.cpp:3046`、`ownedcs.cpp:27-31`）。
  - キャッシュヒット時の `CS_LIBRARY`（`ft.cpp:1865`）は、検索の後で取っているので、この競合を防がない。
- 推測: 同じフォント・同じサイズの文字を、2 つ以上のスレッドが同時に GDI で描くプロセス（複数の UI スレッドを持つアプリなど）で、まれに `std::map` が壊れてクラッシュやハングになる。実際の頻度は不明。
- 直し方の方向: 検索〜参照コピーまでと、追加・間引きを、同じロックで囲む。キャッシュヒット時の `CS_LIBRARY` と置き換えれば、ロック回数は増えない（P-05 と一緒に設計する）。サイズ別キャッシュ自体（`FreeTypeFontCache`）が破棄される経路（`fteng.cpp:373-381`）も同じ問題を持つが、既定値（1200 サイズ）ではまず起きない。

#### S-02 所有者つきロックの排他破れ

- 確認済み: `EnterOwnedCritialSection` は、内部ロックを離した後で `nOwner` と `nRecursiveCount` を書く（`ownedcs.cpp:36-42`）。この 2 つの代入の間に、同じ owner の別スレッドが入るとカウントが 1 つ失われる。先に入ったスレッドが抜けた時点でロックが「空き」と見なされ、owner=2（フォント削除、設定反映）が、まだ中にいる描画スレッドと同時に入れる。
- 確認済み（再現コード）: ロジックをそのまま Linux に写したテストで確かめた。写しそのままでは 5 秒 × 3 回で違反 0 件（窓が非常に狭い）。2 つの代入の間に関数呼び出し 1 つ分の遅れを入れると、約 2900 万回中 17 件、`yield` を入れると 60〜90 件の「同時進入」が出た。
- 推測: 実害が出るのは、ちょうどその瞬間にフォント削除や設定の再読み込みが重なったときだけ。頻度は低い。
- 直し方の方向: 所有者とカウントの更新を内部ロックの中で行う形に直す。動作を変えずに済むが、全描画が通るロックなので慎重に。

#### S-03 TLS 一覧と `TLSDCArray`

- 確認済み: スレッドが初めて描画すると `CTlsData::GetPtr` が `pArray->Add`（`tlsdata.h:29-31`）を、`CThreadLocalInfo` のコンストラクタが `TLSDCArray.insert`（`override.h:74`）を、どちらもロックなしで行う。スレッド終了時の削除も同じ（`tlsdata.h:73-75`、`override.h:78`）。`RemoveFont` は `TLSDCArray` を走査する（`fteng.cpp:575-580`）。
- 推測: 複数のスレッドが同時に初回描画する、またはスレッドの開始と終了が重なると、配列や集合が壊れる。壊れた後の追加・削除・スレッド終了時にクラッシュする。
- 直し方の方向: 追加・削除・走査をロックで囲む。スレッドの初回と終了時だけなので、速度への影響はない。

#### S-04 `DCArray`

- 確認済み: `BeginPath` / `EndPath` / `AbortPath` のフックが `std::set<HDC>` をロックなしで更新し（`override.cpp:629, 640, 651`）、`ExtTextOutW` のフックが毎回ロックなしで検索する（`override.cpp:1139`）。
- 推測: パスを使うスレッドと文字を描くスレッドが別にあるアプリで、まれに壊れる。
- 直し方の方向: ロックで囲む。`ExtTextOutW` は毎回通るので、「集合が空ならロックを取らない」形（個数をアトミックに持つ）にして、速度を落とさない。

#### S-05 除外フォント集合

- 確認済み: `IsFontExcluded` は `std::set<wstring>` をロックなしで検索する（`settings.cpp:1056-1065`）。`AddFontExclude` はロックなしで追加する（`settings.cpp:1067-1071`）。追加は、描画の途中で「TrueType/OpenType ではない」「フェイスを開けない」とわかったときに起きる（`override.cpp:1317`、`ft.cpp:1445, 1517`）。
- 推測: ビットマップフォントなどを初めて描く瞬間に、別スレッドの検索と重なると壊れる。

#### S-06 `FontNameCache`

- 確認済み: `CHashedStringList` は中で `std::map` を使い、ロックを持たない（`hash_list.h`、`hash_list.cpp:5-37`）。呼び出し元のロックはばらばら。`FreeTypeFontInfo` のコンストラクタ（`CS_FONTENG` の下、`fteng.h:412`）、リンクフォント作成（`CS_FONTLINK` の下、`settings.cpp:1790`）、遅延初期化（`CS_SETTING` の下、`settings.cpp:216`）、フォント作成フック（ロックなし、`override.cpp:724` → `settings.cpp:1745-1766`）。
- 直し方の方向: `CHashedStringList` の中にロックを 1 つ持たせる。

#### S-07 / S-08 置換フォントまわり（FontSubstitutes 有効時のみ）

- S-07 確認済み: 書き込み側（`AddToCachedFont` / `DeleteCachedFont`）は `CS_CACHEDFONT` を取るが、読み取り側（`GetCachedFont` / `GetCachedFontLocale`、`override.cpp:399-415`）は取らない。しかも中の文字列へのポインタを返すので、別スレッドが同じフォントを `DeleteObject` すると、解放済みメモリを読む。
- S-08 確認済み: `IMPL_GetTextFaceAliasW` は `nLen` を見ずに `StringCchCopy(lpAliasW, LF_FACESIZE, ...)`（`override.cpp:447`）、`IMPL_GetTextFaceA` は `c` を見ずに `StringCchCopyA(lpName, LF_FACESIZE, ...)`（`override.cpp:590`）。アプリが 32 文字より小さいバッファを渡すと、その先まで書く。W 版（`override.cpp:567-568`）は長さを見ている。
- FontSubstitutes を使っていなければ `FontCache` は常に空なので、どちらも起きない。

#### S-09 COM フック設置の競合

- 確認済み:
  - `HOOK` マクロは「未フックか確認 → `ORIG_xxx` に vtable の関数アドレスを代入 → `hook_demand_xxx`」の 3 段階で、全体を守るロックがない（`directwrite.cpp:28-35`）。別スレッドが同じフックを先に完了させた直後に代入が走ると、`ORIG_xxx`（Detours がトランポリンに書き換え済み）が、フック済みの関数アドレスで上書きされる。以後 `IMPL_xxx` → `ORIG_xxx` → `REF_xxx` → `IMPL_xxx` の無限再帰になる。
  - `GetAlphaBlendParams` は 3 箇所から設置されうる（`directwrite.cpp:625, 797, 881`）。それぞれ別の `static bool loaded` で守られているだけなので、3 箇所の間では排他されない。Win32 ビルドは `static` の初期化自体がスレッド安全でない（`gdipp.vcxproj:509`）。
  - `hook_demand_xxx` は `DetourTransactionBegin` の戻り値を見ず、`DetourAttach` が失敗しても `IsHooked_xxx = true` にする（`hook.cpp:113-118`）。Detours は、他スレッドのトランザクション中だと `Begin` も `Attach` も `ERROR_INVALID_OPERATION` を返す（Detours の `detours.cpp:1873-1881, 2319-2321` で確認）。つまり競合すると、そのフックは「設置済み」と記録されたまま、設置されない。
- 推測: 窓はごく狭い。ただし無限再帰は一度起きると、そのプロセスではそのメソッドを呼ぶたびにスタックオーバーフローする。過去の 0522b1f（スタックオーバーフロー修正）、b9f9e05（フックの競合修正）と同じ系統。
- 直し方の方向: `HOOK` マクロの中身全体を 1 つのロック（既存の `CS_DWRITE`）で囲む。

#### S-10 可変フォントの名前比較

Windows 11 の Segoe UI Variable など、名前つきインスタンスを持つ可変フォントを開くたびに通る（`face_requester` → `VarFontByAlias` → `IsSFNTNameMatch`、`ft.cpp:3210, 1380-1399, 1320-1378`）。

- 確認済み (a) 読み越し: `FT_SfntName.string_len` はバイト数だが、`wstring(ポインタ, string_len)` と文字数として使っている（`ft.cpp:1325, 1336`）。FreeType はこの文字列をちょうど `string_len` バイトで確保する（freetype の `src/base/ftsnames.c:68-70` で確認）ので、確保した領域の後ろを同じバイト数だけ読む。
- 確認済み (b) 例外: Mac プラットフォームなどの名前を `to_wide_string` に渡す（`ft.cpp:1329, 1339`）。中身は `std::wstring_convert`（`common.cpp:16-20`）で、UTF-8 として不正なバイト列だと `std::range_error` を投げる。途中に `catch` はなく、FreeType（C のコード）を突き抜けてアプリまで届く。
- 確認済み (c) リーク: 名前つきインスタンスが 0 個の可変フォントでは、`FT_Done_MM_Var` を呼ばずに戻る（`ft.cpp:1386`）。
- 推測: (a) は読むだけなので、ほぼ表面化しない。(b) は、Mac プラットフォームの名前にアクセント記号などを持つ可変フォントがあると、そのフォントを使うアプリが確実に落ちる。そういうフォントが実在するかは未確認。
- 注意: (a) を直すとき、Windows プラットフォームの名前（`ft.cpp:1336`）は結果を変えずに直せる。Unicode プラットフォーム（`ft.cpp:1325`）は、今は壊れた文字列になっていて一致しない。正しく直すと一致するようになり、選ばれるインスタンスが変わりうる（Windows 用の名前を持たないフォントに限る）。

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

#### S-14 動的アンロード

- 確認済み: `hook_term` はフックを外した後、フック内にいるスレッド数が 0 になるのを最大 3000 回（1 回 1ms 以上のスリープ）待つ（`hook.cpp:146`、`hookCounter.h:18-23`）。これは `DllMain`（ローダーロック中）で行われる。待ち切れなくても、そのままエンジンや設定、ロックを破棄する（`hook.cpp:656-677`）。
- 推測: フック内でローダーロックを待っているスレッドがいると、待ちは必ずタイムアウトし、その後そのスレッドが解放済みの資源に触れて落ちる。
- 直し方の方向: 根本的には「アンロードしない」しかない。ローダー側の仕様が絡むので、後回し。

#### S-15 / S-16 `FT_Face` の扱い

- S-15 確認済み: ColorFont 設定が有効で、フェイスがカラーを持つとき、`FT_Glyph_To_BitmapEx` にフェイスを渡す（`ft.cpp:2186-2188`、`CS_LIBRARY` の下）。改造版 FreeType はここでフェイスにグリフスロットを足し、`FT_Load_Glyph` を呼ぶ（freetype の `src/base/ftglyph.c:930-934`、`src/base/ftobjs.c:4744-4806` で確認）。同じフェイスを、別スレッドは `CS_MANAGER` の下で使う（`ft.cpp:2123-2136`）。
- S-16 確認済み: `FreeTypeDrawInfo::GetFace` は `CS_MANAGER` の中で得たフェイスのポインタを保存し、ロックを離した後に使う（`ft.h:358-377`。使用例は `ft.cpp:2076` のカーニング、`2113` のスタイル判定）。
- S-16 推測: キャッシュマネージャは、フェイス数が上限（既定 64 以上）を超えると古いフェイスを閉じる。別スレッドの検索でそれが起きると、解放済みのフェイスを読む。65 書体以上を使い、かつ複数スレッドで描くプロセスに限られる。

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

---

## 3. 高速化候補

### 3.1 一覧

| ID | 内容 | 根拠 | 影響 | 難しさ | 確信度 |
|---|---|---|---|---|---|
| P-01 | アルファブレンドの表引き `conv2` が、画素の色成分ごとに実行時の割り算をする。シフトで置き換えられる | `ft.cpp:208-210, 388-395, 450-460` | 大 | 低 | 高 |
| P-02 | 空白など「絵のないグリフ」1 文字ごとに、GDI の `ExtTextOutW` を 1 回呼ぶ | `ft.cpp:2702-2707` | 中〜大 | 低〜中 | 中 |
| P-03 | グリフ 1 個ごとに `CGdippSettings::GetInstance()`（グローバルロック）を呼ぶ | `ft.cpp:2712`、`settings.cpp:136-146` | 小〜中 | 低 | 高 |
| P-04 | `GetInstance()` が、初期化済みでも毎回 `CS_SETTING` を取る | `settings.cpp:136-146`、`settings.h:262-271` | 中 | 中 | 中 |
| P-05 | キャッシュヒット時に、文字ごとに `CS_LIBRARY` を取る（参照カウントはすでに Interlocked） | `ft.cpp:1865-1866, 2411-2412`、`ftref.c:7-16` | 中 | 低（S-01 と同時） | 高 |
| P-06 | `ExtTextOutW` 1 回あたりのヒープ確保が多い（読んで数えた概算で 15〜20 回） | `override.cpp:1156-1158, 1262-1263, 1294, 1299`、`ft.h:223-229, 336-338`、`expfunc.cpp:199-202`、`ft.cpp:1798, 2880-2881`、`settings.cpp:1058` | 中 | 低〜中 | 高 |
| P-07 | `GetOutlineTextMetrics` を毎回 2 回呼ぶ（サイズ取得と本取得） | `override.cpp:1288, 1294-1300` | 中 | 低〜中 | 中 |
| P-08 | グリフキャッシュの間引きがキーの小さい順。使用頻度を見ない（MRU カウンタは更新だけされて使われていない） | `fteng.cpp:123-136, 254-264, 285-287`、`fteng.h:125-140, 228, 237, 263, 273` | 中（日本語環境） | 中 | 高 |
| P-09 | `DeleteObject` フックが、成功のたびにグローバルロックを取って表を検索する（置換フォントがなければ表は常に空） | `override.cpp:760-768, 425-435` | 小〜中 | 低 | 高 |
| P-10 | 起動時: `ControlIder` の静的初期化で `iswcntrl` を 36,863 回呼ぶ。除外・アンロード対象のプロセスでも走る | `ft.h:146-165`、`ft.cpp:62` | 中 | 低〜中 | 中 |
| P-11 | 起動時: D2D / DirectWrite を使わないプロセスでも、`d2d1.dll` と `dwrite.dll` を読み込む | `directwrite.cpp:1500-1506`、`hook.cpp:595-599` | 中〜大 | 中〜高 | 高（効果は推測） |
| P-12 | 起動時: DirectWrite 設定が無効でも、先に `IsRunAsUser()`（トークン情報の取得）を実行する | `hook.cpp:595, 279-409` | 小 | 低 | 高 |
| P-13 | 起動時: `undocAPI.h` の `static` 変数が、.cpp ごとに `LoadLibrary` / `GetProcAddress` を実行する | `undocAPI.h:72, 91-96`、`common.h:28` | 小 | 低 | 高 |
| P-14 | FontSubstitutes 有効時: 存在しないフォント名でフォントを作るたびに、フォント列挙が走る（見つからなかった結果を覚えない） | `fteng.cpp:38-57`、`settings.cpp:1745-1768` | 小〜中 | 中 | 中 |
| P-15 | 遅延初期化が、最初のフック呼び出しの中で `CS_SETTING` を握ったまま、レジストリ列挙とフォント列挙を大量に行う | `settings.cpp:136-146, 156-306, 1314-1525, 1596-1628, 1643-1672` | 中 | 高 | 中 |
| P-16 | `GetGlyphOutline` フックが、寸法だけの問い合わせのたびに `GetTextMetrics` を呼ぶ | `override.cpp:477-487, 526-531` | 小〜中 | 中 | 中 |
| P-17 | 作業用 DIB を 256 回ごとに「そのときの要求サイズ」へ縮め、直後にまた広げ直す | `cache.cpp:4, 22-41` | 小 | 低〜中 | 中 |
| P-18 | すべてのフック呼び出しが、プロセス共通のアトミックカウンタを増減する | `hook.cpp:60-66`、`hookCounter.h:10-16` | 小 | 中 | 中 |

### 3.2 各項目の説明

#### P-01 `conv2` の割り算

- 確認済み:
  - `conv2` は `tbl2[n / (BASE * BASE / (tbl2.size() - 1))]`（`ft.cpp:208-210`）。`tbl2.size()` は実行時の値なので、コンパイラは割り算を消せない。`CAlphaBlendColorOne::doAB`（`ft.cpp:388-395`）から、文字のある画素の R / G / B それぞれで呼ばれる。
  - `tbl2` は 4097 要素（`ft.cpp:185`）、`BASE` は 0x4000（`ft.cpp:217`）なので、割る数は常に 65536。`n` は 0 以上 `BASE * BASE` 以下。よって `n >> 16` と完全に同じ値になる。0 から `BASE * BASE` までの全値で一致することをプログラムで確認した（不一致 0 件）。
  - 再現ベンチ（合成ループを写したもの。Linux、g++ -O2）: 割り算版 約 13 ns/画素、シフト版 約 2.8 ns/画素。約 4.5 倍。
- 推測: MSVC でも同程度の差になる見込み。描画全体に占める割合は未計測だが、文字の画素数に比例する部分なので、文字の多い画面ほど効く。
- 描画結果: 変わらない（同値）。

#### P-02 空白ごとの GDI 呼び出し

- 確認済み: `TextOutCallback` は、ビットマップのバッファが NULL のグリフに対して、元の `ExtTextOutW` を 1 文字分呼ぶ（`ft.cpp:2702-2707`）。FreeType は、輪郭が空のグリフ（空白など）ではバッファを確保しない（freetype の `src/smooth/ftsmooth.c:635-636` で確認）。空白はキャッシュに「空のグリフ」として入るので、文字列中の空白 1 個ごとに、この GDI 呼び出しが起きる。
- 推測: 欧文では文字の 15〜20% が空白なので、1 回の描画に占める割合が大きい。日本語では小さい。
- 描画結果を変えないための条件（要確認）: 呼び出し先は背景透明モード（`override.cpp:1615`）なので、空白なら GDI は何も描かないはず。ただし下線・取り消し線つきフォントでは GDI が線を引く。省略するのは「グリフ番号指定でない」「文字が空白」「下線も取り消し線もない」ときに限るのが安全。実機で画素を比べてから採用する。

#### P-03 / P-04 設定取得のロック

- 確認済み: `GetInstance()` は毎回 `CS_SETTING` を取る（`settings.cpp:138`）。`ExtTextOutW` 1 回の中で、`override.cpp:1144`、`ft.cpp:1431, 1755, 2851, 3052` の 5 回に加えて、グリフごとに 1 回（`ft.cpp:2712`）呼ぶ。キャッシュにない文字を太らせるときは、さらに `ft.cpp:3229` でも呼ぶ。
- P-03: `ft.cpp:2712` の分は、ループの外（`FreeTypeTextOut`、`ft.cpp:2851`）で取ったものを渡せば済む。動作は変わらない。
- P-04: 初期化済みならロックなしで返す。ただし初期化済みフラグ `m_bDelayedInit` はビットフィールド（`settings.h:271`）で、同じバイトのほかのフラグと一緒に書き換えられる。再読み込み（S-11）との関係も整理が必要。
- 推測: ロックが競合しない限り、1 回あたりは数十ナノ秒。複数スレッドで描くと、全スレッドがここで直列になる。

#### P-05 キャッシュヒット時の `CS_LIBRARY`

- 確認済み: `FT_Glyph_Ref_Copy` は `InterlockedIncrement` でカウントを増やすだけ（`ftref.c:7-16`）。それをプロセス全体で 1 つの `CS_LIBRARY` で囲んでいる（`ft.cpp:1865-1866`）。`CS_LIBRARY` はラスタライズ中（`ft.cpp:2185-2199`）も握られるので、別スレッドが新しい文字をラスタライズしている間、キャッシュヒットだけのスレッドも待たされる。
- S-01 の修正で「検索〜参照コピー」を別のロックで囲めば、ここは不要になる。S-01 と同じ設計の中で扱う（ブランチは分ける）。

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
- 直し方の方向: 最初に使うとき（最初の `ExtTextOutW`）まで遅らせる。表の中身は同じになる。Win32 ビルドは関数内 static がスレッド安全でないので、既存のロックで守る。

#### P-11 `d2d1.dll` / `dwrite.dll` の読み込み

- 確認済み: `HookD2DDll` は、どちらの DLL も未ロードなら `LoadLibrary` する（`directwrite.cpp:1500-1506`）。条件は「対話ユーザーのプロセス かつ INI の `DirectWrite` が有効」（`hook.cpp:595`）だけで、そのプロセスが D2D を使うかどうかは見ない。`DirectWrite` の既定値は 0（`settings.cpp:631`）なので、INI で有効にしていなければ、この項目は関係ない。
- 推測: コンソールアプリや、短時間で終わるツールでも 2 つの DLL の読み込みが乗り、起動時間とメモリが増える。
- 直し方の方向: すでにロード済みなら今すぐフック。未ロードなら、DLL のロード通知（`LdrRegisterDllNotification`、Vista 以降）を受けて、その時点でフックする。フックの中身は変えない。ローダーロック中の `LoadLibrary` がなくなるので、S-13 にも効く。通知の登録・解除や、古い OS での代替が要るので、難しさは中〜高。

#### P-12 / P-13 起動時の小さな無駄

- P-12: `IsRunAsUser() && bEnableDW && ...` の順（`hook.cpp:595`）を入れ替えるだけで、DirectWrite 設定が無効のときのトークン取得（`hook.cpp:279-409`）を省ける。
- P-13: `undocAPI.h` の `static` 変数は、取り込む .cpp ごとに別々の実体になる。それぞれが `LoadLibrary("gdi32.dll")` を 4 回と `GetProcAddress` を実行する（`undocAPI.h:72, 91-92, 96`）。1 箇所の定義にまとめれば 1 回で済む。

#### P-14〜P-18

- P-14 確認済み: `GetFontLocalName` は、見つかったときだけキャッシュする（`fteng.cpp:51-52`）。見つからない名前は、毎回 `GetDC` → `EnumFontFamiliesEx` → `ReleaseDC`（`fteng.cpp:47-50`）。`lookup` 側の予備キャッシュのキーは、`LOGFONT` の先頭 28 バイト（数値部分）だけから作られ、フォント名を含まない（`settings.cpp:1722-1730, 1755-1756`）。ここを直すと置換の結果が変わりうるので、触るのは「見つからなかった結果のキャッシュ」だけにする。
- P-15: 遅延初期化の中身を「使うときに作る」形へ分ければ、最初の描画の待ちが減る。ただし初期化順に依存する箇所が多く（da2d7f1 の無限ループ修正など）、難しい。
- P-16: ClipBoxFix（既定で有効、`settings.cpp:662`）のために、寸法だけの `GetGlyphOutline` のたびに `GetTextMetrics` を呼ぶ。キャッシュするには「同じ DC・同じフォント」の判定が要る。
- P-17 確認済み: 256 回目の呼び出しで、要求サイズが今の DIB と違えば、要求サイズちょうどで作り直す（`cache.cpp:22-33`）。次に大きい要求が来ると、また作り直す（`cache.cpp:34-41`）。
- P-18: 効果は小さい見込み。アンロードの安全待ち（S-14）に使っているので、外すなら代わりの仕組みが要る。

---

## 4. 着手順の提案

「1 ブランチ 1 修正」なので、上の ID 1 つ（枝番があるものは枝番 1 つ）を 1 ブランチにする。

**段階 0: 確かめる手段を用意する（提案。T-01）**

「描画結果は変えない」を、目視ではなく機械的に確かめたい。案: 小さなテスト用 EXE を Actions で一緒にビルドする。EXE は `MacType64.Core.dll` を直接 `LoadLibrary` し、決まった文字列をメモリ DC に `ExtTextOutW` で描き、画素のハッシュを出力する。修正前後のハッシュが同じなら、描画は変わっていない。同じ EXE で繰り返し回数を増やせば、速度の比較にも使える。
- 推測: Core DLL は、ローダーなしで直接読み込んでもフックを張れる（`DllMain` がすべて自前で行うため）。Actions のランナー上で動くかどうかは未確認。
- これが難しければ、けんじさんの PC で `macloader64.exe` を使い、特定のアプリだけに新しい DLL を当てて目で確かめる方法になる（7 章）。

**段階 1: 小さくて、結果が同じと言い切れるもの（作業の流れに慣れる）**

1. P-01 `conv2` をシフトに（同値を証明済み。効果も大きい見込み）
2. P-03 グリフごとの `GetInstance()` をループの外へ
3. S-03 TLS 一覧と `TLSDCArray` にロック
4. S-04 `DCArray` にロック（空ならロックを取らない形）
5. P-12、P-09、S-19 b、S-18 のうち単純なもの

**段階 2: 効果の大きい安定性修正**

6. S-10 可変フォントの名前比較（(b) 例外 → (c) リーク → (a) 読み越しの順。Windows 11 で日常的に通る経路）
7. S-09 `HOOK` マクロの直列化
8. S-08 `GetTextFace` のバッファ長
9. S-01 グリフキャッシュのロック（先にロック範囲と順序を決める）→ P-05
10. S-02 所有者つきロックの修正
11. S-05、S-06、S-07

**段階 3: 効果の大きい高速化**

12. P-02 空白の GDI 呼び出し省略（画素比較ができてから）
13. P-08 間引きを使用頻度順に
14. P-10 `ControlIder` の遅延初期化
15. P-06 を 1 箇所ずつ、P-07（実機で戻り値を確かめてから）

**段階 4: 大きめの変更（設計してから）**

16. P-11 / S-13 `d2d1` / `dwrite` の遅延フック
17. P-04 `GetInstance()` のロックなし経路
18. S-11 設定再読み込みの排他
19. 残り（S-12、S-14〜S-17、P-13〜P-18）

upstream に Issue / PR を出すなら、再現条件を説明しやすく差分も小さい S-10、S-09、S-08、S-03、S-04、P-01 が候補。

---

## 5. 触らないもの・注意点

**直すと描画結果が変わるので、今回の方針では触らないもの（確認済みの不具合）**

- 演算子の優先順位の誤り: `flags & FT_LOAD_TARGET_LCD_V == FT_LOAD_TARGET_LCD_V`（`ft.cpp:2101, 2520, 2532`）。`==` が先に評価されるので、意図した判定になっていない。
- `CTempMem::init` の条件が逆（`ft.cpp:1302`）。大きいサイズを要求しても、バッファが大きくならない。メモリ破壊にはならない（呼び出し側が実サイズを渡している、`ft.cpp:2543-2555`）。FontLoader=WIN32 のときだけ通る。
- カーニングが、フェイスの「そのとき有効なサイズ」で計算される（`ft.cpp:2073-2082`）。
- 存在しないフォント名の予備キャッシュのキーに、フォント名が入っていない（P-14 参照）。
- DisplayAffinity の集合が、再読み込みで増えるだけで消えない（`settings.cpp:652-659`）。

**使われていないコード（触らない。有効化するなら要注意）**

- 未設置フックの `IMPL_CreateCompatibleDC` は、中でフック対象の `CreateCompatibleDC` 自身を呼ぶ（`override.cpp:1835`）。Detours 版には EasyHook のような再入防止がないので、このフックを設置すると無限再帰になる。`IMPL_DeleteDC` などが使う `ORIG_xxx` も、初期化されていない。
- `EmBoldVariableFont`（`ft.cpp:83-124`。呼び出しなし。中にリークあり）、`CFontSetCache`（`fteng.h:26-53`。`realloc` のサイズ指定が誤り）、`SafeUnload` と `expfunc.cpp` 側の `hook_term`（`expfunc.cpp:89-105, 145-175`。エクスポートされていない）、子プロセス向けのコード（`expfunc.cpp:215-1598` の大半と、`supinfo.h` の `_CreateProcess...` テンプレート。エクスポートも呼び出しもない）、`dll.cpp` の `CMemLoadDll`（`expfunc.cpp:268-271` からのみ使用）。

**外部との約束（変えない）**

- エクスポート 4 つと `IControlCenter` の仮想関数の並び（`settings.h:228-249`）。非公開のローダーや Tuner が使っている可能性がある。
- `MacType.ini` の項目名と意味。

**調べて、候補から外したもの**

- 縦書き用の `ft2vert_init`（フェイスを開くたびに呼ばれる、`ft.cpp:3205`）: GSUB テーブルの検証が重いのではと疑ったが、改造版 FreeType には otvalid モジュールが入っておらず（freetype の `include/freetype/config/ftmodule.h`、`builds/windows/vc2010/freetype.vcxproj` で確認）、検証はすぐ「未実装」で戻る。重くない。
- `FindFont(int faceid)` の「存在 bug！！！」コメント（`fteng.cpp:814`）: フェイス ID と配列の位置は、`CS_FONTENG` の下で一致するように増減されている（`fteng.cpp:585-594, 606-636`）。0 を渡す呼び出し元も見つからなかった。

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

1. **実機での確かめ方**。非公開のローダー（`MacType64.dll`）が Core DLL をどこから読むか、Core だけ差し替えて動くかは、コードからはわからない。`macloader64.exe アプリ名` で特定のアプリだけに当てる方法が安全そうだが、未確認。
2. **実際のクラッシュの原因**。2 章はコードから見つけた「起こりうる」問題。実際に落ちているアプリのクラッシュダンプや、イベントビューアの記録（落ちたモジュール名とオフセット）があれば、優先順位を付け直せる。
3. **速度の実測**。3 章の効果は、P-01 の再現ベンチ以外すべて推測。段階 0 のテスト用 EXE か、実機でのプロファイル（ETW など）で確かめたい。
4. **よく使う設定**。FontSubstitutes、FontLink、DirectWrite、ColorFont、FontLoader のどれを使っているかで、通る経路が変わる（S-07、S-08、P-14 は FontSubstitutes 有効時のみ、P-11 と DirectWrite 側のフックは `DirectWrite=1` のときのみ、など）。使っている `MacType.ini`（プロファイル）がわかれば、優先順位を絞れる。
5. **対応 OS の範囲**。Win32 ビルドに XP 向けの設定（`/Zc:threadSafeInit-`）が残っているが、Actions のツールセット（v143）は XP を対象にできない。Vista 以降の API（SRW ロック、`InitOnceExecuteOnce`、`LdrRegisterDllNotification`）を使ってよいかを決めたい。
6. **`CreateMessageWnd` が呼ばれているか**（S-12）、**動的アンロードをどれくらい使うか**（S-14）。ローダーやトレイの使い方による。
