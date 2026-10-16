; Instalador de Spectre (Inno Setup 6). Instala por usuario, sin pedir administrador.
; Compilar: iscc /DAppVersion=0.1.0 /DSourceDir=..\dist spectre.iss

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif

[Setup]
AppId={{6C4F6F2B-5B0E-4E0B-9C4E-5350454354524D}
AppName=Spectre
AppVersion={#AppVersion}
AppPublisher=Spectre
DefaultDirName={localappdata}\Programs\Spectre
DefaultGroupName=Spectre
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=output
OutputBaseFilename=Spectre-Setup
SetupIconFile=..\res\spectre.ico
UninstallDisplayIcon={app}\spectre.exe
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Iniciar Spectre con Windows"; GroupDescription: "Opciones:"
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Opciones:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Spectre"; Filename: "{app}\spectre.exe"
Name: "{group}\Desinstalar Spectre"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Spectre"; Filename: "{app}\spectre.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Spectre"; ValueData: """{app}\spectre.exe"" --tray"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\spectre.exe"; Description: "Abrir Spectre"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /IM spectre.exe /F"; Flags: runhidden; RunOnceId: "KillSpectre"

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\Spectre"

[Code]
// Las ediciones N/KN de Windows no traen Media Foundation (cámara y
// encoder de respaldo). Se avisa en vez de fallar en silencio.
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not FileExists(ExpandConstant('{sys}\mfplat.dll')) then
    MsgBox('Tu Windows no tiene Media Foundation (ediciones N). Instala el "Media Feature Pack" desde Configuración > Aplicaciones > Características opcionales para que la cámara funcione.', mbInformation, MB_OK);
end;
