// Shared helpers of the vcam tool library: output, errors, elevation and
// command-line parsing.
#include "app.h"

#include <cstdarg>
#include <cstdio>
#include <cwctype>

namespace vcam::app {

std::atomic<bool> g_stop{false};

namespace {

OutputSink g_sink = nullptr;

void VPrint(bool error, const char* format, va_list args)
{
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    if (g_sink) {
        g_sink(error, buffer);
        return;
    }
    FILE* stream = error ? stderr : stdout;
    if (error) {
        fputs("vcam: ", stream);
    }
    fputs(buffer, stream);
    fflush(stream);
}

BOOL WINAPI OnConsoleEvent(DWORD)
{
    g_stop = true;
    return TRUE;  // handled: let the main loop shut down cleanly
}

}  // namespace

void SetOutputSink(OutputSink sink)
{
    g_sink = sink;
}

void Print(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    VPrint(false, format, args);
    va_end(args);
}

void PrintError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    VPrint(true, format, args);
    va_end(args);
}

bool Source::stopping() const
{
    return g_stop || (stop_flag && *stop_flag);
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

}  // namespace vcam::app
