#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
// Deliberately isolated from libobs and encoders. The GUI owns this process and
// its temporary path, so cancelling never risks deleting the original MKV.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() != 4)
    return 2;
  const auto input = QFileInfo(args[1]), output = QFileInfo(args[2]),
             temporary = QFileInfo(args[3]);
  if (!input.isAbsolute() || !output.isAbsolute() || !temporary.isAbsolute() ||
      input.suffix().toLower() != "mkv" || output.suffix().toLower() != "mp4" ||
      input.absoluteFilePath() == output.absoluteFilePath() ||
      QFile::exists(output.absoluteFilePath()) ||
      QFile::exists(temporary.absoluteFilePath()) ||
      output.absolutePath() != temporary.absolutePath())
    return 2;
  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  AVPacket *packet = nullptr;
  bool header = false, success = false;
  const auto inPath = input.absoluteFilePath().toUtf8(),
             tempPath = temporary.absoluteFilePath().toUtf8();
  do {
    if (avformat_open_input(&in, inPath.constData(),
                            av_find_input_format("matroska"), nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0)
      break;
    if (avformat_alloc_output_context2(&out, nullptr, "mp4",
                                       tempPath.constData()) < 0 ||
        !out || !in->nb_streams)
      break;
    bool supported = true;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      auto *source = in->streams[i];
      if ((source->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
           source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) ||
          avformat_query_codec(out->oformat, source->codecpar->codec_id,
                               FF_COMPLIANCE_NORMAL) <= 0) {
        supported = false;
        break;
      }
      auto *destination = avformat_new_stream(out, nullptr);
      if (!destination || avcodec_parameters_copy(destination->codecpar,
                                                  source->codecpar) < 0) {
        supported = false;
        break;
      }
      destination->codecpar->codec_tag = 0;
      destination->time_base = source->time_base;
    }
    if (!supported)
      break;
    out->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    if (avio_open(&out->pb, tempPath.constData(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(out, nullptr) < 0)
      break;
    header = true;
    packet = av_packet_alloc();
    if (!packet)
      break;
    int result;
    while ((result = av_read_frame(in, packet)) >= 0) {
      const auto i = packet->stream_index;
      av_packet_rescale_ts(packet, in->streams[i]->time_base,
                           out->streams[i]->time_base);
      packet->pos = -1;
      result = av_interleaved_write_frame(out, packet);
      av_packet_unref(packet);
      if (result < 0)
        break;
    }
    success = result == AVERROR_EOF && av_write_trailer(out) >= 0;
  } while (false);
  av_packet_free(&packet);
  if (out) {
    if (out->pb && avio_closep(&out->pb) < 0)
      success = false;
    avformat_free_context(out);
  }
  avformat_close_input(&in);
  if (!header || !success ||
      !QFile::rename(temporary.absoluteFilePath(), output.absoluteFilePath())) {
    QFile::remove(temporary.absoluteFilePath());
    return 1;
  }
  return 0;
}
