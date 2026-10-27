#include "app/Voice.h"

#include "app/CrashHandler.h"
#include "common/Util.h"

#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <sapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string_view>
#include <utility>

// mmddk.h: el id de endpoint (el de IMMDevice) de un dispositivo waveIn.
#ifndef DRV_QUERYFUNCTIONINSTANCEID
#define DRV_QUERYFUNCTIONINSTANCEID (DRV_RESERVED + 17)
#define DRV_QUERYFUNCTIONINSTANCEIDSIZE (DRV_RESERVED + 18)
#endif

namespace spectre {

using Microsoft::WRL::ComPtr;

const char* VoiceCommandName(VoiceCommand command) {
  switch (command) {
  case VoiceCommand::SaveClip: return "Guardar clip";
  case VoiceCommand::ShortClip: return "Guardar clip corto";
  case VoiceCommand::StartRecording: return "Empezar a grabar";
  case VoiceCommand::StopRecording: return "Detener la grabación";
  case VoiceCommand::Screenshot: return "Captura de pantalla";
  case VoiceCommand::Marker: return "Marcar un momento";
  default: return "";
  }
}

const std::string& VoicePhrases(const VoiceConfig& config, VoiceCommand command) {
  switch (command) {
  case VoiceCommand::SaveClip: return config.saveClip;
  case VoiceCommand::ShortClip: return config.shortClip;
  case VoiceCommand::StartRecording: return config.startRecording;
  case VoiceCommand::StopRecording: return config.stopRecording;
  case VoiceCommand::Screenshot: return config.screenshot;
  default: return config.marker;
  }
}

std::string& VoicePhrases(VoiceConfig& config, VoiceCommand command) {
  return const_cast<std::string&>(VoicePhrases(std::as_const(config), command));
}

namespace {

// «  guarda   el clip » -> «guarda el clip».
std::string Tidy(std::string_view text) {
  std::string out;
  for (char ch : text) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += ch;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Para comparar frases sin mirar mayúsculas (también las acentuadas).
std::wstring Folded(const std::string& text) {
  std::wstring w = Widen(text);
  if (!w.empty()) CharLowerBuffW(w.data(), DWORD(w.size()));
  return w;
}

} // namespace

std::vector<std::string> SplitPhrases(const std::string& text) {
  std::vector<std::string> out;
  std::vector<std::wstring> keys;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of(",;\n", start);
    if (end == std::string::npos) end = text.size();
    std::string phrase = Tidy(std::string_view(text).substr(start, end - start));
    std::wstring key = Folded(phrase);
    if (!phrase.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) {
      out.push_back(std::move(phrase));
      keys.push_back(std::move(key));
    }
    start = end + 1;
  }
  return out;
}

std::string RepeatedPhrase(const VoiceConfig& config) {
  std::vector<std::wstring> seen;
  for (int i = 0; i < kVoiceCommandCount; ++i) {
    std::vector<std::string> phrases = SplitPhrases(VoicePhrases(config, VoiceCommand(i)));
    for (const std::string& phrase : phrases)
      if (std::find(seen.begin(), seen.end(), Folded(phrase)) != seen.end()) return phrase;
    for (const std::string& phrase : phrases) seen.push_back(Folded(phrase));
  }
  return {};
}

std::string SpokenPhrase(const VoiceConfig& config, const std::string& phrase) {
  std::string wake = Tidy(config.wakeWord);
  return wake.empty() ? phrase : wake + " " + phrase;
}

namespace {

constexpr ULONG kRuleBase = 100; // id de la regla de cada orden: kRuleBase + VoiceCommand

struct RecognizerToken {
  ComPtr<ISpObjectToken> token;
  VoiceRecognizer info;
  LANGID langId = 0; // el primero de sus idiomas
};

// Atributo Language de un reconocedor: LANGID en hexadecimal separados por
// punto y coma ("c0a;40a" en el de español de España).
std::vector<LANGID> ParseLanguages(const wchar_t* text) {
  std::vector<LANGID> out;
  std::wstring_view s(text);
  size_t start = 0;
  while (start < s.size()) {
    size_t end = s.find(L';', start);
    if (end == std::wstring_view::npos) end = s.size();
    std::wstring part(s.substr(start, end - start));
    unsigned long id = wcstoul(part.c_str(), nullptr, 16);
    if (id) out.push_back(LANGID(id));
    start = end + 1;
  }
  return out;
}

std::string TakeString(LPWSTR text) {
  if (!text) return {};
  std::string out = Narrow(text);
  CoTaskMemFree(text);
  return out;
}

// Los reconocedores de SAPI (HKLM\SOFTWARE\Microsoft\Speech\Recognizers). En
// Windows 10 y 11 vienen con el «Reconocimiento de voz» de cada idioma; los
// de OneCore (Speech_OneCore) no se pueden usar desde aquí.
std::vector<RecognizerToken> EnumerateRecognizers() {
  std::vector<RecognizerToken> out;
  ComPtr<ISpObjectTokenCategory> category;
  if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category))) ||
      FAILED(category->SetId(SPCAT_RECOGNIZERS, FALSE)))
    return out;
  ComPtr<IEnumSpObjectTokens> tokens;
  if (FAILED(category->EnumTokens(nullptr, nullptr, &tokens)) || !tokens) return out;
  for (;;) {
    ISpObjectToken* raw = nullptr;
    ULONG fetched = 0;
    if (tokens->Next(1, &raw, &fetched) != S_OK || !raw) break;
    RecognizerToken t;
    t.token.Attach(raw);
    LPWSTR text = nullptr;
    if (SUCCEEDED(t.token->GetId(&text))) t.info.id = TakeString(text);
    text = nullptr;
    if (SUCCEEDED(t.token->GetStringValue(nullptr, &text))) t.info.name = TakeString(text);
    ComPtr<ISpDataKey> attributes;
    if (SUCCEEDED(t.token->OpenKey(SPTOKENKEY_ATTRIBUTES, &attributes))) {
      text = nullptr;
      if (SUCCEEDED(attributes->GetStringValue(L"Language", &text)) && text) {
        std::vector<LANGID> languages = ParseLanguages(text);
        CoTaskMemFree(text);
        if (!languages.empty()) t.langId = languages.front();
        for (LANGID id : languages) t.info.spanish |= PRIMARYLANGID(id) == LANG_SPANISH;
      }
    }
    if (t.info.name.empty()) t.info.name = t.info.id;
    if (!t.info.id.empty()) out.push_back(std::move(t));
  }
  return out;
}

// El elegido en Ajustes; si no, el de español; si no hay, el del idioma de
// Windows, y si tampoco, el primero.
const RecognizerToken* PickRecognizer(const std::vector<RecognizerToken>& list, const std::string& wanted) {
  if (!wanted.empty())
    for (const auto& t : list)
      if (_stricmp(t.info.id.c_str(), wanted.c_str()) == 0) return &t;
  for (const auto& t : list)
    if (t.info.spanish) return &t;
  const LANGID ui = GetUserDefaultUILanguage();
  for (const auto& t : list)
    if (PRIMARYLANGID(t.langId) == PRIMARYLANGID(ui)) return &t;
  return list.empty() ? nullptr : &list.front();
}

// Dispositivo waveIn de ese endpoint (el id de IMMDevice que guarda Ajustes >
// Audio). -1 si no está conectado.
int WaveInFor(const std::string& endpointId) {
  if (endpointId.empty()) return -1;
  const std::wstring wanted = Widen(endpointId);
  const UINT count = waveInGetNumDevs();
  for (UINT i = 0; i < count; ++i) {
    auto device = reinterpret_cast<HWAVEIN>(static_cast<UINT_PTR>(i));
    ULONG size = 0;
    if (waveInMessage(device, DRV_QUERYFUNCTIONINSTANCEIDSIZE, reinterpret_cast<DWORD_PTR>(&size), 0) !=
            MMSYSERR_NOERROR ||
        size < sizeof(wchar_t))
      continue;
    std::wstring id(size / sizeof(wchar_t), L'\0');
    if (waveInMessage(device, DRV_QUERYFUNCTIONINSTANCEID, reinterpret_cast<DWORD_PTR>(id.data()), size) !=
        MMSYSERR_NOERROR)
      continue;
    id.resize(wcsnlen(id.c_str(), id.size()));
    if (_wcsicmp(id.c_str(), wanted.c_str()) == 0) return int(i);
  }
  return -1;
}

// El micrófono predeterminado de Windows, para notar que cambia (unos cascos
// que se encienden): SAPI se queda con el que abrió.
std::wstring DefaultMicId(IMMDeviceEnumerator* enumerator) {
  ComPtr<IMMDevice> device;
  LPWSTR id = nullptr;
  if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) ||
      FAILED(device->GetId(&id)))
    return {};
  std::wstring out = id ? id : L"";
  CoTaskMemFree(id);
  return out;
}

// SpClearEvent de sphelper.h.
void ClearEvent(SPEVENT& event) {
  switch (int(event.elParamType)) {
  case SPET_LPARAM_IS_POINTER:
  case SPET_LPARAM_IS_STRING: CoTaskMemFree(reinterpret_cast<void*>(event.lParam)); break;
  case SPET_LPARAM_IS_TOKEN:
  case SPET_LPARAM_IS_OBJECT:
    if (event.lParam) reinterpret_cast<IUnknown*>(event.lParam)->Release();
    break;
  default: break;
  }
  event = {};
}

float Threshold(const std::string& sensitivity) {
  if (sensitivity == "estricta") return 0.8f;
  if (sensitivity == "permisiva") return 0.4f;
  return 0.6f;
}

enum class Outcome { Stopped, Restart, Failed };

} // namespace

struct VoiceListener::Shared {
  HWND notify = nullptr;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  mutable std::mutex mutex;
  VoiceStatus status;

  // Lo que pasa en el hilo de escucha (no necesita el mutex).
  ULONGLONG lastCommand[kVoiceCommandCount] = {};
  int ignored = 0;            // frases no aceptadas desde la última línea del log
  ULONGLONG ignoredLogAt = 0;
  std::string lastError;

  ~Shared() {
    if (stop) CloseHandle(stop);
    if (done) CloseHandle(done);
  }

  template <typename F>
  void Update(F&& change) {
    {
      std::lock_guard lock(mutex);
      change(status);
    }
    PostMessageW(notify, WM_SPECTRE_VOICE_STATUS, 0, 0);
  }

  Outcome Fail(const std::string& text, HRESULT hr) {
    char code[32] = "";
    if (FAILED(hr)) snprintf(code, sizeof(code), " (error 0x%08lX)", static_cast<unsigned long>(hr));
    // Los reintentos (cada vez más espaciados) no repiten la misma línea en el log.
    if (text + code != lastError) Log("Voz: %s%s", text.c_str(), code);
    lastError = text + code;
    Update([&](VoiceStatus& s) {
      s.state = VoiceStatus::State::Error;
      s.error = text;
      s.note.clear();
    });
    return Outcome::Failed;
  }

  void OnResult(ISpRecoResult* result, bool recognized, const VoiceConfig& config);
  bool Drain(ISpRecoContext* context, const VoiceConfig& config);
  Outcome Session(const VoiceConfig& config, const std::string& micId);
  void Run(const VoiceConfig& config, const std::string& micId);
};

void VoiceListener::Shared::OnResult(ISpRecoResult* result, bool recognized, const VoiceConfig& config) {
  if (!result) return;
  SPPHRASE* phrase = nullptr;
  if (FAILED(result->GetPhrase(&phrase)) || !phrase) return;
  const ULONG ruleId = phrase->Rule.ulId;
  // La confianza del motor (0..1). Si no la da, la de tres niveles de SAPI.
  float confidence = phrase->Rule.SREngineConfidence;
  if (!(confidence >= 0.0f && confidence <= 1.0f))
    confidence = phrase->Rule.Confidence > 0 ? 0.9f : phrase->Rule.Confidence == 0 ? 0.7f : 0.4f;
  CoTaskMemFree(phrase);
  LPWSTR text = nullptr;
  std::string heard;
  const ULONG whole = ULONG(SP_GETWHOLEPHRASE); // -1: todas las palabras
  if (SUCCEEDED(result->GetText(whole, whole, TRUE, &text, nullptr))) heard = TakeString(text);

  const int command = int(ruleId) - int(kRuleBase);
  const bool known = recognized && command >= 0 && command < kVoiceCommandCount && !heard.empty();
  bool accepted = known && confidence >= Threshold(config.sensitivity);
  const ULONGLONG now = GetTickCount64();
  // Una frase solo cuenta una vez aunque el motor la entregue dos veces seguidas.
  if (accepted && now - lastCommand[command] < 1500) return;

  // Cuándo se empezó a decir: el clip acaba ahí y no lleva la frase dentro.
  int64_t spokenQpc = 0;
  long long agoMs = -1;
  SPRECORESULTTIMES times{};
  if (SUCCEEDED(result->GetResultTimes(&times)) && times.dwTickCount) {
    agoMs = static_cast<long long>(DWORD(GetTickCount() - times.dwTickCount));
    if (agoMs <= 8000) spokenQpc = QpcNow() - int64_t(agoMs) * QpcFrequency() / 1000;
  }

  Update([&](VoiceStatus& s) {
    s.heard = recognized ? heard : std::string(); // lo que SAPI descartó no se parecía a ninguna frase
    s.confidence = confidence;
    s.accepted = accepted;
    s.heardAt = now;
  });
  // Ya se pidió parar (Ajustes cambió y escucha otro): la orden no llega a App.
  if (accepted && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
  if (accepted) {
    lastCommand[command] = now;
    Log("Voz: «%s» -> %s (confianza %.2f, empezó hace %lld ms)", heard.c_str(),
        VoiceCommandName(VoiceCommand(command)), confidence, agoMs);
    auto* event = new VoiceEvent{VoiceCommand(command), heard, spokenQpc};
    if (!PostMessageW(notify, WM_SPECTRE_VOICE, 0, reinterpret_cast<LPARAM>(event))) delete event;
    return;
  }
  // Lo que no era una orden (hablar con los amigos) se resume: una línea cada minuto como mucho.
  ++ignored;
  if (now - ignoredLogAt >= 60000) {
    Log("Voz: %d frases sin aceptar en el último minuto (la última «%s», confianza %.2f, mínimo %.2f)", ignored,
        heard.c_str(), confidence, Threshold(config.sensitivity));
    ignored = 0;
    ignoredLogAt = now;
  }
}

// Lee los eventos de SAPI que haya. true: el audio se acabó (micrófono desconectado).
bool VoiceListener::Shared::Drain(ISpRecoContext* context, const VoiceConfig& config) {
  bool ended = false;
  SPEVENT event{};
  ULONG fetched = 0;
  while (context->GetEvents(1, &event, &fetched) == S_OK && fetched == 1) {
    switch (int(event.eEventId)) {
    case SPEI_RECOGNITION: OnResult(reinterpret_cast<ISpRecoResult*>(event.lParam), true, config); break;
    case SPEI_FALSE_RECOGNITION: OnResult(reinterpret_cast<ISpRecoResult*>(event.lParam), false, config); break;
    case SPEI_END_SR_STREAM: ended = true; break;
    default: break;
    }
    ClearEvent(event);
    fetched = 0;
  }
  return ended;
}

Outcome VoiceListener::Shared::Session(const VoiceConfig& config, const std::string& micId) {
  std::vector<RecognizerToken> recognizers = EnumerateRecognizers();
  const RecognizerToken* chosen = PickRecognizer(recognizers, config.recognizer);
  if (!chosen)
    return Fail("Windows no tiene el reconocimiento de voz instalado. Añádelo en Configuración > Hora e idioma > "
                "Idioma y región: Español (España) > Opciones de idioma > Reconocimiento de voz.",
                S_OK);
  std::string note;
  if (!chosen->info.spanish)
    note = "Este reconocedor no es de español: di las frases en su idioma o instala el español de España (mira la "
           "ayuda de abajo).";

  ComPtr<ISpRecognizer> recognizer;
  HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&recognizer));
  if (FAILED(hr)) return Fail("No se pudo abrir el reconocimiento de voz de Windows", hr);
  hr = recognizer->SetRecognizer(chosen->token.Get());
  if (FAILED(hr)) return Fail("No se pudo cargar el reconocedor «" + chosen->info.name + "»", hr);
  // Sin adaptar el modelo a lo que oye (en una partida, el juego y Discord):
  // con una lista cerrada no hace falta y trabajaría de fondo. Solo esta
  // sesión; el ajuste guardado de Windows no cambia. Si no lo admite, da igual.
  recognizer->SetPropertyNum(L"AdaptationOn", 0);

  // Micrófono: el de Ajustes > Audio o, si no se eligió (o no está), el
  // predeterminado de Windows. En modo compartido: lo puede usar a la vez la
  // pista del micrófono, Discord o el juego.
  ComPtr<ISpAudio> audio;
  hr = CoCreateInstance(CLSID_SpMMAudioIn, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&audio));
  if (FAILED(hr)) return Fail("No se pudo preparar el micrófono para la voz", hr);
  const int wave = WaveInFor(micId);
  if (!micId.empty() && wave < 0)
    note += std::string(note.empty() ? "" : " ") +
            "El micrófono de Ajustes > Audio no está conectado: escucha el predeterminado de Windows.";
  if (wave >= 0) {
    ComPtr<ISpMMSysAudio> mm;
    if (SUCCEEDED(audio.As(&mm))) mm->SetDeviceId(UINT(wave));
  }
  hr = recognizer->SetInput(audio.Get(), TRUE);
  if (FAILED(hr)) return Fail("El reconocimiento de voz no acepta el micrófono", hr);

  ComPtr<ISpRecoContext> context;
  hr = recognizer->CreateRecoContext(&context);
  if (FAILED(hr)) return Fail("No se pudo preparar el reconocimiento de voz", hr);
  const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
  context->SetInterest(interest, interest);
  context->SetNotifyWin32Event();
  HANDLE events = context->GetNotifyEventHandle();

  // Gramática cerrada: una regla por orden con sus frases. El motor solo
  // compara con estas frases, así que gasta muy poco y no hace falta conexión.
  ComPtr<ISpRecoGrammar> grammar;
  hr = context->CreateGrammar(1, &grammar);
  if (SUCCEEDED(hr)) hr = grammar->ResetGrammar(chosen->langId ? chosen->langId : GetUserDefaultUILanguage());
  if (FAILED(hr)) return Fail("No se pudo preparar la lista de frases", hr);
  int phrases = 0;
  std::vector<std::wstring> used;
  for (int i = 0; i < kVoiceCommandCount; ++i) {
    SPSTATEHANDLE rule = nullptr;
    for (const std::string& phrase : SplitPhrases(VoicePhrases(config, VoiceCommand(i)))) {
      std::wstring key = Folded(phrase);
      if (std::find(used.begin(), used.end(), key) != used.end()) continue; // repetida: se la queda la primera orden
      used.push_back(key);
      if (!rule && FAILED(grammar->GetRule(nullptr, kRuleBase + ULONG(i), SPRAF_TopLevel | SPRAF_Active, TRUE, &rule)))
        break;
      std::wstring words = Widen(SpokenPhrase(config, phrase));
      if (SUCCEEDED(grammar->AddWordTransition(rule, nullptr, words.c_str(), L" ", SPWT_LEXICAL, 1.0f, nullptr)))
        ++phrases;
    }
  }
  if (!phrases) return Fail("No hay ninguna frase: escribe al menos una en Ajustes > Voz", S_OK);
  hr = grammar->Commit(0);
  if (SUCCEEDED(hr)) hr = grammar->SetRuleState(nullptr, nullptr, SPRS_ACTIVE);
  if (FAILED(hr)) return Fail("Windows no pudo preparar las frases. Prueba con frases más sencillas", hr);
  // Aquí se abre el micrófono.
  hr = recognizer->SetRecoState(SPRST_ACTIVE);
  if (FAILED(hr))
    return Fail("No se pudo abrir el micrófono. Mira que esté conectado y que Windows deje usarlo a las "
                "aplicaciones de escritorio (Configuración > Privacidad y seguridad > Micrófono)",
                hr);

  lastError.clear();
  Log("Voz: escuchando con %s, %d frases, micrófono %s", chosen->info.name.c_str(), phrases,
      wave >= 0 ? "de Ajustes > Audio" : "predeterminado");
  Update([&](VoiceStatus& s) {
    s.state = VoiceStatus::State::Listening;
    s.recognizer = chosen->info.name;
    s.note = note;
    s.error.clear();
  });

  ComPtr<IMMDeviceEnumerator> devices;
  if (wave < 0) CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
  const std::wstring defaultMic = DefaultMicId(devices.Get());
  ULONGLONG micCheckedAt = GetTickCount64();
  Outcome outcome = Outcome::Stopped;
  HANDLE handles[] = {stop, events};
  for (;;) {
    DWORD w = WaitForMultipleObjects(events ? 2 : 1, handles, FALSE, 3000);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_OBJECT_0 + 1) {
      if (Drain(context.Get(), config)) {
        Log("Voz: el micrófono dejó de dar audio, se vuelve a abrir");
        outcome = Outcome::Restart;
        break;
      }
    } else if (w != WAIT_TIMEOUT) {
      outcome = Fail("El reconocimiento de voz dejó de responder", HRESULT_FROM_WIN32(GetLastError()));
      break;
    }
    // Cada 3 s, también con charla continua (cada frase despierta el bucle).
    const ULONGLONG now = GetTickCount64();
    if (now - micCheckedAt < 3000) continue;
    micCheckedAt = now;
    if (devices && DefaultMicId(devices.Get()) != defaultMic) {
      Log("Voz: cambió el micrófono predeterminado de Windows, se vuelve a abrir");
      outcome = Outcome::Restart;
      break;
    }
  }
  recognizer->SetRecoState(SPRST_INACTIVE_WITH_PURGE); // suelta el micrófono
  return outcome;
}

void VoiceListener::Shared::Run(const VoiceConfig& config, const std::string& micId) {
  InstallThreadCrashHandler();
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int failures = 0;
  for (;;) {
    ULONGLONG started = GetTickCount64();
    Outcome outcome = Session(config, micId);
    if (outcome == Outcome::Stopped) break;
    // Si se cae nada más abrir (micrófono que no está, reconocedor roto), se
    // espera cada vez más antes de reintentar: 2 s, 4 s... hasta 1 min.
    const bool quick = GetTickCount64() - started < 10000;
    failures = outcome == Outcome::Failed || quick ? failures + 1 : 0;
    DWORD delay = failures == 0 ? 200 : DWORD(std::min<ULONGLONG>(60000, 2000ull << std::min(failures - 1, 5)));
    if (WaitForSingleObject(stop, delay) == WAIT_OBJECT_0) break;
  }
  if (SUCCEEDED(co)) CoUninitialize();
  SetEvent(done);
}

void VoiceListener::Start(const VoiceConfig& config, const std::string& micDeviceId, HWND notify) {
  Stop(500); // con otros ajustes: el anterior suelta el micrófono mientras arranca este
  config_ = config;
  micDeviceId_ = micDeviceId;
  auto shared = std::make_shared<Shared>();
  shared->notify = notify;
  shared->status.state = VoiceStatus::State::Starting;
  shared_ = shared;
  thread_ = std::thread([shared, config, micDeviceId] { shared->Run(config, micDeviceId); });
}

void VoiceListener::Stop(DWORD waitMs) {
  if (thread_.joinable()) {
    SetEvent(shared_->stop);
    // Soltar el micrófono tarda milisegundos. Si el driver se queda colgado,
    // el hilo se suelta (su estado vive con él) y la interfaz no se congela.
    if (WaitForSingleObject(shared_->done, waitMs) == WAIT_OBJECT_0) {
      thread_.join();
    } else {
      Log("Voz: el reconocimiento no se cerró en %lu ms; se deja terminar solo", static_cast<unsigned long>(waitMs));
      thread_.detach();
    }
  }
  shared_.reset();
}

VoiceStatus VoiceListener::Status() const {
  if (!shared_) return {};
  std::lock_guard lock(shared_->mutex);
  return shared_->status;
}

std::vector<VoiceRecognizer> InstalledRecognizers() {
  // En un hilo MTA propio: no depende de cómo tenga COM el hilo que pregunta.
  std::vector<VoiceRecognizer> out;
  std::thread([&out] {
    InstallThreadCrashHandler();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (auto& t : EnumerateRecognizers()) out.push_back(t.info);
    if (SUCCEEDED(co)) CoUninitialize();
  }).join();
  return out;
}

} // namespace spectre
