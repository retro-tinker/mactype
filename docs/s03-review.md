# S-03 レビュー（`fix/s03-tls-array-lock`）

- 対象: `directwrite`（`db4c8af`）との差分。`9c79202`（コード 4 ファイル +12 行）と `6fe6d56`（計画書）。2026-11-17。
- やったのはコードを読むことだけ。実機でも Actions でも、何も走らせていない。

## 結論

- **デッドロック**: ローダーロックと `CS_TLSDATA` の循環は、MacType のロックの範囲では起きない（確認済み）。残るのは gdi32 の内部で、ここはローダーロックを取らないと見ている（推測）。
- **取得順の逆転**: 見つからなかった（確認済み）。
- **守り漏れ**: `pArray` と `TLSDCArray` への触り方は、`ProcessTerm` の走査を除いて、すべてロックで囲まれている。`ProcessTerm` はコミットメッセージにあるとおり範囲外。
- **マージしてよいと思う。** ただし、`RemoveFont` の経路は T-01 で一度も通っていない（4 章）。

## 1. デッドロック

循環になるには、「`CS_TLSDATA` を持ったスレッドが、直接か間接に、ローダーロックを待つ」ことが要る。`CS_TLSDATA` を取るのは次の 3 か所。

| 場所 | 入る時点で持っているロック | `CS_TLSDATA` の中でやること |
|---|---|---|
| 最初の描画（`override.cpp:1134` → `override.h:74`、`tlsdata.h:31`） | なし | 集合・配列への追加（ヒープだけ） |
| スレッドの終了（`hook.cpp:634` → `tlsdata.h:78`、`override.h:80`） | ローダーロック | 集合・配列からの削除（ヒープだけ） |
| `RemoveFont` の走査（`fteng.cpp:583`） | `OCS_DC(2)`、`CS_MANAGER` | スレッドの数だけ `~CBitmapCache` |

上の 2 つは、ヒープの操作だけで、ローダーロックも別のロックも待たない（確認済み）。問題は 3 つ目。

**`RemoveFont` を呼ぶ側**（確認済み）

- 呼ぶのは `IMPL_RemoveFontResourceExW`（`override.cpp:843`）だけ（`852` はコメントの中）。フックの入口なので、MacType のロックは何も持っていない。
- アプリが自分の `DllMain` から `RemoveFontResourceEx` を呼ぶと、走査するスレッドがすでにローダーロックを持っている。このとき終わりかけのスレッドはローダーロックを取れず、`CS_TLSDATA` まで来ないので、循環にはならない。（ローダーロックを持ったまま `OCS_DC(2)` を待つこと自体は、S-03 の前からある。S-13 の系統）

**走査の中で呼ぶもの**（`~CBitmapCache`、`cache.h:95-104`）

- `DeleteDC`: フックは設置されていない（`hooklist.h:9` が `HOOK_MANUALLY` で、`hook_demand_DeleteDC` を呼ぶ場所がない）。gdi32 をそのまま呼ぶ（確認済み）。
- `DeleteBitmap` / `DeleteObject`: `IMPL_DeleteObject`（`override.cpp:760`）→ `DeleteCachedFont` → `CS_CACHEDFONT`。`CS_CACHEDFONT` を持つ場所（`override.cpp:420, 428`）でやるのは、`map` の操作と `new` / `delete` だけ。ほかのロックもローダーロックも待たないので、`CS_CACHEDFONT` を経由した間接の待ちもない（確認済み）。
- gdi32 の `DeleteDC` / `DeleteObject` 自体: 消すのは、メモリ DC（`cache.cpp:10`）、DIB セクション、単色ブラシ。メモリ DC の元になる DC は、`IsValidDC`（`override.cpp:1284`。`DT_RASDISPLAY` だけ通す）を通ってから作られる（`override.cpp:1322`）。だから、プリンタ DC と互換の DC（ユーザーモードのプリンタドライバの `FreeLibrary` がありうる）にはならない（ここまで確認済み）。残りはカーネルへのシステムコールで、ローダーロックは取らないと見ている（**推測**。デバッガでは確かめていない）。ほかのソフトの DLL が `DeleteObject` / `DeleteDC` をフックしている環境のことは、読んでもわからない。

→ 循環は作れない。終わりかけのスレッドが `CS_TLSDATA` で待つのは、走査（GDI の削除をスレッドの数だけ）が終わるまで。その間は、プロセス全体で `LoadLibrary` やスレッドの開始・終了も止まるが、短い。

**ほかに確かめたこと**（確認済み）

- `~CThreadLocalInfo` のロックは本体の終わりで離れ、`m_bmpCache` のデストラクタはその後に走る。`RemoveFont` が先に壊した `CBitmapCache` を、持ち主のスレッドがもう一度壊しても、ハンドルは NULL に戻してあるので二重解放にはならない。
- `RemoveFont` がほかのスレッドの `CBitmapCache` を壊すとき、そのスレッドが使っている最中でないことは `OCS_DC` が保証している。キャッシュに触るのは `override.cpp:1253` 以降で、`1252` の `OCS_DC(1)` の中だけ。`ft.cpp:3103` の `GetPtr` はコメントの中（`3091-3156`）。

## 2. 取得順

- この差分でできる組: ローダーロック → `CS_TLSDATA`、`OCS_DC(2)` → `CS_MANAGER` → `CS_TLSDATA` → `CS_CACHEDFONT`。
- 逆向きの組（`CS_CACHEDFONT`、`CS_MANAGER`、`OCS_DC` を持ったまま `CS_TLSDATA` を待つ。`CS_TLSDATA` を持ったままローダーロックを待つ）はない。`IMPL_ExtTextOutW` を中から呼ぶのは `IMPL_TextOutW`（`override.cpp:867`）と `IMPL_ExtTextOutA`（`970`）で、どちらもロックを持たずに呼ぶ。だから最初の描画のときに `CS_TLSDATA` の外側に何かがあることはない。
- 計画書 1.6 の記述（227〜229 行）と一致する。

## 3. 守り漏れ・気づいた点

- a. `ProcessTerm`（`tlsdata.h:60-66`）は、`pArray` をロックなしで走査して `delete` する。`FreeLibrary` のときに、ほかのスレッドが最初の描画をすると、`Add` の `realloc` と重なる。コミットメッセージどおり S-03 の範囲外で、S-14 と同じ種類。直すなら S-14 で。
- b. コミット `9c79202` のメッセージは「`DeleteDC` のフックが `CS_DCRELATION` を取る」と書いているが、フックは設置されていない。計画書（2.2 の S-03）では訂正済みなので、実害はない。
- c. 計画書に書いた行番号（`tlsdata.h:31, 78`、`override.h:74, 80`、`fteng.cpp:583`）は、今のコードと合っている。
- d. 改行: 4 ファイルとも LF のファイルに LF で足していて、混ざっていない。`CS_TLSDATA` は列挙の 14 番（配列は 20 本）で、既存の番号は変わらない。描画結果: 足したのはロックの取得だけで、処理の中身と順番は同じ（コミットメッセージに同意）。

## 4. テストの穴

- T-01（`test/s01-stress` の `tools/t01/t01.cpp`）は `AddFontResourceEx` を呼ぶだけで、`RemoveFontResourceEx` は呼ばない。だから 402 回の結果は、追加・削除の側の確認にはなるが、この差分でいちばん長くロックを持つ `RemoveFont` の走査は、一度も通っていない。1 章のこの部分は、コードを読んだだけ。
- 提案: 条件 E（短命のスレッドを次々に作る）と同時に、別のスレッドで「テスト用フォントを `AddFontResourceEx(FR_PRIVATE)` → そのフォントで描く → `RemoveFontResourceEx`」を繰り返すモードを足す。消す前にそのフォントで描いておかないと、走査（`bIsFontFileLoaded` が真のとき）に入らない。S-17 で `RemoveFont` を触るときにも使える。
