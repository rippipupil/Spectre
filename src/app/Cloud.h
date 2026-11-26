#pragma once

// Subidas a tu propia nube: un bucket compatible con S3 (Cloudflare R2,
// Backblaze B2 u otro). Spectre firma cada petición (AWS Signature V4) y habla
// directamente con el bucket por WinHTTP: sin servidores intermedios ni
// librerías nuevas. Los vídeos grandes van por partes de 8 MB que se retoman
// tras un corte o al volver a abrir Spectre.
//
// Enlaces: spectre/<identificador>-<código>.mp4. El identificador sale de la
// fecha (ordena las subidas en el bucket) y el código son 24 caracteres
// aleatorios (unos 143 bits): nadie puede adivinar el enlace de otro clip.
// Con la dirección pública del bucket el enlace no caduca; sin ella es una
// URL firmada que vale hasta 7 días y se vuelve a firmar al copiarla.
//
// Spectre Holder (la app para Android de holder/): «Enviar al móvil» sube a
// spectre/movil/<identificador>-<código>/<nombre del archivo>, con el nombre a
// la vista para que el móvil lo enseñe sin pedir nada más. El móvil lo lista,
// lo descarga y lo borra de la nube; se empareja con el código de
// PhonePairingCode (el QR de Ajustes > Móvil).
//
// Ningún hilo vive sin trabajo: el de subida arranca con la primera de la
// cola y acaba al vaciarla. Con un juego abierto va en modo de fondo y con la
// velocidad limitada (Ajustes > Nube), para no subir el ping.

#include "app/Config.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spectre {

// Estado de un archivo para la biblioteca y el panel (Alt+Z).
struct CloudFileState {
  enum class Phase { None, Queued, Uploading, Done, Failed };
  Phase phase = Phase::None;
  float progress = 0; // 0..1 mientras sube
  std::string error;  // Failed: en lenguaje claro
  bool copyLink = false; // en la cola o subiendo: al acabar se copia el enlace (se pidió compartir)
  // Sin enlace: firmarlo cuesta; para eso está Link().
};

// Qué pasa con el archivo del PC cuando se sube.
enum class CloudDelete {
  Keep,
  AfterUpload, // se borra en cuanto se sube (para siempre, sin papelera)
  AtExit,      // se borra al salir de Spectre (modo cibercafé: lo copiado para pegarlo)
};

struct CloudKeys {
  std::string keyId;  // Access Key ID
  std::string secret; // Secret Access Key
  bool Empty() const { return keyId.empty() || secret.empty(); }
};

// Una subida que terminó o falló, para el aviso.
struct CloudEvent {
  std::string path;
  std::string name;
  bool ok = false;
  std::string link;
  std::string error;
  bool copyLink = false;  // el enlace va al portapapeles (compartir, o Ajustes > Nube)
  bool automatic = false; // subida automática: el aviso es más discreto
  bool deleted = false;   // se borró del PC al subirse
  bool phone = false;     // «Enviar al móvil»: ya está en la nube esperando al móvil
};

// Ajustes > Nube y la línea de la biblioteca.
struct CloudStatus {
  bool configured = false; // endpoint, bucket y claves
  bool keysStored = false; // las claves están en disco (cifradas); false = solo en memoria o ninguna
  bool hasKeys = false;
  std::string keyIdHint;   // "AKIA...X7Q"
  int queued = 0;          // esperando, sin contar la que sube
  int failed = 0;
  std::string current;     // la que se está subiendo
  float progress = 0;      // de la actual, 0..1
  double bytesPerSecond = 0;
  bool paused = false;      // pausadas a mano
  bool waitingGame = false; // con un juego abierto y «Esperar a cerrarlo»
  bool offline = false;     // sin conexión: se reintenta sola
  std::string lastError;    // el último fallo (también los que se reintentan)
  struct Recent {
    std::string path, name, when;
    bool local = true; // sigue en el PC
  };
  std::vector<Recent> recent; // las últimas subidas, de la más reciente a la más antigua
  int doneCount = 0;
  // «Enviar al móvil»: lo que espera subirse o falló, y lo último enviado
  // (también en las cuentas de arriba, que son de toda la cola).
  int phoneQueued = 0;       // esperando o subiendo
  int phoneFailed = 0;
  bool currentPhone = false; // la que sube ahora (current) va al móvil
  std::vector<Recent> phoneRecent;
  enum class Test { Idle, Running, Ok, Warning, Failed };
  Test test = Test::Idle;
  std::string testMessage;
  uint64_t version = 0;
};

class CloudUploader {
public:
  CloudUploader();
  ~CloudUploader();
  CloudUploader(const CloudUploader&) = delete;
  CloudUploader& operator=(const CloudUploader&) = delete;

  // notify recibe WM_SPECTRE_CLOUD cuando algo cambia. Carga la cola guardada,
  // las claves (salvo en el modo cibercafé) y retoma lo pendiente.
  void Start(HWND notify, const CloudConfig& config);
  void Configure(const CloudConfig& config);
  // Al salir: guarda la cola (lo que se estaba subiendo sigue la próxima vez)
  // y suelta la conexión. Borra lo subido que tocaba borrar; devuelve lo que
  // no se pudo (abierto en otro programa). En el modo cibercafé olvida lo ya subido.
  std::vector<std::string> Stop();

  bool Configured() const;
  // persist = false: solo en memoria (modo cibercafé). Las de disco se borran.
  bool SetKeys(const CloudKeys& keys, bool persist, std::string* error);
  // Pasa las claves de memoria a disco o al revés (al cambiar el modo cibercafé).
  void PersistKeys(bool persist);
  void ForgetKeys();
  // Al dejar el modo cibercafé: quien lo apaga puede ser el siguiente cliente.
  // Olvida las claves y lo subido, borra lo copiado para pegarlo y lo que no
  // llegó a subirse ya no se sube solo (solo con «Reintentar»).
  void EndCyberSession();
  CloudKeys Keys() const;

  // A la cola. front: la primera (compartir ahora). Si ya está subido, solo
  // avisa con el enlace. En el modo cibercafé va a la nube configurada ahora,
  // aunque luego se pegue otra.
  void Enqueue(const std::string& path, bool front, bool copyLink, CloudDelete removal, bool automatic);
  void Cancel(const std::string& path);
  // «Enviar al móvil»: a spectre/movil/ (aparte de los enlaces). automatic:
  // «Mandar al móvil cada clip nuevo», sin prisa; a mano va la primera.
  void SendToPhone(const std::string& path, CloudDelete removal, bool automatic);
  void CancelPhone(const std::string& path);
  CloudFileState PhoneState(const std::string& path) const;
  // "spectre-holder:<base64url>" con el bucket y las claves para la app del
  // móvil (el QR de Ajustes > Móvil). Vacío sin nube configurada.
  std::string PhonePairingCode(const std::string& pcName) const;
  // Vuelve a intentar las fallidas (todas con path vacío).
  void Retry(const std::string& path = {});
  void Renamed(const std::string& from, const std::string& to);
  // A la papelera o borrados: fuera de la cola; lo ya subido sigue en la lista.
  void Removed(const std::vector<std::string>& paths);
  void SetPaused(bool paused);
  // Hay un juego abierto o se graba (App, cada 2 s): se sube con el límite de Ajustes.
  void SetGameActive(bool active);

  // De los enlaces (compartir); lo enviado al móvil va en PhoneState.
  CloudFileState State(const std::string& path) const;
  // Enlace de algo ya subido (vacío si no). Sin dirección pública se firma ahora.
  std::string Link(const std::string& path) const;
  CloudStatus Status() const;
  // Todo lo que tiene estado, de una vez (biblioteca), sin enlaces.
  std::unordered_map<std::string, CloudFileState> Snapshot() const;
  // Lo que va al móvil y aún no está en la nube (en cola, subiendo o fallado).
  std::unordered_map<std::string, CloudFileState> PhoneSnapshot() const;
  uint64_t Version() const;
  // Lo que espera subirse: el límite de espacio no lo borra.
  std::vector<std::string> PendingPaths() const;
  bool Busy() const; // queda algo por subir
  std::vector<CloudEvent> TakeEvents();
  // Ajustes > Nube > Probar: sube un archivo de texto pequeño, abre su enlace
  // y lo borra. keys vacías = las guardadas.
  void Test(const CloudConfig& config, const CloudKeys& keys);
  // Vacía la lista de lo ya subido (no borra nada del bucket).
  void ClearHistory();

  struct Shared;

private:
  void Kick(); // arranca el hilo de subida si hay trabajo y no va ya
  // Borra del PC lo subido que quedó por borrar (atExit: también lo de salir).
  // Devuelve lo que sigue sin poder borrarse.
  std::vector<std::string> RemoveUploaded(bool atExit);
  std::shared_ptr<Shared> shared_;
};

// La configuración y las claves en un texto para pegarlo en otro PC (el del
// cibercafé): "spectre-nube:<base64>".
std::string ExportCloudSettings(const CloudConfig& config, const CloudKeys& keys);
bool ImportCloudSettings(const std::string& text, CloudConfig* config, CloudKeys* keys);
// "https://<cuenta>.r2.cloudflarestorage.com/<bucket>" (lo que enseña
// Cloudflare) también vale: el bucket sale de ahí si no se escribió.
std::string CloudBucket(const CloudConfig& config);

} // namespace spectre
