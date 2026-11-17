// T-01: MacType の描画結果を機械的に比べるテスト用 EXE（docs/kaizo-plan.md 4 章 段階 0）
//
// MacType.Core.dll / MacType64.Core.dll を直接 LoadLibrary し、決まった文字列を
// メモリ DC に ExtTextOutW で描いて、画素のハッシュを出す。
//
//   1. DLL を読み込む前に、同じケースを素の GDI で描いてハッシュを取る（基準）
//   2. DLL を読み込む（DllMain がフックを張る。INI は DLL と同じフォルダの MacType.ini）
//   3. 同じケースをもう一度描き、ハッシュを出す
//      - 素の GDI と同じハッシュのケースがあれば「MacType が効いていない」として失敗
//      - --repeat N: 2 回目以降（キャッシュに載った状態）も 1 回目と同じ画素かを確かめ、時間を測る
//      - --threads T: T 本のスレッドが、同じケースを同時に描く（S-01〜S-03 の確認用）。
//        落ちないことと、全スレッド・全ラウンドの画素が同じことを確かめる
//
// 出力（--out）: 1 行 1 ケース、タブ区切り
//   id  hash(MacType)  hash(GDI)  色つき画素数(MacType)  色つき画素数(GDI)
// 先頭が '#' の行は情報。比べるときは id と hash(MacType) だけを見る（run-t01.ps1）。
//
// 終了コード: 0 = 問題なし、1 = 検査に失敗、2 = 日本語フォントがない（--list-fonts）、
//             3 = DLL を読み込めない、4 = 引数の誤り、それ以外 = 例外で落ちた（例外コード）

#include <windows.h>
#include <dbghelp.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- 文字列

static const wchar_t* const kTextEN  = L"The quick brown fox jumps over the lazy dog. 0123456789";
static const wchar_t* const kTextSYM = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ ©®±×÷€→←↑↓…";
static const wchar_t* const kTextJP  = L"日本語の表示テスト。ひらがな・カタカナ・漢字「全角」（括弧）１２３ＡＢＣ";

// 欧文フォント（Windows に標準で入っているもの）
static const wchar_t* const kFacesEN[] = { L"Segoe UI", L"Arial", L"Times New Roman", L"Consolas" };
// 日本語フォントの候補。MS Mincho は Default.ini の [Individual] に載っている（NormalWeight=30）
static const wchar_t* const kFacesJP[] = { L"Meiryo", L"Yu Gothic", L"MS Gothic", L"MS Mincho" };

static const int kHeights[] = { -11, -13, -16, -21, -32 };   // LOGFONT の lfHeight（文字の高さ、px）

enum TextKind { TK_EN, TK_SYM, TK_JP, TK_SWEEP };
enum DrawMode { DM_OPAQUE, DM_TRANSPARENT };
enum Variant  { V_PLAIN, V_DX, V_ZOOM, V_WIDTH };

static const wchar_t* const kTextKindName[] = { L"EN", L"SYM", L"JP", L"SWEEP" };
static const wchar_t* const kModeName[]     = { L"opaque", L"transparent" };
static const wchar_t* const kVariantName[]  = { L"plain", L"dx", L"zoom", L"width" };

struct Case {
	std::wstring id;
	std::wstring face;
	int height;
	int weight;
	bool italic;
	TextKind text;
	DrawMode mode;
	Variant variant;
	int w, h;   // 画面（DIB）の大きさ
	bool linked;   // フォント自身に文字がなく、フォントリンクなどに頼るケース（MacType が描かないこともある）
};

struct Result {
	uint64_t hash = 0;
	uint32_t colorPx = 0;   // R, G, B がそろっていない画素の数（LCD で描かれたかの目安）
	double drawMs = 0;      // 文字を描く部分だけの時間（下地の塗りとハッシュは含めない）
};

static std::wstring g_sweepText;   // 漢字を多く並べた文字列（グリフキャッシュの間引きを起こすため）
static const int kSweepPerLine = 60;

// ---------------------------------------------------------------- 小物

static double NowMs()
{
	static LARGE_INTEGER freq = {};
	if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static std::string ToUtf8(const std::wstring& s)
{
	if (s.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
	return out;
}

static FILE* g_out = nullptr;   // --out のファイル

// コンソールと --out の両方に出す情報行（先頭に '#'）
static void Info(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("# %s\n", buf);
	fflush(stdout);
	if (g_out) { fprintf(g_out, "# %s\n", buf); fflush(g_out); }
}

static void Fail(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("FAIL: %s\n", buf);
	fflush(stdout);
	if (g_out) { fprintf(g_out, "# FAIL: %s\n", buf); fflush(g_out); }
}

static std::string Hex(uint64_t v)
{
	char b[32];
	snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
	return b;
}

// ---------------------------------------------------------------- 落ちたときの記録

static std::wstring g_symPath;   // PDB を探すフォルダ（DLL の置き場所）
static volatile LONG g_crashing = 0;

// 落ちたスレッドの呼び出し履歴を、PDB があれば関数名と行番号つきで出す
static void PrintStack(const CONTEXT* ctxIn)
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
	if (!SymInitializeW(proc, g_symPath.empty() ? nullptr : g_symPath.c_str(), TRUE)) {
		printf("  (SymInitialize failed: %lu)\n", GetLastError());
		return;
	}
	CONTEXT ctx = *ctxIn;
	STACKFRAME64 sf = {};
#ifdef _WIN64
	const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	sf.AddrPC.Offset = ctx.Rip; sf.AddrFrame.Offset = ctx.Rbp; sf.AddrStack.Offset = ctx.Rsp;
#else
	const DWORD machine = IMAGE_FILE_MACHINE_I386;
	sf.AddrPC.Offset = ctx.Eip; sf.AddrFrame.Offset = ctx.Ebp; sf.AddrStack.Offset = ctx.Esp;
#endif
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 48; ++i) {
		if (!StackWalk64(machine, proc, GetCurrentThread(), &sf, &ctx, nullptr,
			SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
			break;
		const DWORD64 pc = sf.AddrPC.Offset;
		char modName[MAX_PATH] = "?";
		const DWORD64 base = SymGetModuleBase64(proc, pc);
		if (base) {
			char full[MAX_PATH];
			if (GetModuleFileNameA((HMODULE)(ULONG_PTR)base, full, MAX_PATH)) {
				const char* s = strrchr(full, '\\');
				lstrcpynA(modName, s ? s + 1 : full, MAX_PATH);
			}
		}
		alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
		SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
		memset(symBuf, 0, sizeof(symBuf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 511;
		DWORD64 disp = 0;
		char where[800];
		if (SymFromAddr(proc, pc, &disp, sym))
			snprintf(where, sizeof(where), "%s!%s+0x%llx", modName, sym->Name, (unsigned long long)disp);
		else
			snprintf(where, sizeof(where), "%s+0x%llx", modName, (unsigned long long)(pc - base));
		IMAGEHLP_LINE64 line = {};
		line.SizeOfStruct = sizeof(line);
		DWORD ldisp = 0;
		if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
			const char* f = strrchr(line.FileName, '\\');
			printf("  #%02d %s  (%s:%lu)\n", i, where, f ? f + 1 : line.FileName, line.LineNumber);
		} else {
			printf("  #%02d %s\n", i, where);
		}
	}
	fflush(stdout);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
	// 複数のスレッドが同時に落ちたら、最初の 1 本だけ記録する
	if (InterlockedCompareExchange(&g_crashing, 1, 0) != 0) {
		Sleep(INFINITE);
	}
	const EXCEPTION_RECORD* er = ep->ExceptionRecord;
	wchar_t mod[MAX_PATH] = L"?";
	HMODULE hm = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)er->ExceptionAddress, &hm) && hm) {
		GetModuleFileNameW(hm, mod, MAX_PATH);
	}
	printf("CRASH: exception 0x%08lx at %p (module %s + 0x%llx), thread %lu\n",
		(unsigned long)er->ExceptionCode, er->ExceptionAddress, ToUtf8(mod).c_str(),
		(unsigned long long)((const char*)er->ExceptionAddress - (const char*)hm), GetCurrentThreadId());
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		printf("  access violation: %s address %p\n",
			er->ExceptionInformation[0] == 0 ? "read" : (er->ExceptionInformation[0] == 1 ? "write" : "execute"),
			(void*)er->ExceptionInformation[1]);
	fflush(stdout);
	PrintStack(ep->ContextRecord);
	if (g_out) {
		fprintf(g_out, "# CRASH: exception 0x%08lx module %s\n", (unsigned long)er->ExceptionCode, ToUtf8(mod).c_str());
		fflush(g_out);
	}
	TerminateProcess(GetCurrentProcess(), er->ExceptionCode);
	return EXCEPTION_EXECUTE_HANDLER;
}

// [stress 用・使い捨て] ヒープ破壊（0xC0000374）は SetUnhandledExceptionFilter を通らずにプロセスが終わるので、
// ベクター例外ハンドラで先に拾って、検出した場所の呼び出し履歴を出す（壊した場所とは限らない）
static LONG WINAPI HeapCorruptionVeh(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode == 0xC0000374) return CrashFilter(ep);
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- フォントの有無

static int CALLBACK EnumFaceProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp)
{
	*(bool*)lp = true;
	return 0;
}

static bool FaceExists(const std::wstring& face)
{
	HDC dc = GetDC(nullptr);
	LOGFONTW lf = {};
	lf.lfCharSet = DEFAULT_CHARSET;
	lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);
	bool found = false;
	EnumFontFamiliesExW(dc, &lf, EnumFaceProc, (LPARAM)&found, 0);
	ReleaseDC(nullptr, dc);
	return found;
}

// そのフォント自身が、文字列の全文字を持っているか（フォントリンクに頼らず描けるか）
static bool FaceCovers(const std::wstring& face, const std::wstring& text, int* missing = nullptr)
{
	HDC dc = CreateCompatibleDC(nullptr);
	LOGFONTW lf = {};
	lf.lfHeight = -16;
	lf.lfCharSet = DEFAULT_CHARSET;
	lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);
	HFONT f = CreateFontIndirectW(&lf);
	HGDIOBJ old = SelectObject(dc, f);
	std::vector<WORD> idx(text.size());
	int miss = -1;
	if (GetGlyphIndicesW(dc, text.c_str(), (int)text.size(), idx.data(), GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
		miss = 0;
		for (WORD w : idx) if (w == 0xFFFF) ++miss;
	}
	SelectObject(dc, old);
	DeleteObject(f);
	DeleteDC(dc);
	if (missing) *missing = miss;
	return miss == 0;
}

// レジストリの SystemLink（フォントリンク先の一覧）。FontLink=2 の MacType はこれを読む
static std::string SystemLinkInfo(const std::wstring& face)
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink",
		0, KEY_READ, &key) != ERROR_SUCCESS)
		return "no SystemLink key";
	DWORD type = 0, size = 0;
	std::string out = "no SystemLink entry";
	if (RegQueryValueExW(key, face.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
		std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
		if (RegQueryValueExW(key, face.c_str(), nullptr, &type, (LPBYTE)buf.data(), &size) == ERROR_SUCCESS) {
			int n = 0;
			std::wstring first;
			for (const wchar_t* p = buf.data(); *p; p += lstrlenW(p) + 1) { if (!n) first = p; ++n; }
			out = "SystemLink " + std::to_string(n) + " entries, first: " + ToUtf8(first);
		}
	}
	RegCloseKey(key);
	return out;
}

// U+4E00 から順に、フォントが持っている漢字を count 個集める
static std::wstring CollectKanji(const std::wstring& face, size_t count)
{
	HDC dc = CreateCompatibleDC(nullptr);
	LOGFONTW lf = {};
	lf.lfHeight = -16;
	lf.lfCharSet = DEFAULT_CHARSET;
	lstrcpynW(lf.lfFaceName, face.c_str(), LF_FACESIZE);
	HFONT f = CreateFontIndirectW(&lf);
	HGDIOBJ old = SelectObject(dc, f);
	std::wstring out;
	wchar_t chunk[256];
	WORD idx[256];
	for (wchar_t base = 0x4E00; base < 0x9F00 && out.size() < count; base += 256) {
		for (int i = 0; i < 256; ++i) chunk[i] = (wchar_t)(base + i);
		if (GetGlyphIndicesW(dc, chunk, 256, idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) break;
		for (int i = 0; i < 256 && out.size() < count; ++i)
			if (idx[i] != 0xFFFF) out.push_back(chunk[i]);
	}
	SelectObject(dc, old);
	DeleteObject(f);
	DeleteDC(dc);
	return out;
}

// ---------------------------------------------------------------- 描画

struct Canvas {
	HDC dc = nullptr;
	HBITMAP bmp = nullptr;
	HGDIOBJ old = nullptr;
	uint32_t* bits = nullptr;
	int w = 0, h = 0;

	bool Create(int width, int height)
	{
		w = width; h = height;
		dc = CreateCompatibleDC(nullptr);
		if (!dc) return false;
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = w;
		bi.bmiHeader.biHeight = -h;   // 上から下
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;   // 16bpp 以下だと MacType は描かない（override.cpp の ETOE_MONO）
		bi.bmiHeader.biCompression = BI_RGB;
		void* p = nullptr;
		bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
		if (!bmp) return false;
		bits = (uint32_t*)p;
		old = SelectObject(dc, bmp);
		return true;
	}
	void Destroy()
	{
		if (dc) { SelectObject(dc, old); DeleteDC(dc); }
		if (bmp) DeleteObject(bmp);
		dc = nullptr; bmp = nullptr; bits = nullptr;
	}
};

static void FillBackground(Canvas& c, DrawMode mode)
{
	GdiFlush();
	if (mode == DM_OPAQUE) {
		for (int i = 0, n = c.w * c.h; i < n; ++i) c.bits[i] = 0x00FFFFFF;
		return;
	}
	// 透明描画の下地: 決まった模様（MacType は描画先から背景を写し取って合成する）
	for (int y = 0; y < c.h; ++y) {
		uint32_t* row = c.bits + (size_t)y * c.w;
		for (int x = 0; x < c.w; ++x) {
			uint32_t r = (uint32_t)(x * 255 / (c.w - 1));
			uint32_t g = (uint32_t)(y * 255 / (c.h - 1));
			uint32_t b = (uint32_t)(((x / 7) ^ (y / 5)) & 1 ? 0xC0 : 0x40);
			row[x] = (r << 16) | (g << 8) | b;
		}
	}
}

static Result HashCanvas(Canvas& c)
{
	GdiFlush();
	// FNV-1a 64bit。BGRA の 4 バイトすべてを入れる（αだけ変わる変更も見逃さない）
	uint64_t h = 0xcbf29ce484222325ULL;
	const uint8_t* p = (const uint8_t*)c.bits;
	const size_t n = (size_t)c.w * c.h * 4;
	for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
	h ^= (uint64_t)c.w << 32 | (uint32_t)c.h; h *= 0x100000001b3ULL;
	Result r;
	r.hash = h;
	for (size_t i = 0, m = (size_t)c.w * c.h; i < m; ++i) {
		uint32_t v = c.bits[i];
		uint32_t b = v & 0xFF, g = (v >> 8) & 0xFF, rr = (v >> 16) & 0xFF;
		if (b != g || g != rr) ++r.colorPx;
	}
	return r;
}

static const wchar_t* CaseText(const Case& k, int* len)
{
	const wchar_t* s = nullptr;
	switch (k.text) {
	case TK_EN:    s = kTextEN; break;
	case TK_SYM:   s = kTextSYM; break;
	case TK_JP:    s = kTextJP; break;
	case TK_SWEEP: *len = (int)g_sweepText.size(); return g_sweepText.c_str();
	}
	*len = lstrlenW(s);
	return s;
}

static Result DrawCase(const Case& k, Canvas& c)
{
	FillBackground(c, k.mode);
	const double t0 = NowMs();

	LOGFONTW lf = {};
	lf.lfHeight = k.height;
	lf.lfWidth = (k.variant == V_WIDTH) ? (-k.height) / 3 : 0;
	lf.lfWeight = k.weight;
	lf.lfItalic = k.italic ? TRUE : FALSE;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = DEFAULT_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	lstrcpynW(lf.lfFaceName, k.face.c_str(), LF_FACESIZE);
	HFONT font = CreateFontIndirectW(&lf);
	HGDIOBJ oldFont = SelectObject(c.dc, font);
	SetTextAlign(c.dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);

	UINT opts = 0;
	RECT rc = { 4, 4, c.w - 4, c.h - 4 };
	if (k.mode == DM_OPAQUE) {
		SetBkMode(c.dc, OPAQUE);
		SetBkColor(c.dc, RGB(255, 255, 255));
		SetTextColor(c.dc, RGB(0, 0, 0));
		opts = ETO_OPAQUE;
	} else {
		SetBkMode(c.dc, TRANSPARENT);
		SetTextColor(c.dc, RGB(20, 40, 160));
	}

	if (k.variant == V_ZOOM) {
		SetGraphicsMode(c.dc, GM_ADVANCED);
		XFORM xf = { 1.25f, 0, 0, 1.25f, 0, 0 };   // 拡大だけ（MacType は拡大縮小だけ自分で描く）
		SetWorldTransform(c.dc, &xf);
	}

	int len = 0;
	const wchar_t* s = CaseText(k, &len);
	const int x = 8, y = 6;
	if (k.text == TK_SWEEP) {
		const int lineH = -k.height + 6;
		for (int i = 0, line = 0; i < len; i += kSweepPerLine, ++line) {
			int n = (len - i < kSweepPerLine) ? (len - i) : kSweepPerLine;
			ExtTextOutW(c.dc, x, y + line * lineH, opts, (opts & ETO_OPAQUE) ? &rc : nullptr, s + i, (UINT)n, nullptr);
			opts &= ~ETO_OPAQUE;   // 塗りつぶしは最初の 1 回だけ
		}
	} else if (k.variant == V_DX) {
		// アプリが文字ごとの送り幅を渡す場合（Word などはこちら）。GDI の幅に 1px ずつ足す
		std::vector<int> dx(len);
		SIZE sz = {};
		if (GetTextExtentExPointW(c.dc, s, len, 0, nullptr, dx.data(), &sz)) {
			for (int i = len - 1; i > 0; --i) dx[i] -= dx[i - 1];
			for (int i = 0; i < len; ++i) dx[i] += 1;
			ExtTextOutW(c.dc, x, y, opts, &rc, s, (UINT)len, dx.data());
		}
	} else {
		ExtTextOutW(c.dc, x, y, opts, (opts & ETO_OPAQUE) ? &rc : nullptr, s, (UINT)len, nullptr);
	}

	if (k.variant == V_ZOOM) {
		ModifyWorldTransform(c.dc, nullptr, MWT_IDENTITY);
		SetGraphicsMode(c.dc, GM_COMPATIBLE);
	}
	SelectObject(c.dc, oldFont);
	DeleteObject(font);
	GdiFlush();
	const double t1 = NowMs();
	Result r = HashCanvas(c);
	r.drawMs = t1 - t0;
	return r;
}

// ケースごとに使う DIB（大きさ別に 1 枚ずつ、スレッドごと）
struct CanvasSet {
	std::vector<Canvas> list;
	Canvas* Get(int w, int h)
	{
		for (auto& c : list) if (c.w == w && c.h == h) return &c;
		list.emplace_back();
		if (!list.back().Create(w, h)) {
			printf("FAIL: CreateDIBSection %dx%d\n", w, h);
			fflush(stdout);
			ExitProcess(1);
		}
		return &list.back();
	}
	~CanvasSet() { for (auto& c : list) c.Destroy(); }
};

// ---------------------------------------------------------------- ケース一覧

static std::wstring MakeId(const std::wstring& face, int height, int weight, bool italic, TextKind t, DrawMode m, Variant v)
{
	wchar_t b[256];
	const wchar_t* style = (weight >= FW_BOLD) ? (italic ? L"BI" : L"B") : (italic ? L"I" : L"R");
	swprintf(b, 256, L"%ls|h%d|%ls|%ls|%ls|%ls", face.c_str(), -height, style, kTextKindName[t], kModeName[m], kVariantName[v]);
	return b;
}

static void AddCase(std::vector<Case>& cases, const std::wstring& face, int height, int weight, bool italic,
	TextKind t, DrawMode m, Variant v)
{
	Case k;
	k.face = face; k.height = height; k.weight = weight; k.italic = italic;
	k.text = t; k.mode = m; k.variant = v;
	if (t == TK_SWEEP) {
		int lines = (int)((g_sweepText.size() + kSweepPerLine - 1) / kSweepPerLine);
		k.w = kSweepPerLine * (-height) + 40;
		k.h = lines * (-height + 6) + 20;
	} else {
		k.w = 1400;
		k.h = 2 * (-height) + 24;
	}
	k.id = MakeId(face, height, weight, italic, t, m, v);
	int len = 0;
	const wchar_t* s = CaseText(k, &len);
	k.linked = !FaceCovers(face, std::wstring(s, len));
	cases.push_back(k);
}

static std::vector<Case> BuildCases(const std::vector<std::wstring>& facesEN, const std::vector<std::wstring>& facesJP,
	const std::wstring& sweepFace)
{
	std::vector<Case> cases;
	struct Style { int weight; bool italic; };
	const Style styles[] = { { FW_NORMAL, false }, { FW_BOLD, false }, { FW_NORMAL, true } };
	std::vector<std::wstring> faces = facesEN;
	faces.insert(faces.end(), facesJP.begin(), facesJP.end());
	// 基本: フォント × 太さ・斜体 × 大きさ × 文字列 × 背景（欧文フォントの日本語はフォントリンク経由）
	for (const auto& f : faces)
		for (const auto& st : styles)
			for (int h : kHeights)
				for (int t = TK_EN; t <= TK_JP; ++t)
					for (int m = DM_OPAQUE; m <= DM_TRANSPARENT; ++m)
						AddCase(cases, f, h, st.weight, st.italic, (TextKind)t, (DrawMode)m, V_PLAIN);
	// 描き方の違い: 送り幅つき、拡大した DC、幅を指定したフォント
	std::vector<std::wstring> vf;
	if (!facesEN.empty()) vf.push_back(facesEN[0]);
	if (!facesJP.empty()) vf.push_back(facesJP[0]);
	for (const auto& f : vf)
		for (int v = V_DX; v <= V_WIDTH; ++v)
			for (int t : { TK_EN, TK_JP })
				AddCase(cases, f, -16, FW_NORMAL, false, (TextKind)t, DM_OPAQUE, (Variant)v);
	// 漢字を多く描いて、グリフキャッシュの間引き（2048 個追加ごと）を起こす
	if (!sweepFace.empty() && !g_sweepText.empty())
		AddCase(cases, sweepFace, -16, FW_NORMAL, false, TK_SWEEP, DM_OPAQUE, V_PLAIN);
	return cases;
}

// ---------------------------------------------------------------- フックの確認（情報として出す）

// インポート用の小さな飛び先（jmp [addr]）なら、その先を返す。Detours も同じように辿って本体をフックする
static const uint8_t* FollowJmpStub(const uint8_t* p)
{
	for (int i = 0; i < 4 && p; ++i) {
#ifdef _WIN64
		if (p[0] == 0x48 && p[1] == 0xFF && p[2] == 0x25) ++p;   // rex.w jmp
		if (p[0] == 0xFF && p[1] == 0x25) {
			int32_t d = *(const int32_t*)(p + 2);
			p = *(const uint8_t* const*)(p + 6 + d);
			continue;
		}
#else
		if (p[0] == 0xFF && p[1] == 0x25) {
			p = **(const uint8_t* const* const*)(p + 2);
			continue;
		}
#endif
		break;
	}
	return p;
}

static std::string Bytes(const uint8_t* p, int n)
{
	std::string s;
	char b[4];
	for (int i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02x", p[i]); s += b; }
	return s;
}

// MacType の IsRunAsUser（hook.cpp）と同じ判定。false だと DirectWrite のフックは張られない
static bool IsRunAsUserLikeMacType()
{
	HANDLE tok = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
	DWORD len = 0;
	GetTokenInformation(tok, TokenGroups, nullptr, 0, &len);
	std::vector<uint8_t> buf(len ? len : 1);
	bool ret = false;
	if (GetTokenInformation(tok, TokenGroups, buf.data(), len, &len)) {
		SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
		PSID inter = nullptr, svc = nullptr;
		AllocateAndInitializeSid(&nt, 1, SECURITY_INTERACTIVE_RID, 0, 0, 0, 0, 0, 0, 0, &inter);
		AllocateAndInitializeSid(&nt, 1, SECURITY_SERVICE_RID, 0, 0, 0, 0, 0, 0, 0, &svc);
		const TOKEN_GROUPS* g = (const TOKEN_GROUPS*)buf.data();
		for (DWORD i = 0; i < g->GroupCount; ++i) {
			if (inter && EqualSid(g->Groups[i].Sid, inter)) { ret = true; break; }
			if (svc && EqualSid(g->Groups[i].Sid, svc)) { ret = false; break; }
		}
		if (inter) FreeSid(inter);
		if (svc) FreeSid(svc);
	}
	CloseHandle(tok);
	return ret;
}

// ---------------------------------------------------------------- 複数スレッド用の待ち合わせ

class Barrier {
public:
	explicit Barrier(int n) : n_(n), left_(n) {}
	void Wait()
	{
		std::unique_lock<std::mutex> lk(m_);
		const unsigned gen = gen_;
		if (--left_ == 0) { left_ = n_; ++gen_; cv_.notify_all(); return; }
		cv_.wait(lk, [&] { return gen != gen_; });
	}
private:
	std::mutex m_;
	std::condition_variable cv_;
	const int n_;
	int left_;
	unsigned gen_ = 0;
};

// ---------------------------------------------------------------- main

static void Usage()
{
	printf(
		"usage: t01.exe --dll <MacType(64).Core.dll> [options]\n"
		"       t01.exe --list-fonts [--add-font <file>]... [--jp-face <name>]...\n"
		"  --out <file>        結果を書き出す（UTF-8、タブ区切り）\n"
		"  --repeat <N>        単スレッドで全ケースを N 回描く（既定 1）。2 回目以降も同じ画素か確かめ、時間を出す\n"
		"  --threads <T>       T 本のスレッドで、同じケースを同時に描く（S-01〜S-03 の確認）\n"
		"  --rounds <R>        --threads のとき、全ケースを何周するか（既定 3）\n"
		"  --add-font <file>   AddFontResourceEx(FR_PRIVATE) でフォントを読み込む（日本語フォントがないとき用）\n"
		"  --jp-face <name>    日本語フォントの候補に足す（--add-font で読んだフォントの名前など）\n"
		"  --no-gdi-check      素の GDI と画素が違うかの確認を省く（速度だけ見たいとき）\n");
}

int wmain(int argc, wchar_t** argv)
{
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	SetUnhandledExceptionFilter(CrashFilter);
	AddVectoredExceptionHandler(1, HeapCorruptionVeh);

	std::wstring dllPath, outPath;
	std::vector<std::wstring> addFonts, extraJP;
	int repeat = 1, threads = 0, rounds = 3;
	bool listFonts = false, gdiCheck = true, serialFirst = false;
	for (int i = 1; i < argc; ++i) {
		std::wstring a = argv[i];
		auto next = [&]() -> std::wstring { if (i + 1 >= argc) { Usage(); exit(4); } return argv[++i]; };
		if (a == L"--dll") dllPath = next();
		else if (a == L"--out") outPath = next();
		else if (a == L"--repeat") repeat = _wtoi(next().c_str());
		else if (a == L"--threads") threads = _wtoi(next().c_str());
		else if (a == L"--rounds") rounds = _wtoi(next().c_str());
		else if (a == L"--add-font") addFonts.push_back(next());
		else if (a == L"--jp-face") extraJP.push_back(next());
		else if (a == L"--list-fonts") listFonts = true;
		else if (a == L"--no-gdi-check") gdiCheck = false;
		else if (a == L"--serial-first-draw") serialFirst = true;
		else { Usage(); return 4; }
	}
	if ((!listFonts && dllPath.empty()) || repeat < 1 || threads < 0 || rounds < 1) { Usage(); return 4; }

	if (!outPath.empty()) {
		g_out = _wfopen(outPath.c_str(), L"wb");
		if (!g_out) { printf("FAIL: cannot open %s\n", ToUtf8(outPath).c_str()); return 4; }
	}

	Info("T-01 render check (%s)", sizeof(void*) == 8 ? "x64" : "Win32");
	{
		typedef LONG(WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOW*);
		OSVERSIONINFOW vi = { sizeof(vi) };
		auto p = (PFN_RtlGetVersion)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
		if (p) p(&vi);
		Info("os %lu.%lu.%lu, ACP %u, logical CPUs %u", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber,
			GetACP(), std::thread::hardware_concurrency());
	}

	// ---- フォントの準備（DLL を読む前に）
	for (const auto& f : addFonts) {
		int n = AddFontResourceExW(f.c_str(), FR_PRIVATE, nullptr);
		Info("AddFontResourceEx %s -> %d", ToUtf8(f).c_str(), n);
		if (!n) { Fail("could not add font %s", ToUtf8(f).c_str()); return 1; }
	}
	std::vector<std::wstring> facesEN, facesJP;
	for (auto f : kFacesEN) {
		bool ok = FaceExists(f);
		Info("font %-16s %s; %s", ToUtf8(f).c_str(), ok ? "found" : "missing", SystemLinkInfo(f).c_str());
		if (ok) facesEN.push_back(f);
	}
	std::vector<std::wstring> jpCandidates(std::begin(kFacesJP), std::end(kFacesJP));
	jpCandidates.insert(jpCandidates.end(), extraJP.begin(), extraJP.end());
	for (const auto& f : jpCandidates) {
		bool ok = FaceExists(f);
		int miss = -1;
		if (ok) ok = FaceCovers(f, kTextJP, &miss);
		Info("font %-16s %s", ToUtf8(f).c_str(), ok ? "found, covers JP text" :
			(miss > 0 ? "found, but lacks some JP glyphs (not used)" : "missing"));
		if (ok) facesJP.push_back(f);
	}
	if (listFonts) return facesJP.empty() ? 2 : 0;
	if (facesJP.empty()) {
		Fail("no Japanese font. Use --add-font <file> --jp-face <name>");
		return 2;
	}
	if (facesEN.empty()) { Fail("no Latin font found"); return 1; }

	// 間引きを起こすための漢字（2048 を超える数）。一番多く持っている日本語フォントで描く
	std::wstring sweepFace;
	for (const auto& f : facesJP) {
		std::wstring s = CollectKanji(f, 2400);
		if (s.size() > g_sweepText.size()) { g_sweepText = s; sweepFace = f; }
	}
	Info("sweep: %u kanji with %s%s", (unsigned)g_sweepText.size(), ToUtf8(sweepFace).c_str(),
		g_sweepText.size() > 2048 ? "" : " (fewer than 2048: cache compaction may not happen)");

	const std::vector<Case> cases = BuildCases(facesEN, facesJP, sweepFace);
	Info("cases: %u", (unsigned)cases.size());

	// ---- フックの確認用に、読み込む前の状態を取っておく
	const uint8_t* etoExport = (const uint8_t*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "ExtTextOutW");
	const uint8_t* etoBody = FollowJmpStub(etoExport);
	uint8_t etoBefore[16];
	memcpy(etoBefore, etoBody, sizeof(etoBefore));
	const bool dwBefore = GetModuleHandleW(L"dwrite.dll") != nullptr;
	const bool d2dBefore = GetModuleHandleW(L"d2d1.dll") != nullptr;

	// ---- 素の GDI で描く（基準）
	std::vector<Result> gdi(cases.size());
	if (gdiCheck) {
		CanvasSet cs;
		const double t0 = NowMs();
		for (size_t i = 0; i < cases.size(); ++i)
			gdi[i] = DrawCase(cases[i], *cs.Get(cases[i].w, cases[i].h));
		Info("gdi pass: %.1f ms", NowMs() - t0);
	}

	// ---- MacType を読み込む
	{
		WIN32_FILE_ATTRIBUTE_DATA fa = {};
		GetFileAttributesExW(dllPath.c_str(), GetFileExInfoStandard, &fa);
		Info("dll %s (%lu bytes)", ToUtf8(dllPath).c_str(), fa.nFileSizeLow);
	}
	const double tl0 = NowMs();
	HMODULE hCore = LoadLibraryW(dllPath.c_str());
	const double tl1 = NowMs();
	if (!hCore) {
		Fail("LoadLibrary failed, error %lu", GetLastError());
		if (g_out) fclose(g_out);
		return 3;
	}
	Info("LoadLibrary: %.2f ms", tl1 - tl0);
	{
		// [stress 用・使い捨て] ページヒープが効いているか（NtGlobalFlag の 0x02000000）
		typedef ULONG(NTAPI* PFN_RtlGetNtGlobalFlags)(void);
		auto p = (PFN_RtlGetNtGlobalFlags)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtGlobalFlags");
		Info("NtGlobalFlag 0x%08lx, verifier.dll %s", p ? p() : 0UL, GetModuleHandleW(L"verifier.dll") ? "loaded" : "not loaded");
	}

	const bool etoPatched = memcmp(etoBefore, etoBody, sizeof(etoBefore)) != 0;
	Info("ExtTextOutW %s: before %s after %s", etoPatched ? "patched" : "NOT patched",
		Bytes(etoBefore, 8).c_str(), Bytes(etoBody, 8).c_str());
	{
		const bool dwAfter = GetModuleHandleW(L"dwrite.dll") != nullptr;
		const bool d2dAfter = GetModuleHandleW(L"d2d1.dll") != nullptr;
		std::string dwHead = "-";
		if (HMODULE dw = GetModuleHandleW(L"dwrite.dll"))
			if (auto p = (const uint8_t*)GetProcAddress(dw, "DWriteCreateFactory")) dwHead = Bytes(p, 5);
		Info("IsRunAsUser (same check as MacType): %s", IsRunAsUserLikeMacType() ? "yes" : "no");
		Info("dwrite.dll loaded: before %s, after %s; d2d1.dll: before %s, after %s; DWriteCreateFactory head %s%s",
			dwBefore ? "yes" : "no", dwAfter ? "yes" : "no", d2dBefore ? "yes" : "no", d2dAfter ? "yes" : "no",
			dwHead.c_str(), (dwHead.size() >= 2 && dwHead.compare(0, 2, "e9") == 0) ? " (jmp: likely hooked)" : "");
	}

	int failures = 0;
	std::vector<Result> mt(cases.size());

	if (threads == 0) {
		// ---- 単スレッド
		// 時間は「文字を描く部分」だけを足し合わせる（下地の塗りとハッシュ計算は除く）
		CanvasSet cs;
		double cold = 0;
		for (size_t i = 0; i < cases.size(); ++i) {
			mt[i] = DrawCase(cases[i], *cs.Get(cases[i].w, cases[i].h));
			cold += mt[i].drawMs;
		}
		Info("mactype pass 1 (cold cache): %.1f ms drawing", cold);
		if (repeat > 1) {
			int unstable = 0;
			double warm = 0;
			for (int r = 1; r < repeat; ++r)
				for (size_t i = 0; i < cases.size(); ++i) {
					Result x = DrawCase(cases[i], *cs.Get(cases[i].w, cases[i].h));
					warm += x.drawMs;
					if (x.hash != mt[i].hash) {
						if (unstable++ < 10)
							Fail("pass %d differs from pass 1: %s", r + 1, ToUtf8(cases[i].id).c_str());
					}
				}
			Info("mactype pass 2..%d (warm): %.1f ms drawing, %.2f ms/pass", repeat, warm, warm / (repeat - 1));
			Info("TIME cold_ms=%.1f warm_ms_per_pass=%.2f passes=%d cases=%u", cold, warm / (repeat - 1), repeat - 1,
				(unsigned)cases.size());
			if (unstable) { Fail("%d renders differ from pass 1", unstable); ++failures; }
		}
	} else {
		// ---- 複数スレッド: 全スレッドが同じケースを同時に描く。1 周目はキャッシュにない文字を同時に描くことになる
		std::vector<std::vector<uint64_t>> hashes(cases.size(), std::vector<uint64_t>((size_t)threads * rounds));
		std::vector<uint32_t> color0(cases.size());
		Barrier bar(threads);
		std::mutex firstDraw;
		auto worker = [&](int t) {
			CanvasSet cs;
			if (serialFirst) {
				// [stress 用・使い捨て] 各スレッドの最初の描画だけ 1 本ずつにする。MacType はスレッドの最初の描画で
				// TLS データを作って共有の一覧に足す（S-03 の場所）。そこが重ならないようにして、S-01 だけを見る
				std::lock_guard<std::mutex> g(firstDraw);
				DrawCase(cases[0], *cs.Get(cases[0].w, cases[0].h));
			}
			for (int r = 0; r < rounds; ++r)
				for (size_t i = 0; i < cases.size(); ++i) {
					bar.Wait();
					Result x = DrawCase(cases[i], *cs.Get(cases[i].w, cases[i].h));
					hashes[i][(size_t)r * threads + t] = x.hash;
					if (r == 0 && t == 0) color0[i] = x.colorPx;
				}
		};
		const double t0 = NowMs();
		std::vector<std::thread> ths;
		for (int t = 0; t < threads; ++t) ths.emplace_back(worker, t);
		for (auto& th : ths) th.join();
		const double el = NowMs() - t0;
		Info("multithread: %d threads x %d rounds x %u cases: %.1f ms%s", threads, rounds, (unsigned)cases.size(), el,
			serialFirst ? " (first draw of each thread serialized)" : "");
		Info("TIME mt_ms=%.1f threads=%d rounds=%d cases=%u", el, threads, rounds, (unsigned)cases.size());
		int bad = 0;
		for (size_t i = 0; i < cases.size(); ++i) {
			mt[i].hash = hashes[i][0];
			mt[i].colorPx = color0[i];
			int diff = 0;
			for (uint64_t h : hashes[i]) if (h != hashes[i][0]) ++diff;
			if (diff) {
				++bad;
				if (bad <= 10) Fail("threads disagree on %s (%d of %d renders differ from thread 0 round 1)",
					ToUtf8(cases[i].id).c_str(), diff, threads * rounds);
			}
		}
		if (bad) { Fail("%d cases differ between threads/rounds", bad); ++failures; }
	}

	// ---- MacType が効いているか（素の GDI と違う画素になっているか）
	// フォント自身が全文字を持つケースだけを必須にする。フォントリンクに頼るケース（欧文フォントで日本語など）は、
	// リンク先の設定（レジストリの SystemLink）次第で MacType が素の GDI に任せることがあるので、数を出すだけ
	if (gdiCheck) {
		int same = 0, noColor = 0, opaqueCases = 0, checked = 0, linked = 0, linkedSame = 0;
		for (size_t i = 0; i < cases.size(); ++i) {
			if (cases[i].linked) {
				++linked;
				if (mt[i].hash == gdi[i].hash) {
					++linkedSame;
					Info("linked case drawn by plain GDI: %s", ToUtf8(cases[i].id).c_str());
				}
				continue;
			}
			++checked;
			if (mt[i].hash == gdi[i].hash) {
				if (same++ < 10) Fail("same pixels as plain GDI (MacType not effective?): %s", ToUtf8(cases[i].id).c_str());
			}
			if (cases[i].mode == DM_OPAQUE) {
				++opaqueCases;
				// AntiAliasMode=4 は LCD で描くので、白地に黒でも色つきの画素が出る。
				// INI が読めずに既定値（AntiAliasMode=0、グレー）になっていれば 0 になる
				if (mt[i].colorPx == 0) {
					if (noColor++ < 10) Fail("no colored pixels on black/white (LCD path not used?): %s", ToUtf8(cases[i].id).c_str());
				}
			}
		}
		Info("effect: %d of %d cases differ from plain GDI; %d of %d black/white cases have LCD color fringes",
			checked - same, checked, opaqueCases - noColor, opaqueCases);
		Info("linked cases (font lacks some glyphs, not required): %d, of which %d drawn by plain GDI", linked, linkedSame);
		if (same) { Fail("%d cases look like plain GDI", same); ++failures; }
		if (noColor) { Fail("%d black/white cases without color fringes", noColor); ++failures; }
	}

	// ---- 結果
	uint64_t all = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < cases.size(); ++i) {
		for (int b = 0; b < 8; ++b) { all ^= (mt[i].hash >> (b * 8)) & 0xFF; all *= 0x100000001b3ULL; }
		if (g_out)
			fprintf(g_out, "%s\t%s\t%s\t%u\t%u\n", ToUtf8(cases[i].id).c_str(), Hex(mt[i].hash).c_str(),
				gdiCheck ? Hex(gdi[i].hash).c_str() : "-", mt[i].colorPx, gdi[i].colorPx);
	}
	Info("overall hash %s (%u cases)", Hex(all).c_str(), (unsigned)cases.size());
	Info("result: %s", failures ? "FAIL" : "OK");
	if (g_out) fclose(g_out);
	fflush(stdout);
	// MacType は DllMain の DLL_PROCESS_DETACH で、プロセス終了時は何もしない。そのまま終わる
	ExitProcess(failures ? 1 : 0);
}
