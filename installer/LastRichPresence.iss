#define MyAppName "Last Rich Presence"
#define MyAppVersion "2.5.0"
#define MyAppPublisher "Last Projects"
#define MyAppURL "https://lastprojects.com/"
#define MyAppExeName "Last_Rich_Presence.exe"
#define BuildOutputDir "..\x64\Release-Inno\Last Rich Presence"

[Setup]
AppId={{BE5F3A61-C450-4822-B5EB-C575F4D5ED53}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
LicenseFile=LICENSE.txt
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=LastRichPresence-Setup-x64
SetupIconFile=..\Assets\logo.ico
UninstallDisplayIcon={app}\Assets\logo.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildOutputDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,*.ilk,*.exp,*.lib,*.build.appxrecipe"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\Assets\logo.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\Assets\logo.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM {#MyAppExeName}"; Flags: runhidden skipifdoesntexist; RunOnceId: "KillLastRichPresenceProcess"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /T /IM RestartAgent.exe"; Flags: runhidden skipifdoesntexist; RunOnceId: "KillLastRichPresenceRestartAgent"

[Code]
procedure TerminateLastRichPresenceIfRunning();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM {#MyAppExeName}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM RestartAgent.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function InitializeSetup(): Boolean;
begin
  TerminateLastRichPresenceIfRunning();
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    TerminateLastRichPresenceIfRunning();
end;
