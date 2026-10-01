#include "ttsbenchmark.h"
#include "x2tts.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QtEndian>
#include <cstdio>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QNetworkAccessManager network;
  auto *x2 = new X2Tts(&app);
  X2Tts::Config streamConfig;
  const auto arguments = app.arguments();
  const int modeArg = arguments.indexOf("--input-mode");
  if (modeArg >= 0)
    streamConfig.inputMode = arguments.value(modeArg + 1);
  if (streamConfig.inputMode != "token" &&
      streamConfig.inputMode != "long_segment")
    return 2;
  x2->configure(streamConfig);
  auto *qwen = new QwenTts(&network, &app);
  QwenTts::Config legacy;
  legacy.endpoint = QUrl("http://127.0.0.1:8880");
  qwen->configure(legacy);
  auto *fish = new FishSpeech(&network, &app);
  fish->configure(QUrl("http://127.0.0.1:8080"), {}, true, {});
  QVector<TextToSpeech *> engines;
  if (app.arguments().contains("--legacy"))
    engines << qwen;
  else
    engines << x2;
  engines << fish;
  QByteArray pcm;
  int rate = 24000, channels = 1, bits = 16, index = 0;
  const auto args = app.arguments();
  const int outputArg = args.indexOf("--audio-dir");
  const QString outputDir = outputArg >= 0 && outputArg + 1 < args.size()
                                ? args[outputArg + 1]
                                : QString();
  if (!outputDir.isEmpty()) {
    QDir().mkpath(outputDir);
    QFile::setPermissions(outputDir, QFile::ReadOwner | QFile::WriteOwner |
                                         QFile::ExeOwner);
    for (auto *engine : engines) {
      QObject::connect(engine, &TextToSpeech::format, &app,
                       [&](int r, int c, int b) {
                         rate = r;
                         channels = c;
                         bits = b;
                       });
      QObject::connect(engine, &TextToSpeech::audio, &app,
                       [&](QByteArray bytes) {
                         if (pcm.size() + bytes.size() <= 16 * 1024 * 1024)
                           pcm += bytes;
                       });
    }
  }
  auto *bench = new TtsBenchmark(
      engines,
      [&](QString line) {
        if (!outputDir.isEmpty() && line.contains(" | ") &&
            !line.startsWith("case |") && !line.contains('\n')) {
          QByteArray wave("RIFF");
          const auto u32 = [&](quint32 v) {
            v = qToLittleEndian(v);
            wave.append(reinterpret_cast<const char *>(&v), 4);
          };
          const auto u16 = [&](quint16 v) {
            v = qToLittleEndian(v);
            wave.append(reinterpret_cast<const char *>(&v), 2);
          };
          u32(36 + pcm.size());
          wave += "WAVEfmt ";
          u32(16);
          u16(1);
          u16(channels);
          u32(rate);
          u32(rate * channels * bits / 8);
          u16(channels * bits / 8);
          u16(bits);
          wave += "data";
          u32(pcm.size());
          wave += pcm;
          QFile file(QDir(outputDir).filePath(QString("%1.wav").arg(index++)));
          if (file.open(QIODevice::WriteOnly)) {
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            file.write(wave);
          }
          pcm.clear();
        }
        std::printf("%s\n", qPrintable(line));
        std::fflush(stdout);
      },
      [&app] { app.quit(); }, &app);
  const int minArg = args.indexOf("--min-chars");
  if (minArg >= 0 && minArg + 1 < args.size()) {
    const int minimum = args[minArg + 1].toInt();
    if (minimum < 8 || minimum > 120)
      return 2;
    bench->configureCommitment({minimum, minimum, 220, true}, 350);
  }
  bench->start();
  return app.exec();
}
