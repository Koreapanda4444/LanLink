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
DisableReadyPage=yes
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
WizardStyle=modern
WizardSizePercent=100,100
WizardResizable=no
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
SetupIconFile=..\..\assets\branding\lanlink.ico
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
  DataDirectory: String;
  InstallResult: Integer;
  InstallProgress: Integer;
  InstallGauge: TNewProgressBar;
  ProgressTimer: UINT_PTR;
  ProgressPath, ProgressStage: String;
  BackendRunning: Boolean;

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
begin
  WizardForm.WizardBitmapImage.Visible := False;
  WizardForm.WizardBitmapImage2.Visible := False;
  WizardForm.WizardSmallBitmapImage.Visible := False;
  WizardForm.WelcomeLabel1.SetBounds(ScaleX(24), ScaleY(24),
    WizardForm.WelcomePage.ClientWidth - ScaleX(48), ScaleY(40));
  WizardForm.WelcomeLabel1.Font.Size := 14;
  WizardForm.WelcomeLabel1.Caption := CustomMessage('InstallTitle');
  WizardForm.WelcomeLabel2.SetBounds(ScaleX(24), ScaleY(86),
    WizardForm.WelcomePage.ClientWidth - ScaleX(48), WizardForm.WelcomePage.ClientHeight - ScaleY(110));
  WizardForm.WelcomeLabel2.Caption := CustomMessage('InstallDescription');
  WizardForm.FinishedHeadingLabel.Left := ScaleX(24);
  WizardForm.FinishedHeadingLabel.Width := WizardForm.FinishedPage.ClientWidth - ScaleX(48);
  WizardForm.FinishedLabel.Left := ScaleX(24);
  WizardForm.FinishedLabel.Width := WizardForm.FinishedPage.ClientWidth - ScaleX(48);
  WizardForm.RunList.Left := ScaleX(24);
  WizardForm.RunList.Width := WizardForm.FinishedPage.ClientWidth - ScaleX(48);
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
  ExistingLocation: String;
  Value: Cardinal;
begin
  InstallResult := 1;
  Log('LanLink setup language: ' + ActiveLanguage);
  Installed := RegQueryStringValue(HKEY_LOCAL_MACHINE_64,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'InstallLocation', ExistingLocation);
  RequestedMode := Lowercase(ExpandConstant('{param:MODE|}'));
  StyleWizard;
  InstallGauge := TNewProgressBar.Create(WizardForm);
  InstallGauge.Parent := WizardForm.ProgressGauge.Parent;
  InstallGauge.SetBounds(WizardForm.ProgressGauge.Left, WizardForm.ProgressGauge.Top,
    WizardForm.ProgressGauge.Width, WizardForm.ProgressGauge.Height);
  InstallGauge.Min := 0;
  InstallGauge.Max := 100;
  InstallGauge.Style := npbstNormal;
  WizardForm.ProgressGauge.Visible := False;
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
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE_64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\LanLink', 'DataDirectory', DataDirectory) then
    DataDirectory := ExpandConstant('{commonappdata}\LanLink');
  OptionsPage := CreateInputOptionPage(ProgramPage.ID, CustomMessage('OptionsTitle'),
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
  else if PageID = ProgramPage.ID then Result := Installed or (Mode <> 'install')
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

function SetTimer(hWnd: HWND; nIDEvent: UINT_PTR; uElapse: UINT; lpTimerFunc: NativeInt): UINT_PTR;
  external 'SetTimer@user32.dll stdcall';
function KillTimer(hWnd: HWND; nIDEvent: UINT_PTR): Integer;
  external 'KillTimer@user32.dll stdcall';

procedure SetInstallProgress(Value: Integer; Stage: String);
var
  Detail: String;
begin
  if Value < InstallProgress then Value := InstallProgress;
  if Value > 100 then Value := 100;
  if (Value <> InstallProgress) or (Stage <> ProgressStage) then
    Log(Format('LanLink progress: %d %s', [Value, Stage]));
  InstallProgress := Value;
  ProgressStage := Stage;
  InstallGauge.Position := InstallProgress;
  if Stage = 'copy' then Detail := CustomMessage('ProgressCopy')
  else if Stage = 'apply' then Detail := CustomMessage('ProgressApply')
  else if Stage = 'shortcuts' then Detail := CustomMessage('ProgressShortcuts')
  else if Stage = 'cleanup' then Detail := CustomMessage('ProgressCleanup')
  else if Stage = 'rollback' then Detail := CustomMessage('ProgressRollback')
  else if Stage = 'remove' then Detail := CustomMessage('ProgressRemove')
  else if Stage = 'complete' then Detail := CustomMessage('ProgressComplete')
  else Detail := CustomMessage('ProgressPrepare');
  WizardForm.StatusLabel.Caption := Detail;
  WizardForm.FilenameLabel.Caption := IntToStr(InstallProgress) + '%';
end;

procedure ReadSetupProgress;
var
  Data: AnsiString;
  Content, Stage: String;
  Separator, Value: Integer;
begin
  if not LoadStringFromFile(ProgressPath, Data) then Exit;
  Content := Trim(String(Data));
  if Length(Content) > 80 then Exit;
  Separator := Pos('|', Content);
  if Separator = 0 then Exit;
  Value := StrToIntDef(Copy(Content, 1, Separator - 1), -2);
  if (Value < -1) or (Value > 100) then Exit;
  Stage := Copy(Content, Separator + 1, Length(Content));
  if Value < 0 then SetInstallProgress(InstallProgress, Stage)
  else SetInstallProgress(20 + (Value * 79) div 100, Stage);
end;

procedure UpdateSetupProgress(Arg1: HWND; Arg2: UINT; Arg3: UINT_PTR; Arg4: DWORD);
begin
  if BackendRunning then ReadSetupProgress;
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
begin
  if MaxProgress > 0 then
    SetInstallProgress(Integer((Int64(CurProgress) * 20) div MaxProgress), 'prepare');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Parameters, LogPath: String;
  Transcript: AnsiString;
  ResultCode: Integer;
begin
  if CurStep = ssInstall then begin
    InstallProgress := 0;
    ProgressStage := '';
    SetInstallProgress(0, 'prepare');
  end;
  if CurStep <> ssPostInstall then Exit;
  InstallResult := 1;
  LogPath := ExpandConstant('{log}') + '.client.log';
  ProgressPath := ExpandConstant('{tmp}\LanLink-progress.txt');
  DeleteFile(ProgressPath);
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    Quote(ExpandConstant('{tmp}\LanLink\SetupClient.ps1')) +
    ' -InstallDirectory ' + Quote(ProgramDirectory('')) +
    ' -DataDirectory ' + Quote(DataDirectory) +
    ' -Mode ' + MaintenanceMode + ' -SetupExecutable ' + Quote(ExpandConstant('{srcexe}')) +
    ' -LogFile ' + Quote(LogPath) + ' -ProgressFile ' + Quote(ProgressPath);
  if not OptionsPage.Values[0] then Parameters := Parameters + ' -NoCli';
  if not OptionsPage.Values[1] then Parameters := Parameters + ' -NoStartMenu';
  if OptionsPage.Values[2] then Parameters := Parameters + ' -DesktopShortcut';
  if RemoveDataPage.Values[0] then Parameters := Parameters + ' -RemoveData';
  if ExpandConstant('{param:NoStart|0}') = '1' then Parameters := Parameters + ' -NoStart';
  Log('LanLink maintenance mode: ' + MaintenanceMode);
  SetInstallProgress(20, 'prepare');
  BackendRunning := True;
  ProgressTimer := SetTimer(0, 0, 100, CreateCallback(@UpdateSetupProgress));
  if ProgressTimer = 0 then begin
    BackendRunning := False;
    RaiseException(CustomMessage('CannotRun'));
  end;
  try
    if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
        '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then RaiseException(CustomMessage('CannotRun'));
    if ResultCode <> 0 then begin
      if LoadStringFromFile(LogPath, Transcript) then Log(String(Transcript));
      RaiseException(CustomMessage('InstallFailed') + #13#10 + LogPath);
    end;
    ReadSetupProgress;
    InstallResult := 0;
    SetInstallProgress(100, 'complete');
  finally
    BackendRunning := False;
    KillTimer(0, ProgressTimer);
    ProgressTimer := 0;
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := InstallResult;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpInstalling then begin
    WizardForm.ProgressGauge.Visible := False;
    InstallGauge.BringToFront;
  end;
  if (CurPageID = wpFinished) and (MaintenanceMode = 'uninstall') then begin
    WizardForm.FinishedHeadingLabel.Caption := CustomMessage('RemovedHeading');
    WizardForm.FinishedLabel.Caption := CustomMessage('RemovedDetails');
  end;
end;
