// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tileserver.h"

#include "network.h"
#include "tilestore.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>

#include <memory>

using namespace Qt::StringLiterals;

// Enable with QT_LOGGING_RULES="karten.tileserver.debug=true".
Q_LOGGING_CATEGORY(Log, "karten.tileserver", QtWarningMsg)

namespace
{
TileServer *s_instance = nullptr;
const QString DefaultSource = u"https://tiles.openfreemap.org/styles/liberty"_s;
const QString DarkSource = u"https://tiles.openfreemap.org/styles/dark"_s;
const QString Automatic = u"auto"_s;
constexpr qint64 Week = 7 * 86400;
constexpr int MaxZoom = 22;
constexpr char Busy[] = "kartenBusy";
constexpr char Path[] = "kartenPath";

/// When something freshly fetched should be checked again, following the
/// server's caching headers and falling back to a week.
qint64 expiryOf(QNetworkReply *reply)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    static const QRegularExpression maxAge(u"max-age=(\\d+)"_s);
    const auto match = maxAge.match(QString::fromLatin1(reply->rawHeader("Cache-Control")));
    if (match.hasMatch()) {
        return now + std::max<qint64>(match.captured(1).toLongLong(), 3600);
    }
    const QDateTime expires = QDateTime::fromString(QString::fromLatin1(reply->rawHeader("Expires")), Qt::RFC2822Date);
    if (expires.isValid() && expires.toSecsSinceEpoch() > now) {
        return expires.toSecsSinceEpoch();
    }
    return now + Week;
}

bool isTileTemplate(const QString &text)
{
    return text.contains("{z}"_L1) && text.contains("{x}"_L1) && text.contains("{y}"_L1);
}

bool isAllowedUrl(const QUrl &url)
{
    const bool local = url.host() == "localhost"_L1 || url.host() == "127.0.0.1"_L1;
    return url.isValid() && !url.host().isEmpty() && (url.scheme() == "https"_L1 || (local && url.scheme() == "http"_L1));
}

/// Resolves an address from a style, keeping {placeholders} readable.
QString resolveAddress(const QUrl &base, const QString &address)
{
    QString result = QUrl(address).isRelative() ? base.resolved(QUrl(address)).toString() : address;
    return result.replace("%7B"_L1, "{"_L1, Qt::CaseInsensitive).replace("%7D"_L1, "}"_L1, Qt::CaseInsensitive);
}

bool hostIs(const QString &host, QLatin1StringView domain)
{
    return host == domain || host.endsWith(u'.' + domain);
}

/// What a map source permits for offline use.
struct Policy {
    int maxTiles;
    qint64 maxBytes;
};

Policy policyFor(const QString &host)
{
    // The OpenStreetMap Foundation's tile servers do not permit downloading
    // tiles ahead of time: https://operations.osmfoundation.org/policies/tiles/
    if (hostIs(host, "tile.openstreetmap.org"_L1) || hostIs(host, "tile.osm.org"_L1)) {
        return {0, 0};
    }
    // OpenFreeMap permits it "as long as each download is at most 3,000 tiles
    // or 50 MB": https://openfreemap.org/tos/
    if (hostIs(host, "openfreemap.org"_L1)) {
        return {3000, 50'000'000};
    }
    // Anything else was entered by the user, who is told to check its terms.
    return {30000, 1'000'000'000};
}

/// The fonts a style layer asks for, when it names them outright.
void collectFontStacks(const QJsonValue &value, QStringList &stacks)
{
    if (!value.isArray()) {
        return;
    }
    const QJsonArray array = value.toArray();
    QStringList fonts;
    for (const QJsonValue &entry : array) {
        if (entry.isString()) {
            fonts.append(entry.toString());
        } else {
            collectFontStacks(entry, stacks); // an expression that may contain font lists
        }
    }
    // An expression such as ["literal", [...]] starts with a string as well.
    if (!fonts.isEmpty() && fonts.size() == array.size()) {
        const QString stack = fonts.join(u',');
        if (!stacks.contains(stack)) {
            stacks.append(stack);
        }
    }
}
}

TileServer::TileServer(TileStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_token(QUuid::createUuid().toString(QUuid::Id128))
{
    s_instance = this;
    const KConfigGroup config = KSharedConfig::openConfig()->group(u"Map"_s);
    // "TileUrl" is where versions before 0.2 kept the address of image tiles.
    m_choice = config.readEntry("Source", config.readEntry("TileUrl", DefaultSource));
    if (!isValidSource(m_choice)) {
        m_choice = DefaultSource;
    }
    m_source = resolve(m_choice);
    m_styleId = m_store->sourceId(m_source);
    // To notice when the application's colours change.
    QCoreApplication::instance()->installEventFilter(this);
}

TileServer *TileServer::create(QQmlEngine *, QJSEngine *)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

TileServer *TileServer::instance()
{
    return s_instance;
}

bool TileServer::listen()
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = m_server->nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                readRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    return m_server->listen(QHostAddress::LocalHost, 0);
}

QString TileServer::localBase() const
{
    // The random token keeps other local programs from using this as a proxy.
    return u"http://127.0.0.1:%1/%2/"_s.arg(m_server ? m_server->serverPort() : 0).arg(m_token);
}

QString TileServer::styleUrl() const
{
    // The number makes the map view see a new style after every change.
    return localBase() + u"style/%1.json"_s.arg(m_generation);
}

QString TileServer::source() const
{
    return m_choice;
}

QString TileServer::resolve(const QString &choice) const
{
    if (choice != Automatic) {
        return choice;
    }
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    return dark ? DarkSource : DefaultSource;
}

bool TileServer::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange && watched == QCoreApplication::instance() && m_choice == Automatic) {
        useSource(resolve(m_choice));
    }
    return QObject::eventFilter(watched, event);
}

QVariantList TileServer::presets() const
{
    const auto preset = [](const QString &name, const QString &style) {
        return QVariantMap{{u"name"_s, name}, {u"url"_s, QString(u"https://tiles.openfreemap.org/styles/"_s + style)}};
    };
    return {
        QVariantMap{{u"name"_s, i18nc("@item:inlistbox map style that is light or dark like the application", "Automatic")}, {u"url"_s, Automatic}},
        preset(i18nc("@item:inlistbox map style", "Standard"), u"liberty"_s),
        preset(i18nc("@item:inlistbox map style", "Bright"), u"bright"_s),
        preset(i18nc("@item:inlistbox map style", "Light"), u"positron"_s),
        preset(i18nc("@item:inlistbox map style", "Dark"), u"dark"_s),
    };
}

bool TileServer::isValidSource(const QString &source) const
{
    return source.trimmed() == Automatic || isAllowedUrl(QUrl(source.trimmed()));
}

void TileServer::setSource(const QString &source)
{
    const QString value = source.trimmed().isEmpty() ? DefaultSource : source.trimmed();
    if (value == m_choice || !isValidSource(value)) {
        return;
    }
    m_choice = value;
    KConfigGroup config = KSharedConfig::openConfig()->group(u"Map"_s);
    if (value == DefaultSource) {
        config.deleteEntry("Source");
    } else {
        config.writeEntry("Source", value);
    }
    config.sync();
    if (resolve(m_choice) == m_source) {
        // The choice changed, though the map stays.
        m_counterpart = {};
        loadCounterpart();
        Q_EMIT sourceChanged();
        Q_EMIT styleChanged();
    } else {
        useSource(resolve(m_choice));
    }
}

void TileServer::useSource(const QString &address)
{
    if (address == m_source) {
        return;
    }
    m_source = address;
    m_styleId = m_store->sourceId(m_source);
    ++m_generation;
    m_ready = false;
    m_style = {};
    m_counterpart = {};
    Q_EMIT sourceChanged();
    Q_EMIT styleChanged();
}

int TileServer::styleId() const
{
    return m_styleId;
}

QList<TileServer::TileSource> TileServer::tileSources() const
{
    return m_style.tileSources;
}

QList<TileServer::TileSource> TileServer::packSources() const
{
    QList<TileSource> sources = m_style.tileSources;
    for (const TileSource &other : m_counterpart.tileSources) {
        const bool known = std::any_of(sources.cbegin(), sources.cend(), [&other](const TileSource &source) {
            return source.id == other.id;
        });
        if (!known) {
            sources.append(other);
        }
    }
    return sources;
}

bool TileServer::offlineAllowed() const
{
    return maxPackTiles() > 0;
}

int TileServer::maxPackTiles() const
{
    // The strictest of the servers involved decides.
    int limit = m_style.tileSources.isEmpty() ? 0 : std::numeric_limits<int>::max();
    for (const TileSource &source : m_style.tileSources) {
        limit = std::min(limit, policyFor(QUrl(source.urlTemplate).host()).maxTiles);
    }
    return limit;
}

qint64 TileServer::maxPackBytes() const
{
    qint64 limit = m_style.tileSources.isEmpty() ? 0 : std::numeric_limits<qint64>::max();
    for (const TileSource &source : m_style.tileSources) {
        limit = std::min(limit, policyFor(QUrl(source.urlTemplate).host()).maxBytes);
    }
    return limit;
}

bool TileServer::isVector() const
{
    return std::any_of(m_style.tileSources.begin(), m_style.tileSources.end(), [](const TileSource &source) {
        return source.vector;
    });
}

QString TileServer::attribution() const
{
    return m_style.attribution;
}

// --- Loading the style ---

void TileServer::loadStyle(const std::function<void(bool)> &done)
{
    if (m_ready) {
        done(true);
        return;
    }
    m_waiting.append(done);
    if (m_loading) {
        return;
    }
    m_loading = true;

    const int generation = m_generation;
    fetchStyle(m_source, [this, generation](bool ok, const Style &style) {
        m_loading = false;
        if (generation != m_generation) {
            // The map source changed meanwhile; start over for those waiting.
            const auto waiting = std::exchange(m_waiting, {});
            for (const auto &callback : waiting) {
                loadStyle(callback);
            }
            return;
        }
        m_ready = ok;
        if (ok) {
            m_style = style;
            Q_EMIT styleChanged();
            loadCounterpart();
        }
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &callback : waiting) {
            callback(ok);
        }
    });
}

void TileServer::loadCounterpart()
{
    if (m_choice != Automatic || !m_ready) {
        return;
    }
    const int generation = m_generation;
    fetchStyle(m_source == DarkSource ? DefaultSource : DarkSource, [this, generation](bool ok, const Style &style) {
        if (ok && generation == m_generation && m_choice == Automatic) {
            m_counterpart = style;
            Q_EMIT styleChanged();
        }
    });
}

void TileServer::fetchStyle(const QString &address, const std::function<void(bool, const Style &)> &done)
{
    if (isTileTemplate(address)) {
        // Not from within the caller, which may not expect that.
        QMetaObject::invokeMethod(
            this,
            [this, address, done] {
                done(true, buildRasterStyle(address));
            },
            Qt::QueuedConnection);
        return;
    }

    const QUrl styleUrl(address);
    // The style and what it refers to are worth waiting for: they say where
    // the current tiles are.
    fetchResource(styleUrl, PreferCurrent, [=, this](const QByteArray &data, Status) {
        const QJsonObject style = QJsonDocument::fromJson(data).object();
        if (!style.contains("layers"_L1)) {
            done(false, {});
            return;
        }
        // Sources may leave their details to a separate TileJSON document.
        struct Progress {
            QHash<QString, QJsonObject> tileJson;
            QHash<QString, QString> addresses;
            int open = 0;
            bool failed = false;
        };
        const auto progress = std::make_shared<Progress>();
        const QJsonObject sources = style["sources"_L1].toObject();
        QList<std::pair<QString, QUrl>> wanted;
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            const QString url = it.value()["url"_L1].toString();
            if (!url.isEmpty() && isAllowedUrl(styleUrl.resolved(QUrl(url)))) {
                wanted.append({it.key(), styleUrl.resolved(QUrl(url))});
            }
        }
        if (wanted.isEmpty()) {
            done(true, buildStyle(address, style, {}, {}));
            return;
        }
        progress->open = int(wanted.size());
        for (const auto &[name, url] : std::as_const(wanted)) {
            fetchResource(url, PreferCurrent, [=, this](const QByteArray &json, Status) {
                const QJsonObject object = QJsonDocument::fromJson(json).object();
                if (object["tiles"_L1].toArray().isEmpty()) {
                    progress->failed = true;
                } else {
                    QJsonObject resolved = object;
                    QJsonArray tiles;
                    for (const QJsonValue &tile : object["tiles"_L1].toArray()) {
                        tiles.append(resolveAddress(url, tile.toString()));
                    }
                    resolved["tiles"_L1] = tiles;
                    progress->tileJson.insert(name, resolved);
                    progress->addresses.insert(name, url.toString());
                }
                if (--progress->open == 0) {
                    if (progress->failed) {
                        done(false, {});
                    } else {
                        done(true, buildStyle(address, style, progress->tileJson, progress->addresses));
                    }
                }
            });
        }
    });
}

TileServer::Style TileServer::buildRasterStyle(const QString &address)
{
    TileSource tiles;
    tiles.id = m_store->sourceId(address);
    tiles.urlTemplate = address;
    tiles.maxZoom = 19;

    const QJsonObject source{
        {u"type"_s, u"raster"_s},
        {u"tiles"_s, QJsonArray{QString(localBase() + u"tiles/%1/{z}/{x}/{y}"_s.arg(tiles.id))}},
        {u"tileSize"_s, 256},
        {u"maxzoom"_s, tiles.maxZoom},
    };
    const QJsonObject style{
        {u"version"_s, 8},
        {u"sources"_s, QJsonObject{{u"tiles"_s, source}}},
        {u"layers"_s, QJsonArray{QJsonObject{{u"id"_s, u"tiles"_s}, {u"type"_s, u"raster"_s}, {u"source"_s, u"tiles"_s}}}},
    };
    Style result;
    result.tileSources = {tiles};
    result.json = QJsonDocument(style).toJson(QJsonDocument::Compact);
    return result;
}

TileServer::Style TileServer::buildStyle(const QString &address,
                                         const QJsonObject &original,
                                         const QHash<QString, QJsonObject> &tileJson,
                                         const QHash<QString, QString> &tileJsonAddresses)
{
    Style result;
    const QUrl styleUrl(address);
    QJsonObject style = original;
    QStringList credits;

    QJsonObject sources = original["sources"_L1].toObject();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        QJsonObject source = it.value().toObject();
        // value() throughout: reading with [] would add the key as null.
        const QString type = source.value("type"_L1).toString();
        if (type != "vector"_L1 && type != "raster"_L1 && type != "raster-dem"_L1) {
            continue;
        }
        // What the TileJSON says applies unless the style overrides it.
        const QJsonObject details = tileJson.value(it.key());
        for (const QLatin1StringView key : {"tiles"_L1, "minzoom"_L1, "maxzoom"_L1, "bounds"_L1, "scheme"_L1, "attribution"_L1}) {
            if (!source.contains(key) && details.contains(key)) {
                source[key] = details[key];
            }
        }
        source.remove("url"_L1);
        const QString tiles = resolveAddress(styleUrl, source.value("tiles"_L1).toArray().at(0).toString());
        if (!isTileTemplate(tiles) || !isAllowedUrl(QUrl(tiles))) {
            continue;
        }

        TileSource entry;
        // Not keyed by the tile address: that changes with every update of
        // the map data, and stored tiles should outlive it. Styles that draw
        // from the same TileJSON document share their tiles, which is what
        // lets a light and a dark map use one download.
        const QString ownName = address + u'#' + it.key();
        const QString shared = tileJsonAddresses.value(it.key());
        entry.id = shared.isEmpty() ? m_store->sourceId(ownName) : m_store->sourceId(shared, ownName);
        entry.urlTemplate = tiles;
        entry.minZoom = std::clamp(source.value("minzoom"_L1).toInt(0), 0, MaxZoom);
        entry.maxZoom = std::clamp(source.value("maxzoom"_L1).toInt(MaxZoom), entry.minZoom, MaxZoom);
        entry.vector = type == "vector"_L1;
        entry.tms = source.value("scheme"_L1).toString() == "tms"_L1;
        result.tileSources.append(entry);

        source["tiles"_L1] = QJsonArray{QString(localBase() + u"tiles/%1/{z}/{x}/{y}"_s.arg(entry.id))};
        it.value() = source;
        const QString credit = source.value("attribution"_L1).toString().trimmed();
        if (!credit.isEmpty() && !credits.contains(credit)) {
            credits.append(credit);
        }
    }
    style["sources"_L1] = sources;

    if (style.contains("glyphs"_L1)) {
        result.glyphs = resolveAddress(styleUrl, original["glyphs"_L1].toString());
        style["glyphs"_L1] = QString(localBase() + u"fonts/{fontstack}/{range}.pbf"_s);
    }

    // Either one address or a list of {id, url}.
    const QJsonValue sprite = original["sprite"_L1];
    if (sprite.isString()) {
        result.sprites.append(resolveAddress(styleUrl, sprite.toString()));
        style["sprite"_L1] = QString(localBase() + u"sprites/0/sprite"_s);
    } else if (sprite.isArray()) {
        QJsonArray rewritten;
        for (const QJsonValue &value : sprite.toArray()) {
            QJsonObject entry = value.toObject();
            entry["url"_L1] = QString(localBase() + u"sprites/%1/sprite"_s.arg(result.sprites.size()));
            result.sprites.append(resolveAddress(styleUrl, value["url"_L1].toString()));
            rewritten.append(entry);
        }
        style["sprite"_L1] = rewritten;
    }

    const QJsonArray layers = original["layers"_L1].toArray();
    for (const QJsonValue &layer : layers) {
        collectFontStacks(layer["layout"_L1]["text-font"_L1], result.fontStacks);
    }

    result.attribution = credits.join(u" · "_s);
    result.json = QJsonDocument(style).toJson(QJsonDocument::Compact);
    return result;
}

// --- Fetching ---

void TileServer::fetchTile(int source, int z, int x, int y, const Callback &callback)
{
    const QList<TileSource> sources = packSources();
    const auto entry = std::find_if(sources.cbegin(), sources.cend(), [source](const TileSource &tiles) {
        return tiles.id == source;
    });
    if (entry == sources.cend() || z < 0 || z > MaxZoom || x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z)) {
        callback({}, Missing);
        return;
    }
    const TileStore::Tile cached = m_store->tile(source, z, x, y);
    if (cached.found && cached.expires > QDateTime::currentSecsSinceEpoch()) {
        callback(cached.data, Ok);
        return;
    }

    // An out-of-date tile is shown right away and replaced behind the scenes:
    // waiting for the network first would hold up the map when there is none.
    const QString key = u"%1/%2/%3/%4"_s.arg(source).arg(z).arg(x).arg(y);
    const bool inFlight = m_pending.contains(key);
    if (cached.found) {
        callback(cached.data, Ok);
        m_pending[key]; // nobody waits, but the request is under way
    } else {
        m_pending[key].append(callback);
    }
    if (inFlight) {
        return;
    }

    QString url = entry->urlTemplate;
    url.replace("{z}"_L1, QString::number(z)).replace("{x}"_L1, QString::number(x)).replace("{y}"_L1, QString::number(y));
    QNetworkRequest request = Network::request(QUrl(url));
    // Asking for compression by hand makes Qt leave the data as it is, which
    // is how it is stored and passed on: smaller, and its size is what the
    // download limits of the map sources are about.
    request.setRawHeader("Accept-Encoding", "gzip");
    if (cached.found && !cached.etag.isEmpty()) {
        request.setRawHeader("If-None-Match", cached.etag);
    }
    QNetworkReply *reply = Network::manager()->get(request);
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray result;
        Status outcome = Unavailable;
        if (status == 304 && cached.found) {
            m_store->setExpiry(source, z, x, y, expiryOf(reply));
            result = cached.data;
            outcome = Ok;
        } else if (reply->error() == QNetworkReply::NoError && status == 200) {
            result = reply->readAll();
            outcome = result.isEmpty() ? Missing : Ok;
            if (!result.isEmpty()) {
                m_store->putTile(source, z, x, y, result, reply->rawHeader("ETag"), expiryOf(reply));
            }
        } else if (status == 204 || status == 404) {
            outcome = Missing; // nothing there, such as open sea
        }
        const QList<Callback> callbacks = m_pending.take(key);
        for (const Callback &pending : callbacks) {
            pending(result, outcome);
        }
    });
}

QByteArray TileServer::storedTile(int source, int z, int x, int y) const
{
    return m_store->tile(source, z, x, y).data;
}

void TileServer::fetchResource(const QUrl &url, Freshness freshness, const Callback &callback)
{
    const QString key = url.toString();
    const TileStore::Resource cached = m_store->resource(key);
    if (cached.found && cached.expires > QDateTime::currentSecsSinceEpoch()) {
        callback(cached.data, Ok);
        return;
    }
    const bool inFlight = m_pending.contains(key);
    if (cached.found && freshness == StaleIsFine) {
        callback(cached.data, Ok);
        m_pending[key]; // nobody waits, but the request is under way
    } else {
        m_pending[key].append(callback);
    }
    if (inFlight) {
        return;
    }

    QNetworkRequest request = Network::request(url);
    if (cached.found) {
        // There is something to fall back on, so do not wait for long.
        request.setTransferTimeout(5000);
    }
    if (cached.found && !cached.etag.isEmpty()) {
        request.setRawHeader("If-None-Match", cached.etag);
    }
    QNetworkReply *reply = Network::manager()->get(request);
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray result;
        Status outcome = Unavailable;
        if (status == 304 && cached.found) {
            m_store->setResourceExpiry(key, expiryOf(reply));
            result = cached.data;
            outcome = Ok;
        } else if (reply->error() == QNetworkReply::NoError && status == 200) {
            result = reply->readAll();
            outcome = result.isEmpty() ? Missing : Ok;
            if (!result.isEmpty()) {
                m_store->putResource(key, result, reply->rawHeader("ETag"), expiryOf(reply));
            }
        } else if (status == 204 || status == 404) {
            outcome = Missing;
        } else if (cached.found) {
            result = cached.data;
            outcome = Ok;
        }
        const QList<Callback> callbacks = m_pending.take(key);
        for (const Callback &pending : callbacks) {
            pending(result, outcome);
        }
    });
}

void TileServer::fetchGlyphs(const Style &style, const QString &fontStack, const QString &range, const Callback &callback)
{
    static const QRegularExpression validRange(u"^\\d{1,6}-\\d{1,6}$"_s);
    if (style.glyphs.isEmpty() || fontStack.isEmpty() || !validRange.match(range).hasMatch()) {
        callback({}, Missing);
        return;
    }
    QString url = style.glyphs;
    url.replace("{fontstack}"_L1, QString::fromLatin1(QUrl::toPercentEncoding(fontStack))).replace("{range}"_L1, range);
    fetchResource(QUrl(url), StaleIsFine, callback);
}

void TileServer::fetchSprite(const Style &style, int index, const QString &suffix, const Callback &callback)
{
    static const QRegularExpression validSuffix(u"^(@[23]x)?\\.(json|png)$"_s);
    if (index < 0 || index >= style.sprites.size() || !validSuffix.match(suffix).hasMatch()) {
        callback({}, Missing);
        return;
    }
    fetchResource(QUrl(style.sprites[index] + suffix), StaleIsFine, callback);
}

void TileServer::fetchStyleResources(const std::function<void(bool)> &done)
{
    struct Progress {
        int open = 1; // held until everything has been requested
        bool failed = false;
    };
    const auto progress = std::make_shared<Progress>();
    const Callback count = [progress, done](const QByteArray &, Status status) {
        // A font without, say, Greek letters is complete all the same.
        progress->failed |= status == Unavailable;
        if (--progress->open == 0) {
            done(!progress->failed);
        }
    };
    // Latin, Greek and Cyrillic, and general punctuation. Other scripts are
    // kept once they have been looked at.
    static const int ranges[] = {0, 256, 512, 768, 1024, 1280, 8192};
    // The counterpart is empty unless the choice is automatic.
    for (const Style *style : {&m_style, &m_counterpart}) {
        for (const QString &stack : style->fontStacks) {
            for (const int start : ranges) {
                ++progress->open;
                fetchGlyphs(*style, stack, u"%1-%2"_s.arg(start).arg(start + 255), count);
            }
        }
        for (int index = 0; index < style->sprites.size(); ++index) {
            for (const QLatin1StringView suffix : {".json"_L1, ".png"_L1, "@2x.json"_L1, "@2x.png"_L1}) {
                ++progress->open;
                fetchSprite(*style, index, suffix, count);
            }
        }
    }
    count({}, Ok);
}

// --- Serving the map view ---

void TileServer::readRequest(QTcpSocket *socket)
{
    // One request at a time per connection; anything further stays buffered.
    if (socket->property(Busy).toBool()) {
        return;
    }
    const QByteArray buffered = socket->peek(8192);
    const qsizetype end = buffered.indexOf("\r\n\r\n");
    if (end < 0) {
        if (buffered.size() >= 8192) {
            socket->abort();
        }
        return;
    }
    const QByteArray head = socket->read(end + 4);
    const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
    // GET /<token>/style/<generation>.json
    //     /<token>/tiles/<source>/<z>/<x>/<y>
    //     /<token>/fonts/<font stack>/<range>.pbf
    //     /<token>/sprites/<index>/sprite<suffix>
    const QString path = requestLine.size() == 3 ? QString::fromLatin1(requestLine[1]).section(u'?', 0, 0) : QString();
    const QStringList parts = path.split(u'/');
    if (requestLine.value(0) != "GET" || parts.size() < 4 || parts[1] != m_token) {
        respond(socket, {}, Missing, {});
        return;
    }

    socket->setProperty(Busy, true);
    socket->setProperty(Path, path.section(u'/', 2));
    const auto reply = [this, guard = QPointer<QTcpSocket>(socket)](const QByteArray &contentType) {
        return [this, guard, contentType](const QByteArray &data, Status status) {
            if (guard) {
                guard->setProperty(Busy, false);
                respond(guard, data, status, contentType);
            }
        };
    };
    const QString kind = parts[2];
    if (kind == "style"_L1 && parts.size() == 4) {
        loadStyle([this, send = reply("application/json")](bool ok) {
            send(ok ? m_style.json : QByteArray(), ok ? Ok : Unavailable);
        });
    } else if (kind == "tiles"_L1 && parts.size() == 7) {
        fetchTile(parts[3].toInt(), parts[4].toInt(), parts[5].toInt(), parts[6].toInt(), reply("application/octet-stream"));
    } else if (kind == "fonts"_L1 && parts.size() == 5 && parts[4].endsWith(".pbf"_L1)) {
        fetchGlyphs(m_style, QUrl::fromPercentEncoding(parts[3].toLatin1()), parts[4].chopped(4), reply("application/x-protobuf"));
    } else if (kind == "sprites"_L1 && parts.size() == 5 && parts[4].startsWith("sprite"_L1)) {
        const QString suffix = parts[4].mid(6);
        fetchSprite(m_style, parts[3].toInt(), suffix, reply(suffix.endsWith(".png"_L1) ? "image/png" : "application/json"));
    } else {
        reply({})({}, Missing);
    }
}

void TileServer::respond(QTcpSocket *socket, const QByteArray &body, Status status, const QByteArray &contentType)
{
    qCDebug(Log) << "answering" << socket->property(Path).toString() << "with" << status << body.size() << "bytes";
    QByteArray response;
    switch (status) {
    case Ok:
        response = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType + "\r\n";
        // Tiles are stored the way the map source sent them.
        if (body.startsWith("\x1f\x8b")) {
            response += "Content-Encoding: gzip\r\n";
        }
        break;
    case Missing:
        response = "HTTP/1.1 404 Not Found\r\n";
        break;
    case Unavailable:
        // Unlike "not found", this makes the map view ask again later.
        response = "HTTP/1.1 503 Service Unavailable\r\n";
        break;
    }
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n\r\n";
    socket->write(response);
    socket->write(body);
    if (socket->bytesAvailable() > 0) {
        // Handle the next buffered request, outside of this call chain.
        QMetaObject::invokeMethod(
            this,
            [this, guard = QPointer<QTcpSocket>(socket)] {
                if (guard) {
                    readRequest(guard);
                }
            },
            Qt::QueuedConnection);
    }
}
