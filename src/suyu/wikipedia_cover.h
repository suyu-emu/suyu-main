// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

class QElapsedTimer;
class QNetworkAccessManager;
class QUrl;

namespace WikipediaCover {

/// Lead image of a game's English Wikipedia article, as listed by the REST page summary.
struct CoverUrls {
    QString thumbnail; ///< thumbnail.source, a few hundred pixels wide
    QString original;  ///< originalimage.source, full size
};

/// GETs url within what is left of budget_ms on clock, identifying the client with user_agent.
/// Blocks in a local event loop that excludes user input. Returns an empty array on failure.
QByteArray GetWithin(QNetworkAccessManager& network, const QUrl& url, const QElapsedTimer& clock,
                     qint64 budget_ms, const QString& user_agent);

/// Looks the game up on English Wikipedia, which receives its title: "<title> (video game)"
/// first, then "<title>". A page counts only when its short description calls it a video game
/// and not a series, franchise or character. All requests share budget_ms on clock. Blocks like
/// GetWithin, so run it off the GUI thread unless the caller already accepts that. Both URLs are
/// empty when nothing matched.
CoverUrls FindCoverUrls(QNetworkAccessManager& network, const QString& title,
                        const QElapsedTimer& clock, qint64 budget_ms, const QString& user_agent);

/// The image to show in Discord: the thumbnail, else the original. Empty unless it is an https
/// URL of at most 256 bytes, which is what Discord accepts as an image key.
QString DiscordImageUrl(const CoverUrls& urls);

/// Writes discord.ini into package_dir, beside the exported launcher. suyu-cmd reads it at
/// start: enabled=0 keeps it from contacting Discord, cover_url is the image it shows.
bool WriteDiscordIni(const QString& package_dir, bool enabled, const QString& cover_url);

} // namespace WikipediaCover
