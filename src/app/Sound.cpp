#include "app/Sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace spectre {

namespace {

// WAV PCM de 16 bits mono con una o dos notas seguidas. Cada nota sube y baja
// de volumen en unos milisegundos para que no chasquee.
std::vector<uint8_t> MakeWave(std::initializer_list<float> notes, float noteSeconds, float volume) {
  const int rate = 44100;
  const int perNote = int(float(rate) * noteSeconds);
  const int samples = perNote * int(notes.size());
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
  for (float freq : notes) {
    for (int n = 0; n < perNote; ++n, ++i) {
      float env = 1.0f;
      if (n < fade) env = float(n) / float(fade);
      else if (n > perNote - fade * 4) env = float(perNote - n) / float(fade * 4);
      float v = std::sin(2.0f * pi * freq * float(n) / float(rate)) * env * volume;
      put16(44 + size_t(i) * 2, uint16_t(int16_t(v * 32767.0f)));
    }
  }
  return wav;
}

} // namespace

void PlayCue(Cue cue) {
  // PlaySound con SND_ASYNC lee el búfer mientras suena: tiene que seguir vivo.
  static const std::vector<uint8_t> saved = MakeWave({880.0f, 1318.5f}, 0.07f, 0.22f);
  static const std::vector<uint8_t> marker = MakeWave({1046.5f}, 0.06f, 0.18f);
  const std::vector<uint8_t>& wav = cue == Cue::Saved ? saved : marker;
  PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace spectre
