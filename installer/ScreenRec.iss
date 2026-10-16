; ScreenRec 1.0 — лёгкий установщик (Inno Setup 6).
; Сборка: build-installer.cmd из каталога с уже собранным ScreenRec-x64.exe

#define MyAppName "ScreenRec"
#define MyAppVersion "1.0.0"
#define MyAppExe "ScreenRec-x64.exe"

[Setup]
AppId={{7E2A9C14-5B48-4F0E-A6D3-8C91F0B2E6A7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=ScreenRec
AppPublisherURL=https://github.com/neuromafka-create/ScreenRec
AppSupportURL=https://github.com/neuromafka-create/ScreenRec/issues
AppUpdatesURL=https://github.com/neuromafka-create/ScreenRec/releases
DefaultDirName={localappdata}\Programs\ScreenRec
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist
OutputBaseFilename=ScreenRec-1.0.0-x64-setup
SetupIconFile=..\wcap1.ico
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
VersionInfoVersion=1.0.0
VersionInfoProductVersion=1.0.0
VersionInfoCompany=ScreenRec
VersionInfoDescription=ScreenRec 1.0 setup
VersionInfoProductName=ScreenRec
CloseApplications=yes
RestartApplications=no
LicenseFile=..\LICENSE

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "Запускать ScreenRec при входе в Windows"; GroupDescription: "Дополнительно:"; Flags: unchecked

[Files]
Source: "..\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Comment: "Запись экрана"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ScreenRec"; ValueData: """{app}\{#MyAppExe}"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "Запустить ScreenRec"; Flags: nowait postinstall skipifsilent unchecked
