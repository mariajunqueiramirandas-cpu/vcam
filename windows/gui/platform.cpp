// Windows pieces of the GUI: theme and accent colour, Mica, file pickers,
// thumbnails, autostart and per-user settings.
#include "gui.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <cstdio>

#include "vcam/draw.h"

using Microsoft::WRL::ComPtr;

namespace vcam::gui {

namespace {

constexpr wchar_t kGuiKey[] = L"Software\\vcam\\gui";
constexpr wchar_t kTaskName[] = L"vcam-gui";

std::string Hex(winrt::Windows::UI::Color c)
{
    char text[8];
    snprintf(text, sizeof(text), "#%02x%02x%02x", c.R, c.G, c.B);
    return text;
}

DWORD WindowsBuild()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn && fn(&info) == 0 ? info.dwBuildNumber : 0;
}

int RunHidden(const std::wstring& command_line)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = command_line;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return -1;
    }
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Converts BGRA to RGBA (the canvas order) in place.
void SwapRedBlue(std::vector<uint8_t>* pixels)
{
    for (size_t i = 0; i + 3 < pixels->size(); i += 4) {
        std::swap((*pixels)[i], (*pixels)[i + 2]);
    }
}

Json ThumbJson(const std::vector<uint8_t>& rgba, int width, int height)
{
    Json thumb = Json::Object();
    thumb["w"] = width;
    thumb["h"] = height;
    thumb["data"] = Base64(rgba.data(), rgba.size());
    return thumb;
}

}  // namespace

// --- theme -----------------------------------------------------------------------

bool SystemUsesDarkTheme()
{
    DWORD light = 1;
    win::RegReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"AppsUseLightTheme", &light);
    return light == 0;
}

void AccentColors(std::string* for_light, std::string* for_dark)
{
    for_light->clear();
    for_dark->clear();
    try {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        winrt::Windows::UI::ViewManagement::UISettings settings;
        // What Windows 11 itself uses for accent-coloured controls.
        *for_light = Hex(settings.GetColorValue(UIColorType::AccentDark1));
        *for_dark = Hex(settings.GetColorValue(UIColorType::AccentLight2));
    } catch (const winrt::hresult_error&) {
    }
}

bool MicaSupported()
{
    return WindowsBuild() >= 22621;  // DWMWA_SYSTEMBACKDROP_TYPE
}

void ApplyWindowTheme(HWND window, bool mica)
{
    const BOOL dark = SystemUsesDarkTheme() ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (mica) {
        const int backdrop = 2;  // DWMSBT_MAINWINDOW
        DwmSetWindowAttribute(window, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop));
        // The backdrop shows through the whole client area, under the transparent page.
        const MARGINS margins = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(window, &margins);
    }
}

// --- files -----------------------------------------------------------------------

std::wstring PickFile(HWND owner, const std::string& kind)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {};
    }
    static const COMDLG_FILTERSPEC images[] = {
        {L"Pictures", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.heic;*.heif;*.jxr"},
        {L"All files", L"*.*"},
    };
    static const COMDLG_FILTERSPEC videos[] = {
        {L"Videos", L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.mpg;*.mpeg;*.ts;*.gif"},
        {L"All files", L"*.*"},
    };
    if (kind == "video") {
        dialog->SetFileTypes(ARRAYSIZE(videos), videos);
        dialog->SetTitle(L"Choose a video");
    } else {
        dialog->SetFileTypes(ARRAYSIZE(images), images);
        dialog->SetTitle(kind == "placeholder" ? L"Choose the picture apps see when nothing plays" : L"Choose a picture");
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dialog->Show(owner))) {
        return {};
    }
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return {};
    }
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

Json ImageThumbnail(const std::wstring& path, int width, int height)
{
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    std::string error;
    if (!app::LoadImageBgra(path, &pixels, &w, &h, &error)) {
        return Json();
    }
    std::vector<uint8_t> thumb(static_cast<size_t>(width) * height * 4);
    Converter converter;
    ConvertOptions options;
    options.scale = ScaleMode::Fill;
    converter.Convert(MakeView(PixelFormat::BGRA, w, h, pixels.data()),
                      MakeSpan(PixelFormat::RGBA, width, height, thumb.data()), options);
    return ThumbJson(thumb, width, height);
}

void RenderPlaceholderRgba(int width, int height, std::vector<uint8_t>* rgba)
{
    rgba->assign(static_cast<size_t>(width) * height * 4, 0);
    const win::Config config = win::LoadConfig();
    std::string image;
    uint32_t iw = 0, ih = 0;
    Converter converter;
    if (!config.placeholder.empty() && win::LoadPlaceholderImage(config.placeholder, &image, &iw, &ih)) {
        converter.Convert(MakeView(PixelFormat::BGRA, static_cast<int>(iw), static_cast<int>(ih),
                                   reinterpret_cast<const uint8_t*>(image.data())),
                          MakeSpan(PixelFormat::RGBA, width, height, rgba->data()), ConvertOptions{});
        return;
    }
    // Same picture the cameras render (feed.cpp), in canvas byte order.
    RenderPlaceholder(MakeSpan(PixelFormat::BGRA, width, height, rgba->data()), "No video source",
                      "Open VCam and pick a source");
    SwapRedBlue(rgba);
}

Json PlaceholderThumbnail(int width, int height)
{
    std::vector<uint8_t> rgba;
    RenderPlaceholderRgba(width, height, &rgba);
    return ThumbJson(rgba, width, height);
}

std::string ProcessName(DWORD pid)
{
    if (!pid) {
        return {};
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const std::wstring p(path);
        name = win::Narrow(p.substr(p.find_last_of(L'\\') + 1));
    }
    CloseHandle(process);
    return name;
}

std::wstring ExecutablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// --- autostart -------------------------------------------------------------------

bool AutostartEnabled()
{
    return RunHidden(std::wstring(L"schtasks.exe /Query /TN ") + kTaskName) == 0;
}

bool SetAutostart(bool enable, std::string* error)
{
    if (!enable) {
        RunHidden(std::wstring(L"schtasks.exe /Delete /F /TN ") + kTaskName);
        return true;
    }
    // Highest privileges: no UAC prompt at sign-in, and the cameras' shared
    // memory can be created before any app opens them.
    const std::wstring command = std::wstring(L"schtasks.exe /Create /F /TN ") + kTaskName +
                                 L" /SC ONLOGON /RL HIGHEST /TR \"\\\"" + ExecutablePath() + L"\\\" --background\"";
    if (RunHidden(command) != 0) {
        *error = "creating the sign-in task failed (it needs administrator rights)";
        return false;
    }
    return true;
}

// --- per-user settings ---------------------------------------------------------------

std::string LoadUiState()
{
    std::wstring value;
    return win::RegReadString(HKEY_CURRENT_USER, kGuiKey, L"State", &value) ? win::Narrow(value) : std::string();
}

void SaveUiState(const std::string& json)
{
    win::RegWriteString(HKEY_CURRENT_USER, kGuiKey, L"State", win::Widen(json));
}

bool LoadTraySetting()
{
    DWORD value = 1;
    win::RegReadDword(HKEY_CURRENT_USER, kGuiKey, L"Tray", &value);
    return value != 0;
}

void SaveTraySetting(bool tray)
{
    win::RegWriteDword(HKEY_CURRENT_USER, kGuiKey, L"Tray", tray ? 1 : 0);
}

bool LoadPlacement(WINDOWPLACEMENT* placement)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kGuiKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, size = sizeof(*placement);
    const bool ok = RegQueryValueExW(key, L"Placement", nullptr, &type, reinterpret_cast<BYTE*>(placement), &size) ==
                        ERROR_SUCCESS &&
                    type == REG_BINARY && size == sizeof(*placement) && placement->length == sizeof(*placement);
    RegCloseKey(key);
    return ok;
}

void SavePlacement(const WINDOWPLACEMENT& placement)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kGuiKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"Placement", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&placement), sizeof(placement));
        RegCloseKey(key);
    }
}

// --- worker ------------------------------------------------------------------------

Worker::Worker() : thread_(&Worker::Run, this) {}

Worker::~Worker()
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Worker::Post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> lock(lock_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Worker::Run()
{
    SetThreadDescription(GetCurrentThread(), L"vcam-gui worker");
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(lock_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                break;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
    if (com) {
        CoUninitialize();
    }
}

// --- preview ------------------------------------------------------------------------

void Preview::Select(int camera)
{
    selected_ = camera;
    std::lock_guard<std::mutex> lock(lock_);
    fresh_ = false;
}

void Preview::SetSize(int width, int height)
{
    std::lock_guard<std::mutex> lock(lock_);
    width_ = std::clamp(width, 2, 1920);
    height_ = std::clamp(height, 2, 1080);
}

void Preview::Size(int* width, int* height)
{
    std::lock_guard<std::mutex> lock(lock_);
    *width = width_;
    *height = height_;
}

void Preview::OnSourceFrame(int camera, const app::Frame& frame, const app::SourceOptions& options, HWND notify)
{
    if (camera != selected_ || camera < 0 || camera >= win::kMaxCameras) {
        return;
    }
    const int64_t now = win::NowTicks();
    if (now - last_[camera] < shm::kTicksPerSecond / 31) {
        return;
    }
    last_[camera] = now;
    int w = 0, h = 0;
    Size(&w, &h);
    ImageView view;
    view.format = frame.format;
    view.width = frame.width;
    view.height = frame.height;
    for (int i = 0; i < 3; i++) {
        view.plane[i] = frame.planes[i];
        view.pitch[i] = frame.pitches[i];
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    ConvertOptions o;
    o.scale = options.scale;
    o.mirror = options.mirror;
    o.flip = options.flip;
    o.background = options.background;
    o.input_colorspace = frame.colorspace;
    if (!converters_[camera].Convert(view, MakeSpan(PixelFormat::RGBA, w, h, rgba.data()), o)) {
        return;
    }
    bool notify_now = false;
    {
        std::lock_guard<std::mutex> lock(lock_);
        pending_ = std::move(rgba);
        pending_camera_ = camera;
        pending_w_ = w;
        pending_h_ = h;
        fresh_ = true;
        notify_now = !notified_;
        notified_ = true;
    }
    if (notify_now) {
        PostMessageW(notify, WM_APP_FRAME, 0, 0);
    }
}

void Preview::SetFrame(int camera, std::vector<uint8_t> rgba, int width, int height)
{
    std::lock_guard<std::mutex> lock(lock_);
    pending_ = std::move(rgba);
    pending_camera_ = camera;
    pending_w_ = width;
    pending_h_ = height;
    fresh_ = true;
}

bool Preview::Take(int camera, std::vector<uint8_t>* rgba, int* width, int* height)
{
    std::lock_guard<std::mutex> lock(lock_);
    notified_ = false;
    if (!fresh_ || pending_camera_ != camera) {
        return false;
    }
    fresh_ = false;
    rgba->swap(pending_);
    *width = pending_w_;
    *height = pending_h_;
    return true;
}

}  // namespace vcam::gui
