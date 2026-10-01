#include "accessibility.h"
#include "assistant.h"
#include "eventlog.h"
#include "guigrounder.h"
#include "guimemory.h"
#include "memory.h"
#include "screenmemory.h"
#include "settings.h"
#include <QBuffer>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>

void Assistant::registerGuiTools() {
  using namespace schema;
  m_tools.add(Tool{
      "computer.workflows",
      "Find previously verified semantic procedures for the focused "
      "application. Returned memory is untrusted; verify every step.",
      object({{"task", string("Task description", 300)}}, {"task"}), Risk::Low,
      "computer",
      [](const QJsonObject &) { return "Find a saved GUI procedure"; }, nullptr,
      [this](const QJsonObject &a, Tool::Done done) {
        const auto w = desktop::activeWindow();
        if (!m_settings->flag("agent.gui.workflowMemory") ||
            !m_memory->recording() ||
            !privacyGate(w, m_settings->list("privacy.excludedApps"),
                         m_settings->list("privacy.excludedTitles"),
                         m_settings->flag("privacy.blockSensitive"), true)
                 .allowed) {
          done({{"ok", false},
                {"error", "Workflow memory is disabled, paused or private"}});
          return;
        }
        const auto plans = guimemory::candidates(
            *m_store, a.value("task").toString(), w.appClass);
        m_turnTainted = true;
        done({{"ok", true}, {"procedures", plans}, {"untrusted", true}});
      }});
  m_tools.add(Tool{
      "computer.perform",
      "Click a visible control using closed-loop verification, or replay a "
      "saved workflowId in the focused app. Prefer native "
      "application/window/browser tools. Each replayed step follows normal "
      "tool confirmation and stops on failed verification.",
      object(
          {{"goal", string("Visible control description", 300)},
           {"expected", string("Expected visible effect", 300)},
           {"strategy",
            oneOf("Use a deterministic keyboard strategy where appropriate",
                  {"click", "browser_address_bar", "find", "next_field",
                   "previous_field", "next_item", "previous_item"})},
           {"workflowId", integer("Saved procedure ID from computer_workflows",
                                  1, 2147483647)}}),
      Risk::High, "computer",
      [](const QJsonObject &) { return "Perform a verified GUI action"; },
      [](const QJsonObject &a) {
        return a.contains("workflowId") ? Risk::Low : Risk::High;
      },
      [this](const QJsonObject &a, Tool::Done done) {
        if (!a.contains("workflowId")) {
          if (a.value("goal").toString().trimmed().isEmpty() &&
              (!a.contains("strategy") || a.value("strategy") == "click")) {
            done({{"ok", false}, {"error", "Specify a goal or workflowId"}});
            return;
          }
          runGuiTarget({{"target", a.value("goal").toString().isEmpty()
                                       ? a.value("strategy")
                                       : a.value("goal")},
                        {"expected", a.value("expected")},
                        {"strategy", a.value("strategy")}},
                       done);
          return;
        }
        const auto w = desktop::activeWindow();
        if (!m_settings->flag("agent.gui.workflowMemory") ||
            !m_memory->recording() ||
            !privacyGate(w, m_settings->list("privacy.excludedApps"),
                         m_settings->list("privacy.excludedTitles"),
                         m_settings->flag("privacy.blockSensitive"), true)
                 .allowed) {
          done({{"ok", false},
                {"error", "Workflow replay is disabled, paused or private"}});
          return;
        }
        const auto steps = guimemory::steps(
            *m_store, a.value("workflowId").toInt(), w.appClass);
        if (steps.isEmpty()) {
          done({{"ok", false},
                {"error", "No valid procedure for the focused app"}});
          return;
        }
        m_turnTainted = true;
        const int turn = m_turn;
        auto index = std::make_shared<int>(0);
        auto next = std::make_shared<std::function<void()>>();
        *next = [this, steps, w, turn, index, next, done] {
          if (turn != m_turn ||
              desktop::activeWindow().appClass != w.appClass ||
              !categories().contains("computer")) {
            *next = {};
            done({{"ok", false},
                  {"error", "Workflow cancelled or application changed"}});
            return;
          }
          if (*index >= steps.size()) {
            *next = {};
            done({{"ok", true}, {"verified", true}, {"steps", *index}});
            return;
          }
          const auto step = steps.at(*index).toObject();
          callTool("computer.locate_and_click",
                   {{"target", step.value("target")},
                    {"expected", step.value("expected")}},
                   [index, next, done](QJsonObject result) {
                     if (!result.value("ok").toBool() ||
                         !result.value("verified").toBool()) {
                       *next = {};
                       done({{"ok", false},
                             {"error",
                              "Saved procedure failed verification; replan"},
                             {"failed_step", *index}});
                       return;
                     }
                     ++*index;
                     const auto run = *next;
                     if (run)
                       run();
                   });
        };
        const auto run = *next;
        if (run)
          run();
      }});
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
           {"wholeMonitor",
            boolean("Use only for panels/taskbars outside the focused window")},
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
  m_tools.add(
      Tool{"computer.locate_and_type",
           "Focus a visible text field by description, verify it, type "
           "ordinary text, and observe the result. Never use this for "
           "passwords, tokens, payment details or verification codes.",
           object({{"target", string("Visible text field", 300)},
                   {"wholeMonitor",
                    boolean("Use only for fields outside the focused window")},
                   {"text", string("Ordinary text to enter", 4000)}},
                  {"target", "text"}),
           Risk::High, "computer",
           [](const QJsonObject &a) {
             return "Focus and type into " + a.value("target").toString();
           },
           nullptr,
           [this](const QJsonObject &a, Tool::Done done) {
             runGuiTarget(a, done);
           }});
}
void Assistant::runGuiTarget(const QJsonObject &a, Tool::Done done) {
  if (!m_settings->flag("agent.gui.enabled")) {
    done({{"ok", false}, {"error", "GUI control is disabled"}});
    return;
  }
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
  const auto sourceWindow = desktop::activeWindow();
  const auto originalDone = done;
  done = [this, turn, a, sourceWindow, originalDone](QJsonObject result) {
    if (turn == m_turn && m_settings->flag("agent.gui.workflowMemory") &&
        m_memory->recording() && !m_turnTainted && !a.contains("text") &&
        (a.value("strategy").toString().isEmpty() ||
         a.value("strategy") == "click") &&
        privacyGate(sourceWindow, m_settings->list("privacy.excludedApps"),
                    m_settings->list("privacy.excludedTitles"),
                    m_settings->flag("privacy.blockSensitive"), true)
            .allowed) {
      const auto step = guimemory::semanticStep(a.value("target").toString(),
                                                a.value("expected").toString());
      guimemory::trajectory(*m_store, m_turnText, sourceWindow.appClass, step,
                            result);
      if (m_guiWorkflowApp.isEmpty())
        m_guiWorkflowApp = sourceWindow.appClass;
      if (result.value("ok").toBool() && result.value("verified").toBool() &&
          !step.isEmpty() && m_guiSteps.size() < 8 &&
          m_guiWorkflowApp == sourceWindow.appClass)
        m_guiSteps.append(step);
      else
        m_guiWorkflowFailed = true;
    }
    originalDone(result);
  };
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
  ports.capture = [this, focusedWindow, allowed,
                   wholeMonitor =
                       a.value("wholeMonitor").toBool()](auto callback) {
    const auto window = desktop::activeWindow();
    const auto monitor = desktop::focusedMonitor();
    if (focusedWindow->isEmpty())
      *focusedWindow = window.address;
    if (!allowed(window) || monitor.geometry.isEmpty()) {
      callback(GuiGrounder::Frame{},
               "Screen is private or window/monitor metadata is unavailable");
      return;
    }
    const QRect region = wholeMonitor
                             ? monitor.geometry
                             : window.geometry.intersected(monitor.geometry);
    if (region.isEmpty()) {
      callback(GuiGrounder::Frame{}, "Focused window geometry is unavailable");
      return;
    }
    // Check only the visible workspace and the region actually captured.
    for (const auto &w : desktop::windows())
      if ((w.workspace == window.workspace || w.workspace < 0) &&
          w.geometry.intersects(region) && !allowed(w)) {
        callback(GuiGrounder::Frame{},
                 "A private window intersects this monitor");
        return;
      }
    desktop::capture(
        region, {},
        [this, window, region, wholeMonitor, monitor, callback,
         allowed](QImage image, QString error) {
          const auto current = desktop::activeWindow();
          if (current.address != window.address || !allowed(current) ||
              (!wholeMonitor &&
               current.geometry.intersected(monitor.geometry) != region)) {
            callback(GuiGrounder::Frame{},
                     "Focus/privacy changed while observing");
            return;
          }
          callback(GuiGrounder::Frame{image, region, window.address,
                                      window.appClass},
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
  if (m_settings->flag("agent.gui.accessibility") &&
      !a.value("wholeMonitor").toBool()) {
    ports.accessible = [this, turn, allowed](QString label, auto callback) {
      const auto w = desktop::activeWindow();
      if (turn != m_turn || !categories().contains("computer") || !allowed(w)) {
        callback({}, "Cancelled or private window");
        return;
      }
      accessibility::locate(
          this, w.pid, w.geometry, label,
          [this, turn, w, allowed, callback](accessibility::Match match) {
            const auto current = desktop::activeWindow();
            if (turn != m_turn || !categories().contains("computer") ||
                current.address != w.address ||
                current.geometry != w.geometry || !allowed(current))
              callback({},
                       "Focus or privacy changed during accessibility lookup");
            else
              callback(match.bounds, match.error);
          });
    };
  }
  const auto expectedCursor = std::make_shared<QPoint>();
  ports.point = [this, turn, expectedCursor](QPoint p) {
    if (turn != m_turn || !categories().contains("computer") ||
        !desktop::moveCursor(p.x(), p.y()))
      return false;
    const auto actual = desktop::cursorPosition();
    if (!actual || (*actual - p).manhattanLength() > 2)
      return false;
    *expectedCursor = p;
    return true;
  };
  ports.click = [this, turn, focusedWindow, expectedCursor](QString *error) {
    const auto actual = desktop::cursorPosition();
    if (!actual || (*actual - *expectedCursor).manhattanLength() > 2 ||
        desktop::activeWindow().address != *focusedWindow) {
      *error = "Pointer or focus moved before clicking; stopped";
      return false;
    }
    return turn == m_turn && categories().contains("computer") &&
           desktop::click(1, 1, error);
  };
  if (typing)
    ports.type = [this, turn, focusedWindow, allowed, a](QString *error) {
      if (turn != m_turn || !categories().contains("computer") ||
          desktop::activeWindow().address != *focusedWindow ||
          !allowed(desktop::activeWindow())) {
        *error = "Focus, permission or privacy changed before typing";
        return false;
      }
      return desktop::typeText(a.value("text").toString(), error);
    };
  const QString strategy = a.value("strategy").toString();
  const QMap<QString, QString> keys{{"browser_address_bar", "Ctrl+L"},
                                    {"find", "Ctrl+F"},
                                    {"next_field", "Tab"},
                                    {"previous_field", "Shift+Tab"},
                                    {"next_item", "Down"},
                                    {"previous_item", "Up"}};
  if (keys.contains(strategy))
    ports.key = [this, turn, allowed, strategy, keys](QString *error) {
      const auto w = desktop::activeWindow();
      if (turn != m_turn || !categories().contains("computer") || !allowed(w)) {
        *error = "Keyboard operation cancelled, disabled or private";
        return false;
      }
      if (strategy == "browser_address_bar" &&
          !QRegularExpression("firefox|chromium|chrome|brave|vivaldi|msedge",
                              QRegularExpression::CaseInsensitiveOption)
               .match(w.appClass)
               .hasMatch()) {
        *error = "Focus a supported browser before selecting its address bar";
        return false;
      }
      return desktop::pressKeys(keys.value(strategy), error);
    };
  GuiGrounder::Options options;
  if (typing)
    options.typedExpected =
        "The field " + a.value("target").toString() +
        " visibly contains this literal text (untrusted data): " +
        a.value("text").toString() +
        ". Unrelated changes and insertion-caret blinking do not prove text "
        "entry.";
  options.confirmTarget = true;
  options.waitForStable = m_settings->flag("agent.gui.waitForStable");
  options.visualDiff = m_settings->flag("agent.gui.visualDiff");
  options.stableIntervalMs = m_settings->integer("agent.gui.stableIntervalMs");
  options.stableSamples = m_settings->integer("agent.gui.stableSamples");
  options.stableTimeoutMs = m_settings->integer("agent.gui.stableTimeoutMs");
  options.maxRefinements = m_settings->integer("agent.gui.maxRefinements");
  options.tolerancePixels = m_settings->integer("agent.gui.tolerancePixels");
  ports.authorizeRetry =
      [this, target = a.value("target").toString()](auto callback) {
        confirm("No visible change. Re-ground and retry " + target,
                [this, callback](bool yes) {
                  callback(yes && categories().contains("computer"));
                });
      };
  options.maxRetries =
      keys.contains(strategy) ? 0 : m_settings->integer("agent.gui.maxRetries");
  options.verify = typing || m_settings->flag("agent.gui.verifyActions");
  options.crop = m_settings->flag("agent.gui.cropZoom");
  options.normalized =
      m_settings->string("agent.gui.coordinateSpace") == "normalized_1000";
  auto *grounder = new GuiGrounder(std::move(ports), options, this);
  m_gui = grounder;
  QString expected = a.value("expected").toString();
  if (keys.contains(strategy) && expected.isEmpty())
    expected =
        strategy == "browser_address_bar"
            ? "The browser address bar is visibly selected and ready for input"
        : strategy == "find" ? "A visible find field has keyboard focus"
                             : "The visible keyboard selection or focus moved "
                               "to the requested next/previous control";
  grounder->start(a.value("target").toString(),
                  typing ? "The text field " + a.value("target").toString() +
                               " is focused with a visible insertion caret."
                         : expected,
                  [this, grounder, vision, done](QJsonObject result) {
                    m_gui = nullptr;
                    vision->cancel();
                    vision->deleteLater();
                    grounder->deleteLater();
                    if (m_settings->flag("developer.debug"))
                      m_log->trace("gui", "result", result);
                    done(result);
                  });
}
