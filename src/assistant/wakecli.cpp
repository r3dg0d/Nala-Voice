#include "wakecli.h"
#include "audio.h"
#include "audioutil.h"
#include "wakeword.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <ctime>

namespace {

// openWakeWord v0.5.1's feature models, pinned by hash.
struct Asset {
  const char *name;
  const char *url;
  const char *sha256;
};
// openWakeWord v0.5.1's feature models, and its published negative
// features (~185 MB), pinned by hash.
constexpr Asset kAssets[] = {
    {"melspectrogram.onnx",
     "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/"
     "melspectrogram.onnx",
     "ba2b0e0f8b7b875369a2c89cb13360ff53bac436f2895cced9f479fa65eb176f"},
    {"embedding_model.onnx",
     "https://github.com/dscripka/openWakeWord/releases/download/v0.5.1/"
     "embedding_model.onnx",
     "70d164290c1d095d1d4ee149bc5e00543250a7316b59f31d056cff7bd3075c1f"},
    {"validation_set_features.npy",
     "https://huggingface.co/datasets/davidscripka/openwakeword_features/"
     "resolve/main/validation_set_features.npy",
     "a56a8a0f8e0efb91900acc6de4c0cdf4c564842e8475a7d49b36c039e17a690f"},
};

QTextStream &out() {
  static QTextStream stream(stdout);
  return stream;
}
QTextStream &err() {
  static QTextStream stream(stderr);
  return stream;
}

QString featureDir() {
  const QString env = qEnvironmentVariable("NALA_WAKE_FEATURES");
  return env.isEmpty() ? wake::Features::defaultDir() : env;
}

} // namespace

bool readClip(const QString &path, wake::Clip *clip, QString *error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    *error = file.errorString();
    return false;
  }
  const QByteArray bytes = file.readAll();
  const audio::WavInfo info = audio::parseWav(bytes);
  if (!info.ok || info.bitsPerSample != 16) {
    *error = QStringLiteral("%1 is not 16-bit PCM WAV").arg(path);
    return false;
  }
  const QByteArray raw = bytes.mid(info.dataOffset);
  const int frames = int(raw.size() / 2 / info.channels);
  *clip = audio::toMono16k(reinterpret_cast<const int16_t *>(raw.constData()),
                           frames, info.channels, info.sampleRate);
  return true;
}

bool installFeatureModels(const QString &dir, QString *error,
                          std::function<void(QString)> progress) {
  QDir().mkpath(dir);
  QNetworkAccessManager network;
  for (const Asset &asset : kAssets) {
    const QString target = dir + "/" + asset.name;
    if (QFileInfo::exists(target)) {
      QFile existing(target);
      if (existing.open(QIODevice::ReadOnly) &&
          QCryptographicHash::hash(existing.readAll(), QCryptographicHash::Sha256)
                  .toHex() == asset.sha256)
        continue;
    }
    if (progress)
      progress(QStringLiteral("downloading %1").arg(asset.name));
    QNetworkRequest request(QUrl(QString::fromLatin1(asset.url)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    QNetworkReply *reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      *error = reply->errorString();
      return false;
    }
    const QByteArray bytes = reply->readAll();
    // Only exactly the files that were reviewed are accepted.
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
        asset.sha256) {
      *error = QStringLiteral("%1 does not match its expected checksum")
                   .arg(asset.name);
      return false;
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
      *error = QStringLiteral("could not write %1").arg(target);
      return false;
    }
  }
  return true;
}

namespace {

int usage() {
  err() << "usage: nala wakeword <command>\n"
           "  setup                         download the feature models "
           "(openWakeWord v0.5.1)\n"
           "  train <phrase> <yes.wav>... [--negative <no.wav>...] "
           "[--synthetic <other-voice.wav>...]\n"
           "  eval <phrase> <file.wav>...   score recordings, print detections\n"
           "  test [phrase]                 live: the confidence meter\n"
           "  list                          trained phrases\n"
           "  delete <phrase>               remove a phrase and its recordings\n";
  return 2;
}

int setup() {
  const QString dir = featureDir();
  QString error;
  out() << "Nala's wake-word detector uses openWakeWord's feature models\n"
           "(https://github.com/dscripka/openWakeWord) and ~11 hours of its "
           "published\nnegative features for training (about 190 MB in all). "
           "The embedding model\nderives from Google's speech_embedding "
           "(Apache-2.0); openWakeWord distributes\nits models and features "
           "under CC BY-NC-SA 4.0. Personal, non-commercial use\nis fine. "
           "Nothing is uploaded; these only come down.\n\n";
  out().flush();
  if (!installFeatureModels(dir, &error,
                            [](const QString &s) { out() << s << "\n"; out().flush(); })) {
    err() << "failed: " << error << "\n";
    return 1;
  }
  out() << "installed in " << dir << "\n";
  return 0;
}

int train(const QStringList &args) {
  if (args.size() < 2)
    return usage();
  const QString phrase = args[0];
  QVector<wake::Clip> positives, negatives, synthetic;
  QVector<wake::Clip> *into = &positives;
  for (const QString &arg : args.mid(1)) {
    if (arg == "--negative") {
      into = &negatives;
      continue;
    }
    if (arg == "--synthetic") {
      into = &synthetic;
      continue;
    }
    wake::Clip clip;
    QString error;
    if (!readClip(arg, &clip, &error)) {
      err() << error << "\n";
      return 1;
    }
    *into << clip;
  }
  wake::Features features;
  QString error;
  if (!features.load(featureDir(), &error)) {
    err() << error << "\n";
    return 1;
  }
  wake::NegativeBank bank;
  if (!bank.load(featureDir(), &error))
    err() << "note: " << error << " -- training without general negative "
             "data will false-trigger on other voices\n";
  wake::Trainer trainer(&features, &bank);
  wake::TrainReport report;
  const wake::Model model =
      trainer.train(phrase, positives, negatives, &report, 1, synthetic);
  if (!report.ok) {
    err() << "training failed: " << report.error << "\n";
    return 1;
  }
  if (!wake::saveModel(model, &error)) {
    err() << error << "\n";
    return 1;
  }
  out() << QStringLiteral("trained \"%1\" in %2 ms: %3 positive and %4 negative "
                          "windows, threshold %5, held-out recall %6, "
                          "held-out false accepts %7%, %8 false wakes/hour "
                          "on %9 h of unseen audio\n")
               .arg(model.phrase)
               .arg(report.milliseconds)
               .arg(report.positives)
               .arg(report.negatives)
               .arg(report.threshold, 0, 'f', 2)
               .arg(report.recall < 0 ? QStringLiteral("n/a")
                                      : QString::number(report.recall, 'f', 2))
               .arg(report.falseAcceptRate * 100.0, 0, 'f', 3)
               .arg(report.falseAcceptsPerHour < 0
                        ? QStringLiteral("n/a")
                        : QString::number(report.falseAcceptsPerHour, 'f', 2))
               .arg(report.benchmarkHours, 0, 'f', 1);
  return 0;
}

int eval(const QStringList &args) {
  if (args.size() < 2)
    return usage();
  const wake::Model model = wake::loadModel(args[0]);
  if (!model.valid()) {
    err() << "no trained model for \"" << args[0] << "\"\n";
    return 1;
  }
  for (const QString &path : args.mid(1)) {
    wake::Clip clip;
    QString error;
    if (!readClip(path, &clip, &error)) {
      err() << error << "\n";
      return 1;
    }
    wake::NeuralBackend backend(featureDir());
    if (!backend.initialize(&error)) {
      err() << error << "\n";
      return 1;
    }
    backend.registerWakeword(model);
    backend.start();
    float best = 0.0f;
    QStringList hits;
    QObject::connect(&backend, &wake::WakeWordBackend::scored,
                     [&](const QString &, float s, float) { best = std::max(best, s); });
    QObject::connect(&backend, &wake::WakeWordBackend::detected,
                     [&](const wake::Detection &d) {
                       hits << QStringLiteral("%1s (%2, %3 ms)")
                                   .arg(backend.audioClock() / 1000.0, 0, 'f', 2)
                                   .arg(d.score, 0, 'f', 2)
                                   .arg(d.latencyMs);
                     });
    QElapsedTimer clock;
    clock.start();
    // Two seconds of quiet first, so the stream is settled as it would be live.
    const wake::Clip quiet = wake::augment::pinkNoise(wake::kRate * 2, 0.002, 3);
    backend.processAudio(quiet.constData(), int(quiet.size()));
    backend.processAudio(clip.constData(), int(clip.size()));
    const qint64 ms = clock.elapsed();
    const double audioSeconds = (quiet.size() + clip.size()) / double(wake::kRate);
    out() << QStringLiteral("%1: best %2, threshold %3, %4 detection(s)%5; "
                            "%6 ms for %7 s of audio (%8x real time)\n")
                 .arg(QFileInfo(path).fileName())
                 .arg(best, 0, 'f', 3)
                 .arg(model.threshold, 0, 'f', 2)
                 .arg(hits.size())
                 .arg(hits.isEmpty() ? QString() : " at " + hits.join(", "))
                 .arg(ms)
                 .arg(audioSeconds, 0, 'f', 1)
                 .arg(audioSeconds * 1000.0 / std::max<qint64>(1, ms), 0, 'f', 0);
  }
  return 0;
}

volatile std::sig_atomic_t g_stop = 0;

int test(const QStringList &args) {
  QStringList phrases = args;
  if (phrases.isEmpty())
    phrases = wake::trainedPhrases();
  wake::NeuralBackend backend(featureDir());
  QString error;
  if (!backend.initialize(&error)) {
    err() << error << "\n";
    return 1;
  }
  for (const QString &phrase : phrases)
    backend.registerWakeword(wake::loadModel(phrase));
  if (backend.wakewords().isEmpty()) {
    err() << "no trained wake phrases; train one in Preferences or with "
             "\"nala wakeword train\"\n";
    return 1;
  }
  backend.start();
  Microphone mic;
  QObject::connect(&mic, &Microphone::frames, &backend,
                   [&](const QVector<int16_t> &samples) {
                     backend.processAudio(samples.constData(), int(samples.size()));
                   });
  QObject::connect(&backend, &wake::WakeWordBackend::scored,
                   [](const QString &phrase, float score, float threshold) {
                     const int width = 40;
                     const int filled = int(score * width);
                     const int mark = int(threshold * width);
                     QString bar;
                     for (int i = 0; i < width; ++i)
                       bar += i < filled ? QChar(0x2588) : i == mark ? '|' : ' ';
                     out() << "\r" << phrase.leftJustified(16, ' ', true) << " ["
                           << bar << "] " << QString::number(score, 'f', 2) << " ";
                     out().flush();
                   });
  QObject::connect(&backend, &wake::WakeWordBackend::detected,
                   [](const wake::Detection &d) {
                     out() << "\n>> detected \"" << d.phrase << "\"  score "
                           << QString::number(d.score, 'f', 2) << ", "
                           << d.latencyMs << " ms to decide\n";
                     out().flush();
                   });
  if (!mic.start({}, {})) {
    err() << "could not open the microphone\n";
    return 1;
  }
  out() << "listening for: " << backend.wakewords().join(", ")
        << " -- Ctrl-C to stop\n";
  out().flush();
  std::signal(SIGINT, [](int) { g_stop = 1; });
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, qApp, [] {
    if (g_stop)
      qApp->quit();
  });
  poll.start(100);
  std::clock_t cpu = std::clock();
  QElapsedTimer wall;
  wall.start();
  qApp->exec();
  const double cpuSeconds = double(std::clock() - cpu) / CLOCKS_PER_SEC;
  out() << "\nCPU " << QString::number(100.0 * cpuSeconds / (wall.elapsed() / 1000.0), 'f', 1)
        << "% of one core while listening\n";
  return 0;
}

} // namespace

int runWakewordCli(const QStringList &args) {
  const QString command = args.value(0);
  const QStringList rest = args.mid(1);
  if (command == "setup")
    return setup();
  if (command == "train")
    return train(rest);
  if (command == "eval")
    return eval(rest);
  if (command == "test")
    return test(rest);
  if (command == "list") {
    for (const QString &phrase : wake::trainedPhrases()) {
      const wake::Model m = wake::loadModel(phrase);
      out() << phrase << "  (threshold " << QString::number(m.threshold, 'f', 2)
            << ", trained " << m.trained.toString(Qt::ISODate) << ")\n";
    }
    return 0;
  }
  if (command == "delete" && !rest.isEmpty())
    return wake::deletePhrase(rest.join(' ')) ? 0 : 1;
  return usage();
}
