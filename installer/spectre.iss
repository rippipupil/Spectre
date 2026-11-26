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
; Archivos que una actualización apartó porque seguían en uso (ver MoveAsideInUseFiles).
Type: files; Name: "{app}\*.old"

[Code]
function IsUpgrade(): Boolean;
begin
  Result := RegValueExists(HKCU, ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1'), 'UninstallString');
end;

var
  InstallFinished: Boolean;

// Spectre lanza el instalador con /SILENT o /VERYSILENT y /RELAUNCH=1 al actualizarse desde la app.
function RelaunchAfterUpdate(): Boolean;
begin
  Result := WizardSilent and (ExpandConstant('{param:relaunch|0}') = '1');
end;

const
  SpectreMutex = 'Local\SpectreSingleInstance';

// Espera como mucho Ms milisegundos a que ningún Spectre tenga su mutex de
// instancia única, que suelta al terminar del todo.
function WaitSpectreGone(Ms: Integer): Boolean;
var
  Waited: Integer;
begin
  Waited := 0;
  while CheckForMutexes(SpectreMutex) and (Waited < Ms) do begin
    Sleep(200);
    Waited := Waited + 200;
  end;
  Result := not CheckForMutexes(SpectreMutex);
  if Result and (Waited > 0) then
    Log(Format('Spectre termino de cerrarse en %d ms', [Waited]));
end;

var
  MovedDir: String;
  Moved: TArrayOfString; // lo que MoveAsideInUseFiles apartó

// Último recurso si Spectre no termina ni a la fuerza (un hilo atascado
// dentro del driver de la webcam): Windows deja renombrar un exe o una DLL en
// uso, aunque no reemplazarlos. Se apartan como .old, el instalador copia los
// nuevos en su sitio y Spectre borra los .old al abrirse (src/main.cpp).
procedure MoveAsideInUseFiles();
var
  Dir: String;
  Names: TArrayOfString;
  FindRec: TFindRec;
  I: Integer;
begin
  if not RegQueryStringValue(HKCU, ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1'), 'InstallLocation', Dir) then
    Exit;
  Dir := AddBackslash(Dir);
  SetArrayLength(Names, 1);
  Names[0] := 'spectre.exe';
  if FindFirst(Dir + '*.dll', FindRec) then begin
    try
      repeat
        if CompareText(ExtractFileExt(FindRec.Name), '.dll') = 0 then begin
          SetArrayLength(Names, GetArrayLength(Names) + 1);
          Names[GetArrayLength(Names) - 1] := FindRec.Name;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  for I := 0 to GetArrayLength(Names) - 1 do
    if FileExists(Dir + Names[I]) then begin
      DeleteFile(Dir + Names[I] + '.old');
      if RenameFile(Dir + Names[I], Dir + Names[I] + '.old') then begin
        Log('Apartado ' + Names[I] + ' (sigue en uso)');
        SetArrayLength(Moved, GetArrayLength(Moved) + 1);
        Moved[GetArrayLength(Moved) - 1] := Names[I];
      end else
        Log('No se pudo apartar ' + Names[I]);
    end;
  MovedDir := Dir;
end;

// La instalación no terminó: lo apartado que no tenga ya un archivo nuevo en
// su sitio vuelve a su nombre, para que la versión anterior se pueda abrir.
procedure RestoreMovedFiles();
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(Moved) - 1 do
    if not FileExists(MovedDir + Moved[I]) then
      RenameFile(MovedDir + Moved[I] + '.old', MovedDir + Moved[I]);
end;

// Spectre lanza la actualización mientras termina de cerrarse. Si algo lo
// retiene (el driver de una webcam que no suelta la cámara), su exe y sus DLL
// siguen en uso, los archivos no se pueden reemplazar y la instalación se
// deshace. Se espera a que termine; si no, se cierra a la fuerza (a esas
// alturas ya lo guardó todo); y si ni así, se apartan sus archivos.
procedure WaitForSpectreToExit();
var
  Pid, Code: Integer;
  Filter: String;
begin
  Pid := StrToIntDef(ExpandConstant('{param:spectrepid|0}'), 0);
  if WaitSpectreGone(15000) then Exit;
  // Una instalación silenciosa que no lanzó Spectre (a mano, un script): el
  // Spectre abierto se está usando y lo cierra Restart Manager, que le deja
  // terminar la grabación y salir bien. Lo lanzó Spectre si trae /SPECTREPID
  // o, desde el actualizador de la 0.1.138 y anteriores, /RELAUNCH=1.
  if (Pid = 0) and not RelaunchAfterUpdate() then Exit;
  // Solo el proceso que lanzó la actualización, y solo si sigue siendo Spectre.
  Filter := '/FI "IMAGENAME eq spectre.exe"';
  if Pid > 0 then Filter := Filter + ' /FI "PID eq ' + IntToStr(Pid) + '"';
  Log('Spectre sigue abierto 15 s despues de lanzar la actualizacion: se cierra a la fuerza');
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F ' + Filter, '', SW_HIDE, ewWaitUntilTerminated, Code);
  if WaitSpectreGone(10000) then Exit;
  Log('Spectre sigue sin cerrarse: se apartan sus archivos');
  MoveAsideInUseFiles();
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssDone then InstallFinished := True;
end;

// Si la actualización silenciosa falla o se cancela, [Run] no llega a
// ejecutarse: se vuelve a abrir la versión que haya quedado instalada para
// que el usuario no se quede sin Spectre. --update-failed hace que avise de
// que no se instaló y por qué (lo lee del registro de /LOG).
procedure DeinitializeSetup();
var
  Dir: String;
  Code: Integer;
begin
  if not InstallFinished then RestoreMovedFiles();
  if RelaunchAfterUpdate() and not InstallFinished and
     RegQueryStringValue(HKCU, ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#emit SetupSetting("AppId")}_is1'), 'InstallLocation', Dir) and
     FileExists(AddBackslash(Dir) + 'spectre.exe') then
    Exec(AddBackslash(Dir) + 'spectre.exe', '--update-failed', Dir, SW_SHOWNORMAL, ewNoWait, Code);
end;

// Las ediciones N/KN de Windows no traen Media Foundation. Spectre funciona
// igual, pero la webcam no; se avisa en vez de fallar en silencio.
function InitializeSetup(): Boolean;
begin
  Result := True;
  // Una instalación silenciosa es la actualización lanzada desde Spectre.
  if WizardSilent then WaitForSpectreToExit();
  // En una actualización silenciosa no se pregunta nada (ya se avisó al instalar).
  if not WizardSilent and not FileExists(ExpandConstant('{sys}\mfplat.dll')) then
    MsgBox('Tu Windows no incluye Media Foundation (ediciones N). Spectre funcionará, pero sin webcam. Para usarla, instala el "Paquete de características multimedia" desde Configuración > Aplicaciones > Características opcionales.', mbInformation, MB_OK);
end;
