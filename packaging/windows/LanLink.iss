#ifndef PackageDir
#error PackageDir must name the extracted client package
#endif
#ifndef AppVersion
#define AppVersion "0.2.0"
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
DisableProgramGroupPage=yes
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
WizardStyle=modern
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
Filename: "{code:ProgramDirectory}\bin\lanlink-ui.exe"; Description: "Open LanLink"; Flags: postinstall nowait skipifsilent runasoriginaluser

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

procedure InitializeWizard;
var
  DataDirectory: String;
begin
  ProgramPage := CreateInputDirPage(wpWelcome, 'Program location',
    'Choose where to install LanLink', 'Select a program directory separate from your persistent data.', False, '');
  ProgramPage.Add('Program directory:');
  ProgramPage.Values[0] := ExpandConstant('{param:DIR|}');
  if ProgramPage.Values[0] = '' then ProgramPage.Values[0] := DefaultProgramDirectory('');
  RelayPage := CreateInputQueryPage(ProgramPage.ID, 'Relay connection',
    'Connect to your relay server', 'Enter its hostname or IP address. Leave the hostname empty to preserve an existing connection.');
  RelayPage.Add('Relay hostname or IP:', False);
  RelayPage.Add('Port:', False);
  RelayPage.Values[0] := ExpandConstant('{param:RelayHost|}');
  RelayPage.Values[1] := ExpandConstant('{param:RelayPort|4433}');
  TokenPage := CreateInputFilePage(RelayPage.ID, 'Authentication',
    'Select the token file supplied by your relay administrator', 'The file is stored with administrator and service access only. Leave it empty to preserve an existing token.');
  TokenPage.Add('Authentication token file:', 'All files|*.*', '');
  TokenPage.Values[0] := ExpandConstant('{param:TokenFile|}');
  DataPage := CreateInputDirPage(TokenPage.ID, 'Persistent data',
    'Keep your identity and connection settings', 'Upgrades must use the original data directory.', False, '');
  DataPage.Add('Data directory:');
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
  if (Port < 1) or (Port > 65535) then Result := 'Enter a port between 1 and 65535.';
  for Index := 1 to Length(Host) do
    if Pos(Host[Index], ' "#=\/') > 0 then Result := 'Enter a hostname or IP address without spaces, quotes or a URL scheme.';
  if (Host = '') and not FileExists(DataPage.Values[0] + '\client.conf') then
    Result := 'A relay hostname or IP address is required for the first installation.';
  if (TokenPage.Values[0] = '') and not FileExists(DataPage.Values[0] + '\auth.token') then
    Result := 'Select an authentication token file for the first installation.';
  if (TokenPage.Values[0] <> '') and not FileExists(TokenPage.Values[0]) then
    Result := 'The selected token file does not exist.';
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
  WizardForm.StatusLabel.Caption := 'Installing the Windows service and configuring LanLink...';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Parameters,
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Cannot run the LanLink installer.');
  if ResultCode <> 0 then begin
    if LoadStringFromFile(LogPath, Transcript) then Log(String(Transcript));
    RaiseException('LanLink installation failed. The previous installation was restored when possible. See ' + LogPath);
  end;
  InstallResult := 0;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := InstallResult;
end;
