#include "guigrounder.h"
#include "memory.h"
#include "retrieval.h"
#include "settings.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

static int visionBenchmark(const QString &model) {
  QNetworkAccessManager network;
  QJsonArray fixtures;
  for (int fixture = 0; fixture < 6; ++fixture) {
    QImage image(800, 500, QImage::Format_RGB32);
    image.fill(QColor("#f4f5f7"));
    const bool dense = fixture >= 3;
    const QRect box(460 - (fixture % 3) * 120, 350 - (fixture % 3) * 75,
                    dense ? 64 : 120, dense ? 24 : 44);
    {
      QPainter painter(&image);
      QFont font = painter.font();
      font.setPixelSize(dense ? 12 : 20);
      painter.setFont(font);
      painter.setPen(Qt::black);
      painter.drawText(QRect(30, 20, 500, 45), "Export settings");
      painter.drawText(QRect(30, 100, 550, 45),
                       "Choose the output format for your document.");
      if (dense) {
        for (int row = 0; row < 4; ++row) {
          const QRect other(40 + row * 100, 120 + row * 35, 70, 24);
          painter.setBrush(Qt::white);
          painter.drawRect(other);
          painter.drawText(other, Qt::AlignCenter,
                           row % 2 ? "Import" : "Preview");
        }
      }
      painter.setBrush(QColor("#1769ce"));
      painter.drawRoundedRect(box, 6, 6);
      painter.setPen(Qt::white);
      painter.drawText(box, Qt::AlignCenter, "Export");
      painter.setPen(Qt::black);
      painter.setBrush(Qt::white);
      painter.drawRoundedRect(QRect(610, 410, 120, 44), 6, 6);
      painter.drawText(QRect(610, 410, 120, 44), Qt::AlignCenter, "Cancel");
    }
    LlmClient vision(&network);
    LlmClient::Config config;
    config.endpoint = QUrl("http://127.0.0.1:11434/v1");
    config.model = model;
    config.provider = "ollama";
    config.numCtx = 8192;
    config.temperature = 0;
    config.maxTokens = 180;
    config.timeoutSec = 20;
    config.thinking = "off";
    vision.configure(config);
    QPoint first, last;
    bool pointed = false;
    int requests = 0, clicks = 0;
    QJsonArray modelOutputs;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb(GuiGrounder::Frame{image, image.rect(), "synthetic"}, {});
    };
    ports.predict = [&](QImage observation, QString prompt, auto callback) {
      ++requests;
      QByteArray bytes;
      QBuffer buffer(&bytes);
      buffer.open(QIODevice::WriteOnly);
      observation.save(&buffer, "PNG");
      auto links = std::make_shared<QVector<QMetaObject::Connection>>();
      const auto done = [links, callback](QJsonObject result) {
        for (auto link : *links)
          QObject::disconnect(link);
        callback(result);
      };
      *links << QObject::connect(
          &vision, &LlmClient::replied, &vision,
          [done, &modelOutputs](const LlmReply &r) {
            auto text = r.content.trimmed();
            const int start = text.indexOf('{'), end = text.lastIndexOf('}');
            if (start >= 0 && end > start)
              text = text.mid(start, end - start + 1);
            auto parsed = QJsonDocument::fromJson(text.toUtf8()).object();
            modelOutputs.append(parsed);
            done(parsed);
          });
      *links << QObject::connect(
          &vision, &LlmClient::failed, &vision,
          [done](QString error) { done({{"error", error}}); });
      vision.chat(QJsonArray{QJsonObject{
          {"role", "user"},
          {"content",
           QJsonArray{
               QJsonObject{{"type", "text"}, {"text", prompt}},
               QJsonObject{
                   {"type", "image_url"},
                   {"image_url",
                    QJsonObject{{"url", "data:image/png;base64," +
                                            QString::fromLatin1(
                                                bytes.toBase64())}}}}}}}});
    };
    ports.point = [&](QPoint point) {
      if (!pointed) {
        first = point;
        pointed = true;
      }
      last = point;
      return true;
    };
    ports.click = [&](QString *) {
      ++clicks;
      if (box.contains(last)) {
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        QFont font = painter.font();
        font.setPixelSize(32);
        painter.setFont(font);
        painter.drawText(image.rect(), Qt::AlignCenter, "Export complete");
      }
      return true;
    };
    GuiGrounder::Options options;
    options.maxRetries = 0;
    options.normalized = true;
    options.confirmTarget = true;
    GuiGrounder grounder(ports, options);
    QEventLoop loop;
    QJsonObject result;
    QElapsedTimer timer;
    timer.start();
    grounder.start("the blue Export button", "Export complete is displayed",
                   [&](auto value) {
                     result = value;
                     loop.quit();
                   });
    if (result.isEmpty())
      loop.exec();
    vision.cancel();
    const auto distance = [&](QPoint point) {
      return std::hypot(point.x() - box.center().x(),
                        point.y() - box.center().y());
    };
    result.insert("model_outputs", modelOutputs);
    result.insert("fixture", fixture);
    result.insert("initial_error_px", pointed ? distance(first) : -1);
    result.insert("refined_error_px", pointed ? distance(last) : -1);
    result.insert("requests", requests);
    result.insert("clicks", clicks);
    result.insert("target_hit", clicks > 0 && box.contains(last));
    result.insert("elapsed_ms", timer.elapsed());
    fixtures.append(result);
  }
  std::puts(
      QJsonDocument(
          QJsonObject{{"model", model},
                      {"dataset", "6 synthetic export dialogs (3 dense, small "
                                  "controls); actual local "
                                  "VLM predictions, simulated pointer/clicks"},
                      {"fixtures", fixtures}})
          .toJson(QJsonDocument::Indented)
          .constData());
  return 0;
}

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  const bool live = app.arguments().contains("--live");
  const int visionIndex = app.arguments().indexOf("--vision");
  if (visionIndex >= 0)
    return visionBenchmark(
        app.arguments().value(visionIndex + 1, "gemma4:e2b"));
  QTemporaryDir directory;
  MemoryStore store;
  // Isolate the lexical/dense comparison; association behavior has unit tests.
  store.setFeatures(false, false);
  QString error;
  if (!store.open(directory.path(), &error)) {
    std::fprintf(stderr, "%s\n", qPrintable(error));
    return 1;
  }
  AssistantSettings settings(directory.path() + "/settings", false);
  settings.set("memory.embedding.enabled", live);
  settings.set("memory.backendTimeoutMs", 30000);
  QNetworkAccessManager network;
  Retrieval retrieval(&store, &settings, &network);
  struct Document {
    QString text;
    int topic;
  };
  const QVector<Document> documents = {
      {"Tantivy is a Rust library for building full text search engines.", 0},
      {"Meilisearch indexes documents and offers typo tolerant search in Rust.",
       0},
      {"Nala is a local desktop voice companion with wake word and speech "
       "recognition.",
       1},
      {"Whisper transcribes microphone audio to words offline.", 1},
      {"Qwen3 TTS turns text into natural speech on the local GPU.", 1},
      {"NixOS uses declarative configurations with reproducible builds and "
       "flakes.",
       2},
      {"Hyprland is a Wayland compositor with window and workspace control.",
       2},
      {"Gemma4 understands screenshots and can call structured tools.", 3},
      {"GUI grounding maps button descriptions to pixels and verifies clicks.",
       3},
      {"SQLite FTS5 provides BM25 lexical retrieval for episodic memories.", 4},
      {"Dense embeddings and reciprocal rank fusion provide semantic memory "
       "search.",
       4},
      {"Provenance tracks evidence sources and superseded durable facts.", 4},
      {"A pasta recipe uses tomatoes, basil and olive oil.", 5},
      {"A running route follows the river for five kilometers.", 5},
      {"The gallery displays watercolor landscapes and portraits.", 5},
      {"Chess opening preparation studies the Sicilian Defense.", 5},
      {"The calendar has an appointment at nine tomorrow morning.", 5},
      {"Music playlists contain jazz and electronic songs.", 5},
      {"The cat sleeps beside a sunny window.", 5},
      {"A spreadsheet tracks household grocery expenses.", 5}};
  struct Query {
    QString text;
    int topic;
  };
  const QVector<Query> queries = {
      {"Rust search engine", 0},
      {"spoken audio transcription", 1},
      {"reproducible operating system setup", 2},
      {"find and press an on screen button", 3},
      {"remember decisions by meaning and evidence", 4},
      {"NixOS flakes", 2},
      {"SQLite memory retrieval", 4},
      {"voice companion", 1}};
  const auto embedding = [&](QString text, int topic) {
    if (!live) {
      QVector<float> v(6, 0);
      v[topic] = 1;
      return v;
    }
    QEventLoop loop;
    QVector<float> vector;
    bool finished = false;
    retrieval.embed(text, [&](auto value, auto failure) {
      vector = value;
      error = failure;
      finished = true;
      loop.quit();
    });
    if (!finished)
      loop.exec();
    return vector;
  };
  QVector<qint64> ids;
  QElapsedTimer clock;
  clock.start();
  for (const auto &document : documents) {
    MemoryRecord r;
    r.started = r.lastSeen = QDateTime::currentDateTime();
    r.title = r.summary = document.text;
    r.source = "agent";
    auto id = store.insert(r);
    ids << id;
    const auto vector = embedding(store.embeddingText(id), document.topic);
    if (vector.isEmpty() ||
        !store.storeEmbedding(
            id, "benchmark", semantic::hash(store.embeddingText(id)), vector)) {
      std::fprintf(stderr, "Embedding failed: %s\n", qPrintable(error));
      return 1;
    }
  }
  QJsonObject output{
      {"dataset", "20 synthetic records, 8 labeled queries; no user memories"},
      {"embedding", live ? "qwen3-embedding:0.6b local Ollama"
                         : "controlled one-hot fixture; validates ranking "
                           "mechanics, not model quality"},
      {"index_ms", clock.elapsed()}};
  QJsonArray measurements;
  QVector<QVector<float>> vectors;
  QJsonArray queryTimes;
  for (const auto &query : queries) {
    clock.restart();
    vectors << embedding(query.text, query.topic);
    queryTimes.append(clock.elapsed());
    if (vectors.last().isEmpty())
      return 1;
  }
  output.insert("query_embedding_ms", queryTimes);
  for (int mode = 0; mode < 4; ++mode) {
    double recall = 0, mrr = 0, ndcg = 0;
    QJsonArray times;
    for (int qi = 0; qi < queries.size(); ++qi) {
      QSet<qint64> gold;
      for (int i = 0; i < documents.size(); ++i)
        if (documents[i].topic == queries[qi].topic)
          gold.insert(ids[i]);
      clock.restart();
      QVector<qint64> ranked;
      if (mode == 0)
        for (const auto &r :
             store.search(queries[qi].text, QDateTime::currentDateTime(), 5))
          ranked << r.id;
      else
        for (const auto &hit : store.hybrid(
                 queries[qi].text, QDateTime::currentDateTime(), vectors[qi],
                 "benchmark", mode == 1 ? 0 : 30, 30, 60, 5))
          ranked << hit.id;
      times.append(clock.nsecsElapsed() / 1e6);
      int found = 0;
      double dcg = 0, rr = 0;
      for (int rank = 0; rank < ranked.size(); ++rank)
        if (gold.contains(ranked[rank])) {
          ++found;
          dcg += 1 / std::log2(rank + 2.0);
          if (rr == 0)
            rr = 1. / (rank + 1);
        }
      double ideal = 0;
      for (int rank = 0; rank < std::min(5, int(gold.size())); ++rank)
        ideal += 1 / std::log2(rank + 2.0);
      recall += double(found) / gold.size();
      mrr += rr;
      ndcg += dcg / ideal;
    }
    measurements.append(QJsonObject{
        {"mode", QStringList{"FTS", "dense", "hybrid",
                             "hybrid_rerank_unavailable_fallback"}[mode]},
        {"recall_at_5", recall / queries.size()},
        {"mrr", mrr / queries.size()},
        {"ndcg_at_5", ndcg / queries.size()},
        {"retrieval_ms", times}});
  }
  output.insert("retrieval", measurements);
  QJsonArray gui;
  for (int i = 0; i < 6; ++i) {
    QImage image(640, 400, QImage::Format_RGB32);
    image.fill(Qt::white);
    const QPoint target(60 + 70 * i, 100 + 20 * i);
    {
      QPainter p(&image);
      p.fillRect(QRect(target - QPoint(10, 10), QSize(20, 20)), Qt::blue);
    }
    int predictions = 0;
    QPoint last;
    GuiGrounder::Ports ports;
    ports.capture = [&](auto cb) {
      cb(GuiGrounder::Frame{image, image.rect(), "fixture"}, {});
    };
    ports.predict = [&](QImage, QString prompt, auto cb) {
      ++predictions;
      const bool coarse = prompt.contains("Estimate its center");
      QPoint p = target + (coarse ? QPoint(40, 30) : QPoint());
      cb({{"x", p.x()},
          {"y", p.y()},
          {"confidence", .99},
          {"ready", !coarse && predictions > 2}});
    };
    ports.point = [&](QPoint p) {
      last = p;
      return true;
    };
    ports.click = [&](QString *) {
      image.fill(Qt::green);
      return true;
    };
    GuiGrounder::Options options;
    options.crop = false;
    GuiGrounder grounder(ports, options);
    QEventLoop loop;
    QJsonObject result;
    grounder.start("blue square", {}, [&](auto value) {
      result = value;
      loop.quit();
    });
    if (result.isEmpty())
      loop.exec();
    gui.append(
        QJsonObject{{"fixture", i},
                    {"initial_error_px", 50},
                    {"refined_error_px",
                     std::hypot(last.x() - target.x(), last.y() - target.y())},
                    {"refinements", result["refinements"]},
                    {"success", result["ok"]}});
  }
  output.insert("gui", gui);
  output.insert("gui_note",
                "Scripted predictions on synthetic screenshots; tests loop "
                "behavior, not real VLM accuracy. No live clicks.");
  std::puts(QJsonDocument(output).toJson(QJsonDocument::Indented).constData());
  return 0;
}
