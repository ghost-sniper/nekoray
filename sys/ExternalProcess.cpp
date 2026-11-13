#include "ExternalProcess.hpp"
#include "main/NekoGui.hpp"

#include <QTimer>
#include <QDir>
#include <QApplication>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkProxy>

namespace NekoGui_sys {

    ExternalProcess::ExternalProcess() : QProcess() {
        this->env = QProcessEnvironment::systemEnvironment().toStringList();
    }

    ExternalProcess::~ExternalProcess() { Kill(); }

    void ExternalProcess::Start() {
        if (started) return;
        started = true;

        connect(this, &QProcess::readyReadStandardOutput, this, [&]() {
            auto log = readAllStandardOutput();
            if (logCounter.fetchAndAddRelaxed(log.count("\n")) > NekoGui::dataStore->max_log_line) return;
            MW_show_log_ext_vt100(log);
        });
        connect(this, &QProcess::readyReadStandardError, this, [&]() {
            MW_show_log_ext_vt100(readAllStandardError().trimmed());
        });
        connect(this, &QProcess::errorOccurred, this, [&](QProcess::ProcessError) {
            if (!killed) {
                crashed = true;
                MW_show_log_ext(tag, "errorOccurred:" + errorString());
                if (reportFailure) MW_dialog_message("ExternalProcess", "Crashed");
            }
        });
        connect(this, &QProcess::stateChanged, this, [&](QProcess::ProcessState state) {
            if (state == QProcess::NotRunning) {
                if (killed) { // 用户命令退出
                    MW_show_log_ext(tag, "External core stopped");
                } else if (!crashed) { // 异常退出
                    crashed = true;
                    MW_show_log_ext(tag, "[Error] Program exited accidentally: " + errorString());
                    if (reportFailure) MW_dialog_message("ExternalProcess", "Crashed");
                }
            }
        });
        MW_show_log_ext(tag, "External core starting: " + program + " " + arguments.join(" "));

        QProcess::setEnvironment(env);
        QProcess::start(program, arguments);
    }

    QString ExternalProcess::CheckConfiguration(const QStringList &checkArguments, const QList<QPair<QString, QString>> &assets) {
        QDir directory(workingDirectory());
        if (!QDir().mkpath(directory.absolutePath()))
            return QObject::tr("Cannot create the core data directory.");
        for (const auto &asset : assets) {
            QFile source(asset.first);
            if (!source.open(QIODevice::ReadOnly)) return source.errorString();
            const auto destination = directory.filePath(asset.second);
            QFile previous(destination);
            if (previous.open(QIODevice::ReadOnly) && previous.size() == source.size()) {
                QCryptographicHash oldHash(QCryptographicHash::Sha256), newHash(QCryptographicHash::Sha256);
                if (oldHash.addData(&previous) && newHash.addData(&source) && oldHash.result() == newHash.result())
                    continue;
                if (!source.seek(0)) return source.errorString();
            }
            previous.close();
            QSaveFile output(destination);
            if (!output.open(QIODevice::WriteOnly)) return output.errorString();
            while (!source.atEnd()) {
                const auto chunk = source.read(64 * 1024);
                if (chunk.isEmpty()) return source.errorString();
                if (output.write(chunk) != chunk.size()) return output.errorString();
            }
            if (!output.commit()) return output.errorString();
        }

        QProcess check;
        check.setWorkingDirectory(workingDirectory());
        check.setEnvironment(env);
        check.setProcessChannelMode(QProcess::MergedChannels);
        check.start(program, checkArguments);
        if (!check.waitForStarted(3000)) return check.errorString();
        if (!check.waitForFinished(8000)) {
            check.kill();
            check.waitForFinished();
            return QObject::tr("The external core configuration check timed out.");
        }
        if (check.exitStatus() != QProcess::NormalExit || check.exitCode() != 0) {
            return QObject::tr("The external core rejected the configuration:\n%1")
                .arg(QString::fromUtf8(check.readAll().left(8192)).trimmed());
        }

        return {};
    }

    QString ExternalProcess::StartChecked(const QStringList &checkArguments, const QList<QPair<QString, QString>> &assets, int port) {
        const auto error = CheckConfiguration(checkArguments, assets);
        if (!error.isEmpty()) return error;
        Start();
        return WaitUntilReady(QStringLiteral("127.0.0.1"), port, true);
    }

    QString ExternalProcess::WaitUntilReady(const QString &host, int port, bool socks) {
        if (!waitForStarted(3000)) return errorString();
        QElapsedTimer deadline;
        deadline.start();
        while (state() == QProcess::Running && deadline.elapsed() < 5000) {
            QTcpSocket socket;
            socket.setProxy(QNetworkProxy::NoProxy);
            socket.connectToHost(host, port);
            if (socket.waitForConnected(100)) {
                if (!socks) return {};
                socket.write(QByteArray::fromHex("050100"));
                socket.waitForBytesWritten(100);
                if (socket.bytesAvailable() < 2) socket.waitForReadyRead(100);
                if (socket.read(2) == QByteArray::fromHex("0500")) return {};
            }
            waitForReadyRead(50);
        }
        Kill();
        return QObject::tr("%1 did not open its local listener. See the core log.").arg(tag);
    }

    void ExternalProcess::Kill() {
        if (killed) return;
        killed = true;

        if (state() == QProcess::NotRunning) return;
#ifndef Q_OS_WIN
        terminate();
        if (waitForFinished(1000)) return;
#endif
        kill();
        waitForFinished(1000);
    }

} // namespace NekoGui_sys
