// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "extractimporter.h"

#include "graphbuilder.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

using namespace Qt::StringLiterals;

QString ExtractImporter::graphPath(const QString &directory)
{
    return directory + u"/graph.bin"_s;
}

QString ExtractImporter::placesPath(const QString &directory)
{
    return directory + u"/places.sqlite"_s;
}

static QString writePlaces(const QString &path, const std::vector<Place> &places)
{
    QFile::remove(path);
    const QString connection = u"import-"_s + QUuid::createUuid().toString(QUuid::Id128);
    QString error;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        db.setDatabaseName(path);
        if (!db.open()) {
            error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.exec(u"PRAGMA synchronous = OFF"_s);
            query.exec(
                u"CREATE TABLE places (name TEXT NOT NULL, fold TEXT NOT NULL, kind INTEGER NOT NULL, type TEXT, lat REAL NOT NULL, lon REAL NOT NULL, tags TEXT)"_s);
            db.transaction();
            query.prepare(u"INSERT INTO places (name, fold, kind, type, lat, lon, tags) VALUES (?, ?, ?, ?, ?, ?, ?)"_s);
            for (const Place &place : places) {
                query.bindValue(0, place.name);
                query.bindValue(1, place.name.toCaseFolded());
                query.bindValue(2, int(place.kind));
                query.bindValue(3, place.type);
                query.bindValue(4, place.lat);
                query.bindValue(5, place.lon);
                query.bindValue(6, place.tags.isEmpty() ? QVariant() : QVariant(QString::fromUtf8(place.tags)));
                if (!query.exec()) {
                    error = query.lastError().text();
                    break;
                }
            }
            query.finish();
            if (!db.commit() && error.isEmpty()) {
                error = db.lastError().text();
            }
            QSqlQuery(db).exec(u"CREATE INDEX places_fold ON places (fold)"_s);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return error;
}

ExtractImporter::Result ExtractImporter::run(const QString &pbfPath, const QString &directory, const std::function<bool(int)> &progress)
{
    Result result;
    QFile file(pbfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    const double size = double(std::max<qint64>(file.size(), 1));

    // The file lists nodes before ways, but we only know which nodes matter
    // once we have seen the roads, hence two passes.
    GraphBuilder builder;
    Osm::PbfReader reader;
    const auto pass = [&](int want, int from, int to) {
        file.seek(0);
        int last = -1;
        return reader.read(&file, &builder, want, [&](qint64 position) {
            const int percent = from + int((to - from) * (double(position) / size));
            if (percent == last) {
                return true;
            }
            last = percent;
            return progress(percent);
        });
    };
    if (!pass(Osm::PbfReader::Ways, 0, 35)) {
        result.error = reader.errorString();
        return result;
    }
    builder.prepareNodes();
    if (!pass(Osm::PbfReader::Nodes, 35, 80)) {
        result.error = reader.errorString();
        return result;
    }
    file.close();

    RoadGraph graph;
    std::vector<Place> places;
    builder.finish(&graph, &places);
    if (graph.nodeCount() == 0) {
        result.error = i18n("The downloaded data does not contain any roads.");
        return result;
    }
    if (!progress(90)) {
        result.error = u"Cancelled"_s;
        return result;
    }

    QDir().mkpath(directory);
    if (!graph.save(graphPath(directory), &result.error)) {
        return result;
    }
    result.error = writePlaces(placesPath(directory), places);
    result.south = graph.minLat;
    result.west = graph.minLon;
    result.north = graph.maxLat;
    result.east = graph.maxLon;
    progress(100);
    return result;
}
