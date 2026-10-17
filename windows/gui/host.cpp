// The GUI host: the window, WebView2 and the page's messages, the cameras'
// source runners, the preview and the notification area icon.
#include "gui.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>

#include "WebView2.h"
#include "resource.h"

using Microsoft::WRL::ComPtr;

namespace vcam::gui {

namespace {

constexpr wchar_t kWindowClass[] = L"VCamWindow";
constexpr wchar_t kOrigin[] = L"https://vcam.ui/";
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kPreviewTimer = 2;
constexpr int kMinWidth = 960, kMinHeight = 620;  // DIPs
constexpr UINT kTrayId = 1;
constexpr size_t kPreviewBytes = 1920 * 1080 * 4;
constexpr wchar_t kWebView2Download[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";

// WebView2 completion and event handlers from lambdas: one COM object per
// interface, whose Invoke forwards to the function.
template <typename Interface, typename Signature>
class Handler;

template <typename Interface, typename... Args>
class Handler<Interface, HRESULT (STDMETHODCALLTYPE Interface::*)(Args...)> final : public Interface {
public:
    explicit Handler(std::function<HRESULT(Args...)> fn) : fn_(std::move(fn)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(Interface))) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --refs_;
        if (left == 0) {
            delete this;
        }
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn_(args...); }

private:
    std::atomic<ULONG> refs_{1};
    std::function<HRESULT(Args...)> fn_;
};

template <typename Interface, typename Fn>
ComPtr<Interface> MakeHandler(Fn fn)
{
    ComPtr<Interface> handler;
    handler.Attach(new Handler<Interface, decltype(&Interface::Invoke)>(std::move(fn)));
    return handler;
}

std::wstring LocalAppDataDir()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buffer) : std::wstring(L".");
}

Rgb ParseHex(const std::string& text, Rgb fallback)
{
    Rgb c;
    return app::ParseColor(text, &c) ? c : fallback;
}

// The page's description of a camera's picture options.
app::SourceOptions OptionsFromJson(int camera, const Json& source, const Json& options)
{
    app::SourceOptions o;
    o.camera = camera + 1;
    const std::string& scale = options["scale"].string();
    o.scale = scale == "fill" ? ScaleMode::Fill : scale == "stretch" ? ScaleMode::Stretch : ScaleMode::Fit;
    o.mirror = options["mirror"].boolean();
    o.flip = options["flip"].boolean();
    o.background = ParseHex(options["background"].string(), Rgb{0, 0, 0});
    o.always = options["always"].boolean();
    o.cursor = source["cursor"].boolean(true);
    o.loop = source["loop"].boolean(true);
    o.quiet = true;
    return o;
}

// Builds the factory for a source the page describes; empty with *error when incomplete.
app::SourceFactory FactoryFromJson(const Json& source, const app::SourceOptions& o, std::string* error)
{
    const std::string& kind = source["kind"].string();
    if (kind == "bars") {
        return [o] { return app::MakeBarsSource(o); };
    }
    if (kind == "colour") {
        const Rgb colour = ParseHex(source["color"].string(), Rgb{31, 111, 235});
        return [colour] { return app::MakeColorSource(colour); };
    }
    if (kind == "image" || kind == "video") {
        const std::wstring path = source["path"].wstring();
        if (path.empty()) {
            *error = kind == "image" ? "Choose a picture first." : "Choose a video first.";
            return {};
        }
        if (kind == "image") {
            return [path] { return app::MakeImageSource(path); };
        }
        const bool loop = o.loop;
        return [path, loop] { return app::MakeVideoSource(path, loop); };
    }
    if (kind == "webcam") {
        const std::string name = source["webcam"].string();
        return [name] { return app::MakeWebcamSource(name); };
    }
    if (kind == "screen") {
        const int monitor = std::max(1, source["monitor"].integer(1));
        const bool cursor = o.cursor;
        return [monitor, cursor] { return app::MakeScreenSource(monitor, cursor); };
    }
    if (kind == "window") {
        const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(strtoull(source["window"].string().c_str(), nullptr, 10)));
        const bool cursor = o.cursor;
        return [window, cursor] { return app::MakeWindowSourceFor(window, cursor); };
    }
    *error = "Unknown source.";
    return {};
}

// --- output of the tool library (installation, sources) ---------------------------

std::mutex g_output_lock;
std::string g_output;           // collected while an operation runs
bool g_collecting = false;
std::function<void(const std::string&)> g_error_sink;  // set by the App: shows errors

void OnOutput(bool error, const std::string& text)
{
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(g_output_lock);
        if (g_collecting) {
            g_output += text;
            return;
        }
        sink = g_error_sink;
    }
    win::Log("%s", text.c_str());
    if (error && sink) {
        std::string line = text;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        while (!line.empty() && (line.front() == '\n' || line.front() == '\r')) line.erase(0, 1);
        if (!line.empty()) {
            sink(line);
        }
    }
}

// Runs an operation of the tool library and returns what it printed.
template <typename Fn>
std::string Collect(Fn fn, int* code)
{
    {
        std::lock_guard<std::mutex> lock(g_output_lock);
        g_collecting = true;
        g_output.clear();
    }
    *code = fn();
    std::lock_guard<std::mutex> lock(g_output_lock);
    g_collecting = false;
    return std::move(g_output);
}

// The most useful line of an operation's output, for a short message.
std::string LastErrorLine(const std::string& output)
{
    std::string best;
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && (best.empty() || line.find("failed") != std::string::npos ||
                              line.find("missing") != std::string::npos || line.find("need") != std::string::npos)) {
            best = line;
        }
        start = end + 1;
    }
    return best;
}

struct CameraState {
    std::unique_ptr<app::SourceRunner> runner;  // App::runners_lock_
    uint64_t generation = 0;                     // App::runners_lock_: newest start/stop request
    std::string error;                           // UI thread: why it stopped
    bool ended = false;                          // UI thread
    win::SharedFrames ring;                      // UI thread: status and other producers' frames
    int64_t next_open = 0;
    std::string placeholder_key;                 // UI thread: the placeholder last shown
};

class App {
public:
    int Run(HINSTANCE instance, const std::wstring& install_name, bool background);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);

    void ShowWindowNow();
    void HideToTray();
    void Quit();
    void UpdateKeepAlive();
    void ResizeWebView();

    void CreateWebView();
    void WebViewFailed(HRESULT hr);
    void ConfigureWebView();
    HRESULT ServeResource(ICoreWebView2WebResourceRequestedEventArgs* args);
    void Send(const Json& message);
    void OnPageMessage(const Json& m);

    Json InitMessage();
    Json StatusMessage();
    void SendLists();
    void SendThumb(const std::wstring& path);

    void StartCamera(int camera, const Json& source, const Json& options);
    void StopCamera(int camera);
    void RestoreRunningCameras();
    bool AnyRunning();

    void PumpPreview();
    void PollPreview();

    void Install(const std::wstring& name);
    void SaveSettings(const Json& m);
    void Repair();
    void Uninstall();
    void RestartService();

    void PostToUi(std::function<void()> fn);
    void Toast(const std::string& level, const std::string& text);
    void Busy(const std::string& text);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTray();
    void ShowTrayMenu();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT show_message_ = 0, taskbar_created_ = 0;
    bool mica_ = false;
    bool elevated_ = false;
    bool tray_ = true;
    bool tray_icon_ = false;
    bool tray_hint_shown_ = false;
    bool quitting_ = false;
    int initial_show_ = SW_SHOW;
    win::Config config_;

    ComPtr<ICoreWebView2Environment> env_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    ComPtr<ICoreWebView2SharedBuffer> shared_;
    BYTE* shared_bytes_ = nullptr;
    bool webview_requested_ = false;
    bool page_ready_ = false;
    bool page_wants_frame_ = false;

    Preview preview_;
    std::mutex runners_lock_;
    CameraState cameras_[win::kMaxCameras];

    std::mutex ui_lock_;
    std::deque<std::function<void()>> ui_queue_;

    // Last: destroyed first, so no job outlives what it uses.
    Worker worker_;
};

App* g_app = nullptr;

// --- running -------------------------------------------------------------------------

int App::Run(HINSTANCE instance, const std::wstring& install_name, bool background)
{
    instance_ = instance;
    g_app = this;
    elevated_ = app::IsElevated();
    config_ = win::LoadConfig();
    tray_ = LoadTraySetting();
    mica_ = MicaSupported();
    show_message_ = RegisterWindowMessageW(L"VCam.Show");
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    win::LogInit(L"vcam-gui", true);
    app::SetOutputSink(OnOutput);
    {
        std::lock_guard<std::mutex> lock(g_output_lock);
        g_error_sink = [this](const std::string& line) { PostToUi([this, line] { Toast("error", line); }); };
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    window_ = CreateWindowExW(0, kWindowClass, L"VCam", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760, nullptr, nullptr, instance, this);
    if (!window_) {
        return 1;
    }
    // Let a second, non-elevated launch reach this window.
    ChangeWindowMessageFilterEx(window_, show_message_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(window_, taskbar_created_, MSGFLT_ALLOW, nullptr);
    ApplyWindowTheme(window_, mica_);

    const UINT dpi = GetDpiForWindow(window_);
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (LoadPlacement(&placement)) {
        initial_show_ = placement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOW;
        placement.showCmd = SW_HIDE;
        SetWindowPlacement(window_, &placement);
    } else {
        const int w = MulDiv(1180, static_cast<int>(dpi), 96), h = MulDiv(760, static_cast<int>(dpi), 96);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowPos(window_, nullptr, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    SetTimer(window_, kStatusTimer, 500, nullptr);
    SetTimer(window_, kPreviewTimer, 66, nullptr);
    if (tray_ || background) {
        AddTrayIcon();
    }
    if (config_.install_dir.empty() || !install_name.empty()) {
        background = false;
    }
    if (!config_.install_dir.empty() && elevated_) {
        worker_.Post([] { app::EnsureCamerasRegistered(); });  // Windows sometimes drops cameras
    }
    RestoreRunningCameras();
    if (!background) {
        ShowWindowNow();
    }
    if (!install_name.empty() && elevated_) {
        Install(install_name);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (int i = 0; i < win::kMaxCameras; i++) {
        std::unique_ptr<app::SourceRunner> runner;
        {
            std::lock_guard<std::mutex> lock(runners_lock_);
            cameras_[i].generation++;  // queued starts become no-ops
            runner = std::move(cameras_[i].runner);
        }
        runner.reset();  // stops it
    }
    {
        std::lock_guard<std::mutex> lock(g_output_lock);
        g_error_sink = nullptr;
    }
    return 0;
}

void App::ShowWindowNow()
{
    if (IsIconic(window_)) {
        ShowWindow(window_, SW_RESTORE);
    } else {
        ShowWindow(window_, initial_show_);
        initial_show_ = SW_SHOW;
    }
    SetForegroundWindow(window_);
    CreateWebView();
    UpdateKeepAlive();
}

void App::HideToTray()
{
    if (!tray_icon_) {
        AddTrayIcon();
    }
    ShowWindow(window_, SW_HIDE);
    UpdateKeepAlive();
    if (!tray_hint_shown_) {
        tray_hint_shown_ = true;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = window_;
        nid.uID = kTrayId;
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"VCam keeps running");
        wcscpy_s(nid.szInfo, L"Your cameras keep their sources. Open VCam from the notification area.");
        nid.dwInfoFlags = NIIF_NONE | NIIF_RESPECT_QUIET_TIME;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void App::Quit()
{
    if (quitting_) {
        return;
    }
    quitting_ = true;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(window_, &placement)) {
        SavePlacement(placement);
    }
    ShowWindow(window_, SW_HIDE);
    RemoveTrayIcon();
    DestroyWindow(window_);
}

// While the window shows the preview, sources keep running even if no app
// watches; hidden, they pause like the command-line tool's.
void App::UpdateKeepAlive()
{
    const bool visible = IsWindowVisible(window_) && !IsIconic(window_);
    std::lock_guard<std::mutex> lock(runners_lock_);
    for (auto& camera : cameras_) {
        if (camera.runner) {
            camera.runner->SetKeepAlive(visible);
        }
    }
}

void App::PostToUi(std::function<void()> fn)
{
    {
        std::lock_guard<std::mutex> lock(ui_lock_);
        ui_queue_.push_back(std::move(fn));
    }
    PostMessageW(window_, WM_APP_POST, 0, 0);
}

void App::Toast(const std::string& level, const std::string& text)
{
    Json m = Json::Object();
    m["type"] = "toast";
    m["level"] = level;
    m["text"] = text;
    Send(m);
}

void App::Busy(const std::string& text)
{
    Json m = Json::Object();
    m["type"] = "busy";
    m["text"] = text.empty() ? Json() : Json(text);
    Send(m);
}

// --- window --------------------------------------------------------------------------

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == show_message_) {
        ShowWindowNow();
        return 0;
    }
    if (message == taskbar_created_) {
        if (tray_icon_) {
            tray_icon_ = false;
            AddTrayIcon();
        }
        return 0;
    }
    switch (message) {
    case WM_APP_POST: {
        std::deque<std::function<void()>> queue;
        {
            std::lock_guard<std::mutex> lock(ui_lock_);
            queue.swap(ui_queue_);
        }
        for (auto& fn : queue) {
            fn();
        }
        return 0;
    }
    case WM_APP_FRAME:
        PumpPreview();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowWindowNow();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        default:
            break;
        }
        return 0;
    case WM_TIMER:
        if (wparam == kStatusTimer) {
            const Json status = StatusMessage();
            Send(status);
            UpdateTray();
        } else if (wparam == kPreviewTimer) {
            PollPreview();
        }
        return 0;
    case WM_SIZE:
        ResizeWebView();
        UpdateKeepAlive();
        return 0;
    case WM_MOVE:
    case WM_MOVING:
        if (controller_) {
            controller_->NotifyParentWindowPositionChanged();
        }
        return 0;
    case WM_SETFOCUS:
        if (controller_) {
            controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT dpi = GetDpiForWindow(window_);
        info->ptMinTrackSize.x = MulDiv(kMinWidth, static_cast<int>(dpi), 96);
        info->ptMinTrackSize.y = MulDiv(kMinHeight, static_cast<int>(dpi), 96);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
            ApplyWindowTheme(window_, mica_);
            std::string light, dark;
            AccentColors(&light, &dark);
            Json m = Json::Object();
            m["type"] = "accent";
            m["light"] = light;
            m["dark"] = dark;
            Send(m);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // Until the page shows: the theme's window colour, or black, which lets Mica through.
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        const COLORREF colour = mica_ ? RGB(0, 0, 0) : SystemUsesDarkTheme() ? RGB(0x20, 0x20, 0x20) : RGB(0xf3, 0xf3, 0xf3);
        HBRUSH brush = CreateSolidBrush(colour);
        FillRect(dc, &ps.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (tray_ && AnyRunning()) {
            HideToTray();
        } else {
            Quit();
        }
        return 0;
    case WM_ENDSESSION:
        if (wparam) {
            Quit();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window_, kStatusTimer);
        KillTimer(window_, kPreviewTimer);
        if (controller_) {
            controller_->Close();
            controller_.Reset();
            webview_.Reset();
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

void App::ResizeWebView()
{
    if (controller_) {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        controller_->put_Bounds(bounds);
    }
}

// --- notification area -------------------------------------------------------------------

void App::AddTrayIcon()
{
    if (tray_icon_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcscpy_s(nid.szTip, L"VCam");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        tray_icon_ = true;
    }
}

void App::RemoveTrayIcon()
{
    if (!tray_icon_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_icon_ = false;
}

void App::UpdateTray()
{
    if (!tray_icon_) {
        return;
    }
    int running = 0;
    {
        std::lock_guard<std::mutex> lock(runners_lock_);
        for (int i = 0; i < config_.cameras && i < win::kMaxCameras; i++) {
            running += cameras_[i].runner ? 1 : 0;
        }
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    swprintf_s(nid.szTip, L"VCam: %s", running == 0 ? L"no source running"
                                       : running == 1 ? L"1 camera running"
                                                      : L"cameras running");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open VCam");
    SetMenuDefaultItem(menu, 1, FALSE);
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(runners_lock_);
        for (int i = 0; i < config_.cameras && i < win::kMaxCameras; i++) {
            if (cameras_[i].runner) {
                if (!any) {
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    any = true;
                }
                AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(100 + i), (L"Stop " + config_.names[i]).c_str());
            }
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Quit VCam");
    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(window_);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, p.x, p.y, window_, nullptr));
    DestroyMenu(menu);
    if (cmd == 1) {
        ShowWindowNow();
    } else if (cmd == 2) {
        Quit();
    } else if (cmd >= 100 && cmd < 100 + win::kMaxCameras) {
        StopCamera(static_cast<int>(cmd - 100));
        // Keep the page's idea of what runs in step.
        Json state = Json::Parse(LoadUiState());
        if (state.is_object() && state["cameras"].is_array()) {
            Json cameras = Json::Array();
            for (size_t i = 0; i < state["cameras"].size(); i++) {
                Json c = state["cameras"][i];
                if (i == cmd - 100) {
                    c["running"] = false;
                }
                cameras.push_back(c);
            }
            state["cameras"] = cameras;
            SaveUiState(state.Dump());
        }
        Json m = Json::Object();
        m["type"] = "stopped";
        m["camera"] = static_cast<int>(cmd - 100);
        Send(m);
    }
}

// --- WebView2 -----------------------------------------------------------------------------

void App::CreateWebView()
{
    if (webview_requested_) {
        return;
    }
    webview_requested_ = true;
    const std::wstring data = LocalAppDataDir() + L"\\vcam\\WebView2";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        MakeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    WebViewFailed(result);
                    return S_OK;
                }
                env_ = env;
                return env_->CreateCoreWebView2Controller(
                    window_, MakeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                     if (FAILED(result) || !controller) {
                                         WebViewFailed(result);
                                         return S_OK;
                                     }
                                     controller_ = controller;
                                     controller_->get_CoreWebView2(&webview_);
                                     ConfigureWebView();
                                     return S_OK;
                                 })
                                 .Get());
            })
            .Get());
    if (FAILED(hr)) {
        WebViewFailed(hr);
    }
}

void App::WebViewFailed(HRESULT hr)
{
    win::Log("WebView2 failed: 0x%08lX", static_cast<unsigned long>(hr));
    const bool missing = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    const std::wstring text = missing
        ? std::wstring(L"VCam needs the Microsoft Edge WebView2 Runtime, which is not installed.\n\nOpen the download page?")
        : L"VCam could not start its window (WebView2 error 0x" + [hr] {
              wchar_t code[16];
              swprintf_s(code, L"%08lX", static_cast<unsigned long>(hr));
              return std::wstring(code);
          }() + L").\n\nOpen the WebView2 Runtime download page to repair it?";
    if (MessageBoxW(window_, text.c_str(), L"VCam", MB_ICONWARNING | MB_YESNO) == IDYES) {
        ShellExecuteW(window_, L"open", kWebView2Download, nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (!AnyRunning()) {
        Quit();
    } else {
        HideToTray();
        webview_requested_ = false;
    }
}

void App::ConfigureWebView()
{
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDevToolsEnabled(GetEnvironmentVariableW(L"VCAM_DEVTOOLS", nullptr, 0) > 0);
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
    }
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_.As(&controller2))) {
        // Transparent over Mica; otherwise the theme's window colour, so nothing flashes white.
        const bool dark = SystemUsesDarkTheme();
        COREWEBVIEW2_COLOR colour = mica_ ? COREWEBVIEW2_COLOR{0, 0, 0, 0}
                                          : (dark ? COREWEBVIEW2_COLOR{255, 0x20, 0x20, 0x20}
                                                  : COREWEBVIEW2_COLOR{255, 0xf3, 0xf3, 0xf3});
        controller2->put_DefaultBackgroundColor(colour);
    }
    ResizeWebView();

    webview_->AddWebResourceRequestedFilter(L"https://vcam.ui/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken token{};
    webview_->add_WebResourceRequested(
        MakeHandler<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                return ServeResource(args);
            })
            .Get(),
        &token);
    // The page never leaves VCam's own origin, and opens no windows.
    webview_->add_NavigationStarting(
        MakeHandler<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    if (wcsncmp(uri, kOrigin, wcslen(kOrigin)) != 0) {
                        args->put_Cancel(TRUE);
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_NewWindowRequested(
        MakeHandler<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_WebMessageReceived(
        MakeHandler<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR json = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                    bool ok = false;
                    const Json message = Json::Parse(win::Narrow(json), &ok);
                    CoTaskMemFree(json);
                    if (ok && message.is_object()) {
                        OnPageMessage(message);
                    }
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_ProcessFailed(
        MakeHandler<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                args->get_ProcessFailedKind(&kind);
                win::Log("WebView2 process failed (%d)", static_cast<int>(kind));
                page_ready_ = false;
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                    controller_.Reset();
                    webview_.Reset();
                    env_.Reset();
                    shared_.Reset();
                    webview_requested_ = false;
                    if (IsWindowVisible(window_)) {
                        CreateWebView();
                    }
                } else if (webview_) {
                    webview_->Reload();
                }
                return S_OK;
            })
            .Get(),
        &token);

    // Preview frames travel through shared memory, not as messages.
    ComPtr<ICoreWebView2Environment12> env12;
    if (SUCCEEDED(env_.As(&env12)) && SUCCEEDED(env12->CreateSharedBuffer(kPreviewBytes, &shared_))) {
        shared_->get_Buffer(&shared_bytes_);
    }
    webview_->Navigate(L"https://vcam.ui/index.html");
}

HRESULT App::ServeResource(ICoreWebView2WebResourceRequestedEventArgs* args)
{
    ComPtr<ICoreWebView2WebResourceRequest> request;
    LPWSTR uri = nullptr;
    if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&uri)) || !uri) {
        return S_OK;
    }
    std::wstring path(uri);
    CoTaskMemFree(uri);
    path = path.substr(std::min(path.size(), wcslen(kOrigin)));
    path = path.substr(0, path.find_first_of(L"?#"));
    struct Resource {
        const wchar_t* path;
        const wchar_t* name;
        const wchar_t* type;
    };
    static const Resource resources[] = {
        {L"index.html", L"UI_INDEX", L"text/html; charset=utf-8"},
        {L"app.css", L"UI_CSS", L"text/css; charset=utf-8"},
        {L"app.js", L"UI_JS", L"text/javascript; charset=utf-8"},
    };
    ComPtr<ICoreWebView2WebResourceResponse> response;
    for (const Resource& r : resources) {
        if (path != r.path) {
            continue;
        }
        HRSRC found = FindResourceW(instance_, r.name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL loaded = found ? LoadResource(instance_, found) : nullptr;
        const void* data = loaded ? LockResource(loaded) : nullptr;
        if (!data) {
            break;
        }
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(data), SizeofResource(instance_, found)));
        const std::wstring headers = std::wstring(L"Content-Type: ") + r.type + L"\r\nCache-Control: no-store";
        env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
        break;
    }
    if (!response) {
        env_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
    }
    return args->put_Response(response.Get());
}

void App::Send(const Json& message)
{
    if (webview_ && page_ready_) {
        webview_->PostWebMessageAsJson(win::Widen(message.Dump()).c_str());
    }
}

// --- the page's messages -------------------------------------------------------------------

void App::OnPageMessage(const Json& m)
{
    const std::string& type = m["type"].string();
    if (type == "ready") {
        page_ready_ = true;
        page_wants_frame_ = true;
        config_ = win::LoadConfig();
        Send(InitMessage());
        ComPtr<ICoreWebView2_17> webview17;
        if (shared_ && SUCCEEDED(webview_.As(&webview17))) {
            webview17->PostSharedBufferToScript(shared_.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
                                                L"{\"kind\":\"preview\"}");
        }
        SendLists();
        Json thumb = Json::Object();
        thumb["type"] = "placeholderThumb";
        thumb["thumb"] = PlaceholderThumbnail(256, 144);
        Send(thumb);
        const Json state = Json::Parse(LoadUiState());
        for (size_t i = 0; i < state["cameras"].size(); i++) {
            const std::wstring path = state["cameras"][i]["source"]["path"].wstring();
            if (!path.empty()) {
                SendThumb(path);
            }
        }
        for (auto& camera : cameras_) {
            camera.placeholder_key.clear();
        }
        Send(StatusMessage());
    } else if (type == "select") {
        const int camera = std::clamp(m["camera"].integer(), 0, win::kMaxCameras - 1);
        preview_.Select(camera);
        cameras_[camera].placeholder_key.clear();
        PollPreview();
    } else if (type == "previewSize") {
        preview_.SetSize(m["w"].integer(640), m["h"].integer(360));
        for (auto& camera : cameras_) {
            camera.placeholder_key.clear();
        }
        PollPreview();
    } else if (type == "frameAck") {
        page_wants_frame_ = true;
        PumpPreview();
    } else if (type == "start") {
        const int camera = m["camera"].integer(-1);
        if (camera >= 0 && camera < config_.cameras) {
            StartCamera(camera, m["source"], m["options"]);
        }
    } else if (type == "stop") {
        const int camera = m["camera"].integer(-1);
        if (camera >= 0 && camera < win::kMaxCameras) {
            StopCamera(camera);
        }
    } else if (type == "refresh") {
        SendLists();
    } else if (type == "pick") {
        const std::string kind = m["kind"].string();
        const std::wstring path = PickFile(window_, kind);
        if (path.empty()) {
            return;
        }
        if (kind == "placeholder") {
            Busy("Saving the picture");
            worker_.Post([this, path] {
                std::string error;
                const bool ok = app::SetPlaceholderImage(path, &error);
                PostToUi([this, ok, error] {
                    Busy("");
                    if (!ok) {
                        Toast("error", elevated_ ? error : "Changing the placeholder needs administrator rights.");
                        return;
                    }
                    Json thumb = Json::Object();
                    thumb["type"] = "placeholderThumb";
                    thumb["thumb"] = PlaceholderThumbnail(256, 144);
                    Send(thumb);
                    for (auto& camera : cameras_) {
                        camera.placeholder_key.clear();
                    }
                    Toast("info", "Apps show the new picture the next time they open VCam.");
                });
            });
            return;
        }
        Json file = Json::Object();
        file["type"] = "file";
        file["kind"] = kind;
        file["id"] = m["id"].integer();
        file["path"] = path;
        file["name"] = path.substr(path.find_last_of(L"\\/") + 1);
        Send(file);
        if (kind == "image") {
            SendThumb(path);
        }
    } else if (type == "saveState") {
        SaveUiState(m["state"].Dump());
    } else if (type == "install") {
        Install(m["name"].wstring());
    } else if (type == "saveSettings") {
        SaveSettings(m);
    } else if (type == "resetPlaceholder") {
        std::string error;
        if (!app::ResetPlaceholderImage(&error)) {
            Toast("error", elevated_ ? error : "Changing the placeholder needs administrator rights.");
            return;
        }
        Json thumb = Json::Object();
        thumb["type"] = "placeholderThumb";
        thumb["thumb"] = PlaceholderThumbnail(256, 144);
        Send(thumb);
        for (auto& camera : cameras_) {
            camera.placeholder_key.clear();
        }
    } else if (type == "openLogs") {
        const std::wstring logs = win::DataDirectory() + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        ShellExecuteW(window_, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (type == "restartService") {
        RestartService();
    } else if (type == "repair") {
        Repair();
    } else if (type == "uninstall") {
        Uninstall();
    }
}

Json App::InitMessage()
{
    Json m = Json::Object();
    m["type"] = "init";
    m["version"] = VCAM_VERSION_STRING;
    m["installed"] = !config_.install_dir.empty();
    m["elevated"] = elevated_;
    m["mica"] = mica_;
    std::string light, dark;
    AccentColors(&light, &dark);
    if (!light.empty()) {
        Json accent = Json::Object();
        accent["light"] = light;
        accent["dark"] = dark;
        m["accent"] = accent;
    }
    const bool mf = config_.install_dir.empty() ? app::VirtualCamerasSupported()
                                                : config_.backend == win::Backend::MediaFoundation;
    m["backend"] = mf ? "mediafoundation" : "directshow";
    m["mfSupported"] = app::VirtualCamerasSupported();
    Json config = Json::Object();
    config["cameras"] = config_.cameras;
    Json names = Json::Array();
    for (int i = 0; i < config_.cameras && i < win::kMaxCameras; i++) {
        names.push_back(config_.names[i]);
    }
    config["names"] = names;
    config["maxWidth"] = config_.max_width;
    config["maxHeight"] = config_.max_height;
    config["allow60"] = config_.allow_60fps;
    config["logging"] = config_.logging;
    config["installDir"] = config_.install_dir;
    config["customPlaceholder"] = !config_.placeholder.empty();
    m["config"] = config;
    Json settings = Json::Object();
    settings["autostart"] = AutostartEnabled();
    settings["tray"] = tray_;
    m["settings"] = settings;
    bool ok = false;
    const Json saved = Json::Parse(LoadUiState(), &ok);
    m["saved"] = ok ? saved : Json();
    return m;
}

Json App::StatusMessage()
{
    Json cameras = Json::Array();
    const int64_t now = win::NowTicks();
    const bool dshow = config_.backend == win::Backend::DirectShow;
    for (int i = 0; i < config_.cameras && i < win::kMaxCameras; i++) {
        CameraState& camera = cameras_[i];
        app::RunnerStats stats;
        bool has_runner = false;
        {
            std::lock_guard<std::mutex> lock(runners_lock_);
            if (camera.runner) {
                stats = camera.runner->Stats();
                has_runner = true;
            }
        }
        // A source that finished on its own (end of a video, closed window, error).
        if (has_runner && !stats.running && stats.ended) {
            camera.ended = true;
            camera.error = stats.error.empty() ? "The source ended." : stats.error;
            StopCamera(i);
            has_runner = false;
        }
        if (!camera.ring.is_open() && now >= camera.next_open) {
            camera.next_open = now + 2 * shm::kTicksPerSecond;
            camera.ring.Open(i, false, 0, dshow);
        }

        Json s = Json::Object();
        s["index"] = i;
        s["running"] = has_runner && stats.running;
        s["ended"] = camera.ended;
        s["error"] = camera.error;
        s["fps"] = stats.fps;
        s["ms"] = stats.ms_per_frame;
        s["frames"] = stats.frames;
        Json consumer = Json::Object();
        Json producer = Json::Object();
        producer["external"] = false;
        uint64_t delivered = stats.camera.frames_delivered;
        if (camera.ring.is_open()) {
            const shm::Header* h = camera.ring.ring().header();
            const bool active = ConsumerStreaming(h, now);
            consumer["active"] = active;
            shm::FrameRequest r;
            if (active && camera.ring.ring().ReadRequest(&r) && IsValidFormat(r.format)) {
                consumer["width"] = r.width;
                consumer["height"] = r.height;
                consumer["format"] = FormatName(static_cast<PixelFormat>(r.format));
                consumer["fps"] = r.fps_den ? static_cast<double>(r.fps_num) / r.fps_den : 0.0;
            }
            // With DirectShow the consumer is the application itself; Frame
            // Server hides which app it is.
            if (active && dshow) {
                const std::string app_name = ProcessName(AtomicLoad(h->consumer_pid));
                consumer["app"] = app_name.empty() ? Json() : Json(app_name);
            }
            const uint32_t pid = AtomicLoad(h->producer_pid);
            if (!has_runner && ProducerAlive(h, now) && pid != GetCurrentProcessId()) {
                producer["external"] = true;
                const std::string name = ProcessName(pid);
                producer["app"] = name.empty() ? Json() : Json(name);
            }
            delivered = AtomicLoad(h->frames_delivered, std::memory_order_relaxed);
        } else {
            consumer["active"] = stats.camera.consumer_active != 0;
            if (stats.camera.consumer_active) {
                consumer["width"] = stats.camera.width;
                consumer["height"] = stats.camera.height;
                consumer["format"] = FormatName(static_cast<PixelFormat>(stats.camera.format));
                consumer["fps"] = stats.camera.fps_denominator
                                      ? static_cast<double>(stats.camera.fps_numerator) / stats.camera.fps_denominator
                                      : 0.0;
            }
        }
        s["consumer"] = consumer;
        s["producer"] = producer;
        s["delivered"] = delivered;
        cameras.push_back(s);
    }
    Json m = Json::Object();
    m["type"] = "status";
    m["cameras"] = cameras;
    return m;
}

void App::SendLists()
{
    const HWND own = window_;
    worker_.Post([this, own] {
        Json monitors = Json::Array();
        for (const app::MonitorInfo& mi : app::EnumerateMonitors()) {
            Json j = Json::Object();
            j["number"] = mi.number;
            j["x"] = static_cast<int>(mi.rect.left);
            j["y"] = static_cast<int>(mi.rect.top);
            j["width"] = static_cast<int>(mi.rect.right - mi.rect.left);
            j["height"] = static_cast<int>(mi.rect.bottom - mi.rect.top);
            j["primary"] = mi.primary;
            monitors.push_back(j);
        }
        Json windows = Json::Array();
        for (const app::WindowInfo& w : app::EnumerateWindows()) {
            if (w.handle == own) {
                continue;
            }
            Json j = Json::Object();
            j["id"] = std::to_string(reinterpret_cast<uintptr_t>(w.handle));
            j["title"] = w.title;
            j["process"] = w.process;
            j["width"] = w.width;
            j["height"] = w.height;
            windows.push_back(j);
        }
        Json webcams = Json::Array();
        for (const app::CameraInfo& c : app::EnumerateCameras()) {
            if (!c.is_vcam) {
                Json j = Json::Object();
                j["name"] = c.name;
                webcams.push_back(j);
            }
        }
        Json m = Json::Object();
        m["type"] = "lists";
        m["monitors"] = monitors;
        m["windows"] = windows;
        m["webcams"] = webcams;
        PostToUi([this, m] { Send(m); });
    });
}

void App::SendThumb(const std::wstring& path)
{
    worker_.Post([this, path] {
        Json m = Json::Object();
        m["type"] = "thumb";
        m["path"] = path;
        m["name"] = path.substr(path.find_last_of(L"\\/") + 1);
        m["thumb"] = ImageThumbnail(path, 128, 72);
        PostToUi([this, m] { Send(m); });
    });
}

// --- cameras ----------------------------------------------------------------------------------

void App::StartCamera(int camera, const Json& source, const Json& options)
{
    const app::SourceOptions o = OptionsFromJson(camera, source, options);
    std::string error;
    app::SourceFactory factory = FactoryFromJson(source, o, &error);
    if (!factory) {
        Toast("warning", error);
        return;
    }
    cameras_[camera].ended = false;
    cameras_[camera].error.clear();
    const bool visible = IsWindowVisible(window_) && !IsIconic(window_);
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(runners_lock_);
        generation = ++cameras_[camera].generation;
    }
    worker_.Post([this, camera, o, factory, generation, visible] {
        std::unique_ptr<app::SourceRunner> old;
        {
            std::lock_guard<std::mutex> lock(runners_lock_);
            if (cameras_[camera].generation != generation) {
                return;  // superseded by a newer request
            }
            old = std::move(cameras_[camera].runner);
        }
        old.reset();  // only one producer per camera: stop the previous source first
        auto runner = std::make_unique<app::SourceRunner>(factory, o);
        runner->SetTap([this, camera, o](const app::Frame& frame, const app::Target&) {
            preview_.OnSourceFrame(camera, frame, o, window_);
        });
        runner->SetKeepAlive(visible);
        std::string error;
        if (!runner->Start(&error)) {
            PostToUi([this, camera, error] {
                cameras_[camera].ended = true;
                cameras_[camera].error = error;
                Toast("error", error);
            });
            return;
        }
        {
            std::lock_guard<std::mutex> lock(runners_lock_);
            if (cameras_[camera].generation == generation) {
                cameras_[camera].runner = std::move(runner);
            }
        }
        runner.reset();  // superseded while it started: stop it again
    });
    PostToUi([this] { UpdateKeepAlive(); });
}

void App::StopCamera(int camera)
{
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(runners_lock_);
        generation = ++cameras_[camera].generation;
    }
    cameras_[camera].placeholder_key.clear();
    worker_.Post([this, camera, generation] {
        std::unique_ptr<app::SourceRunner> old;
        {
            std::lock_guard<std::mutex> lock(runners_lock_);
            if (cameras_[camera].generation != generation) {
                return;
            }
            old = std::move(cameras_[camera].runner);
        }
    });
}

bool App::AnyRunning()
{
    std::lock_guard<std::mutex> lock(runners_lock_);
    for (const auto& camera : cameras_) {
        if (camera.runner) {
            return true;
        }
    }
    return false;
}

// Brings back the sources that ran when VCam last closed (sign-in start).
void App::RestoreRunningCameras()
{
    if (config_.install_dir.empty()) {
        return;
    }
    const Json state = Json::Parse(LoadUiState());
    for (size_t i = 0; i < state["cameras"].size() && static_cast<int>(i) < config_.cameras; i++) {
        const Json& c = state["cameras"][i];
        if (!c["running"].boolean()) {
            continue;
        }
        if (c["source"]["kind"].string() == "window") {
            // Windows do not survive a restart; the page offers to pick it again.
            cameras_[i].ended = true;
            cameras_[i].error = "Pick the window to share again.";
            continue;
        }
        StartCamera(static_cast<int>(i), c["source"], c["options"]);
    }
}

// --- preview -------------------------------------------------------------------------------------

void App::PumpPreview()
{
    if (!page_ready_ || !page_wants_frame_ || !shared_bytes_) {
        return;
    }
    const int camera = preview_.selected();
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!preview_.Take(camera, &rgba, &w, &h) || rgba.size() > kPreviewBytes) {
        return;
    }
    memcpy(shared_bytes_, rgba.data(), rgba.size());
    page_wants_frame_ = false;
    Json m = Json::Object();
    m["type"] = "frame";
    m["camera"] = camera;
    m["w"] = w;
    m["h"] = h;
    Send(m);
}

// Frames that do not come from a source this window runs: another producer's
// (read from the shared memory), or the placeholder apps see.
void App::PollPreview()
{
    if (!page_ready_ || !IsWindowVisible(window_)) {
        return;
    }
    const int i = preview_.selected();
    if (i < 0 || i >= config_.cameras) {
        return;
    }
    CameraState& camera = cameras_[i];
    {
        std::lock_guard<std::mutex> lock(runners_lock_);
        if (camera.runner) {
            return;  // the runner's frames feed the preview
        }
    }
    int w = 0, h = 0;
    preview_.Size(&w, &h);
    const int64_t now = win::NowTicks();
    if (camera.ring.is_open() && ProducerAlive(camera.ring.ring().header(), now)) {
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        static Converter converter;
        const Ring::ReadStatus status = camera.ring.ring().ReadLatest([&](const ImageView& view, const shm::SlotHeader& slot) {
            ConvertOptions o;
            o.input_colorspace = static_cast<ColorSpace>(slot.colorspace);
            converter.Convert(SlotContent(slot, view), MakeSpan(PixelFormat::RGBA, w, h, rgba.data()), o);
        });
        if (status == Ring::ReadStatus::Ok) {
            camera.placeholder_key.clear();
            preview_.SetFrame(i, std::move(rgba), w, h);
            PumpPreview();
        }
        return;
    }
    const std::string key = std::to_string(w) + "x" + std::to_string(h);
    if (camera.placeholder_key == key) {
        return;
    }
    camera.placeholder_key = key;
    std::vector<uint8_t> rgba;
    RenderPlaceholderRgba(w, h, &rgba);
    preview_.SetFrame(i, std::move(rgba), w, h);
    PumpPreview();
}

// --- installation and settings ------------------------------------------------------------

void App::Install(const std::wstring& name)
{
    if (!elevated_) {
        // Ask for administrator rights: start again elevated, which installs.
        const std::wstring exe = ExecutablePath();
        std::wstring quoted;
        for (const wchar_t c : name) {
            if (c != L'"') quoted += c;
        }
        const std::wstring params = L"--install \"" + quoted + L"\"";
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = exe.c_str();
        info.lpParameters = params.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info)) {
            Quit();
        } else {
            Json m = Json::Object();
            m["type"] = "welcomeNote";
            m["text"] = "Installing needs administrator permission.";
            Send(m);
        }
        return;
    }
    Busy("Installing VCam");
    worker_.Post([this, name] {
        app::InstallOptions options;
        options.cameras = 1;
        options.names.push_back(name.empty() ? std::wstring(L"VCam") : name);
        int code = 0;
        const std::string output = Collect([&] { return app::CmdInstall(options); }, &code);
        PostToUi([this, code, output] {
            Busy("");
            config_ = win::LoadConfig();
            if (code != 0) {
                Json m = Json::Object();
                m["type"] = "welcomeNote";
                m["text"] = "Installing failed: " + LastErrorLine(output);
                Send(m);
                if (!config_.install_dir.empty()) {
                    Send(InitMessage());  // installed, but a camera failed: show the app, with the error
                    Toast("error", LastErrorLine(output));
                }
                return;
            }
            Send(InitMessage());
            Toast("info", "VCam is installed. Pick it as the camera in your video app.");
        });
    });
}

void App::SaveSettings(const Json& m)
{
    if (!elevated_) {
        Toast("error", "Changing VCam's settings needs administrator rights. Start VCam as administrator.");
        return;
    }
    const bool tray = m["tray"].boolean(true);
    tray_ = tray;
    SaveTraySetting(tray);
    if (tray && !tray_icon_) {
        AddTrayIcon();
    } else if (!tray && tray_icon_ && IsWindowVisible(window_)) {
        RemoveTrayIcon();
    }
    Busy("Saving");
    const win::Config before = config_;
    worker_.Post([this, m, before] {
        std::vector<std::wstring> names;
        for (size_t i = 0; i < m["names"].size() && i < win::kMaxCameras; i++) {
            names.push_back(m["names"][i].wstring());
        }
        const int cameras = std::clamp(m["cameras"].integer(1), 1, win::kMaxCameras);
        bool cameras_changed = cameras != before.cameras;
        for (int i = 0; i < cameras && i < static_cast<int>(names.size()); i++) {
            cameras_changed |= i >= before.cameras || names[static_cast<size_t>(i)] != before.names[i];
        }
        std::string failure;
        const uint32_t max_w = static_cast<uint32_t>(std::clamp(m["maxWidth"].integer(1920), 320, 4096));
        const uint32_t max_h = static_cast<uint32_t>(std::clamp(m["maxHeight"].integer(1080), 180, 4096));
        if (cameras_changed) {
            // Registering cameras is what installing does; repeat it with the new list.
            app::InstallOptions options;
            options.cameras = cameras;
            options.names = names;
            options.max_width = max_w;
            options.max_height = max_h;
            options.allow_60fps = m["allow60"].boolean() ? 1 : 0;
            options.backend = static_cast<int>(before.backend);
            options.managed = before.managed;
            int code = 0;
            const std::string output = Collect([&] { return app::CmdInstall(options); }, &code);
            if (code != 0) {
                failure = LastErrorLine(output);
            }
        }
        win::Config config = win::LoadConfig();
        config.max_width = max_w;
        config.max_height = max_h;
        config.allow_60fps = m["allow60"].boolean();
        config.logging = m["logging"].boolean();
        if (!win::SaveConfig(config) && failure.empty()) {
            failure = "Saving the settings failed.";
        }
        std::string autostart_error;
        if (m["autostart"].boolean() != AutostartEnabled()) {
            SetAutostart(m["autostart"].boolean(), &autostart_error);
        }
        PostToUi([this, failure, autostart_error, cameras] {
            Busy("");
            config_ = win::LoadConfig();
            for (int i = cameras; i < win::kMaxCameras; i++) {
                StopCamera(i);
            }
            Send(InitMessage());
            if (!failure.empty()) {
                Toast("error", failure);
            } else if (!autostart_error.empty()) {
                Toast("error", autostart_error);
            } else {
                Toast("info", "Settings saved.");
            }
        });
    });
}

void App::Repair()
{
    if (!elevated_) {
        Toast("error", "Repairing needs administrator rights. Start VCam as administrator.");
        return;
    }
    Busy("Repairing VCam");
    const win::Config config = config_;
    worker_.Post([this, config] {
        app::InstallOptions options;
        options.cameras = std::max(config.cameras, 1);
        for (int i = 0; i < options.cameras; i++) {
            options.names.push_back(config.names[i]);
        }
        options.backend = static_cast<int>(config.backend);
        options.managed = config.managed;
        int code = 0;
        const std::string output = Collect([&] { return app::CmdInstall(options); }, &code);
        PostToUi([this, code, output] {
            Busy("");
            config_ = win::LoadConfig();
            Toast(code == 0 ? "info" : "error", code == 0 ? "VCam is repaired." : LastErrorLine(output));
        });
    });
}

void App::RestartService()
{
    if (!elevated_) {
        Toast("error", "Restarting the camera service needs administrator rights.");
        return;
    }
    Busy("Restarting the camera service");
    worker_.Post([this] {
        int code = 0;
        Collect([] { return app::CmdRestartFrameServer(); }, &code);
        PostToUi([this, code] {
            Busy("");
            Toast(code == 0 ? "info" : "error", code == 0 ? "The camera service restarts when an app opens a camera."
                                                          : "Restarting the camera service failed.");
        });
    });
}

void App::Uninstall()
{
    if (MessageBoxW(window_,
                    L"Remove VCam? Apps will no longer list its cameras. Your settings in ProgramData are kept.",
                    L"Uninstall VCam", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (config_.managed) {
        // The setup program installed VCam: its uninstaller removes everything.
        std::wstring command;
        win::RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VCam_is1",
                           L"UninstallString", &command);
        if (!command.empty()) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                Quit();
                return;
            }
        }
        Toast("error", "Uninstall VCam from Settings > Apps > Installed apps.");
        return;
    }
    if (!elevated_) {
        Toast("error", "Uninstalling needs administrator rights. Start VCam as administrator.");
        return;
    }
    for (int i = 0; i < win::kMaxCameras; i++) {
        StopCamera(i);
    }
    Busy("Removing VCam");
    worker_.Post([this] {
        int code = 0;
        const std::string output = Collect([] { return app::CmdUninstall(false); }, &code);
        PostToUi([this, code, output] {
            Busy("");
            MessageBoxW(window_, code == 0 ? L"VCam is uninstalled." : win::Widen(LastErrorLine(output)).c_str(),
                        L"VCam", code == 0 ? MB_ICONINFORMATION : MB_ICONERROR);
            Quit();
        });
    });
}

}  // namespace

int RunApp(HINSTANCE instance, const std::wstring& install_name, bool background)
{
    App app;
    return app.Run(instance, install_name, background);
}

}  // namespace vcam::gui
