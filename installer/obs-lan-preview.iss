#define PluginName "LAN Preview"
#define PluginVersion "0.2.5"
#define PluginDll "..\release\windows-x64\obs-plugins\64bit\obs-lan-preview.dll"

[Setup]
AppId={{9EBA14EA-27E6-4C2C-90B6-D1E2D94CB271}
AppName={#PluginName}
AppVersion={#PluginVersion}
AppPublisher=maseaaao
DefaultDirName={code:GetObsStudioDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputBaseFilename=obs-lan-preview-{#PluginVersion}-windows-x64-installer
OutputDir=..\release\installer
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#PluginName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PluginDll}"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion
; Mirror the plugin into the other detected OBS installation when both the
; regular and Steam editions are installed. The selected {app} directory is
; still the primary target and remains user-editable in the wizard.
Source: "{#PluginDll}"; DestDir: "{code:GetRegularObsStudioDir}\obs-plugins\64bit"; Flags: ignoreversion; Check: ShouldInstallRegularObsCopy
Source: "..\README.md"; DestDir: "{code:GetRegularObsStudioDir}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion; Check: ShouldInstallRegularObsCopy
Source: "..\LICENSE"; DestDir: "{code:GetRegularObsStudioDir}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion; Check: ShouldInstallRegularObsCopy
Source: "{#PluginDll}"; DestDir: "{code:GetSteamObsStudioDir}\obs-plugins\64bit"; Flags: ignoreversion; Check: ShouldInstallSteamObsCopy
Source: "..\README.md"; DestDir: "{code:GetSteamObsStudioDir}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion; Check: ShouldInstallSteamObsCopy
Source: "..\LICENSE"; DestDir: "{code:GetSteamObsStudioDir}\data\obs-plugins\obs-lan-preview"; Flags: ignoreversion; Check: ShouldInstallSteamObsCopy

[Code]
const
  SteamObsAppId = '1905180';

var
  RegularObsDirChecked: Boolean;
  RegularObsDirFound: Boolean;
  RegularObsDirCache: String;
  SteamObsDirChecked: Boolean;
  SteamObsDirFound: Boolean;
  SteamObsDirCache: String;

function IsObsStudioDir(const Dir: String): Boolean;
begin
  Result := (Dir <> '') and
    FileExists(AddBackslash(Dir) + 'bin\64bit\obs64.exe');
end;

function NormalizePath(const Value: String): String;
begin
  Result := Trim(Value);
  StringChangeEx(Result, '/', '\', True);
  while (Length(Result) > 3) and
        (Result[Length(Result)] = '\') do
    Delete(Result, Length(Result), 1);
end;

function IsSamePath(const LeftPath, RightPath: String): Boolean;
begin
  Result := (NormalizePath(LeftPath) <> '') and
    (CompareText(NormalizePath(LeftPath), NormalizePath(RightPath)) = 0);
end;

function ExtractExecutablePath(const CommandLine: String): String;
var
  Value: String;
  QuotePosition: Integer;
  SpacePosition: Integer;
begin
  Value := Trim(CommandLine);
  Result := '';
  if Value = '' then
    Exit;

  if Value[1] = #34 then
  begin
    Delete(Value, 1, 1);
    QuotePosition := Pos(#34, Value);
    if QuotePosition > 0 then
      Value := Copy(Value, 1, QuotePosition - 1);
  end
  else
  begin
    SpacePosition := Pos(' ', Value);
    if SpacePosition > 0 then
      Value := Copy(Value, 1, SpacePosition - 1);
  end;

  Result := NormalizePath(Value);
end;

function GetQuotedVdfValue(const Line, Key: String; var Value: String): Boolean;
var
  KeyPosition: Integer;
  Remainder: String;
  FirstQuote: Integer;
  LastQuote: Integer;
begin
  Result := False;
  KeyPosition := Pos(#34 + Key + #34, Line);
  if KeyPosition = 0 then
    Exit;

  Remainder := Copy(Line, KeyPosition + Length(Key) + 2, MaxInt);
  FirstQuote := Pos(#34, Remainder);
  if FirstQuote = 0 then
    Exit;

  Remainder := Copy(Remainder, FirstQuote + 1, MaxInt);
  LastQuote := Pos(#34, Remainder);
  if LastQuote = 0 then
    Exit;

  Value := Copy(Remainder, 1, LastQuote - 1);
  StringChangeEx(Value, '\\', '\', True);
  StringChangeEx(Value, '\"', '"', True);
  Value := NormalizePath(Value);
  Result := True;
end;

function FindSteamObsInLibrary(const LibraryDir: String; var ObsDir: String): Boolean;
var
  ManifestPath: String;
  Manifest: TArrayOfString;
  I: Integer;
  InstallDir: String;
begin
  Result := False;
  ManifestPath := AddBackslash(LibraryDir) + 'steamapps\appmanifest_' + SteamObsAppId + '.acf';
  if not LoadStringsFromFile(ManifestPath, Manifest) then
    Exit;

  for I := 0 to GetArrayLength(Manifest) - 1 do
  begin
    if GetQuotedVdfValue(Manifest[I], 'installdir', InstallDir) and
       (InstallDir <> '') then
    begin
      ObsDir := AddBackslash(LibraryDir) + 'steamapps\common\' + InstallDir;
      if IsObsStudioDir(ObsDir) then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;
end;

function FindSteamObsInVdf(const VdfPath: String; var ObsDir: String): Boolean;
var
  Libraries: TArrayOfString;
  I: Integer;
  LibraryDir: String;
begin
  Result := False;
  if not LoadStringsFromFile(VdfPath, Libraries) then
    Exit;

  for I := 0 to GetArrayLength(Libraries) - 1 do
  begin
    if GetQuotedVdfValue(Libraries[I], 'path', LibraryDir) and
       FindSteamObsInLibrary(LibraryDir, ObsDir) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

function TryGetSteamRoot(var SteamDir: String): Boolean;
var
  SteamExe: String;
  Candidate: String;
begin
  Result := False;
  SteamDir := '';

  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', Candidate) and
     (NormalizePath(Candidate) <> '') then
  begin
    SteamDir := NormalizePath(Candidate);
    if DirExists(SteamDir) then
    begin
      Result := True;
      Exit;
    end;
  end;

  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamExe', SteamExe) then
  begin
    Candidate := ExtractFileDir(ExtractExecutablePath(SteamExe));
    if DirExists(Candidate) then
    begin
      SteamDir := NormalizePath(Candidate);
      Result := True;
      Exit;
    end;
  end;

  if RegQueryStringValue(HKLM32, 'Software\Valve\Steam', 'SteamPath', Candidate) and
     DirExists(NormalizePath(Candidate)) then
  begin
    SteamDir := NormalizePath(Candidate);
    Result := True;
    Exit;
  end;

  if RegQueryStringValue(HKLM64, 'Software\Valve\Steam', 'SteamPath', Candidate) and
     DirExists(NormalizePath(Candidate)) then
  begin
    SteamDir := NormalizePath(Candidate);
    Result := True;
    Exit;
  end;

  Candidate := ExpandConstant('{autopf32}\Steam');
  if DirExists(Candidate) then
  begin
    SteamDir := Candidate;
    Result := True;
    Exit;
  end;

  Candidate := ExpandConstant('{autopf64}\Steam');
  if DirExists(Candidate) then
  begin
    SteamDir := Candidate;
    Result := True;
  end;
end;

function TryGetSteamObsStudioDir(var ObsDir: String): Boolean;
var
  SteamDir: String;
  LibrariesPath: String;
begin
  if SteamObsDirChecked then
  begin
    ObsDir := SteamObsDirCache;
    Result := SteamObsDirFound;
    Exit;
  end;

  SteamObsDirChecked := True;
  SteamObsDirFound := False;
  SteamObsDirCache := '';
  ObsDir := '';

  if not TryGetSteamRoot(SteamDir) then
    Exit;

  if FindSteamObsInLibrary(SteamDir, ObsDir) then
  begin
    SteamObsDirCache := NormalizePath(ObsDir);
    SteamObsDirFound := True;
    Result := True;
    Exit;
  end;

  LibrariesPath := AddBackslash(SteamDir) + 'steamapps\libraryfolders.vdf';
  if FindSteamObsInVdf(LibrariesPath, ObsDir) then
  begin
    SteamObsDirCache := NormalizePath(ObsDir);
    SteamObsDirFound := True;
    Result := True;
    Exit;
  end;

  LibrariesPath := AddBackslash(SteamDir) + 'config\libraryfolders.vdf';
  if FindSteamObsInVdf(LibrariesPath, ObsDir) then
  begin
    SteamObsDirCache := NormalizePath(ObsDir);
    SteamObsDirFound := True;
    Result := True;
  end;
end;

function TryGetRegularObsFromRegistry(RootKey: Integer; var ObsDir: String): Boolean;
var
  InstallLocation: String;
  UninstallString: String;
  UninstallExe: String;
begin
  Result := False;

  if RegQueryStringValue(RootKey,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
       'InstallLocation', InstallLocation) then
  begin
    ObsDir := NormalizePath(InstallLocation);
    if IsObsStudioDir(ObsDir) then
    begin
      Result := True;
      Exit;
    end;
  end;

  if RegQueryStringValue(RootKey,
       'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
       'UninstallString', UninstallString) then
  begin
    UninstallExe := ExtractExecutablePath(UninstallString);
    ObsDir := NormalizePath(ExtractFileDir(UninstallExe));
    Result := IsObsStudioDir(ObsDir);
  end;
end;

function TryGetRegularObsStudioDir(var ObsDir: String): Boolean;
var
  Candidate: String;
begin
  if RegularObsDirChecked then
  begin
    ObsDir := RegularObsDirCache;
    Result := RegularObsDirFound;
    Exit;
  end;

  RegularObsDirChecked := True;
  RegularObsDirFound := False;
  RegularObsDirCache := '';
  ObsDir := '';

  if TryGetRegularObsFromRegistry(HKLM64, ObsDir) or
     TryGetRegularObsFromRegistry(HKLM32, ObsDir) or
     TryGetRegularObsFromRegistry(HKCU, ObsDir) then
  begin
    RegularObsDirCache := NormalizePath(ObsDir);
    RegularObsDirFound := True;
    Result := True;
    Exit;
  end;

  Candidate := ExpandConstant('{autopf}\obs-studio');
  if IsObsStudioDir(Candidate) then
  begin
    RegularObsDirCache := NormalizePath(Candidate);
    RegularObsDirFound := True;
    ObsDir := RegularObsDirCache;
    Result := True;
  end;
end;

function GetSteamObsStudioDir(Param: String): String;
var
  ObsDir: String;
begin
  if TryGetSteamObsStudioDir(ObsDir) then
    Result := ObsDir
  else
    Result := '';
end;

function GetRegularObsStudioDir(Param: String): String;
var
  ObsDir: String;
begin
  if TryGetRegularObsStudioDir(ObsDir) then
    Result := ObsDir
  else
    Result := '';
end;

function IsAutoDetectedObsDir: Boolean;
var
  ObsDir: String;
begin
  Result := False;

  if TryGetRegularObsStudioDir(ObsDir) and
     IsSamePath(ObsDir, ExpandConstant('{app}')) then
  begin
    Result := True;
    Exit;
  end;

  if TryGetSteamObsStudioDir(ObsDir) and
     IsSamePath(ObsDir, ExpandConstant('{app}')) then
    Result := True;
end;

function ShouldInstallSteamObsCopy: Boolean;
var
  ObsDir: String;
begin
  Result := IsAutoDetectedObsDir and
    TryGetSteamObsStudioDir(ObsDir) and
    (not IsSamePath(ObsDir, ExpandConstant('{app}')));
end;

function ShouldInstallRegularObsCopy: Boolean;
var
  ObsDir: String;
begin
  Result := IsAutoDetectedObsDir and
    TryGetRegularObsStudioDir(ObsDir) and
    (not IsSamePath(ObsDir, ExpandConstant('{app}')));
end;

function GetObsStudioDir(Param: String): String;
var
  ObsDir: String;
begin
  if TryGetRegularObsStudioDir(ObsDir) then
  begin
    Result := ObsDir;
    Exit;
  end;

  if TryGetSteamObsStudioDir(ObsDir) then
  begin
    Result := ObsDir;
    Exit;
  end;

  Result := ExpandConstant('{autopf}\obs-studio');
end;












