; Hancho installer (Inno Setup 6)
;
; Prerequisites:
;   1. Build the Release binaries and assemble the payload into
;      dist\Hancho-v<version>-win64\ :
;        Hancho.exe, hancho-cli.exe, conpty.dll, OpenConsole.exe,
;        LICENSE, README.md, README-ja.md, doc\* (topic documents and doc\images)
;      tools\ (the ww task-card runner template) is taken from the repository directly.
;   2. Run installer\build_installer.cmd (it reads the version from CMakeLists.txt),
;      or compile with:  ISCC.exe /DMyAppVersion=<version> installer\Hancho.iss
;
; The install is per-user ({localappdata}\Programs) so the dashboard can write
; logs/state/task queues next to the executable without administrator rights.

#define MyAppName "Hancho"
; The version is the single source in CMakeLists.txt, passed as /DMyAppVersion=x.y.z.
#ifndef MyAppVersion
  #error MyAppVersion is not defined. Run installer\build_installer.cmd or pass /DMyAppVersion=x.y.z to ISCC.
#endif
#define MyAppPublisher "gadget114514"
#define MyAppURL "https://github.com/gadget114514/Hancho"
#define MyAppExeName "Hancho.exe"
#define PayloadDir "..\dist\Hancho-v" + MyAppVersion + "-win64"

[Setup]
AppId={{4ec6fed2-804c-4d7c-ad67-1cd12be5ed22}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Hancho Setup
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile={#PayloadDir}\LICENSE
OutputDir=..\dist
OutputBaseFilename=Hancho-v{#MyAppVersion}-win64-setup
SetupIconFile=..\images\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
SetupLogging=yes
AppMutex=Hancho_GUI_Mutex_v1

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "addtopath"; Description: "Add hancho-cli.exe to the user PATH (recommended for CLI / LLM agents)"; GroupDescription: "Integration:"; Flags: checkedonce
Name: "autostartgui"; Description: "Start the Hancho dashboard at sign-in (tray)"; GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\Hancho.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\hancho-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\conpty.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\OpenConsole.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\README-ja.md"; DestDir: "{app}"; Flags: ignoreversion
; doc\: the topic documents the READMEs link to (doc/start.md, doc/cli.md, ...) and doc\images
Source: "{#PayloadDir}\doc\*"; DestDir: "{app}\doc"; Flags: ignoreversion recursesubdirs createallsubdirs
; tools\ww: optional task-card runner template (copied into projects by an LLM agent; see tools\ww\README-ww.md)
Source: "..\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\README (English)"; Filename: "{app}\README.md"
Name: "{group}\README (日本語)"; Filename: "{app}\README-ja.md"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: NeedsAddPath('{app}'); Flags: preservestringtype
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Hancho"; ValueData: """{app}\{#MyAppExeName}"" gui"; Tasks: autostartgui; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"

[Code]
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    Exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

procedure RemoveFromUserPath(Path: string);
var
  OrigPath: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', OrigPath) then Exit;
  P := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(OrigPath) + ';');
  if P = 0 then Exit;
  Delete(OrigPath, P, Length(Path) + 1);
  RegWriteExpandStringValue(HKCU, 'Environment', 'Path', OrigPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromUserPath(ExpandConstant('{app}'));
end;
