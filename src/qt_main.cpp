#include <QtCore>
#include <QtWidgets>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kCommandTimeoutMs = 6000;
constexpr qreal kNodeWidth = 280.0;
constexpr qreal kTitleHeight = 46.0;
constexpr qreal kRowHeight = 25.0;
constexpr qreal kPortRadius = 6.0;
constexpr int kLinkItemRole = 0;
constexpr int kLinkOutputRole = 1;
constexpr int kLinkInputRole = 2;

struct CommandResult {
    bool ok = false;
    QString output;
    QString error;
};

struct PortInfo {
    QString alias;
    QString nodeName;
    QString portName;
    QString direction;
    QString channel;
    QString mediaClass;
    bool audio = false;
};

struct NodeInfo {
    int id = -1;
    QString nodeName;
    QString displayName;
    QString subtitle;
    QString mediaClass;
    QStringList inputs;
    QStringList outputs;
};

struct Graph {
    QHash<QString, NodeInfo> nodes;
    QHash<QString, PortInfo> ports;
    QStringList inputs;
    QStringList outputs;
    QSet<QString> links;
};

struct SnapshotResult {
    bool ok = false;
    Graph graph;
    QString error;
};

struct PortHit {
    bool valid = false;
    bool output = false;
    QString alias;
};

CommandResult runCommand(const QString &program, const QStringList &args, int timeoutMs = kCommandTimeoutMs) {
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(3000)) {
        return {false, {}, QStringLiteral("Could not start %1").arg(program)};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {false, QString::fromUtf8(process.readAllStandardOutput()), QStringLiteral("%1 timed out").arg(program)};
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {false, output, error.isEmpty() ? QStringLiteral("%1 exited with %2").arg(program).arg(process.exitCode()) : error};
    }
    return {true, output, error};
}

QString property(const QJsonObject &props, const QString &key) {
    const QJsonValue value = props.value(key);
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble());
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

QString stripPortId(QString line) {
    line = line.trimmed();
    static const QRegularExpression re(QStringLiteral("^\\d+\\s+"));
    return line.replace(re, QString()).trimmed();
}

QString nodeFromAlias(const QString &alias) {
    const int index = alias.lastIndexOf(QLatin1Char(':'));
    return index > 0 ? alias.left(index) : alias;
}

QString leafFromAlias(const QString &alias) {
    const int index = alias.lastIndexOf(QLatin1Char(':'));
    return index >= 0 ? alias.mid(index + 1) : alias;
}

QString channelFromName(const QString &name) {
    static const QStringList channels = {
        QStringLiteral("FL"), QStringLiteral("FR"), QStringLiteral("FC"), QStringLiteral("LFE"),
        QStringLiteral("SL"), QStringLiteral("SR"), QStringLiteral("RL"), QStringLiteral("RR"),
        QStringLiteral("FLC"), QStringLiteral("FRC"), QStringLiteral("RC"),
    };
    for (const QString &channel : channels) {
        const QRegularExpression re(QStringLiteral("(?:^|[_-])%1$").arg(QRegularExpression::escape(channel)));
        if (re.match(name).hasMatch()) {
            return channel;
        }
    }
    const QRegularExpression numberRe(QStringLiteral("(?:^|[_-])(\\d+)$"));
    const QRegularExpressionMatch match = numberRe.match(name);
    return match.hasMatch() ? match.captured(1) : QString();
}

int channelRank(const QString &channel) {
    static const QStringList order = {
        QStringLiteral("FL"), QStringLiteral("FR"), QStringLiteral("FC"), QStringLiteral("LFE"),
        QStringLiteral("SL"), QStringLiteral("SR"), QStringLiteral("RL"), QStringLiteral("RR"),
        QStringLiteral("FLC"), QStringLiteral("FRC"), QStringLiteral("RC"),
    };
    const int index = order.indexOf(channel);
    if (index >= 0) {
        return index;
    }
    bool ok = false;
    const int number = channel.toInt(&ok);
    return ok ? 100 + number : 1000;
}

QStringList parsePortList(const QString &text) {
    QStringList ports;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString stripped = stripPortId(line);
        if (!stripped.isEmpty()) {
            ports << stripped;
        }
    }
    return ports;
}

QString linkKey(const QString &output, const QString &input) {
    return output + QLatin1Char('\n') + input;
}

QPair<QString, QString> splitLinkKey(const QString &key) {
    const int index = key.indexOf(QLatin1Char('\n'));
    if (index < 0) {
        return {key, {}};
    }
    return {key.left(index), key.mid(index + 1)};
}

QSet<QString> parseLinks(const QString &text) {
    QSet<QString> links;
    QString current;
    for (const QString &rawLine : text.split(QLatin1Char('\n'))) {
        const QString stripped = stripPortId(rawLine);
        if (stripped.isEmpty()) {
            continue;
        }
        if (stripped.startsWith(QStringLiteral("|->"))) {
            if (!current.isEmpty()) {
                const QString target = stripPortId(stripped.mid(3));
                if (!target.isEmpty()) {
                    links.insert(linkKey(current, target));
                }
            }
            continue;
        }
        if (stripped.startsWith(QStringLiteral("|<-"))) {
            continue;
        }
        current = stripped;
    }
    return links;
}

bool outputKindMatches(const QString &alias, const QString &kind) {
    const QString leaf = leafFromAlias(alias).toLower();
    if (kind == QStringLiteral("any-output")) {
        return true;
    }
    if (kind == QStringLiteral("monitor-output")) {
        return leaf.startsWith(QStringLiteral("monitor"));
    }
    return leaf.startsWith(QStringLiteral("output"));
}

bool inputKindMatches(const QString &alias, const QString &kind) {
    const QString leaf = leafFromAlias(alias).toLower();
    if (kind == QStringLiteral("any-input")) {
        return true;
    }
    if (kind == QStringLiteral("app-input")) {
        return leaf.startsWith(QStringLiteral("input")) || leaf.startsWith(QStringLiteral("capture"));
    }
    return leaf.startsWith(QStringLiteral("playback"));
}

QString outputKindForPort(const QString &alias) {
    const QString leaf = leafFromAlias(alias).toLower();
    if (leaf.startsWith(QStringLiteral("monitor"))) {
        return QStringLiteral("monitor-output");
    }
    if (leaf.startsWith(QStringLiteral("output"))) {
        return QStringLiteral("app-output");
    }
    return QStringLiteral("any-output");
}

QString inputKindForPort(const QString &alias) {
    const QString leaf = leafFromAlias(alias).toLower();
    if (leaf.startsWith(QStringLiteral("playback"))) {
        return QStringLiteral("sink-playback");
    }
    if (leaf.startsWith(QStringLiteral("input")) || leaf.startsWith(QStringLiteral("capture"))) {
        return QStringLiteral("app-input");
    }
    return QStringLiteral("any-input");
}

bool isSupportedAutoConnection(const QString &output, const QString &input) {
    const QString outputKind = outputKindForPort(output);
    return (outputKind == QStringLiteral("app-output") || outputKind == QStringLiteral("monitor-output"))
        && inputKindForPort(input) == QStringLiteral("sink-playback");
}

bool isValidDeviceName(const QString &name) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return re.match(name).hasMatch();
}

bool looksAudio(const QString &format, const QString &mediaClass, const QString &channel) {
    const QString combined = (format + QLatin1Char(' ') + mediaClass).toLower();
    if (combined.contains(QStringLiteral("audio"))) {
        return true;
    }
    static const QSet<QString> audioChannels = {
        QStringLiteral("FL"), QStringLiteral("FR"), QStringLiteral("FC"), QStringLiteral("LFE"),
        QStringLiteral("SL"), QStringLiteral("SR"), QStringLiteral("RL"), QStringLiteral("RR"),
        QStringLiteral("FLC"), QStringLiteral("FRC"), QStringLiteral("RC"),
    };
    return audioChannels.contains(channel);
}

QString configPath() {
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::home().filePath(QStringLiteral(".config")) : QString::fromLocal8Bit(xdg);
    return QDir(base).filePath(QStringLiteral("pipewire-auto-router/rules.json"));
}

QString appSourceDir() {
    const QString installedDataDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(QDir(installedDataDir).filePath(QStringLiteral("pipewire_auto_router.py")))) {
        return installedDataDir;
    }
#ifdef APP_SOURCE_DIR
    return QStringLiteral(APP_SOURCE_DIR);
#else
    return installedDataDir;
#endif
}

void uniqueSort(QStringList &items) {
    items.removeDuplicates();
    std::sort(items.begin(), items.end(), [](const QString &left, const QString &right) {
        const int channelLeft = channelRank(channelFromName(leafFromAlias(left)));
        const int channelRight = channelRank(channelFromName(leafFromAlias(right)));
        if (channelLeft != channelRight) {
            return channelLeft < channelRight;
        }
        return left.toLower() < right.toLower();
    });
}

void appendUnique(QStringList &items, const QString &item) {
    if (!items.contains(item)) {
        items << item;
    }
}

SnapshotResult loadSnapshot() {
    Graph graph;

    const CommandResult dumpResult = runCommand(QStringLiteral("pw-dump"), {});
    if (!dumpResult.ok) {
        return {false, {}, dumpResult.error};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(dumpResult.output.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        return {false, {}, QStringLiteral("Could not parse pw-dump JSON: %1").arg(parseError.errorString())};
    }

    QHash<int, QString> nodeNameById;
    const QJsonArray objects = document.array();
    for (const QJsonValue &value : objects) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("type")).toString().endsWith(QStringLiteral(":Node"))) {
            continue;
        }
        const int id = object.value(QStringLiteral("id")).toInt(-1);
        const QJsonObject props = object.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        QString nodeName = property(props, QStringLiteral("node.name"));
        if (nodeName.isEmpty()) {
            nodeName = QStringLiteral("node-%1").arg(id);
        }
        QString displayName = property(props, QStringLiteral("node.description"));
        if (displayName.isEmpty()) {
            displayName = property(props, QStringLiteral("application.name"));
        }
        if (displayName.isEmpty()) {
            displayName = property(props, QStringLiteral("media.name"));
        }
        if (displayName.isEmpty()) {
            displayName = nodeName;
        }
        const QString mediaClass = property(props, QStringLiteral("media.class"));

        NodeInfo node = graph.nodes.value(nodeName);
        node.id = id;
        node.nodeName = nodeName;
        if (node.displayName.isEmpty() || node.displayName == node.nodeName) {
            node.displayName = displayName;
        }
        if (node.mediaClass.isEmpty()) {
            node.mediaClass = mediaClass;
        } else if (!mediaClass.isEmpty() && !node.mediaClass.contains(mediaClass)) {
            node.mediaClass += QStringLiteral(", ") + mediaClass;
        }
        node.subtitle = node.nodeName;
        if (!node.mediaClass.isEmpty()) {
            node.subtitle += QStringLiteral(" - ") + node.mediaClass;
        }
        graph.nodes.insert(nodeName, node);
        nodeNameById.insert(id, nodeName);
    }

    for (const QJsonValue &value : objects) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("type")).toString().endsWith(QStringLiteral(":Port"))) {
            continue;
        }
        const QJsonObject props = object.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        const QString portAlias = property(props, QStringLiteral("port.alias"));
        const QString portName = property(props, QStringLiteral("port.name"));
        const int nodeId = property(props, QStringLiteral("node.id")).toInt();
        QString nodeName = nodeNameById.value(nodeId);
        if (nodeName.isEmpty()) {
            nodeName = nodeFromAlias(portAlias);
        }
        QString alias = !nodeName.isEmpty() && !portName.isEmpty() ? nodeName + QLatin1Char(':') + portName : portAlias;
        if (alias.isEmpty()) {
            continue;
        }

        const QString mediaClass = graph.nodes.value(nodeName).mediaClass;
        QString channel = property(props, QStringLiteral("audio.channel"));
        if (channel.isEmpty()) {
            channel = channelFromName(portName.isEmpty() ? leafFromAlias(alias) : portName);
        }

        PortInfo port;
        port.alias = alias;
        port.nodeName = nodeName;
        port.portName = portName.isEmpty() ? leafFromAlias(alias) : portName;
        port.direction = property(props, QStringLiteral("port.direction"));
        port.channel = channel;
        port.mediaClass = mediaClass;
        port.audio = looksAudio(property(props, QStringLiteral("format.dsp")), mediaClass, channel);
        graph.ports.insert(alias, port);

        NodeInfo node = graph.nodes.value(nodeName);
        if (node.nodeName.isEmpty()) {
            node.nodeName = nodeName;
            node.displayName = nodeName;
            node.subtitle = nodeName;
        }
        if (port.direction == QStringLiteral("out")) {
            appendUnique(node.outputs, alias);
        } else if (port.direction == QStringLiteral("in")) {
            appendUnique(node.inputs, alias);
        }
        graph.nodes.insert(nodeName, node);
    }

    const CommandResult outputsResult = runCommand(QStringLiteral("pw-link"), {QStringLiteral("-o")});
    if (!outputsResult.ok) {
        return {false, {}, outputsResult.error};
    }
    const CommandResult inputsResult = runCommand(QStringLiteral("pw-link"), {QStringLiteral("-i")});
    if (!inputsResult.ok) {
        return {false, {}, inputsResult.error};
    }
    const CommandResult linksResult = runCommand(QStringLiteral("pw-link"), {QStringLiteral("-l")});
    if (!linksResult.ok) {
        return {false, {}, linksResult.error};
    }

    graph.outputs = parsePortList(outputsResult.output);
    graph.inputs = parsePortList(inputsResult.output);
    graph.links = parseLinks(linksResult.output);

    auto ensurePort = [&graph](const QString &alias, const QString &direction) {
        PortInfo port = graph.ports.value(alias);
        if (port.alias.isEmpty()) {
            port.alias = alias;
            port.nodeName = nodeFromAlias(alias);
            port.portName = leafFromAlias(alias);
            port.direction = direction;
            port.channel = channelFromName(port.portName);
            port.audio = looksAudio({}, {}, port.channel);
            graph.ports.insert(alias, port);
        }
        NodeInfo node = graph.nodes.value(port.nodeName);
        if (node.nodeName.isEmpty()) {
            node.nodeName = port.nodeName;
            node.displayName = port.nodeName;
            node.subtitle = port.nodeName;
        }
        if (direction == QStringLiteral("out")) {
            appendUnique(node.outputs, alias);
        } else {
            appendUnique(node.inputs, alias);
        }
        graph.nodes.insert(node.nodeName, node);
    };

    for (const QString &port : graph.outputs) {
        ensurePort(port, QStringLiteral("out"));
    }
    for (const QString &port : graph.inputs) {
        ensurePort(port, QStringLiteral("in"));
    }
    for (auto it = graph.nodes.begin(); it != graph.nodes.end(); ++it) {
        uniqueSort(it->inputs);
        uniqueSort(it->outputs);
    }

    return {true, graph, {}};
}

class NodeItem final : public QGraphicsItem {
public:
    NodeItem(NodeInfo node, QStringList inputs, QStringList outputs, QHash<QString, PortInfo> ports)
        : m_node(std::move(node)), m_inputs(std::move(inputs)), m_outputs(std::move(outputs)), m_ports(std::move(ports)) {
        setZValue(10);
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    }

    QRectF boundingRect() const override {
        return QRectF(0, 0, kNodeWidth, height());
    }

    qreal height() const {
        const int rows = std::max(m_inputs.size(), m_outputs.size());
        return kTitleHeight + 18.0 + std::max(1, rows) * kRowHeight;
    }

    QString nodeName() const {
        return m_node.nodeName;
    }

    QPointF anchorFor(const QString &alias) const {
        int index = m_inputs.indexOf(alias);
        if (index >= 0) {
            return mapToScene(QPointF(0, rowY(index)));
        }
        index = m_outputs.indexOf(alias);
        if (index >= 0) {
            return mapToScene(QPointF(kNodeWidth, rowY(index)));
        }
        return {};
    }

    PortHit portAt(const QPointF &local) const {
        for (int i = 0; i < m_inputs.size(); ++i) {
            const QPointF anchor(0, rowY(i));
            if (QLineF(local, anchor).length() <= 14.0 || QRectF(0, anchor.y() - 10, 150, 20).contains(local)) {
                return {true, false, m_inputs.at(i)};
            }
        }
        for (int i = 0; i < m_outputs.size(); ++i) {
            const QPointF anchor(kNodeWidth, rowY(i));
            if (QLineF(local, anchor).length() <= 14.0 || QRectF(kNodeWidth - 150, anchor.y() - 10, 150, 20).contains(local)) {
                return {true, true, m_outputs.at(i)};
            }
        }
        return {};
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF rect = boundingRect();
        const QString lower = (m_node.nodeName + QLatin1Char(' ') + m_node.displayName + QLatin1Char(' ') + m_node.mediaClass).toLower();
        QColor header = QColor(QStringLiteral("#e8edf7"));
        if (lower.contains(QStringLiteral("obs-"))) {
            header = QColor(QStringLiteral("#fff1d4"));
        } else if (lower.contains(QStringLiteral("audio/sink"))) {
            header = QColor(QStringLiteral("#dff0e8"));
        } else if (lower.contains(QStringLiteral("input")) || lower.contains(QStringLiteral("source"))) {
            header = QColor(QStringLiteral("#e9e4f5"));
        }

        painter->setPen(QPen(QColor(QStringLiteral("#b9c0c7")), 1.0));
        painter->setBrush(QColor(QStringLiteral("#fbfcfd")));
        painter->drawRoundedRect(rect, 8, 8);

        painter->setPen(Qt::NoPen);
        painter->setBrush(header);
        painter->drawRoundedRect(QRectF(0, 0, kNodeWidth, kTitleHeight), 8, 8);
        painter->drawRect(QRectF(0, kTitleHeight - 8, kNodeWidth, 8));

        QFont titleFont = painter->font();
        titleFont.setBold(true);
        painter->setFont(titleFont);
        painter->setPen(QColor(QStringLiteral("#18202a")));
        const QFontMetrics titleMetrics(titleFont);
        painter->drawText(QRectF(12, 8, kNodeWidth - 24, 18), Qt::AlignVCenter,
                          titleMetrics.elidedText(m_node.displayName, Qt::ElideRight, int(kNodeWidth - 24)));

        QFont subtitleFont = painter->font();
        subtitleFont.setBold(false);
        subtitleFont.setPointSize(std::max(8, subtitleFont.pointSize() - 1));
        painter->setFont(subtitleFont);
        painter->setPen(QColor(QStringLiteral("#59636e")));
        const QFontMetrics subtitleMetrics(subtitleFont);
        painter->drawText(QRectF(12, 26, kNodeWidth - 24, 16), Qt::AlignVCenter,
                          subtitleMetrics.elidedText(m_node.subtitle, Qt::ElideRight, int(kNodeWidth - 24)));

        QFont portFont = painter->font();
        portFont.setPointSize(std::max(8, portFont.pointSize()));
        painter->setFont(portFont);
        const QFontMetrics portMetrics(portFont);

        for (int i = 0; i < m_inputs.size(); ++i) {
            drawPort(painter, portMetrics, m_inputs.at(i), false, rowY(i));
        }
        for (int i = 0; i < m_outputs.size(); ++i) {
            drawPort(painter, portMetrics, m_outputs.at(i), true, rowY(i));
        }
    }

private:
    qreal rowY(int index) const {
        return kTitleHeight + 18.0 + index * kRowHeight + kRowHeight / 2.0;
    }

    QString portLabel(const QString &alias) const {
        const PortInfo port = m_ports.value(alias);
        const QString label = port.portName.isEmpty() ? leafFromAlias(alias) : port.portName;
        return label;
    }

    void drawPort(QPainter *painter, const QFontMetrics &metrics, const QString &alias, bool output, qreal y) const {
        const QPointF center(output ? kNodeWidth : 0, y);
        const QColor fill = output ? QColor(QStringLiteral("#3f6fb5")) : QColor(QStringLiteral("#318060"));
        painter->setPen(QPen(QColor(QStringLiteral("#ffffff")), 1.2));
        painter->setBrush(fill);
        painter->drawEllipse(center, kPortRadius, kPortRadius);

        painter->setPen(QColor(QStringLiteral("#2d3741")));
        const QString text = metrics.elidedText(portLabel(alias), Qt::ElideMiddle, 112);
        if (output) {
            painter->drawText(QRectF(kNodeWidth - 128, y - 9, 116, 18), Qt::AlignRight | Qt::AlignVCenter, text);
        } else {
            painter->drawText(QRectF(12, y - 9, 116, 18), Qt::AlignLeft | Qt::AlignVCenter, text);
        }
    }

    NodeInfo m_node;
    QStringList m_inputs;
    QStringList m_outputs;
    QHash<QString, PortInfo> m_ports;
};

class PatchbayView final : public QGraphicsView {
public:
    using PressHandler = std::function<bool(const QPointF &, Qt::MouseButton, const QPoint &)>;
    using MoveHandler = std::function<bool(const QPointF &)>;
    using ReleaseHandler = std::function<bool(const QPointF &, Qt::MouseButton)>;
    using SceneChangedHandler = std::function<void()>;

    explicit PatchbayView(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    }

    PressHandler onPress;
    MoveHandler onMove;
    ReleaseHandler onRelease;
    SceneChangedHandler onSceneChanged;

protected:
    void wheelEvent(QWheelEvent *event) override {
        const qreal factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
        scale(factor, factor);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (onPress && onPress(mapToScene(event->position().toPoint()), event->button(), event->globalPosition().toPoint())) {
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (onMove && onMove(mapToScene(event->position().toPoint()))) {
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
        if (onSceneChanged) {
            onSceneChanged();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (onRelease && onRelease(mapToScene(event->position().toPoint()), event->button())) {
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
        if (onSceneChanged) {
            onSceneChanged();
        }
    }
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(QStringLiteral("PipeWire Auto Router"));
        resize(1280, 820);

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        auto *toolbar = new QHBoxLayout();
        auto *refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
        auto *applyButton = new QPushButton(QStringLiteral("Apply auto-reconnect"), this);
        auto *createDeviceButton = new QPushButton(QStringLiteral("Create Fake Audio"), this);
        auto *savedButton = new QPushButton(QStringLiteral("Auto-connect Status..."), this);
        savedButton->setToolTip(QStringLiteral("See which saved links are connected and which devices are available"));
        m_audioOnly = new QCheckBox(QStringLiteral("Audio only"), this);
        m_audioOnly->setChecked(true);
        m_pairChannels = new QCheckBox(QStringLiteral("Connect matching channels"), this);
        m_pairChannels->setChecked(true);
        m_autoReconnectNewLinks = new QCheckBox(QStringLiteral("Auto-reconnect new links"), this);
        m_autoReconnectNewLinks->setChecked(false);
        m_autoApplyReconnect = new QCheckBox(QStringLiteral("Auto-apply reconnect"), this);
        m_autoApplyReconnect->setChecked(true);
        m_liveRefresh = new QCheckBox(QStringLiteral("Live refresh"), this);
        m_liveRefresh->setChecked(true);

        toolbar->addWidget(refreshButton);
        toolbar->addWidget(applyButton);
        toolbar->addWidget(createDeviceButton);
        toolbar->addWidget(savedButton);
        toolbar->addSpacing(12);
        toolbar->addWidget(m_audioOnly);
        toolbar->addWidget(m_pairChannels);
        toolbar->addWidget(m_autoReconnectNewLinks);
        toolbar->addWidget(m_autoApplyReconnect);
        toolbar->addStretch(1);
        toolbar->addWidget(m_liveRefresh);
        layout->addLayout(toolbar);

        m_scene = new QGraphicsScene(this);
        m_scene->setBackgroundBrush(QColor(QStringLiteral("#eef1f3")));
        m_view = new PatchbayView(this);
        m_view->setScene(m_scene);
        layout->addWidget(m_view, 1);

        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        m_log->setMaximumBlockCount(300);
        m_log->setFixedHeight(118);
        layout->addWidget(m_log);

        setCentralWidget(central);
        statusBar()->showMessage(QStringLiteral("Ready"));

        connect(refreshButton, &QPushButton::clicked, this, [this] { refreshGraph(true); });
        connect(applyButton, &QPushButton::clicked, this, [this] { applySavedConnections(false); });
        connect(createDeviceButton, &QPushButton::clicked, this, [this] { showCreateFakeAudioDialog(); });
        connect(savedButton, &QPushButton::clicked, this, [this] { showSavedDialog(); });
        connect(m_audioOnly, &QCheckBox::toggled, this, [this] { rebuildScene(); });

        m_view->onPress = [this](const QPointF &pos, Qt::MouseButton button, const QPoint &globalPos) {
            return handlePress(pos, button, globalPos);
        };
        m_view->onMove = [this](const QPointF &pos) {
            return handleMove(pos);
        };
        m_view->onRelease = [this](const QPointF &pos, Qt::MouseButton button) {
            return handleRelease(pos, button);
        };
        m_view->onSceneChanged = [this] {
            updateLinkPaths();
        };

        connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
            if (m_dragPort.isEmpty() && m_autoApplyReconnect->isChecked()) {
                applySavedConnections(true);
                return;
            }
            if (m_dragPort.isEmpty() && m_liveRefresh->isChecked()) {
                refreshGraph(false);
            }
        });
        m_refreshTimer.start(2500);
        refreshGraph(true);
    }

private:
    void refreshGraph(bool userVisible) {
        const SnapshotResult snapshot = loadSnapshot();
        if (!snapshot.ok) {
            appendLog(QStringLiteral("PipeWire error: %1").arg(snapshot.error));
            statusBar()->showMessage(QStringLiteral("PipeWire error"));
            return;
        }
        m_graph = snapshot.graph;
        loadAutoConnections();
        rebuildScene();
        int connectedAutomatically = 0;
        for (const QString &key : std::as_const(m_autoConnections)) {
            if (m_graph.links.contains(key)) {
                ++connectedAutomatically;
            }
        }
        statusBar()->showMessage(QStringLiteral("%1 outputs, %2 inputs, %3 links - %4/%5 auto-connect links active")
                                     .arg(m_graph.outputs.size())
                                     .arg(m_graph.inputs.size())
                                     .arg(m_graph.links.size())
                                     .arg(connectedAutomatically)
                                     .arg(m_autoConnections.size()));
        if (userVisible) {
            appendLog(QStringLiteral("Loaded %1 nodes").arg(m_graph.nodes.size()));
        }
    }

    void rebuildScene() {
        rememberNodePositions();
        m_scene->clear();
        m_items.clear();
        m_anchors.clear();
        m_linkItems.clear();
        m_preview = nullptr;

        struct DisplayNode {
            NodeInfo node;
            QStringList inputs;
            QStringList outputs;
            int column = 1;
        };

        QList<DisplayNode> nodes;
        for (const NodeInfo &node : m_graph.nodes) {
            QStringList inputs;
            QStringList outputs;
            for (const QString &alias : node.inputs) {
                const PortInfo port = m_graph.ports.value(alias);
                if (!m_audioOnly->isChecked() || port.audio) {
                    inputs << alias;
                }
            }
            for (const QString &alias : node.outputs) {
                const PortInfo port = m_graph.ports.value(alias);
                if (!m_audioOnly->isChecked() || port.audio) {
                    outputs << alias;
                }
            }
            uniqueSort(inputs);
            uniqueSort(outputs);
            if (inputs.isEmpty() && outputs.isEmpty()) {
                continue;
            }
            DisplayNode display{node, inputs, outputs, columnFor(node, inputs, outputs)};
            nodes << display;
        }

        std::sort(nodes.begin(), nodes.end(), [](const DisplayNode &left, const DisplayNode &right) {
            if (left.column != right.column) {
                return left.column < right.column;
            }
            return left.node.displayName.toLower() < right.node.displayName.toLower();
        });

        QVector<qreal> nextY = {30.0, 30.0, 30.0};
        const QVector<qreal> x = {30.0, 390.0, 750.0};
        for (const DisplayNode &display : nodes) {
            auto *item = new NodeItem(display.node, display.inputs, display.outputs, m_graph.ports);
            if (m_nodePositions.contains(display.node.nodeName)) {
                item->setPos(m_nodePositions.value(display.node.nodeName));
            } else {
                item->setPos(x.at(display.column), nextY[display.column]);
            }
            m_scene->addItem(item);
            m_items.insert(display.node.nodeName, item);
            nextY[display.column] += item->height() + 30.0;
        }

        refreshAnchors();

        for (const QString &key : std::as_const(m_graph.links)) {
            const auto [output, input] = splitLinkKey(key);
            if (m_anchors.contains(output) && m_anchors.contains(input)) {
                const bool autoReconnect = m_autoConnections.contains(key);
                const bool madeThisSession = m_sessionLinks.contains(key);
                addLinkPath(
                    output,
                    input,
                    autoReconnect ? QColor(QStringLiteral("#d47424"))
                                  : madeThisSession ? QColor(QStringLiteral("#1f9d73"))
                                                    : QColor(QStringLiteral("#496c9d")),
                    autoReconnect ? 3.2 : madeThisSession ? 2.8 : 2.2,
                    Qt::SolidLine);
            }
        }

        const qreal maxY = *std::max_element(nextY.begin(), nextY.end());
        updateSceneRect(QRectF(0, 0, 1060, std::max(maxY + 40.0, 650.0)));
    }

    void rememberNodePositions() {
        for (auto it = m_items.begin(); it != m_items.end(); ++it) {
            if (it.value()) {
                m_nodePositions.insert(it.key(), it.value()->pos());
            }
        }
    }

    void refreshAnchors() {
        m_anchors.clear();
        for (NodeItem *item : std::as_const(m_items)) {
            for (const QString &alias : m_graph.nodes.value(item->nodeName()).inputs) {
                if ((!m_audioOnly->isChecked() || m_graph.ports.value(alias).audio) && !item->anchorFor(alias).isNull()) {
                    m_anchors.insert(alias, item->anchorFor(alias));
                }
            }
            for (const QString &alias : m_graph.nodes.value(item->nodeName()).outputs) {
                if ((!m_audioOnly->isChecked() || m_graph.ports.value(alias).audio) && !item->anchorFor(alias).isNull()) {
                    m_anchors.insert(alias, item->anchorFor(alias));
                }
            }
        }
    }

    void updateSceneRect(QRectF base) {
        for (NodeItem *item : std::as_const(m_items)) {
            contentRectExpand(base, item->sceneBoundingRect().adjusted(-80, -80, 160, 160));
        }
        m_scene->setSceneRect(base);
    }

    void contentRectExpand(QRectF &base, const QRectF &addition) const {
        base = base.united(addition);
    }

    void updateLinkPaths() {
        rememberNodePositions();
        refreshAnchors();
        for (auto it = m_linkItems.begin(); it != m_linkItems.end(); ++it) {
            const auto [output, input] = splitLinkKey(it.key());
            QGraphicsPathItem *item = it.value();
            if (!item) {
                continue;
            }
            const bool visible = m_anchors.contains(output) && m_anchors.contains(input);
            item->setVisible(visible);
            if (visible) {
                item->setPath(linkPath(m_anchors.value(output), m_anchors.value(input)));
            }
        }
        updateSceneRect(m_scene->sceneRect());
    }

    int columnFor(const NodeInfo &node, const QStringList &inputs, const QStringList &outputs) const {
        const QString text = (node.nodeName + QLatin1Char(' ') + node.displayName + QLatin1Char(' ') + node.mediaClass).toLower();
        const bool hasPlayback = std::any_of(inputs.begin(), inputs.end(), [](const QString &alias) {
            return leafFromAlias(alias).toLower().startsWith(QStringLiteral("playback"));
        });
        if (text.contains(QStringLiteral("audio/sink")) || hasPlayback) {
            return 2;
        }
        if (text.contains(QStringLiteral("stream/output/audio")) || (!outputs.isEmpty() && inputs.isEmpty())) {
            return 0;
        }
        if (!inputs.isEmpty() && outputs.isEmpty()) {
            return 2;
        }
        return 1;
    }

    QPainterPath linkPath(const QPointF &start, const QPointF &end) const {
        QPainterPath path(start);
        const qreal dx = std::max<qreal>(90.0, std::abs(end.x() - start.x()) * 0.45);
        path.cubicTo(start + QPointF(dx, 0), end - QPointF(dx, 0), end);
        return path;
    }

    void addLinkPath(const QString &output, const QString &input, const QColor &color, qreal width, Qt::PenStyle style) {
        auto *item = m_scene->addPath(linkPath(m_anchors.value(output), m_anchors.value(input)), QPen(color, width, style, Qt::RoundCap, Qt::RoundJoin));
        item->setZValue(-10);
        item->setData(kLinkItemRole, QStringLiteral("link"));
        item->setData(kLinkOutputRole, output);
        item->setData(kLinkInputRole, input);
        item->setFlag(QGraphicsItem::ItemIsSelectable, true);
        m_linkItems.insert(linkKey(output, input), item);
    }

    PortHit findPort(const QPointF &scenePos) const {
        for (NodeItem *item : m_items) {
            const QPointF local = item->mapFromScene(scenePos);
            if (!item->boundingRect().adjusted(-18, -18, 18, 18).contains(local)) {
                continue;
            }
            const PortHit hit = item->portAt(local);
            if (hit.valid) {
                return hit;
            }
        }
        return {};
    }

    QPair<QString, QString> findLink(const QPointF &scenePos) const {
        for (QGraphicsItem *item : m_scene->items(scenePos)) {
            if (item->data(kLinkItemRole).toString() != QStringLiteral("link")) {
                continue;
            }
            const QString output = item->data(kLinkOutputRole).toString();
            const QString input = item->data(kLinkInputRole).toString();
            if (!output.isEmpty() && !input.isEmpty()) {
                return {output, input};
            }
        }
        return {};
    }

    bool handlePress(const QPointF &scenePos, Qt::MouseButton button, const QPoint &globalPos) {
        const PortHit hit = findPort(scenePos);
        if (button == Qt::RightButton && hit.valid) {
            QMenu menu;
            QAction *disconnect = menu.addAction(QStringLiteral("Disconnect port links"));
            QAction *chosen = menu.exec(globalPos);
            if (chosen == disconnect) {
                disconnectPortLinks(hit.alias);
            }
            return true;
        }
        if (button == Qt::RightButton) {
            const auto [output, input] = findLink(scenePos);
            if (!output.isEmpty() && !input.isEmpty()) {
                QMenu menu;
                const QString key = linkKey(output, input);
                const bool autoReconnect = m_autoConnections.contains(key);
                const bool madeThisSession = m_sessionLinks.contains(key);
                const bool supportedAutoConnection = isSupportedAutoConnection(output, input);
                QAction *toggleAuto = nullptr;
                if (autoReconnect) {
                    toggleAuto = menu.addAction(QStringLiteral("Disable auto-reconnect for this connection"));
                } else if (madeThisSession && supportedAutoConnection) {
                    toggleAuto = menu.addAction(QStringLiteral("Enable auto-reconnect for this connection"));
                } else if (!supportedAutoConnection) {
                    QAction *unsupported = menu.addAction(QStringLiteral("Auto-reconnect supports output/monitor -> playback"));
                    unsupported->setEnabled(false);
                } else {
                    QAction *notSessionLink = menu.addAction(QStringLiteral("Auto-reconnect is only for links made in this session"));
                    notSessionLink->setEnabled(false);
                }
                QAction *disconnect = menu.addAction(QStringLiteral("Disconnect this connection"));
                QAction *chosen = menu.exec(globalPos);
                if (toggleAuto && chosen == toggleAuto) {
                    setAutoReconnect(output, input, !autoReconnect);
                } else if (chosen == disconnect) {
                    disconnectOne(output, input);
                    refreshGraph(false);
                }
                return true;
            }
        }
        if (button != Qt::LeftButton || !hit.valid) {
            return false;
        }

        m_dragPort = hit.alias;
        m_dragFromOutput = hit.output;
        m_view->setDragMode(QGraphicsView::NoDrag);
        m_preview = m_scene->addPath(QPainterPath(), QPen(QColor(QStringLiteral("#d07a28")), 2.0, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
        m_preview->setZValue(-5);
        handleMove(scenePos);
        return true;
    }

    bool handleMove(const QPointF &scenePos) {
        if (m_dragPort.isEmpty() || !m_preview) {
            return false;
        }
        const QPointF anchor = m_anchors.value(m_dragPort);
        if (m_dragFromOutput) {
            m_preview->setPath(linkPath(anchor, scenePos));
        } else {
            m_preview->setPath(linkPath(scenePos, anchor));
        }
        return true;
    }

    bool handleRelease(const QPointF &scenePos, Qt::MouseButton button) {
        if (button != Qt::LeftButton || m_dragPort.isEmpty()) {
            return false;
        }

        const PortHit target = findPort(scenePos);
        QString output;
        QString input;
        if (target.valid && target.output != m_dragFromOutput) {
            output = m_dragFromOutput ? m_dragPort : target.alias;
            input = m_dragFromOutput ? target.alias : m_dragPort;
        }

        if (m_preview) {
            m_scene->removeItem(m_preview);
            delete m_preview;
            m_preview = nullptr;
        }
        m_dragPort.clear();
        m_view->setDragMode(QGraphicsView::ScrollHandDrag);

        if (!output.isEmpty() && !input.isEmpty()) {
            connectVisual(output, input);
        }
        return true;
    }

    void connectVisual(const QString &output, const QString &input) {
        const PortInfo outputPort = m_graph.ports.value(output);
        const PortInfo inputPort = m_graph.ports.value(input);
        if (outputPort.alias.isEmpty() || inputPort.alias.isEmpty()) {
            appendLog(QStringLiteral("Could not resolve ports for connection"));
            return;
        }

        const QString sourceKind = outputKindForPort(output);
        const QString targetKind = inputKindForPort(input);
        QList<QPair<QString, QString>> pairs;
        if (m_pairChannels->isChecked()) {
            pairs = matchingPairs(outputPort.nodeName, inputPort.nodeName, sourceKind, targetKind, output, input);
        } else {
            pairs << qMakePair(output, input);
        }

        QList<QPair<QString, QString>> linksToDisconnect;
        if (!chooseLinksToDisconnect(pairs, &linksToDisconnect)) {
            appendLog(QStringLiteral("Connection cancelled"));
            return;
        }
        for (const auto &pair : linksToDisconnect) {
            disconnectOne(pair.first, pair.second);
        }

        int changed = 0;
        QList<QPair<QString, QString>> createdPairs;
        for (const auto &pair : pairs) {
            if (connectOne(pair.first, pair.second)) {
                ++changed;
                createdPairs << pair;
                m_sessionLinks.insert(linkKey(pair.first, pair.second));
            }
        }

        if (m_autoReconnectNewLinks->isChecked() && !createdPairs.isEmpty()) {
            saveAutoReconnectForPairs(createdPairs);
        }
        appendLog(QStringLiteral("Connected %1 channel%2 from %3 to %4")
                      .arg(changed)
                      .arg(changed == 1 ? QString() : QStringLiteral("s"))
                      .arg(displayForNode(outputPort.nodeName))
                      .arg(displayForNode(inputPort.nodeName)));
        refreshGraph(false);
    }

    QList<QPair<QString, QString>> matchingPairs(
        const QString &sourceNode,
        const QString &targetNode,
        const QString &sourceKind,
        const QString &targetKind,
        const QString &selectedOutput,
        const QString &selectedInput) const {
        QList<QPair<QString, QString>> pairs;
        const NodeInfo source = m_graph.nodes.value(sourceNode);
        const NodeInfo target = m_graph.nodes.value(targetNode);

        QHash<QString, QString> inputByChannel;
        for (const QString &input : target.inputs) {
            const PortInfo port = m_graph.ports.value(input);
            if (!port.audio || !inputKindMatches(input, targetKind)) {
                continue;
            }
            if (!port.channel.isEmpty() && !inputByChannel.contains(port.channel)) {
                inputByChannel.insert(port.channel, input);
            }
        }

        for (const QString &output : source.outputs) {
            const PortInfo port = m_graph.ports.value(output);
            if (!port.audio || !outputKindMatches(output, sourceKind)) {
                continue;
            }
            QString matchedInput;
            if (!port.channel.isEmpty()) {
                matchedInput = inputByChannel.value(port.channel);
            }
            if (matchedInput.isEmpty() && output == selectedOutput) {
                matchedInput = selectedInput;
            }
            if (!matchedInput.isEmpty()) {
                pairs << qMakePair(output, matchedInput);
            }
        }

        if (pairs.isEmpty()) {
            pairs << qMakePair(selectedOutput, selectedInput);
        }
        return pairs;
    }

    bool chooseLinksToDisconnect(const QList<QPair<QString, QString>> &desiredPairs, QList<QPair<QString, QString>> *selected) {
        selected->clear();
        QSet<QString> candidatesSeen;
        QList<QPair<QString, QString>> candidates;
        for (const auto &desired : desiredPairs) {
            const QStringList links = QStringList(m_graph.links.begin(), m_graph.links.end());
            for (const QString &key : links) {
                const auto [existingOutput, existingInput] = splitLinkKey(key);
                if (existingOutput != desired.first || existingInput == desired.second || candidatesSeen.contains(key)) {
                    continue;
                }
                candidatesSeen.insert(key);
                candidates << qMakePair(existingOutput, existingInput);
            }
        }

        if (candidates.isEmpty()) {
            return true;
        }

        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Choose Links To Disconnect"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel(QStringLiteral("These source ports already have other links. Check only the links you want to disconnect."), &dialog);
        label->setWordWrap(true);
        layout->addWidget(label);

        auto *list = new QListWidget(&dialog);
        for (const auto &candidate : candidates) {
            auto *item = new QListWidgetItem(connectionName(candidate.first, candidate.second), list);
            item->setCheckState(Qt::Unchecked);
            item->setData(Qt::UserRole, linkKey(candidate.first, candidate.second));
        }
        layout->addWidget(list);

        auto *buttons = new QDialogButtonBox(&dialog);
        auto *keepButton = buttons->addButton(QStringLiteral("Keep all and connect"), QDialogButtonBox::AcceptRole);
        auto *disconnectButton = buttons->addButton(QStringLiteral("Disconnect checked and connect"), QDialogButtonBox::AcceptRole);
        buttons->addButton(QDialogButtonBox::Cancel);
        int mode = 0;
        connect(keepButton, &QPushButton::clicked, &dialog, [&] {
            mode = 1;
            dialog.accept();
        });
        connect(disconnectButton, &QPushButton::clicked, &dialog, [&] {
            mode = 2;
            dialog.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);

        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }
        if (mode != 2) {
            return true;
        }

        for (int i = 0; i < list->count(); ++i) {
            const QListWidgetItem *item = list->item(i);
            if (item->checkState() != Qt::Checked) {
                continue;
            }
            selected->append(splitLinkKey(item->data(Qt::UserRole).toString()));
        }
        return true;
    }

    bool connectOne(const QString &output, const QString &input) {
        if (m_graph.links.contains(linkKey(output, input))) {
            return false;
        }
        const CommandResult result = runCommand(QStringLiteral("pw-link"), {QStringLiteral("-L"), QStringLiteral("-w"), output, input});
        if (!result.ok) {
            appendLog(QStringLiteral("Connect failed: %1").arg(result.error));
            return false;
        }
        m_graph.links.insert(linkKey(output, input));
        return true;
    }

    bool disconnectOne(const QString &output, const QString &input) {
        const CommandResult result = runCommand(QStringLiteral("pw-link"), {QStringLiteral("-d"), output, input});
        if (!result.ok) {
            appendLog(QStringLiteral("Disconnect failed: %1").arg(result.error));
            return false;
        }
        m_graph.links.remove(linkKey(output, input));
        m_sessionLinks.remove(linkKey(output, input));
        removeAutoReconnectEntry(output, input, true);
        return true;
    }

    void disconnectPortLinks(const QString &alias) {
        int count = 0;
        const QStringList links = QStringList(m_graph.links.begin(), m_graph.links.end());
        for (const QString &key : links) {
            const auto [output, input] = splitLinkKey(key);
            if (output == alias || input == alias) {
                if (disconnectOne(output, input)) {
                    ++count;
                }
            }
        }
        appendLog(QStringLiteral("Disconnected %1 link%2 for %3").arg(count).arg(count == 1 ? QString() : QStringLiteral("s")).arg(alias));
        refreshGraph(false);
    }

    QString displayForNode(const QString &nodeName) const {
        const NodeInfo node = m_graph.nodes.value(nodeName);
        return node.displayName.isEmpty() ? nodeName : node.displayName;
    }

    QJsonObject loadConfigRoot() const {
        const QString path = configPath();
        QFile file(path);
        QJsonObject root;
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error == QJsonParseError::NoError) {
                if (document.isObject()) {
                    root = document.object();
                } else if (document.isArray()) {
                    root.insert(QStringLiteral("rules"), document.array());
                }
            }
            file.close();
        }
        if (!root.contains(QStringLiteral("interval_seconds"))) {
            root.insert(QStringLiteral("interval_seconds"), 2.0);
        }
        if (!root.contains(QStringLiteral("rules"))) {
            root.insert(QStringLiteral("rules"), QJsonArray());
        }
        if (!root.contains(QStringLiteral("connections"))) {
            root.insert(QStringLiteral("connections"), QJsonArray());
        }
        if (!root.contains(QStringLiteral("devices"))) {
            root.insert(QStringLiteral("devices"), QJsonArray());
        }
        return root;
    }

    bool writeConfigRoot(const QJsonObject &root) {
        const QString path = configPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path + QStringLiteral(".tmp"));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            appendLog(QStringLiteral("Could not write %1").arg(path));
            return false;
        }
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        out.close();
        QFile::remove(path);
        if (!QFile::rename(out.fileName(), path)) {
            appendLog(QStringLiteral("Could not replace %1").arg(path));
            return false;
        }
        return true;
    }

    QSet<QString> currentSinkNames() const {
        QSet<QString> sinks;
        const CommandResult result = runCommand(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sinks")});
        if (!result.ok) {
            return sinks;
        }
        for (const QString &line : result.output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList pieces = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (pieces.size() >= 2) {
                sinks.insert(pieces.at(1));
            }
        }
        return sinks;
    }

    bool createFakeAudioDeviceNow(const QString &name, const QString &description, int channels) {
        if (currentSinkNames().contains(name)) {
            appendLog(QStringLiteral("Fake audio device already exists: %1").arg(name));
            return true;
        }

        QStringList args = {
            QStringLiteral("load-module"),
            QStringLiteral("module-null-sink"),
            QStringLiteral("sink_name=%1").arg(name),
            QStringLiteral("sink_properties=device.description=%1").arg(description),
            QStringLiteral("channels=%1").arg(channels),
        };
        if (channels == 2) {
            args << QStringLiteral("channel_map=front-left,front-right");
        }

        const CommandResult result = runCommand(QStringLiteral("pactl"), args);
        if (!result.ok) {
            appendLog(QStringLiteral("Could not create fake audio device: %1").arg(result.error));
            return false;
        }
        appendLog(QStringLiteral("Created fake audio device: %1").arg(name));
        return true;
    }

    bool saveFakeAudioDevice(const QString &name, const QString &description, int channels) {
        QJsonObject root = loadConfigRoot();
        QJsonArray devices = root.value(QStringLiteral("devices")).toArray();
        QJsonArray updated;
        bool replaced = false;

        for (const QJsonValue &value : devices) {
            const QJsonObject existing = value.toObject();
            if (existing.value(QStringLiteral("name")).toString() != name) {
                updated.append(existing);
                continue;
            }
            replaced = true;
            QJsonObject device = existing;
            device.insert(QStringLiteral("name"), name);
            device.insert(QStringLiteral("description"), description);
            device.insert(QStringLiteral("channels"), channels);
            device.insert(QStringLiteral("enabled"), true);
            updated.append(device);
        }

        if (!replaced) {
            QJsonObject device;
            device.insert(QStringLiteral("name"), name);
            device.insert(QStringLiteral("description"), description);
            device.insert(QStringLiteral("channels"), channels);
            device.insert(QStringLiteral("enabled"), true);
            updated.append(device);
        }

        root.insert(QStringLiteral("devices"), updated);
        if (!writeConfigRoot(root)) {
            return false;
        }
        appendLog(QStringLiteral("Saved fake audio device: %1").arg(name));
        return true;
    }

    void removeFakeAudioDeviceEntry(const QString &name) {
        QJsonObject root = loadConfigRoot();
        const QJsonArray devices = root.value(QStringLiteral("devices")).toArray();
        QJsonArray updated;
        for (const QJsonValue &value : devices) {
            const QJsonObject existing = value.toObject();
            if (existing.value(QStringLiteral("name")).toString() == name) {
                continue;
            }
            updated.append(existing);
        }
        root.insert(QStringLiteral("devices"), updated);
        if (writeConfigRoot(root)) {
            appendLog(QStringLiteral("Removed saved fake audio device: %1").arg(name));
        }
    }

    void showCreateFakeAudioDialog() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Create Fake Audio"));
        auto *layout = new QFormLayout(&dialog);

        auto *nameEdit = new QLineEdit(&dialog);
        nameEdit->setPlaceholderText(QStringLiteral("obs-game"));
        auto *descriptionEdit = new QLineEdit(&dialog);
        descriptionEdit->setPlaceholderText(QStringLiteral("OBS Game Audio"));
        auto *channels = new QSpinBox(&dialog);
        channels->setRange(1, 8);
        channels->setValue(2);
        auto *saveDevice = new QCheckBox(QStringLiteral("Save and recreate automatically"), &dialog);
        saveDevice->setChecked(true);

        layout->addRow(QStringLiteral("Name"), nameEdit);
        layout->addRow(QStringLiteral("Description"), descriptionEdit);
        layout->addRow(QStringLiteral("Channels"), channels);
        layout->addRow(saveDevice);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        const QString name = nameEdit->text().trimmed();
        const QString description = descriptionEdit->text().trimmed().isEmpty() ? name : descriptionEdit->text().trimmed();
        if (!isValidDeviceName(name)) {
            QMessageBox::warning(this, QStringLiteral("Invalid Name"), QStringLiteral("Use letters, numbers, dot, dash, or underscore only."));
            return;
        }
        if (!createFakeAudioDeviceNow(name, description, channels->value())) {
            return;
        }
        if (saveDevice->isChecked()) {
            saveFakeAudioDevice(name, description, channels->value());
        }
        refreshGraph(true);
    }

    void showSavedDialog() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Auto-connect Status"));
        dialog.resize(900, 500);
        auto *layout = new QVBoxLayout(&dialog);
        auto *summary = new QLabel(&dialog);
        summary->setWordWrap(true);
        layout->addWidget(summary);
        auto *tabs = new QTabWidget(&dialog);
        auto *connectionsList = new QListWidget(&dialog);
        auto *devicesList = new QListWidget(&dialog);

        auto populate = [&] {
            connectionsList->clear();
            devicesList->clear();
            const QJsonObject root = loadConfigRoot();
            int enabledConnections = 0;
            int connectedConnections = 0;
            for (const QJsonValue &value : root.value(QStringLiteral("connections")).toArray()) {
                const QJsonObject connection = value.toObject();
                const QString output = connection.value(QStringLiteral("output")).toString();
                const QString input = connection.value(QStringLiteral("input")).toString();
                const bool enabled = connection.value(QStringLiteral("enabled")).toBool(true);
                const QString key = linkKey(output, input);
                QString state;
                QString detail;
                QColor color;
                if (!enabled) {
                    state = QStringLiteral("Disabled");
                    detail = QStringLiteral("This saved connection is disabled");
                    color = QColor(QStringLiteral("#777777"));
                } else if (m_graph.links.contains(key)) {
                    state = QStringLiteral("Connected");
                    detail = QStringLiteral("This auto-connect link is active now");
                    color = QColor(QStringLiteral("#238636"));
                    ++enabledConnections;
                    ++connectedConnections;
                } else {
                    ++enabledConnections;
                    const bool outputPresent = m_graph.outputs.contains(output);
                    const bool inputPresent = m_graph.inputs.contains(input);
                    if (!outputPresent && !inputPresent) {
                        state = QStringLiteral("Waiting - both devices are offline");
                    } else if (!outputPresent) {
                        state = QStringLiteral("Waiting - source is offline");
                    } else if (!inputPresent) {
                        state = QStringLiteral("Waiting - destination is offline");
                    } else {
                        state = QStringLiteral("Disconnected - ready to reconnect");
                    }
                    detail = QStringLiteral("The rule is saved and will connect when both ports are available");
                    color = QColor(QStringLiteral("#b26a00"));
                }
                auto *item = new QListWidgetItem(QStringLiteral("%1  |  %2").arg(state, connectionName(output, input)), connectionsList);
                item->setData(Qt::UserRole, linkKey(output, input));
                item->setForeground(color);
                item->setToolTip(detail);
            }
            const QSet<QString> sinks = currentSinkNames();
            int runningDevices = 0;
            int enabledDevices = 0;
            for (const QJsonValue &value : root.value(QStringLiteral("devices")).toArray()) {
                const QJsonObject device = value.toObject();
                const QString name = device.value(QStringLiteral("name")).toString();
                const QString description = device.value(QStringLiteral("description")).toString();
                const int channels = device.value(QStringLiteral("channels")).toInt(2);
                const bool enabled = device.value(QStringLiteral("enabled")).toBool(true);
                const bool running = sinks.contains(name);
                const QString state = !enabled ? QStringLiteral("Disabled")
                                               : running ? QStringLiteral("Running") : QStringLiteral("Waiting to be created");
                auto *item = new QListWidgetItem(QStringLiteral("%1  |  %2 - %3 (%4ch)").arg(state, name, description).arg(channels), devicesList);
                item->setData(Qt::UserRole, name);
                item->setForeground(QColor(!enabled ? QStringLiteral("#777777")
                                                   : running ? QStringLiteral("#238636") : QStringLiteral("#b26a00")));
                if (enabled) {
                    ++enabledDevices;
                    if (running) {
                        ++runningDevices;
                    }
                }
            }
            summary->setText(QStringLiteral("%1 of %2 saved auto-connect links are connected now. %3 of %4 saved fake audio devices are running.")
                                 .arg(connectedConnections)
                                 .arg(enabledConnections)
                                 .arg(runningDevices)
                                 .arg(enabledDevices));
            tabs->setTabText(0, QStringLiteral("Auto-connect Links (%1/%2 connected)").arg(connectedConnections).arg(enabledConnections));
            tabs->setTabText(1, QStringLiteral("Fake Audio Devices (%1/%2 running)").arg(runningDevices).arg(enabledDevices));
        };

        tabs->addTab(connectionsList, QStringLiteral("Auto-connect Links"));
        tabs->addTab(devicesList, QStringLiteral("Fake Audio Devices"));
        layout->addWidget(tabs);
        populate();

        auto *buttons = new QDialogButtonBox(&dialog);
        auto *refreshStatusButton = buttons->addButton(QStringLiteral("Refresh Status"), QDialogButtonBox::ActionRole);
        auto *removeButton = buttons->addButton(QStringLiteral("Remove Selected"), QDialogButtonBox::ActionRole);
        buttons->addButton(QDialogButtonBox::Close);
        layout->addWidget(buttons);

        connect(refreshStatusButton, &QPushButton::clicked, &dialog, [&] {
            const SnapshotResult snapshot = loadSnapshot();
            if (!snapshot.ok) {
                QMessageBox::warning(&dialog, QStringLiteral("PipeWire Error"), snapshot.error);
                return;
            }
            m_graph = snapshot.graph;
            loadAutoConnections();
            rebuildScene();
            populate();
        });
        connect(removeButton, &QPushButton::clicked, &dialog, [&] {
            if (tabs->currentWidget() == connectionsList) {
                QListWidgetItem *item = connectionsList->currentItem();
                if (!item) {
                    return;
                }
                const auto [output, input] = splitLinkKey(item->data(Qt::UserRole).toString());
                removeAutoReconnectEntry(output, input, true);
            } else {
                QListWidgetItem *item = devicesList->currentItem();
                if (!item) {
                    return;
                }
                removeFakeAudioDeviceEntry(item->data(Qt::UserRole).toString());
            }
            loadAutoConnections();
            rebuildScene();
            populate();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    }

    void loadAutoConnections() {
        m_autoConnections.clear();
        const QJsonObject root = loadConfigRoot();
        const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
        for (const QJsonValue &value : connections) {
            const QJsonObject connection = value.toObject();
            if (!connection.value(QStringLiteral("enabled")).toBool(true)) {
                continue;
            }
            const QString output = connection.value(QStringLiteral("output")).toString();
            const QString input = connection.value(QStringLiteral("input")).toString();
            if (!output.isEmpty() && !input.isEmpty() && isSupportedAutoConnection(output, input)) {
                m_autoConnections.insert(linkKey(output, input));
            }
        }
    }

    QString connectionName(const QString &output, const QString &input) const {
        return QStringLiteral("%1 -> %2").arg(output, input);
    }

    void removeAutoReconnectEntry(const QString &output, const QString &input, bool announce) {
        const QString key = linkKey(output, input);
        if (!m_autoConnections.contains(key)) {
            return;
        }

        QJsonObject root = loadConfigRoot();
        const QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
        QJsonArray updated;
        for (const QJsonValue &value : connections) {
            const QJsonObject existing = value.toObject();
            if (existing.value(QStringLiteral("output")).toString() == output
                && existing.value(QStringLiteral("input")).toString() == input) {
                continue;
            }
            updated.append(existing);
        }

        root.insert(QStringLiteral("connections"), updated);
        if (!writeConfigRoot(root)) {
            return;
        }
        m_autoConnections.remove(key);
        if (announce) {
            appendLog(QStringLiteral("Removed auto-reconnect: %1").arg(connectionName(output, input)));
        }
    }

    void setAutoReconnect(const QString &output, const QString &input, bool enabled) {
        if (enabled && !isSupportedAutoConnection(output, input)) {
            appendLog(QStringLiteral("Auto-reconnect skipped for unsupported link: %1").arg(connectionName(output, input)));
            return;
        }
        if (!enabled) {
            removeAutoReconnectEntry(output, input, true);
            rebuildScene();
            return;
        }

        QJsonObject root = loadConfigRoot();
        QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
        QJsonArray updated;
        bool replaced = false;

        for (const QJsonValue &value : connections) {
            const QJsonObject existing = value.toObject();
            const bool sameConnection = existing.value(QStringLiteral("output")).toString() == output
                && existing.value(QStringLiteral("input")).toString() == input;
            if (!sameConnection) {
                updated.append(existing);
                continue;
            }
            replaced = true;
            QJsonObject connection = existing;
            connection.insert(QStringLiteral("name"), connectionName(output, input));
            connection.insert(QStringLiteral("output"), output);
            connection.insert(QStringLiteral("input"), input);
            connection.insert(QStringLiteral("enabled"), true);
            connection.insert(QStringLiteral("disconnect_others"), false);
            connection.insert(QStringLiteral("linger"), true);
            updated.append(connection);
        }

        if (!replaced) {
            QJsonObject connection;
            connection.insert(QStringLiteral("name"), connectionName(output, input));
            connection.insert(QStringLiteral("output"), output);
            connection.insert(QStringLiteral("input"), input);
            connection.insert(QStringLiteral("enabled"), true);
            connection.insert(QStringLiteral("disconnect_others"), false);
            connection.insert(QStringLiteral("linger"), true);
            updated.append(connection);
        }

        root.insert(QStringLiteral("connections"), updated);
        if (!writeConfigRoot(root)) {
            return;
        }

        const QString key = linkKey(output, input);
        m_autoConnections.insert(key);
        appendLog(QStringLiteral("Enabled auto-reconnect: %1").arg(connectionName(output, input)));
        rebuildScene();
    }

    void saveAutoReconnectForPairs(const QList<QPair<QString, QString>> &pairs) {
        for (const auto &pair : pairs) {
            setAutoReconnect(pair.first, pair.second, true);
        }
    }

    void applySavedConnections(bool quiet) {
        const QString script = QDir(appSourceDir()).filePath(QStringLiteral("pipewire_auto_router.py"));
        const CommandResult result = runCommand(QStringLiteral("python3"), {script, QStringLiteral("--apply")}, 12000);
        const QString text = (result.output + QLatin1Char('\n') + result.error).trimmed();
        if (!quiet) {
            appendLog(text.isEmpty() ? QStringLiteral("Saved auto-reconnect connections applied") : text);
        } else if (text.contains(QStringLiteral(": connect ")) || text.contains(QStringLiteral(": disconnect "))) {
            appendLog(text);
        }
        if (!result.ok) {
            statusBar()->showMessage(QStringLiteral("Applying auto-reconnect failed"));
        }
        refreshGraph(false);
    }

    void appendLog(const QString &message) {
        if (message.trimmed().isEmpty()) {
            return;
        }
        const QStringList lines = message.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line.trimmed()));
        }
    }

    PatchbayView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QCheckBox *m_audioOnly = nullptr;
    QCheckBox *m_pairChannels = nullptr;
    QCheckBox *m_autoReconnectNewLinks = nullptr;
    QCheckBox *m_autoApplyReconnect = nullptr;
    QCheckBox *m_liveRefresh = nullptr;
    QTimer m_refreshTimer;
    Graph m_graph;
    QHash<QString, NodeItem *> m_items;
    QHash<QString, QPointF> m_anchors;
    QHash<QString, QGraphicsPathItem *> m_linkItems;
    QHash<QString, QPointF> m_nodePositions;
    QSet<QString> m_autoConnections;
    QSet<QString> m_sessionLinks;
    QString m_dragPort;
    bool m_dragFromOutput = true;
    QGraphicsPathItem *m_preview = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    bool checkOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--check")) {
            checkOnly = true;
        }
    }

    if (checkOnly) {
        QCoreApplication app(argc, argv);
        const SnapshotResult snapshot = loadSnapshot();
        if (!snapshot.ok) {
            QTextStream(stderr) << snapshot.error << Qt::endl;
            return 1;
        }
        QTextStream(stdout) << snapshot.graph.nodes.size() << " nodes, "
                            << snapshot.graph.outputs.size() << " outputs, "
                            << snapshot.graph.inputs.size() << " inputs, "
                            << snapshot.graph.links.size() << " links" << Qt::endl;
        return 0;
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PipeWire Auto Router"));
    QApplication::setOrganizationName(QStringLiteral("pipewire-auto-router"));
    QGuiApplication::setDesktopFileName(QStringLiteral("pipewire-auto-router"));
#ifdef APP_VERSION
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
#endif

    MainWindow window;
    window.show();
    return app.exec();
}
