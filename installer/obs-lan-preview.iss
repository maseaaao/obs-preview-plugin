#define PluginName "OBS LAN Preview"
#define PluginVersion "0.1.4"
#define PluginDll "..\release\windows-x64\obs-plugins\64bit\obs-lan-preview.dll"

[Setup]
AppId={{9EBA14EA-27E6-4C2C-90B6-D1E2D94CB271}
AppName={#PluginName}
AppVersion={#PluginVersion}
AppPublisher=obs-lan-preview
DefaultDirName={code:GetObsStudioDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
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

[Code]
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

  Result := ExpandConstant('{autopf}\obs-studio');
end;




