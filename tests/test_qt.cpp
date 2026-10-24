#define PIPEWIRE_AUTO_ROUTER_UI_TEST
#include "../src/qt_main.cpp"

#include <QtTest>

namespace {

// Each test owns its configuration and every command that the window can run.
// PATH deliberately contains only these stubs, so no test can reach PipeWire.
class RouterUiTests final : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_environment;
    QByteArray m_originalPath;
    QByteArray m_originalConfig;
    QString m_stateDir;

    static bool writeFile(const QString &path, const QByteArray &data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }

    static QByteArray readFile(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static QJsonObject rule(const QString &output, const QString &input, bool enabled = true) {
        return {{QStringLiteral("output"), output}, {QStringLiteral("input"), input},
                {QStringLiteral("enabled"), enabled}, {QStringLiteral("name"), output + QStringLiteral(" → ") + input}};
    }

    static QString leftKey() {
        return linkKey(QStringLiteral("Music:output_FL"), QStringLiteral("Ryzen:playback_FL"));
    }

    static QString rightKey() {
        return linkKey(QStringLiteral("Music:output_FR"), QStringLiteral("Ryzen:playback_FR"));
    }

    QJsonObject stereoConfig() const {
        return {{QStringLiteral("connections"), QJsonArray{
                     rule(QStringLiteral("Music:output_FL"), QStringLiteral("Ryzen:playback_FL")),
                     rule(QStringLiteral("Music:output_FR"), QStringLiteral("Ryzen:playback_FR")),
                     rule(QStringLiteral("Voice:output_FL"), QStringLiteral("Headphones:playback_FL"), false)}},
                {QStringLiteral("devices"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("VirtualMix")}}}},
                {QStringLiteral("rules"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Legacy pattern")}}}}};
    }

    void setConfig(const QJsonObject &config) {
        QVERIFY(writeFile(configPath(), QJsonDocument(config).toJson()));
    }

    QJsonObject savedConfig() const {
        return QJsonDocument::fromJson(readFile(configPath())).object();
    }

    void loadGraph(MainWindow &window) {
        window.refreshGraph(false);
        QCOMPARE(window.m_graph.links, (QSet<QString>{leftKey(), rightKey()}));
    }

    static QTreeWidgetItem *findRoute(MainWindow &window, const QString &source) {
        for (int i = 0; i < window.m_savedRules->topLevelItemCount(); ++i) {
            auto *item = window.m_savedRules->topLevelItem(i);
            if (item->text(0) == source) {
                return item;
            }
        }
        return nullptr;
    }

    QList<QJsonObject> disconnectSnapshots() const {
        const QByteArray log = readFile(m_stateDir + QStringLiteral("/commands"));
        QList<QJsonObject> snapshots;
        qsizetype offset = 0;
        const QByteArray start("BEGIN_CONFIG\n");
        const QByteArray end("END_CONFIG\n");
        while ((offset = log.indexOf(start, offset)) >= 0) {
            offset += start.size();
            const qsizetype last = log.indexOf(end, offset);
            if (last < 0) {
                return {};
            }
            snapshots.append(QJsonDocument::fromJson(log.mid(offset, last - offset)).object());
            offset = last + end.size();
        }
        return snapshots;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_environment.isValid());
        m_originalPath = qgetenv("PATH");
        m_originalConfig = qgetenv("XDG_CONFIG_HOME");
        const QString bin = m_environment.filePath(QStringLiteral("bin"));
        QVERIFY(QDir().mkpath(bin));
        const QByteArray script = R"SH(#!/bin/sh
name=${0##*/}
printf '%s' "$name" >> "$ROUTER_TEST_STATE/commands"
for arg in "$@"; do printf '\t%s' "$arg" >> "$ROUTER_TEST_STATE/commands"; done
printf '\n' >> "$ROUTER_TEST_STATE/commands"
case "$name" in
  pw-dump) printf '[]\n' ;;
  pw-link)
    case "$1" in
      -o) /bin/cat "$ROUTER_TEST_STATE/outputs" ;;
      -i) /bin/cat "$ROUTER_TEST_STATE/inputs" ;;
      -l) /bin/cat "$ROUTER_TEST_STATE/links" ;;
      -d)
        printf 'BEGIN_CONFIG\n' >> "$ROUTER_TEST_STATE/commands"
        /bin/cat "$XDG_CONFIG_HOME/pipewire-auto-router/rules.json" >> "$ROUTER_TEST_STATE/commands"
        printf '\nEND_CONFIG\n' >> "$ROUTER_TEST_STATE/commands"
        : > "$ROUTER_TEST_STATE/links"
        ;;
      -L) ;;
      *) exit 9 ;;
    esac ;;
  pactl) ;;
  python3) printf 'Saved rules applied (test stub)\n' ;;
  *) exit 9 ;;
esac
)SH";
        for (const QString &command : {QStringLiteral("pw-link"), QStringLiteral("pw-dump"), QStringLiteral("pactl"), QStringLiteral("python3")}) {
            const QString executable = QDir(bin).filePath(command);
            QVERIFY(writeFile(executable, script));
            QVERIFY(QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        qputenv("PATH", QFile::encodeName(bin));
    }

    void cleanupTestCase() {
        qputenv("PATH", m_originalPath);
        if (m_originalConfig.isNull()) {
            qunsetenv("XDG_CONFIG_HOME");
        } else {
            qputenv("XDG_CONFIG_HOME", m_originalConfig);
        }
        qunsetenv("ROUTER_TEST_STATE");
    }

    void init() {
        const QString testName = QString::fromLatin1(QTest::currentTestFunction());
        m_stateDir = m_environment.filePath(testName);
        qputenv("ROUTER_TEST_STATE", QFile::encodeName(m_stateDir));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_stateDir + QStringLiteral("/config")));
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/outputs"), "Music:output_FL\nMusic:output_FR\nVoice:output_FL\n"));
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/inputs"), "Ryzen:playback_FL\nRyzen:playback_FR\nHeadphones:playback_FL\n"));
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/links"), "Music:output_FL\n  |-> Ryzen:playback_FL\nMusic:output_FR\n  |-> Ryzen:playback_FR\n"));
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/commands"), {}));
        setConfig(stereoConfig());
    }

    void removeStereoRouteSavesBeforeDisconnecting() {
        MainWindow window(false);
        loadGraph(window);
        auto *route = findRoute(window, QStringLiteral("Music"));
        QVERIFY(route);
        QCOMPARE(route->childCount(), 2);
        window.m_savedRules->setCurrentItem(route);
        QCOMPARE(window.selectedRuleKeys(), (QSet<QString>{leftKey(), rightKey()}));
        QVERIFY(window.m_removeRuleButton->isEnabled());
        window.m_removeRuleButton->click();

        const QJsonObject remaining = savedConfig();
        QCOMPARE(remaining.value(QStringLiteral("connections")).toArray().size(), 1);
        QCOMPARE(remaining.value(QStringLiteral("connections")).toArray().first().toObject().value(QStringLiteral("output")).toString(), QStringLiteral("Voice:output_FL"));
        QCOMPARE(remaining.value(QStringLiteral("devices")), stereoConfig().value(QStringLiteral("devices")));
        QCOMPARE(remaining.value(QStringLiteral("rules")), stereoConfig().value(QStringLiteral("rules")));
        QVERIFY(!window.m_graph.links.contains(leftKey()));
        QVERIFY(!window.m_graph.links.contains(rightKey()));
        QVERIFY(!window.m_autoConnections.contains(leftKey()));
        QVERIFY(!window.m_autoConnections.contains(rightKey()));
        const auto snapshots = disconnectSnapshots();
        QCOMPARE(snapshots.size(), 2);
        for (const auto &snapshot : snapshots) {
            QCOMPARE(snapshot.value(QStringLiteral("connections")), remaining.value(QStringLiteral("connections")));
        }
        window.applySavedConnections(true);
        QVERIFY(window.m_graph.links.isEmpty());
        QVERIFY(!findRoute(window, QStringLiteral("Music")));
    }

    void forgetStereoRouteKeepsLiveConnections() {
        MainWindow window(false);
        loadGraph(window);
        auto *route = findRoute(window, QStringLiteral("Music"));
        QVERIFY(route);
        window.m_savedRules->setCurrentItem(route);
        window.m_forgetRuleButton->click();

        QCOMPARE(savedConfig().value(QStringLiteral("connections")).toArray().size(), 1);
        QCOMPARE(window.m_graph.links, (QSet<QString>{leftKey(), rightKey()}));
        QVERIFY(window.m_autoConnections.isEmpty());
        QVERIFY(disconnectSnapshots().isEmpty());
        QVERIFY(!readFile(m_stateDir + QStringLiteral("/commands")).contains("pw-link\t-d"));
        QVERIFY(!findRoute(window, QStringLiteral("Music")));
    }

    void removingOneExpandedChannelKeepsItsSiblingRule() {
        MainWindow window(false);
        loadGraph(window);
        auto *route = findRoute(window, QStringLiteral("Music"));
        QVERIFY(route);
        window.m_savedRules->setCurrentItem(route->child(0));
        const QSet<QString> selected = window.selectedRuleKeys();
        QCOMPARE(selected.size(), 1);
        QVERIFY(window.removeAndDisconnect(selected));
        QCOMPARE(savedConfig().value(QStringLiteral("connections")).toArray().size(), 2);
        QCOMPARE(window.m_graph.links.size(), 1);
        QVERIFY(!window.m_graph.links.contains(*selected.begin()));
        QCOMPARE(window.m_autoConnections.size(), 1);
        QCOMPARE(disconnectSnapshots().size(), 1);
    }

    void removingLiveRouteAlsoRemovesItsOfflineSavedSibling() {
        MainWindow window(false);
        loadGraph(window);
        window.m_graph.links.remove(rightKey());
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/links"), "Music:output_FL\n  |-> Ryzen:playback_FL\n"));
        const auto [output, input] = splitLinkKey(leftKey());
        const QSet<QString> route = window.routeLinks(output, input);
        QCOMPARE(route, (QSet<QString>{leftKey(), rightKey()}));
        QVERIFY(window.removeAndDisconnect(route));
        const QJsonArray remaining = savedConfig().value(QStringLiteral("connections")).toArray();
        QCOMPARE(remaining.size(), 1);
        QCOMPARE(remaining.first().toObject().value(QStringLiteral("output")).toString(), QStringLiteral("Voice:output_FL"));
        QVERIFY(window.m_autoConnections.isEmpty());
        QCOMPARE(disconnectSnapshots().size(), 1);
    }

    void removingRulesDisconnectsLinksCreatedAfterLastSnapshot() {
        MainWindow window(false);
        loadGraph(window);
        // The watcher can finish reconnecting while a save waits for its lock.
        window.m_graph.links.clear();
        QVERIFY(window.removeAndDisconnect({leftKey(), rightKey()}));
        QCOMPARE(disconnectSnapshots().size(), 2);
        QCOMPARE(savedConfig().value(QStringLiteral("connections")).toArray().size(), 1);
        QVERIFY(window.m_graph.links.isEmpty());
    }

    void disconnectingPortWithoutLiveLinksRemovesItsSavedRule_data() {
        QTest::addColumn<QString>("alias");
        QTest::newRow("enabled") << QStringLiteral("Music:output_FR");
        QTest::newRow("disabled") << QStringLiteral("Voice:output_FL");
    }

    void disconnectingPortWithoutLiveLinksRemovesItsSavedRule() {
        QFETCH(QString, alias);
        MainWindow window(false);
        loadGraph(window);
        window.m_graph.links.clear();
        QVERIFY(writeFile(m_stateDir + QStringLiteral("/links"), {}));
        window.disconnectPortLinks(alias);
        const QJsonArray remaining = savedConfig().value(QStringLiteral("connections")).toArray();
        QCOMPARE(remaining.size(), 2);
        for (const auto &value : remaining) {
            const auto entry = value.toObject();
            QVERIFY(entry.value(QStringLiteral("output")).toString() != alias);
            QVERIFY(entry.value(QStringLiteral("input")).toString() != alias);
        }
        QVERIFY(window.m_graph.links.isEmpty());
        QVERIFY(disconnectSnapshots().isEmpty());
    }

    void disabledOfflineAndUnsupportedRulesStayVisibleAndRemovable() {
        auto config = stereoConfig();
        auto connections = config.value(QStringLiteral("connections")).toArray();
        connections.append(rule(QStringLiteral("Offline:output_FL"), QStringLiteral("Missing:playback_FL")));
        connections.append(rule(QStringLiteral("Midi:events"), QStringLiteral("Ryzen:playback_FL")));
        config.insert(QStringLiteral("connections"), connections);
        setConfig(config);
        MainWindow window(false);
        loadGraph(window);
        QCOMPARE(window.m_savedRules->topLevelItemCount(), 4);
        auto *disabled = findRoute(window, QStringLiteral("Voice"));
        auto *offline = findRoute(window, QStringLiteral("Offline"));
        auto *unsupported = findRoute(window, QStringLiteral("Midi"));
        QVERIFY(disabled);
        QVERIFY(offline);
        QVERIFY(unsupported);
        QCOMPARE(disabled->child(0)->text(2), QStringLiteral("Paused"));
        QCOMPARE(offline->child(0)->text(3), QStringLiteral("Both ports offline"));
        QCOMPARE(unsupported->child(0)->text(2), QStringLiteral("Unsupported"));
        QVERIFY(window.m_savedSummary->text().contains(QStringLiteral("pattern rule")));
        window.m_savedRules->setCurrentItem(disabled);
        QVERIFY(window.m_removeRuleButton->isEnabled());
        window.m_removeRuleButton->click();
        QVERIFY(!findRoute(window, QStringLiteral("Voice")));
        QCOMPARE(savedConfig().value(QStringLiteral("connections")).toArray().size(), 4);
        QVERIFY(disconnectSnapshots().isEmpty());
    }

    void invalidConfigurationPreventsDisconnect_data() {
        QTest::addColumn<QByteArray>("invalid");
        QTest::newRow("broken-json") << QByteArray("{\"connections\": [");
        QTest::newRow("wrong-connections-type") << QByteArray("{\"connections\": {}}");
    }

    void invalidConfigurationPreventsDisconnect() {
        QFETCH(QByteArray, invalid);
        MainWindow window(false);
        loadGraph(window);
        QVERIFY(writeFile(configPath(), invalid));
        QVERIFY(!window.removeAndDisconnect({leftKey(), rightKey()}));
        QCOMPARE(readFile(configPath()), invalid);
        QCOMPARE(window.m_graph.links, (QSet<QString>{leftKey(), rightKey()}));
        QVERIFY(disconnectSnapshots().isEmpty());
        QVERIFY(window.m_log->toPlainText().contains(QStringLiteral("No changes were made")));
    }

    void failedPersistencePreventsDisconnect() {
        MainWindow window(false);
        loadGraph(window);
        const QByteArray original = readFile(configPath());
        // A directory at the lock path fails reliably, even when tests run as root.
        QVERIFY(QDir().mkpath(configPath() + QStringLiteral(".lock")));
        QVERIFY(!window.removeAndDisconnect({leftKey(), rightKey()}));
        QCOMPARE(readFile(configPath()), original);
        QCOMPARE(window.m_graph.links, (QSet<QString>{leftKey(), rightKey()}));
        QVERIFY(window.m_autoConnections.contains(leftKey()));
        QVERIFY(window.m_autoConnections.contains(rightKey()));
        QVERIFY(disconnectSnapshots().isEmpty());
    }

    void newConnectionChoicesHaveExplicitDefaults() {
        MainWindow window(false);
        QCOMPARE(window.m_connectionLifetime->currentText(), QStringLiteral("Connect once"));
        QCOMPARE(window.m_channelMode->currentIndex(), 0);
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("not saved")));
        QVERIFY(!window.m_removeRuleButton->isEnabled());
        QVERIFY(!window.m_forgetRuleButton->isEnabled());
        window.m_connectionLifetime->setCurrentIndex(1);
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("Save reconnect rule")));
        window.m_channelMode->setCurrentIndex(1);
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("two dragged ports"), Qt::CaseInsensitive));
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("Save reconnect rule")));
        window.m_connectionLifetime->setCurrentIndex(0);
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("not saved")));
        QVERIFY(window.m_drawingHelp->text().contains(QStringLiteral("two dragged ports"), Qt::CaseInsensitive));
    }

    void representativeScreenshots() {
        const QString directory = qEnvironmentVariable("ROUTER_UI_SCREENSHOT_DIR");
        if (directory.isEmpty()) {
            QSKIP("Set ROUTER_UI_SCREENSHOT_DIR to capture the two main views");
        }
        QVERIFY(QDir().mkpath(directory));
        auto config = stereoConfig();
        auto connections = config.value(QStringLiteral("connections")).toArray();
        connections.append(rule(QStringLiteral("Offline:output_FL"), QStringLiteral("Missing:playback_FL")));
        config.insert(QStringLiteral("connections"), connections);
        setConfig(config);
        MainWindow window(false);
        loadGraph(window);
        window.show();
        QTest::qWait(30);
        QVERIFY(window.grab().save(QDir(directory).filePath(QStringLiteral("live-connections.png"))));
        window.m_tabs->setCurrentIndex(1);
        window.m_savedRules->expandAll();
        auto *route = findRoute(window, QStringLiteral("Music"));
        QVERIFY(route);
        window.m_savedRules->setCurrentItem(route);
        QTest::qWait(30);
        QVERIFY(window.grab().save(QDir(directory).filePath(QStringLiteral("saved-reconnect-rules.png"))));
    }
};

} // namespace

QTEST_MAIN(RouterUiTests)
#include "test_qt.moc"
