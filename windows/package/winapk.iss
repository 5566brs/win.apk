; winapk.iss -- the Win.apk installer (Inno Setup 6).
;
;   iscc /DSourceDir=<staged folder with jre> /DAppVersion=0.1.0-preview windows\package\winapk.iss
;
; Installs per user (no administrator rights) to %LOCALAPPDATA%\Programs\Win.apk,
; adds Win.apk to the Start menu (and the desktop if asked), opens .apk files
; with it, and uninstalls cleanly. Apps and their data live in
; %LOCALAPPDATA%\Win.apk and stay when Win.apk is uninstalled.

#ifndef SourceDir
  #error Pass /DSourceDir=<the staged Win.apk folder>
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{6E1B7C2A-5F3D-4C8B-9A0E-7D2F1A4B3C59}
AppName=Win.apk
AppVersion={#AppVersion}
AppVerName=Win.apk {#AppVersion}
AppPublisher=Win.apk
VersionInfoVersion=0.1.0
DefaultDirName={localappdata}\Programs\Win.apk
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=Win.apk-Setup
SetupIconFile=..\launcher\winapk.ico
UninstallDisplayIcon={app}\Win.apk.exe
UninstallDisplayName=Win.apk
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
CloseApplications=yes

[Tasks]
Name: desktopicon; Description: "Put Win.apk on the desktop"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion createallsubdirs

[Icons]
Name: "{userprograms}\Win.apk"; Filename: "{app}\Win.apk.exe"; Comment: "Android apps on Windows"
Name: "{userdesktop}\Win.apk"; Filename: "{app}\Win.apk.exe"; Comment: "Android apps on Windows"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.apk"; ValueType: string; ValueData: "WinApk.Package"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\WinApk.Package"; ValueType: string; ValueData: "Android app"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\WinApk.Package\DefaultIcon"; ValueType: string; ValueData: """{app}\Win.apk.exe"",0"
Root: HKCU; Subkey: "Software\Classes\WinApk.Package\shell\open\command"; ValueType: string; ValueData: """{app}\Win.apk.exe"" ""%1"""

[UninstallDelete]
; The apps' shortcuts the library made (Start > Win.apk Apps).
Type: filesandordirs; Name: "{userprograms}\Win.apk Apps"

[Run]
Filename: "{app}\Win.apk.exe"; Description: "Open Win.apk"; Flags: nowait postinstall skipifsilent
