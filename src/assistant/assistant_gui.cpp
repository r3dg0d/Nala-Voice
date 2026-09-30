#include "assistant.h"
#include "eventlog.h"
#include "guigrounder.h"
#include "settings.h"
#include <QBuffer>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>

void Assistant::registerGuiTools() {
  using namespace schema;
  m_tools.add(Tool{
      "computer.locate_and_click",
      "Find a visible control by its description, refine with marked "
      "screenshots, then click and verify. "
      "Use apps.launch/window.focus for known apps/windows. Never ask the user "
      "for coordinates. "
      "This tool always confirms because a visual click may submit, send or "
      "delete.",
      object(
          {{"target", string("Visible control description", 300)},
           {"expected", string("Expected visible state after clicking", 300)}},
          {"target"}),
      Risk::High, "computer",
      [](const QJsonObject &a) {
        return "Find and click " + a.value("target").toString();
      },
      nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        runGuiTarget(a, done);
      }});
  m_tools.add(Tool{"computer.locate_and_type",
                   "Focus a visible text field by description, verify it, type "
                   "ordinary text, and observe the result. Never use this for "
                   "passwords, tokens, payment details or verification codes.",
                   object({{"target", string("Visible text field", 300)},
                           {"text", string("Ordinary text to enter", 4000)}},
                          {"target", "text"}),
                   Risk::High, "computer",
                   [](const QJsonObject &a) {
                     return "Focus and type into " +
                            a.value("target").toString();
                   },
                   nullptr,
                   [this](const QJsonObject &a, Tool::Done done) {
                     runGuiTarget(a, done);
                   }});
}
void Assistant::runGuiTarget(const QJsonObject &a, Tool::Done done) {
  if (m_gui) {
    done({{"ok", false}, {"error", "Another GUI operation is active"}});
    return;
  }
  if (!visionEnabled() && m_settings->string("llm.visionModel").isEmpty()) {
    done({{"ok", false},
          {"error",
           "Configure llm.visionModel with an installed vision model"}});
    return;
  }
  const int turn = m_turn;
  const bool typing = a.contains("text");
  if (typing &&
      (QRegularExpression(
           "password|secret|token|card|payment|verification|otp|pin\\b",
           QRegularExpression::CaseInsensitiveOption)
           .match(a.value("target").toString())
           .hasMatch() ||
       EventLog::redact(a.value("text").toString()) !=
           a.value("text").toString())) {
    done({{"ok", false},
          {"error", "Visual text entry is not permitted for secrets"}});
    return;
  }
  const auto focusedWindow = std::make_shared<QString>();
  const auto allowed = [this](const WindowInfo &w) {
    return privacyGate(w, m_settings->list("privacy.excludedApps"),
                       m_settings->list("privacy.excludedTitles"),
                       m_settings->flag("privacy.blockSensitive"), true)
        .allowed;
  };
  auto *vision = new LlmClient(&m_network, this);
  auto config = m_memoryLlm->config();
  config.temperature = 0;
  config.maxTokens = 180;
  config.thinking = "off";
  vision->configure(config);
  GuiGrounder::Ports ports;
  ports.capture = [this, focusedWindow, allowed](auto callback) {
    const auto window = desktop::activeWindow();
    const auto monitor = desktop::focusedMonitor();
    if (focusedWindow->isEmpty())
      *focusedWindow = window.address;
    if (!allowed(window) || monitor.geometry.isEmpty()) {
      callback(GuiGrounder::Frame{},
               "Screen is private or window/monitor metadata is unavailable");
      return;
    }
    // A whole-monitor observation may include other visible private
    // windows.
    for (const auto &w : desktop::windows())
      if (w.geometry.intersects(monitor.geometry) && !allowed(w)) {
        callback(GuiGrounder::Frame{},
                 "A private window intersects this monitor");
        return;
      }
    desktop::capture(
        {}, monitor.name,
        [this, window, monitor, callback, allowed](QImage image,
                                                   QString error) {
          const auto current = desktop::activeWindow();
          if (current.address != window.address || !allowed(current)) {
            callback(GuiGrounder::Frame{},
                     "Focus/privacy changed while observing");
            return;
          }
          callback(GuiGrounder::Frame{image, monitor.geometry, window.address},
                   error);
        },
        this);
  };
  ports.predict = [vision](QImage image, QString prompt, auto callback) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    auto links = std::make_shared<QVector<QMetaObject::Connection>>();
    const auto finish = [links, callback](QJsonObject result) {
      for (auto link : *links)
        QObject::disconnect(link);
      callback(result);
    };
    *links << QObject::connect(
        vision, &LlmClient::replied, vision, [finish](const LlmReply &reply) {
          QString text = reply.content.trimmed();
          if (text.startsWith("```")) {
            text = text.section('\n', 1);
            text = text.left(text.lastIndexOf("```"));
          }
          finish(QJsonDocument::fromJson(text.toUtf8()).object());
        });
    *links << QObject::connect(
        vision, &LlmClient::failed, vision,
        [finish](const QString &error) { finish({{"error", error}}); });
    vision->chat(QJsonArray{QJsonObject{
        {"role", "user"},
        {"content",
         QJsonArray{QJsonObject{{"type", "text"}, {"text", prompt}},
                    QJsonObject{
                        {"type", "image_url"},
                        {"image_url",
                         QJsonObject{{"url", "data:image/png;base64," +
                                                 QString::fromLatin1(
                                                     bytes.toBase64())}}}}}}}});
  };
  ports.point = [this, turn](QPoint p) {
    return turn == m_turn && categories().contains("computer") &&
           desktop::moveCursor(p.x(), p.y());
  };
  ports.click = [this, turn](QString *error) {
    return turn == m_turn && categories().contains("computer") &&
           desktop::click(1, 1, error);
  };
  GuiGrounder::Options options;
  options.maxRefinements = m_settings->integer("agent.gui.maxRefinements");
  options.tolerancePixels = m_settings->integer("agent.gui.tolerancePixels");
  ports.authorizeRetry =
      [this, target = a.value("target").toString()](auto callback) {
        confirm("No visible change. Re-ground and retry " + target,
                [this, callback](bool yes) {
                  callback(yes && categories().contains("computer"));
                });
      };
  options.maxRetries = m_settings->integer("agent.gui.maxRetries");
  options.verify = m_settings->flag("agent.gui.verifyActions");
  options.crop = m_settings->flag("agent.gui.cropZoom");
  options.normalized =
      m_settings->string("agent.gui.coordinateSpace") == "normalized_1000";
  auto *grounder = new GuiGrounder(std::move(ports), options, this);
  m_gui = grounder;
  grounder->start(
      a.value("target").toString(),
      typing ? "The text field " + a.value("target").toString() +
                   " is focused with a visible insertion caret."
             : a.value("expected").toString(),
      [this, grounder, vision, done, typing, a, focusedWindow, turn,
       allowed](QJsonObject result) {
        m_gui = nullptr;
        vision->cancel();
        vision->deleteLater();
        grounder->deleteLater();
        if (!typing || !result.value("ok").toBool()) {
          done(result);
          return;
        }
        if (desktop::activeWindow().address != *focusedWindow ||
            !allowed(desktop::activeWindow())) {
          done({{"ok", false}, {"error", "Focus changed before typing"}});
          return;
        }
        const auto monitor = desktop::focusedMonitor();
        desktop::capture(
            {}, monitor.name,
            [this, a, done, result, focusedWindow, monitor, turn,
             allowed](QImage before, QString error) mutable {
              if (turn != m_turn || !categories().contains("computer") ||
                  before.isNull() || !allowed(desktop::activeWindow()) ||
                  desktop::activeWindow().address != *focusedWindow) {
                done({{"ok", false},
                      {"error", error.isEmpty() ? "Focus changed before typing"
                                                : error}});
                return;
              }
              if (!desktop::typeText(a.value("text").toString(), &error)) {
                done({{"ok", false}, {"error", error}});
                return;
              }
              desktop::capture(
                  {}, monitor.name,
                  [done, before, result, focusedWindow](QImage after,
                                                        QString error) mutable {
                    const bool verified =
                        !after.isNull() &&
                        desktop::activeWindow().address == *focusedWindow &&
                        GuiGrounder::difference(before, after) > 0;
                    result.insert("ok", verified);
                    result.insert("typed", true);
                    result.insert("typing_visual_change", verified);
                    if (!verified)
                      result.insert(
                          "error",
                          error.isEmpty()
                              ? "Text entry happened but no stable visual "
                                "change was verified; do not repeat blindly"
                              : error);
                    done(result);
                  },
                  this);
            },
            this);
      });
}
