; ac0oculus.iss -- the Quest installer, Inno Setup 6 (ac0oculus, 2026-11-17). Built by tools/setup/quest/make_release_quest.sh:
;   ISCC.exe /DVERSION=0.1.0 /DSTAGE=<OUT\stage> /O<OUT> ac0oculus.iss
; Per user, no administrator. Its own AppId and folder: never ac0vr's. The disc is referenced, never copied. The setup holds nothing of the
; game (make_release_quest.sh scans the stage); it builds the Quest app on this PC from the player's disc with
; tools\quest_player_build.py, which takes the Android NDK from this PC or, after the player accepts Google's licence on the page
; below, from Google. Silent installs (tests): /DISC=<iso> /NDK=<folder> /ACCEPTANDROIDLICENSE=1 /KEY=<key file> /NOBUILD=1.

#ifndef VERSION
  #define VERSION "0.0.0"
#endif
#ifndef STAGE
  #define STAGE "stage"
#endif

[Setup]
AppId=ac0oculus-3d51c9a4
AppName=ac0oculus
AppVersion={#VERSION}
AppVerName=ac0oculus {#VERSION}
AppPublisher=ac0oculus
DefaultDirName={localappdata}\Programs\ac0oculus
DefaultGroupName=ac0oculus
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=ac0oculus-{#VERSION}-setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
UninstallDisplayName=ac0oculus {#VERSION}
SetupLogging=yes
UsePreviousAppDir=yes

[Files]
Source: "{#STAGE}\android-sdk-license.txt"; Flags: dontcopy
Source: "{#STAGE}\*"; DestDir: "{app}"; Excludes: "\headset_textures,\android-sdk-license.txt"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#STAGE}\android-sdk-license.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
; our own controller glyphs, then the assets folder beside the setup when there is one (cockpits_q4\, cockpits_ql_t9\, textures\): laid out
; as /sdcard/ac0oculus/ on the headset, copied in (newer files only), left where it is
Source: "{#STAGE}\headset_textures\*"; DestDir: "{app}\headset\textures"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{src}\ac0oculus-assets\*"; DestDir: "{app}\headset"; Flags: external recursesubdirs createallsubdirs skipifsourcedoesntexist comparetimestamp

[Icons]
Name: "{group}\ac0oculus - copy to the Quest"; Filename: "{app}\python\python.exe"; Parameters: "-I -B ""{app}\tools\push_to_quest.py"" --app ""{app}"""; WorkingDir: "{app}"
Name: "{group}\ac0oculus - build the APK again"; Filename: "{app}\python\python.exe"; Parameters: "-I -B ""{app}\tools\quest_player_build.py"" --app ""{app}"" --log ""{app}\build.log"""; WorkingDir: "{app}"
Name: "{group}\ac0oculus folder"; Filename: "{app}"
Name: "{group}\Uninstall ac0oculus"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\python\python.exe"; Parameters: "-I -B ""{app}\tools\push_to_quest.py"" --app ""{app}"""; WorkingDir: "{app}"; Description: "Copy it to the Quest now (USB cable, developer mode)"; Flags: postinstall unchecked skipifsilent; Check: Built

[UninstallDelete]
Type: filesandordirs; Name: "{app}\build"
Type: filesandordirs; Name: "{app}\headset"
Type: filesandordirs; Name: "{app}\toolchain"
Type: files; Name: "{app}\ac0oculus.apk"
Type: files; Name: "{app}\ac0oculus.json"
Type: files; Name: "{app}\build.log"
Type: filesandordirs; Name: "{app}\recomp"
Type: filesandordirs; Name: "{app}\tools"
Type: dirifempty; Name: "{app}"

[Code]
const
  SLUS_SHA256 = '34ee58ca92a6e8913aa84b6dc5af3534b878a44ca4e33173e962c6090ea243c1';
  NDK_VERSION = '29.0.14206865';

var
  DiscPage: TInputFileWizardPage;
  DiscMessage: TNewStaticText;
  LicensePage: TOutputMsgMemoWizardPage;
  LicenseAccept: TNewCheckBox;
  FoundNdk: String;
  BuildRc: Integer;
  BuildWords: String;

{ ---- the disc: the US executable inside the image (ISO 9660), by its SHA-256; or a folder that holds it }
function U32(const S: AnsiString; P: Integer): Cardinal;
begin
  Result := Ord(S[P]) or (Ord(S[P + 1]) shl 8) or (Ord(S[P + 2]) shl 16) or (Ord(S[P + 3]) shl 24);
end;

function ReadAt(F: TFileStream; Pos: Int64; Count: Integer): AnsiString;
begin
  SetLength(Result, Count);
  F.Seek(Pos, soFromBeginning);
  F.ReadBuffer(Result, Count);
end;

function CheckDisc(const Path: String; var Words: String): Boolean;
var
  F: TFileStream;
  Pvd, Dir, Elf, Id: AnsiString;
  Lba, Size, Ext, Len: Cardinal;
  P, N, L: Integer;
begin
  Result := False;
  if Path = '' then begin Words := 'The game cannot be built without your disc.'; Exit; end;
  if DirExists(Path) then
  begin
    if not FileExists(AddBackslash(Path) + 'SLUS_213.46') then
      Words := 'This folder has no SLUS_213.46: it is not the US disc of Ace Combat Zero (SLUS-21346).'
    else if GetSHA256OfFile(AddBackslash(Path) + 'SLUS_213.46') <> SLUS_SHA256 then
      Words := 'This folder holds another release of the game: only the US disc (SLUS-21346) works.'
    else begin Words := 'Your US disc: found.'; Result := True; end;
    Exit;
  end;
  if not FileExists(Path) then begin Words := 'There is no such file.'; Exit; end;
  try
    F := TFileStream.Create(Path, fmOpenRead or fmShareDenyWrite);
    try
      Pvd := ReadAt(F, 16 * 2048, 2048);
      if Copy(Pvd, 2, 5) <> 'CD001' then begin Words := 'This is not an ISO disc image.'; Exit; end;
      Lba := U32(Pvd, 157 + 2);
      Size := U32(Pvd, 157 + 10);
      if Size > 1048576 then begin Words := 'This disc image has an unexpected layout.'; Exit; end;
      Dir := ReadAt(F, Int64(Lba) * 2048, Size);
      P := 1;
      while P <= Length(Dir) do
      begin
        N := Ord(Dir[P]);
        if N = 0 then begin P := ((P - 1) div 2048 + 1) * 2048 + 1; continue; end;
        L := Ord(Dir[P + 32]);
        Id := Copy(Dir, P + 33, L);
        if Pos('SLUS_213.46', Uppercase(Id)) = 1 then
        begin
          Ext := U32(Dir, P + 2);
          Len := U32(Dir, P + 10);
          Elf := ReadAt(F, Int64(Ext) * 2048, Len);
          if GetSHA256OfString(Elf) = SLUS_SHA256 then
          begin
            Words := 'Your US disc: checked.';
            Result := True;
          end else
            Words := 'This image holds another release of the game: only the US disc (SLUS-21346) works.';
          Exit;
        end;
        P := P + N;
      end;
      Words := 'This image has no SLUS_213.46: it is not the US disc of Ace Combat Zero (SLUS-21346).';
    finally
      F.Free;
    end;
  except
    Words := 'The image could not be read: ' + GetExceptionMessage;
  end;
end;

function FindDiscBesideSetup: String;
var
  F: TFindRec;
  Dir, Words: String;
begin
  Result := '';
  Dir := ExpandConstant('{src}');
  if FindFirst(Dir + '\*.iso', F) then
  try
    repeat
      if CheckDisc(Dir + '\' + F.Name, Words) then begin Result := Dir + '\' + F.Name; Exit; end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

{ ---- the Android NDK r29 already on this PC (Android Studio's folder, ANDROID_NDK_ROOT, /NDK=) }
function NdkOk(const D: String): Boolean;
var
  Props: AnsiString;
begin
  Result := (D <> '') and LoadStringFromFile(AddBackslash(D) + 'source.properties', Props) and
            (Pos('Pkg.Revision = ' + NDK_VERSION, Props) > 0) and
            FileExists(AddBackslash(D) + 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe');
end;

function FindNdk: String;
begin
  Result := ExpandConstant('{param:NDK}');
  if NdkOk(Result) then Exit;
  Result := GetEnv('ANDROID_NDK_ROOT');
  if NdkOk(Result) then Exit;
  Result := ExpandConstant('{localappdata}\Android\Sdk\ndk\' + NDK_VERSION);
  if NdkOk(Result) then Exit;
  Result := ExpandConstant('{app}\toolchain\android-ndk-r29');
  if NdkOk(Result) then Exit;
  Result := '';
end;

{ a test hook: /CHECKDISCONLY=<path> /CHECKOUT=<file> runs the disc page's check, writes "ok|refused: words" and ends the setup at
  once (nothing installed) }
function InitializeSetup: Boolean;
var
  Words: String;
begin
  Result := True;
  if ExpandConstant('{param:CHECKDISCONLY}') <> '' then
  begin
    if CheckDisc(ExpandConstant('{param:CHECKDISCONLY}'), Words) then Words := 'ok: ' + Words else Words := 'refused: ' + Words;
    SaveStringToFile(ExpandConstant('{param:CHECKOUT}'), Words + #13#10, False);
    Result := False;
  end;
end;

function AssetsBeside: Boolean;
begin
  Result := DirExists(ExpandConstant('{src}\ac0oculus-assets'));
end;

procedure LicenseClick(Sender: TObject);
begin
  WizardForm.NextButton.Enabled := LicenseAccept.Checked;
end;

procedure InitializeWizard;
var
  Text: AnsiString;
begin
  DiscPage := CreateInputFilePage(wpSelectDir, 'Your disc',
    'This program holds no game. It builds the game for the Quest from your own US disc of Ace Combat Zero: The Belkan War (SLUS-21346).',
    'An ISO image of the disc, or a folder with its files. It is read, never copied; the copy to the Quest takes it from where it is.');
  DiscPage.Add('The disc image:', 'Disc image (*.iso)|*.iso|All files|*.*', '.iso');
  DiscMessage := TNewStaticText.Create(DiscPage);
  DiscMessage.Parent := DiscPage.Surface;
  DiscMessage.Left := DiscPage.Edits[0].Left;
  DiscMessage.Top := DiscPage.Edits[0].Top + DiscPage.Edits[0].Height + ScaleY(16);
  DiscMessage.Width := DiscPage.SurfaceWidth - DiscMessage.Left;
  DiscMessage.Height := ScaleY(80);
  DiscMessage.AutoSize := False;
  DiscMessage.WordWrap := True;
  ExtractTemporaryFile('android-sdk-license.txt');
  LoadStringFromFile(ExpandConstant('{tmp}\android-sdk-license.txt'), Text);
  LicensePage := CreateOutputMsgMemoPage(DiscPage.ID, 'The Android NDK (Google)',
    'The build needs Google''s Android NDK r29 (the compiler for the Quest). It is not on this PC, so the setup downloads it from Google (834 MB, checked) under Google''s licence below; the copy to the Quest later also downloads Google''s adb under it.',
    'Please read the Android Software Development Kit License Agreement:', Text);
  LicenseAccept := TNewCheckBox.Create(LicensePage);
  LicenseAccept.Parent := LicensePage.Surface;
  LicenseAccept.Caption := 'I accept the Android SDK License Agreement (the download of the NDK and of adb)';
  LicenseAccept.Top := LicensePage.RichEditViewer.Top + LicensePage.RichEditViewer.Height + ScaleY(8);
  LicenseAccept.Left := LicensePage.RichEditViewer.Left;
  LicenseAccept.Width := LicensePage.SurfaceWidth;
  LicenseAccept.OnClick := @LicenseClick;
  LicensePage.RichEditViewer.Height := LicensePage.RichEditViewer.Height - ScaleY(24);
  LicenseAccept.Top := LicensePage.RichEditViewer.Top + LicensePage.RichEditViewer.Height + ScaleY(6);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = LicensePage.ID then
  begin
    FoundNdk := FindNdk;
    Result := FoundNdk <> '';
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Words: String;
begin
  Result := True;
  { a silent install (/DISC=) leaves the disc to the build's own check, which writes its reason into build.log }
  if (CurPageID = DiscPage.ID) and not WizardSilent then
  begin
    Result := CheckDisc(DiscPage.Values[0], Words);
    DiscMessage.Caption := Words;
  end;
end;

function DiscPath: String;
begin
  if WizardSilent then Result := ExpandConstant('{param:DISC}') else Result := DiscPage.Values[0];
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Assets, Ndk: String;
begin
  if AssetsBeside then
    Assets := 'Assets (cockpits, textures), copied in for the headset:' + NewLine + Space + ExpandConstant('{src}\ac0oculus-assets')
  else
    Assets := 'No ac0oculus-assets folder beside the setup: the game will fly from its own views (no 3D cockpits).';
  if FoundNdk <> '' then Ndk := 'Android NDK r29, found on this PC:' + NewLine + Space + FoundNdk
  else Ndk := 'Android NDK r29: downloaded from Google during the install (834 MB).';
  Result := MemoDirInfo + NewLine + NewLine + 'Your disc:' + NewLine + Space + DiscPath + NewLine + NewLine + Assets + NewLine + NewLine + Ndk;
end;

function Built: Boolean;
begin
  Result := (BuildRc = 0) and FileExists(ExpandConstant('{app}\ac0oculus.apk'));
end;

{ the last "ac0oculus: ERROR" line of the build's log: the reason, in the build's own words }
function BuildError: String;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if LoadStringsFromFile(ExpandConstant('{app}\build.log'), Lines) then
    for I := 0 to GetArrayLength(Lines) - 1 do
      if Pos('ac0oculus: ERROR', Lines[I]) = 1 then Result := Lines[I];
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Params, Accept, Key: String;
  Show: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    BuildRc := -1;
    if ExpandConstant('{param:NOBUILD}') = '1' then Exit;
    Accept := '';
    if (not WizardSilent and LicenseAccept.Checked) or (ExpandConstant('{param:ACCEPTANDROIDLICENSE}') = '1') then
      Accept := ' --accept-android-sdk-license';
    Params := '-I -B "' + ExpandConstant('{app}\tools\quest_player_build.py') + '" --app "' + ExpandConstant('{app}') + '" --disc "' +
              RemoveBackslash(DiscPath) + '" --log "' + ExpandConstant('{app}\build.log') + '"' + Accept;
    if FindNdk <> '' then Params := Params + ' --ndk "' + FindNdk + '"';
    Key := ExpandConstant('{param:KEY}');
    if Key <> '' then Params := Params + ' --key "' + Key + '"';
    if WizardSilent then Show := SW_HIDE else Show := SW_SHOWNORMAL;
    WizardForm.StatusLabel.Caption := 'Building the Quest app from your disc (a few minutes; the console window shows each step)...';
    if not Exec(ExpandConstant('{app}\python\python.exe'), Params, ExpandConstant('{app}'), Show, ewWaitUntilTerminated, BuildRc) then
      BuildRc := -2;
    Log('the build returned ' + IntToStr(BuildRc));
    if BuildRc <> 0 then
    begin
      BuildWords := BuildError;
      if BuildWords = '' then BuildWords := 'The build did not finish (see build.log in the program''s folder).';
      SuppressibleMsgBox('The Quest app could not be built:' + #13#10#13#10 + BuildWords + #13#10#13#10 +
        'The program is installed; after fixing this, start "ac0oculus - build the APK again" from the Start menu.', mbError, MB_OK, IDOK);
    end;
  end;
end;

function FinishWords: String;
begin
  if Built then
    Result := 'The Quest app is built: ' + ExpandConstant('{app}\ac0oculus.apk') + #13#10#13#10 +
      'Next: put the Quest 3 in developer mode, connect it with a USB-C cable, and start "ac0oculus - copy to the Quest" from the Start menu (or tick it below). It installs the app and copies your disc image and the data (about 20 seconds over USB 3).'
  else
    Result := 'The program is installed, but the Quest app is not built yet:' + #13#10 + BuildWords;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = DiscPage.ID) and (DiscPage.Values[0] = '') then
    DiscPage.Values[0] := FindDiscBesideSetup;
  if CurPageID = LicensePage.ID then
    WizardForm.NextButton.Enabled := LicenseAccept.Checked;
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption := FinishWords;
end;
