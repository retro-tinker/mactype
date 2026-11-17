# S-01（グリフキャッシュのロック）レビュー

- 対象: ブランチ `fix/s01-glyph-cache-lock`（`ac092b7` コード、`d7ce59d` 計画書）。比較先は `directwrite`（`a7f7c87`）。
- レビュー日: 2026-11-17
- 行番号は、断りがなければこのブランチ（`d7ce59d`）の行。
- 「確認済み」はコードを読んで、または Actions のログで確かめたこと。「推測」は確かめていないこと。実機では何も試していない。

## 0. 結論

**マージを止める問題は見つからなかった。** 5 つの重点項目は、どれもコードと合っていた。

- デッドロック: `CS_FONTCACHE` は、持ったまま別のロックを取らない「末端のロック」のまま。新しくできた組 `CS_LIBRARY` → `CS_FONTCACHE` は、今ある向きと同じ。逆順の組はない（確認済み）。
- 守り漏れ: `m_GlyphCache` に触る場所は全部で 7 か所あり、すべて `CS_FONTCACHE` の中にある（確認済み）。
- 解放済みメモリ: ロックを離した後、キャッシュの中身へのポインタ（`chData`）は使っていない。グリフは自分の参照を持ってから離している（確認済み）。
- 再入: `CRITICAL_SECTION` は同じスレッドなら再入できる。回数を見るコードもない（確認済み）。
- 計画書の主張: ロックの順、ストレステストの数字、行番号のずれは合っていた。ただし、文書の更新漏れが 1 つと、書き足した方がよい残りの経路が 1 つある（R-1、R-2）。

マージ前に直すとよいのは、計画書の 2 点（R-1、R-2）。どちらも文書だけ。コードの変更は要らない。

## 1. デッドロック（ロックの取得順）

### 1.1 この変更で増えた組

| 場所 | 取る順 | 前との違い |
|---|---|---|
| 読む側 `ft.cpp:1865-1866`（FT）、`ft.cpp:2409-2410`（GGO） | `CS_LIBRARY` → `CS_FONTCACHE` | 前は「検索 → ヒットしたら `CS_LIBRARY`」。`CS_FONTCACHE` は取っていなかった |
| 書く側 `fteng.cpp:287`（`AddCharData`）、`fteng.cpp:334`（`AddGlyphData`） | `CS_FONTCACHE` だけ | 前は間引きのときだけ `CompactMap` の中（`fteng.cpp:126`）で取っていた |

- 確認済み: 読む側は、2 つの `CCriticalSectionLock` を同じブロックで宣言している。離す順は逆（`CS_FONTCACHE` → `CS_LIBRARY`）で、問題ない。
- 確認済み: 書く側の呼び出し元（`ft.cpp:2253-2261`、`ft.cpp:2656-2664`）は、その時点でどのロックも持っていない。ラスタライズ用の `CS_LIBRARY`（`ft.cpp:2153`、`2195`）と `CS_MANAGER`（`ft.cpp:1918`、`2133`）は、どれもブロックを抜けて離した後。`goto gdiexit` / `goto cont` で抜けてもデストラクタが走るので、持ったまま来ることはない。
- 確認済み: 外側で持っているのは `OCS_DC(1)`（`override.cpp:1252`）と `OCS_FREETYPE(1)`（`ft.cpp:3060`）だけ。キャッシュヒット時にはもともと `CS_LIBRARY` を取っていたので、「外側 → `CS_LIBRARY`」の組は前からある。

### 1.2 `CS_FONTCACHE` を持ったまま呼ぶものを、呼び出し先まで追った

`CS_FONTCACHE` を取る場所（読む側、書く側、`GetCache`、`FreeTypeFontInfo::Erase`）と、その中から呼ばれるもの（`~FreeTypeCharData`、`SetGlyph`）を、全部たどった。

| 場所 | 中で呼ぶもの | ほかのロック |
|---|---|---|
| 読む側（`ft.cpp:1868-1877`、`2412-2427`） | `FindChar` / `FindGlyphIndex`（`fteng.h:258-276`。`map::find` と `SetMruCounter`）、`GetGDIWidth` などの読み取り、`FT_Glyph_Ref_Copy`（`ftref.c:7-16`。`InterlockedIncrement` だけ） | なし |
| `AddCharData` / `AddGlyphData`（`fteng.cpp:287-324`、`334-375`） | `Compact` → `CompactMap`（`CS_FONTCACHE` を取り直す。再入）→ `delete FreeTypeCharData`。`SetGlyph`。`new FreeTypeCharData`。`map` の挿入 | なし（自分自身の再入だけ） |
| `~FreeTypeCharData`（`fteng.cpp:190-213`。上から呼ばれる） | `FT_Done_Ref_Glyph`（`ftref.c:18-27`）→ `FT_Done_Glyph` → FreeType の `ft_free`（`ftsystem.c`、中身は `free`）。`g_pFTEngine->SubMemUsed` / `SubMemUsedObj`（`fteng.h:599-618`。ただの足し算） | なし |
| `SetGlyph`（`fteng.cpp:215-231`） | `FT_Done_Ref_Glyph`、`FT_Glyph_Ref_Copy`、`AddMemUsed` | なし |
| `FreeTypeFontInfo::GetCache`（`fteng.cpp:449`） | `Compact` → `CompactMap(m_cache)` → `~FreeTypeFontCache` → `Erase` → `~FreeTypeCharData`。`new FreeTypeFontCache`（`AddMemUsedObj`）。`Activate` | なし |
| `FreeTypeFontInfo::Erase`（`fteng.h:515-524`） | `~FreeTypeFontCache` → 同上 | なし |

- 確認済み: どこにも、GDI などフックされる API、`LoadLibrary`、待ち合わせ（イベント、所有者つきロック）がない。ヒープのロック（`new` / `free`）は末端なので、組にならない。
- したがって `CS_FONTCACHE` は、自分自身の再入を除いて、持ったまま別のロックを取らない。**末端のロックなので、ロックの順による循環には入れない。** 新しい組 `CS_LIBRARY` → `CS_FONTCACHE` も、今ある設定反映（`fteng.h:623` → `640`）と同じ向き。
- 確認済み: `CS_LIBRARY` を参照コピーの直前（もとの位置）に残すと、`CS_FONTCACHE` → `CS_LIBRARY` の逆順ができる。設定反映とは `OCS_FREETYPE` の owner 違いで排他されるので、すぐにデッドロックにはならない。ただし S-02 で排他が破れると成り立たなくなる。前へ移した判断は正しい。
- 確認済み（Actions のログ）: `test/s01-stress`（run 44014998991）では、head の全実行で timeout は 0 回。ただし T-01 は設定反映（`ReloadAll`）とフォント削除（`RemoveFont`）を通らないので、その 2 つとの組み合わせは、コードを読んだだけ。

### 1.3 参考: 所有者つきロックとの関係

- 確認済み: 所有者つきロックを取るのは、`override.cpp:1252`、`ft.cpp:3060`、`fteng.cpp:556`、`fteng.h:622` の 4 か所だけで、どれもそのスレッドで最初に取るロック。普通のロックを持ったまま所有者つきロックを待つ経路はない。この変更で変わる点もない。

## 2. 守り漏れ（`m_GlyphCache` に触る場所）

`ft - non-ref.cpp` はビルドされない（`gdipp.vcxproj` の `ClCompile` にない）。`_USE_ARRAY` はどこでも定義されていないので、配列の側（`m_chars` / `m_glyphs`、`::Compact(T**)`）もビルドされない。残りの、`m_GlyphCache` に触る場所を全部並べた。

| 操作 | 場所 | 呼ばれる場所 | `CS_FONTCACHE` |
|---|---|---|---|
| 検索 | `_GetChar` / `_GetGlyph`（`fteng.h:223-240`）、`FindChar` / `FindGlyphIndex`（`fteng.h:258-276`） | `ft.cpp:1868-1870`、`2412-2414` だけ | あり（今回） |
| 追加・上書き | `AddCharData`（`fteng.cpp:300-322`）、`AddGlyphData`（`fteng.cpp:349-373`） | `ft.cpp:2256, 2259, 2659, 2662` | あり（今回） |
| 間引き | `FreeTypeFontCache::Compact`（`fteng.cpp:254-264`）→ `CompactMap`（`fteng.cpp:123-136`） | `AddCharData` / `AddGlyphData` の中だけ | あり（前から＋今回） |
| 全部の破棄 | `FreeTypeFontCache::Erase`（`fteng.cpp:266-278`。自分ではロックを取らない） | `~FreeTypeFontCache`（`fteng.cpp:248-252`）からだけ | 呼び出し元がすべて持っている（下） |
| 作成時の `clear` | コンストラクタ（`fteng.cpp:243`） | まだ共有されていない | 不要 |

`~FreeTypeFontCache` が呼ばれる場所（= `FreeTypeFontCache` 自体の削除）:

| 場所 | ロック |
|---|---|
| `FreeTypeFontInfo::Compact` の `CompactMap(m_cache, ...)`（`fteng.cpp:383`）。呼ぶのは `GetCache`（`fteng.cpp:452`）だけ | `CS_FONTCACHE`（`fteng.cpp:449`、`126`） |
| `FreeTypeFontInfo::Erase`（`fteng.h:515-524`）。呼ぶのは設定反映（`fteng.h:640`）と `~FreeTypeFontInfo`（`fteng.h:433`）。後者はフォント削除（`fteng.cpp:527, 544`）と DLL の動的アンロード（`hook.cpp:657`） | `CS_FONTCACHE`（`fteng.h:517`） |
| `GetCache` の `delete p`（`fteng.cpp:475`） | `CS_FONTCACHE`。しかも `m_cache[hash]=p` が非 NULL を返すので、通らない |

- 確認済み: マップそのもの（中身の読み書き）は、すべて `CS_FONTCACHE` の中になった。**守り漏れはない。**
- 確認済み: 一緒に守られるようになったもの: `FreeTypeFontCache` の `m_addcount` / `m_mrucount`（`AddIncrement`、`MruIncrement`、`ResetGCCounter`）、`FreeTypeCharData::m_mrucounter`。前は読む側がロックなしで書いていた。
- 残る問題（`FreeTypeFontCache` という「入れ物」の寿命）は R-1。マップの中身ではなく、入れ物へのポインタの話。

## 3. 解放済みメモリ（ロックを離した後のポインタ）

- 確認済み: 読む側で、ロックの中で得たポインタは `chData`（`FreeTypeCharData*`）だけ。`chData` を使うのはブロックの中（`ft.cpp:1872-1876`、`2416-2426`）だけで、ブロックを出た後には出てこない（`grep` で確認）。
- 確認済み: 手元に残るのは `*glyph_bitmap` で、これは `FT_Glyph_Ref_Copy` で参照カウントを 1 つ増やしたもの。キャッシュ側の `SetGlyph` や `~FreeTypeCharData` が自分の参照を手放しても、0 にはならない。参照コピーと手放す側は、どちらも `CS_FONTCACHE` の中なので、「手放した直後の 0 のグリフを拾う」ことも起きない。
- 確認済み: キャッシュから取ったグリフを、その場で書き換えるコードはない。太字化、斜体、`advance.x += 0x10000`、ビットマップ化（`ft.cpp:2162-2211`）は、すべてキャッシュにない場合（`ft.cpp:1880` の `if (!*glyph_bitmap)` の中）で、新しく作ったグリフにだけ行う。
- 確認済み: 書く側は、渡したグリフを `SetGlyph` で参照コピーして持つ。呼び出し元は自分の参照をそのまま使い続けるので、問題ない。
- 確認済み: キャッシュにヒットしたときは `glyph_index` が 0 のまま（値を入れるのは、キャッシュにない場合の中だけ）。だから、ヒットしたグリフを書く側へ渡し直すことはない（`ft.cpp:2253` の `if (glyph_index)`）。
- 残るもの: `pftCache`（入れ物）へのポインタは、`GetCache`（`ft.cpp:1527`）でロックを離した後も使い続ける。R-1。

## 4. `Compact` の中で `CS_FONTCACHE` を取り直すこと（再入）

- 確認済み: `CCriticalSectionLock` は `CRITICAL_SECTION` の薄い包み（`common.h:110-155`）。同じスレッドは何度でも入れて、同じ回数だけ出れば空く。`CompactMap` の取り直し（`fteng.cpp:126`）は、関数を抜けるときに 1 段戻るだけ。
- 確認済み: 再入の回数（`RecursionCount`）を見るのは `CCriticalSectionLockTry::CritalCount`（`common.h:341`）だけで、どこからも使われていない。`TryEnter` も使われていない。
- 確認済み: `GetCache` → `FreeTypeFontInfo::Compact` → `CompactMap(m_cache)` → `~FreeTypeFontCache` も、同じく再入するだけ（前から）。
- 確認済み: `AddCharData` の中で、間引き（`Compact`）は `find` より前にある。間引きで消えた項目のイテレータを使うことはない。
- 問題なし。

## 5. 計画書 S-01「状態」の主張とコードの照合

| 主張 | 結果 |
|---|---|
| 変更は `ft.cpp` +10 / −2、`fteng.cpp` +6 | 合っている（`git diff --stat`） |
| 読む側は `CS_LIBRARY` → `CS_FONTCACHE`。今ある順と同じ向き | 合っている（1 章） |
| もとの位置の `CS_LIBRARY` だと逆順ができるので前へ移した | 合っている（1.2） |
| 書く側は `CS_FONTCACHE` だけ。呼び出し元はほかのロックを持っていない | 合っている（1.1） |
| `CS_FONTCACHE` を持ったまま別のロックを取る経路は、変更後もない | 合っている（1.2。全部たどった） |
| 1.6 に足す組は `OCS_FREETYPE(1)` → `CS_LIBRARY` → `CS_FONTCACHE` の 1 つ | 組としては合っている。**ただし 1.6 にまだ書き足されていない**（R-2） |
| ビルド成功（run 44013646059、Win32 / x64） | 合っている。対象は `ac092b7`。`d7ce59d` は `.md` だけなので走らないが、コードは同じ |
| 単スレッドの画素は 553 ケースすべて同じ（run 44013646015） | 合っている。ログに「base vs head (single thread): all 553 cases identical」 |
| ストレステストの表（A〜D の回数と落ちた回数） | **すべて合っている**。run 44014998991 のログの `STRESS` 行を足し上げた: A base 36/36、A head 4/36、B base 36/36、B head 0/36、C head 0/18、D base 6/6、D head 6/24、D 1 本ずつ 0/6。timeout と画素の不一致はすべて 0 |
| 条件 B の base の 36 回の落ちた場所（1872 が 23、グリフの解放が 8、2708 が 3、`SetGlyph` が 2） | 合っている（各回の呼び出し履歴の、最初の MacType のフレームで数えた） |
| A の head の 4 回は、すべて `override.cpp:1134` → `_recalloc`（x64 は `tlsdata.h:30`） | 合っている（4 回とも呼び出し履歴を見た）。S-03 の場所そのもの |
| `test/s01-stress` の DLL は、このブランチと同じソース | 合っている。`ac092b7` との差は `.github/`、`tools/t01/`、`docs/` だけ |
| 行番号のずれ（ft.cpp 1862〜 +5、1872〜 +4、2402〜 +9、2418〜 +8、fteng.cpp 285〜 +3、329〜 +6） | 合っている（`git diff -U0` の範囲から計算） |
| 「落ちなくなった条件」（各スレッドの最初の描画を 1 本ずつにする） | 数字は合っている。この条件でも base は 36 回すべて落ちているので、S-01 の競合は条件 B でも起きている。だから「B で head が 0 回」は、S-01 が直った根拠として使える |
| 「残りは S-03」 | 呼び出し履歴があるのは A の 4 回で、4 回とも S-03 の場所。**これは確認済み**。push で走った T-01（run 44013646015）の x64 の 1 回と、ページヒープの 6 回は、呼び出し履歴がない。S-03 と見るのは推測（計画書も、ページヒープの方は推測と書いている） |

## 6. 指摘

### R-1（中・計画書に書き足す）: `FreeTypeFontCache` 自体の寿命は、この修正の外にある

- 確認済み: 描画スレッドは `GetCache`（`ft.cpp:1527`）で `pftCache` を受け取り、そこで `CS_FONTCACHE` を離す。その後、ループの間ずっと `pftCache` を使う（`ft.cpp:1869`、`2256`、`2259` など）。この間に `FreeTypeFontCache` が `delete` されると、今回のロックの中で解放済みの入れ物を触る。
- 削除する経路は 3 つある（2 章の表）。計画書の「直していないもの」は 1 つ目しか書いていない。
  1. `GetCache` の間引き（`fteng.cpp:383`）。描画スレッドどうしは排他されないので、本当に重なりうる。ただし 1 つのフォントで CacheMaxSizes を超えるサイズを使ったときだけ（計画書のとおり）。
  2. 設定反映（`fteng.h:640` → `fteng.h:515-524`）。描画とは `OCS_FREETYPE` の owner 違い（`fteng.h:622` と `ft.cpp:3060`）でしか排他されていない。
  3. フォント削除（`fteng.cpp:527, 544` → `fteng.h:433`）。描画とは `OCS_DC` の owner 違い（`fteng.cpp:556` と `override.cpp:1252`）でしか排他されていない。
- 2 と 3 は、所有者つきロックが正しく動けば重ならない。だが S-02（排他破れ）が起きると重なる。S-01 の修正は、ここを守っていない。
- 提案: S-01 の「この修正で直していないもの」に、2 と 3 を「所有者つきロック頼み。S-02 が起きると重なる」として書き足す。コードは変えない（直すのは S-02 の方）。

### R-2（小・計画書の更新漏れ）: 1.5 と 1.6 が修正前のまま

- 確認済み: 「状態」には「1.6 の『描画（グリフ取得中）』に足す組は…」とあるが、1.6 本体は変わっていない。マージ後に 1.6 だけを読むと、古い情報になる。
  - 1.5 の表、グリフキャッシュの行の「ロック」列（`docs/kaizo-plan.md:171`）: 「検索と追加は取らない」のまま。
  - 1.6 の `CS_LIBRARY` の取得箇所（`docs/kaizo-plan.md:188`）: `ft.cpp:1865, 2411` は、修正後は検索の前（`ft.cpp:1865`、`2409`。このブランチの行）に移った。
  - 1.6 の `CS_FONTCACHE` の取得箇所（`docs/kaizo-plan.md:192`）: 読む側 2 か所と書く側 2 か所（`ft.cpp:1866, 2410`、`fteng.cpp:287, 334`）が足りない。
  - 1.6 の取得順（`docs/kaizo-plan.md:220`）: 「`CS_FONTCACHE`、`CS_LIBRARY`、`CS_SETTING`（それぞれ単独）」に、`CS_LIBRARY` → `CS_FONTCACHE` を足す。
- 提案: このブランチで、上の 4 行だけを直す（文書だけなので、ビルドは走らない）。

### R-3（小・性能。P-05 / P-08 の設計で考える）: `CS_FONTCACHE` は、全フォント共通の 1 本

- 確認済み: `CS_FONTCACHE` はプロセスに 1 本で、フォントやサイズごとではない。今回から、すべてのキャッシュ検索と追加がこれを取る。
- 確認済み: 間引き（2048 回の追加ごとに、1024 個を超える分を `delete` し、グリフを `free`）は、この 1 本を持ったまま走る。その間、別のフォントを描いているスレッドの検索も待つ。前も間引きは `CS_FONTCACHE` の中だったが、検索は待たなかった。
- 確認済み: キャッシュにない文字でも、検索の前に `CS_LIBRARY` を取るようになった（計画書にも記載）。`CS_LIBRARY` は、別スレッドのラスタライズ（`ft.cpp:2195-2210`）の間ずっと握られるので、検索がその後ろに並ぶ。
- 確認済み: 速度は単スレッドでしか比べていない（±6% の揺れの内側）。複数スレッドでは base が落ちるので比べようがない。
- 推測: 1 つのプロセスで多くのスレッドが同時に描くとき（ブラウザの UI など）に、少し待ちが増える。落ちるよりはずっとよいので、今回の判断（既存のロックを使う）は妥当。
- 提案: 計画書の P-05 に「`CS_LIBRARY` を外すと、キャッシュにない文字の検索がラスタライズを待たなくなる」、P-08 に「間引きの間、全フォントの検索が止まる」を書き足す。

### R-4（小・壊れにくさ）: `chData` がブロックの外で宣言されている

- 確認済み: `chData` は、ロックのブロックの外（`ft.cpp:1857`、`2401`）で宣言されている。今はブロックを出た後に使っていないので問題ないが、ブロックを出た後も変数としては生きている。P-05 などで、この辺りを書き換えるときに、ロックの外で `chData` を使うコードを足すと、解放済みメモリを読む。
- 提案: 今回はコードを変えない（差分最小）。P-05 の項目に「`chData` はブロックの外で使わない」と注意を書いておく。

### R-5（参考・新しい問題ではない）: S-18 f の NULL で落ちる場所が、2 本のロックの中になった

- 確認済み: 要求したモードのグリフがキャッシュにないと、`GetGlyph` が NULL を返し、`FT_Glyph_Ref_Copy` が NULL を読む（S-18 f）。修正前も `CS_LIBRARY` の中で落ちていた。今回から `CS_FONTCACHE` も持ったまま落ちる。
- 確認済み: ビルドされるコードに `__try` / `__except` はない（`directwrite.cpp:1311` はコメントの中、`easyhook.h` は使われない）。普通はそのままプロセスが落ちるだけ。
- 推測: OS がウィンドウプロシージャの中の例外を握りつぶす場合（古い Windows の 32 ビットプロセスなど）、ロックが持たれたまま残り、そのプロセスの文字描画がすべて止まる。修正前も `CS_LIBRARY` で同じことが起きうるので、悪化ではない。
- 提案: S-18 f を、S-03 の次あたりに上げる。

### R-6（小・計画書の表現）: 「約 1 割」の範囲

- 2.1 の表の「S-03 の場所で、約 1 割の実行がまだ落ちる」は、条件 A（40 回中 5 回 = 12.5%）の数字。ページヒープ（条件 D）では 24 回中 6 回（25%）。「T-01 の複数スレッドのモード（8 スレッドが同時に最初の描画）で 40 回中 5 回」と、条件を添えて書いた方が誤解がない。

### R-7（提案・T-01）: S-03 が直るまで、複数スレッドのチェックは head の悪化を見つけられない

- 確認済み: T-01 のワークフローは、base が落ちたら head が落ちても警告にとどめる。base は今後も毎回落ちる（S-01 と S-03 の両方がある）ので、push で走る T-01 では、複数スレッドのモードで head が悪化しても失敗にならない。
- 提案: S-03 が直るまでの間、push で走る T-01 の複数スレッドのモードにも `--serial-first-draw` を使う（`test/s01-stress` で作ったもの）。別の ID（T-01 の改良）として扱う。

## 7. 確かめていないこと

- 実機（実際のアプリ）での動き。けんじさんの PC ではまだ試していない。
- 設定反映（`ReloadAll`）とフォント削除（`RemoveFont`）を、描画と同時に走らせたときの動き。T-01 では通らないので、コードを読んだだけ。
- `ForEachGetGlyphGGO`（FontLoader=1）側。読む側・書く側とも FT 側と同じ形で、コードを読んだ限り問題はないが、実行はしていない（計画書のとおり）。
- 複数スレッドでの速度。
