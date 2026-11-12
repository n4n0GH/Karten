// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QUrl>

#include <functional>

class QTcpServer;
class QTcpSocket;
class TileStore;

/// Supplies the map to the map view through a web server on the loopback
/// interface, backed by the TileStore and the configured online map source.
///
/// A map source is a MapLibre style: a description of how to draw the map
/// that points at tiles, fonts and icons. This class hands the map view a
/// copy of the style in which all of those addresses lead back here. Going
/// through this class is what makes the map work offline: the map view only
/// ever talks to it, and it answers from disk whenever it can.
///
/// The address of plain image tiles, with {z}, {x} and {y} placeholders, is
/// accepted as a map source too and wrapped in a minimal style.
class TileServer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// Style address for the map view; changes whenever the map source changes.
    Q_PROPERTY(QString styleUrl READ styleUrl NOTIFY sourceChanged)
    /// The chosen map source: the address of a style or of image tiles, or
    /// "auto" for a light or dark map to go with the application's colours.
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    /// Map sources to offer, as maps with the keys name and url.
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    /// Whether the map source permits downloading it for offline use.
    Q_PROPERTY(bool offlineAllowed READ offlineAllowed NOTIFY styleChanged)
    /// The most tiles the map source permits in one download.
    Q_PROPERTY(int maxPackTiles READ maxPackTiles NOTIFY styleChanged)
    /// The most bytes of tiles the map source permits in one download.
    Q_PROPERTY(double maxPackBytes READ maxPackBytes NOTIFY styleChanged)
    /// Whether the map is drawn from vector data, which has one level of
    /// detail to download instead of one image per zoom level.
    Q_PROPERTY(bool vector READ isVector NOTIFY styleChanged)
    /// Credits the map source asks for, as rich text. Empty when it names none.
    Q_PROPERTY(QString attribution READ attribution NOTIFY styleChanged)

public:
    enum Status {
        Ok,
        Missing, ///< the source has nothing at this address
        Unavailable, ///< not stored, and the source cannot be reached
    };
    using Callback = std::function<void(const QByteArray &, Status)>;

    /// One set of tiles a style draws from.
    struct TileSource {
        int id = 0; ///< id in the TileStore
        QString urlTemplate;
        int minZoom = 0;
        int maxZoom = 0;
        bool vector = false;
        bool tms = false; ///< rows are counted from the south
    };

    explicit TileServer(TileStore *store, QObject *parent = nullptr);
    static TileServer *create(QQmlEngine *, QJSEngine *);
    static TileServer *instance();

    bool listen();

    QString styleUrl() const;
    QString source() const;
    void setSource(const QString &source);
    QVariantList presets() const;
    bool offlineAllowed() const;
    int maxPackTiles() const;
    qint64 maxPackBytes() const;
    bool isVector() const;
    QString attribution() const;
    /// TileStore id of the map source as a whole.
    int styleId() const;
    /// Empty until the style has been loaded.
    QList<TileSource> tileSources() const;
    /// The tiles to store for offline use. With the automatic choice that
    /// includes those of the other map, light or dark, as far as they differ.
    QList<TileSource> packSources() const;

    Q_INVOKABLE bool isValidSource(const QString &source) const;

    /// Delivers a tile from disk, or from the network when it is missing or
    /// out of date.
    void fetchTile(int source, int z, int x, int y, const Callback &callback);
    /// The stored copy of a tile as the map source sent it, without asking
    /// the source; empty when it is not stored.
    QByteArray storedTile(int source, int z, int x, int y) const;
    /// Stores the fonts and icons of the style, so that a downloaded area can
    /// be drawn without a connection. Reports whether all of it worked.
    void fetchStyleResources(const std::function<void(bool)> &done);

Q_SIGNALS:
    void sourceChanged();
    void styleChanged();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    /// The address that a choice stands for right now.
    QString resolve(const QString &choice) const;
    /// Switches to another address and makes the map view load it.
    void useSource(const QString &address);
    void readRequest(QTcpSocket *socket);
    void respond(QTcpSocket *socket, const QByteArray &body, Status status, const QByteArray &contentType);
    QString localBase() const;

    /// A map style, taken apart.
    struct Style {
        QByteArray json; ///< as handed to the map view
        QList<TileSource> tileSources;
        QString glyphs;
        QStringList sprites;
        QStringList fontStacks;
        QString attribution;
    };
    /// Loads the style of the current map source and everything it refers to.
    void loadStyle(const std::function<void(bool)> &done);
    /// With the automatic choice: loads the map for the other colours as
    /// well, so that it can be stored for offline use alongside.
    void loadCounterpart();
    void fetchStyle(const QString &address, const std::function<void(bool, const Style &)> &done);
    Style
    buildStyle(const QString &address, const QJsonObject &style, const QHash<QString, QJsonObject> &tileJson, const QHash<QString, QString> &tileJsonAddresses);
    Style buildRasterStyle(const QString &address);
    enum Freshness {
        PreferCurrent, ///< ask the source first when the stored copy is out of date
        StaleIsFine, ///< answer with the stored copy and update it afterwards
    };
    void fetchResource(const QUrl &url, Freshness freshness, const Callback &callback);
    void fetchGlyphs(const Style &style, const QString &fontStack, const QString &range, const Callback &callback);
    void fetchSprite(const Style &style, int index, const QString &suffix, const Callback &callback);

    TileStore *const m_store;
    QTcpServer *m_server = nullptr;
    QString m_token;
    QString m_choice; ///< as chosen, possibly "auto"
    QString m_source; ///< the address in use
    int m_styleId = 0;
    int m_generation = 0; ///< counts map source changes

    bool m_ready = false;
    bool m_loading = false;
    QList<std::function<void(bool)>> m_waiting;
    Style m_style;
    Style m_counterpart; ///< empty unless the choice is automatic

    QHash<QString, QList<Callback>> m_pending;
};
