; Minima — Inno Setup script for a real setup.exe (per-user, no admin prompt).
; Build: install Inno Setup 6 (https://jrsoftware.org/isinfo.php), then:
;   iscc installer\minima.iss
; Output: installer\Output\minima-setup.exe
;
; The scripts\install.ps1 path does the same job without Inno Setup.

[Setup]
AppName=Minima
AppVersion=0.9
AppPublisher=Minima
DefaultDirName={localappdata}\Programs\Minima
DefaultGroupName=Minima
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=minima-setup
SetupIconFile=..\assets\minima.ico
UninstallDisplayIcon={app}\minima.exe
Compression=lzma2
SolidCompression=yes

[Files]
Source: "..\build\minima.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\Minima"; Filename: "{app}\minima.exe"
Name: "{userdesktop}\Minima"; Filename: "{app}\minima.exe"; Tasks: desktopicon

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
; Register as a browser candidate so Minima appears in Windows' Default apps.
Filename: "{app}\minima.exe"; Parameters: "--register"; Flags: runhidden
Filename: "{app}\minima.exe"; Description: "Launch Minima"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{app}\minima.exe"; Parameters: "--unregister"; Flags: runhidden; RunOnceId: "unreg"

; Browsing data in {localappdata}\Minima is intentionally left behind on uninstall.
