#ifndef PackageDir
#error PackageDir must name the extracted client package
#endif
#ifndef AppVersion
#define AppVersion "1.0.0"
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
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
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

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl,languages\English.isl"
Name: "ko"; MessagesFile: "languages\KoreanDefault.isl,languages\Korean.isl"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{tmp}\LanLink"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{code:ProgramDirectory}\bin\lanlink-ui.exe"; Description: "{cm:OpenLanLink}"; Flags: postinstall nowait skipifsilent runasoriginaluser; Check: ShouldLaunch

[Code]
var
  ProgramPage: TInputDirWizardPage;
  MaintenancePage: TInputOptionWizardPage;
  OptionsPage: TInputOptionWizardPage;
  RemoveDataPage: TInputOptionWizardPage;
  Installed: Boolean;
  RequestedMode: String;
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

function MaintenanceMode: String;
begin
  Result := RequestedMode;
  if Result <> '' then Exit;
  if not Installed then begin Result := 'install'; Exit; end;
  case MaintenancePage.SelectedValueIndex of
    1: Result := 'repair';
    2: Result := 'uninstall';
  else Result := 'modify';
  end;
end;

function ShouldLaunch: Boolean;
begin
  Result := (InstallResult = 0) and (MaintenanceMode <> 'uninstall');
end;

procedure InitializeWizard;
var
  DataDirectory: String;
  Value: Cardinal;
begin
  InstallResult := 1;
  Log('LanLink setup language: ' + ActiveLanguage);
  Installed := RegQueryStringValue(HKEY_LOCAL_MACHINE_64,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'InstallLocation', DataDirectory);
  RequestedMode := Lowercase(ExpandConstant('{param:MODE|}'));
  StyleWizard;
  MaintenancePage := CreateInputOptionPage(wpWelcome, CustomMessage('MaintenanceTitle'),
    CustomMessage('MaintenanceSubtitle'), CustomMessage('MaintenanceDescription'), True, False);
  MaintenancePage.Add(CustomMessage('ModifyOption'));
  MaintenancePage.Add(CustomMessage('RepairOption'));
  MaintenancePage.Add(CustomMessage('UninstallOption'));
  MaintenancePage.SelectedValueIndex := 0;
  ProgramPage := CreateInputDirPage(MaintenancePage.ID, CustomMessage('ProgramTitle'),
    CustomMessage('ProgramSubtitle'), CustomMessage('ProgramDescription'), False, '');
  ProgramPage.Add(CustomMessage('ProgramDirectory'));
  ProgramPage.Values[0] := ExpandConstant('{param:DIR|}');
  if ProgramPage.Values[0] = '' then ProgramPage.Values[0] := DefaultProgramDirectory('');
  DataPage := CreateInputDirPage(ProgramPage.ID, CustomMessage('DataTitle'),
    CustomMessage('DataSubtitle'), CustomMessage('DataDescription'), False, '');
  DataPage.Add(CustomMessage('DataDirectory'));
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'DataDirectory', DataDirectory) then
    DataDirectory := ExpandConstant('{commonappdata}\LanLink');
  DataPage.Values[0] := ExpandConstant('{param:DataDir|}');
  if DataPage.Values[0] = '' then DataPage.Values[0] := DataDirectory;
  OptionsPage := CreateInputOptionPage(DataPage.ID, CustomMessage('OptionsTitle'),
    CustomMessage('OptionsSubtitle'), CustomMessage('OptionsDescription'), False, False);
  OptionsPage.Add(CustomMessage('CliOption'));
  OptionsPage.Add(CustomMessage('StartMenuOption'));
  OptionsPage.Add(CustomMessage('DesktopOption'));
  OptionsPage.Values[0] := True;
  OptionsPage.Values[1] := True;
  OptionsPage.Values[2] := False;
  if Installed then begin
    if RegQueryDWordValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'Cli', Value) then OptionsPage.Values[0] := Value <> 0;
    if RegQueryDWordValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'StartMenu', Value) then OptionsPage.Values[1] := Value <> 0;
    if RegQueryDWordValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'Desktop', Value) then OptionsPage.Values[2] := Value <> 0;
  end;
  if ExpandConstant('{param:Cli|}') <> '' then OptionsPage.Values[0] := ExpandConstant('{param:Cli|1}') = '1';
  if ExpandConstant('{param:StartMenu|}') <> '' then OptionsPage.Values[1] := ExpandConstant('{param:StartMenu|1}') = '1';
  if ExpandConstant('{param:Desktop|}') <> '' then OptionsPage.Values[2] := ExpandConstant('{param:Desktop|0}') = '1';
  RemoveDataPage := CreateInputOptionPage(OptionsPage.ID, CustomMessage('RemoveTitle'),
    CustomMessage('RemoveSubtitle'), CustomMessage('RemoveDescription'), False, False);
  RemoveDataPage.Add(CustomMessage('RemoveDataOption'));
  RemoveDataPage.Values[0] := ExpandConstant('{param:RemoveData|0}') = '1';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
var
  Mode: String;
begin
  Mode := MaintenanceMode;
  Result := False;
  if PageID = MaintenancePage.ID then Result := not Installed or (RequestedMode <> '')
  else if (PageID = ProgramPage.ID) or (PageID = DataPage.ID) then Result := Installed or (Mode <> 'install')
  else if PageID = OptionsPage.ID then Result := (Mode = 'repair') or (Mode = 'uninstall')
  else if PageID = RemoveDataPage.ID then Result := Mode <> 'uninstall';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Mode: String;
begin
  Mode := MaintenanceMode;
  Result := '';
  if (Mode <> 'install') and (Mode <> 'modify') and (Mode <> 'repair') and (Mode <> 'uninstall') then
    Result := CustomMessage('InvalidMode');
  if (not Installed) and (Mode <> 'install') then Result := CustomMessage('NotInstalled');
  // Server options have no meaning in Setup, including unattended installs.
  if (ExpandConstant('{param:RelayHost|}') <> '') or (ExpandConstant('{param:TokenFile|}') <> '') or
     (ExpandConstant('{param:RelayPort|}') <> '') then Result := CustomMessage('NoServerOptions');
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := CustomMessage('SummaryMode') + ': ' + MaintenanceMode + NewLine + NewLine +
    CustomMessage('SummaryApp') + ':' + NewLine + Space + ProgramDirectory('') + NewLine + NewLine +
    CustomMessage('SummaryData') + ':' + NewLine + Space + DataPage.Values[0] + NewLine + NewLine +
    CustomMessage('SummaryComponents') + ':' + NewLine + Space + CustomMessage('Components');
  if MaintenanceMode = 'uninstall' then
    if RemoveDataPage.Values[0] then Result := Result + NewLine + CustomMessage('DeleteDataSummary')
    else Result := Result + NewLine + CustomMessage('PreserveDataSummary');
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
    ' -Mode ' + MaintenanceMode + ' -SetupExecutable ' + Quote(ExpandConstant('{srcexe}')) +
    ' -LogFile ' + Quote(LogPath);
  if not OptionsPage.Values[0] then Parameters := Parameters + ' -NoCli';
  if not OptionsPage.Values[1] then Parameters := Parameters + ' -NoStartMenu';
  if OptionsPage.Values[2] then Parameters := Parameters + ' -DesktopShortcut';
  if RemoveDataPage.Values[0] then Parameters := Parameters + ' -RemoveData';
  if ExpandConstant('{param:NoStart|0}') = '1' then Parameters := Parameters + ' -NoStart';
  Log('LanLink maintenance mode: ' + MaintenanceMode);
  WizardForm.StatusLabel.Caption := CustomMessage('ConfigureStatus');
  WizardForm.FilenameLabel.Caption := CustomMessage('ConfigureDetails');
  WizardForm.ProgressGauge.Style := npbstMarquee;
  try
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
        '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then RaiseException(CustomMessage('CannotRun'));
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

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and (MaintenanceMode = 'uninstall') then begin
    WizardForm.FinishedHeadingLabel.Caption := CustomMessage('RemovedHeading');
    WizardForm.FinishedLabel.Caption := CustomMessage('RemovedDetails');
  end;
end;
