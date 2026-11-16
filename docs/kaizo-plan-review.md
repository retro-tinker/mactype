# kaizo-plan.md の検証結果

`docs/kaizo-plan.md`（別の AI が作った改造候補リスト）を、実際のコードと照らし合わせて確かめた結果。範囲は依頼どおり次の 3 つに絞った。

1. 4 章の段階 1〜2 に入っている項目: P-01, P-03, S-03, S-04, S-10, S-09, S-08, S-01, P-05, S-02
2. 1.6 章の「所有者つきロックは、同じ owner 同士なら同時に入れる」という説明と、ロックの取得順の一覧
3. P-01 の「シフトに置き換えても結果が完全に同じ」という主張

| 項目 | 内容 |
|---|---|
| 対象 | `docs/kaizo-plan` ブランチ `8e5eccf`。ソースは `693ca92`（`directwrite`）と同一であることを `git diff --stat` で確認（差分は `CLAUDE.md` と `docs/` だけ） |
| 行番号 | すべて `693ca92` 時点のもの（計画書と同じ） |
| 依存ライブラリ | microsoft/Detours `1dbd4ea`、snowie2000/freetype `ef77157`（計画書と同じコミット） |
| 方法 | ソースを読む静的調査。P-01 は式を写したプログラムを Linux（g++ -O2）で実行。所有者つきロックは状態遷移を 1 行ずつ手で追った。Windows 実機での実行・計測はしていない |
| 調査日 | 2026-11-17 |

判定の意味: **正しい** = 主張・根拠ともコードと合う（補足があっても結論は変わらない）。**一部違う** = 結論の一部、または説明の仕方がコードと合わない。**間違い** = 結論がコードと合わない。

---

## 0. 結論

| 対象 | 判定 | ひとこと |
|---|---|---|
| 1.6 所有者つきロックの説明 | **一部違う** | 「同じ owner なら何スレッドでも同時に入れる」は、すでにその owner が持っているときだけ。待ちに入った同じ owner のスレッドは 1 本ずつしか起きない。owner=2 も「単独」ではない |
| 1.6 ロックの取得順 | **一部違う** | 「逆順の組はない」には同意。ただし一覧に抜けがあり、`OCS_FREETYPE(1)` が描画の前半しか覆っていないことが書かれていない |
| P-01 `conv2` の割り算 | **正しい** | 自分で全域を確かめた。不一致 0。前提は「設定値が正常範囲」 |
| P-03 グリフごとの `GetInstance()` | **正しい** | |
| S-03 TLS 一覧と `TLSDCArray` | **正しい** | |
| S-04 `DCArray` | **正しい** | 補足 1 点（DC のリーク） |
| S-10 可変フォントの名前比較 | **正しい** | 補足 2 点（例外の後遺症、`to_utf16le` の符号拡張） |
| S-09 COM フック設置の競合 | **正しい** | |
| S-08 `GetTextFace` のバッファ長 | **正しい** | 起きる条件の補足と、A 版の別のバグ |
| S-01 グリフキャッシュ | **正しい** | 壊れ方がもう 1 経路ある（同じ文字の上書き） |
| P-05 キャッシュヒット時の `CS_LIBRARY` | **正しい** | 補足: 今の `CS_LIBRARY` は実質何も守っていない |
| S-02 所有者つきロックの排他破れ | **正しい** | 手で追って同じ結果。加えて `Leave` 側にも同系統の穴 |

計画を覆すような見落としは見つからなかった。ただし、**修正の設計に影響するもの**がいくつかある（3 章）。特に S-01 は「間引き」だけでなく「同じ文字の上書き」でも解放済みメモリを読むので、ロック範囲はそれも含めて決める必要がある。

---

## 1. 1.6 章の検証

### 1.1 所有者つきロックの説明 — 一部違う

計画書の説明: 「同じ owner 同士は何スレッドでも同時に入れる（`ownedcs.cpp:27-31`）。つまり読み手/書き手ロックに近い」「owner=2（単独で入る側）」。

コードの実際（`ownedcs.cpp:24-61`）:

- 同じ owner が**すでに持っているとき**だけ、`nOwner == Owner` で合流できる（`ownedcs.cpp:27-30`）。ここは計画書のとおり。
- それ以外のスレッドは、owner に関係なく `nRequests` を 1 つ増やして待ち行列に入る（`ownedcs.cpp:34-37`）。イベントは自動リセット（`ownedcs.cpp:13` の `CreateEvent(NULL, false, ...)`）で、解放 1 回につき 1 本しか起きない（`ownedcs.cpp:54-55`）。
- そのため、**待ちに入った owner=1 のスレッドどうしは 1 本ずつ順に入る**。例: owner=2 が持っている間に描画スレッド T2、T4（どちらも owner=1）が来ると、owner=2 が抜けた後に入れるのは T2 だけで、T4 は T2 の世代が全員抜けるまで待つ。
- さらに、**後から来た owner=1 は、待っている owner=1 や owner=2 を追い越して合流する**（`nOwner == 1` の間は無条件に合流するため）。描画が絶えないプロセスでは、owner=2（フォント削除 `fteng.cpp:550`、設定反映 `fteng.h:622`）がいつまでも入れない（飢餓）。
- ロックが空いた直後（`ownedcs.cpp:40` で内部ロックを離してから `41` で `nOwner` を書くまで）に来た同じ owner のスレッドも、`nOwner` がまだ -1 なので待ち行列に入る。
- owner=2 も同じ仕組みなので、**owner=2 どうしは同時に入れる**。「単独で入る側」ではない。実際には、`RemoveFont` は関数全体で `CS_MANAGER`（`fteng.cpp:551`）、`ReloadAll` は `CS_LIBRARY`（`fteng.h:623`）を持つので、その内側で直列になっている。

状態遷移を 1 行ずつ追った結果（`nOwner / nRec / nReq`）:

| 手順 | 状態 | 備考 |
|---|---|---|
| T1(o1) が入る | 1 / 1 / 0 | |
| T3(o2) が来る | 1 / 1 / 1 | 待ち |
| T1 が抜ける | -1 / 0 / 0、イベント 1 | T3 が起きる |
| T3 が入る | 2 / 1 / 0 | |
| T2(o1)、T4(o1) が来る | 2 / 1 / 2 | 2 本とも待ち |
| T3 が抜ける | -1 / 0 / 1、イベント 1 | 起きるのは 1 本だけ |
| T2 が入る | 1 / 1 / 1 | **T4 は owner=1 なのに待ち続ける** |
| T5(o1) が来る | 1 / 2 / 1 | **後から来た T5 は T4 を追い越して入る** |

結論として、「描画スレッドどうしを排他しない」（S-01 などの前提）は正しい。ただ、「読み手/書き手ロックに近い」と言えるのは「描画が owner=1 を持っている最中」だけで、待ち行列に入ると同じ owner でも直列になる。性能面では、フォント削除や設定反映の直後に描画スレッドが 1 本ずつ流れる形になる（推測: 体感できるほどかは不明）。

### 1.2 ロックの取得順の一覧 — 一部違う

「読んだ範囲では逆順になる組は見つからなかった」には**同意する**。下に挙げる抜けを足しても、逆順の組はできなかった。`CS_SETTING`、`CS_FONTCACHE`、`CS_CACHEDFONT` は、持ったまま他のロックを取る経路がない（末端）。`CS_LIBRARY` と `CS_MANAGER` へ入ってくる辺は、所有者つきロックからだけだった。

ただし、一覧には次の抜けと不正確な点がある。

1. **`OCS_FREETYPE(1)` は描画の前半しか覆っていない。** `OCS_FREETYPE(1)` を取るのは `FreeTypeGetGlyph` の中（`ft.cpp:3046`）で、関数を抜けると離す（`ft.cpp:3064`）。合成の `FreeTypeTextOut`（`override.cpp:1618` → `ft.cpp:2823-3035`）は `OCS_DC(1)` だけで走る。この区間で `CS_SETTING` を取る（`ft.cpp:2851, 2712`）。一覧の書き方（`OCS_DC(1) → OCS_FREETYPE(1) → ...`）だと、描画全体が `OCS_FREETYPE` の中にあるように読める。設定反映（`OCS_FREETYPE(2)` だけ）は、この後半とは同時に走れる。
2. 描画側の抜け:
   - `CS_FONTLINK → CS_SETTING`（`fteng.h:484-486` → `fteng.cpp:394`）
   - `CS_MANAGER → CS_SETTING`（フェイスを開くとき。`ft.cpp:3162` → `fteng.cpp:852`）
   - `CS_MANAGER → CS_CACHEDFONT`（同じ経路で `DeleteFont`、`fteng.cpp:1008`。`DeleteObject` フックは常設なので設定によらない。`override.cpp:760-767, 428`）
   - FontSubstitutes 有効時: `CS_FONTENG → CS_SETTING → CS_CACHEDFONT`（`FreeTypeFontInfo` のコンストラクタの `CreateFont`、`fteng.h:388-391` → フック `override.cpp:712, 753` → `420`）
3. フォント削除側の抜け:
   - `CS_MANAGER → CS_FONTENG` の直接の辺（`fteng.cpp:551` → `566`）
   - `CS_FONTMAP → CS_CACHEDFONT`（`delete fontinfo` → `DeleteFont(m_ggoFont)`、`fteng.cpp:521` → `fteng.h:434` → `override.cpp:766`）
4. 設定反映側の抜け: `ReloadConfig` は `m_bDelayedInit = false` にしてから `ReloadAll` を呼ぶ（`settings.h:1007, 1011`）。このため `ReloadAll` の中の `GetInstance`（`fteng.h:624`）で遅延初期化がもう一度走り、`CS_LIBRARY → CS_SETTING →（FontSubstitutes 有効時）CS_CACHEDFONT`（`settings.cpp:143, 1468`）になる。
5. 所有者つきロックどうしの関係: フォント削除は `OCS_DC(2)` だけ、設定反映は `OCS_FREETYPE(2)` だけを取る。**この 2 つは互いに排他されない**（3.1 参照）。

---

## 2. 段階 1〜2 の各項目

### P-01 `conv2` の割り算 — 正しい

自分で確かめたこと:

- 式は `tbl2[n / (BASE * BASE / (tbl2.size() - 1))]`（`ft.cpp:208-210`）。`tbl2` は 4097 要素（`ft.cpp:185`）、`BASE` は 0x4000（`ft.cpp:217`）。`tbl2.size()` は `size_t` なので、割る数は `size_t` の 65536 になり、**`n` は符号なしに変換されてから割られる**（Win32 は 32 ビット、x64 は 64 ビットの符号なし割り算）。
- `n` の範囲: 呼び出し元は実質 `CAlphaBlendColorOne::doAB` だけ（`ft.cpp:392-393`）。`CAlphaBlend::doAB`（`ft.cpp:450-460`）を呼ぶ `DoAlphaBlend` → `AlphaBlendColor(LCD)`（`ft.cpp:462-522`）は、どこからも呼ばれていない。`n = tbl1[bg] × (BASE − t) + tbl1[fg] × t` で、`tbl1` は 0〜BASE（`ft.cpp:311, 314`）。`t = tunetbl[alpha]` は `Bound(0, alphatbl[k], BASE)`（`ft.cpp:329-341`）。
  - 注意: `Bound` の引数は `(値, 最小, 最大)`（`common.h:373`）なので、この呼び方は実際には `max(0, alphatbl[k])` で、上限 BASE は効いていない。ただ `alphatbl` 自体が 0〜BASE に収まる（`ft.cpp:276-285`。重み・コントラストは INI 読み込みで 0.0625 以上に制限、`settings.cpp:603-604`）ので、結果は変わらない。
  - よって `n` は 0 以上 BASE² 以下。
- プログラムで確かめた結果（付録 A）:
  1. `n` = 0〜BASE²（268,435,457 通り）の全値で、Win32 の割り方・x64 の割り方・`n >> 16` の 3 つが一致。不一致 0。
  2. ガンマ 5 通り × 重み 4 × コントラスト 5 × GammaMode 4（-1, 0, 1, 2）の 400 設定で、`init()` を写して実際の表を作り、(fg, bg, alpha) の全組み合わせ（約 57.6 億回）で出力の画素値を比べた。不一致 0。`n` の実測範囲は [0, 268435456]、`tunetbl` と `tbl1` はどちらも [0, 16384]。
  3. 速さの目安（x86-64、Xeon 2.1GHz、g++ -O2）: 1 成分あたり、割り算版 約 7 ns、シフト版 約 1〜2 ns。計画書の「約 4.5 倍」と同じ程度。

補足:

- 「完全に同じ」は**設定値が正常範囲のとき**の話。`IControlCenter::SetFloatAttribute`（`settings.h:816-832`）はガンマなどの範囲を見ずに表を作り直す。ガンマに 0 以下が入ると `tbl1[0]` が `INT_MIN` になり（付録 A の [3]）、`n` が負やオーバーフローになる。このときは今の割り算でもシフトでも範囲外を読む（どちらも未定義動作で、値は違う）。修正で悪くなることはない。
- 書き方の提案: `tbl2[(unsigned)n >> 16]` にすると、Win32 の今の動作と**全ての `int` で**ビット単位で同じになる（x64 とは `n ≥ 0` で同じ）。表の大きさを変えたときにずれないよう、`static_assert` か定数で 4096 を縛っておくとよい。
- 影響「大」は推測のまま。1 文字あたりの GDI 呼び出しや `BitBlt` と比べてどれくらいの割合かは測っていない。Win32 は 32 ビットの割り算なので、x64 より差は小さい見込み（推測）。

### P-03 グリフごとの `GetInstance()` — 正しい

- `TextOutCallback` の中で、絵のあるグリフごとに `GetInstance()`（`CS_SETTING` を毎回取る、`settings.cpp:136-146`）を呼ぶ（`ft.cpp:2712`）。呼び出し元は `FreeTypeTextOut` のループだけ（`ft.cpp:2945`）で、ループの前に同じものを取っている（`ft.cpp:2851`）。
- `GetInstance()` が返すのは、プロセス起動時に 1 回だけ作る `s_pInstance`（`settings.cpp:110-124`）で、描画中に別物にはならない。外で取ったポインタを渡しても同じオブジェクトを読む。
- 細かい違い: 別スレッドが `ReloadConfig` で `m_bDelayedInit = false` にした直後だと、今はループの途中で遅延初期化が走りうる。直すとそれがループの外（次の `GetInstance`）に移る。画素は変わらない。
- 渡し方は `FreeTypeGlyphInfo`（`ft.cpp:524-533`）に 1 つ足すか、`FTInfo` 経由。どちらでも差分は数行。

### S-03 TLS 一覧と `TLSDCArray` — 正しい

- `CTlsData::GetPtr` は、初回だけ `new T` → `pArray->Add`（`tlsdata.h:21-31`）。`CPtrArray` は ATL の `CSimpleArray` 系（`array.h:51`）で、追加時に再確保する。ロックはない。
- `CThreadLocalInfo` のコンストラクタ／デストラクタが `std::set` の `TLSDCArray` に insert／erase（`override.h:71-79`）。ロックはない。
- `GetPtr` を呼ぶのは `IMPL_ExtTextOutW` の先頭近く（`override.cpp:1134`）。空白 1 文字の早期リターン（`1131-1132`）より後なので、文字を描く全スレッドが初回に通る。
- 削除は `DLL_THREAD_DETACH`（`hook.cpp:633-634` → `tlsdata.h:69-79`）。ここはローダーロックの中なので、削除どうしは直列。ただし、他スレッドの初回描画とは同時に走る。
- `RemoveFont` は `TLSDCArray` を走査する（`fteng.cpp:575-580`）。

修正時の注意: 削除側はローダーロックの中で新しいロックを取ることになる（ローダーロック → 新ロック）。新ロックを持ったまま `LoadLibrary` など、ローダーロックを待つものを呼ばないこと。`GetPtr` 側は `new T`（GDI を呼びうる）の外で、配列への追加だけを囲むのが安全。

### S-04 `DCArray` — 正しい

- `BeginPath` / `EndPath` / `AbortPath` のフックが `std::set<HDC>` をロックなしで insert / erase（`override.cpp:629, 640, 651`）。`ExtTextOutW` が毎回ロックなしで find（`override.cpp:1139`）。
- 計画書の「空ならロックを取らない」案は正しく動く。同じスレッドで `BeginPath` → `ExtTextOutW` と呼んだ場合、自分の書き込みは自分から見える。

補足（計画書にない）: パス作成中のまま `EndPath` を呼ばずに DC を消すと、その `HDC` は集合に残り続ける。同じ値の `HDC` が再利用されると、その DC では MacType が描かなくなる（今もそうなっているはず。推測）。また、集合が空にならないので「空ならロックを取らない」近道が効かなくなる。直すと描画が変わるので、S-04 では触らないこと。

### S-10 可変フォントの名前比較 — 正しい

- (a) 読み越し: `wstring(reinterpret_cast<wchar_t*>(fn.string), fn.string_len)`（`ft.cpp:1325, 1336`）は `string_len` を文字数として扱い、`2 × string_len` バイトを読む。FreeType はちょうど `string_len` バイトを確保する（freetype `src/base/ftsnames.c:68-70`、`82`）。計画書のとおり。
- (b) 例外: Mac プラットフォーム（`ft.cpp:1329`）と、Windows プラットフォームの符号化 0/1 以外（`ft.cpp:1339`）を `to_wide_string`（`common.cpp:16-20`、`std::wstring_convert<codecvt_utf8>`）に渡す。不正な UTF-8 で `std::range_error` が出る。`IMPL_ExtTextOutW` の `ETO_TRY` / `ETO_CATCH` は名前だけで、`try` / `catch` ではない（`override.cpp:1001-1003`）。どこでも捕まらない。
- (c) リーク: `ft.cpp:1386` の早期 `return` で `FT_Done_MM_Var` を呼ばない。

補足（計画書にない）:

1. **(b) の後遺症**: 例外で `IMPL_ExtTextOutW` を抜けると、`pTLInfo->InExtTextOut(true)`（`override.cpp:1270`）が戻されない（戻すのは `1687, 1691`）。アプリが例外を捕まえて動き続けた場合、そのスレッドは以後ずっと再入扱い（`override.cpp:1196-1198`）になり、MacType が効かなくなる。作業配列（`1262-1263`）と、`face_requester` で開いたフェイス（`ft.cpp:3162-3211`）もリークする。
2. **(a) を直すときの注意**: Windows 名前の変換 `to_utf16le`（`common.cpp:3-14`）は、`char` を `char16_t` に直接キャストしている（`common.cpp:9-10`）。下位バイトが 0x80 以上だと符号拡張で上位バイトが 0xFF になり、非 ASCII の名前は正しく変換されない。計画書の「Windows プラットフォームは結果を変えずに直せる」は、**読み越しだけを直し、この変換はそのまま残す**場合に正しい。変換も直すと、非 ASCII のインスタンス名を持つフォントで、選ばれるインスタンスが変わりうる。
3. 起きる条件の絞り込み: `getSfntName` を呼ぶのは、名前 ID がインスタンスの `strid` と一致するレコードだけ（`ft.cpp:1367`）。Mac 名前が ASCII だけなら (b) は起きない。「そういうフォントが実在するかは未確認」は妥当。

### S-09 COM フック設置の競合 — 正しい

- `HOOK` マクロ（`directwrite.cpp:28-35`）は「`ISHOOKED` を見る → `ORIG_xxx` に vtable の値を代入 → `hook_demand_xxx`」で、全体を守るロックがない。Detours は関数本体を書き換え、vtable の値（元の関数の先頭）はそのまま残る。後から代入されると、`ORIG_xxx` が「書き換え済みの元関数」を指し、`IMPL → ORIG → REF → IMPL` の無限再帰になる。計画書のとおり。
- `GetAlphaBlendParams` は 3 箇所（`directwrite.cpp:625, 797, 881`）で、それぞれ別の `static bool loaded`。Win32 は `/Zc:threadSafeInit-`（`gdipp.vcxproj:509`）なので、1 箇所の中でも 2 回走りうる。
- `hook_demand_xxx` は `DetourTransactionBegin` / `DetourAttach` の戻り値を見ずに `IsHooked_xxx = true` にする（`hook.cpp:113-118`）。Detours は 1 プロセスで同時に 1 トランザクションだけで、他スレッドの途中だと `Begin`（`detours.cpp:1873-1881`）も `Attach`（`detours.cpp:2319-2321`）も `ERROR_INVALID_OPERATION` を返す。確認した。
- 補足: `hook.cpp:117` の `&ORIG_##name` はグローバル変数のアドレスなので常に真。チェックとして意味がない（動作には影響しない）。
- 修正案の「`HOOK` マクロ全体を `CS_DWRITE` で囲む」で足りる。`ComMethodHooker`（`directwrite.cpp:46-57`）も中で `HOOK` を使うので一緒に守られる。`HookRenderTargetMethod` の呼び出し側はすでに `CS_DWRITE` を持っている（`directwrite.cpp:484, 499, 514`）が、`CRITICAL_SECTION` は再入できるので問題ない。

### S-08 `GetTextFace` のバッファ長 — 正しい

- `IMPL_GetTextFaceAliasW` は `nLen` を見ずに `StringCchCopy(lpAliasW, LF_FACESIZE, ...)`（`override.cpp:447`）。`IMPL_GetTextFaceA` は `c` を見ずに `StringCchCopyA(lpName, LF_FACESIZE, ...)`（`override.cpp:590`）。W 版は `Min(LF_FACESIZE, c)`（`override.cpp:567-568`）。計画書のとおり。
- `FontCache` に入れるのはフォント作成フック（`override.cpp:753`）だけで、このフックは FontSubstitutes 有効時だけ設置（`hook.cpp:601-603, 434-451`）。計画書のとおり。

補足:

1. 起きる条件をもう少し正確に: あふれるのは「置換**前**の名前の長さ + 1 > アプリのバッファ長」のとき。`StringCchCopy` は元の文字列の長さまでしか書かないので、32 文字ちょうどを書くとは限らない。
2. **A 版の別のバグ**（計画書にない）: 名前の変換に CRT の `wcstombs`（`override.cpp:585-592`）を使っている。CRT のロケールは既定で "C" なので、日本語などの名前では変換に失敗して -1 を返し、`nSize` が 0 になる（`override.cpp:592`）。本来は `WideCharToMultiByte(CP_ACP, ...)`。またアプリが UTF-8 ロケールにしていると、`2 × 文字数 + 1` バイト（`585`）では足りず、終端なしで切れる（`StringCchCopyA` が 32 バイトで止めるので、読み越しは最大 32 バイト）。描画は変わらないが、API の戻り値が変わるので、S-08 とは別の ID にするのがよい。

### S-01 グリフキャッシュ — 正しい

- 検索 `FindChar` / `FindGlyphIndex`（`fteng.h:258-276`）、追加 `AddCharData` / `AddGlyphData`（`fteng.cpp:280-369`）とも、`std::map` をロックなしで触る。間引き `CompactMap` は `CS_FONTCACHE` を取る（`fteng.cpp:126`）が、検索・追加が取らないので意味がない（計画書 1.5 の表の「ロック: なし」は厳密には違うが、結論は同じ）。
- キャッシュヒット時の `CS_LIBRARY`（`ft.cpp:1865`）は、検索（`1857-1859`）と `GetGDIWidth` / `GetAAMode`（`1863-1864`）の**後**で取る。計画書のとおり。
- 呼び出し元のロックは `OCS_DC(1)` と `OCS_FREETYPE(1)` だけ。1.1 のとおり、これは描画スレッドどうしを排他しない。

補足（計画書にない、もう 1 つの壊れ方）:

- `AddCharData` / `AddGlyphData` は、同じキーがすでにあると `SetGlyph` で**中のグリフを差し替える**（`fteng.cpp:297-307, 343-354`）。`SetGlyph` は古いグリフを `FT_Done_Ref_Glyph` で手放す（`fteng.cpp:219-220`）。参照カウントが 0 になれば即解放（`ftref.c:18-27`）。
- 2 つのスレッドが同じ文字を同時に取りこぼすと、両方がラスタライズして両方が追加し、後の方が先の方のグリフを差し替える。その間に 3 本目のスレッドが `FindChar` → `GetGlyph` まで進んでいると、解放済みのグリフに `FT_Glyph_Ref_Copy` することになる。間引き（2048 回に 1 回）と違い、**新しい文字が最初に出たときに毎回起きうる**。
- モノクロと非モノクロが混ざる場合（S-18 f）にも同じ差し替えが起きる。

修正の設計への影響: ロックで囲む範囲は「検索〜参照コピー」と「追加・間引き」に加えて、`SetGlyph` での差し替えも含める（`AddCharData` 全体を囲めば入る）。いちばん単純なのは、新しいロックを作らず `CS_FONTCACHE` を使うこと。`CompactMap` がすでにこれを取っているので再入で済み、ラスタライズ（`CS_LIBRARY`）とも競合しない。取得順は `CS_LIBRARY → CS_FONTCACHE`（`ReloadAll`）なので、`CS_FONTCACHE` を持ったまま `CS_LIBRARY` を取らないこと（つまりラスタライズをロックの外に置く）。

推測: Explorer のように、ウィンドウごとに別スレッドで同じ UI フォント・同じサイズを描くアプリはよくあるので、計画書が見込むより起きやすいかもしれない。

### P-05 キャッシュヒット時の `CS_LIBRARY` — 正しい

- `FT_Glyph_Ref_Copy` は `InterlockedIncrement` だけ（`ftref.c:7-16`）。`CS_LIBRARY` はラスタライズ中も握られる（`ft.cpp:2185-2200`）。計画書のとおり。
- 補足: 今の `CS_LIBRARY`（`ft.cpp:1865`）は、実質何も守っていない。参照を手放す側の `SetGlyph`（`fteng.cpp:220`）と `~FreeTypeCharData`（`fteng.cpp:203-208`）は `CS_LIBRARY` を取らない。`ReloadAll` は `CS_LIBRARY` の中でキャッシュを消す（`fteng.h:623, 640`）が、読み手は `CS_LIBRARY` を取る前に `chData` を手に入れているので、これも防げない。
- 計画書のとおり、S-01 と同じ設計で扱うのが安全（S-01 のロックで「検索〜参照コピー」を囲めば、`CS_LIBRARY` は外せる）。

### S-02 所有者つきロックの排他破れ — 正しい

- `EnterOwnedCritialSection` は、内部ロックを離した後で `nOwner`（`ownedcs.cpp:41`）と `nRecursiveCount`（`42`）を書く。手で追った結果:

| 手順 | `nOwner / nRec / nReq` |
|---|---|
| T1(o1) が `40` で内部ロックを離し、`41` で `nOwner = 1` | 1 / 0 / 0 |
| T2(o1) が来て合流（`29`） | 1 / 1 / 0 |
| T1 が `42` で `nRec = 1`（T2 の分が消える） | 1 / 1 / 0 |
| T1 が抜ける（`53-55`） | -1 / 0 / -1 |
| T3(o2) が来て、待たずに入る | 2 / 1 / 0 |
| → **T2(o1) と T3(o2) が同時に中にいる** | |

- 補足: `LeaveOwnedCritialSection` は、`nOwner` が自分の owner でないとき、**他の owner のカウントを減らす**（`ownedcs.cpp:58-59`）。上の例の後で T2 が抜けると、T3 のカウントが 0 になる。T3 側に別のスレッドが合流していれば、そのスレッドが中にいるのに解放されうる。破れが連鎖する。
- 計画書の再現実験（スレッドを使ったもの）は再実行していない。上は状態遷移を 1 行ずつ追った確認。
- 修正の方向（計画書に追加）: 待たない経路は、`40` の前に `nOwner` と `nRec` を内部ロックの中で書く。待つ経路は、起きた後でもう一度内部ロックを取ってから書く（起きた時点では `nOwner == -1` なので、他の誰も合流していない）。`58-59` の分岐は、上の 2 つを直せば通らなくなる。1.1 の飢餓は直らないので、直すなら別の ID にする。

---

## 3. 計画書にない問題（追加の提案）

計画書のルール（CLAUDE.md）では、計画書にない問題は ID を付けて計画書に足すことになっている。今回は計画書を変えていないので、ID は案として書く。

| 案 | 内容 | 根拠 | 影響 | 難しさ | 確信度 |
|---|---|---|---|---|---|
| S-20 | フォント削除と設定反映が互いに排他されない。`RemoveFont` は `OCS_DC(2)` + `CS_MANAGER` + `CS_FONTMAP` の下で `m_mfullMap` から消して `FreeTypeFontInfo` を `delete` する。`ReloadAll` は `OCS_FREETYPE(2)` + `CS_LIBRARY` の下で、`CS_FONTMAP` なしに `m_mfullMap` を走査する。同時に走ると、解放済みの `FreeTypeFontInfo` を触るか、壊れたイテレータで進む | `fteng.cpp:493-522, 550-551`、`fteng.h:619-647` | 小（同時に起きることがまれ） | 低〜中 | 高（実害の頻度は推測） |
| S-21 | `GetTextFaceA` フックの名前変換が CRT ロケール依存（`wcstombs`）。日本語名で失敗して 0 を返す。UTF-8 ロケールでは終端なしで切れる（FontSubstitutes 有効時のみ） | `override.cpp:585-592` | 小 | 低 | 高 |
| S-22 | 所有者つきロックの飢餓。描画中は後から来た owner=1 が無条件に合流するので、フォント削除・設定反映がいつまでも入れない。待ち行列に入った owner=1 は 1 本ずつしか起きない | `ownedcs.cpp:27-31, 34-37, 54-55` | 小〜中（応答性） | 中 | 高（体感への影響は推測） |

既存項目への補足（ID は増やさず、計画書の各項目に書き足す案）:

- S-01: 同じ文字の上書き（`SetGlyph`）でも解放済みグリフを読む（2 章 S-01）。ロック範囲の設計に入れる。
- S-02: `Leave` 側でも他の owner のカウントを減らす（2 章 S-02）。
- S-10: 例外の後、そのスレッドで MacType が効かなくなる。`to_utf16le` の符号拡張（2 章 S-10）。
- S-04: `EndPath` なしで DC を消すと集合に残る（2 章 S-04）。
- 1.6: `OCS_FREETYPE(1)` の範囲と、取得順の抜け（1.2）。

着手順について一言: S-01 は「新しい文字が出るたび」に起きうるとわかったので、段階 2 の 9 番目より前（S-03 / S-04 の次あたり）に上げる価値がある。ただし S-01 の修正は全描画が通る場所なので、段階 0 の画素比較の仕組み（T-01）ができてから、という順番は変えない方がよい。

---

## 付録 A: P-01 の確認プログラム

`ft.cpp:156-343` の表の作り方と、`ft.cpp:388-395` の合成を写したもの。g++ -O2（x86-64 Linux）で実行。MSVC と同じく、`float` → `int` で NaN や ∞ を変換すると `INT_MIN` になる。

<details>
<summary>p01.cpp</summary>

```cpp
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <climits>
#include <algorithm>
template<typename T> inline T Bound(T x, T m, T M) { return (x < m) ? m : ((x > M) ? M : x); } // common.h:373
static const int BASE = 0x4000;
struct AB {
  std::vector<int> alphatbl=std::vector<int>(256), tbl1=std::vector<int>(257), tunetbl=std::vector<int>(256);
  std::vector<unsigned char> tbl2=std::vector<unsigned char>(256*16+1);
  unsigned char rconv1(int n){ int pos=0x80,i=pos>>1; while(i>0){ if(n>=tbl1[pos]) pos+=i; else pos-=i; i>>=1;} if(n>=tbl1[pos]) ++pos; return (unsigned char)(pos-1);}
  void init(float gamma,float weight,float contrast,int mode){
    int i; float temp, alpha;
    for(i=0;i<256;++i){
      temp = std::pow((1.0f/255.0f)*i, 1.0f/weight);
      if(temp<0.5f) alpha = std::pow(temp*2, contrast)/2.0f; else alpha = 1.0f - std::pow((1.0f-temp)*2, contrast)/2.0f;
      alphatbl[i]=(int)(alpha*BASE);
      if(mode<0) temp=(1.0f/255.0f)*i;
      else if(mode==1){ temp = i<=10 ? (float)i/(12.92f*255.0f) : std::pow(((1.0f/255.0f)*i+0.055f)/1.055f, 2.4f); }
      else if(mode==2){ temp = i<=10 ? ((float)i/(12.92f*255.0f)+(float)i/255.0f)/2 : (std::pow(((1.0f/255.0f)*i+0.055f)/1.055f,2.4f)+(float)i/255.0f)/2; }
      else temp = std::pow((1.0f/255.0f)*i, gamma);
      tbl1[i]=(int)(temp*BASE);
    }
    tbl1[i]=BASE;
    for(i=0;i<=(int)(tbl2.size()-1);++i) tbl2[i]=rconv1(i*(BASE/(int)(tbl2.size()-1)));
    // TuneTable は恒等とした。実際の tunetbl の値は alphatbl の値の部分集合なので、
    // alpha を 0〜255 全部回せば、取りうる t はすべて試される。
    for(i=0;i<256;++i) tunetbl[i]=Bound(0, alphatbl[Bound(i,0,255)], BASE);
  }
};
int main(){
  const uint32_t d32 = (uint32_t)(BASE*BASE) / (uint32_t)4096;          // Win32: size_t は 32 ビット
  const uint64_t d64 = (uint64_t)(int64_t)(BASE*BASE) / (uint64_t)4096; // x64
  long long bad=0;
  for (int64_t n=0; n<=(int64_t)BASE*BASE; ++n){
    int ni=(int)n;
    if ((uint32_t)ni/d32 != (uint32_t)(ni>>16) || (uint64_t)(int64_t)ni/d64 != (uint64_t)(ni>>16)) ++bad;
  }
  printf("[1] n=0..BASE^2: mismatches=%lld\n", bad);
  float gammas[]={0.0625f,1.0f,1.4f,2.2f,20.0f}, weights[]={0.0625f,1.0f,1.7f,10.0f}, contr[]={0.0625f,0.7f,1.0f,1.3f,10.0f}; int modes[]={-1,0,1,2};
  long long total=0, mism=0; int nmin=INT_MAX, nmax=INT_MIN;
  AB ab;
  for(float g:gammas) for(float w:weights) for(float c:contr) for(int m:modes){
    ab.init(g,w,c,m);
    for(int fg=0;fg<256;fg++) for(int bg=0;bg<256;bg++) for(int al=0;al<256;al++){
      int t=ab.tunetbl[al]; if(!t) continue;                 // ft.cpp:392 と同じく t==0 は割り算に来ない
      int n = ab.tbl1[bg]*(BASE-t) + ab.tbl1[fg]*t;
      nmin=std::min(nmin,n); nmax=std::max(nmax,n);
      unsigned char a = ab.tbl2[(uint64_t)(int64_t)n / d64], b = ab.tbl2[(uint32_t)n / d32], s = ab.tbl2[n>>16];
      ++total; if(a!=s || b!=s) ++mism;
    }
  }
  printf("[2] evaluations=%lld mismatches=%lld n=[%d,%d]\n", total, mism, nmin, nmax);
  ab.init(-1.0f,1.0f,1.0f,0); printf("[3] gamma=-1: tbl1[0]=%d\n", ab.tbl1[0]);
}
```

</details>

実行結果:

```
[1] n=0..BASE^2: mismatches=0
[2] evaluations=5763235840 mismatches=0 n=[0,268435456]
[3] gamma=-1: tbl1[0]=-2147483648
```

[1] は n = 0〜BASE²（268,435,457 通り）、[2] は 400 設定 × (fg, bg, alpha) の全組み合わせ（t = 0 を除く）。
