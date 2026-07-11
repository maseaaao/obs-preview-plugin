#define PluginName "LAN Preview"
#define PluginVersion "0.2.3"
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

[Code]
const
  SteamObsAppId = '1905180';

function IsObsStudioDir(const Dir: String): Boolean;
begin
  Result := FileExists(AddBackslash(Dir) + 'bin\64bit\obs64.exe');
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
    if GetQuotedVdfValue(Manifest[I], 'installdir', InstallDir) then
    begin
      ObsDir := AddBackslash(LibraryDir) + 'steamapps\common\' + InstallDir;
      Result := IsObsStudioDir(ObsDir);
      Exit;
    end;
  end;
end;

function GetSteamObsStudioDir(var ObsDir: String): Boolean;
var
  SteamDir: String;
  LibrariesPath: String;
  Libraries: TArrayOfString;
  I: Integer;
  LibraryDir: String;
begin
  Result := False;
  if not RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamDir) then
    Exit;

  if FindSteamObsInLibrary(SteamDir, ObsDir) then
  begin
    Result := True;
    Exit;
  end;

  LibrariesPath := AddBackslash(SteamDir) + 'steamapps\libraryfolders.vdf';
  if not LoadStringsFromFile(LibrariesPath, Libraries) then
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

function GetObsStudioDir(Param: String): String;
var
  UninstallString: String;
begin
  if RegQueryStringValue(HKLM64, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'UninstallString', UninstallString) then
  begin
    Result := ExtractFileDir(RemoveQuotes(UninstallString));
    Exit;
  end;

  if RegQueryStringValue(HKLM32, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'UninstallString', UninstallString) then
  begin
    Result := ExtractFileDir(RemoveQuotes(UninstallString));
    Exit;
  end;

  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'UninstallString', UninstallString) then
  begin
    Result := ExtractFileDir(RemoveQuotes(UninstallString));
    Exit;
  end;

  if GetSteamObsStudioDir(Result) then
    Exit;

  Result := ExpandConstant('{autopf}\obs-studio');
end;










