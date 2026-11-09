#define AppName "LR Visual ASIO"
#define AppVersion "1.0.0"

[Setup]
AppId={{B1E7C0A4-5D3F-4C2E-9A61-7F8D2E4C9B10}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=LR PRO
DefaultDirName={autopf}\LR Visual ASIO
DefaultGroupName=LR Visual ASIO
OutputDir=..\dist
OutputBaseFilename=LR_Visual_ASIO_Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\app\Assets\icon.ico
UninstallDisplayIcon={app}\LRVisualASIO.exe
CloseApplications=yes

[Files]
Source: "..\build\app\LRVisualASIO.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\driver\LRVisualASIO.dll"; DestDir: "{app}"; Flags: ignoreversion regserver

[Registry]
Root: HKLM; Subkey: "SOFTWARE\LRVisual\ASIO"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey

[Icons]
Name: "{group}\LR Visual ASIO"; Filename: "{app}\LRVisualASIO.exe"
Name: "{group}\Uninstall LR Visual ASIO"; Filename: "{uninstallexe}"
Name: "{autodesktop}\LR Visual ASIO"; Filename: "{app}\LRVisualASIO.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Buat shortcut di Desktop"; Flags: unchecked

[Run]
Filename: "{app}\LRVisualASIO.exe"; Description: "Buka LR Visual ASIO"; Flags: nowait postinstall skipifsilent

[Code]
procedure KillApp();
var rc: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM LRVisualASIO.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  KillApp();
  Result := '';
end;

procedure CurrentUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    KillApp();
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'LR Visual ASIO');
  end;
end;
