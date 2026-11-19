; DockStation Operator per-machine Windows installer.
#ifndef AppVersion
  #define AppVersion "0.1.0-dev"
#endif

#define AppName "DockStation Operator"
#define AppPublisher "DockStation"

[Setup]
AppId={{B4D37F6C-0A42-4A76-9C0E-5EED2C7A68B1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\DockStation Operator
DefaultGroupName=DockStation Operator
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\DockStationOperator.exe
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
OutputDir=..\..\artifacts
OutputBaseFilename=DockStationOperator-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes

[Files]
Source: "..\..\dist\DockStationOperator\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\DockStation Operator"; Filename: "{app}\DockStationOperator.exe"
Name: "{autodesktop}\DockStation Operator"; Filename: "{app}\DockStationOperator.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Run]
Filename: "{app}\DockStationOperator.exe"; Description: "Launch DockStation Operator"; Flags: postinstall nowait skipifsilent
