; VCam setup program (Inno Setup 6.4 or later).
;
;   iscc /DAppVersion=2.1.0 /DFileVersion=2.1.0.0 /DDist=..\dist /DOutput=..\release installer\vcam.iss
;
; Dist is the packaged build (vcam.exe, vcam-gui.exe, the camera DLLs, sdk\,
; python\, README.md). The files go to Program Files\vcam; `vcam install
; --managed` then registers the cameras, which picks the Windows 11 or the
; Windows 10 backend by itself and keeps an earlier installation's cameras.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef FileVersion
  #define FileVersion "0.0.0.0"
#endif
#ifndef Dist
  #define Dist "..\dist"
#endif
#ifndef Output
  #define Output "..\release"
#endif

[Setup]
AppId=VCam
AppName=VCam
AppVersion={#AppVersion}
AppVerName=VCam {#AppVersion}
AppPublisher=vcam
AppComments=A virtual camera for Windows: share your screen, a window, a picture, a video or another camera with any video app.
VersionInfoVersion={#FileVersion}
VersionInfoDescription=VCam setup
DefaultDirName={autopf}\vcam
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809 or later (screen capture and WebView2 need it).
MinVersion=10.0.17763
WizardStyle=modern
SetupIconFile=..\windows\gui\vcam.ico
UninstallDisplayIcon={app}\vcam-gui.exe
UninstallDisplayName=VCam
OutputDir={#Output}
OutputBaseFilename=VCam-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
; Close VCam while files are replaced; cameras that ran come back when it starts again.
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#Dist}\vcam.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\vcam-gui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\vcam.dll"; DestDir: "{app}"; Flags: ignoreversion
; Loaded by Windows Frame Server or by the apps using the camera: replaced at
; the next restart when in use.
Source: "{#Dist}\vcamsource.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#Dist}\vcamdshow.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete skipifsourcedoesntexist
Source: "{#Dist}\vcamdshow32.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete skipifsourcedoesntexist
Source: "{#Dist}\sdk\*"; DestDir: "{app}\sdk"; Flags: ignoreversion
Source: "{#Dist}\python\*"; DestDir: "{app}\python"; Flags: ignoreversion
Source: "{#Dist}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\VCam"; Filename: "{app}\vcam-gui.exe"; Comment: "Choose what your virtual camera shows"

[Run]
Filename: "{app}\vcam.exe"; Parameters: "install --managed"; StatusMsg: "Registering the camera..."; Flags: runhidden waituntilterminated
Filename: "{app}\vcam-gui.exe"; Description: "Open VCam"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM vcam-gui.exe /F"; Flags: runhidden waituntilterminated; RunOnceId: "CloseVCam"
Filename: "{app}\vcam.exe"; Parameters: "uninstall --managed"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveCameras"

[UninstallDelete]
Type: dirifempty; Name: "{app}\sdk"
Type: dirifempty; Name: "{app}\python"
Type: dirifempty; Name: "{app}"
