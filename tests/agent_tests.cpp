#include "assistant.h"
#include "guigrounder.h"
#include "memory.h"
#include "retrieval.h"
#include "semantic.h"
#include "settings.h"
#include "websearch.h"
#include "windowtarget.h"
#include <QDir>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
MemoryRecord record(QString text,
                    QDateTime when = QDateTime::currentDateTime()) {
  MemoryRecord r;
  r.started = r.lastSeen = when;
  r.title = r.summary = text;
  r.source = "agent";
  return r;
}
class Server : public QObject {
public:
  QTcpServer server;
  QJsonObject response{{"embeddings", QJsonArray{QJsonArray{1., 0.}}}};
  QStringList paths;
  int status = 200;
  bool hang = false;
  QByteArray rawBody;
  QByteArray contentType = "application/json";
  Server() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection())
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          const int end = bytes.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          int length = 0;
          for (auto line : bytes.left(end).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < end + 4 + length ||
              socket->property("done").toBool())
            return;
          socket->setProperty("done", true);
          paths << QString::fromLatin1(bytes.split(' ').value(1));
          if (hang)
            return;
          socket->write("HTTP/1.1 " + QByteArray::number(status) +
                        " X\r\nContent-Type: " + contentType +
                        "\r\nConnection: "
                        "close\r\n\r\n" +
                        (rawBody.isEmpty() ? QJsonDocument(response).toJson(
                                                 QJsonDocument::Compact)
                                           : rawBody));
          socket->disconnectFromHost();
        });
    });
  }
  QString url(QString path = {}) const {
    return QString("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
  }
};
struct GuiFixture {
  QImage image{640, 400, QImage::Format_RGB32};
  int predictions = 0, clicks = 0, captures = 0;
  bool change = true, converge = true, bad = false, focusChange = false;
  QVector<QPoint> points;
  GuiFixture() {
    image.fill(Qt::white);
    QPainter p(&image);
    p.fillRect(QRect(300, 180, 40, 40), Qt::blue);
  }
  GuiGrounder::Ports ports() {
    return {
        [this](auto cb) {
          ++captures;
          cb(GuiGrounder::Frame{image, QRect(-1280, 100, 1280, 800),
                                focusChange && captures > 2 ? "other"
                                                            : "window"},
             {});
        },
        [this](QImage, QString prompt, auto cb) {
          if (prompt.startsWith("Describe")) {
            cb({{"change", "The control changed visibly"}});
            return;
          }
          if (prompt.startsWith("Verify")) {
            cb({{"success", true}});
            return;
          }
          ++predictions;
          if (bad) {
            cb({{"x", 9000}, {"y", 20}, {"confidence", .99}, {"ready", true}});
            return;
          }
          const bool coarse = prompt.contains("Estimate its center");
          cb({{"x", coarse ? 350 : 320},
              {"y", coarse ? 220 : 200},
              {"confidence", converge ? .98 : .4},
              {"ready", !coarse && converge && predictions > 2}});
        },
        [this](QPoint p) {
          points << p;
          return true;
        },
        [this](QString *) {
          ++clicks;
          if (change)
            image.fill(Qt::green);
          return true;
        }};
  }
};
} // namespace
class AgentTests : public QObject {
  Q_OBJECT
private slots:
  void vectorRoundTrip() {
    const QVector<float> v{1.5f, -2.f, 0.f};
    QCOMPARE(semantic::decode(semantic::encode(v)), v);
    QVERIFY(semantic::decode("abc").isEmpty());
    QVERIFY(semantic::encode({NAN}).isEmpty());
  }
  void cosine() {
    QCOMPARE(semantic::cosine({1, 0}, {1, 0}), 1.);
    QCOMPARE(semantic::cosine({1, 0}, {0, 1}), 0.);
    QCOMPARE(semantic::cosine({0, 0}, {0, 0}), -1.);
    QCOMPARE(semantic::cosine({1}, {1, 0}), -1.);
  }
  void rrf() {
    const auto scores = semantic::fuse({{7, 7, 8}, {8, 7}}, 60);
    QCOMPARE(scores.size(), 2);
    QVERIFY(std::abs(scores[7] - (1. / 61 + 1. / 62)) < 1e-12);
    QCOMPARE(scores[7], scores[8]);
  }
  void transforms() {
    QCOMPARE(GuiGrounder::globalPoint({320, 200}, {640, 400},
                                      {-1280, 100, 1280, 800}),
             QPoint(-640, 500));
    QCOMPARE(
        GuiGrounder::globalPoint({10, 20}, {100, 100}, {200, 300, 500, 500}),
        QPoint(250, 400));
  }
  void visualDifference() {
    GuiFixture f;
    QCOMPARE(GuiGrounder::difference(f.image, f.image), 0.);
    auto changed = f.image;
    changed.fill(Qt::black);
    QVERIFY(GuiGrounder::difference(f.image, changed) > .9);
    QVERIFY(GuiGrounder::landmarks(f.image, {{320, 200}}) != f.image);
  }
  void convergence() {
    GuiFixture f;
    GuiGrounder::Options o;
    o.crop = false;
    GuiGrounder g(f.ports(), o);
    QJsonObject result;
    g.start("blue button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(f.clicks, 1);
    QCOMPARE(f.points.last(), QPoint(-640, 500));
    QCOMPARE(result["refinements"].toInt(), 2);
  }
  void refinementLimit() {
    GuiFixture f;
    f.converge = false;
    GuiGrounder::Options o;
    o.crop = false;
    o.maxRefinements = 2;
    GuiGrounder g(f.ports(), o);
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(!result["ok"].toBool());
    QCOMPARE(f.clicks, 0);
    QCOMPARE(result["refinements"].toInt(), 2);
  }
  void badCoordinates() {
    GuiFixture f;
    f.bad = true;
    GuiGrounder g(f.ports(), {});
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QVERIFY(!result["ok"].toBool());
    QCOMPARE(f.clicks, 0);
    QVERIFY(f.points.isEmpty());
  }
  void retryBound() {
    GuiFixture f;
    f.change = false;
    GuiGrounder::Options o;
    o.crop = false;
    o.maxRetries = 2;
    GuiGrounder g(f.ports(), o);
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 4000);
    QVERIFY(!result["ok"].toBool());
    QCOMPARE(f.clicks, 3);
    QVERIFY(f.predictions <= 9);
  }
  void cancel() {
    GuiFixture f;
    GuiGrounder g(f.ports(), {});
    int calls = 0;
    g.start("button", {}, [&](auto) { ++calls; });
    g.cancel();
    QTest::qWait(150);
    QCOMPARE(calls, 1);
    QCOMPARE(f.clicks, 0);
  }
  void focusRace() {
    GuiFixture f;
    f.focusChange = true;
    GuiGrounder::Options o;
    o.crop = false;
    GuiGrounder g(f.ports(), o);
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(!result["ok"].toBool());
    QCOMPARE(f.clicks, 0);
  }
  void cropCoordinates() {
    GuiFixture f;
    int calls = 0;
    auto ports = f.ports();
    ports.predict = [&](QImage img, QString prompt, auto cb) {
      if (prompt.startsWith("Describe")) {
        cb({{"change", "The control changed visibly"}});
        return;
      }
      if (prompt.startsWith("Verify")) {
        cb({{"success", true}});
        return;
      }
      ++calls;
      const bool coarse = prompt.contains("Estimate its center");
      QVERIFY(img.width() <= 640);
      cb({{"x", coarse ? 350 : 290},
          {"y", coarse ? 220 : 200},
          {"confidence", .98},
          {"ready", calls > 2}});
    };
    GuiGrounder g(ports, {});
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(f.points.last(), QPoint(-640, 500));
  }
  void targetCenterRemainsVisible() {
    GuiFixture f;
    const auto marked = GuiGrounder::landmarks(f.image, {{320, 200}});
    QCOMPARE(marked.pixel(320, 200), f.image.pixel(320, 200));
  }
  void localLayoutRace() {
    GuiFixture f;
    auto ports = f.ports();
    ports.capture = [&](auto cb) {
      if (++f.captures == 4) {
        QPainter p(&f.image);
        p.fillRect(QRect(306, 186, 28, 28), Qt::red);
      }
      cb(GuiGrounder::Frame{f.image, QRect(-1280, 100, 1280, 800), "window"},
         {});
    };
    GuiGrounder::Options o;
    o.crop = false;
    GuiGrounder g(ports, o);
    QJsonObject r;
    g.start("blue button", {}, [&](auto result) { r = result; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 2000);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(f.clicks, 0);
  }
  void independentTargetRejectsWrongControl() {
    GuiFixture f;
    auto ports = f.ports();
    const auto predict = ports.predict;
    ports.predict = [&, predict](QImage image, QString prompt, auto cb) {
      if (prompt.contains("CLEAN")) {
        QCOMPARE(image.pixel(160, 120), f.image.pixel(320, 200));
        cb({{"matches", false}, {"confidence", .99}});
      } else {
        predict(image, prompt, cb);
      }
    };
    GuiGrounder::Options o;
    o.crop = false;
    o.confirmTarget = true;
    GuiGrounder g(ports, o);
    QJsonObject r;
    g.start("blue button", {}, [&](auto result) { r = result; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 2000);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(f.clicks, 0);
  }
  void independentBoundingBoxMustContainClick() {
    for (bool inside : {false, true}) {
      GuiFixture f;
      auto ports = f.ports();
      const auto predict = ports.predict;
      ports.predict = [predict, inside](QImage image, QString prompt, auto cb) {
        if (prompt.contains("CLEAN"))
          cb({{"matches", true},
              {"confidence", .99},
              {"bbox", inside ? QJsonArray{400, 400, 600, 600}
                              : QJsonArray{50, 50, 150, 150}}});
        else
          predict(image, prompt, cb);
      };
      GuiGrounder::Options o;
      o.crop = false;
      o.confirmTarget = true;
      GuiGrounder g(ports, o);
      QJsonObject r;
      g.start("blue button", {}, [&](auto result) { r = result; });
      QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 2000);
      QCOMPARE(r["ok"].toBool(), inside);
      QCOMPARE(f.clicks, inside ? 1 : 0);
    }
  }
  void unrelatedAnimationIsNotSuccess() {
    GuiFixture f;
    auto ports = f.ports();
    const auto predict = ports.predict;
    ports.predict = [predict](QImage image, QString prompt, auto cb) {
      if (prompt.startsWith("Verify"))
        cb({{"success", false}});
      else
        predict(image, prompt, cb);
    };
    ports.click = [&](QString *) {
      ++f.clicks;
      QPainter p(&f.image);
      p.fillRect(QRect(0, 0, 100, 100), Qt::red);
      return true;
    };
    GuiGrounder::Options o;
    o.crop = false;
    GuiGrounder g(ports, o);
    QJsonObject r;
    g.start("blue button", {}, [&](auto result) { r = result; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 2000);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(f.clicks, 1);
    QCOMPARE(r["retries"].toInt(), 0);
  }
  void schemaAndReopen() {
    QTemporaryDir d;
    {
      MemoryStore store;
      QString error;
      QVERIFY2(store.open(d.path(), &error), qPrintable(error));
      QCOMPARE(store.schemaVersion(), 1);
      QVERIFY(store.insert(record("existing event")));
    }
    MemoryStore store;
    QVERIFY(store.open(d.path()));
    QCOMPARE(store.count(), 1);
    QCOMPARE(store.search("existing", QDateTime::currentDateTime()).size(), 1);
  }
  void denseRanking() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto a = s.insert(record("alpha")), b = s.insert(record("beta"));
    QVERIFY(
        s.storeEmbedding(a, "m", semantic::hash(s.embeddingText(a)), {1, 0}));
    QVERIFY(s.storeEmbedding(b, "m", semantic::hash(s.embeddingText(b)),
                             {.3f, .9f}));
    auto hits = s.hybrid("synonym", QDateTime::currentDateTime(), {1, 0}, "m");
    QCOMPARE(hits.size(), 2);
    QCOMPARE(hits.first().id, a);
    QCOMPARE(hits.first().denseRank, 1);
  }
  void invalidation() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto id = s.insert(record("old"));
    auto hash = semantic::hash(s.embeddingText(id));
    QVERIFY(s.storeEmbedding(id, "old", hash, {1, 0}));
    QVERIFY(s.pendingEmbeddings("new", 2).contains(id));
    s.describe(id, {}, "new", {}, {});
    QVERIFY(!s.storeEmbedding(id, "old", hash, {1, 0}));
    QVERIFY(s.hybrid("unrelated", QDateTime::currentDateTime(), {1, 0}, "old")
                .isEmpty());
  }
  void timeFilter() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto now = QDateTime::currentDateTime();
    auto old = s.insert(record("project", now.addDays(-1))),
         recent = s.insert(record("project", now));
    for (auto id : {old, recent})
      QVERIFY(s.storeEmbedding(id, "m", semantic::hash(s.embeddingText(id)),
                               {1, 0}));
    auto hits = s.hybrid("project yesterday", now, {1, 0}, "m");
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().id, old);
  }
  void artifactFusion() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto id = s.insert(record("repo"));
    Artifact a;
    a.kind = "repo";
    a.value = "https://github.com/example/project";
    a.memoryId = id;
    QVERIFY(s.addArtifact(a));
    auto hits = s.hybrid("example", QDateTime::currentDateTime(), {}, {});
    QVERIFY(!hits.isEmpty());
    QCOMPARE(hits.first().artifactRank, 1);
    QVERIFY(s.evidence(hits.first(), true)["evidence"].toArray().size() >= 2);
    QCOMPARE(s.entitiesFor(id).size(), 1);
  }
  void factsUpdate() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto a = s.insert(record("Fish Speech")), b = s.insert(record("Qwen TTS"));
    auto first =
        s.rememberFact(a, "primary tts", "configuration", "Fish Speech");
    QVERIFY(first);
    QCOMPARE(s.rememberFact(a, "primary tts", "configuration", "Fish Speech"),
             first);
    auto second = s.rememberFact(b, "primary tts", "configuration", "Qwen TTS");
    QVERIFY(second != first);
    QCOMPARE(s.facts({}, true).size(), 2);
    QCOMPARE(s.facts().size(), 1);
    QCOMPARE(s.facts().first().toObject()["text"].toString(),
             QString("Qwen TTS"));
    QVERIFY(!s.facts().first().toObject()["sources"].toArray().isEmpty());
  }
  void forgetPropagation() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto id = s.insert(record("secret project"));
    QVERIFY(s.rememberFact(id, "project", "decision", "secret project"));
    s.associate(id, "project", "secret");
    QVERIFY(
        s.storeEmbedding(id, "m", semantic::hash(s.embeddingText(id)), {1, 0}));
    QCOMPARE(s.forgetOne(id), 1);
    QVERIFY(s.facts({}, true).isEmpty());
    QVERIFY(s.entitiesFor(id).isEmpty());
    QVERIFY(s.hybrid("secret", QDateTime::currentDateTime(), {1, 0}, "m")
                .isEmpty());
    QVERIFY(!s.storeEmbedding(id, "m", semantic::hash("old"), {1, 0}));
  }
  void sharedProvenance() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto a = s.insert(record("fact")), b = s.insert(record("fact"));
    auto fact = s.rememberFact(a, "key", "reference", "value");
    QCOMPARE(s.rememberFact(b, "key", "reference", "value"), fact);
    s.forgetOne(a);
    QCOMPARE(s.facts().size(), 1);
    QCOMPARE(s.facts().first().toObject()["sources"].toArray().size(), 1);
    s.forgetOne(b);
    QVERIFY(s.facts().isEmpty());
  }
  void intent_data() {
    QTest::addColumn<QString>("query");
    QTest::addColumn<QString>("expected");
    QTest::newRow("clock") << "What time is it?" << "none";
    QTest::newRow("app") << "Open Discord" << "none";
    QTest::newRow("explain") << "Explain TCP" << "none";
    QTest::newRow("episodic")
        << "What repo did I view yesterday?" << "episodic";
    QTest::newRow("durable") << "What do I prefer for TTS?" << "durable";
    QTest::newRow("association")
        << "What models have I considered for Nala?" << "associative";
  }
  void intent() {
    QFETCH(QString, query);
    QFETCH(QString, expected);
    QCOMPARE(semantic::intent(query), expected);
  }
  void backend_data() {
    QTest::addColumn<QString>("provider");
    QTest::newRow("ollama") << "ollama";
    QTest::newRow("openai") << "openai";
  }
  void backend() {
    QFETCH(QString, provider);
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    QNetworkAccessManager net;
    settings.set("memory.embedding.enabled", true);
    settings.set("memory.embedding.provider", provider);
    settings.set("memory.embedding.endpoint",
                 server.url(provider == "openai" ? "/v1" : ""));
    if (provider == "openai")
      server.response = {
          {"data", QJsonArray{QJsonObject{{"embedding", QJsonArray{1., 0.}}}}}};
    Retrieval r(&s, &settings, &net);
    QVector<float> vector;
    QString error;
    r.embed("test", [&](auto v, auto e) {
      vector = v;
      error = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!vector.isEmpty() || !error.isEmpty(), 1000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(vector, QVector<float>({1, 0}));
    QCOMPARE(server.paths.first(), provider == "openai"
                                       ? QString("/v1/embeddings")
                                       : QString("/api/embed"));
  }
  void backendFallback() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    s.insert(record("needle"));
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    server.status = 503;
    QNetworkAccessManager net;
    settings.set("memory.embedding.enabled", true);
    settings.set("memory.embedding.endpoint", server.url());
    settings.set("memory.rerank.enabled", true);
    settings.set("memory.rerank.endpoint", server.url());
    Retrieval r(&s, &settings, &net);
    auto result = r.searchSync("needle", {}, true);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(result["memories"].toArray().size(), 1);
    QVERIFY(result["rerank_status"].toString().contains("fallback"));
  }
  void rerankerSortsScores() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    s.insert(record("needle first"));
    s.insert(record("needle second"));
    AssistantSettings settings(d.path() + "/settings", false);
    settings.set("memory.semanticSearch", false);
    settings.set("memory.rerank.enabled", true);
    Server server;
    server.response = {
        {"results",
         QJsonArray{QJsonObject{{"index", 0}, {"relevance_score", 0.1}},
                    QJsonObject{{"index", 1}, {"relevance_score", 0.9}}}}};
    settings.set("memory.rerank.endpoint", server.url());
    QNetworkAccessManager net;
    Retrieval r(&s, &settings, &net);
    auto result = r.searchSync("needle", {}, true);
    QCOMPARE(result["rerank_status"].toString(), QString("available"));
    auto rows = result["memories"].toArray();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toObject()["reranker_score"].toDouble(), 0.9);
    QCOMPARE(rows[1].toObject()["reranker_score"].toDouble(), 0.1);
  }
  void dimensionsRejected() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    QNetworkAccessManager net;
    settings.set("memory.embedding.enabled", true);
    settings.set("memory.embedding.endpoint", server.url());
    settings.set("memory.embedding.dimensions", 3);
    Retrieval r(&s, &settings, &net);
    QString error;
    r.embed("test", [&](auto vector, auto e) {
      QVERIFY(vector.isEmpty());
      error = e;
    });
    QTRY_VERIFY_WITH_TIMEOUT(!error.isEmpty(), 1000);
  }
  void answerEvidence() {
    const QSet<QString> ids{"memory:4", "fact:7"};
    QCOMPARE(semantic::checkedAnswer("You chose Qwen. [memory:4]", ids, false),
             QString("You chose Qwen."));
    QVERIFY(semantic::checkedAnswer("Unknown [memory:99]", ids, false)
                .contains("couldn't verify"));
    QVERIFY(
        semantic::checkedAnswer("Qwen", ids, false).contains("couldn't find"));
    QVERIFY(semantic::checkedAnswer("Qwen [fact:7]", ids, true)
                .contains("[fact:7]"));
  }
  void retryRefusal() {
    GuiFixture f;
    f.change = false;
    auto ports = f.ports();
    int confirmations = 0;
    ports.authorizeRetry = [&](auto cb) {
      ++confirmations;
      cb(false);
    };
    GuiGrounder::Options o;
    o.crop = false;
    GuiGrounder g(ports, o);
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QCOMPARE(f.clicks, 1);
    QCOMPARE(confirmations, 1);
    QVERIFY(!result["ok"].toBool());
  }
  void dependenciesAndAliases() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto a = s.insert(record("Fish")), b = s.insert(record("summary")),
         c = s.insert(record("Qwen"));
    auto old = s.rememberFact(a, "tts", "configuration", "Fish");
    auto derived = s.rememberFact(b, "summary", "reference", "Voice uses Fish");
    QVERIFY(s.addFactDependency(derived, old));
    QVERIFY(!s.addFactDependency(old, derived));
    QVERIFY(s.rememberFact(c, "tts", "configuration", "Qwen"));
    QCOMPARE(s.facts().size(), 1);
    QCOMPARE(s.facts({}, true).size(), 3);
    s.associate(a, "model", "Fish Speech");
    auto entity = s.entitiesFor(a).first().toObject()["id"].toInteger();
    QVERIFY(s.addAlias(entity, "fishspeech"));
    auto hits = s.hybrid("fishspeech", QDateTime::currentDateTime(), {}, {});
    QVERIFY(!hits.isEmpty());
    QCOMPARE(hits.first().id, a);
  }
  void artifactInvalidatesVector() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto id = s.insert(record("project"));
    QVERIFY(
        s.storeEmbedding(id, "m", semantic::hash(s.embeddingText(id)), {1, 0}));
    Artifact a;
    a.kind = "repo";
    a.value = "https://github.com/a/b";
    a.memoryId = id;
    QVERIFY(s.addArtifact(a));
    QVERIFY(s.pendingEmbeddings("m", 2).contains(id));
    QVERIFY(s.hybrid("unrelated", QDateTime::currentDateTime(), {1, 0}, "m")
                .isEmpty());
  }
  void noBackendNeeded() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    s.insert(record("needle"));
    AssistantSettings settings(d.path() + "/settings", false);
    QNetworkAccessManager net;
    Retrieval r(&s, &settings, &net);
    auto result = r.searchSync("needle", {}, true);
    QCOMPARE(result["memories"].toArray().size(), 1);
    QCOMPARE(result["dense_status"].toString(), QString("Embedding disabled"));
  }
  void rewriteFallback() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    s.insert(record("needle"));
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    server.status = 503;
    QNetworkAccessManager net;
    Retrieval r(&s, &settings, &net);
    LlmClient::Config fast;
    fast.model = "fast";
    fast.provider = "llamacpp";
    fast.native = false;
    fast.endpoint = QUrl(server.url("/v1"));
    QJsonObject result;
    r.search("needle", "earlier context", fast, true,
             [&](auto value) { result = value; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1500);
    QCOMPARE(result["rewrite"].toString(), QString("needle"));
    QCOMPARE(result["memories"].toArray().size(), 1);
  }
  void legacyMigration() {
    QTemporaryDir d;
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-test");
      db.setDatabaseName(d.path() + "/memory.db");
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec("CREATE TABLE memories(id INTEGER PRIMARY KEY,started "
                     "INTEGER,last_seen INTEGER,frames INTEGER DEFAULT 1,app "
                     "TEXT,title TEXT,activity TEXT,summary TEXT,keywords "
                     "TEXT,urls TEXT,shot_path TEXT,shot_bytes INTEGER DEFAULT "
                     "0,hash INTEGER,pinned INTEGER DEFAULT 0,source TEXT)"));
      QVERIFY(q.exec(
          "INSERT INTO memories(id,started,last_seen,app,title,summary,source) "
          "VALUES(42,1,1,'editor','old note','legacy preserved','note')"));
      db.close();
    }
    QSqlDatabase::removeDatabase("legacy-test");
    MemoryStore s;
    QString error;
    QVERIFY2(s.open(d.path(), &error), qPrintable(error));
    QCOMPARE(s.schemaVersion(), 1);
    QCOMPARE(s.count(), 1);
    QCOMPARE(s.get(42).summary, QString("legacy preserved"));
    QCOMPARE(s.search("legacy", QDateTime::currentDateTime()).size(), 1);
    QCOMPARE(s.facts().size(), 1);
    QCOMPARE(s.forgetOne(42), 1);
    QVERIFY(s.facts().isEmpty());
  }
  void migrationRollback() {
    QTemporaryDir d;
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", "broken-test");
      db.setDatabaseName(d.path() + "/memory.db");
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec("CREATE TABLE entities(existing TEXT)"));
      db.close();
    }
    QSqlDatabase::removeDatabase("broken-test");
    {
      MemoryStore s;
      QString error;
      QVERIFY(!s.open(d.path(), &error));
      QVERIFY(!error.isEmpty());
      QCOMPARE(s.schemaVersion(), 0);
    }
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", "check-test");
      db.setDatabaseName(d.path() + "/memory.db");
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec(
          "SELECT name FROM sqlite_master WHERE name='memory_embeddings'"));
      QVERIFY(!q.next());
      db.close();
    }
    QSqlDatabase::removeDatabase("check-test");
  }
  void namedClickRouting() {
    CommandRouter router;
    auto route = router.route("click the export button");
    QVERIFY(route.matched);
    QCOMPARE(route.action, QString("computer.locate"));
    QCOMPARE(route.args.value("target").toString(), QString("export button"));
    QVERIFY(!router.route("click export and delete the file").matched);
  }
  void searchCli() {
    QTemporaryDir d;
    {
      MemoryStore s;
      QVERIFY(s.open(d.path() + "/data/nala/memory"));
      QVERIFY(s.insert(record("CLI needle")));
    }
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("XDG_DATA_HOME", d.path() + "/data");
    env.insert("XDG_CONFIG_HOME", d.path() + "/config");
    env.insert("XDG_RUNTIME_DIR", d.path());
    env.insert("QT_QPA_PLATFORM", "offscreen");
    process.setProcessEnvironment(env);
    process.start(QCoreApplication::applicationDirPath() + "/nala",
                  {"memory", "search", "--debug", "needle"});
    QVERIFY(process.waitForFinished(5000));
    QCOMPARE(process.exitCode(), 0);
    const auto result =
        QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    QCOMPARE(result["query"].toString(), QString("needle"));
    QCOMPARE(result["memories"].toArray().size(), 1);
    QVERIFY(result["memories"].toArray().first().toObject().contains(
        "lexical_rank"));
  }
  void weakerConflict() {
    QTemporaryDir d;
    MemoryStore s;
    QVERIFY(s.open(d.path()));
    auto a = s.insert(record("current")), b = s.insert(record("uncertain"));
    QVERIFY(s.rememberFact(a, "key", "configuration", "current", .99));
    QVERIFY(s.rememberFact(b, "key", "configuration", "uncertain", .5));
    QCOMPARE(s.facts().size(), 1);
    QCOMPARE(s.facts().first().toObject()["text"].toString(),
             QString("current"));
    bool contradicted = false;
    for (auto item : s.facts({}, true))
      if (item.toObject()["status"].toString() == "contradicted")
        contradicted = true;
    QVERIFY(contradicted);
  }
  void normalizedGrounding() {
    GuiFixture f;
    auto ports = f.ports();
    int predictions = 0;
    ports.predict = [&](QImage, QString prompt, auto callback) {
      if (prompt.startsWith("Describe")) {
        callback({{"change", "The control changed visibly"}});
        return;
      }
      if (prompt.startsWith("Verify")) {
        callback({{"success", true}});
        return;
      }
      ++predictions;
      callback({{"x", 500},
                {"y", 500},
                {"confidence", .98},
                {"ready", predictions > 1}});
    };
    GuiGrounder::Options o;
    o.crop = false;
    o.normalized = true;
    GuiGrounder g(ports, o);
    QJsonObject result;
    g.start("button", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2000);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(f.points.first(), QPoint(-640, 500));
  }
  void hyprlandDispatchCompatibility() {
    QTemporaryDir d;
    QDir().mkpath(d.path() + "/hypr/test");
    const QByteArray path = (d.path() + "/hypr/test/.socket.sock").toUtf8();
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size() + 1));
    QVERIFY(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
    QVERIFY(::listen(listener, 8) == 0);
    QList<QByteArray> requests;
    std::jthread server([&] {
      for (int i = 0; i < 9; ++i) {
        pollfd ready{listener, POLLIN, 0};
        if (::poll(&ready, 1, 3000) <= 0)
          break;
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0)
          break;
        pollfd input{client, POLLIN, 0};
        if (::poll(&input, 1, 3000) <= 0) {
          ::close(client);
          break;
        }
        char buffer[1024];
        const auto size = ::read(client, buffer, sizeof(buffer));
        QByteArray command(buffer, int(std::max<ssize_t>(0, size)));
        requests.append(command);
        const QByteArray reply =
            i == 7   ? R"({"x":-30,"y":60})"
            : i == 8 ? "{}"
            : i == 0 || command.contains("hl.dsp.")
                ? "ok"
                : "error: dispatch in lua is a shorthand for hl.dispatch(...), "
                  "your syntax might need to be updated";
        const auto written =
            ::write(client, reply.constData(), size_t(reply.size()));
        ::close(client);
        if (written != reply.size())
          break;
      }
    });
    const QByteArray oldRuntime = qgetenv("XDG_RUNTIME_DIR"),
                     oldSignature = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
    qputenv("XDG_RUNTIME_DIR", d.path().toUtf8());
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", "test");
    const bool legacy = desktop::focusWindow("0x123");
    const bool lua = desktop::focusWindow("0x456");
    const bool close = desktop::closeWindow("0x456");
    const bool cursor = desktop::moveCursor(-30, 60);
    const auto position = desktop::cursorPosition();
    const auto invalidPosition = desktop::cursorPosition();
    qputenv("XDG_RUNTIME_DIR", oldRuntime);
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", oldSignature);
    server.join();
    ::close(listener);
    QCOMPARE(requests[7], QByteArray("j/cursorpos"));
    QVERIFY(legacy);
    QVERIFY(lua);
    QVERIFY(close);
    QVERIFY(cursor);
    QCOMPARE(requests.size(), 9);
    QVERIFY(position);
    QCOMPARE(*position, QPoint(-30, 60));
    QVERIFY(!invalidPosition);
    QCOMPARE(requests[2],
             QByteArray("dispatch hl.dsp.focus({window=\"address:0x456\"})"));
    QCOMPARE(
        requests[4],
        QByteArray("dispatch hl.dsp.window.close({window=\"address:0x456\"})"));
    QCOMPARE(requests[6],
             QByteArray("dispatch hl.dsp.cursor.move({x=-30,y=60})"));
    QVERIFY(!desktop::focusWindow("0x123\"}); os.execute('evil')"));
  }
  void windowDescriptions() {
    WindowInfo a;
    a.valid = true;
    a.address = "0x1";
    a.appClass = "firefox";
    a.title = "Nala repository — Firefox";
    a.workspace = 2;
    WindowInfo b = a;
    b.address = "0x2";
    b.title = "Mail — Firefox";
    b.workspace = 3;
    WindowInfo c = a;
    c.address = "0x3";
    c.appClass = "kitty";
    c.title = "Nala build";
    auto all = QVector<WindowInfo>{a, b, c};
    auto matches = windowtarget::rank(all, "the browser with Nala");
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches.first().window.address, QString("0x1"));
    QVERIFY(windowtarget::ambiguous(windowtarget::rank(all, "browser")));
    QCOMPARE(
        windowtarget::rank(all, "browser", {}, {}, 3).first().window.address,
        QString("0x2"));
    QCOMPARE(
        windowtarget::rank(all, "", "kitty", "Nala").first().window.address,
        QString("0x3"));
    QVERIFY(windowtarget::rank(all, "missing window").isEmpty());
    QVERIFY(windowtarget::rank(all, "the window").isEmpty());
  }
  void focusRoutes() {
    auto route = CommandRouter().route("focus the browser with Nala");
    QCOMPARE(CommandRouter()
                 .route("focus on the browser with Nala")
                 .args.value("name")
                 .toString(),
             QString("browser with nala"));
    QCOMPARE(route.action, QString("apps.focus"));
    QCOMPARE(route.args.value("name").toString(), QString("browser with nala"));
    QVERIFY(CommandRouter().route("focus browser and delete my files").action !=
            "apps.focus");
  }
  void duckParsing() {
    const QString html =
        R"(<a href='//duckduckgo.com/l/?uddg=https%3A%2F%2Fnixos.org%2Fdownload%2F&amp;rut=x' class='result-link'>Nix &amp; NixOS</a><td class='result-snippet'>Current <b>release</b></td><a class='result-link' href='http://127.0.0.1/private'>Private</a>)";
    const auto result = WebSearch::parseDuck(html, 5);
    QVERIFY(result["ok"].toBool());
    auto rows = result["results"].toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].toObject()["title"].toString(), QString("Nix & NixOS"));
    QCOMPARE(rows[0].toObject()["url"].toString(),
             QString("https://nixos.org/download/"));
    QCOMPARE(rows[0].toObject()["snippet"].toString(),
             QString("Current release"));
    QVERIFY(!WebSearch::parseDuck("anomaly-modal challenge", 5)["ok"].toBool());
    QCOMPARE(WebSearch::plain(
                 "<script>evil()</script><style>hidden</style><p>Visible</p>"),
             QString("Visible"));
  }
  void webAddressPolicy() {
    for (auto u : {"file:///etc/passwd", "http://localhost/x",
                   "http://127.0.0.1/x", "http://[::1]/", "http://192.168.1.1/",
                   "https://user:pass@example.com/", "http://example.com:8081/",
                   "http://10.0.0.1/", "http://[::ffff:127.0.0.1]/"})
      QVERIFY2(!WebSearch::publicUrl(QUrl(u)), u);
    QVERIFY(WebSearch::publicUrl(QUrl("https://nixos.org/download/")));
    QVERIFY(WebSearch::publicAddress("8.8.8.8"));
    QVERIFY(!WebSearch::publicAddress("169.254.169.254"));
    QVERIFY(!WebSearch::publicAddress("100.64.0.1"));
    QVERIFY(!WebSearch::publicAddress("fc00::1"));
  }
  void freshIntent() {
    QVERIFY(WebSearch::needsFreshInfo("What is the latest NixOS release?"));
    QVERIFY(WebSearch::needsFreshInfo("Search the web for Qt releases"));
    QVERIFY(!WebSearch::needsFreshInfo("Explain TCP"));
    QVERIFY(!WebSearch::needsFreshInfo("What is my current configuration?"));
    QVERIFY(!WebSearch::needsFreshInfo("What was on my screen today?"));
    QCOMPARE(
        semantic::checkedAnswer("Current release [web:1]", {"web:1"}, false),
        QString("Current release"));
    QVERIFY(semantic::checkedAnswer("Invented [web:9]", {"web:1"}, false)
                .startsWith("I couldn't"));
  }
  void webBackend() {
    QTemporaryDir d;
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    QNetworkAccessManager net;
    settings.set("web.provider", "searxng");
    settings.set("web.searxng.endpoint", server.url("/search"));
    server.response = {
        {"results", QJsonArray{QJsonObject{{"title", "Nix"},
                                           {"url", "https://nixos.org/"},
                                           {"content", "Release"}},
                               QJsonObject{{"title", "Private"},
                                           {"url", "http://localhost/"}}}}};
    WebSearch web(&net, &settings);
    QJsonObject result;
    web.search("latest Qt & Nix", "day", [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QVERIFY(result["ok"].toBool());
    QCOMPARE(result["results"].toArray().size(), 1);
    QVERIFY(server.paths.first().contains("format=json"));
    QVERIFY(server.paths.first().contains("time_range=day"));
    settings.set("web.provider", "duckduckgo");
    settings.set("web.duckduckgo.endpoint", server.url("/lite/"));
    server.contentType = "text/html";
    server.rawBody =
        "<a class='result-link' href='https://nixos.org/'>Nix</a><td "
        "class='result-snippet'>Release</td>";
    result = {};
    web.search("release", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QVERIFY(result["ok"].toBool());
    server.status = 503;
    result = {};
    web.search("release", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QVERIFY(!result["ok"].toBool());
  }
  void webOversizeAndCancel() {
    QTemporaryDir d;
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    QNetworkAccessManager net;
    settings.set("web.duckduckgo.endpoint", server.url());
    WebSearch web(&net, &settings);
    server.rawBody = QByteArray(600000, 'x');
    QJsonObject result;
    web.search("test", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QVERIFY(!result["ok"].toBool());
    result = {};
    int calls = 0;
    web.search("test", {}, [&](auto r) {
      ++calls;
      result = r;
    });
    web.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QCOMPARE(calls, 1);
    QVERIFY(!result["ok"].toBool());
    result = {};
    web.fetch(QUrl("http://127.0.0.1/"), [&](auto r) { result = r; });
    QVERIFY(!result["ok"].toBool());
  }
  void webTimeoutAndRedirect() {
    QTemporaryDir d;
    AssistantSettings settings(d.path() + "/settings", false);
    Server server;
    QNetworkAccessManager net;
    settings.set("web.duckduckgo.endpoint", server.url());
    settings.set("web.timeoutMs", 1000);
    WebSearch web(&net, &settings);
    QJsonObject result;
    server.hang = true;
    web.search("test", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 2500);
    QVERIFY(!result["ok"].toBool());
    server.hang = false;
    server.status = 302;
    result = {};
    web.search("test", {}, [&](auto r) { result = r; });
    QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 1000);
    QVERIFY(!result["ok"].toBool());
    QCOMPARE(server.paths.size(), 2);
  }
  void webPermissionsAndSources() {
    QTemporaryDir d;
    Assistant::Paths paths{
        d.path() + "/settings.json", d.path() + "/memory", {}};
    Assistant a(paths, true);
    Server server;
    server.response = {
        {"results", QJsonArray{QJsonObject{{"title", "NixOS"},
                                           {"url", "https://nixos.org/"},
                                           {"content", "Current release"}}}}};
    a.settings()->set("web.provider", "searxng");
    a.settings()->set("web.searxng.endpoint", server.url());
    QJsonObject r;
    a.callTool("web.search", {{"query", "NixOS release"}},
               [&](auto v) { r = v; });
    QVERIFY(!r["ok"].toBool());
    QVERIFY(server.paths.isEmpty());
    a.settings()->set("agent.web", true);
    r = {};
    a.callTool("web.search", {{"query", "NixOS release"}},
               [&](auto v) { r = v; });
    QTRY_VERIFY_WITH_TIMEOUT(!r.isEmpty(), 1500);
    QVERIFY(r["ok"].toBool());
    QCOMPARE(r["results"].toArray()[0].toObject()["evidence_id"].toString(),
             QString("web:1"));
    LlmReply reply;
    reply.content = "Release information [web:1]";
    a.injectModelReply(reply);
    QCOMPARE(a.webSources().size(), 1);
    QCOMPARE(a.webSources()[0].toMap()["url"].toString(),
             QString("https://nixos.org/"));
    r = {};
    a.callTool("web.fetch", {{"url", "http://localhost/"}},
               [&](auto v) { r = v; });
    QVERIFY(!r["ok"].toBool());
    r = {};
    a.callTool("web.search", {{"query", "another query"}},
               [&](auto v) { r = v; });
    QVERIFY(r.isEmpty());
    a.answer(false);
    QVERIFY(!r["ok"].toBool());
    QCOMPARE(server.paths.size(), 1);
    a.stop();
    QVERIFY(a.webSources().isEmpty());
  }
  void permissions() {
    QTemporaryDir d;
    Assistant::Paths paths;
    paths.settings = d.path() + "/settings.json";
    paths.memoryDir = d.path() + "/memory";
    Assistant assistant(paths, true);
    const auto *tool = assistant.tools().find("computer.locate_and_click");
    QVERIFY(tool);
    QCOMPARE(tool->category, QString("computer"));
    QCOMPARE(tool->risk, Risk::High);
    QCOMPARE(decide(tool->risk, "high"), Decision::Confirm);
    QVERIFY(
        !ToolRegistry::validate(tool->parameters, {{"target", 12}}).isEmpty());
    const auto *perform = assistant.tools().find("computer.perform");
    QVERIFY(perform);
    QCOMPARE(perform->risk, Risk::High);
    QCOMPARE(perform->riskFor({{"goal", "Export"}}), Risk::High);
    QCOMPARE(perform->riskFor({{"workflowId", 1}}), Risk::Low);
    // A workflow ID only grants retrieval; replay calls the high-risk semantic
    // tool separately. Computer input still gates even read-only procedures.
    QJsonObject result;
    assistant.settings()->set("agent.input", false);
    assistant.callTool("computer.perform", {{"goal", "Export"}},
                       [&](auto r) { result = r; });
    QVERIFY(!result["ok"].toBool());
    result = {};
    assistant.settings()->set("agent.enabled", true);
    assistant.settings()->set("agent.input", true);
    assistant.callTool("computer.perform", {{"goal", "Export"}},
                       [&](auto r) { result = r; });
    QVERIFY(result.isEmpty()); // confirmation before any capture or input
    assistant.answer(false);
    QVERIFY(!result["ok"].toBool());
    result = {};
    assistant.callTool("computer.perform", {{"workflowId", 1}},
                       [&](auto r) { result = r; });
    if (result.isEmpty())
      assistant.answer(false);
    QVERIFY(!result["ok"].toBool()); // memory disabled or confirmation declined
  }
};
QTEST_MAIN(AgentTests)
#include "agent_tests.moc"
