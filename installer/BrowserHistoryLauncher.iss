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
Name: "{group}\配置 Listary 浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; Parameters: "--settings"; WorkingDir: "{app}"
Name: "{userdesktop}\Listary浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\Listary浏览器插件"; Filename: "{app}\BrowserHistoryLauncher.exe"; WorkingDir: "{app}"; Tasks: autostart

[Run]
Filename: "{app}\BrowserHistoryLauncher.exe"; Parameters: "--settings"; Description: "启动并配置 Listary浏览器插件"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\ListaryIntegrationState.ini"
Type: dirifempty; Name: "{app}\docs"
Type: dirifempty; Name: "{app}"

[Code]
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
