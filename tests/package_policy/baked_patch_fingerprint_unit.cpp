#include <iostream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QStringBuilder>
#include <QString>
#include <nlohmann/json.hpp>

int main() {
    int failures = 0;
    const char* names[] = {"Static FPS", reinterpret_cast<const char*>(u8"静的 FPS 😀"),
                          "quote\" slash/ backslash\\ newline\n", reinterpret_cast<const char*>(u8"line separator")};
    for (const auto* name : names) {
        const QString path = QStringLiteral("exefs/") + QString::fromStdString("main.pchtxt");
        const auto digest = QCryptographicHash::hash(QByteArrayView("patch bytes", 11),
                                                    QCryptographicHash::Sha256).toHex();
        const QJsonObject file{{QStringLiteral("path"), path},
                              {QStringLiteral("sha256"), QString::fromLatin1(digest)},
                              {QStringLiteral("size"), static_cast<qint64>(11)},
                              {QStringLiteral("target_build_id"), QString(64, 'A').toUpper()}};
        const QJsonArray mods{QJsonObject{{"name", QString::fromUtf8(name)},
                                          {"files", QJsonArray{file}}}};
        const auto qt = QJsonDocument(mods).toJson(QJsonDocument::Compact);
        const std::string serialized(qt.constData(), static_cast<std::size_t>(qt.size()));
        const auto native = nlohmann::json::parse(serialized).dump();
        if (native != serialized) {
            ++failures;
            std::cerr << "Fingerprint serialization differs:\nQt: " << serialized << "\nJSON: " << native << '\n';
        }
    }
    std::cout << "4 Qt/nlohmann fingerprint comparisons, " << failures << " failures\n";
    return failures ? 1 : 0;
}
