#include "app.h"

#include <cstdarg>
#include <cstdio>
#include <cwctype>

#include "vcam/modes.h"

namespace vcam::app {

std::atomic<bool> g_stop{false};

namespace {

void VPrint(FILE* stream, const char* format, va_list args)
{
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    fputs(buffer, stream);
    fflush(stream);
}

BOOL WINAPI OnConsoleEvent(DWORD)
{
    g_stop = true;
    return TRUE;  // handled: let the main loop shut down cleanly
}

}  // namespace

void Print(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    VPrint(stdout, format, args);
    va_end(args);
}

void PrintError(const char* format, ...)
{
    fputs("vcam: ", stderr);
    va_list args;
    va_start(args, format);
    VPrint(stderr, format, args);
    va_end(args);
}

std::string Utf8(const std::wstring& text)
{
    return win::Narrow(text);
}

std::wstring Wide(const std::string& text)
{
    return win::Widen(text);
}

std::string ErrorText(DWORD error)
{
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = message ? Utf8(message) : std::string();
    if (message) {
        LocalFree(message);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    char code[32];
    snprintf(code, sizeof(code), " (error %lu)", error);
    return (text.empty() ? std::string("unknown error") : text) + code;
}

std::string HresultText(HRESULT hr)
{
    char code[32];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = message ? Utf8(message) : std::string();
    if (message) {
        LocalFree(message);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text.empty() ? std::string(code) : text + " (" + code + ")";
}

void InstallStopHandler()
{
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool RequireElevation(const char* what)
{
    if (IsElevated()) {
        return true;
    }
    PrintError("%s needs administrator rights. Run it from an administrator terminal, or sign in with an "
               "administrator account (vcam then asks for elevation itself).\n",
               what);
    return false;
}

// --- command line ---------------------------------------------------------------------

Args::Args(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; i++) {
        items_.push_back(Utf8(argv[i]));
    }
    used_.assign(items_.size(), false);
}

std::string Args::Next()
{
    for (size_t i = 0; i < items_.size(); i++) {
        if (!used_[i] && (items_[i].rfind("--", 0) != 0 || items_[i] == "--")) {
            used_[i] = true;
            return items_[i];
        }
    }
    return {};
}

bool Args::Flag(const char* name)
{
    const std::string flag = std::string("--") + name;
    for (size_t i = 0; i < items_.size(); i++) {
        if (!used_[i] && items_[i] == flag) {
            used_[i] = true;
            return true;
        }
    }
    return false;
}

bool Args::Value(const char* name, std::string* value)
{
    const std::string flag = std::string("--") + name;
    for (size_t i = 0; i < items_.size(); i++) {
        if (used_[i]) {
            continue;
        }
        if (items_[i] == flag && i + 1 < items_.size()) {
            used_[i] = used_[i + 1] = true;
            *value = items_[i + 1];
            return true;
        }
        if (items_[i].rfind(flag + "=", 0) == 0) {
            used_[i] = true;
            *value = items_[i].substr(flag.size() + 1);
            return true;
        }
    }
    return false;
}

bool Args::Finish()
{
    bool ok = true;
    for (size_t i = 0; i < items_.size(); i++) {
        if (!used_[i]) {
            PrintError("unexpected argument '%s' (see `vcam help`)\n", items_[i].c_str());
            ok = false;
        }
    }
    return ok;
}

bool ParseInt(const std::string& text, int* value)
{
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long v = strtol(text.c_str(), &end, 10);
    if (*end != '\0' || v < -1000000 || v > 1000000) {
        return false;
    }
    *value = static_cast<int>(v);
    return true;
}

bool ParseColor(const std::string& text, Rgb* color)
{
    struct Named {
        const char* name;
        uint32_t rgb;
    };
    static const Named named[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"gray", 0x808080},  {"grey", 0x808080},
        {"red", 0xFF0000},   {"green", 0x00FF00}, {"blue", 0x0000FF},  {"yellow", 0xFFFF00},
        {"cyan", 0x00FFFF},  {"magenta", 0xFF00FF}, {"orange", 0xFF8000}, {"purple", 0x800080},
    };
    std::string lower;
    for (char c : text) {
        lower += static_cast<char>(towlower(static_cast<unsigned char>(c)));
    }
    uint32_t rgb = 0;
    bool found = false;
    for (const Named& n : named) {
        if (lower == n.name) {
            rgb = n.rgb;
            found = true;
        }
    }
    if (!found) {
        std::string hex = lower;
        if (!hex.empty() && hex[0] == '#') {
            hex = hex.substr(1);
        } else if (hex.rfind("0x", 0) == 0) {
            hex = hex.substr(2);
        }
        if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) {
            return false;
        }
        rgb = static_cast<uint32_t>(strtoul(hex.c_str(), nullptr, 16));
    }
    color->r = static_cast<uint8_t>(rgb >> 16);
    color->g = static_cast<uint8_t>(rgb >> 8);
    color->b = static_cast<uint8_t>(rgb);
    return true;
}

namespace {

const char kUsage[] =
    "vcam - a virtual camera for Windows 11\n"
    "\n"
    "Setup (administrator):\n"
    "  vcam install [--cameras N] [--name NAME]... [--max-resolution 1080p|1440p|4k|WxH]\n"
    "               [--no-60fps] [--no-path]\n"
    "                            install vcam and register N cameras (default 1, up to 4)\n"
    "  vcam uninstall            remove the cameras and vcam\n"
    "  vcam config [--name [N=]NAME] [--max-resolution R] [--60fps on|off] [--logging on|off]\n"
    "  vcam placeholder FILE | --reset\n"
    "                            picture shown while nothing feeds a camera\n"
    "  vcam autostart SOURCE [ARGS...] | --off\n"
    "                            start a source whenever you sign in, without prompts\n"
    "  vcam restart-frame-server apply configuration changes immediately\n"
    "\n"
    "Information:\n"
    "  vcam status               installation, cameras, and what is streaming\n"
    "  vcam list                 monitors, windows and cameras you can use as sources\n"
    "\n"
    "Sources (stream until Ctrl+C):\n"
    "  vcam bars                 colour bars with a moving marker and a clock\n"
    "  vcam color COLOR          solid colour: #RRGGBB or black, white, red, green, blue, ...\n"
    "  vcam image FILE           a picture (PNG, JPEG, BMP, GIF, TIFF, WebP, HEIC...)\n"
    "  vcam video FILE           a video file, looped (--no-loop: stop at the end)\n"
    "  vcam webcam [NAME|N]      a real camera, passed through (with --mirror, cropping...)\n"
    "  vcam screen [N]           monitor N (default: the primary monitor)\n"
    "  vcam window TITLE         the first window whose title contains TITLE\n"
    "\n"
    "Source options:\n"
    "  --camera N                which vcam camera to feed (default 1)\n"
    "  --fit | --fill | --stretch  when the aspect ratio differs: pad (default), crop, stretch\n"
    "  --mirror, --flip          flip left/right, upside down\n"
    "  --background COLOR        padding colour for --fit (default black)\n"
    "  --fps N                   frame rate to produce (default: the rate apps ask for)\n"
    "  --size WxH                render size of bars/color (default: the size apps ask for)\n"
    "  --no-cursor               screen/window: hide the mouse pointer\n"
    "  --always                  keep producing while no app watches (default: idle)\n"
    "  --quiet                   no live status line\n"
    "\n"
    "Applications see the cameras as \"VCam\" (Windows adds \"Windows Virtual Camera\").\n"
    "Feed them from your own code with vcam.dll / vcam.h or python/vcam.py.\n";

bool ParseSourceOptions(Args& args, SourceOptions* o)
{
    std::string v;
    if (args.Value("camera", &v) && (!ParseInt(v, &o->camera) || o->camera < 1 || o->camera > win::kMaxCameras)) {
        PrintError("--camera takes a number from 1 to %d\n", win::kMaxCameras);
        return false;
    }
    if (args.Flag("fill")) {
        o->scale = ScaleMode::Fill;
    }
    if (args.Flag("stretch")) {
        o->scale = ScaleMode::Stretch;
    }
    if (args.Flag("fit")) {
        o->scale = ScaleMode::Fit;
    }
    o->mirror = args.Flag("mirror");
    o->flip = args.Flag("flip");
    if (args.Value("background", &v) && !ParseColor(v, &o->background)) {
        PrintError("bad colour '%s' (use #RRGGBB or a name)\n", v.c_str());
        return false;
    }
    if (args.Value("fps", &v)) {
        o->fps = atof(v.c_str());
        if (o->fps < 1 || o->fps > 240) {
            PrintError("--fps takes a number from 1 to 240\n");
            return false;
        }
    }
    if (args.Value("size", &v)) {
        uint32_t w = 0, h = 0;
        if (!ParseResolution(v.c_str(), &w, &h)) {
            PrintError("bad size '%s' (use WxH, e.g. 1280x720)\n", v.c_str());
            return false;
        }
        o->width = static_cast<int>(w);
        o->height = static_cast<int>(h);
    }
    o->cursor = !args.Flag("no-cursor");
    o->loop = !args.Flag("no-loop");
    o->always = args.Flag("always");
    o->quiet = args.Flag("quiet");
    if (args.Flag("hidden")) {
        // Started at sign-in by `vcam autostart`: no console window.
        o->quiet = true;
        FreeConsole();
    }
    return true;
}

int RunSourceCommand(const std::string& command, Args& args)
{
    SourceOptions options;
    if (!ParseSourceOptions(args, &options)) {
        return 2;
    }
    std::unique_ptr<Source> source;
    if (command == "bars") {
        source = MakeBarsSource(options);
    } else if (command == "color" || command == "colour") {
        Rgb color;
        const std::string value = args.Next();
        if (value.empty() || !ParseColor(value, &color)) {
            PrintError("usage: vcam color #RRGGBB (or black, white, red, ...)\n");
            return 2;
        }
        source = MakeColorSource(color);
    } else if (command == "image") {
        const std::string path = args.Next();
        if (path.empty()) {
            PrintError("usage: vcam image FILE\n");
            return 2;
        }
        source = MakeImageSource(Wide(path));
    } else if (command == "video") {
        const std::string path = args.Next();
        if (path.empty()) {
            PrintError("usage: vcam video FILE [--no-loop]\n");
            return 2;
        }
        source = MakeVideoSource(Wide(path), options.loop);
    } else if (command == "webcam") {
        source = MakeWebcamSource(args.Next());
    } else if (command == "screen") {
        int monitor = 0;
        const std::string n = args.Next();
        if (!n.empty() && (!ParseInt(n, &monitor) || monitor < 1)) {
            PrintError("usage: vcam screen [N]  (see `vcam list` for monitor numbers)\n");
            return 2;
        }
        source = MakeScreenSource(monitor, options.cursor);
    } else if (command == "window") {
        const std::string title = args.Next();
        if (title.empty()) {
            PrintError("usage: vcam window TITLE  (see `vcam list` for window titles)\n");
            return 2;
        }
        source = MakeWindowSource(title, options.cursor);
    }
    if (!args.Finish()) {
        return 2;
    }
    ConfigureCapture(*source, options.scale, options.background);
    return RunSource(*source, options);
}

int Dispatch(int argc, wchar_t** argv)
{
    Args args(argc, argv);
    const std::string command = args.Next();
    if (command.empty() || command == "help" || args.Flag("help") || command == "-h" || command == "/?") {
        Print("%s", kUsage);
        return command.empty() ? 2 : 0;
    }
    if (command == "version" || command == "--version") {
        Print("vcam %s (API %d)\n", VCAM_VERSION_STRING, vcam_api_version());
        return 0;
    }
    if (command == "install") {
        InstallOptions options;
        std::string v;
        if (args.Value("cameras", &v) && (!ParseInt(v, &options.cameras) || options.cameras < 1 ||
                                          options.cameras > win::kMaxCameras)) {
            PrintError("--cameras takes a number from 1 to %d\n", win::kMaxCameras);
            return 2;
        }
        while (args.Value("name", &v)) {
            options.names.push_back(Wide(v));
        }
        if (args.Value("max-resolution", &v) && !ParseResolution(v.c_str(), &options.max_width, &options.max_height)) {
            PrintError("bad resolution '%s' (1080p, 1440p, 4k or WxH)\n", v.c_str());
            return 2;
        }
        if (args.Flag("no-60fps")) {
            options.allow_60fps = 0;
        }
        options.add_to_path = !args.Flag("no-path");
        if (!args.Finish()) {
            return 2;
        }
        return CmdInstall(options);
    }
    if (command == "uninstall") {
        return args.Finish() ? CmdUninstall() : 2;
    }
    if (command == "status") {
        return args.Finish() ? CmdStatus() : 2;
    }
    if (command == "list") {
        return args.Finish() ? CmdList() : 2;
    }
    if (command == "config") {
        return CmdConfig(args);
    }
    if (command == "placeholder") {
        return CmdPlaceholder(args);
    }
    if (command == "restart-frame-server") {
        return args.Finish() ? CmdRestartFrameServer() : 2;
    }
    if (command == "autostart") {
        return CmdAutostart(args, argc, argv);
    }
    static const char* const sources[] = {"bars", "color", "colour", "image", "video", "webcam", "screen", "window"};
    for (const char* s : sources) {
        if (command == s) {
            return RunSourceCommand(command, args);
        }
    }
    PrintError("unknown command '%s' (see `vcam help`)\n", command.c_str());
    return 2;
}

// Elevation from a non-elevated terminal opens a console window of our own,
// which would vanish with the output. Keep it open until a key is pressed.
void HoldOwnConsole(int code)
{
    DWORD processes[4];
    if (GetConsoleProcessList(processes, 4) != 1 || !GetConsoleWindow() || !IsWindowVisible(GetConsoleWindow())) {
        return;
    }
    Print(code == 0 ? "\nPress Enter to close this window.\n" : "\nFailed. Press Enter to close this window.\n");
    getchar();
}

}  // namespace
}  // namespace vcam::app

int wmain(int argc, wchar_t** argv)
{
    using namespace vcam::app;
    SetConsoleOutputCP(CP_UTF8);
    InstallStopHandler();
    const int code = Dispatch(argc, argv);
    HoldOwnConsole(code);
    return code;
}
