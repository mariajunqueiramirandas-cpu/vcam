#include "app.h"

#include <cstdio>

#include "vcam/modes.h"

namespace vcam::app {

namespace {

const char kUsage[] =
    "vcam - a virtual camera for Windows 10 and 11\n"
    "\n"
    "Setup (administrator):\n"
    "  vcam install [--cameras N] [--name NAME]... [--max-resolution 1080p|1440p|4k|WxH]\n"
    "               [--no-60fps] [--no-path] [--backend mediafoundation|directshow]\n"
    "                            install vcam and register N cameras (default 1, up to 4);\n"
    "                            Windows 11 uses Media Foundation, Windows 10 DirectShow\n"
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
    "Applications see the cameras as \"VCam\". The VCam app (vcam-gui.exe) does all of this\n"
    "with a window; feed the cameras from your own code with vcam.dll / vcam.h or python/vcam.py.\n";

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
    // Sources are made on the runner's thread, which owns their COM apartment.
    SourceFactory factory;
    if (command == "bars") {
        factory = [options] { return MakeBarsSource(options); };
    } else if (command == "color" || command == "colour") {
        Rgb color;
        const std::string value = args.Next();
        if (value.empty() || !ParseColor(value, &color)) {
            PrintError("usage: vcam color #RRGGBB (or black, white, red, ...)\n");
            return 2;
        }
        factory = [color] { return MakeColorSource(color); };
    } else if (command == "image") {
        const std::string path = args.Next();
        if (path.empty()) {
            PrintError("usage: vcam image FILE\n");
            return 2;
        }
        factory = [path] { return MakeImageSource(Wide(path)); };
    } else if (command == "video") {
        const std::string path = args.Next();
        if (path.empty()) {
            PrintError("usage: vcam video FILE [--no-loop]\n");
            return 2;
        }
        const bool loop = options.loop;
        factory = [path, loop] { return MakeVideoSource(Wide(path), loop); };
    } else if (command == "webcam") {
        const std::string selector = args.Next();
        factory = [selector] { return MakeWebcamSource(selector); };
    } else if (command == "screen") {
        int monitor = 0;
        const std::string n = args.Next();
        if (!n.empty() && (!ParseInt(n, &monitor) || monitor < 1)) {
            PrintError("usage: vcam screen [N]  (see `vcam list` for monitor numbers)\n");
            return 2;
        }
        const bool cursor = options.cursor;
        factory = [monitor, cursor] { return MakeScreenSource(monitor, cursor); };
    } else if (command == "window") {
        const std::string title = args.Next();
        if (title.empty()) {
            PrintError("usage: vcam window TITLE  (see `vcam list` for window titles)\n");
            return 2;
        }
        const bool cursor = options.cursor;
        factory = [title, cursor] { return MakeWindowSource(title, cursor); };
    }
    if (!args.Finish()) {
        return 2;
    }
    return RunSource(std::move(factory), options);
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
        if (args.Value("backend", &v)) {
            if (v == "mediafoundation" || v == "mf") {
                options.backend = static_cast<int>(win::Backend::MediaFoundation);
            } else if (v == "directshow" || v == "dshow") {
                options.backend = static_cast<int>(win::Backend::DirectShow);
            } else {
                PrintError("--backend takes mediafoundation or directshow\n");
                return 2;
            }
        }
        options.managed = args.Flag("managed");
        if (!args.Finish()) {
            return 2;
        }
        return CmdInstall(options);
    }
    if (command == "uninstall") {
        const bool managed = args.Flag("managed");
        return args.Finish() ? CmdUninstall(managed) : 2;
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
