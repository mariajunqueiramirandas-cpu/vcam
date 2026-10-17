/*
 * vcamctl - installs or removes the vcam virtual webcam driver.
 *
 *   vcamctl install [path\to\vcam.inf]   create the ROOT\VCAM device and install the driver on it
 *   vcamctl uninstall                    remove the device and delete the driver package
 *
 * This is what `devcon install` does for a root-enumerated device. Run it elevated.
 */
#define WIN32_LEAN_AND_MEAN
// DiUninstallDriver is part of the Windows 10 API set.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <stdio.h>
#include <wchar.h>

#define HARDWARE_ID L"ROOT\\VCAM"

// GUID_DEVCLASS_CAMERA
static const GUID CameraClass = { 0xca3e7ab9, 0xb4c3, 0x4ae6, { 0x82, 0x51, 0x57, 0x9e, 0xf9, 0x33, 0x89, 0x0f } };

// DEVPKEY_Device_DriverInfPath: the oemNN.inf the device's driver was installed from.
static const DEVPROPKEY InfPathKey = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 3 };

static void PrintError(const wchar_t *what)
{
    fwprintf(stderr, L"%ls failed (error 0x%08lX).\n", what, GetLastError());
}

// Advances *Index to the next Camera-class device whose hardware ID is ROOT\VCAM.
static BOOL NextVCamDevice(HDEVINFO Set, DWORD *Index, SP_DEVINFO_DATA *Dev)
{
    Dev->cbSize = sizeof(*Dev);
    while (SetupDiEnumDeviceInfo(Set, (*Index)++, Dev)) {
        wchar_t ids[512] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(Set, Dev, SPDRP_HARDWAREID, NULL, (BYTE *)ids,
                                               sizeof(ids) - 2 * sizeof(wchar_t), NULL)) {
            continue;
        }
        for (const wchar_t *id = ids; *id; id += wcslen(id) + 1) {
            if (_wcsicmp(id, HARDWARE_ID) == 0) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL NeedsReboot(HDEVINFO Set, SP_DEVINFO_DATA *Dev)
{
    SP_DEVINSTALL_PARAMS_W params = { sizeof(params) };
    return SetupDiGetDeviceInstallParamsW(Set, Dev, &params) &&
           (params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT));
}

static BOOL DeviceExists(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(&CameraClass, NULL, NULL, 0);
    SP_DEVINFO_DATA dev;
    DWORD index = 0;
    BOOL found;

    if (set == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    found = NextVCamDevice(set, &index, &dev);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static int RemoveDevices(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(&CameraClass, NULL, NULL, 0);
    SP_DEVINFO_DATA dev;
    DWORD index = 0;
    int removed = 0;
    BOOL reboot = FALSE;

    if (set == INVALID_HANDLE_VALUE) {
        PrintError(L"SetupDiGetClassDevs");
        return -1;
    }

    while (NextVCamDevice(set, &index, &dev)) {
        wchar_t inf[MAX_PATH] = L"";
        DEVPROPTYPE type;

        // Remember which driver package the device uses before it goes away.
        SetupDiGetDevicePropertyW(set, &dev, &InfPathKey, &type, (BYTE *)inf, sizeof(inf), NULL, 0);

        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &dev)) {
            PrintError(L"Removing the device");
            continue;
        }
        removed++;
        reboot |= NeedsReboot(set, &dev);

        if (inf[0]) {
            wchar_t path[MAX_PATH];
            BOOL pkgReboot = FALSE;
            UINT n = GetWindowsDirectoryW(path, MAX_PATH);
            if (n && n < MAX_PATH &&
                wcscat_s(path, MAX_PATH, L"\\INF\\") == 0 && wcscat_s(path, MAX_PATH, inf) == 0 &&
                DiUninstallDriverW(NULL, path, 0, &pkgReboot)) {
                reboot |= pkgReboot;
            } else {
                PrintError(L"Deleting the driver package");
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    if (reboot) {
        wprintf(L"A restart is needed to finish removing the device.\n");
    }
    return removed;
}

static int Install(const wchar_t *infArg)
{
    wchar_t inf[MAX_PATH];
    BOOL reboot = FALSE;
    BOOL created = FALSE;

    if (infArg) {
        if (!GetFullPathNameW(infArg, MAX_PATH, inf, NULL)) {
            PrintError(L"GetFullPathName");
            return 1;
        }
    } else {
        // Default: vcam.inf next to this executable.
        wchar_t *slash;
        if (!GetModuleFileNameW(NULL, inf, MAX_PATH) || !(slash = wcsrchr(inf, L'\\'))) {
            PrintError(L"GetModuleFileName");
            return 1;
        }
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - inf), L"vcam.inf");
    }
    if (GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Cannot find %ls\n", inf);
        return 1;
    }

    // 1. Create the device node, unless a previous install already did.
    if (!DeviceExists()) {
        static const wchar_t hwid[] = HARDWARE_ID L"\0";  // multi-sz: ends with two NULs
        HDEVINFO set = SetupDiCreateDeviceInfoList(&CameraClass, NULL);
        SP_DEVINFO_DATA dev = { sizeof(dev) };

        if (set == INVALID_HANDLE_VALUE) {
            PrintError(L"SetupDiCreateDeviceInfoList");
            return 1;
        }
        if (!SetupDiCreateDeviceInfoW(set, L"Camera", &CameraClass, NULL, NULL, DICD_GENERATE_ID, &dev) ||
            !SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, (const BYTE *)hwid, sizeof(hwid)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev)) {
            PrintError(L"Creating the device");
            SetupDiDestroyDeviceInfoList(set);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(set);
        created = TRUE;
    }

    // 2. Install the driver package on it.
    wprintf(L"Installing %ls ...\n", inf);
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, HARDWARE_ID, inf, INSTALLFLAG_FORCE, &reboot)) {
        PrintError(L"Installing the driver");
        fwprintf(stderr, L"If this is a signature error, see the README: the driver must be test-signed and test signing enabled.\n");
        if (created) {
            RemoveDevices();  // don't leave a half-installed device behind
        }
        return 1;
    }

    wprintf(L"Installed. \"Virtual Webcam\" now shows up under Cameras in Device Manager.\n");
    if (reboot) {
        wprintf(L"A restart is needed to finish the installation.\n");
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && _wcsicmp(argv[1], L"install") == 0 && argc <= 3) {
        return Install(argc == 3 ? argv[2] : NULL);
    }
    if (argc == 2 && _wcsicmp(argv[1], L"uninstall") == 0) {
        int removed = RemoveDevices();
        if (removed < 0) {
            return 1;
        }
        wprintf(removed ? L"Removed.\n" : L"Nothing to remove.\n");
        return 0;
    }

    fwprintf(stderr,
             L"usage: vcamctl install [path\\to\\vcam.inf]\n"
             L"       vcamctl uninstall\n");
    return 2;
}
