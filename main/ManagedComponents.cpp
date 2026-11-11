#include "ManagedComponents.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace NekoGui {
    QString ManagedComponentsRoot() {
        return QDir::current().absoluteFilePath("components");
    }

    QString ManagedComponentFile(const QString &component, const QString &name) {
        const QDir dir(ManagedComponentsRoot() + "/" + component);
        QFile manifest(dir.filePath("current.json"));
        if (!manifest.open(QIODevice::ReadOnly)) return {};
        const auto revision = QJsonDocument::fromJson(manifest.read(8192)).object().value("revision").toString();
        static const QRegularExpression validRevision(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        if (!validRevision.match(revision).hasMatch()) return {};
        const QFileInfo info(dir.filePath(revision + "/" + name));
        return info.isFile() && info.size() > 0 ? info.absoluteFilePath() : QString();
    }

    QString ManagedCoreProgram(const QString &core) {
#ifdef Q_OS_WIN
        const auto name = core + ".exe";
#else
        const auto &name = core;
#endif
        const auto file = ManagedComponentFile(core, name);
        return !file.isEmpty() && QFileInfo(file).isExecutable() ? file : QString();
    }

    QString UpdateHelperProgram() {
        const QDir dir(QApplication::applicationDirPath());
#ifdef Q_OS_WIN
        const QStringList names{"updater.exe"};
#else
        const QStringList names{"updater", "launcher"};
#endif
        for (const auto &name : names) {
            const QFileInfo helper(dir.filePath(name));
            if (helper.isFile() && helper.isExecutable()) return helper.absoluteFilePath();
        }
        return {};
    }
}
