// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCache>
#include <QElapsedTimer>
#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

class OfflineManager;
class QJsonObject;
class QSqlDatabase;

/// Place search. Typing searches the downloaded regions only; submitting a
/// query also asks the online Nominatim service, whose usage policy forbids
/// search-as-you-type and more than one request per second.
class Geocoder : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// Maps with the keys title, subtitle, latitude and longitude.
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// True once a submitted search has finished, until the next query.
    Q_PROPERTY(bool finished READ finished NOTIFY resultsChanged)
    /// True when the last submitted search could not reach the online service.
    Q_PROPERTY(bool onlineFailed READ onlineFailed NOTIFY resultsChanged)

public:
    explicit Geocoder(OfflineManager *offline, QObject *parent = nullptr);
    ~Geocoder() override;
    static Geocoder *create(QQmlEngine *, QJSEngine *);

    QVariantList results() const;
    bool busy() const;
    bool finished() const;
    bool onlineFailed() const;

    /// Search while typing: downloaded regions only. The position biases the order.
    Q_INVOKABLE void suggest(const QString &text, double latitude, double longitude);
    /// Search on submit: online as well.
    Q_INVOKABLE void search(const QString &text, double latitude, double longitude);
    Q_INVOKABLE void clear();
    /// Looks up a name for a position; answers with reverseFinished().
    Q_INVOKABLE void reverse(double latitude, double longitude);
    /// The point of interest that the map labels at a position, read from the
    /// map data that is already stored: a map with the keys title, subtitle,
    /// type (OpenStreetMap's word for the kind of place), latitude and
    /// longitude, or an empty one when there is none.
    /// @p zoomLevel is the map's, to tell what is drawn and how large.
    Q_INVOKABLE QVariantMap placeAt(double latitude, double longitude, double zoomLevel) const;
    /// Looks up more about a named place: its address, opening hours and the
    /// like from the downloaded region it lies in, or else online, and an
    /// encyclopedia article where one is linked. Answers with
    /// detailsFinished() and articleFinished() when there is something to show.
    /// Asks Google for the rating and reviews of a named place, with the
    /// user's own key for the Places API; answers with reviewsFinished().
    /// Only to be called when the user has asked for this in the settings:
    /// it tells Google which place was looked at, and is billed to the key.
    Q_INVOKABLE void lookUpReviews(double latitude, double longitude, const QString &name, const QString &key);
    /// @p type is the kind of place as returned by placeAt().
    Q_INVOKABLE void lookUpDetails(double latitude, double longitude, const QString &name, const QString &type);

Q_SIGNALS:
    void resultsChanged();
    void busyChanged();
    void reverseFinished(double latitude, double longitude, const QString &name);
    /// @p details are maps with the keys icon, text, link, note (further text
    /// in a quieter tone) and state ("open" or "closed", to colour the text);
    /// all but the first two may be empty.
    void detailsFinished(double latitude, double longitude, const QVariantList &details);
    /// @p article is a map with the keys text, url, image and imagePage; the
    /// last two are empty when the article has no picture.
    void articleFinished(double latitude, double longitude, const QVariantMap &article);
    /// @p reviews is a map with the keys error (a message, or empty), rating,
    /// count, url (the place on Google Maps) and reviews: maps with the keys
    /// author, authorUrl, photo, rating, text, time and url. Nothing is
    /// reported when Google does not know the place.
    void reviewsFinished(double latitude, double longitude, const QVariantMap &reviews);

private:
    QVariantList offlineSearch(const QString &text, double latitude, double longitude, bool substring) const;
    void closeDatabases();
    void setResults(const QVariantList &results, bool finished, bool onlineFailed);
    void setBusy(bool busy);
    /// Runs @p send now, or later if the last online request was under a second ago.
    void throttled(const std::function<void()> &send);
    QString baseUrl() const;
    /// Opens the place database of a downloaded region, once.
    QSqlDatabase regionDatabase(const QString &id, const QString &directory) const;
    /// The stored OSM tags of the named place near a position.
    bool offlineTags(double latitude, double longitude, const QString &name, const QString &type, QJsonObject *tags) const;
    void lookUpArticle(double latitude, double longitude, const QJsonObject &tags);
    void fetchArticle(double latitude, double longitude, const QString &language, const QString &title);
    void fetchReviews(double latitude, double longitude, const QString &placeId, const QString &key);

    OfflineManager *const m_offline;
    QVariantList m_results;
    bool m_busy = false;
    bool m_finished = false;
    bool m_onlineFailed = false;
    int m_generation = 0;
    QElapsedTimer m_lastRequest;
    QCache<QString, QVariantList> m_cache{50};
    QCache<QString, QVariantMap> m_articles{50};
    mutable QStringList m_connections;
};
