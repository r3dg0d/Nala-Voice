#include "ttsbenchmark.h"
#include "x2tts.h"

TtsBenchmark::TtsBenchmark(QVector<TextToSpeech *> engines,
                           std::function<void(QString)> progress,
                           std::function<void()> done, QObject *parent)
    : QObject(parent), m_progress(std::move(progress)),
      m_done(std::move(done)) {
  m_chain.setEngines(engines);
  for (auto *engine : engines)
    if (auto *x2 = qobject_cast<X2Tts *>(engine))
      connect(x2, &X2Tts::segmentFinished, this, [this](bool inherited) {
        if (m_running) {
          ++m_segments;
          if (inherited)
            ++m_carried;
        }
      });
  m_names = {
      "first request (server cold/warm state must be measured separately)",
      "warm confirmation",
      "conversation",
      "long response",
      "punctuation",
      "numbers",
      "technical",
      "interruption"};
  m_texts = {
      "Ready.",
      "Ready.",
      "I can open Firefox and help you find the settings you need.",
      "The desktop assistant first checks native tools and accessibility "
      "metadata. "
      "When those interfaces cannot identify the requested control, it "
      "observes the focused window, "
      "zooms into the relevant area, and refines the pointer position. "
      "After an action, it waits for the interface to stabilize and compares "
      "the visible changes "
      "against the expected outcome. Each step remains interruptible and "
      "follows your permission settings.",
      "Yes, that works; next, choose Settings. Is the panel open? Great!",
      "The temperature is 25 degrees. The distance is 3.5 kilometers, and the "
      "cost is 1,000 dollars.",
      "The C plus plus service streams PCM audio at 24 kilohertz over a local "
      "WebSocket connection.",
      "This is an interrupted response. " +
          QString("The next sentence should be cancelled. ").repeated(15)};
  m_input.setInterval(25);
  m_timeout.setSingleShot(true);
  connect(&m_input, &QTimer::timeout, this, [this] {
    if (!m_running)
      return;
    const auto text = m_texts[m_case];
    const auto commit = [this](QString piece) {
      if (piece.isEmpty())
        return;
      if (m_firstCommit < 0)
        m_firstCommit = m_clock.elapsed();
      m_lastCommit = m_clock.elapsed();
      m_chain.pushText(piece + ' ');
    };
    if (m_position >= text.size()) {
      m_input.stop();
      commit(m_commitment.flush());
      m_chain.finishStream();
      return;
    }
    const int end = std::min(m_position + 4, int(text.size()));
    for (const auto &piece :
         m_commitment.feed(text.mid(m_position, end - m_position)))
      commit(piece);
    m_position = end;
    if (m_clock.elapsed() - m_lastCommit >= m_maxDelayMs)
      for (const auto &piece : m_commitment.deadline())
        commit(piece);
  });
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    m_chain.stop();
    finishCase("timed out");
  });
  connect(&m_chain, &TextToSpeech::format, this, [this](int r, int c, int b) {
    m_rate = r;
    m_frameBytes = c * b / 8;
  });
  connect(&m_chain, &TextToSpeech::audio, this, [this](QByteArray bytes) {
    if (!m_running)
      return;
    if (m_first < 0 && !bytes.isEmpty()) {
      m_first = m_clock.elapsed();
      if (m_case == m_texts.size() - 1)
        QTimer::singleShot(150, this, [this] {
          if (m_running) {
            m_chain.stop();
            finishCase("cancelled 150 ms after first PCM");
          }
        });
    }
    m_bytes += bytes.size();
  });
  connect(&m_chain, &TextToSpeech::done, this, [this] { finishCase(); });
  connect(&m_chain, &TextToSpeech::failed, this,
          [this](QString error) { finishCase(error); });
}

void TtsBenchmark::start() {
  m_progress("Silent TTS benchmark: first PCM and full synthesis measured "
             "locally; output underruns/physical audible onset: n/a.\n"
             "First request is not a cold model measurement: restart the "
             "service and inspect /nala/diagnostics load_ms for cold startup.\n"
             "case | first commit ms | first PCM ms (input start) | TTS first "
             "PCM ms (commit) | total ms | generated audio seconds | RTF "
             "(includes paced input) | engine");
  runNext();
}
void TtsBenchmark::runNext() {
  if (!m_done)
    return;
  if (++m_case >= m_texts.size()) {
    auto done = std::move(m_done);
    if (done)
      done();
    deleteLater();
    return;
  }
  m_running = true;
  m_position = 0;
  m_first = -1;
  m_bytes = 0;
  m_firstCommit = -1;
  m_lastCommit = 0;
  m_commitment.reset();
  m_clock.start();
  m_timeout.start(60000);
  m_segments = m_carried = 0;
  if (m_case == m_texts.size() - 1)
    QTimer::singleShot(1500, this, [this] {
      if (m_running) {
        m_chain.stop();
        finishCase("cancelled at 1500 ms without completion");
      }
    });
  if (m_chain.incremental()) {
    m_chain.beginStream();
    if (m_running)
      m_input.start();
  } else {
    m_firstCommit = 0;
    m_chain.synthesize(m_texts[m_case]);
  }
}
void TtsBenchmark::finishCase(QString error) {
  if (!m_running)
    return;
  m_running = false;
  m_input.stop();
  m_timeout.stop();
  const auto elapsed = m_clock.elapsed();
  const double seconds = double(m_bytes) / std::max(1, m_rate * m_frameBytes);
  m_progress(
      QString("%1 | %2 | %3 | %4 | %5 | %6 | %7 | %8%9")
          .arg(m_names[m_case])
          .arg(m_firstCommit)
          .arg(m_first)
          .arg(m_first < 0 ? -1 : m_first - std::max<qint64>(0, m_firstCommit))
          .arg(elapsed)
          .arg(seconds, 0, 'f', 3)
          .arg(seconds > 0 ? elapsed / (1000. * seconds) : 0, 0, 'f', 3)
          .arg(m_chain.lastEngine().isEmpty() ? m_chain.name()
                                              : m_chain.lastEngine(),
               error.isEmpty() ? QString() : " (" + error + ")"));
  m_progress(
      QString(
          "  X2 completed segments: %1; healthy state snapshots carried: %2")
          .arg(m_segments)
          .arg(m_carried));
  QTimer::singleShot(0, this, [this] { runNext(); });
}
void TtsBenchmark::cancel() {
  m_chain.stop();
  m_input.stop();
  m_timeout.stop();
  m_running = false;
  m_progress("TTS benchmark cancelled.");
  auto done = std::move(m_done);
  if (done)
    done();
  deleteLater();
}
