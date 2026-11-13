#pragma once

#include <memory>
#include <QProcess>
#include <QTemporaryFile>

namespace NekoGui_sys {
    class ExternalProcess : public QProcess {
    public:
        QString tag;
        QString program;
        QStringList arguments;
        QStringList env;
        std::shared_ptr<QTemporaryFile> temporaryConfig;

        bool reportFailure = false;

        ExternalProcess();
        ~ExternalProcess();

        // start & kill is one time

        void Start();

        QString CheckConfiguration(const QStringList &checkArguments, const QList<QPair<QString, QString>> &assets = {});
        QString WaitUntilReady(const QString &host, int port, bool socks = false);

        QString StartChecked(const QStringList &checkArguments, const QList<QPair<QString, QString>> &assets, int port);

        void Kill();

    protected:
        bool started = false;
        bool killed = false;
        bool crashed = false;
    };

    inline QAtomicInt logCounter;
} // namespace NekoGui_sys
