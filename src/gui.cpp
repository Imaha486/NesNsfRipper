// gui.cpp - Win32 GUI front end for NSF ripping (candidate list, audition, export)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <memory>
#include "ripper.h"
#include "synth.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

enum { ID_EDIT_PATH = 100, ID_BTN_OPEN, ID_BTN_ANALYZE, ID_BTN_CANCEL, ID_LIST, ID_BTN_PLAY, ID_BTN_STOP, ID_BTN_SAVE, ID_BTN_CHECKALL, ID_BTN_CHECKNONE, ID_BTN_CHECKGOOD,
       ID_EDIT_TITLE, ID_EDIT_ARTIST, ID_EDIT_COPY, ID_LOG, ID_STATUS, ID_CHK_SFX, ID_CHK_M3U, ID_EDIT_FRAMES, ID_BTN_VERIFY, ID_CHK_UNSAFE };
enum { WM_APP_PROGRESS = WM_APP + 1, WM_APP_DONE, WM_APP_PLAYPOS };

static HWND g_hwnd, g_list, g_log, g_status, g_edPath, g_edTitle, g_edArtist, g_edCopy, g_btnAnalyze, g_btnCancel, g_btnPlay, g_btnStop, g_btnSave, g_chkSfx, g_chkM3u, g_edFrames, g_btnVerify, g_chkUnsafe;
static std::unique_ptr<Ripper> g_rip;
static std::atomic<bool> g_cancel{ false };
static std::thread g_worker;

// crash diagnostics: write crash.log next to the EXE so the cause can be reported
static std::wstring g_crashLogPath;
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    static bool once = false; if (once) return EXCEPTION_EXECUTE_HANDLER; once = true;
    wchar_t mod[MAX_PATH] = L""; HMODULE hm = nullptr; uintptr_t addr = ep && ep->ExceptionRecord ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &hm);
    if (hm) GetModuleFileNameW(hm, mod, MAX_PATH);
    wchar_t msg[1024];
    swprintf(msg, 1024, L"code=0x%08X address=0x%p module=%s base=0x%p offset=0x%llX thread=%u",
        ep && ep->ExceptionRecord ? (unsigned)ep->ExceptionRecord->ExceptionCode : 0u, (void*)addr, mod[0] ? mod : L"?", (void*)hm, (unsigned long long)(hm ? addr - (uintptr_t)hm : 0), (unsigned)GetCurrentThreadId());
    HANDLE h = CreateFileW(g_crashLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { char u8[2048]; int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, u8, 2048, nullptr, nullptr); if (n > 1) { DWORD w; WriteFile(h, u8, n - 1, &w, nullptr); WriteFile(h, "\r\n", 2, &w, nullptr); } CloseHandle(h); }
    std::wstring t = L"NesNsfRipper がクラッシュしました。\n"; t += msg; t += L"\n\n詳細を crash.log に記録しました（EXEと同じフォルダ）。"; MessageBoxW(nullptr, t.c_str(), L"クラッシュ", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void installCrashHandler() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); std::wstring e = exe; size_t sl = e.find_last_of(L"\\/"); g_crashLogPath = (sl == std::wstring::npos ? L"" : e.substr(0, sl + 1)) + L"crash.log";
    SetUnhandledExceptionFilter(crashFilter);
}
static std::wstring g_romPath;
static std::vector<std::string> g_verifyMsg;

// ---------------------------------------------------------------- helpers
static std::wstring utf8ToW(const std::string& s) { if (s.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w; }
static std::string wToUtf8(const std::wstring& w) { if (w.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s; }
static std::string wToAscii(const std::wstring& w) { std::string s; for (wchar_t c : w) if (c >= 32 && c < 127) s += (char)c; while (!s.empty() && s.back() == ' ') s.pop_back(); return s; }
static std::vector<uint8_t> readFileW(const std::wstring& p) { std::vector<uint8_t> d; FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return d; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); d.resize(n); if (n) fread(d.data(), 1, n, f); fclose(f); return d; }
static bool writeFileW(const std::wstring& p, const std::vector<uint8_t>& d) { FILE* f = _wfopen(p.c_str(), L"wb"); if (!f) return false; fwrite(d.data(), 1, d.size(), f); fclose(f); return true; }
static void setStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }
static void appendLog(const std::wstring& s) {
    int len = GetWindowTextLengthW(g_log); SendMessageW(g_log, EM_SETSEL, len, len);
    std::wstring t = s; for (size_t i = 0; i < t.size(); i++) if (t[i] == L'\n' && (i == 0 || t[i - 1] != L'\r')) { t.insert(i, L"\r"); i++; }
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
}
static std::wstring fmtTime(int frames, double rate) { if (frames <= 0) return L"-"; double sec = frames / (60.0 * rate); wchar_t b[32]; swprintf(b, 32, L"%d:%02d", (int)sec / 60, (int)sec % 60); return b; }

// ---------------------------------------------------------------- audio player (waveOut streaming)
struct Player {
    HWAVEOUT hwo = nullptr; std::thread th; std::atomic<bool> stop{ false }; std::atomic<bool> running{ false };
    std::vector<uint8_t> nsf; int track = 0;
    static const int NBUF = 4, BUFSAMP = 4096;
    WAVEHDR hdr[NBUF]; std::vector<int16_t> buf[NBUF];
    void start(const std::vector<uint8_t>& n, int t) {
        shutdown(); nsf = n; track = t; stop = false;
        WAVEFORMATEX wf{}; wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 44100; wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 88200;
        if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { hwo = nullptr; return; }
        running = true;
        th = std::thread([this]() { run(); });
    }
    void run() {
        NsfPlayer pl; if (!pl.load(nsf) || !pl.selectTrack(track)) { running = false; return; }
        Synth syn; syn.reset(pl.expansion); NES* np = &pl.nes; syn.setMemReader([np](uint16_t a) { return np->peek(a); });
        syn.write(0x4015, 0x0F); syn.write(0x4017, 0x40);
        int cpf = pl.cyclesPerFrame(); std::vector<SoundWrite> ws; std::vector<int16_t> pending;
        for (int i = 0; i < NBUF; i++) { buf[i].assign(BUFSAMP, 0); memset(&hdr[i], 0, sizeof(WAVEHDR)); hdr[i].lpData = (LPSTR)buf[i].data(); hdr[i].dwBufferLength = BUFSAMP * 2; waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR)); hdr[i].dwFlags |= WHDR_DONE; }
        int cur = 0; int frames = 0; bool err = false;
        while (!stop) {
            while (pending.size() < (size_t)BUFSAMP && !err) {
                if (!pl.frame(ws)) { err = true; break; }
                uint32_t t = 0; for (auto& w : ws) { uint32_t at = std::min<uint32_t>(w.cycle, cpf - 1); if (at > t) { syn.run(at - t, pending); t = at; } syn.write(w.addr, w.val); }
                if ((int)t < cpf) syn.run(cpf - t, pending);
                frames++;
            }
            if (err && pending.empty()) break;
            WAVEHDR& h = hdr[cur];
            while (!(h.dwFlags & WHDR_DONE) && !stop) Sleep(5);
            if (stop) break;
            size_t n = std::min<size_t>(BUFSAMP, pending.size());
            memcpy(buf[cur].data(), pending.data(), n * 2); if (n < (size_t)BUFSAMP) memset(buf[cur].data() + n, 0, (BUFSAMP - n) * 2);
            pending.erase(pending.begin(), pending.begin() + n);
            h.dwFlags &= ~WHDR_DONE; waveOutWrite(hwo, &h, sizeof(WAVEHDR));
            cur = (cur + 1) % NBUF;
            PostMessageW(g_hwnd, WM_APP_PLAYPOS, frames, 0);
        }
        waveOutReset(hwo);
        for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
        running = false;
    }
    void shutdown() { stop = true; if (th.joinable()) th.join(); if (hwo) { waveOutClose(hwo); hwo = nullptr; } }
};
static Player g_player;

// ---------------------------------------------------------------- list
static void addColumn(int idx, const wchar_t* name, int width) { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)name; c.cx = width; ListView_InsertColumn(g_list, idx, &c); }
static void fillList() {
    ListView_DeleteAllItems(g_list);
    if (!g_rip) return;
    for (size_t i = 0; i < g_rip->cands.size(); i++) {
        Candidate& c = g_rip->cands[i];
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = (int)i; it.lParam = (LPARAM)i;
        wchar_t b[64]; swprintf(b, 64, L"%u", (unsigned)(i + 1)); it.pszText = b; ListView_InsertItem(g_list, &it);
        auto set = [&](int col, const std::wstring& s) { ListView_SetItemText(g_list, (int)i, col, (LPWSTR)s.c_str()); };
        set(1, utf8ToW(c.name));
        std::wstring conf = c.confidence == 2 ? L"◎ 使用確認" : c.confidence == 1 ? L"○ 妥当" : L"△ 範囲外?";
        if (c.suspicious) conf += L" !";
        set(2, conf);
        set(3, c.durationFrames ? fmtTime(c.durationFrames, 1.0) : (c.loopLen ? L"∞ (loop " + fmtTime(c.loopLen, 1.0) + L")" : L"∞"));
        std::wstring ch; const wchar_t* names[] = { L"P1", L"P2", L"Tr", L"No", L"DM", L"Ex" }; for (int k = 0; k < 6; k++) if (c.channels & (1 << k)) { if (!ch.empty()) ch += L" "; ch += names[k]; }
        set(4, ch);
        set(5, utf8ToW(c.recipe.describe()));
        set(6, c.verified ? L"OK" : (i < g_verifyMsg.size() && !g_verifyMsg[i].empty() ? utf8ToW(g_verifyMsg[i]) : L""));
        set(7, utf8ToW(c.note));
        ListView_SetCheckState(g_list, (int)i, c.confidence > 0 && !c.suspicious && (c.isMusic || true));
    }
}
static std::vector<int> checkedItems() { std::vector<int> v; if (!g_rip) return v; for (size_t i = 0; i < g_rip->cands.size(); i++) if (ListView_GetCheckState(g_list, (int)i)) v.push_back((int)i); return v; }
static int selectedItem() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }

// ---------------------------------------------------------------- analysis thread
static void startAnalysis() {
    std::wstring path; wchar_t b[1024]; GetWindowTextW(g_edPath, b, 1024); path = b;
    std::vector<uint8_t> data = readFileW(path);
    if (data.empty()) { MessageBoxW(g_hwnd, L"ROMファイルを読み込めません", L"エラー", MB_ICONERROR); return; }
    g_player.shutdown();
    g_rip.reset(new Ripper(data));
    if (!g_rip->ok()) { MessageBoxW(g_hwnd, utf8ToW("ROM読み込み失敗: " + g_rip->error).c_str(), L"エラー", MB_ICONERROR); g_rip.reset(); return; }
    g_romPath = path;
    size_t sl = path.find_last_of(L"\\/"); std::wstring fn = sl == std::wstring::npos ? path : path.substr(sl + 1); size_t dot = fn.find_last_of(L'.'); if (dot != std::wstring::npos) fn = fn.substr(0, dot);
    SetWindowTextW(g_edTitle, fn.c_str());
    wchar_t fb[32]; GetWindowTextW(g_edFrames, fb, 32); int frames = _wtoi(fb); if (frames < 300) frames = 300; if (frames > 20000) frames = 20000;
    g_rip->opt.traceFrames = frames; g_rip->opt.threads = std::max(1u, std::thread::hardware_concurrency()); g_rip->opt.cancel = &g_cancel; g_cancel = false;
    g_rip->opt.progress = [](const std::string& s) { PostMessageW(g_hwnd, WM_APP_PROGRESS, 0, (LPARAM)new std::string(s)); };
    g_verifyMsg.clear();
    ListView_DeleteAllItems(g_list); SetWindowTextW(g_log, L"");
    appendLog(L"ROM: " + fn + utf8ToW(" / mapper " + std::to_string(g_rip->base.mapper) + " (" + g_rip->base.mapperName() + ") PRG " + std::to_string(g_rip->base.prg.size() / 1024) + "KB\n"));
    EnableWindow(g_btnAnalyze, FALSE); EnableWindow(g_btnCancel, TRUE); EnableWindow(g_btnSave, FALSE); EnableWindow(g_btnVerify, FALSE);
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread([]() {
        bool ok2 = false;
        try { ok2 = g_rip->run(); }
        catch (const std::exception& ex) { g_rip->error = std::string("内部エラー: ") + ex.what(); }
        catch (...) { g_rip->error = "内部エラー (unknown exception)"; }
        PostMessageW(g_hwnd, WM_APP_DONE, ok2 ? 1 : 0, 0);
    });
}

static NsfBuild buildFor(const std::vector<int>& sel) {
    wchar_t t[64], a[64], c[64]; GetWindowTextW(g_edTitle, t, 64); GetWindowTextW(g_edArtist, a, 64); GetWindowTextW(g_edCopy, c, 64);
    std::string title = wToAscii(t); if (title.empty()) title = "NSF rip";
    return g_rip->buildNsf(sel, title, wToAscii(a), wToAscii(c));
}

static void playSelected() {
    if (!g_rip) return; int i = selectedItem(); if (i < 0) { setStatus(L"再生する候補をリストで選択してください"); return; }
    NsfBuild nb = buildFor({ i });
    if (!nb.ok) { MessageBoxW(g_hwnd, utf8ToW(nb.report).c_str(), L"NSF生成エラー", MB_ICONERROR); return; }
    g_player.start(nb.file, 0);
    setStatus(L"再生中: " + utf8ToW(g_rip->cands[i].name + "  " + g_rip->cands[i].recipe.describe()));
}

static void saveNsf() {
    if (!g_rip) return; std::vector<int> sel = checkedItems();
    if (sel.empty()) { MessageBoxW(g_hwnd, L"出力するトラックにチェックを入れてください", L"NSF保存", MB_ICONINFORMATION); return; }
    NsfBuild nb = buildFor(sel);
    if (!nb.ok) { MessageBoxW(g_hwnd, utf8ToW(nb.report).c_str(), L"NSF生成エラー", MB_ICONERROR); return; }
    wchar_t fn[1024] = L""; { std::wstring base = g_romPath; size_t dot = base.find_last_of(L'.'); if (dot != std::wstring::npos) base = base.substr(0, dot); wcsncpy(fn, (base + L".nsf").c_str(), 1023); }
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_hwnd; ofn.lpstrFilter = L"NSF files (*.nsf)\0*.nsf\0All files\0*.*\0"; ofn.lpstrFile = fn; ofn.nMaxFile = 1024; ofn.lpstrDefExt = L"nsf"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!writeFileW(fn, nb.file)) { MessageBoxW(g_hwnd, L"書き込みに失敗しました", L"エラー", MB_ICONERROR); return; }
    appendLog(L"\n=== NSF保存: " + std::wstring(fn) + L" ===\n" + utf8ToW(nb.report));
    // verification
    int okc = 0;
    for (size_t t = 0; t < sel.size(); t++) { std::string msg; bool ok = g_rip->verifyNsf(nb, (int)t, sel[t], 300, &msg); g_rip->cands[sel[t]].verified = ok; if (ok) okc++; else appendLog(utf8ToW("  track " + std::to_string(t + 1) + " (" + g_rip->cands[sel[t]].name + "): " + msg + "\n")); }
    appendLog(utf8ToW("検証: " + std::to_string(okc) + "/" + std::to_string(sel.size()) + " トラックがゲーム実行時と一致\n"));
    if (SendMessageW(g_chkM3u, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        std::wstring m3u = fn; size_t dot = m3u.find_last_of(L'.'); if (dot != std::wstring::npos) m3u = m3u.substr(0, dot); m3u += L".m3u";
        std::wstring nsfName = fn; size_t sl = nsfName.find_last_of(L"\\/"); if (sl != std::wstring::npos) nsfName = nsfName.substr(sl + 1);
        std::string body = "# NSF playlist generated by NesNsfRipper\r\n";
        for (size_t t = 0; t < sel.size(); t++) { Candidate& c = g_rip->cands[sel[t]]; std::string len = c.durationFrames ? std::to_string(c.durationFrames / 3600) + ":" + (c.durationFrames / 60 % 60 < 10 ? "0" : "") + std::to_string(c.durationFrames / 60 % 60) : "-"; body += wToUtf8(nsfName) + "::NSF," + std::to_string(t + 1) + "," + c.name + " (" + c.recipe.describe() + ")," + len + "\r\n"; }
        std::vector<uint8_t> bd(body.begin(), body.end()); writeFileW(m3u, bd);
    }
    { wchar_t b[256]; swprintf(b, 256, L"保存しました: %d トラック (%d/%d 検証OK)", (int)sel.size(), okc, (int)sel.size()); setStatus(b); }
    fillList();
}

static void verifyAll() {
    if (!g_rip) return; std::vector<int> sel = checkedItems(); if (sel.empty()) return;
    NsfBuild nb = buildFor(sel);
    if (!nb.ok) { MessageBoxW(g_hwnd, utf8ToW(nb.report).c_str(), L"NSF生成エラー", MB_ICONERROR); return; }
    g_verifyMsg.assign(g_rip->cands.size(), "");
    int okc = 0;
    for (size_t t = 0; t < sel.size(); t++) { std::string msg; bool ok = g_rip->verifyNsf(nb, (int)t, sel[t], 300, &msg); g_rip->cands[sel[t]].verified = ok; g_verifyMsg[sel[t]] = ok ? "OK" : msg; if (ok) okc++; }
    appendLog(L"\n=== NSF検証 ===\n" + utf8ToW(nb.report) + utf8ToW("検証: " + std::to_string(okc) + "/" + std::to_string(sel.size()) + " OK\n"));
    std::vector<bool> chk(g_rip->cands.size()); for (size_t i = 0; i < chk.size(); i++) chk[i] = ListView_GetCheckState(g_list, (int)i);
    fillList(); for (size_t i = 0; i < chk.size(); i++) ListView_SetCheckState(g_list, (int)i, chk[i]);
}

// ---------------------------------------------------------------- window
static void layout(HWND hwnd) {
    RECT r; GetClientRect(hwnd, &r); int W = r.right, H = r.bottom;
    int y = 8;
    MoveWindow(g_edPath, 8, y, W - 8 - 90 - 8 - 90 - 8 - 70 - 8, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_BTN_OPEN), W - 90 - 8 - 90 - 8 - 70 - 8, y, 70, 24, TRUE);
    MoveWindow(g_btnAnalyze, W - 90 - 8 - 90 - 8, y, 90, 24, TRUE);
    MoveWindow(g_btnCancel, W - 90 - 8, y, 90, 24, TRUE);
    y += 32;
    MoveWindow(GetDlgItem(hwnd, ID_EDIT_FRAMES - 1000), 8, y + 4, 140, 20, TRUE); MoveWindow(g_edFrames, 150, y, 60, 24, TRUE);
    MoveWindow(g_status, 220, y + 4, W - 228, 20, TRUE);
    y += 32;
    int listH = (H - y - 8) * 55 / 100; if (listH < 120) listH = 120;
    MoveWindow(g_list, 8, y, W - 16, listH, TRUE);
    y += listH + 8;
    int bx = 8;
    MoveWindow(g_btnPlay, bx, y, 80, 26, TRUE); bx += 88; MoveWindow(g_btnStop, bx, y, 80, 26, TRUE); bx += 96;
    MoveWindow(GetDlgItem(hwnd, ID_BTN_CHECKGOOD), bx, y, 110, 26, TRUE); bx += 118; MoveWindow(GetDlgItem(hwnd, ID_BTN_CHECKALL), bx, y, 90, 26, TRUE); bx += 98; MoveWindow(GetDlgItem(hwnd, ID_BTN_CHECKNONE), bx, y, 90, 26, TRUE); bx += 98;
    MoveWindow(g_btnVerify, bx, y, 90, 26, TRUE); bx += 98;
    MoveWindow(g_btnSave, W - 8 - 150, y, 150, 26, TRUE);
    y += 34;
    MoveWindow(GetDlgItem(hwnd, ID_EDIT_TITLE - 1000), 8, y + 4, 50, 20, TRUE); MoveWindow(g_edTitle, 60, y, 180, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_EDIT_ARTIST - 1000), 250, y + 4, 50, 20, TRUE); MoveWindow(g_edArtist, 300, y, 180, 24, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_EDIT_COPY - 1000), 490, y + 4, 70, 20, TRUE); MoveWindow(g_edCopy, 560, y, 180, 24, TRUE);
    MoveWindow(g_chkM3u, 750, y + 2, 200, 22, TRUE);
    y += 32;
    MoveWindow(g_log, 8, y, W - 16, H - y - 8, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Meiryo UI");
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) { HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, hi, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE); return h; };
        g_edPath = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_PATH, WS_EX_CLIENTEDGE);
        mk(L"BUTTON", L"開く...", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_OPEN);
        g_btnAnalyze = mk(L"BUTTON", L"解析開始", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_BTN_ANALYZE);
        g_btnCancel = mk(L"BUTTON", L"中止", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_CANCEL); EnableWindow(g_btnCancel, FALSE);
        mk(L"STATIC", L"自動プレイ追跡フレーム数:", SS_LEFT, ID_EDIT_FRAMES - 1000);
        g_edFrames = mk(L"EDIT", L"2400", WS_BORDER | ES_NUMBER, ID_EDIT_FRAMES, WS_EX_CLIENTEDGE);
        g_status = mk(L"STATIC", L"ROMファイルを開いて「解析開始」を押してください", SS_LEFT | SS_NOPREFIX, ID_STATUS);
        g_list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);
        addColumn(0, L"#", 40); addColumn(1, L"種別", 80); addColumn(2, L"信頼度", 100); addColumn(3, L"長さ", 110); addColumn(4, L"ch", 110); addColumn(5, L"起動手順 (NSF INIT の動作)", 400); addColumn(6, L"NSF検証", 120); addColumn(7, L"備考", 260);
        g_btnPlay = mk(L"BUTTON", L"▶ 再生", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_PLAY);
        g_btnStop = mk(L"BUTTON", L"■ 停止", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_STOP);
        mk(L"BUTTON", L"妥当な候補のみ選択", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_CHECKGOOD);
        mk(L"BUTTON", L"全て選択", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_CHECKALL);
        mk(L"BUTTON", L"選択解除", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_CHECKNONE);
        g_btnVerify = mk(L"BUTTON", L"NSF検証", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_VERIFY);
        g_btnSave = mk(L"BUTTON", L"チェック項目をNSF保存...", BS_PUSHBUTTON | WS_TABSTOP, ID_BTN_SAVE);
        mk(L"STATIC", L"Title", SS_LEFT, ID_EDIT_TITLE - 1000); g_edTitle = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_TITLE, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"Artist", SS_LEFT, ID_EDIT_ARTIST - 1000); g_edArtist = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_ARTIST, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"Copyright", SS_LEFT, ID_EDIT_COPY - 1000); g_edCopy = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_EDIT_COPY, WS_EX_CLIENTEDGE);
        g_chkM3u = mk(L"BUTTON", L"M3Uプレイリストも保存", BS_AUTOCHECKBOX | WS_TABSTOP, ID_CHK_M3U); SendMessageW(g_chkM3u, BM_SETCHECK, BST_CHECKED, 0);
        g_log = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ID_LOG, WS_EX_CLIENTEDGE);
        EnableWindow(g_btnSave, FALSE); EnableWindow(g_btnVerify, FALSE);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SIZE: layout(hwnd); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = 900; mm->ptMinTrackSize.y = 600; return 0; }
    case WM_DROPFILES: { wchar_t f[1024]; if (DragQueryFileW((HDROP)wp, 0, f, 1024)) { SetWindowTextW(g_edPath, f); if (!g_worker.joinable() || IsWindowEnabled(g_btnAnalyze)) startAnalysis(); } DragFinish((HDROP)wp); return 0; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_OPEN: { wchar_t fn[1024] = L""; OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof ofn; ofn.hwndOwner = hwnd; ofn.lpstrFilter = L"NES ROM (*.nes)\0*.nes\0All files\0*.*\0"; ofn.lpstrFile = fn; ofn.nMaxFile = 1024; ofn.Flags = OFN_FILEMUSTEXIST; if (GetOpenFileNameW(&ofn)) { SetWindowTextW(g_edPath, fn); startAnalysis(); } return 0; }
        case ID_BTN_ANALYZE: startAnalysis(); return 0;
        case ID_BTN_CANCEL: g_cancel = true; setStatus(L"中止中..."); return 0;
        case ID_BTN_PLAY: playSelected(); return 0;
        case ID_BTN_STOP: g_player.shutdown(); setStatus(L"停止"); return 0;
        case ID_BTN_SAVE: saveNsf(); return 0;
        case ID_BTN_VERIFY: verifyAll(); return 0;
        case ID_BTN_CHECKALL: if (g_rip) for (size_t i = 0; i < g_rip->cands.size(); i++) ListView_SetCheckState(g_list, (int)i, TRUE); return 0;
        case ID_BTN_CHECKNONE: if (g_rip) for (size_t i = 0; i < g_rip->cands.size(); i++) ListView_SetCheckState(g_list, (int)i, FALSE); return 0;
        case ID_BTN_CHECKGOOD: if (g_rip) for (size_t i = 0; i < g_rip->cands.size(); i++) ListView_SetCheckState(g_list, (int)i, g_rip->cands[i].confidence > 0 && !g_rip->cands[i].suspicious); return 0;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == ID_LIST && nm->code == NM_DBLCLK) { playSelected(); }
        if (nm->idFrom == ID_LIST && nm->code == LVN_KEYDOWN) { NMLVKEYDOWN* k = (NMLVKEYDOWN*)lp; if (k->wVKey == VK_RETURN) playSelected(); if (k->wVKey == VK_ESCAPE) g_player.shutdown(); }
        return 0;
    }
    case WM_APP_PROGRESS: { std::string* s = (std::string*)lp; setStatus(utf8ToW(*s)); delete s; return 0; }
    case WM_APP_PLAYPOS: { if (g_player.running) { wchar_t b[128]; swprintf(b, 128, L"再生中 %s", fmtTime((int)wp, 1.0).c_str()); /* keep status */ } return 0; }
    case WM_APP_DONE: {
        if (g_worker.joinable()) g_worker.join();
        EnableWindow(g_btnAnalyze, TRUE); EnableWindow(g_btnCancel, FALSE);
        if (g_rip) appendLog(utf8ToW(g_rip->drv.log));
        if (wp && g_rip) {
            fillList(); EnableWindow(g_btnSave, TRUE); EnableWindow(g_btnVerify, TRUE);
            int mu = 0, sf = 0; for (auto& c : g_rip->cands) (c.isMusic ? mu : sf)++;
            wchar_t b[256]; swprintf(b, 256, L"解析完了: 曲候補 %d 件 / 効果音候補 %d 件。行を選んで「再生」で試聴、チェックした項目をNSF保存できます。", mu, sf); setStatus(b);
        } else {
            std::wstring e = g_rip ? utf8ToW(g_rip->error) : L"";
            setStatus(g_cancel ? L"中止しました" : (L"解析失敗: " + e));
            if (!g_cancel) MessageBoxW(hwnd, (L"候補を抽出できませんでした。\n" + e).c_str(), L"解析失敗", MB_ICONWARNING);
        }
        return 0;
    }
    case WM_CLOSE: g_cancel = true; g_player.shutdown(); if (g_worker.joinable()) g_worker.join(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int show) {
    installCrashHandler();
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"NesNsfRipperWnd"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NES ROM → NSF Ripper (エミュレーション追跡式)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, hi, nullptr);
    ShowWindow(g_hwnd, show); UpdateWindow(g_hwnd);
    if (cmd && cmd[0]) { std::wstring c = cmd; if (c.size() > 1 && c[0] == L'"') { c = c.substr(1); size_t q = c.find(L'"'); if (q != std::wstring::npos) c = c.substr(0, q); } SetWindowTextW(g_edPath, c.c_str()); startAnalysis(); }
    MSG m; while (GetMessageW(&m, nullptr, 0, 0)) { if (!IsDialogMessageW(g_hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    g_player.shutdown(); if (g_worker.joinable()) g_worker.join();
    return 0;
}
