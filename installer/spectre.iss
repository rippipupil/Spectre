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
; 2004 (19041): ocultar el panel de las capturas y el audio por proceso lo necesitan.
MinVersion=10.0.19041
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Solo en la primera instalación: al actualizar manda lo que el usuario eligió en la app.
Name: "autostart"; Description: "Iniciar Spectre con Windows"; GroupDescription: "Opciones:"; Check: not IsUpgrade
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Opciones:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Spectre"; Filename: "{app}\spectre.exe"
Name: "{group}\Desinstalar Spectre"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Spectre"; Filename: "{app}\spectre.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Spectre"; ValueData: """{app}\spectre.exe"" --tray"; Flags: uninsdeletevalue; Tasks: autostart
; La app también puede activar el inicio con Windows: al desinstalar se borra siempre.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Spectre"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\spectre.exe"; Description: "Abrir Spectre"; Flags: nowait postinstall skipifsilent
; Actualización lanzada desde Spectre (Ajustes > General): se instala en silencio
; y la app vuelve a abrirse sola al terminar.
Filename: "{app}\spectre.exe"; Parameters: "--updated"; Flags: nowait; Check: RelaunchAfterUpdate

[UninstallRun]
Filename: "{cmd}"; Parameters: "/C taskkill /IM spectre.exe /F"; Flags: runhidden; RunOnceId: "KillSpectre"

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\Spectre"

[Code]
function IsUpgrade(): Boolean;
begin
  Result := RegValueExists(HKCU, ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1'), 'UninstallString');
end;

var
  InstallFinished: Boolean;

// Spectre lanza el instalador con /SILENT /RELAUNCH=1 al actualizarse desde la app.
function RelaunchAfterUpdate(): Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:relaunch|0}') = '1');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then InstallFinished := True;
end;

// Si la actualización silenciosa falla o se cancela, [Run] no llega a
// ejecutarse: se vuelve a abrir la versión que haya quedado instalada para
// que el usuario no se quede sin Spectre.
procedure DeinitializeSetup();
var
  Dir: String;
  Code: Integer;
begin
  if RelaunchAfterUpdate() and not InstallFinished and
     RegQueryStringValue(HKCU, ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1'), 'InstallLocation', Dir) and
     FileExists(AddBackslash(Dir) + 'spectre.exe') then
    Exec(AddBackslash(Dir) + 'spectre.exe', '', Dir, SW_SHOWNORMAL, ewNoWait, Code);
end;

// Las ediciones N/KN de Windows no traen Media Foundation. Spectre funciona
// igual, pero la webcam no; se avisa en vez de fallar en silencio.
function InitializeSetup(): Boolean;
begin
  Result := True;
  // En una actualización silenciosa no se pregunta nada (ya se avisó al instalar).
  if not WizardSilent and not FileExists(ExpandConstant('{sys}\mfplat.dll')) then
    MsgBox('Tu Windows no incluye Media Foundation (ediciones N). Spectre funcionará, pero sin webcam. Para usarla, instala el "Paquete de características multimedia" desde Configuración > Aplicaciones > Características opcionales.', mbInformation, MB_OK);
end;
