// 2048 AI · Blue — native WebView2 host
// Bridges a native bitboard solver (Solver4x4.h) with an animated web UI.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <wrl/implements.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "solver/Solver4x4.h"

#ifndef WEB_ASSET_DIR
#define WEB_ASSET_DIR L"web"
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_APP_AI_RESULT (WM_APP + 1)

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- globals

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
// Persistent worker pool.
//
// The search runs once per move, so creating and joining N threads every move
// costs more than the search itself at these depths. One pool lives for the
// whole session and just hands out worker ids.
class Pool {
public:
    explicit Pool(int n) {
        for (int i = 1; i < n; ++i) th.emplace_back([this, i] { loop(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Runs f(0) on the caller and f(1..N-1) on the pool; returns when all finish.
    void run(const std::function<void(int)>& f) {
        {
            std::lock_guard<std::mutex> lk(m);
            job = &f;
            finished = 0;
            ++gen;
        }
        cv.notify_all();
        f(0);
        std::unique_lock<std::mutex> lk(m);
        done.wait(lk, [this] { return finished == (int)th.size(); });
        job = nullptr;
    }

private:
    void loop(int i) {
        int seen = 0;
        for (;;) {
            const std::function<void(int)>* f = nullptr;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || gen != seen; });
                if (stop) return;
                seen = gen;
                f = job;
            }
            if (f) (*f)(i);
            {
                std::lock_guard<std::mutex> lk(m);
                if (++finished == (int)th.size()) done.notify_one();
            }
        }
    }

    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cv;
    std::condition_variable done;
    const std::function<void(int)>* job = nullptr;
    int  gen = 0;
    int  finished = 0;
    bool stop = false;
};

static Solver4x4 g_solver;             
static std::vector<std::unique_ptr<Solver4x4>> g_par;
static int g_nPar = 1;
static std::unique_ptr<Pool> g_pool;

static std::mutex g_jobMutex;
static std::condition_variable g_jobCv;
static bool g_hasJob = false;
static bool g_shutdown = false;
static int g_jobId = 0;
static std::vector<std::vector<int>> g_jobBoard;

static std::wstring g_logPath;
static bool g_checkMode = false;   
static void LogLine(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    fputws(buf, f);
    fputws(L"\n", f);
    fclose(f);
}

struct AiResult {
    int   id;
    int   dir;      // 0 up, 1 right, 2 down, 3 left, -1 none
    float score[4]; // canonical order
    int   ms;
};

// ---------------------------------------------------------------- worker

static void WorkerMain() {
    for (;;) {
        int id = 0;
        std::vector<std::vector<int>> board;

        {
            std::unique_lock<std::mutex> lk(g_jobMutex);
            g_jobCv.wait(lk, [] { return g_hasJob || g_shutdown; });
            if (g_shutdown && !g_hasJob) return;
            id = g_jobId;
            board = g_jobBoard;
            g_hasJob = false;
        }

        const auto t0 = std::chrono::steady_clock::now();

        
        int depth = g_par[0]->searchDepth(board);
        if (depth < 1) depth = 1;
        if (depth > 23) depth = 23;

      
        int nSpawn[4] = {0, 0, 0, 0};
        int offTask[4] = {0, 0, 0, 0};
        int nTasks = 0;
        for (int d = 0; d < 4; ++d) {
            const int n = g_par[0]->freeAfterMove(board, d);
            nSpawn[d] = n > 0 ? n : 0;
            offTask[d] = nTasks;
            nTasks += nSpawn[d];
        }
        if (nTasks > 60) nTasks = 60;

        float part[64] = {0};
        std::atomic<int> next{0};
        auto work = [&](int tid) {
            Solver4x4& sv = *g_par[(std::size_t)tid];
            for (;;) {
                const int id = next.fetch_add(1, std::memory_order_relaxed);
                if (id >= nTasks) break;
                int d = 0;
                while (d < 3 && offTask[d + 1] <= id) ++d;
                part[id] = sv.scoreDirectionShard(board, d, depth, id - offTask[d], nSpawn[d]);
            }
        };

        g_pool->run(work);

        float sc[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        for (int d = 0; d < 4; ++d) {
            if (nSpawn[d] == 0) continue;
            bool ok = true;
            float sum = 0;
            for (int sh = 0; sh < nSpawn[d]; ++sh) {
                const float v = part[offTask[d] + sh];
                if (v <= -1e29f) { ok = false; break; }
                sum += v;
            }
            if (ok) sc[d] = sum;
        }

        const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        float canon[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        int bestDir = -1;
        float bestScore = -1e29f;

        for (int d = 0; d < 4; ++d) {
            int cd = 0;
            switch (d) {              // solver: 0 left, 1 right, 2 up, 3 down
            case 0: cd = 3; break;
            case 1: cd = 1; break;
            case 2: cd = 0; break;
            default: cd = 2; break;
            }
            canon[cd] = sc[d];
            if (sc[d] > -1e29f && sc[d] > bestScore) {
                bestScore = sc[d];
                bestDir = cd;
            }
        }

        if (g_checkMode) {
            int refDir = -1;
            float refBest = -1e29f;
            float refCanon[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
            const auto ref = g_solver.getAllMoveScores(board);
            for (const auto& m : ref) {
                int cd = 0;
                switch (m.dir) {
                case 0: cd = 3; break;
                case 1: cd = 1; break;
                case 2: cd = 0; break;
                default: cd = 2; break;
                }
                refCanon[cd] = m.score;
                if (m.allowed && m.score > refBest) { refBest = m.score; refDir = cd; }
            }
            if (refDir != bestDir) {
                LogLine(L"CHECK MISMATCH depth=%d par=%d(%.1f) ref=%d(%.1f) parS=[%.1f %.1f %.1f %.1f] refS=[%.1f %.1f %.1f %.1f]",
                    depth, bestDir, bestScore, refDir, refBest,
                    canon[0], canon[1], canon[2], canon[3],
                    refCanon[0], refCanon[1], refCanon[2], refCanon[3]);
            } else {
                LogLine(L"CHECK ok depth=%d dir=%d score=%.1f", depth, bestDir, bestScore);
            }
        }

        auto* res = new AiResult{ id, bestDir, { canon[0], canon[1], canon[2], canon[3] }, ms };
        if (!PostMessageW(g_hwnd, WM_APP_AI_RESULT, 0, reinterpret_cast<LPARAM>(res)))
            delete res;
    }
}

// ---------------------------------------------------------------- helpers

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : s.substr(0, pos);
}

static bool DirExists(const std::wstring& p) {
    const DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring AssetDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : ExeDir();
    return base + L"\\2048AIBlue";
}

// Pull one embedded resource out to disk. A build without app.rc simply has no
// resource, which the caller treats as "nothing to unpack".
static bool UnpackRes(int id, const std::wstring& path) {
    const HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) return false;
    const HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    const void* src = LockResource(hg);
    const DWORD size = SizeofResource(nullptr, hr);
    if (!src || !size) return false;
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = std::fwrite(src, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

// Unpack the UI and the WebView2 loader so the distributed exe needs no side
// files: the WebView2 runtime still comes from Windows, everything else is
// inside the exe and lands in %LOCALAPPDATA% on first run. Files are only
// rewritten when missing or when their size differs, so starts stay cheap and a
// truncated copy repairs itself.
static void EnsureAssets() {
    const std::wstring dir = AssetDir();
    const std::wstring web = dir + L"\\web";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(web.c_str(), nullptr);
    CreateDirectoryW((web + L"\\wasm").c_str(), nullptr);

    // The engine ships as a WebAssembly worker pool, so engine.js, ai-worker.js
    // and wasm/solver.wasm have to be unpacked too; with only the three UI files
    // the single-file build starts and then never makes a move.
    struct Item { int id; const wchar_t* name; const wchar_t* sub; };
    const Item items[] = {
        {1000, L"index.html",   L""},
        {1001, L"app.css",      L""},
        {1002, L"app.js",       L""},
        {1003, L"engine.js",    L""},
        {1004, L"ai-worker.js", L""},
        {1005, L"solver.wasm",  L"wasm"},
    };
    for (const auto& it : items) {
        std::wstring path = web + L"\\";
        if (it.sub[0]) { path += it.sub; path += L"\\"; }
        path += it.name;
        const HRSRC hr = FindResourceW(nullptr, MAKEINTRESOURCEW(it.id), RT_RCDATA);
        if (!hr) continue;
        const DWORD want = SizeofResource(nullptr, hr);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) != 0;
        if (!exists || fad.nFileSizeLow != want) UnpackRes(it.id, path);
    }

    // The loader is delay-loaded, so the first WebView2 call resolves it from the
    // search path; putting the unpacked directory there is enough.
    const std::wstring beside = ExeDir() + L"\\WebView2Loader.dll";
    if (!FileExists(beside)) UnpackRes(2000, dir + L"\\WebView2Loader.dll");
    SetDllDirectoryW(dir.c_str());
}

// Directory the UI files are actually served from: a web\ folder next to the exe
// wins (that is how the build tree works during development), otherwise the copy
// that EnsureAssets() unpacked into %LOCALAPPDATA% on first run.
static std::wstring WebDir() {
    std::wstring local = ExeDir() + L"\\web";
    if (DirExists(local)) return local;
    return AssetDir() + L"\\web";
}

static std::wstring UserDataDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : ExeDir();
    return base + L"\\2048AIBlue\\WebView2";
}

static bool ParseAi(const std::wstring& msg, int& id, std::vector<std::vector<int>>& board) {
    std::wistringstream is(msg);
    std::wstring tag;
    if (!(is >> tag) || tag != L"AI") return false;
    if (!(is >> id)) return false;

    board.assign(4, std::vector<int>(4, 0));
    for (int i = 0; i < 16; ++i) {
        int v = 0;
        if (!(is >> v)) return false;
        if (v < 0) v = 0;
        board[i / 4][i % 4] = v;
    }
    return true;
}

static void SubmitJob(int id, std::vector<std::vector<int>> board) {
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        g_jobId = id;
        g_jobBoard = std::move(board);
        g_hasJob = true;   // newest request wins; older ones are dropped
    }
    g_jobCv.notify_one();
}

// ---------------------------------------------------------------- webview


static void ApplyZoom() {
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    const double sc = (double)GetDpiForWindow(g_hwnd) / 96.0;
    const double cssW = rc.right / sc;
    const double cssH = rc.bottom / sc;

    double z = cssW / 540.0;
    if (cssH / 904.0 < z) z = cssH / 904.0;
    if (z > 1.0) z = 1.0;
    if (z < 0.62) z = 0.62;
    g_controller->put_ZoomFactor(z);
}

static void ResizeWebView() {
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
    ApplyZoom();
}

static void SetupWebView(ICoreWebView2Controller* controller) {
    g_controller = controller;
    controller->get_CoreWebView2(&g_webview);
    ResizeWebView();

    ApplyZoom();

    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&ctrl2)))) {
        COREWEBVIEW2_COLOR bg{};
        bg.A = 255; bg.R = 5; bg.G = 10; bg.B = 22;
        ctrl2->put_DefaultBackgroundColor(bg);
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(g_webview->QueryInterface(IID_PPV_ARGS(&wv3)))) {
        wv3->SetVirtualHostNameToFolderMapping(
            L"game.2048ai", WebDir().c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    EventRegistrationToken token{};
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
                std::wstring msg(raw);
                CoTaskMemFree(raw);

                if (msg.rfind(L"ERR", 0) == 0) {
                    LogLine(L"[js] %s", msg.c_str());
                } else if (msg == L"NEW") {
                    g_solver.newGame();
                    for (auto& sv : g_par) sv->newGame();   
                    LogLine(L"[game] new board, caches cleared");
                } else if (msg.rfind(L"AI ", 0) == 0) {
                    int id = 0;
                    std::vector<std::vector<int>> board;
                    LogLine(L"<- %s", msg.c_str());
                    if (ParseAi(msg, id, board)) SubmitJob(id, std::move(board));
                } else if (msg == L"QUIT") {
                    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                } else if (msg == L"MIN") {
                    ShowWindow(g_hwnd, SW_MINIMIZE);
                } else if (msg == L"MAX") {
                    const bool maxed = IsZoomed(g_hwnd) != FALSE;
                    ShowWindow(g_hwnd, maxed ? SW_RESTORE : SW_MAXIMIZE);
                } else if (msg.rfind(L"THEME ", 0) == 0) {
                    const BOOL dark = (msg == L"THEME dark");
                    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
                    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else if (msg == L"DEVTOOLS") {
                    if (g_webview) g_webview->OpenDevToolsWindow();
                }
                return S_OK;
            }).Get(), &token);

    std::wstring url = L"https://game.2048ai/index.html";
    wchar_t q[256] = {};
    if (GetEnvironmentVariableW(L"G2048_QUERY", q, 256) > 0) {
        url += L"?";
        url += q;
    }
    LogLine(L"navigate %s (web=%s)", url.c_str(), WebDir().c_str());
    g_webview->Navigate(url.c_str());
}

static void CreateWebView() {
    const std::wstring dataDir = UserDataDir();

    ComPtr<CoreWebView2EnvironmentOptions> options =
        Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (options) {
        options->put_AdditionalBrowserArguments(
            L"--disable-background-timer-throttling "
            L"--disable-renderer-backgrounding "
            L"--disable-backgrounding-occluded-windows");
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    MessageBoxW(g_hwnd,
                        L"无法初始化 WebView2 运行时。\n\n请安装 Microsoft Edge WebView2 Runtime 后重试。",
                        L"2048 AI", MB_ICONERROR | MB_OK);
                    return E_FAIL;
                }
                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hr2) || !controller) {
                                MessageBoxW(g_hwnd, L"创建 WebView2 控制器失败。",
                                    L"2048 AI", MB_ICONERROR | MB_OK);
                                return E_FAIL;
                            }
                            SetupWebView(controller);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const UINT dpi = g_hwnd ? GetDpiForWindow(g_hwnd) : 96;
        const double sc = dpi / 96.0;
        mmi->ptMinTrackSize.x = (LONG)(420 * sc);
        mmi->ptMinTrackSize.y = (LONG)(540 * sc);
        return 0;
    }

    case WM_DPICHANGED: {
        auto* target = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, target->left, target->top,
            target->right - target->left, target->bottom - target->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_APP_AI_RESULT: {
        auto* res = reinterpret_cast<AiResult*>(lp);
        if (res) {
            LogLine(L"-> id=%d dir=%d ms=%d", res->id, res->dir, res->ms);
            if (g_webview) {
                wchar_t script[512];
                swprintf_s(script, 512,
                    L"window.__aiRes&&window.__aiRes(%d,%d,%.1f,%.1f,%.1f,%.1f,%d);",
                    res->id, res->dir,
                    res->score[0], res->score[1], res->score[2], res->score[3], res->ms);
                const HRESULT hre = g_webview->ExecuteScript(script, nullptr);
                if (FAILED(hre)) LogLine(L"[exec] failed hr=0x%08X", (unsigned)hre);
            }
            delete res;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_F12 && g_webview) {
            g_webview->OpenDevToolsWindow();
            return 0;
        }
        break;

    case WM_DESTROY:
        {
            std::lock_guard<std::mutex> lk(g_jobMutex);
            g_shutdown = true;
            g_hasJob = false;
        }
        g_jobCv.notify_all();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnsureAssets();   // before the first WebView2 call: the loader is delay-loaded

    {
        wchar_t lp[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"G2048_LOG", lp, MAX_PATH) > 0) g_logPath = lp;
        if (GetEnvironmentVariableW(L"G2048_CHECK", lp, MAX_PATH) > 0) g_checkMode = true;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(5, 10, 22));
    wc.lpszClassName = L"Game2048AIBlueWindow";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPEDWINDOW;

   
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;

    UINT dpi = 96;
    {
        HDC dc = GetDC(nullptr);
        if (dc) { dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    const double sc = dpi / 96.0;

    int clientW = (int)(540 * sc);
    int clientH = (int)(904 * sc);
    if (clientW > workW - 24) clientW = workW - 24;
    if (clientH > workH - 24) clientH = workH - 24;

    RECT rc{ 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;
    const int winX = work.left + (workW - winW) / 2;
    const int winY = work.top + (workH - winH) / 2;

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"liu珍妮 2048 ai",
        style,
        winX, winY, winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    g_nPar = std::max(1, (int)std::thread::hardware_concurrency());
    if (g_nPar > 16) g_nPar = 16;
    g_pool = std::make_unique<Pool>(g_nPar);
    for (int i = 0; i < g_nPar; ++i) {
        auto sv = std::make_unique<Solver4x4>();
        sv->setCacheBits(18);
        sv->setTtDeep(true);
        sv->setBudget((std::size_t)1 << 21);
        g_par.push_back(std::move(sv));
    }
    g_solver.setBudget((std::size_t)1 << 17);

    std::thread worker(WorkerMain);
    CreateWebView();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        g_shutdown = true;
    }
    g_jobCv.notify_all();
    if (worker.joinable()) worker.join();

    g_webview.Reset();
    g_controller.Reset();
    CoUninitialize();
    return 0;
}
