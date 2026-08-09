#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{2D501664-9D74-4D75-96A0-6BD0F564CE74}
AppName=Listary浏览器插件
AppVersion={#AppVersion}
AppPublisher=BrowserHistoryLauncher
DefaultDirName={localappdata}\Programs\ListaryBrowserPlugin
DefaultGroupName=Listary浏览器插件
DisableProgramGroupPage=yes
OutputDir=..\build\installer
OutputBaseFilename=ListaryBrowserPlugin-Setup-x64
SetupIconFile=..\assets\app.ico
UninstallDisplayIcon={app}\BrowserHistoryLauncher.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Listary浏览器插件安装程序

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "开机自动启动"; GroupDescription: "附加选项："; Flags: checkedonce
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项："; Flags: unchecked

[Files]
Source: "..\build\Release\BrowserHistoryLauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\BrowserHistoryLauncher.ini"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\LISTARY_INTEGRATION.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\RESOURCE_REPORT.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Classes\bhl"; ValueType: string; ValueName: ""; ValueData: "URL:Browser History Launcher"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\bhl"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\bhl\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\BrowserHistoryLauncher.exe"",0"
Root: HKCU; Subkey: "Software\Classes\bhl\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\BrowserHistoryLauncher.exe"" --listary-open ""%1"""

[Icons]
Name: "{group}\Listary浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; WorkingDir: "{app}"
Name: "{group}\配置 Listary 浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; Parameters: "--configure-listary"; WorkingDir: "{app}"
Name: "{userdesktop}\Listary浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\Listary浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; WorkingDir: "{app}"; Tasks: autostart

[Run]
Filename: "{app}\BrowserHistoryLauncher.exe"; Description: "立即启动 Listary浏览器插件"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\ListaryIntegrationState.ini"
Type: dirifempty; Name: "{app}\docs"
Type: dirifempty; Name: "{app}"

[Code]
var
  BrowserPage: TWizardPage;
  ChromeCheck: TNewCheckBox;
  ChromeKeywordLabel: TNewStaticText;
  ChromeKeywordEdit: TNewEdit;
  EdgeCheck: TNewCheckBox;
  EdgeKeywordLabel: TNewStaticText;
  EdgeKeywordEdit: TNewEdit;
  QuickListaryCheck: TNewCheckBox;
  ListaryStatusLabel: TNewStaticText;
  ListaryPreferencesPath: String;
  ListaryExePath: String;

function FindListaryExe: String;
var
  Candidate: String;
begin
  Candidate := ExpandConstant('{pf64}\Listary\Listary.exe');
  if FileExists(Candidate) then begin Result := Candidate; exit; end;
  Candidate := ExpandConstant('{pf32}\Listary\Listary.exe');
  if FileExists(Candidate) then begin Result := Candidate; exit; end;
  Candidate := ExpandConstant('{localappdata}\Programs\Listary\Listary.exe');
  if FileExists(Candidate) then begin Result := Candidate; exit; end;
  Result := '';
end;

function ChromeInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{pf64}\Google\Chrome\Application\chrome.exe')) or
    FileExists(ExpandConstant('{localappdata}\Google\Chrome\Application\chrome.exe'));
end;

function EdgeInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{pf32}\Microsoft\Edge\Application\msedge.exe')) or
    FileExists(ExpandConstant('{pf64}\Microsoft\Edge\Application\msedge.exe'));
end;

function IsValidKeyword(const Value: String): Boolean;
var
  I: Integer;
  Ch: Char;
begin
  Result := (Length(Value) >= 1) and (Length(Value) <= 16);
  if not Result then exit;
  for I := 1 to Length(Value) do begin
    Ch := Value[I];
    if not (((Ch >= 'a') and (Ch <= 'z')) or ((Ch >= 'A') and (Ch <= 'Z')) or
      ((Ch >= '0') and (Ch <= '9')) or (Ch = '-') or (Ch = '_')) then begin
      Result := False;
      exit;
    end;
  end;
end;

procedure InitializeWizard;
var
  Version: String;
begin
  BrowserPage := CreateCustomPage(wpSelectDir, '浏览器与 Listary',
    '选择需要接入的浏览器，并设置在 Listary 中输入的关键字。');

  ChromeCheck := TNewCheckBox.Create(BrowserPage);
  ChromeCheck.Parent := BrowserPage.Surface;
  ChromeCheck.Left := 0;
  ChromeCheck.Top := ScaleY(8);
  ChromeCheck.Width := ScaleX(250);
  ChromeCheck.Caption := 'Google Chrome';
  ChromeCheck.Checked := ChromeInstalled;

  ChromeKeywordLabel := TNewStaticText.Create(BrowserPage);
  ChromeKeywordLabel.Parent := BrowserPage.Surface;
  ChromeKeywordLabel.Left := ScaleX(270);
  ChromeKeywordLabel.Top := ScaleY(10);
  ChromeKeywordLabel.Caption := '关键字：';

  ChromeKeywordEdit := TNewEdit.Create(BrowserPage);
  ChromeKeywordEdit.Parent := BrowserPage.Surface;
  ChromeKeywordEdit.Left := ScaleX(330);
  ChromeKeywordEdit.Top := ScaleY(6);
  ChromeKeywordEdit.Width := ScaleX(90);
  ChromeKeywordEdit.Text := 'g';

  EdgeCheck := TNewCheckBox.Create(BrowserPage);
  EdgeCheck.Parent := BrowserPage.Surface;
  EdgeCheck.Left := 0;
  EdgeCheck.Top := ScaleY(45);
  EdgeCheck.Width := ScaleX(250);
  EdgeCheck.Caption := 'Microsoft Edge';
  EdgeCheck.Checked := EdgeInstalled;

  EdgeKeywordLabel := TNewStaticText.Create(BrowserPage);
  EdgeKeywordLabel.Parent := BrowserPage.Surface;
  EdgeKeywordLabel.Left := ScaleX(270);
  EdgeKeywordLabel.Top := ScaleY(47);
  EdgeKeywordLabel.Caption := '关键字：';

  EdgeKeywordEdit := TNewEdit.Create(BrowserPage);
  EdgeKeywordEdit.Parent := BrowserPage.Surface;
  EdgeKeywordEdit.Left := ScaleX(330);
  EdgeKeywordEdit.Top := ScaleY(43);
  EdgeKeywordEdit.Width := ScaleX(90);
  EdgeKeywordEdit.Text := 'e';

  ListaryPreferencesPath := ExpandConstant('{userappdata}\Listary\UserProfile\Settings\Preferences.json');
  if not FileExists(ListaryPreferencesPath) then
    ListaryPreferencesPath := ExpandConstant('{userappdata}\Listary\Preferences.json');
  ListaryExePath := FindListaryExe;

  QuickListaryCheck := TNewCheckBox.Create(BrowserPage);
  QuickListaryCheck.Parent := BrowserPage.Surface;
  QuickListaryCheck.Left := 0;
  QuickListaryCheck.Top := ScaleY(92);
  QuickListaryCheck.Width := BrowserPage.SurfaceWidth;
  QuickListaryCheck.Caption := '快速配置 Listary（修改前自动备份）';
  QuickListaryCheck.Enabled := FileExists(ListaryPreferencesPath);
  QuickListaryCheck.Checked := QuickListaryCheck.Enabled;

  ListaryStatusLabel := TNewStaticText.Create(BrowserPage);
  ListaryStatusLabel.Parent := BrowserPage.Surface;
  ListaryStatusLabel.Left := ScaleX(20);
  ListaryStatusLabel.Top := ScaleY(122);
  ListaryStatusLabel.Width := BrowserPage.SurfaceWidth - ScaleX(20);
  ListaryStatusLabel.WordWrap := True;
  if (ListaryExePath <> '') and GetVersionNumbersString(ListaryExePath, Version) then
    ListaryStatusLabel.Caption := '检测到 Listary ' + Version + '。如果配置结构兼容，将自动添加所选浏览器；否则安全回退为手动配置。'
  else if FileExists(ListaryPreferencesPath) then
    ListaryStatusLabel.Caption := '检测到 Listary 用户配置，但未识别程序版本，将使用结构兼容检测。'
  else
    ListaryStatusLabel.Caption := '未检测到 Listary 配置。安装仍可继续，之后可按照文档手动配置。';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID <> BrowserPage.ID then exit;
  if not ChromeCheck.Checked and not EdgeCheck.Checked then begin
    MsgBox('请至少选择一个浏览器。', mbError, MB_OK);
    Result := False;
    exit;
  end;
  if ChromeCheck.Checked and not IsValidKeyword(ChromeKeywordEdit.Text) then begin
    MsgBox('Chrome 关键字必须为 1-16 个英文字母、数字、短横线或下划线。', mbError, MB_OK);
    Result := False;
    exit;
  end;
  if EdgeCheck.Checked and not IsValidKeyword(EdgeKeywordEdit.Text) then begin
    MsgBox('Edge 关键字必须为 1-16 个英文字母、数字、短横线或下划线。', mbError, MB_OK);
    Result := False;
    exit;
  end;
  if ChromeCheck.Checked and EdgeCheck.Checked and
    (CompareText(ChromeKeywordEdit.Text, EdgeKeywordEdit.Text) = 0) then begin
    MsgBox('两个浏览器不能使用相同关键字。', mbError, MB_OK);
    Result := False;
  end;
end;

function BoolText(Value: Boolean): String;
begin
  if Value then Result := 'true' else Result := 'false';
end;

procedure ConfigureListary;
var
  ResultCode: Integer;
  Retry: Boolean;
begin
  if WizardSilent or not QuickListaryCheck.Checked then exit;
  repeat
    Retry := False;
    if not Exec(ExpandConstant('{app}\BrowserHistoryLauncher.exe'), '--configure-listary --quiet',
      ExpandConstant('{app}'), SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) then begin
      MsgBox('无法启动 Listary 快速配置助手，请按照安装目录中的文档手动配置。', mbError, MB_OK);
      exit;
    end;
    if ResultCode = 0 then begin
      if ListaryExePath <> '' then
        Exec(ListaryExePath, '--startup', ExtractFileDir(ListaryExePath), SW_SHOWNORMAL, ewNoWait, ResultCode);
      exit;
    end;
    if ResultCode = 6 then
      Retry := MsgBox('请从系统托盘完全退出 Listary，然后点击“重试”。选择“取消”将跳过快速配置。',
        mbConfirmation, MB_RETRYCANCEL) = IDRETRY
    else
      MsgBox('自动配置未完成。安装不会覆盖未知格式，请按照 docs\LISTARY_INTEGRATION.md 手动配置。', mbInformation, MB_OK);
  until not Retry;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  IniPath: String;
begin
  if CurStep <> ssPostInstall then exit;
  IniPath := ExpandConstant('{app}\BrowserHistoryLauncher.ini');
  SetIniString('browser.chrome', 'Enabled', BoolText(ChromeCheck.Checked), IniPath);
  SetIniString('browser.chrome', 'Prefix', Lowercase(ChromeKeywordEdit.Text), IniPath);
  SetIniString('browser.edge', 'Enabled', BoolText(EdgeCheck.Checked), IniPath);
  SetIniString('browser.edge', 'Prefix', Lowercase(EdgeKeywordEdit.Text), IniPath);
  ConfigureListary;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Retry: Boolean;
begin
  if CurUninstallStep <> usUninstall then exit;
  if FileExists(ExpandConstant('{app}\BrowserHistoryLauncher.exe')) then begin
    Exec(ExpandConstant('{app}\BrowserHistoryLauncher.exe'), '--exit', ExpandConstant('{app}'),
      SW_HIDE, ewWaitUntilTerminated, ResultCode);
    repeat
      Retry := False;
      Exec(ExpandConstant('{app}\BrowserHistoryLauncher.exe'), '--remove-listary-integration --quiet',
        ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
      if (ResultCode = 6) and not UninstallSilent then
        Retry := MsgBox('请从系统托盘完全退出 Listary，然后点击“重试”。选择“取消”将保留 Listary 中的网页搜索项。',
          mbConfirmation, MB_RETRYCANCEL) = IDRETRY;
    until not Retry;
  end;
end;
