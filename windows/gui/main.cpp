// VCam's entry point: one instance per session; a second launch shows the first.
#include "gui.h"

#include <objbase.h>
#include <shellapi.h>

namespace vcam::gui {
int RunApp(HINSTANCE instance, const std::wstring& install_name, bool background);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    bool background = false;
    std::wstring install_name;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            const std::wstring arg = argv[i];
            if (arg == L"--background") {
                background = true;  // started at sign-in: notification area only
            } else if (arg == L"--install" && i + 1 < argc) {
                install_name = argv[++i];  // restarted elevated to install
            }
        }
        LocalFree(argv);
    }

    // The instance that holds the mutex owns the cameras. An elevated restart
    // waits for the instance that started it to close.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\VCam.Gui");
    const DWORD wait = mutex ? WaitForSingleObject(mutex, install_name.empty() ? 0 : 10000) : WAIT_FAILED;
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        if (!background) {
            if (HWND other = FindWindowW(L"VCamWindow", nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, RegisterWindowMessageW(L"VCam.Show"), 0, 0);
            }
        }
        if (mutex) {
            CloseHandle(mutex);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const int code = vcam::gui::RunApp(instance, install_name, background);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
