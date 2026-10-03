#include "accessibility.h"
#include "guigrounder.h"
#include "guimemory.h"
#include "latency.h"
#include "memory.h"
#include "sentencestream.h"
#include "settings.h"
#include "tts.h"
#include "visualdiff.h"
#include "x2tts.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {
class Gateway : public QObject {
public:
  QWebSocketServer server{"test", QWebSocketServer::NonSecureMode};
  QPointer<QWebSocket> socket;
  QVector<QJsonObject> messages;
  int connections = 0;
  QString session;
  Gateway() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QWebSocketServer::newConnection, this, [this] {
      ++connections;
      socket = server.nextPendingConnection();
      const auto peer = socket;
      connect(
          peer, &QWebSocket::textMessageReceived, this,
          [this, peer](QString text) {
            auto frame = QJsonDocument::fromJson(text.toUtf8()).object();
            messages << frame;
            if (frame["type"] == "start") {
              session = frame["session_id"].toString();
              peer->sendTextMessage(QString::fromUtf8(
                  QJsonDocument(
                      QJsonObject{
                          {"type", "event"},
                          {"event",
                           QJsonObject{
                               {"type", "start"},
                               {"session_id", session},
                               {"audio", QJsonObject{{"encoding", "pcm_s16le"},
                                                     {"sample_rate", 24000},
                                                     {"channels", 1}}}}}})
                      .toJson()));
            }
          });
      connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
    });
  }
  QUrl url() const {
    return QUrl(QString("ws://127.0.0.1:%1/v1/ws").arg(server.serverPort()));
  }
  void event(QString type, QString id = {}) {
    socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(
            QJsonObject{{"type", "event"},
                        {"event", QJsonObject{{"type", type},
                                              {"session_id",
                                               id.isEmpty() ? session : id}}}})
            .toJson()));
  }
};
class Voice : public TextToSpeech {
public:
  using TextToSpeech::TextToSpeech;
  QStringList texts;
  bool streaming = false, failFormat = false;
  int starts = 0;
  QString id;
  int failStarts = 0;
  QString name() const override {
    return !id.isEmpty() ? id : streaming ? "stream" : "fallback";
  }
  bool incremental() const override { return streaming; }
  void beginStream() override {
    ++starts;
    if (failStarts > 0) {
      --failStarts;
      emit failed("offline");
    }
  }
  void pushText(const QString &text) override { texts << text; }
  void finishStream() override { emit done(); }
  void synthesize(const QString &text) override {
    texts << text;
    emit format(24000, 1, 16);
    if (failFormat)
      emit failed("before PCM");
    else {
      emit audio(QByteArray(8, '\0'));
      emit done();
    }
  }
  void stop() override {}
};
} // namespace

class StreamTests : public QObject {
  Q_OBJECT
private slots:
  void clauseCommitsBeforeSentence() {
    SentenceStream s({14, 14, 220, true});
    QCOMPARE(s.feed("Sure, I can op").size(), 0);
    QCOMPARE(s.feed("en Firefox and ").size(), 0);
    QCOMPARE(s.deadline(), QStringList{"Sure, I can open Firefox and"});
    QCOMPARE(s.feed("prepare your report, then "),
             QStringList{"prepare your report,"});
    QCOMPARE(s.flush(), QString("then"));
  }
  void noHalfWordOrUrl() {
    SentenceStream s({8, 8, 24, true});
    QVERIFY(s.feed(QString(50, 'a')).isEmpty());
    QVERIFY(s.deadline().isEmpty());
    QCOMPARE(s.flush(), QString(50, 'a'));
    s.reset();
    QVERIFY(s.feed("Open https://example.test/long/pa").isEmpty());
    QCOMPARE(s.deadline(),
             QStringList{}); // the only complete prefix is below minimum
    QCOMPARE(s.flush(), QString("Open a link"));
  }
  void numbersAndAbbreviations() {
    SentenceStream s({8, 8, 220, true});
    QVERIFY(s.feed("Ask Dr. Jones about 3.5").isEmpty());
    QCOMPARE(s.deadline(), QStringList{"Ask Dr. Jones about"});
    QVERIFY(s.feed(" kilograms and 1,000").isEmpty());
    QCOMPARE(s.flush(), QString("3.5 kilograms and 1,000"));
  }
  void fencesAndReasoning() {
    ThinkFilter filter;
    SentenceStream s({8, 8, 220, true});
    QVERIFY(s.feed(filter.feed("<thi")).isEmpty());
    QVERIFY(
        s.feed(filter.feed("nk>private reasoning</think>```cpp\nsecret code"))
            .isEmpty());
    QVERIFY(s.deadline().isEmpty());
    const auto chunks = s.feed(filter.feed("\n```\nThe answer is ready, now "));
    const auto out = chunks.join(' ') + s.flush();
    QVERIFY(!out.contains("private"));
    QVERIFY(!out.contains("secret"));
    QVERIFY(out.contains("answer"));
  }
  void pcmAndSessionReuse() {
    Gateway g;
    X2Tts t;
    X2Tts::Config config;
    config.endpoint = g.url();
    t.configure(config);
    QSignalSpy formats(&t, &TextToSpeech::format),
        audio(&t, &TextToSpeech::audio), done(&t, &TextToSpeech::done);
    t.beginStream();
    t.pushText("Hello, ");
    t.pushText("world.");
    QTRY_COMPARE(formats.size(), 1);
    QTRY_COMPARE(g.messages.size(), 3);
    QCOMPARE(g.messages[0]["config"].toObject()["input_mode"].toString(),
             QString("token"));
    QCOMPARE(g.messages[0]["config"].toObject()["group_policy"].toString(),
             QString("none"));
    QCOMPARE(g.messages[1]["text"].toString(), QString("Hello, "));
    g.socket->sendBinaryMessage(QByteArray("abc", 3));
    g.socket->sendBinaryMessage(QByteArray("d", 1));
    QTRY_COMPARE(audio.size(), 2);
    QCOMPARE(audio[0][0].toByteArray() + audio[1][0].toByteArray(),
             QByteArray("abcd"));
    t.finishStream();
    QTRY_COMPARE(g.messages.size(), 4);
    g.event("done");
    QTRY_COMPARE(done.size(), 1);
    const auto previous = g.session;
    t.synthesize("Second turn.");
    QTRY_COMPARE(formats.size(), 2);
    QCOMPARE(g.connections, 1);
    QVERIFY(g.session != previous);
    g.event("done", previous);
    QTest::qWait(30);
    QCOMPARE(done.size(), 1);
    g.socket->sendBinaryMessage(QByteArray(2, '\0'));
    t.finishStream();
    g.event("done");
    QTRY_COMPARE(done.size(), 2);
  }
  void cancelDropsLateAudio() {
    Gateway g;
    X2Tts t;
    X2Tts::Config c;
    c.endpoint = g.url();
    t.configure(c);
    QSignalSpy formats(&t, &TextToSpeech::format),
        audio(&t, &TextToSpeech::audio), done(&t, &TextToSpeech::done);
    t.beginStream();
    t.pushText("Cancel this.");
    QTRY_COMPARE(formats.size(), 1);
    t.stop();
    if (g.socket)
      g.socket->sendBinaryMessage(QByteArray(100, '\0'));
    QTest::qWait(50);
    QCOMPARE(audio.size(), 0);
    QCOMPARE(done.size(), 0);
    t.beginStream();
    t.pushText("New turn.");
    QTRY_COMPARE(formats.size(), 2);
    QCOMPARE(g.connections, 2);
  }
  void disconnectFails() {
    Gateway g;
    X2Tts t;
    X2Tts::Config c;
    c.endpoint = g.url();
    t.configure(c);
    QSignalSpy formats(&t, &TextToSpeech::format),
        failed(&t, &TextToSpeech::failed);
    t.synthesize("Hello.");
    QTRY_COMPARE(formats.size(), 1);
    g.socket->close();
    QTRY_COMPARE(failed.size(), 1);
  }
  void emptyAudioFails() {
    Gateway g;
    X2Tts t;
    X2Tts::Config c;
    c.endpoint = g.url();
    t.configure(c);
    QSignalSpy formats(&t, &TextToSpeech::format),
        failed(&t, &TextToSpeech::failed);
    t.synthesize("Hello.");
    QTRY_COMPARE(formats.size(), 1);
    g.event("done");
    QTRY_COMPARE(failed.size(), 1);
  }
  void incrementalFallbackBuffersFutureText() {
    Voice primary, fallback;
    primary.streaming = true;
    TtsChain chain;
    chain.setEngines({&primary, &fallback});
    QSignalSpy done(&chain, &TextToSpeech::done);
    chain.beginStream();
    chain.pushText("Hello ");
    emit primary.format(24000, 1, 16);
    emit primary.failed("offline");
    QVERIFY(fallback.texts.isEmpty());
    chain.pushText("world.");
    chain.finishStream();
    QCOMPARE(fallback.texts, QStringList{"Hello world."});
    QCOMPARE(done.size(), 1);
  }
  void streamRetriesWhenEveryEngineIsCoolingDown() {
    Voice primary, fallback;
    primary.streaming = fallback.streaming = true;
    primary.id = "primary";
    fallback.id = "fallback";
    primary.failStarts = 1;
    TtsChain chain;
    chain.setCooldownMs(60000);
    chain.setEngines({&primary, &fallback});
    QSignalSpy failed(&chain, &TextToSpeech::failed);
    QSignalSpy done(&chain, &TextToSpeech::done);

    // One engine is still up: skip the cooled primary. Do not clear it.
    chain.beginStream();
    QCOMPARE(failed.size(), 0);
    QCOMPARE(primary.starts, 1);
    QCOMPARE(fallback.starts, 1);
    QCOMPARE(chain.downEngines(), QStringList{"primary"});
    chain.pushText("Hello.");
    QCOMPARE(primary.texts, QStringList{});
    QCOMPARE(fallback.texts, QStringList{"Hello."});
    chain.finishStream();
    QCOMPARE(done.size(), 1);

    chain.beginStream();
    QCOMPARE(primary.starts, 1);
    QCOMPARE(fallback.starts, 2);
    QCOMPARE(chain.downEngines(), QStringList{"primary"});
    chain.finishStream();

    // Now every engine is cooling down. synthesize would retry rather than
    // stay silent; a new stream must use that same policy.
    fallback.failStarts = 1;
    done.clear();
    chain.beginStream();
    QCOMPARE(failed.size(), 1);
    QCOMPARE(primary.starts, 1);
    QCOMPARE(fallback.starts, 3);
    const QStringList bothDown{"primary", "fallback"};
    QCOMPARE(chain.downEngines(), bothDown);

    failed.clear();
    chain.beginStream();
    QCOMPARE(failed.size(), 0);
    QCOMPARE(primary.starts, 2);
    QCOMPARE(chain.downEngines(), QStringList{});
    chain.pushText("Again.");
    QCOMPARE(primary.texts, QStringList{"Again."});
    QCOMPARE(fallback.texts, QStringList{"Hello."});
  }
  // A probe that found only the fallback up must not reopen the dead primary.
  void probedFallbackCooldownSkipsDeadPrimary() {
    Voice primary, fallback;
    primary.streaming = fallback.streaming = true;
    primary.id = "primary";
    fallback.id = "fallback";
    primary.failStarts = 1;
    fallback.failStarts = 1;
    TtsChain chain;
    chain.setCooldownMs(60000);
    chain.setEngines({&primary, &fallback});
    QSignalSpy failed(&chain, &TextToSpeech::failed);
    chain.beginStream();
    QCOMPARE(failed.size(), 1);
    QCOMPARE(primary.starts, 1);
    QCOMPARE(fallback.starts, 1);
    QCOMPARE(chain.downEngines(), QStringList({"primary", "fallback"}));

    chain.releaseEngineCooldown("fallback");
    QCOMPARE(chain.downEngines(), QStringList{"primary"});

    failed.clear();
    chain.beginStream();
    QCOMPARE(failed.size(), 0);
    QCOMPARE(primary.starts, 1);
    QCOMPARE(fallback.starts, 2);
    QCOMPARE(chain.downEngines(), QStringList{"primary"});
  }
  void neverReplayAfterPcm() {
    Voice primary, fallback;
    primary.streaming = true;
    TtsChain chain;
    chain.setEngines({&primary, &fallback});
    QSignalSpy failed(&chain, &TextToSpeech::failed);
    chain.beginStream();
    chain.pushText("Hello ");
    emit primary.audio(QByteArray(2, '\0'));
    emit primary.failed("lost");
    QCOMPARE(failed.size(), 1);
    QVERIFY(fallback.texts.isEmpty());
  }
  void accessibilitySelection() {
    using accessibility::Element;
    const Element button{"Export",  "push button",         "app",
                         "/button", QRect(20, 20, 80, 20), true,
                         true};
    QCOMPARE(
        accessibility::select({button}, "Export button", QRect(0, 0, 200, 100))
            .bounds,
        button.bounds);
    QVERIFY(!accessibility::select({button, button}, "Export",
                                   QRect(0, 0, 200, 100))
                 .error.isEmpty());
    auto disabled = button;
    disabled.enabled = false;
    QVERIFY(accessibility::select({disabled}, "Export", QRect(0, 0, 200, 100))
                .bounds.isEmpty());
    QVERIFY(accessibility::select({button}, "Export", QRect(0, 0, 20, 20))
                .bounds.isEmpty());
  }
  void tinyVisualDelta() {
    QImage a(3440, 1440, QImage::Format_RGB32);
    a.fill(Qt::white);
    auto b = a;
    for (int y = 80; y < 88; ++y)
      for (int x = 2800; x < 2808; ++x)
        b.setPixelColor(x, y, Qt::black);
    const auto delta = visualdiff::measure(a, b);
    QVERIFY(delta.bounds.contains(QPoint(2804, 84)));
    QVERIFY(delta.changedFraction > 0);
    QVERIFY(visualdiff::measure(a, a).bounds.isEmpty());
    const auto pair = visualdiff::evidence(a, b, delta, QPoint(2804, 84));
    QVERIFY(!pair.isNull());
    QVERIFY(pair.width() <= 1280);
  }
  void workflowMemoryUsesSemanticsAndForget() {
    QTemporaryDir dir;
    MemoryStore store;
    QVERIFY(store.open(dir.path()));
    QJsonArray steps{
        guimemory::semanticStep("Settings", "Settings panel opened"),
        guimemory::semanticStep("Voice", "Voice preferences opened")};
    QVERIFY(guimemory::validSteps(steps));
    const auto id =
        guimemory::workflow(store, "change microphone", "Discord", steps);
    QVERIFY(id > 0);
    QCOMPARE(guimemory::steps(store, id, "Discord"), steps);
    QVERIFY(guimemory::steps(store, id, "Firefox").isEmpty());
    auto brittle = steps;
    auto step = brittle[0].toObject();
    step.insert("x", 12);
    brittle[0] = step;
    QVERIFY(!guimemory::validSteps(brittle));
    QCOMPARE(
        guimemory::workflow(store, "change microphone", "Discord", brittle),
        qint64(0));
    QVERIFY(!guimemory::candidates(store, "microphone", "Discord").isEmpty());
    QVERIFY(store.forgetOne(id) > 0);
    QVERIFY(guimemory::steps(store, id, "Discord").isEmpty());
  }
  void accessibilityBeforeVisionAndStableEvidence() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int lookups = 0, captures = 0, inputs = 0, models = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto callback) {
      ++captures;
      callback(GuiGrounder::Frame{image, image.rect(), "window"}, {});
    };
    ports.accessible = [&](QString, auto callback) {
      ++lookups;
      callback(QRect(20, 20, 60, 20), {});
    };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      ++inputs;
      image.fill(Qt::blue);
      return true;
    };
    ports.predict = [&](QImage evidence, QString prompt, auto callback) {
      ++models;
      QVERIFY(!evidence.isNull());
      if (prompt.startsWith("Describe"))
        callback({{"change", "A blue panel appeared"}});
      else {
        QVERIFY(prompt.startsWith("Verify"));
        callback({{"success", true}});
      }
    };
    GuiGrounder g(ports, {});
    QJsonObject result;
    g.start("Export", "A blue panel appears", [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(inputs, 1);
    QCOMPARE(lookups, 2);
    QCOMPARE(models, 2);
    QVERIFY(captures >= 6);
    QCOMPARE(result["grounding_method"].toString(), QString("at-spi"));
  }
  void staleAccessibilityAndCancelPreventInput() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int lookups = 0, clicks = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb(GuiGrounder::Frame{image, image.rect(), "window"}, {});
    };
    ports.accessible = [&](QString, auto cb) {
      ++lookups;
      cb(QRect(20 + lookups, 20, 60, 20), {});
    };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      ++clicks;
      return true;
    };
    GuiGrounder g(ports, {});
    QJsonObject r;
    g.start("Export", "Panel opened", [&](auto value) { r = value; });
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(clicks, 0);
  }
  void verificationFailureStopsTyping() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int typed = 0, clicked = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb(GuiGrounder::Frame{image, image.rect(), "window"}, {});
    };
    ports.accessible = [](QString, auto cb) { cb(QRect(20, 20, 60, 20), {}); };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      ++clicked;
      image.fill(Qt::blue);
      return true;
    };
    ports.type = [&](QString *) {
      ++typed;
      return true;
    };
    ports.predict = [](QImage, QString prompt, auto cb) {
      if (prompt.startsWith("Describe"))
        cb({{"change", "An unrelated panel changed"}});
      else
        cb({{"success", false}});
    };
    GuiGrounder g(ports, {});
    QJsonObject r;
    g.start("Name field", "Field focused", [&](auto result) { r = result; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 2000);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(clicked, 1);
    QCOMPARE(typed, 0);
  }
  void cancelDuringStableWait() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int inputs = 0, models = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb({image, image.rect(), "window", "app"}, {});
    };
    ports.accessible = [](QString, auto cb) { cb(QRect(20, 20, 60, 20), {}); };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      ++inputs;
      image.fill(Qt::blue);
      return true;
    };
    ports.predict = [&](QImage, QString, auto cb) {
      ++models;
      cb({{"success", true}});
    };
    GuiGrounder g(ports, {});
    QJsonObject r;
    g.start("Export", "Dialog opens", [&](auto value) { r = value; });
    QCOMPARE(inputs, 1);
    g.cancel();
    QTest::qWait(350);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(inputs, 1);
    QCOMPARE(models, 0);
  }
  void unstableUiDoesNotRepeat() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    bool acted = false;
    int samples = 0, inputs = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      if (acted)
        image.fill(++samples % 2 ? Qt::blue : Qt::red);
      cb({image, image.rect(), "window", "app"}, {});
    };
    ports.accessible = [](QString, auto cb) { cb(QRect(20, 20, 60, 20), {}); };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      acted = true;
      ++inputs;
      return true;
    };
    GuiGrounder::Options opts;
    opts.stableIntervalMs = 30;
    opts.stableTimeoutMs = 150;
    GuiGrounder g(ports, opts);
    QJsonObject r;
    g.start("Export", "Dialog opens", [&](auto value) { r = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 1000);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(inputs, 1);
    QVERIFY(r["error"].toString().contains("stabilize"));
  }
  void keyboardAndTypingVerified() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int keys = 0, typed = 0, models = 0;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb({image, image.rect(), "window", "browser"}, {});
    };
    ports.key = [&](QString *) {
      ++keys;
      image.fill(Qt::blue);
      return true;
    };
    ports.type = [&](QString *) {
      ++typed;
      image.fill(Qt::green);
      return true;
    };
    ports.predict = [&](QImage, QString prompt, auto cb) {
      ++models;
      if (prompt.startsWith("Describe"))
        cb({{"change", typed ? "Text appears in address field"
                             : "Address field focused"}});
      else
        cb({{"success", true}});
    };
    GuiGrounder::Options opts;
    opts.stableIntervalMs = 30;
    opts.stableSamples = 2;
    opts.typedExpected = "example.test";
    GuiGrounder g(ports, opts);
    QJsonObject r;
    g.start("Address bar", "Address field focused",
            [&](auto value) { r = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 1500);
    QVERIFY(r["ok"].toBool());
    QVERIFY(r["typed"].toBool());
    QCOMPARE(keys, 1);
    QCOMPARE(typed, 1);
    QCOMPARE(models, 4);
    QCOMPARE(r["grounding_method"].toString(), QString("keyboard"));
  }
  void relativeCorrectionAndBounds() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    int predictions = 0, inputs = 0;
    QPoint cursor;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb({image, QRect(-200, 40, 400, 200), "window"}, {});
    };
    ports.point = [&](QPoint p) {
      cursor = p;
      return true;
    };
    ports.click = [&](QString *) {
      ++inputs;
      return true;
    };
    ports.predict = [&](QImage, QString, auto cb) {
      if (++predictions == 1)
        cb({{"x", 80}, {"y", 40}, {"confidence", .7}, {"ready", false}});
      else if (predictions == 2)
        cb({{"dx", 12}, {"dy", -4}, {"confidence", .95}, {"ready", false}});
      else
        cb({{"dx", 0}, {"dy", 0}, {"confidence", .97}, {"ready", true}});
    };
    GuiGrounder::Options opts;
    opts.crop = false;
    opts.verify = false;
    GuiGrounder g(ports, opts);
    QJsonObject r;
    g.start("Tiny icon", "", [&](auto value) { r = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 1000);
    QVERIFY(r["ok"].toBool());
    QCOMPARE(inputs, 1);
    QCOMPARE(cursor, QPoint(-16, 112));
    QCOMPARE(predictions, 3);
  }
  void popupInSameAppCanBeVerified() {
    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    bool popup = false;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb({image, image.rect(), popup ? "dialog" : "window", "editor"}, {});
    };
    ports.accessible = [](QString, auto cb) { cb(QRect(20, 20, 60, 20), {}); };
    ports.point = [](QPoint) { return true; };
    ports.click = [&](QString *) {
      popup = true;
      image = QImage(100, 80, QImage::Format_RGB32);
      image.fill(Qt::blue);
      return true;
    };
    ports.predict = [](QImage, QString prompt, auto cb) {
      if (prompt.startsWith("Describe"))
        cb({{"change", "An export dialog appeared"}});
      else
        cb({{"success", true}});
    };
    GuiGrounder::Options opts;
    opts.stableIntervalMs = 30;
    opts.stableSamples = 2;
    GuiGrounder g(ports, opts);
    QJsonObject r;
    g.start("Export", "Export dialog opened", [&](auto value) { r = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 1000);
    QVERIFY(r["ok"].toBool());
    QCOMPARE(r["actions"].toInt(), 1);
  }
  void settingsAndElapsedMetrics() {
    QTemporaryDir dir;
    AssistantSettings s(dir.filePath("config.json"), false);
    QVERIFY(s.set("tts.engine", "x2streaming"));
    QVERIFY(!s.set("tts.x2.endpoint", "http://localhost"));
    QVERIFY(s.set("tts.x2.endpoint", "ws://127.0.0.1:50052/v1/ws"));
    QVERIFY(!s.set("agent.gui.stableSamples", 0));
    LatencyTrace latency;
    latency.set(LatencyTrace::FirstPcm, 372);
    latency.set(LatencyTrace::TtsFirstAudio, 83);
    QCOMPARE(latency.perceived(), qint64(372));
    latency.reset();
    QVERIFY(!latency.has(LatencyTrace::FirstPcm));
  }
};
QTEST_MAIN(StreamTests)
#include "stream_tests.moc"
