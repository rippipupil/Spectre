#include "app/Sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace spectre {

namespace {

enum class Wave {
  Sine,  // tono limpio
  Soft,  // seno con un poco de tercer armónico: suena a consola antigua sin chirriar
  Bell,  // ataque instantáneo y caída larga, con un parcial inarmónico
  Glide, // la nota se desliza hasta la siguiente, con vibrato (fantasma)
};

struct Note {
  float freq;
  float seconds;
};

// WAV PCM de 16 bits mono con unas notas seguidas. Cada nota sube y baja de
// volumen en unos milisegundos para que no chasquee.
std::vector<uint8_t> MakeWave(std::initializer_list<Note> notes, Wave wave, float volume) {
  const int rate = 44100;
  int samples = 0;
  for (const Note& n : notes) samples += int(float(rate) * n.seconds);
  std::vector<uint8_t> wav(44 + size_t(samples) * 2);
  auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
  auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
  memcpy(&wav[0], "RIFF", 4);
  put32(4, uint32_t(wav.size() - 8));
  memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1); // PCM
  put16(22, 1); // mono
  put32(24, rate);
  put32(28, rate * 2);
  put16(32, 2);
  put16(34, 16);
  memcpy(&wav[36], "data", 4);
  put32(40, uint32_t(samples * 2));
  const float pi = 3.14159265f;
  const int fade = rate / 200; // 5 ms
  int i = 0;
  const Note* list = notes.begin();
  for (size_t k = 0; k < notes.size(); ++k) {
    const Note& note = list[k];
    const float next = k + 1 < notes.size() ? list[k + 1].freq : note.freq;
    const int perNote = int(float(rate) * note.seconds);
    float phase = 0;
    for (int n = 0; n < perNote; ++n, ++i) {
      const float t = float(n) / float(rate);
      const float u = float(n) / float(std::max(perNote, 1));
      float env = 1.0f;
      if (n < fade) env = float(n) / float(fade);
      else if (n > perNote - fade * 4) env = float(perNote - n) / float(fade * 4);
      float freq = note.freq;
      if (wave == Wave::Glide) freq = note.freq + (next - note.freq) * u + 7.0f * std::sin(2.0f * pi * 9.0f * t);
      phase += 2.0f * pi * freq / float(rate);
      float v = 0;
      switch (wave) {
      case Wave::Sine:
      case Wave::Glide: v = std::sin(phase); break;
      case Wave::Soft: v = std::sin(phase) * 0.8f + std::sin(phase * 3.0f) * 0.2f; break;
      case Wave::Bell:
        env = std::min(env, 1.0f) * std::exp(-t * 7.0f);
        v = std::sin(phase) * 0.7f + std::sin(phase * 2.76f) * 0.3f * std::exp(-t * 12.0f);
        break;
      }
      put16(44 + size_t(i) * 2, uint16_t(int16_t(std::clamp(v * env * volume, -1.0f, 1.0f) * 32767.0f)));
    }
  }
  return wav;
}

std::vector<uint8_t> SavedWave(const std::string& style) {
  if (style == "suave") return MakeWave({{659.25f, 0.09f}, {783.99f, 0.12f}}, Wave::Sine, 0.14f);
  if (style == "moneda") return MakeWave({{987.77f, 0.06f}, {1318.51f, 0.22f}}, Wave::Soft, 0.16f);
  if (style == "arcade")
    return MakeWave({{523.25f, 0.045f}, {659.25f, 0.045f}, {783.99f, 0.045f}, {1046.5f, 0.09f}}, Wave::Soft, 0.16f);
  if (style == "campana") return MakeWave({{1567.98f, 0.45f}}, Wave::Bell, 0.24f);
  if (style == "fantasma") return MakeWave({{880.0f, 0.18f}, {440.0f, 0.16f}}, Wave::Glide, 0.15f);
  return MakeWave({{880.0f, 0.07f}, {1318.5f, 0.07f}}, Wave::Sine, 0.22f);
}

} // namespace

const std::vector<std::pair<std::string, const char*>>& SaveSounds() {
  static const std::vector<std::pair<std::string, const char*>> sounds = {
      {"clasico", "Clásico"}, {"suave", "Suave"},     {"moneda", "Moneda"},
      {"arcade", "Arcade"},   {"campana", "Campana"}, {"fantasma", "Fantasma"}};
  return sounds;
}

bool IsKnownSaveSound(const std::string& id) {
  const auto& sounds = SaveSounds();
  return std::any_of(sounds.begin(), sounds.end(), [&](const auto& s) { return s.first == id; });
}

void PlayCue(Cue cue, const std::string& style) {
  // PlaySound con SND_ASYNC lee el búfer mientras suena: tiene que seguir vivo.
  // Se generan al primer uso y se quedan (unos 40 KB el más largo). Solo se
  // llama desde el hilo de la interfaz.
  static std::map<std::string, std::vector<uint8_t>> saved;
  static const std::vector<uint8_t> marker = MakeWave({{1046.5f, 0.06f}}, Wave::Sine, 0.18f);
  static const std::vector<uint8_t> voice = MakeWave({{587.33f, 0.05f}, {783.99f, 0.07f}}, Wave::Soft, 0.16f);
  const std::string id = IsKnownSaveSound(style) ? style : "clasico";
  if (cue == Cue::Saved && !saved.count(id)) saved[id] = SavedWave(id);
  const std::vector<uint8_t>& wav = cue == Cue::Saved ? saved[id] : cue == Cue::Voice ? voice : marker;
  PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace spectre
