#define PluginName "OBS LAN Preview"
#define PluginVersion "0.1.0"
#define PluginDll "..\release\windows-x64\obs-plugins\64bit\obs-lan-preview.dll"

[Setup]
AppId={{9EBA14EA-27E6-4C2C-90B6-D1E2D94CB271}
AppName={#PluginName}
AppVersion={#PluginVersion}
AppPublisher=obs-lan-preview
DefaultDirName={autopf}\obs-studio
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
