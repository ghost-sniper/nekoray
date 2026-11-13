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

    static QJsonObject managedManifest(const QString &component) {
        QFile manifest(ManagedComponentsRoot() + "/" + component + "/current.json");
        if (!manifest.open(QIODevice::ReadOnly)) return {};
        const auto metadata = QJsonDocument::fromJson(manifest.read(8192)).object();
        const auto revision = metadata.value("revision").toString();
        static const QRegularExpression validRevision(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        return validRevision.match(revision).hasMatch() ? metadata : QJsonObject();
    }

    QString ManagedComponentVersion(const QString &component) {
        return managedManifest(component).value("version").toString();
    }

    QString ManagedComponentFile(const QString &component, const QString &name) {
        const auto revision = managedManifest(component).value("revision").toString();
        if (revision.isEmpty()) return {};
        const QFileInfo info(ManagedComponentsRoot() + "/" + component + "/" + revision + "/" + name);
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

    QProcessEnvironment SingBoxEnvironment(const QString &program) {
        auto environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_LINUX
        const auto directory = QFileInfo(program).absolutePath();
        const auto inherited = environment.value("LD_LIBRARY_PATH");
        environment.insert("LD_LIBRARY_PATH", inherited.isEmpty() ? directory : directory + ":" + inherited);
#endif
        return environment;
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
