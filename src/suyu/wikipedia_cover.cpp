// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "suyu/wikipedia_cover.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

namespace WikipediaCover {

QByteArray GetWithin(QNetworkAccessManager& network, const QUrl& url, const QElapsedTimer& clock,
                     qint64 budget_ms, const QString& user_agent) {
    QByteArray body;
    const qint64 remaining = budget_ms - clock.elapsed();
    if (remaining <= 0 || !url.isValid()) {
        return body;
    }
    QNetworkRequest request(url);
    // Wikimedia asks API clients to identify themselves.
    request.setHeader(QNetworkRequest::UserAgentHeader, user_agent);
    request.setTransferTimeout(static_cast<int>(remaining));
    QNetworkReply* reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(static_cast<int>(remaining), &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
        body = reply->readAll();
    } else {
        reply->abort();
    }
    delete reply;
    return body;
}

CoverUrls FindCoverUrls(QNetworkAccessManager& network, const QString& title,
                        const QElapsedTimer& clock, qint64 budget_ms, const QString& user_agent) {
    QString page = title;
    page.remove(QRegularExpression(QStringLiteral("[\\x{2122}\\x{00AE}\\x{00A9}]")));
    page = page.simplified().replace(QLatin1Char(' '), QLatin1Char('_'));
    if (page.isEmpty()) {
        return {};
    }
    for (const QString& candidate : {QString(page + QStringLiteral("_(video_game)")), page}) {
        const QJsonObject json =
            QJsonDocument::fromJson(
                GetWithin(
                    network,
                    QUrl::fromEncoded(QByteArray(
                        QByteArrayLiteral("https://en.wikipedia.org/api/rest_v1/page/summary/") +
                        QUrl::toPercentEncoding(candidate))),
                    clock, budget_ms, user_agent))
                .object();
        // One game, not a series, franchise or character that shares the name.
        static const QRegularExpression kNotOneGame(
            QStringLiteral("series|franchise|character"),
            QRegularExpression::CaseInsensitiveOption);
        const QString description = json.value(QStringLiteral("description")).toString();
        if (json.value(QStringLiteral("type")).toString() != QStringLiteral("standard") ||
            !description.contains(QStringLiteral("video game"), Qt::CaseInsensitive) ||
            description.contains(kNotOneGame)) {
            continue;
        }
        const auto source = [&json](const char* key) {
            return json.value(QLatin1String(key))
                .toObject()
                .value(QStringLiteral("source"))
                .toString();
        };
        return {source("thumbnail"), source("originalimage")};
    }
    return {};
}

QString DiscordImageUrl(const CoverUrls& urls) {
    for (const QString& url : {urls.thumbnail, urls.original}) {
        if (url.startsWith(QStringLiteral("https://")) && url.toUtf8().size() <= 256 &&
            !url.contains(QRegularExpression(QStringLiteral("[\\s\\x00-\\x1f]")))) {
            return url;
        }
    }
    return {};
}

bool WriteDiscordIni(const QString& package_dir, bool enabled, const QString& cover_url) {
    QSaveFile file(QDir(package_dir).filePath(QStringLiteral("discord.ini")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QByteArray text =
        QByteArrayLiteral("# Discord Rich Presence for this exported game.\r\n"
                          "# enabled=0 stops the game from contacting Discord at all.\r\n"
                          "# cover_url is the https image Discord shows; leave empty for the "
                          "suyu logo.\r\n");
    text += enabled ? QByteArrayLiteral("enabled=1\r\n") : QByteArrayLiteral("enabled=0\r\n");
    text += QByteArrayLiteral("cover_url=") +
            (enabled ? DiscordImageUrl({cover_url, {}}).toUtf8() : QByteArray{}) +
            QByteArrayLiteral("\r\n");
    file.write(text);
    return file.commit();
}

} // namespace WikipediaCover
