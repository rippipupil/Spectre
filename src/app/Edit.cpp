#include "app/Edit.h"

#include "common/Util.h"
#include "core/Mp4Writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace spectre {

namespace {

// Un MP4 abierto, con sus pistas de vídeo y audio en el orden de Mp4Writer
// (el vídeo primero), como en TrimFile.
struct Input {
  AVFormatContext* fmt = nullptr;
  int video = -1;
  std::vector<int> map; // pista del archivo -> pista de salida (-1 = se ignora)
  StreamList streams;

  Input() = default;
  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;
  ~Input() { Close(); }
  void Close() {
    if (fmt) avformat_close_input(&fmt);
    map.clear();
    streams.clear();
    video = -1;
  }

  // videoOnly: solo la pista de vídeo (cámara lenta).
  bool Open(const std::string& path, bool videoOnly, std::string* error) {
    Close();
    std::string name = Narrow(std::filesystem::path(Widen(path)).filename().wstring());
    int err = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (err < 0) {
      *error = "No se pudo abrir " + name + ": " + AvError(err);
      return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) {
      *error = name + " no tiene vídeo";
      return false;
    }
    map.assign(fmt->nb_streams, -1);
    auto add = [&](unsigned i) {
      AVStream* st = fmt->streams[i];
      if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return;
      if (videoOnly && int(i) != video) return;
      StreamDesc d;
      AVCodecParameters* par = avcodec_parameters_alloc();
      avcodec_parameters_copy(par, st->codecpar);
      d.params = std::shared_ptr<AVCodecParameters>(par, CodecParDeleter{});
      d.timeBase = st->time_base;
      d.video = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
      if (auto* t = av_dict_get(st->metadata, "title", nullptr, 0)) d.title = t->value;
      else if (auto* h = av_dict_get(st->metadata, "handler_name", nullptr, 0)) d.title = h->value;
      map[i] = int(streams.size());
      streams.push_back(std::move(d));
    };
    add(unsigned(video));
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
      if (int(i) != video) add(i);
    return true;
  }

  // Duración de un fotograma de vídeo (s), para el último del archivo.
  double FrameSeconds() const {
    AVRational r = fmt->streams[video]->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) r = fmt->streams[video]->r_frame_rate;
    return r.num > 0 && r.den > 0 ? double(r.den) / double(r.num) : 1.0 / 60.0;
  }
};

// Las dos pistas se pueden poner una detrás de otra sin recodificar: mismo
// códec, mismo tamaño o formato de audio y la misma cabecera del códec (en
// H.264, SPS y PPS).
bool SameStream(const AVCodecParameters* a, const AVCodecParameters* b) {
  if (a->codec_type != b->codec_type || a->codec_id != b->codec_id) return false;
  if (a->codec_type == AVMEDIA_TYPE_VIDEO && (a->width != b->width || a->height != b->height || a->format != b->format))
    return false;
  if (a->codec_type == AVMEDIA_TYPE_AUDIO &&
      (a->sample_rate != b->sample_rate || a->ch_layout.nb_channels != b->ch_layout.nb_channels))
    return false;
  return a->extradata_size == b->extradata_size &&
         (a->extradata_size == 0 || memcmp(a->extradata, b->extradata, size_t(a->extradata_size)) == 0);
}

std::string FileName(const std::string& path) { return Narrow(std::filesystem::path(Widen(path)).filename().wstring()); }

} // namespace

bool JoinFiles(const std::vector<std::string>& inputs, const std::string& output, std::string* error) {
  std::string scratch;
  if (!error) error = &scratch;
  if (inputs.size() < 2) {
    *error = "Elige al menos dos vídeos para unir";
    return false;
  }
  // Primero se comprueba que todos encajan con el primero, sin escribir nada.
  StreamList streams;
  {
    Input first;
    if (!first.Open(inputs[0], false, error)) return false;
    streams = first.streams;
    for (size_t k = 1; k < inputs.size(); ++k) {
      Input in;
      if (!in.Open(inputs[k], false, error)) return false;
      bool same = in.streams.size() == streams.size();
      for (size_t i = 0; same && i < streams.size(); ++i)
        same = SameStream(in.streams[i].params.get(), streams[i].params.get());
      if (!same) {
        *error = FileName(inputs[k]) + " se grabó con otra calidad, resolución o pistas de audio que " +
                 FileName(inputs[0]) + ": solo se pueden unir sin recodificar clips grabados con los mismos ajustes";
        return false;
      }
    }
  }

  Mp4Writer writer;
  if (!writer.Open(output, streams, false)) {
    *error = "No se pudo crear el archivo unido";
    return false;
  }
  AVPacket* pkt = av_packet_alloc();
  bool ok = true;
  double base = 0; // segundos de salida donde empieza el archivo actual
  for (size_t k = 0; ok && k < inputs.size(); ++k) {
    Input in;
    if (!in.Open(inputs[k], false, error)) {
      ok = false;
      break;
    }
    // El primer dts de vídeo del archivo va a `base`: el vídeo de cada parte
    // empieza justo donde acaba el de la anterior, y el audio se mueve igual.
    const AVRational videoTb = in.fmt->streams[in.video]->time_base;
    double first = NAN;
    while (std::isnan(first) && av_read_frame(in.fmt, pkt) >= 0) {
      if (pkt->stream_index == in.video) {
        int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (ts != AV_NOPTS_VALUE) first = double(ts) * av_q2d(videoTb);
      }
      av_packet_unref(pkt);
    }
    if (std::isnan(first)) first = 0;
    // Se vuelve a abrir para leerlo desde el principio: más fiable que buscar.
    if (!in.Open(inputs[k], false, error)) {
      ok = false;
      break;
    }
    const double shift = base - first;
    const double frame = in.FrameSeconds();
    double end = base;
    while (av_read_frame(in.fmt, pkt) >= 0) {
      const int out = pkt->stream_index < int(in.map.size()) ? in.map[size_t(pkt->stream_index)] : -1;
      if (out < 0) {
        av_packet_unref(pkt);
        continue;
      }
      // A la base de tiempo de la salida (la del primer archivo) y desplazado.
      const AVRational tb = streams[size_t(out)].timeBase;
      av_packet_rescale_ts(pkt, in.fmt->streams[pkt->stream_index]->time_base, tb);
      const int64_t off = std::llround(shift / av_q2d(tb));
      if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += off;
      if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += off;
      if (out == 0 && pkt->pts != AV_NOPTS_VALUE) {
        double dur = pkt->duration > 0 ? double(pkt->duration) * av_q2d(tb) : frame;
        end = std::max(end, double(pkt->pts) * av_q2d(tb) + dur);
      }
      if (!writer.Write(pkt, out)) {
        ok = false;
        *error = "Error al escribir el vídeo unido";
        av_packet_unref(pkt);
        break;
      }
      av_packet_unref(pkt);
    }
    base = end;
  }
  av_packet_free(&pkt);
  ok = writer.Close() && ok;
  if (!ok) {
    if (error->empty()) *error = "Error al escribir el vídeo unido";
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(Widen(output)), ec);
    return false;
  }
  Log("Unir: %zu vídeos en %s (%.1f s)", inputs.size(), output.c_str(), base);
  return true;
}

bool SlowMotionFile(const std::string& input, const std::string& output, int factor, std::string* error) {
  std::string scratch;
  if (!error) error = &scratch;
  factor = std::clamp(factor, 2, 8);
  Input in;
  if (!in.Open(input, true, error)) return false;
  Mp4Writer writer;
  if (!writer.Open(output, in.streams, false)) {
    *error = "No se pudo crear la copia en cámara lenta";
    return false;
  }
  AVPacket* pkt = av_packet_alloc();
  bool ok = true;
  while (av_read_frame(in.fmt, pkt) >= 0) {
    if (pkt->stream_index == in.video) {
      // Cada fotograma dura `factor` veces más: el vídeo va más despacio sin
      // tocar los fotogramas.
      if (pkt->pts != AV_NOPTS_VALUE) pkt->pts *= factor;
      if (pkt->dts != AV_NOPTS_VALUE) pkt->dts *= factor;
      pkt->duration *= factor;
      if (!writer.Write(pkt, 0)) {
        ok = false;
        av_packet_unref(pkt);
        break;
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  ok = writer.Close() && ok;
  if (!ok) {
    *error = "Error al escribir la copia en cámara lenta";
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(Widen(output)), ec);
    return false;
  }
  Log("Cámara lenta x1/%d: %s", factor, output.c_str());
  return true;
}

} // namespace spectre
