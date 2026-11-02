// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

#include "suyu/steam_integration.h"

// Steam binary VDF type tags
namespace VdfType {
constexpr quint8 SubSection = 0x00;
constexpr quint8 String = 0x01;
constexpr quint8 Uint32 = 0x02;
constexpr quint8 EndSection = 0x08;
} // namespace VdfType

SteamIntegration::SteamIntegration(QObject* parent) : QObject(parent) {
    steam_path_ = FindSteamPath();
    network_manager_ = new QNetworkAccessManager(this);
}

QString SteamIntegration::FindSteamPath() const {
    const QString env_path = QString::fromUtf8(qgetenv("STEAM_PATH"));
    if (!env_path.isEmpty()) {
        return QDir::toNativeSeparators(env_path);
    }

#ifdef _WIN32
    QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                       QSettings::NativeFormat);
    const QString registry_path = settings.value(QStringLiteral("SteamPath")).toString();
    if (!registry_path.isEmpty()) {
        return QDir::toNativeSeparators(registry_path);
    }
    return QStringLiteral("C:/Program Files (x86)/Steam");
#elif defined(__APPLE__)
    return QDir::homePath() + QStringLiteral("/Library/Application Support/Steam");
#else
    const QString home = QDir::homePath();
    const QString steam_root = home + QStringLiteral("/.steam/steam");
    const QString legacy_root = home + QStringLiteral("/.local/share/Steam");
    const QString flatpak_root = home + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
    if (QDir(steam_root).exists()) {
        return steam_root;
    }
    if (QDir(legacy_root).exists()) {
        return legacy_root;
    }
    if (QDir(flatpak_root).exists()) {
        return flatpak_root;
    }
    return steam_root;
#endif
}

SteamIntegration::~SteamIntegration() = default;

bool SteamIntegration::IsSteamInstalled() const {
    return QDir(steam_path_).exists();
}

QString SteamIntegration::GetSteamUserdataPath() const {
    return QDir(steam_path_).filePath(QStringLiteral("userdata"));
}

QString SteamIntegration::FindShortcutsVdf() const {
    QDir userdata(GetSteamUserdataPath());
    if (!userdata.exists()) {
        return {};
    }

    // Iterate user directories, pick the first one that has a config/shortcuts.vdf
    const auto entries = userdata.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& user_id : entries) {
        const QString vdf_path =
            userdata.filePath(user_id + QStringLiteral("/config/shortcuts.vdf"));
        if (QFile::exists(vdf_path)) {
            return vdf_path;
        }
        // Also check if config directory exists but no shortcuts.vdf yet (first time)
        const QString config_dir = userdata.filePath(user_id + QStringLiteral("/config"));
        if (QDir(config_dir).exists()) {
            return vdf_path; // Return the expected path even if file doesn't exist
        }
    }
    return {};
}

quint32 SteamIntegration::GenerateAppId(const QString& exe, const QString& app_name) {
    // Steam generates non-Steam shortcut AppIDs via CRC32 of (exe + app_name)
    // then applies: (crc | 0x80000000) >> 0
    const QByteArray input = (exe + app_name).toUtf8();
    quint32 crc = 0xFFFFFFFF;
    for (char byte : input) {
        crc ^= static_cast<quint8>(byte);
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    crc ^= 0xFFFFFFFF;
    return (crc | 0x80000000);
}

// --- VDF Binary Format Parser ---
// shortcuts.vdf is a binary VDF file with the structure:
//   \x00 "shortcuts" \x00
//     \x00 "0" \x00          (shortcut index as string)
//       \x01 "appid" \x00 <4-byte-uint32>    (or sometimes \x02)
//       \x01 "AppName" \x00 <null-term-string>
//       \x01 "Exe" \x00 <null-term-string>
//       ...
//       \x08                 (end of this shortcut)
//     \x00 "1" \x00
//       ...
//     \x08                   (end of shortcuts section)
//   \x08                     (end of root)

std::vector<SteamIntegration::SteamShortcut>
SteamIntegration::ParseShortcutsVdf(const QByteArray& data) const {
    std::vector<SteamShortcut> shortcuts;

    int pos = 0;
    const int size = data.size();

    auto readByte = [&]() -> quint8 {
        if (pos >= size) return VdfType::EndSection;
        return static_cast<quint8>(data[pos++]);
    };

    auto readString = [&]() -> QByteArray {
        QByteArray result;
        while (pos < size && data[pos] != '\0') {
            result.append(data[pos++]);
        }
        if (pos < size) pos++; // skip null terminator
        return result;
    };

    auto readUint32 = [&]() -> quint32 {
        if (pos + 4 > size) return 0;
        quint32 val = 0;
        val |= static_cast<quint32>(static_cast<quint8>(data[pos]));
        val |= static_cast<quint32>(static_cast<quint8>(data[pos + 1])) << 8;
        val |= static_cast<quint32>(static_cast<quint8>(data[pos + 2])) << 16;
        val |= static_cast<quint32>(static_cast<quint8>(data[pos + 3])) << 24;
        pos += 4;
        return val;
    };

    // Top-level: expect \x00 "shortcuts" \x00
    if (readByte() != VdfType::SubSection) return shortcuts;
    const QByteArray root_key = readString();
    if (root_key != "shortcuts") return shortcuts;

    // Parse each shortcut entry
    while (pos < size) {
        const quint8 entry_type = readByte();
        if (entry_type == VdfType::EndSection) break;
        if (entry_type != VdfType::SubSection) break;

        readString(); // index string like "0", "1", etc.

        SteamShortcut sc;

        // Parse fields within this shortcut
        while (pos < size) {
            const quint8 field_type = readByte();
            if (field_type == VdfType::EndSection) break;

            const QByteArray key = readString();

            if (field_type == VdfType::String) {
                const QByteArray value = readString();
                if (key == "AppName" || key == "appname") {
                    sc.app_name = QString::fromUtf8(value);
                } else if (key == "Exe" || key == "exe") {
                    sc.exe = QString::fromUtf8(value);
                } else if (key == "StartDir" || key == "startdir") {
                    sc.start_dir = QString::fromUtf8(value);
                } else if (key == "icon") {
                    sc.icon = QString::fromUtf8(value);
                } else if (key == "ShortcutPath" || key == "shortcutpath") {
                    sc.shortcut_path = QString::fromUtf8(value);
                } else if (key == "LaunchOptions" || key == "launchoptions") {
                    sc.launch_options = QString::fromUtf8(value);
                }
            } else if (field_type == VdfType::Uint32) {
                const quint32 value = readUint32();
                if (key == "appid") {
                    sc.id = value;
                } else if (key == "IsHidden" || key == "ishidden") {
                    sc.is_hidden = (value != 0);
                } else if (key == "AllowDesktopConfig" || key == "allowdesktopconfig") {
                    sc.allow_desktop_config = (value != 0);
                } else if (key == "AllowOverlay" || key == "allowoverlay") {
                    sc.allow_overlay = (value != 0);
                } else if (key == "LastPlayTime" || key == "lastplaytime") {
                    sc.last_play_time = static_cast<qint32>(value);
                }
            } else if (field_type == VdfType::SubSection) {
                // Nested sub-section like "tags"
                const bool is_tags = (key == "tags");
                while (pos < size) {
                    const quint8 sub_type = readByte();
                    if (sub_type == VdfType::EndSection) break;
                    readString(); // sub-key (index)
                    if (sub_type == VdfType::String) {
                        const QByteArray tag_val = readString();
                        if (is_tags) {
                            sc.tags.append(QString::fromUtf8(tag_val));
                        }
                    } else if (sub_type == VdfType::Uint32) {
                        readUint32();
                    }
                }
            }
        }

        if (!sc.app_name.isEmpty()) {
            if (sc.id == 0) {
                sc.id = GenerateAppId(sc.exe, sc.app_name);
            }
            shortcuts.push_back(std::move(sc));
        }
    }

    return shortcuts;
}

void SteamIntegration::VdfWriteString(QByteArray& buf, quint8 type, const QByteArray& key,
                                       const QByteArray& value) const {
    buf.append(static_cast<char>(type));
    buf.append(key);
    buf.append('\0');
    buf.append(value);
    buf.append('\0');
}

void SteamIntegration::VdfWriteUint32(QByteArray& buf, const QByteArray& key,
                                       quint32 value) const {
    buf.append(static_cast<char>(VdfType::Uint32));
    buf.append(key);
    buf.append('\0');
    buf.append(static_cast<char>(value & 0xFF));
    buf.append(static_cast<char>((value >> 8) & 0xFF));
    buf.append(static_cast<char>((value >> 16) & 0xFF));
    buf.append(static_cast<char>((value >> 24) & 0xFF));
}

QByteArray SteamIntegration::SerializeShortcutsVdf(
    const std::vector<SteamShortcut>& shortcuts) const {
    QByteArray buf;

    // Root section: \x00 "shortcuts" \x00
    buf.append(static_cast<char>(VdfType::SubSection));
    buf.append("shortcuts");
    buf.append('\0');

    for (size_t i = 0; i < shortcuts.size(); ++i) {
        const auto& sc = shortcuts[i];

        // Entry header: \x00 "<index>" \x00
        buf.append(static_cast<char>(VdfType::SubSection));
        buf.append(QByteArray::number(static_cast<int>(i)));
        buf.append('\0');

        VdfWriteUint32(buf, "appid", sc.id);
        VdfWriteString(buf, VdfType::String, "AppName", sc.app_name.toUtf8());
        VdfWriteString(buf, VdfType::String, "Exe", sc.exe.toUtf8());
        VdfWriteString(buf, VdfType::String, "StartDir", sc.start_dir.toUtf8());
        VdfWriteString(buf, VdfType::String, "icon", sc.icon.toUtf8());
        VdfWriteString(buf, VdfType::String, "ShortcutPath", sc.shortcut_path.toUtf8());
        VdfWriteString(buf, VdfType::String, "LaunchOptions", sc.launch_options.toUtf8());
        VdfWriteUint32(buf, "IsHidden", sc.is_hidden ? 1 : 0);
        VdfWriteUint32(buf, "AllowDesktopConfig", sc.allow_desktop_config ? 1 : 0);
        VdfWriteUint32(buf, "AllowOverlay", sc.allow_overlay ? 1 : 0);
        VdfWriteUint32(buf, "OpenVR", 0);
        VdfWriteUint32(buf, "Devkit", 0);
        VdfWriteString(buf, VdfType::String, "DevkitGameID", "");
        VdfWriteUint32(buf, "DevkitOverrideAppID", 0);
        VdfWriteUint32(buf, "LastPlayTime", static_cast<quint32>(sc.last_play_time));
        VdfWriteString(buf, VdfType::String, "FlatpakAppID", "");

        // Tags sub-section
        buf.append(static_cast<char>(VdfType::SubSection));
        buf.append("tags");
        buf.append('\0');
        for (int t = 0; t < sc.tags.size(); ++t) {
            VdfWriteString(buf, VdfType::String, QByteArray::number(t),
                           sc.tags[t].toUtf8());
        }
        buf.append(static_cast<char>(VdfType::EndSection));

        buf.append(static_cast<char>(VdfType::EndSection)); // end shortcut entry
    }

    buf.append(static_cast<char>(VdfType::EndSection)); // end shortcuts section
    buf.append(static_cast<char>(VdfType::EndSection)); // end root

    return buf;
}

std::vector<SteamIntegration::SteamShortcut> SteamIntegration::ListShortcuts() const {
    const QString vdf_path = FindShortcutsVdf();
    if (vdf_path.isEmpty()) {
        return {};
    }

    QFile file(vdf_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }

    return ParseShortcutsVdf(file.readAll());
}

bool SteamIntegration::AddSuyuSelfShortcut() {
    // suyu.ico sits next to the executable in every build/install layout
    // (see dist/suyu.ico, deployed alongside bin/suyu.exe).
    const QString exe_dir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
    QString icon_path = QDir(exe_dir).filePath(QStringLiteral("suyu.ico"));
    if (!QFileInfo::exists(icon_path)) {
        icon_path.clear();
    }
    return AddGameShortcut(QStringLiteral("suyu"), QString(), icon_path);
}

namespace {

// One shortcut entry of shortcuts.vdf, as byte offsets into the file.
struct VdfEntryRange {
    int begin{};      // at the entry's 0x00 type byte
    int body_begin{}; // just past its index key ("0", "1", ...)
    int end{};        // just past its closing 0x08
    QString app_name;
    QString exe;
};

// Walks Steam's binary shortcuts.vdf strictly. It understands every value type Steam writes
// and fails on anything else, so a caller never rewrites a file it has not read completely.
// Returns the length of the header (root key) on success, or -1.
int ScanShortcutsVdf(const QByteArray& d, std::vector<VdfEntryRange>& entries) {
    const int n = static_cast<int>(d.size());
    int p = 0;
    const auto c_string = [&](QByteArray* out) {
        const int z = static_cast<int>(d.indexOf('\0', p));
        if (z < 0) {
            return false;
        }
        if (out) {
            *out = d.mid(p, z - p);
        }
        p = z + 1;
        return true;
    };
    std::function<bool(quint8)> skip_value = [&](quint8 type) -> bool {
        switch (type) {
        case 0x01: // string
            return c_string(nullptr);
        case 0x02: // int32
        case 0x03: // float32
        case 0x04: // pointer
        case 0x06: // color
            if (p + 4 > n) {
                return false;
            }
            p += 4;
            return true;
        case 0x07: // uint64
        case 0x0A: // int64
            if (p + 8 > n) {
                return false;
            }
            p += 8;
            return true;
        case 0x00: // nested map
            while (true) {
                if (p >= n) {
                    return false;
                }
                const quint8 sub = static_cast<quint8>(d[p++]);
                if (sub == 0x08) {
                    return true;
                }
                if (!c_string(nullptr) || !skip_value(sub)) {
                    return false;
                }
            }
        default:
            return false;
        }
    };

    QByteArray root;
    if (n < 2 || d[p++] != '\0' || !c_string(&root) ||
        root.compare("shortcuts", Qt::CaseInsensitive) != 0) {
        return -1;
    }
    const int header_length = p;
    while (true) {
        if (p >= n) {
            return -1;
        }
        VdfEntryRange entry;
        entry.begin = p;
        const quint8 type = static_cast<quint8>(d[p++]);
        if (type == 0x08) {
            break;
        }
        if (type != 0x00 || !c_string(nullptr)) {
            return -1;
        }
        entry.body_begin = p;
        while (true) {
            if (p >= n) {
                return -1;
            }
            const quint8 field = static_cast<quint8>(d[p++]);
            if (field == 0x08) {
                break;
            }
            QByteArray key;
            if (!c_string(&key)) {
                return -1;
            }
            if (field == 0x01) {
                QByteArray value;
                if (!c_string(&value)) {
                    return -1;
                }
                const QByteArray lower = key.toLower();
                if (lower == "appname") {
                    entry.app_name = QString::fromUtf8(value);
                } else if (lower == "exe") {
                    entry.exe = QString::fromUtf8(value);
                }
            } else if (!skip_value(field)) {
                return -1;
            }
        }
        entry.end = p;
        entries.push_back(std::move(entry));
    }
    // The root map closes the file; anything after it is not something this code understands.
    if (p >= n || d[p++] != 0x08 || p != n) {
        return -1;
    }
    return header_length;
}


// What a byte-preserving rewrite of shortcuts.vdf starts from. The same rule as
// AddLauncherShortcut: a file that exists but cannot be read in full, or that the strict scan
// does not fully understand, is never rewritten, because rebuilding it from a partial read
// would delete the user's other shortcuts. A missing file reads as empty.
struct ShortcutsFile {
    QByteArray original;
    std::vector<VdfEntryRange> entries;
    int header_length = 0;
};

bool ReadShortcutsFile(const QString& vdf_path, ShortcutsFile& file) {
    if (!QFileInfo::exists(vdf_path)) {
        return true;
    }
    QFile in(vdf_path);
    if (!in.open(QIODevice::ReadOnly)) {
        return false;
    }
    file.original = in.readAll();
    if (file.original.size() != in.size()) {
        return false; // a short read is not a file to rebuild from
    }
    if (file.original.isEmpty()) {
        return true;
    }
    file.header_length = ScanShortcutsVdf(file.original, file.entries);
    return file.header_length >= 0;
}

// The body of the only entry in a one-shortcut file, and that file's header.
bool SplitSingleEntry(const QByteArray& single, QByteArray& header, QByteArray& body) {
    std::vector<VdfEntryRange> entries;
    if (ScanShortcutsVdf(single, entries) < 0 || entries.size() != 1) {
        return false;
    }
    header = single.left(entries[0].begin);
    body = single.mid(entries[0].body_begin, entries[0].end - entries[0].body_begin);
    return true;
}

// Writes @p bodies in order under @p header, re-indexing their keys, and commits only a
// result the scanner reads back.
bool WriteShortcutsFile(const QString& vdf_path, const QByteArray& header,
                        const std::vector<QByteArray>& bodies) {
    QByteArray out = header;
    int index = 0;
    for (const auto& body : bodies) {
        out.append('\0');
        out.append(QByteArray::number(index++));
        out.append('\0');
        out.append(body);
    }
    out.append('\x08');
    out.append('\x08');
    std::vector<VdfEntryRange> check;
    if (ScanShortcutsVdf(out, check) < 0 || check.size() != bodies.size()) {
        return false;
    }
    QDir().mkpath(QFileInfo(vdf_path).absolutePath());
    QSaveFile save(vdf_path);
    return save.open(QIODevice::WriteOnly) && save.write(out) == out.size() && save.commit();
}

QByteArray EntryBody(const ShortcutsFile& file, const VdfEntryRange& entry) {
    return file.original.mid(entry.body_begin, entry.end - entry.body_begin);
}

} // namespace

bool SteamIntegration::AddLauncherShortcut(const QString& app_name, const QString& launcher_path,
                                           const QString& replace_title) {
    if (!IsSteamInstalled() || !QFileInfo(launcher_path).isFile()) {
        return false;
    }
    const QString vdf_path = FindShortcutsVdf();
    if (vdf_path.isEmpty()) {
        return false;
    }

    // Other shortcuts are copied through byte for byte, including fields this code does not
    // model, rather than parsed and re-serialized. A file that exists but cannot be read, or
    // that the strict scan does not fully understand, is left alone: rewriting it from a
    // partial read would silently delete the user's other shortcuts.
    QByteArray original;
    std::vector<VdfEntryRange> entries;
    int header_length = 0;
    if (QFileInfo::exists(vdf_path)) {
        QFile file(vdf_path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        original = file.readAll();
        if (original.size() != file.size()) {
            return false; // a short read is not a file to rebuild from
        }
    }
    if (!original.isEmpty()) {
        header_length = ScanShortcutsVdf(original, entries);
        if (header_length < 0) {
            return false;
        }
    }

    const QFileInfo launcher(launcher_path);
    const auto runs_launcher = [&launcher](const QString& exe) {
        return QFileInfo(QString(exe).remove(QLatin1Char('"'))) == launcher;
    };

    // This export's entry, serialized alone and then cut out of that one-entry file.
    SteamShortcut sc;
    sc.app_name = app_name;
    sc.exe = QStringLiteral("\"%1\"").arg(launcher.absoluteFilePath());
    // The export finds its portable user/ folder next to the executable, whatever the
    // working directory, but Steam should still start it from there.
    sc.start_dir = QStringLiteral("\"%1\"").arg(launcher.absolutePath());
    sc.shortcut_path = launcher.absolutePath();
    // The exporter embeds the game's icon in the launcher, which Steam can read directly.
    sc.icon = launcher.absoluteFilePath();
    sc.allow_desktop_config = true;
    sc.allow_overlay = true;
    sc.tags = {QStringLiteral("suyu"), QStringLiteral("Nintendo Switch")};
    // Steam keys artwork and play time by this id, which depends only on name and exe.
    sc.id = GenerateAppId(sc.exe, sc.app_name);
    const QByteArray single = SerializeShortcutsVdf({sc});
    std::vector<VdfEntryRange> single_entries;
    if (ScanShortcutsVdf(single, single_entries) < 0 || single_entries.size() != 1) {
        return false;
    }
    const QByteArray new_body = single.mid(single_entries[0].body_begin,
                                           single_entries[0].end - single_entries[0].body_begin);

    QByteArray out = original.isEmpty() ? single.left(single_entries[0].begin)
                                        : original.left(header_length);
    int index = 0;
    bool placed = false;
    const auto append_entry = [&](const QByteArray& body) {
        out.append('\0');
        out.append(QByteArray::number(index++));
        out.append('\0');
        out.append(body);
    };
    for (const auto& entry : entries) {
        if (runs_launcher(entry.exe)) {
            // An earlier export of this same package: replace it in place, once.
            if (!placed) {
                append_entry(new_body);
                placed = true;
            }
            continue;
        }
        // Only the library's own shortcut - suyu launched with the ROM - is taken over; a
        // hand-made shortcut or another export that shares the name is left alone.
        if (!replace_title.isEmpty() && entry.app_name == replace_title &&
            QFileInfo(QString(entry.exe).remove(QLatin1Char('"')))
                .fileName()
                .startsWith(QStringLiteral("suyu"), Qt::CaseInsensitive)) {
            continue;
        }
        append_entry(original.mid(entry.body_begin, entry.end - entry.body_begin));
    }
    if (!placed) {
        append_entry(new_body);
    }
    out.append('\x08');
    out.append('\x08');

    // Never write something this code would itself refuse to read.
    std::vector<VdfEntryRange> check;
    if (ScanShortcutsVdf(out, check) < 0) {
        return false;
    }

    QDir().mkpath(QFileInfo(vdf_path).absolutePath());
    QSaveFile save(vdf_path);
    if (!save.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (save.write(out) != out.size() || !save.commit()) {
        return false;
    }
    emit ShortcutAdded(app_name);
    return true;
}

namespace {

// A soft blur from scaling alone: shrink hard, then grow back in doubling steps so the
// bilinear passes smooth one another out. Fills @p size, cropping the source to it.
QImage BlurredFill(const QImage& source, const QSize& size) {
    const QImage filled =
        source.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QImage blurred = filled
                         .copy((filled.width() - size.width()) / 2,
                               (filled.height() - size.height()) / 2, size.width(), size.height())
                         .scaled(std::max(1, size.width() / 48), std::max(1, size.height() / 48),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    while (blurred.width() * 2 < size.width()) {
        blurred = blurred.scaled(blurred.size() * 2, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
    }
    return blurred.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// The blurred art, dimmed toward the bottom so the sharp art and the title stand out.
QImage Backdrop(const QImage& art, const QSize& size) {
    QImage canvas = BlurredFill(art, size);
    QPainter painter(&canvas);
    QLinearGradient shade(0, 0, 0, size.height());
    shade.setColorAt(0.0, QColor(0, 0, 0, 60));
    shade.setColorAt(1.0, QColor(0, 0, 0, 170));
    painter.fillRect(canvas.rect(), shade);
    return canvas;
}

// The sharp art, fitted and centred in @p bounds, with rounded corners over a soft shadow.
void DrawArt(QPainter& painter, const QImage& art, const QRect& bounds) {
    const QSize fitted = art.size().scaled(bounds.size(), Qt::KeepAspectRatio);
    const QRect target(bounds.x() + (bounds.width() - fitted.width()) / 2,
                       bounds.y() + (bounds.height() - fitted.height()) / 2, fitted.width(),
                       fitted.height());
    const qreal radius = std::min(target.width(), target.height()) * 0.05;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 16));
    for (int spread = 2; spread <= 16; spread += 2) {
        painter.drawRoundedRect(QRectF(target).adjusted(-spread, -spread + 8, spread, spread + 8),
                                radius + spread, radius + spread);
    }
    QPainterPath corners;
    corners.addRoundedRect(QRectF(target), radius, radius);
    painter.save();
    painter.setClipPath(corners);
    painter.drawImage(target,
                      art.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    painter.restore();
}

// The largest bold font, up to @p max_px, at which @p title fits in @p bounds.
QFont TitleFont(const QString& title, const QRect& bounds, int max_px, int flags) {
    QFont font;
    font.setBold(true);
    for (int px = max_px; px > 12; px -= 2) {
        font.setPixelSize(px);
        const QRect needed = QFontMetrics(font).boundingRect(bounds, flags, title);
        if (needed.width() <= bounds.width() && needed.height() <= bounds.height()) {
            break;
        }
    }
    return font;
}

void DrawTitle(QPainter& painter, const QString& title, const QRect& bounds, const QFont& font,
               int flags) {
    painter.setFont(font);
    painter.setPen(QColor(0, 0, 0, 170));
    painter.drawText(bounds.translated(0, std::max(2, font.pixelSize() / 20)), flags, title);
    painter.setPen(Qt::white);
    painter.drawText(bounds, flags, title);
}

bool SavePng(const QImage& image, const QString& path) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
}

} // namespace

bool SteamIntegration::WriteLauncherArtwork(const QString& app_name, const QString& launcher_path,
                                            const QImage& icon, const QImage& cover) {
    if (icon.isNull() && cover.isNull()) {
        return false;
    }
    const QString vdf_path = FindShortcutsVdf();
    if (vdf_path.isEmpty()) {
        return false;
    }
    // The grid folder belongs to the account whose shortcuts.vdf AddLauncherShortcut edits.
    // Steam names a shortcut's artwork by its unsigned 32-bit appid, which has to be computed
    // from the same quoted exe path and name as the shortcut itself.
    const QString grid = QFileInfo(vdf_path).absolutePath() + QStringLiteral("/grid");
    if (!QDir().mkpath(grid)) {
        return false;
    }
    const QString base =
        grid + QLatin1Char('/') +
        QString::number(GenerateAppId(
            QStringLiteral("\"%1\"").arg(QFileInfo(launcher_path).absoluteFilePath()), app_name));
    const QImage art = (cover.isNull() ? icon : cover)
                           .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int centred = Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap;
    const int beside = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap;

    // Box art already carries the title; the icon gets it written underneath.
    QImage portrait = Backdrop(art, {600, 900});
    {
        QPainter painter(&portrait);
        if (!cover.isNull()) {
            DrawArt(painter, art, QRect(40, 40, 520, 820));
        } else {
            DrawArt(painter, art, QRect(80, 110, 440, 440));
            const QRect text(40, 610, 520, 250);
            DrawTitle(painter, app_name, text, TitleFont(app_name, text, 64, centred), centred);
        }
    }

    QImage wide = Backdrop(art, {920, 430});
    {
        QPainter painter(&wide);
        DrawArt(painter, art, QRect(40, 40, 350, 350));
        const QRect text(430, 40, 450, 350);
        DrawTitle(painter, app_name, text, TitleFont(app_name, text, 60, beside), beside);
    }

    // Steam lays the logo over the hero, so the hero carries no text of its own.
    const QImage hero = Backdrop(art, {1920, 620});

    const int logo_flags = Qt::AlignCenter | Qt::TextWordWrap;
    const QRect logo_bounds(0, 0, 1200, 400);
    const QFont logo_font = TitleFont(app_name, logo_bounds, 140, logo_flags);
    const QRect logo_used = QFontMetrics(logo_font).boundingRect(logo_bounds, logo_flags, app_name);
    QImage logo(logo_used.size() + QSize(32, 32), QImage::Format_ARGB32_Premultiplied);
    logo.fill(Qt::transparent);
    {
        QPainter painter(&logo);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        DrawTitle(painter, app_name, logo.rect().adjusted(8, 8, -8, -8), logo_font, logo_flags);
    }

    const QImage steam_icon = (icon.isNull() ? cover : icon)
                                  .scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    return SavePng(portrait, base + QStringLiteral("p.png")) &&
           SavePng(wide, base + QStringLiteral(".png")) &&
           SavePng(hero, base + QStringLiteral("_hero.png")) &&
           SavePng(logo, base + QStringLiteral("_logo.png")) &&
           SavePng(steam_icon, base + QStringLiteral("_icon.png"));
}

bool SteamIntegration::AddGameShortcut(const QString& game_title, const QString& rom_path,
                                       const QString& icon_path) {
    if (!IsSteamInstalled()) {
        return false;
    }
    const QString vdf_path = FindShortcutsVdf();
    if (vdf_path.isEmpty()) {
        return false;
    }
    // Only this game's entry is written; every other one is copied byte for byte.
    ShortcutsFile file;
    if (!ReadShortcutsFile(vdf_path, file)) {
        return false;
    }

    // Path to the currently running suyu executable
    const QString exe_path = QCoreApplication::applicationFilePath();
    const QString quoted_exe = QStringLiteral("\"%1\"").arg(exe_path);
    const QString start_dir = QStringLiteral("\"%1\"").arg(QFileInfo(exe_path).absolutePath());
    const QString shortcut_path = QFileInfo(exe_path).absolutePath();
    // Empty rom_path means "suyu itself" (its own library UI), not a
    // specific game - don't pass an empty -g "" argument in that case.
    const QString launch_options =
        rom_path.isEmpty() ? QString() : QStringLiteral("-g \"%1\"").arg(rom_path);

    // A shortcut with this title may already exist. Matching on the name alone
    // and returning was not enough: an entry written by an earlier install
    // keeps pointing at that install's executable, so Steam goes on launching a
    // binary that has since moved or been deleted - which looks like the Steam
    // integration silently doing nothing. Seen live, pointing at a different
    // suyu directory entirely. Repoint it instead, and only leave it alone when
    // it already refers to this executable.
    const auto match = std::find_if(file.entries.begin(), file.entries.end(),
                                    [&](const auto& entry) { return entry.app_name == game_title; });
    SteamShortcut sc;
    if (match != file.entries.end()) {
        // The entry's own fields, read from it alone.
        QByteArray alone = file.original.left(file.header_length);
        alone.append('\0');
        alone.append('0');
        alone.append('\0');
        alone.append(EntryBody(file, *match));
        alone.append('\x08');
        alone.append('\x08');
        const auto parsed = ParseShortcutsVdf(alone);
        if (parsed.size() != 1) {
            return false;
        }
        sc = parsed.front();
        const QString existing = QString(sc.exe).remove(QLatin1Char('"'));
        const bool executable_changed = QFileInfo(existing) != QFileInfo(exe_path);
        const bool metadata_changed = executable_changed || sc.start_dir != start_dir ||
                                      sc.shortcut_path != shortcut_path ||
                                      sc.launch_options != launch_options ||
                                      (!icon_path.isEmpty() &&
                                       sc.icon != QFileInfo(icon_path).absoluteFilePath());
        if (!metadata_changed) {
            return true;
        }
        sc.exe = quoted_exe;
        if (executable_changed) {
            sc.id = GenerateAppId(sc.exe, sc.app_name);
        }
        sc.start_dir = start_dir;
        sc.shortcut_path = shortcut_path;
        sc.launch_options = launch_options;
        if (!icon_path.isEmpty()) {
            sc.icon = QFileInfo(icon_path).absoluteFilePath();
        }
    } else {
        sc.app_name = game_title;
        sc.exe = quoted_exe;
        sc.start_dir = start_dir;
        sc.icon = icon_path.isEmpty() ? QString() : QFileInfo(icon_path).absoluteFilePath();
        sc.shortcut_path = shortcut_path;
        sc.launch_options = launch_options;
        sc.allow_desktop_config = true;
        sc.allow_overlay = true;
        sc.tags.append(QStringLiteral("suyu"));
        sc.tags.append(QStringLiteral("Nintendo Switch"));
        sc.id = GenerateAppId(sc.exe, sc.app_name);
    }

    // This entry, serialized alone and cut out of that one-entry file.
    QByteArray header;
    QByteArray new_body;
    if (!SplitSingleEntry(SerializeShortcutsVdf({sc}), header, new_body)) {
        return false;
    }
    std::vector<QByteArray> bodies;
    for (auto it = file.entries.begin(); it != file.entries.end(); ++it) {
        bodies.push_back(it == match ? new_body : EntryBody(file, *it));
    }
    if (match == file.entries.end()) {
        bodies.push_back(new_body);
    }
    if (!WriteShortcutsFile(vdf_path,
                            file.original.isEmpty() ? header
                                                    : file.original.left(file.header_length),
                            bodies)) {
        return false;
    }
    if (match == file.entries.end()) {
        emit ShortcutAdded(game_title);
    }
    return true;
}

bool SteamIntegration::RemoveGameShortcut(const QString& game_title) {
    if (!IsSteamInstalled()) {
        return false;
    }
    const QString vdf_path = FindShortcutsVdf();
    if (vdf_path.isEmpty() || !QFileInfo::exists(vdf_path)) {
        return false;
    }
    // Only the matching entries go; every other one is copied byte for byte.
    ShortcutsFile file;
    if (!ReadShortcutsFile(vdf_path, file) || file.original.isEmpty()) {
        return false;
    }
    std::vector<QByteArray> bodies;
    for (const auto& entry : file.entries) {
        if (entry.app_name != game_title) {
            bodies.push_back(EntryBody(file, entry));
        }
    }
    if (bodies.size() == file.entries.size()) {
        return false; // Not found
    }
    if (!WriteShortcutsFile(vdf_path, file.original.left(file.header_length), bodies)) {
        return false;
    }
    emit ShortcutRemoved(game_title);
    return true;
}

namespace {

QString SteamStoreSearchUrl(const QString& game_title) {
    return QStringLiteral("https://store.steampowered.com/api/storesearch?term=%1&cc=us&l=en")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(game_title)));
}

QString SteamStoreArtworkUrl(quint64 app_id, SteamIntegration::ArtworkType artwork_type) {
    switch (artwork_type) {
        case SteamIntegration::ArtworkType::Hero:
            return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/header.jpg").arg(app_id);
        case SteamIntegration::ArtworkType::Icon:
            return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/capsule_184x69.jpg").arg(app_id);
        case SteamIntegration::ArtworkType::Artwork:
            return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/capsule_616x353.jpg").arg(app_id);
        case SteamIntegration::ArtworkType::Grid:
        default:
            return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/capsule_231x87.jpg").arg(app_id);
    }
}

QString NetworkReplyErrorString(QNetworkReply* reply) {
    const int status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status_code >= 400) {
        return QStringLiteral("HTTP %1").arg(status_code);
    }
    return reply->errorString();
}

QString NormalizeSteamSearchText(QString text) {
    text = text.toLower().trimmed();
    text.replace(QRegularExpression(QStringLiteral(R"([^a-z0-9]+)")), QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(\b(deluxe|ultimate|complete|edition|demo|bundle|remaster|remastered|goty)\b)")),
                 QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral(" "));
    return text.trimmed();
}

int SteamStoreMatchScore(const QString& query_title, const QString& candidate_title) {
    const QString query = NormalizeSteamSearchText(query_title);
    const QString candidate = NormalizeSteamSearchText(candidate_title);

    if (query.isEmpty() || candidate.isEmpty()) {
        return 0;
    }
    if (candidate == query) {
        return 1000;
    }
    if (candidate.startsWith(query)) {
        return 850;
    }
    if (candidate.contains(query)) {
        return 700;
    }

    const QStringList query_parts = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int overlap = 0;
    for (const QString& part : query_parts) {
        if (candidate.contains(part)) {
            overlap += 100;
        }
    }

    return overlap - qAbs(candidate.size() - query.size());
}

qint64 SelectBestSteamStoreAppId(const QJsonArray& items, const QString& game_title) {
    qint64 best_app_id = 0;
    int best_score = std::numeric_limits<int>::min();

    for (const QJsonValue& value : items) {
        const QJsonObject item = value.toObject();
        const qint64 app_id = item[QStringLiteral("id")].toVariant().toLongLong();
        const QString candidate_title = item[QStringLiteral("name")].toString();
        if (app_id == 0 || candidate_title.isEmpty()) {
            continue;
        }

        const int score = SteamStoreMatchScore(game_title, candidate_title);
        if (score > best_score) {
            best_score = score;
            best_app_id = app_id;
        }
    }

    return best_app_id;
}

} // namespace

void SteamIntegration::FetchArtwork(const QString& game_title, const QString& output_path,
                                       ArtworkType artwork_type) {
    const QUrl search_url(SteamStoreSearchUrl(game_title));

    QNetworkRequest request(search_url);
    QNetworkReply* search_reply = network_manager_->get(request);
    connect(search_reply, &QNetworkReply::finished, this,
            [this, search_reply, game_title, output_path, artwork_type]() {
                search_reply->deleteLater();

                if (search_reply->error() != QNetworkReply::NoError) {
                    emit ArtworkFetchFailed(game_title, NetworkReplyErrorString(search_reply));
                    return;
                }

                const QJsonDocument doc = QJsonDocument::fromJson(search_reply->readAll());
                const QJsonArray items = doc.object()[QStringLiteral("items")].toArray();
                if (items.isEmpty()) {
                    emit ArtworkFetchFailed(game_title,
                                            QStringLiteral("No store match found on Steam Store"));
                    return;
                }

                const qint64 app_id = SelectBestSteamStoreAppId(items, game_title);
                if (app_id == 0) {
                    emit ArtworkFetchFailed(game_title,
                                            QStringLiteral("Steam Store returned an invalid app id"));
                    return;
                }

                const QUrl image_url = QUrl(SteamStoreArtworkUrl(static_cast<quint64>(app_id), artwork_type));
                QNetworkReply* img_reply = network_manager_->get(QNetworkRequest(image_url));
                connect(img_reply, &QNetworkReply::finished, this,
                        [this, img_reply, game_title, output_path]() {
                            img_reply->deleteLater();

                            if (img_reply->error() != QNetworkReply::NoError) {
                                emit ArtworkFetchFailed(game_title, NetworkReplyErrorString(img_reply));
                                return;
                            }

                            const int status_code = img_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                            if (status_code >= 400) {
                                emit ArtworkFetchFailed(game_title,
                                                        QStringLiteral("HTTP %1").arg(status_code));
                                return;
                            }

                            QFile file(output_path);
                            if (!file.open(QIODevice::WriteOnly)) {
                                emit ArtworkFetchFailed(game_title,
                                                        QStringLiteral("Cannot write to %1").arg(output_path));
                                return;
                            }
                            file.write(img_reply->readAll());
                            file.close();

                            emit ArtworkFetched(game_title, output_path);
                        });
            });
}
