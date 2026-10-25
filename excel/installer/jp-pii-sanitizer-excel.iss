; PII サニタイザ Excel アドインの per-user インストーラ（Inno Setup 6）。
;
; 方針（docs/excel-helper.md・excel/README.md）:
;   - 管理者権限なし。%LOCALAPPDATA%\jp-pii-sanitizer\ に置く（PrivilegesRequired=lowest）
;   - そのフォルダを Excel の「信頼できる場所」に HKCU で登録する（未署名 XLL のダイアログ回避）
;   - Excel の起動時読み込み（HKCU\...\Excel\Options の OPEN 値）に XLL を登録する
;   - アンインストールで両方の登録を外す。利用者の設定（excel-column-rules.json）は残す
;   - 未署名。ダウンロードした setup.exe には SmartScreen が出る（README.txt に案内）
;
; ビルド: excel/installer/build_installer.ps1 が同梱物を stage に集めてから
;   ISCC /DAppVersion=0.6.1 /DStageDir=... /DOutputDir=... jp-pii-sanitizer-excel.iss
; で呼ぶ。手で ISCC を叩くなら 3 つの define を渡すこと。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "PII サニタイザ Excel アドイン"
#define AppFolder "jp-pii-sanitizer"
#define XllName "jp-pii-sanitizer.xll"

[Setup]
AppId={{8D0C4C7E-3C2B-4E9B-9C1C-7F1E5A2B6D41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=jp-pii-sanitizer
AppPublisherURL=https://github.com/takaokawazoe/jp-pii-sanitizer
AppSupportURL=https://github.com/takaokawazoe/jp-pii-sanitizer/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\{#AppFolder}
DisableProgramGroupPage=yes
DisableWelcomePage=no
; 管理者権限を要求しない（会社 PC でも入る）。HKCU と %LOCALAPPDATA% にしか触らない
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Excel が XLL を掴んでいると上書きできないので、閉じてもらう（Restart Manager）
CloseApplications=yes
CloseApplicationsFilter=*.xll,*.exe,*.dll
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=jp-pii-sanitizer-excel-{#AppVersion}-setup
; モデル（fp16 ONNX 530 MB）は圧縮が効きにくい。lzma2/max でも時間ばかり掛かるので normal
Compression=lzma2/normal
SolidCompression=yes
LZMAUseSeparateProcess=yes
UninstallDisplayName={#AppName}
UninstallFilesDir={app}\uninstall
WizardStyle=modern
ShowLanguageDialog=auto

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
japanese.ExcelNotFound=この PC に Excel（Microsoft 365 / Office 2016 以降）が見つかりません。%nアドインは Excel がないと使えませんが、インストール自体は続けられます。続けますか？
english.ExcelNotFound=Excel (Microsoft 365 / Office 2016 or later) was not found on this PC.%nThe add-in needs Excel, but you can continue the installation. Continue?
japanese.Excel32=この PC の Office は 32 ビット版です。このアドインは 64 ビット版の Excel 専用です。%nインストールを中止します。
english.Excel32=The Office installed on this PC is 32-bit. This add-in requires 64-bit Excel.%nSetup will now exit.
japanese.Done=インストールが完了しました。次に Excel を起動すると、リボンに「PII」タブが現れます。%n%n対応表（実名を含むファイル）は AI に渡さないでください。詳しくはインストール先の README.txt を参照してください。
english.Done=Installation is complete. The next time Excel starts, a "PII" tab appears on the ribbon.%n%nNever hand the mapping file (it contains the real names) to an AI. See README.txt in the installation folder.

[Files]
Source: "{#StageDir}\{#XllName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\jp-pii-helper.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\onnxruntime_providers_shared.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs createallsubdirs

[Messages]
japanese.FinishedLabel=%1 のインストールが完了しました。
english.FinishedLabel=Setup has finished installing %1 on your computer.

[Code]
const
  ExcelRoot    = 'Software\Microsoft\Office\16.0\Excel';
  ExcelOptions = 'Software\Microsoft\Office\16.0\Excel\Options';
  AddinManager = 'Software\Microsoft\Office\16.0\Excel\Add-in Manager';
  TrustedRoot  = 'Software\Microsoft\Office\16.0\Excel\Security\Trusted Locations';
  TrustedDesc  = 'PII Sanitizer Excel add-in';

function XllPath: String;
begin
  Result := ExpandConstant('{app}\{#XllName}');
end;

function AppDirWithSlash: String;
begin
  Result := ExpandConstant('{app}\');
end;

// ---- Excel の起動時読み込み（OPEN, OPEN1, OPEN2, ... は連番でなければならない） ----

function OpenValueName(i: Integer): String;
begin
  if i = 0 then Result := 'OPEN' else Result := 'OPEN' + IntToStr(i);
end;

function IsOurEntry(const v: String): Boolean;
begin
  Result := Pos(Lowercase('\{#AppFolder}\{#XllName}'), Lowercase(v)) > 0;
end;

procedure ReadOpenList(var list: TArrayOfString);
var
  i, n: Integer;
  v: String;
begin
  SetArrayLength(list, 0);
  n := 0;
  i := 0;
  while RegQueryStringValue(HKCU, ExcelOptions, OpenValueName(i), v) do
  begin
    SetArrayLength(list, n + 1);
    list[n] := v;
    n := n + 1;
    i := i + 1;
  end;
end;

procedure WriteOpenList(const list: TArrayOfString);
var
  i: Integer;
begin
  // 既存の OPEN* を全部消してから詰め直す（欠番があると Excel はそこで読むのをやめる）
  i := 0;
  while RegValueExists(HKCU, ExcelOptions, OpenValueName(i)) do
  begin
    RegDeleteValue(HKCU, ExcelOptions, OpenValueName(i));
    i := i + 1;
  end;
  for i := 0 to GetArrayLength(list) - 1 do
    RegWriteStringValue(HKCU, ExcelOptions, OpenValueName(i), list[i]);
end;

procedure RegisterAddin;
var
  list: TArrayOfString;
  i, n: Integer;
  found: Boolean;
  entry: String;
begin
  entry := '/R "' + XllPath + '"';
  ReadOpenList(list);
  found := False;
  for i := 0 to GetArrayLength(list) - 1 do
    if IsOurEntry(list[i]) then
    begin
      list[i] := entry;
      found := True;
    end;
  if not found then
  begin
    n := GetArrayLength(list);
    SetArrayLength(list, n + 1);
    list[n] := entry;
  end;
  WriteOpenList(list);
  // 以前に「無効」にされていた記録があれば消す（Add-in Manager は未チェックの一覧）
  if RegValueExists(HKCU, AddinManager, XllPath) then
    RegDeleteValue(HKCU, AddinManager, XllPath);
end;

procedure UnregisterAddin;
var
  list, keep: TArrayOfString;
  i, n: Integer;
begin
  ReadOpenList(list);
  SetArrayLength(keep, 0);
  n := 0;
  for i := 0 to GetArrayLength(list) - 1 do
    if not IsOurEntry(list[i]) then
    begin
      SetArrayLength(keep, n + 1);
      keep[n] := list[i];
      n := n + 1;
    end;
  WriteOpenList(keep);
  if RegValueExists(HKCU, AddinManager, XllPath) then
    RegDeleteValue(HKCU, AddinManager, XllPath);
end;

// ---- 信頼できる場所 ----

// 自分の登録（Path がインストール先）があればその番号、無ければ空いている最初の番号を返す
function FindTrustedSlot(var exists: Boolean): Integer;
var
  i: Integer;
  key, p: String;
begin
  exists := False;
  for i := 0 to 999 do
  begin
    key := TrustedRoot + '\Location' + IntToStr(i);
    if not RegKeyExists(HKCU, key) then
    begin
      Result := i;
      Exit;
    end;
    if RegQueryStringValue(HKCU, key, 'Path', p) and (CompareText(p, AppDirWithSlash) = 0) then
    begin
      Result := i;
      exists := True;
      Exit;
    end;
  end;
  Result := 999;
end;

procedure AddTrustedLocation;
var
  i: Integer;
  exists: Boolean;
  key: String;
begin
  i := FindTrustedSlot(exists);
  key := TrustedRoot + '\Location' + IntToStr(i);
  RegWriteStringValue(HKCU, key, 'Path', AppDirWithSlash);
  RegWriteDWordValue(HKCU, key, 'AllowSubfolders', 1);
  RegWriteStringValue(HKCU, key, 'Description', TrustedDesc);
  RegWriteStringValue(HKCU, key, 'Date', GetDateTimeString('yyyy/mm/dd hh:nn', '/', ':'));
end;

procedure RemoveTrustedLocation;
var
  i: Integer;
  exists: Boolean;
begin
  i := FindTrustedSlot(exists);
  if exists then
    RegDeleteKeyIncludingSubkeys(HKCU, TrustedRoot + '\Location' + IntToStr(i));
end;

// ---- 事前検査 ----

function InitializeSetup: Boolean;
var
  platform: String;
begin
  Result := True;
  // 32 ビット Office には 64 ビット XLL は載らない
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Office\ClickToRun\Configuration', 'Platform', platform) then
    if CompareText(platform, 'x86') = 0 then
    begin
      MsgBox(CustomMessage('Excel32'), mbError, MB_OK);
      Result := False;
      Exit;
    end;
  if not RegKeyExists(HKCU, ExcelRoot) and not RegKeyExists(HKLM, 'SOFTWARE\Microsoft\Office\ClickToRun') then
    Result := MsgBox(CustomMessage('ExcelNotFound'), mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    AddTrustedLocation;
    RegisterAddin;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UnregisterAddin;
    RemoveTrustedLocation;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := CustomMessage('Done');
end;
