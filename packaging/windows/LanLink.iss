#ifndef PackageDir
#error PackageDir must name the extracted client package
#endif
#ifndef AppVersion
#define AppVersion "0.2.2"
#endif
#ifndef SourceCommit
#error SourceCommit must identify the packaged source
#endif

[Setup]
AppId=LanLink
AppMutex=LanLink.Desktop
AppName=LanLink
AppVersion={#AppVersion}
AppPublisher=Koreapanda4444
AppPublisherURL=https://github.com/Koreapanda4444/LanLink
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableFinishedPage=no
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
WizardStyle=modern
WizardSizePercent=125,125
WizardResizable=no
SetupIconFile=..\..\assets\branding\lanlink.ico
WizardImageFile=..\..\assets\branding\installer-sidebar.bmp
WizardSmallImageFile=..\..\assets\branding\installer-header.bmp
WizardImageStretch=yes
Compression=lzma2
SolidCompression=yes
SetupLogging=yes
OutputBaseFilename=LanLink-Setup-{#AppVersion}-windows-x64
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=LanLink Windows installer
VersionInfoCopyright=Source commit: {#SourceCommit}
CloseApplications=no
RestartApplications=no

[Files]
Source: "{#PackageDir}\*"; DestDir: "{tmp}\LanLink"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{code:ProgramDirectory}\bin\lanlink-ui.exe"; Description: "{cm:OpenLanLink}"; Flags: postinstall nowait skipifsilent runasoriginaluser

#include "languages\English.isl"

[Code]
var
  ProgramPage: TInputDirWizardPage;
  RelayPage: TInputQueryWizardPage;
  TokenPage: TInputFileWizardPage;
  DataPage: TInputDirWizardPage;
  InstallResult: Integer;

function DefaultProgramDirectory(Param: String): String;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'InstallLocation', Result) then
    Result := ExpandConstant('{autopf}\LanLink');
end;

function Quote(Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function ProgramDirectory(Param: String): String;
begin
  Result := ProgramPage.Values[0];
end;

procedure StyleWizard;
var
  BrandLabel: TNewStaticText;
begin
  WizardForm.Color := $00F8F5F1;
  WizardForm.MainPanel.Color := clWhite;
  WizardForm.WelcomePage.Color := clWhite;
  WizardForm.FinishedPage.Color := clWhite;
  WizardForm.WelcomeLabel1.Font.Color := $00432110;
  WizardForm.WelcomeLabel1.Font.Size := 22;
  WizardForm.WelcomeLabel1.Height := ScaleY(82);
  WizardForm.WelcomeLabel2.Top := WizardForm.WelcomeLabel1.Top + WizardForm.WelcomeLabel1.Height + ScaleY(12);
  WizardForm.WelcomeLabel2.Height := WizardForm.WelcomePage.ClientHeight - WizardForm.WelcomeLabel2.Top - ScaleY(16);
  WizardForm.PageNameLabel.Font.Color := $00432110;
  WizardForm.FinishedHeadingLabel.Font.Color := $00432110;
  WizardForm.ReadyMemo.Color := clWhite;
  BrandLabel := TNewStaticText.Create(WizardForm);
  BrandLabel.Parent := WizardForm;
  BrandLabel.SetBounds(ScaleX(18), WizardForm.CancelButton.Top + ScaleY(5), ScaleX(160), ScaleY(18));
  BrandLabel.Font.Color := $00857160;
  BrandLabel.Caption := 'LanLink  {#AppVersion}';
end;

procedure InitializeWizard;
var
  DataDirectory: String;
begin
  StyleWizard;
  ProgramPage := CreateInputDirPage(wpWelcome, CustomMessage('ProgramTitle'),
    CustomMessage('ProgramSubtitle'), CustomMessage('ProgramDescription'), False, '');
  ProgramPage.Add(CustomMessage('ProgramDirectory'));
  ProgramPage.Values[0] := ExpandConstant('{param:DIR|}');
  if ProgramPage.Values[0] = '' then ProgramPage.Values[0] := DefaultProgramDirectory('');
  RelayPage := CreateInputQueryPage(ProgramPage.ID, CustomMessage('RelayTitle'),
    CustomMessage('RelaySubtitle'), CustomMessage('RelayDescription'));
  RelayPage.Add(CustomMessage('RelayHost'), False);
  RelayPage.Add(CustomMessage('RelayPort'), False);
  RelayPage.Values[0] := ExpandConstant('{param:RelayHost|}');
  RelayPage.Values[1] := ExpandConstant('{param:RelayPort|4433}');
  TokenPage := CreateInputFilePage(RelayPage.ID, CustomMessage('TokenTitle'),
    CustomMessage('TokenSubtitle'), CustomMessage('TokenDescription'));
  TokenPage.Add(CustomMessage('TokenFile'), CustomMessage('AllFiles') + '|*.*', '');
  TokenPage.Values[0] := ExpandConstant('{param:TokenFile|}');
  DataPage := CreateInputDirPage(TokenPage.ID, CustomMessage('DataTitle'),
    CustomMessage('DataSubtitle'), CustomMessage('DataDescription'), False, '');
  DataPage.Add(CustomMessage('DataDirectory'));
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'DataDirectory', DataDirectory) then
    DataDirectory := ExpandConstant('{commonappdata}\LanLink');
  DataPage.Values[0] := ExpandConstant('{param:DataDir|}');
  if DataPage.Values[0] = '' then DataPage.Values[0] := DataDirectory;
end;

function ConfigurationError: String;
var
  Port, Index: Integer;
  Host: String;
begin
  Result := '';
  Host := Trim(RelayPage.Values[0]);
  Port := StrToIntDef(RelayPage.Values[1], 0);
  if (Port < 1) or (Port > 65535) then Result := CustomMessage('PortError');
  for Index := 1 to Length(Host) do
    if Pos(Host[Index], ' "#=\/') > 0 then Result := CustomMessage('HostError');
  if (Host = '') and not FileExists(DataPage.Values[0] + '\client.conf') then
    Result := CustomMessage('HostRequired');
  if (TokenPage.Values[0] = '') and not FileExists(DataPage.Values[0] + '\auth.token') then
    Result := CustomMessage('TokenRequired');
  if (TokenPage.Values[0] <> '') and not FileExists(TokenPage.Values[0]) then
    Result := CustomMessage('TokenMissing');
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Relay, Token: String;
begin
  Relay := CustomMessage('KeepRelay');
  if Trim(RelayPage.Values[0]) <> '' then Relay := Trim(RelayPage.Values[0]) + ':' + RelayPage.Values[1];
  Token := CustomMessage('KeepToken');
  if TokenPage.Values[0] <> '' then Token := ExtractFileName(TokenPage.Values[0]);
  Result := CustomMessage('SummaryApp') + ':' + NewLine + Space + ProgramDirectory('') + NewLine + NewLine +
    CustomMessage('SummaryRelay') + ':' + NewLine + Space + Relay + NewLine + NewLine +
    CustomMessage('SummaryToken') + ':' + NewLine + Space + Token + NewLine + NewLine +
    CustomMessage('SummaryData') + ':' + NewLine + Space + DataPage.Values[0] + NewLine + NewLine +
    CustomMessage('SummaryComponents') + ':' + NewLine + Space + CustomMessage('Components');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := ConfigurationError;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Parameters, LogPath: String;
  Transcript: AnsiString;
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  InstallResult := 1;
  LogPath := ExpandConstant('{log}') + '.client.log';
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    Quote(ExpandConstant('{tmp}\LanLink\SetupClient.ps1')) +
    ' -InstallDirectory ' + Quote(ProgramDirectory('')) +
    ' -DataDirectory ' + Quote(DataPage.Values[0]) +
    ' -RelayPort ' + RelayPage.Values[1] + ' -LogFile ' + Quote(LogPath);
  if Trim(RelayPage.Values[0]) <> '' then Parameters := Parameters + ' -RelayHost ' + Quote(Trim(RelayPage.Values[0]));
  if TokenPage.Values[0] <> '' then Parameters := Parameters + ' -AuthenticationTokenFile ' + Quote(TokenPage.Values[0]);
  if ExpandConstant('{param:NoStart|0}') = '1' then Parameters := Parameters + ' -NoStart';
  WizardForm.StatusLabel.Caption := CustomMessage('ConfigureStatus');
  WizardForm.FilenameLabel.Caption := CustomMessage('ConfigureDetails');
  WizardForm.ProgressGauge.Style := npbstMarquee;
  try
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
        '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      RaiseException(CustomMessage('CannotRun'));
    if ResultCode <> 0 then begin
      if LoadStringFromFile(LogPath, Transcript) then Log(String(Transcript));
      RaiseException(CustomMessage('InstallFailed') + #13#10 + LogPath);
    end;
    InstallResult := 0;
  finally
    WizardForm.ProgressGauge.Style := npbstNormal;
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := InstallResult;
end;
