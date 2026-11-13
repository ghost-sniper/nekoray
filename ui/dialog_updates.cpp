#include "dialog_updates.h"
#include "main/ManagedComponents.hpp"
#include "main/NekoGui.hpp"

#include <QDesktopServices>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkProxyFactory>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#ifdef Q_OS_LINUX
#include <QLibrary>
#endif

namespace {
    const QUrl forkReleases(QStringLiteral("https://github.com/ghost-sniper/nekoray/releases"));

    class ProxyResolver : public QObject {
        Q_OBJECT
    signals:
        void resolved(const QByteArray &message, int generation);
    };

    QJsonObject systemProxy(const QUrl &target) {
#ifdef Q_OS_LINUX
        // The bundled Qt can be built without libproxy. Use the desktop resolver
        // directly when it is installed, preserving per-URL bypass and PAC rules.
        static QLibrary library(QStringLiteral("proxy"), 1);
        using Factory = void *;
        static const auto create = reinterpret_cast<Factory (*)()>(library.resolve("px_proxy_factory_new"));
        static const auto destroy = reinterpret_cast<void (*)(Factory)>(library.resolve("px_proxy_factory_free"));
        static const auto query = reinterpret_cast<char **(*)(Factory, const char *)>(library.resolve("px_proxy_factory_get_proxies"));
        static const auto release = reinterpret_cast<void (*)(char **)>(library.resolve("px_proxy_factory_free_proxies"));
        if (create && destroy && query && release) {
            if (const auto factory = create()) {
                auto **addresses = query(factory, target.toEncoded().constData());
                const auto address = addresses && addresses[0] ? QString::fromUtf8(addresses[0]) : QString();
                if (addresses) release(addresses);
                destroy(factory);
                if (!address.isEmpty()) {
                    QUrl proxy(address);
                    if (proxy.scheme() == "direct") return {{"action", "proxy"}};
                    if (proxy.scheme() == "socks") proxy.setScheme("socks5");
                    return {{"action", "proxy"}, {"proxy", proxy.toString(QUrl::FullyEncoded)}};
                }
            }
        }
#endif
        const auto choices = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(target));
        for (const auto &choice : choices) {
            if (choice.type() == QNetworkProxy::NoProxy) break;
            if (choice.type() != QNetworkProxy::HttpProxy && choice.type() != QNetworkProxy::HttpCachingProxy &&
                choice.type() != QNetworkProxy::Socks5Proxy) continue;
            QUrl proxy;
            proxy.setScheme(choice.type() == QNetworkProxy::Socks5Proxy ? "socks5" : "http");
            proxy.setHost(choice.hostName());
            proxy.setPort(choice.port());
            proxy.setUserName(choice.user());
            proxy.setPassword(choice.password());
            return {{"action", "proxy"}, {"proxy", proxy.toString(QUrl::FullyEncoded)}};
        }
#ifdef Q_OS_LINUX
        return {{"action", "environment"}};
#else
        return {{"action", "proxy"}};
#endif
    }
}

UpdateDialog::UpdateDialog(const QString &root, bool portable, std::function<bool(const QString &)> canApply,
                           std::function<void()> stopProfile, QWidget *parent)
    : QDialog(parent), root(root), helper(NekoGui::UpdateHelperProgram()), portable(portable),
      canApply(std::move(canApply)), stopProfile(std::move(stopProfile)), process(new QProcess(this)),
      proxyThread(new QThread) {
    setWindowTitle(tr("Update Center"));
    setWindowModality(Qt::ApplicationModal);
    resize(1020, 410);

    rows = {{"nekoray", tr("Nekoray")}, {"sing-box", tr("Official sing-box")}, {"xray", tr("Xray")},
            {"mihomo", tr("Mihomo")}, {"geoip", tr("GeoIP")}, {"geosite", tr("GeoSite")}};
    auto *layout = new QVBoxLayout(this);
    table = new QTableWidget(rows.size(), 5, this);
    table->setHorizontalHeaderLabels({tr("Component"), tr("Installed"), tr("Available"), tr("Status"), tr("Action")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setColumnWidth(0, 160);
    table->setColumnWidth(1, 190);
    table->setColumnWidth(2, 190);
    table->setColumnWidth(4, 112);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(table);

    for (int index = 0; index < rows.size(); ++index) {
        auto &row = rows[index];
        auto *name = new QTableWidgetItem(row.title);
        name->setCheckState(Qt::Unchecked);
        table->setItem(index, 0, name);
        for (int column = 1; column < 4; ++column)
            table->setItem(index, column, new QTableWidgetItem);
        row.action = new QPushButton(table);
        table->setCellWidget(index, 4, row.action);
        connect(row.action, &QPushButton::clicked, this, [=] { performAction(index); });
    }
    rows[0].installed = NKR_VERSION;

    status = new QLabel(this);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *buttons = new QHBoxLayout;
    checkButton = new QPushButton(tr("Check for updates"), this);
    updateButton = new QPushButton(tr("Update selected"), this);
    closeButton = new QPushButton(tr("Close"), this);
    buttons->addWidget(checkButton);
    buttons->addWidget(updateButton);
    buttons->addStretch();
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    connect(checkButton, &QPushButton::clicked, this, &UpdateDialog::check);
    connect(updateButton, &QPushButton::clicked, this, &UpdateDialog::updateSelected);
    connect(closeButton, &QPushButton::clicked, this, &UpdateDialog::reject);
    connect(table, &QTableWidget::itemChanged, this, [this] { refresh(); });
    connect(process, &QProcess::readyReadStandardOutput, this, &UpdateDialog::readEvents);
    auto *resolver = new ProxyResolver;
    resolver->moveToThread(proxyThread);
    connect(proxyThread, &QThread::finished, resolver, &QObject::deleteLater);
    connect(proxyThread, &QThread::finished, proxyThread, &QObject::deleteLater);
    connect(this, &UpdateDialog::proxyRequested, resolver, [resolver](int id, const QString &url, int requestGeneration) {
        if (QThread::currentThread()->isInterruptionRequested()) return;
        auto response = systemProxy(QUrl(url));
        response.insert("id", id);
        const auto message = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
        emit resolver->resolved(message, requestGeneration);
    });
    connect(resolver, &ProxyResolver::resolved, this, [this](const QByteArray &message, int requestGeneration) {
        if (requestGeneration == generation && !cancelling && process->state() == QProcess::Running)
            process->write(message);
    }, Qt::QueuedConnection);
    proxyThread->start();
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        diagnostics += process->readAllStandardError();
        if (diagnostics.size() > 4096) diagnostics = diagnostics.right(4096);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &UpdateDialog::finish);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            diagnostics = process->errorString().toUtf8();
            finish(1, QProcess::CrashExit);
        }
    });
    QFile lastError(root + "/nekoray/last-error");
    if (lastError.open(QIODevice::ReadOnly))
        status->setText(tr("Previous Nekoray update failed: %1").arg(QString::fromUtf8(lastError.read(4096))));
    if (helper.isEmpty()) status->setText(tr("The update helper is missing from the application directory."));
    refresh();
    if (!helper.isEmpty()) QTimer::singleShot(0, this, &UpdateDialog::check);
}

UpdateDialog::~UpdateDialog() {
    proxyThread->requestInterruption();
    proxyThread->quit();
}

void UpdateDialog::reject() {
    if (operation == "install") {
        cancelDownload();
        return;
    }
    if (!operation.isEmpty() && operation != "check") return;
    if (process->state() != QProcess::NotRunning) {
        disconnect(process, nullptr, this, nullptr);
        process->kill();
        process->waitForFinished(500);
    }
    QDialog::reject();
}

void UpdateDialog::check() {
    QList<int> indexes;
    for (int index = 0; index < rows.size(); ++index) {
        rows[index].available.clear();
        rows[index].revision.clear();
        rows[index].error.clear();
        indexes.append(index);
    }
    run("check", indexes);
}

void UpdateDialog::updateSelected() {
    QList<int> indexes;
    for (int index = 0; index < rows.size(); ++index) {
        if (table->item(index, 0)->flags().testFlag(Qt::ItemIsUserCheckable) &&
            table->item(index, 0)->checkState() == Qt::Checked) indexes.append(index);
    }
    if (!indexes.isEmpty()) run("install", indexes);
}

void UpdateDialog::run(const QString &command, const QList<int> &indexes) {
    if (helper.isEmpty() || !operation.isEmpty()) return;
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto data = NekoGui::dataStore;
    environment.insert("NEKORAY_UPDATE_CONTROL", "1");
    process->setProcessEnvironment(environment);
    QStringList arguments{"components", command, root, NKR_VERSION, data->check_include_pre ? "true" : "false"};
    operation = command;
    ++generation;
    cancelling = false;
    if (command == "install") status->clear();
    diagnostics.clear();
    pending.clear();
    autoActivate.clear();
    for (int index : indexes) {
        auto &row = rows[index];
        pending.append(row.id);
        row.state = command == "check" ? "checking" : "working";
        row.error.clear();
        if (command != "check") arguments.append(row.id + "=" + row.revision);
    }
    refresh();
    process->start(helper, arguments);
}

void UpdateDialog::readEvents() {
    while (process->canReadLine()) {
        const auto event = QJsonDocument::fromJson(process->readLine()).object();
        if (event.value("type").toString() == "proxy") {
            if (!cancelling)
                emit proxyRequested(event.value("request_id").toInt(), event.value("url").toString(), generation);
            continue;
        }
        const auto id = event.value("id").toString();
        for (auto &row : rows) {
            if (row.id != id) continue;
            row.state = event.value("state").toString();
            if (event.contains("installed")) row.installed = event.value("installed").toString();
            if (event.contains("available")) row.available = event.value("available").toString();
            if (event.contains("revision")) row.revision = event.value("revision").toString();
            row.error = event.value("error").toString();
            if (row.state == "downloading") {
                row.error = tr("Downloading %1%").arg(event.value("progress").toInt());
            } else if (row.state != "working") {
                pending.removeAll(id);
                if (operation == "install" && row.state == "staged" && id != "nekoray") autoActivate.append(id);
            }
            break;
        }
    }
    refresh();
}

void UpdateDialog::cancelDownload() {
    if (operation != "install" || cancelling) return;
    cancelling = true;
    process->write("{\"action\":\"cancel\"}\n");
    process->closeWriteChannel();
    refresh();
}

void UpdateDialog::finish(int code, QProcess::ExitStatus exitStatus) {
    if (operation.isEmpty()) return;
    readEvents();
    const bool cancelled = cancelling;
    if (!pending.isEmpty()) {
        auto error = QString::fromUtf8(diagnostics).trimmed();
        if (error.isEmpty()) error = tr("The update helper exited without a result.");
        for (auto &row : rows) {
            if (!pending.contains(row.id)) continue;
            row.state = cancelled ? "cancelled" : "error";
            row.error = cancelled ? QString() : error;
        }
    }
    if (cancelled) {
        status->setText(tr("Download cancelled."));
    } else if (code != 0 || exitStatus != QProcess::NormalExit) {
        status->setText(tr("The update helper did not finish successfully."));
    }
    operation.clear();
    cancelling = false;
    QList<int> activate;
    for (int index = 0; index < rows.size(); ++index)
        if (autoActivate.contains(rows[index].id) && canApply(rows[index].id)) activate.append(index);
    refresh();
    if (!activate.isEmpty()) run("activate", activate);
}

void UpdateDialog::performAction(int index) {
    const auto row = rows[index];
    if (row.id == "nekoray" && (!portable || helper.isEmpty())) {
        QDesktopServices::openUrl(forkReleases);
    } else if (row.state == "staged" && row.id == "nekoray") {
        if (!canApply(row.id)) {
            status->setText(tr("Wait for the running tests or profile transition before restarting."));
            return;
        }
        if (QMessageBox::question(this, tr("Restart Nekoray"), tr("Restart Nekoray to apply the downloaded update?")) == QMessageBox::Yes) {
            restartRevision = row.revision;
            accept();
        }
    } else if (row.state == "staged") {
        if (canApply(row.id)) {
            run("activate", {index});
        } else if (NekoGui::dataStore->started_id >= 0 &&
                   QMessageBox::question(this, tr("Stop profile"),
                                         tr("Stop the running profile before applying this update?")) == QMessageBox::Yes) {
            stopProfile();
            status->setText(tr("Apply the update after the profile has stopped."));
        } else {
            status->setText(tr("Wait for the running profile or tests to stop, then apply the update."));
        }
    } else if (row.state == "available" || row.state == "cancelled" || (row.state == "error" && !row.revision.isEmpty())) {
        run("install", {index});
    } else if (row.state == "error") {
        check();
    }
}

void UpdateDialog::refresh() {
    QSignalBlocker blocker(table);
    const bool busy = !operation.isEmpty();
    bool selected = false;
    for (int index = 0; index < rows.size(); ++index) {
        const auto &row = rows[index];
        const bool appLink = row.id == "nekoray" && (!portable || helper.isEmpty());
        const bool installable = !appLink && (row.state == "available" || row.state == "cancelled" ||
                                              (row.state == "error" && !row.revision.isEmpty()));
        auto *name = table->item(index, 0);
        name->setFlags(Qt::ItemIsEnabled | (installable && !busy ? Qt::ItemIsUserCheckable : Qt::NoItemFlags));
        if (!installable && !busy) name->setCheckState(Qt::Unchecked);
        selected = selected || (installable && name->checkState() == Qt::Checked);

        auto installed = row.installed;
        if (installed.isEmpty() && (row.id == "geoip" || row.id == "geosite") &&
            (!NekoGui::FindCoreAsset(row.id + ".dat").isEmpty() || !NekoGui::FindCoreAsset(row.id + ".db").isEmpty()))
            installed = tr("Bundled or system");
        if (installed.isEmpty()) installed = tr("Not installed");
        table->item(index, 1)->setText(installed);
        table->item(index, 1)->setToolTip(installed);
        table->item(index, 2)->setText(row.available);
        table->item(index, 2)->setToolTip(row.available);

        QString state;
        QString action;
        if (row.state == "checking") state = tr("Checking…");
        else if (row.state == "working") state = tr("Preparing…");
        else if (row.state == "no-release") state = tr("No release yet");
        else if (row.state == "unsupported") state = tr("No compatible package");
        else if (row.state == "current") state = tr("Up to date");
        else if (row.state == "installed") state = tr("Updated");
        else if (row.state == "available") { state = tr("Update available"); action = tr("Download"); }
        else if (row.state == "cancelled") { state = tr("Download cancelled."); action = tr("Retry"); }
        else if (row.state == "staged") {
            state = tr("Ready to apply");
            action = row.id == "nekoray" ? tr("Restart") : tr("Apply");
        } else if (row.state == "error") {
            state = row.error;
            action = tr("Retry");
        } else if (row.state == "downloading") state = row.error;
        if (cancelling && pending.contains(row.id)) state = tr("Cancelling…");
        table->item(index, 3)->setText(state);
        table->item(index, 3)->setToolTip(state);
        if (appLink) action = tr("Releases");
        row.action->setText(action);
        row.action->setVisible(!action.isEmpty());
        row.action->setEnabled(!busy);
    }
    checkButton->setEnabled(!busy && !helper.isEmpty());
    updateButton->setEnabled(!busy && selected && !helper.isEmpty());
    closeButton->setText(operation == "install" ? tr("Cancel download") : tr("Close"));
    closeButton->setEnabled(!busy || operation == "check" || (operation == "install" && !cancelling));
}

#include "dialog_updates.moc"
