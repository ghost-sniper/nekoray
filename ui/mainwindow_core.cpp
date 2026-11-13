#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "ui/dialog_updates.h"
#include "main/ManagedComponents.hpp"
#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "fmt/CustomBean.hpp"
#include "3rdparty/qv2ray/v2/components/proxy/QvProxyConfigurator.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <vector>

namespace {
using Process = NekoGui_sys::ExternalProcess;
using Processes = std::vector<std::unique_ptr<Process>>;
using BuildResult = NekoGui::BuildConfigResult;

// Accessed only on DS_cores.
Processes profileProcesses;

void stopProcesses(Processes &processes) {
    while (!processes.empty()) processes.pop_back();
}

QString startExternal(const BuildResult &result, Processes &processes) {
    for (const auto &external : result.extRs) {
        auto process = std::make_unique<Process>();
        process->tag = external->tag;
        process->program = external->program;
        process->arguments = external->arguments;
        process->env = external->env;
        process->temporaryConfig = external->temporaryConfig;
        process->setWorkingDirectory(external->workingDirectory);
        QString error;
        if (external->readyPort > 0) {
            error = process->StartChecked(external->checkArguments, external->assets, external->readyPort);
        } else {
            process->Start();
            if (!process->waitForStarted(3000)) error = process->errorString();
        }
        if (!error.isEmpty()) return error;
        processes.push_back(std::move(process));
    }
    return {};
}

struct CoreEndpoint {
    QUrl url;
    QByteArray secret;
};

CoreEndpoint configureClashApi(QJsonObject &config) {
    auto experimental = config.value("experimental").toObject();
    auto api = experimental.value("clash_api").toObject();
    auto controller = api.value("external_controller").toString();
    if (controller.isEmpty()) {
        const auto port = MkPort();
        if (port <= 0) return {};
        controller = QStringLiteral("127.0.0.1:%1").arg(port);
        api["external_controller"] = controller;
        api["secret"] = GetRandomString(32);
    }
    QUrl url("http://" + controller);
    if (url.host() == "0.0.0.0") url.setHost("127.0.0.1");
    if (url.host() == "::") url.setHost("::1");
    if (url.host().compare("localhost", Qt::CaseInsensitive) == 0) url.setHost("127.0.0.1");
    const QHostAddress host(url.host());
    if (!url.isValid() || url.port() <= 0 ||
        (!host.isLoopback() && !QNetworkInterface::allAddresses().contains(host))) return {};
    experimental["clash_api"] = api;
    config["experimental"] = experimental;
    return {url, api["secret"].toString().toUtf8()};
}

QJsonObject mixedInbound(const QString &tag, const QString &address, int port) {
    return {{"type", "mixed"}, {"tag", tag}, {"listen", address}, {"listen_port", port}};
}

QString configureNative(BuildResult &result, bool tun, bool testing) {
    const auto rawInbounds = result.coreConfig.value("inbounds");
    if (!rawInbounds.isUndefined() && !rawInbounds.isArray())
        return QObject::tr("The inbounds field must be an array.");
    auto inbounds = rawInbounds.toArray();
    bool hasMixed = false;
    bool hasTun = false;
    for (const auto &item : inbounds) {
        const auto inbound = item.toObject();
        hasTun |= inbound["type"] == "tun";
        hasMixed |= inbound["type"] == "mixed" &&
                    inbound["listen_port"].toInt() == NekoGui::dataStore->inbound_socks_port;
    }
    if (hasTun && testing)
        return QObject::tr("URL testing is unavailable for a sing-box configuration with its own TUN.");
    if (tun)
        return QObject::tr("Disable Nekoray TUN before starting a full sing-box configuration. You can configure TUN in its JSON.");
    if (hasTun && !NekoGui::IsAdmin())
        return QObject::tr("This sing-box configuration uses TUN. Grant TUN permissions before starting it.");
    result.requiresNoTun = true;
    if (!testing && !hasMixed && IsValidPort(NekoGui::dataStore->inbound_socks_port)) {
        auto inbound = mixedInbound("nekoray-mixed-in", NekoGui::dataStore->inbound_address,
                                    NekoGui::dataStore->inbound_socks_port);
        const auto auth = NekoGui::dataStore->inbound_auth;
        if (auth->NeedAuth()) {
            inbound["users"] = QJsonArray{QJsonObject{{"username", auth->username}, {"password", auth->password}}};
        }
        inbounds.append(inbound);
        result.coreConfig["inbounds"] = inbounds;
    }
    return {};
}

QString prepareOfficial(Process &process, BuildResult &result, const QString &program) {
    if (result.migrateLegacyConfig) {
        const auto error = NekoGui::PrepareSingBoxConfig(result.coreConfig, program);
        if (!error.isEmpty()) return error;
    }
    if (!QDir().mkpath("temp")) return QObject::tr("Could not create the temporary configuration directory.");
    process.temporaryConfig = std::make_shared<QTemporaryFile>(QDir::current().absoluteFilePath("temp/sing-box-XXXXXX.json"));
    auto &file = *process.temporaryConfig;
    if (!file.open()) return file.errorString();
    const auto bytes = QJsonDocument(result.coreConfig).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush()) return file.errorString();
    file.close();

    process.tag = "sing-box";
    process.program = program;
    process.env = NekoGui::SingBoxEnvironment(program).toStringList();
    process.setWorkingDirectory(QDir::currentPath());
    process.arguments = QStringList{"--disable-color", "run", "-c", file.fileName()};
    return process.CheckConfiguration({"--disable-color", "check", "-c", file.fileName()});
}

QNetworkRequest clashRequest(QUrl url, const QByteArray &secret, const QString &path) {
    url.setPath(path);
    QNetworkRequest request(url);
    if (!secret.isEmpty()) request.setRawHeader("Authorization", "Bearer " + secret);
    return request;
}

constexpr int statsTimeoutMs = 1500;
}

void MainWindow::setup_core() {
    coreApi = new QNetworkAccessManager(this);
    coreApi->setProxy(QNetworkProxy::NoProxy);
    statsTimer = new QTimer(this);
    connect(statsTimer, &QTimer::timeout, this, &MainWindow::poll_core_stats);
    statsTimer->start(1000);
}

void MainWindow::poll_core_stats() {
    const auto settings = NekoGui::dataStore;
    const int interval = std::clamp(settings->traffic_loop_interval, 500, 5000);
    if (statsTimer->interval() != interval) statsTimer->setInterval(interval);
    if (!running || profileTransition || statsReply ||
        (settings->traffic_loop_interval == 0 && !settings->connection_statistics)) return;
    auto reply = coreApi->get(clashRequest(coreApiUrl, coreApiSecret, "/connections"));
    statsReply = reply;
    QTimer::singleShot(statsTimeoutMs, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation = profileGeneration] {
        if (statsReply == reply) statsReply = nullptr;
        reply->deleteLater();
        if (generation != profileGeneration || !running || reply->error() != QNetworkReply::NoError) return;
        const auto snapshot = QJsonDocument::fromJson(reply->readAll()).object();
        if (update_traffic_totals(snapshot) && NekoGui::dataStore->traffic_loop_interval > 0) {
            refresh_status(tr("Traffic: %1").arg(running->traffic_data->DisplaySpeed()));
            refresh_proxy_list(running->id);
        }
        if (!NekoGui::dataStore->connection_statistics) return;
        QJsonArray connections;
        for (const auto &item : snapshot["connections"].toArray()) {
            const auto connection = item.toObject();
            const auto metadata = connection["metadata"].toObject();
            const auto chains = connection["chains"].toArray();
            const auto start = QDateTime::fromString(connection["start"].toString(), Qt::ISODate);
            auto ip = metadata["destinationIP"].toString();
            auto host = metadata["host"].toString();
            auto port = metadata["destinationPort"].toVariant().toString();
            if (host.isEmpty()) host = ip;
            connections.append(QJsonObject{{"Tag", chains.isEmpty() ? QString() : chains.last().toString()},
                                            {"Dest", WrapIPV6Host(host) + ":" + port}, {"RDest", ip},
                                            {"Start", static_cast<double>(start.toSecsSinceEpoch())}});
        }
        refresh_connection_list(connections);
    });
}

bool MainWindow::update_traffic_totals(const QJsonObject &snapshot) {
    const auto upload = snapshot.value("uploadTotal");
    const auto download = snapshot.value("downloadTotal");
    if (!upload.isDouble() || !download.isDouble()) return false;
    const auto uploadTotal = upload.toVariant().toLongLong();
    const auto downloadTotal = download.toVariant().toLongLong();
    if (uploadTotal < previousUpload || downloadTotal < previousDownload) return false;
    const auto elapsed = std::max<qint64>(1, statsClock.restart());
    if (NekoGui::dataStore->traffic_loop_interval > 0) {
        auto data = running->traffic_data;
        const auto uploaded = uploadTotal - previousUpload;
        const auto downloaded = downloadTotal - previousDownload;
        data->uplink += uploaded;
        data->downlink += downloaded;
        data->uplink_rate = uploaded * 1000 / elapsed;
        data->downlink_rate = downloaded * 1000 / elapsed;
    }
    previousUpload = uploadTotal;
    previousDownload = downloadTotal;
    return true;
}

QNetworkRequest MainWindow::final_stats_request() {
    if (statsReply) statsReply->abort();
    if (running && NekoGui::dataStore->traffic_loop_interval > 0)
        return clashRequest(coreApiUrl, coreApiSecret, "/connections");
    return {};
}

void MainWindow::save_traffic_and_stop_processes(const QNetworkRequest &request) {
    QJsonObject snapshot;
    if (!request.url().isEmpty()) {
        QNetworkAccessManager manager;
        manager.setProxy(QNetworkProxy::NoProxy);
        auto reply = manager.get(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(statsTimeoutMs, reply, &QNetworkReply::abort);
        if (!reply->isFinished()) loop.exec();
        if (reply->error() == QNetworkReply::NoError)
            snapshot = QJsonDocument::fromJson(reply->readAll()).object();
    }

    // Commit the final totals on the UI thread before releasing the running core.
    QMetaObject::invokeMethod(this, [this, &request, &snapshot] {
        if (!running) return;
        if (!request.url().isEmpty() && !update_traffic_totals(snapshot))
            show_log_impl(tr("Could not read final traffic totals from sing-box."));
        if (NekoGui::profileManager->GetProfile(running->id) == running) running->Save();
    }, Qt::BlockingQueuedConnection);
    stopProcesses(profileProcesses);
}

void MainWindow::clear_running_profile() {
    ++profileGeneration;
    if (statsReply) statsReply->abort();
    if (running) {
        if (NekoGui::dataStore->spmode_system_proxy) ClearSystemProxy();
        const auto id = running->id;
        running.reset();
        refresh_proxy_list(id);
    }
    runningRequiresNoTun = false;
    previousUpload = previousDownload = 0;
    NekoGui::dataStore->UpdateStartedId(-1);
    refresh_connection_list({});
    refresh_status("STOP");
}

void MainWindow::finish_profile_transition() {
    profileTransition = false;
    if (exitPending) QTimer::singleShot(0, this, &MainWindow::on_menu_exit_triggered);
}

void MainWindow::restore_running_tun() {
    auto settings = NekoGui::dataStore;
    if (!running || settings->spmode_vpn == runningTun) return;
    settings->spmode_vpn = runningTun;
    settings->remember_spmode.removeAll("vpn");
    if (runningTun && settings->remember_enable) settings->remember_spmode.append("vpn");
    settings->Save();
    refresh_status();
}

void MainWindow::neko_start(int id) {
    const auto settings = NekoGui::dataStore;
    if (settings->prepare_exit || updateDialogOpen || profileTransition) return;

    const auto selected = get_now_selected_list();
    const auto ent = id < 0 && !selected.isEmpty() ? selected.first() : NekoGui::profileManager->GetProfile(id);
    if (!ent) return;
    if (select_mode) {
        emit profile_selected(ent->id);
        select_mode = false;
        refresh_status();
        return;
    }
    const auto group = NekoGui::profileManager->GetGroup(ent->gid);
    if (!group || group->archive) return;

    auto missingCore = [this](const QString &name) {
        if (QMessageBox::question(this, tr("Core not installed"),
                                  tr("%1 is not installed. Open the update center?").arg(name)) == QMessageBox::Yes)
            CheckUpdate();
    };
    const auto program = NekoGui::ManagedCoreProgram("sing-box");
    if (program.isEmpty()) {
        restore_running_tun();
        missingCore("sing-box");
        return;
    }
    auto result = NekoGui::BuildConfig(ent, false, false);
    if (!result->missingManagedCore.isEmpty()) {
        restore_running_tun();
        missingCore(result->missingManagedCore);
        return;
    }
    if (result->error.isEmpty() && result->nativeSingBox)
        result->error = configureNative(*result, settings->spmode_vpn, false);
    if (!result->error.isEmpty()) {
        restore_running_tun();
        MessageBoxWarning(software_name, result->error);
        return;
    }
    const auto endpoint = configureClashApi(result->coreConfig);
    if (endpoint.url.isEmpty()) {
        restore_running_tun();
        MessageBoxWarning(software_name, tr("Set the sing-box Clash API controller to a local IP address and port."));
        return;
    }

    QString mixedAddress;
    const auto mixedPort = settings->inbound_socks_port;
    for (const auto &value : result->coreConfig.value("inbounds").toArray()) {
        const auto inbound = value.toObject();
        if (inbound.value("type") != "mixed" || inbound.value("listen_port").toInt() != mixedPort) continue;
        mixedAddress = inbound.value("listen").toString("127.0.0.1");
        if (mixedAddress == "0.0.0.0") mixedAddress = "127.0.0.1";
        if (mixedAddress == "::") mixedAddress = "::1";
        break;
    }
    profileTransition = true;
    const auto finalStats = final_stats_request();
    MW_show_log(">>>>>>>> " + tr("Starting profile %1").arg(ent->bean->DisplayTypeAndName()));
    runOnUiThread([=] {
        auto process = std::make_unique<Process>();
        auto error = prepareOfficial(*process, *result, program);
        const bool checked = error.isEmpty();
        if (checked) {
            if (!profileProcesses.empty()) save_traffic_and_stop_processes(finalStats);
            error = startExternal(*result, profileProcesses);
        }
        if (error.isEmpty()) {
            process->Start();
            error = process->WaitUntilReady(endpoint.url.host(), endpoint.url.port());
            if (error.isEmpty() && !mixedAddress.isEmpty())
                error = process->WaitUntilReady(mixedAddress, mixedPort);
        }
        if (error.isEmpty()) {
            profileProcesses.push_back(std::move(process));
            for (const auto &child : profileProcesses) {
                if (child->state() != QProcess::Running) {
                    error = tr("%1 exited while the profile was starting. See the core log.").arg(child->tag);
                    break;
                }
                child->reportFailure = true;
            }
        }
        const bool oldProcessExited = !checked && std::any_of(profileProcesses.begin(), profileProcesses.end(), [](const auto &child) {
            return child->state() == QProcess::NotRunning;
        });
        if (checked && !error.isEmpty()) stopProcesses(profileProcesses);
        if (oldProcessExited) save_traffic_and_stop_processes(finalStats);
        process.reset();
        runOnUiThread([=] {
            if (!error.isEmpty()) restore_running_tun();
            if (checked || oldProcessExited) clear_running_profile();
            if (error.isEmpty()) {
                coreApiUrl = endpoint.url;
                coreApiSecret = endpoint.secret;
                running = ent;
                runningRequiresNoTun = result->requiresNoTun;
                statsClock.start();
                const auto settings = NekoGui::dataStore;
                runningTun = settings->spmode_vpn;
                settings->UpdateStartedId(ent->id);
                if (settings->spmode_system_proxy && IsValidPort(settings->inbound_socks_port)) {
                    const auto port = settings->inbound_socks_port;
                    SetSystemProxy(port, port);
                }
                refresh_status();
                refresh_proxy_list(ent->id);
            }
            finish_profile_transition();
            if (!error.isEmpty() && !exitPending) MessageBoxWarning(software_name, error);
        });
    }, DS_cores);
}

void MainWindow::neko_stop(bool exitAfterStop) {
    if (profileTransition || !running) return;
    profileTransition = true;
    const auto finalStats = final_stats_request();
    runOnUiThread([this, finalStats, exitAfterStop] {
        save_traffic_and_stop_processes(finalStats);
        runOnUiThread([this, exitAfterStop] {
            clear_running_profile();
            if (exitAfterStop) finish_exit();
            else finish_profile_transition();
        });
    }, DS_cores);
}

namespace {
struct Probe {
    int latency = -1;
    qint64 bytes = 0;
    qint64 elapsed = 0;
    QString error;
    QByteArray body;
};

Probe fetch(const QNetworkProxy &proxy, const QUrl &url, int timeout, const std::atomic_bool &cancel,
            bool download = false, bool captureBody = false) {
    QNetworkAccessManager manager;
    manager.setProxy(proxy);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QElapsedTimer elapsed;
    elapsed.start();
    auto reply = manager.get(request);
    QEventLoop loop;
    QTimer timer;
    timer.start(100);
    Probe result;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (cancel || elapsed.elapsed() >= timeout) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const auto data = reply->readAll();
        result.bytes += data.size();
        if (captureBody && result.body.size() < 4096) result.body += data.left(4096 - result.body.size());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec();
    result.elapsed = std::max<qint64>(1, elapsed.elapsed());
    if (reply->error() == QNetworkReply::NoError) result.latency = result.elapsed;
    else if (!download || result.bytes == 0) result.error = reply->errorString();
    return result;
}

QString testUdp(const Process &process, const std::atomic_bool &cancel) {
    QProcess stun;
    stun.setEnvironment(process.env);
    stun.setWorkingDirectory(process.workingDirectory());
    stun.setProcessChannelMode(QProcess::MergedChannels);
    stun.start(process.program, {"--disable-color", "tools", "stun", "-c", process.temporaryConfig->fileName()});
    if (!stun.waitForStarted(3000)) return stun.errorString();
    QElapsedTimer deadline;
    deadline.start();
    while (!cancel && deadline.elapsed() < 10000) {
        if (stun.waitForFinished(100)) {
            const auto output = QString::fromUtf8(stun.readAll().trimmed().right(1024));
            if (!output.isEmpty()) return output;
            return stun.exitCode() == 0 ? QObject::tr("Available") : QObject::tr("Unavailable");
        }
    }
    stun.kill();
    stun.waitForFinished(1000);
    return QObject::tr("Unavailable");
}

struct TestSettings {
    int mode = 0;
    bool latency = false;
    bool udp = false;
    bool speed = false;
    bool ips = false;
    QUrl latencyUrl;
    QUrl downloadUrl;
    int downloadTimeout = 30000;
};

struct TestResult {
    int latency = -1;
    QString error;
    QString report;
};

TestResult runProfileTest(BuildResult *config, const QString &program, const QString &address, int serverPort,
                          const TestSettings &settings, const std::atomic_bool &cancel) {
    if (settings.mode == 0) {
        if (address.isEmpty() || !IsValidPort(serverPort))
            return {-1, QObject::tr("This profile has no server address for a TCP test."), {}};
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        QElapsedTimer deadline;
        deadline.start();
        QEventLoop loop;
        QTimer timer;
        QObject::connect(&socket, &QTcpSocket::stateChanged, &loop, [&](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::ConnectedState || state == QAbstractSocket::UnconnectedState) loop.quit();
        });
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (cancel || deadline.elapsed() >= 10000) loop.quit();
        });
        socket.connectToHost(address, serverPort);
        if (socket.state() != QAbstractSocket::ConnectedState && socket.state() != QAbstractSocket::UnconnectedState) {
            timer.start(100);
            loop.exec();
        }
        if (socket.state() != QAbstractSocket::ConnectedState) return {-1, socket.errorString(), {}};
        return {static_cast<int>(std::max<qint64>(1, deadline.elapsed())), {}, {}};
    }

    if (!config->error.isEmpty()) return {-1, config->error, {}};
    const int port = MkPort();
    if (port <= 0) return {-1, QObject::tr("Could not allocate a local test port."), {}};
    auto inbounds = config->nativeSingBox ? QJsonArray{} : config->coreConfig["inbounds"].toArray();
    inbounds.append(mixedInbound("nekoray-test-in", "127.0.0.1", port));
    config->coreConfig["inbounds"] = inbounds;
    auto experimental = config->coreConfig["experimental"].toObject();
    experimental.remove("clash_api");
    experimental.remove("cache_file");
    config->coreConfig["experimental"] = experimental;

    Processes children;
    Process process;
    const auto error = prepareOfficial(process, *config, program);
    if (!error.isEmpty()) return {-1, error, {}};
    if (cancel) return {};
    const auto externalError = startExternal(*config, children);
    if (!externalError.isEmpty()) return {-1, externalError, {}};
    process.Start();
    const auto startError = process.WaitUntilReady("127.0.0.1", port);
    if (!startError.isEmpty()) return {-1, startError, {}};

    const QNetworkProxy proxy(QNetworkProxy::HttpProxy, "127.0.0.1", port);
    TestResult result;
    QStringList report;
    if (settings.mode == 1 || settings.latency) {
        const auto response = fetch(proxy, settings.latencyUrl, 10000, cancel);
        result.latency = response.latency;
        result.error = response.error;
        if (settings.mode == 2)
            report << QObject::tr("Latency: %1").arg(response.latency < 0 ? response.error :
                                                      QStringLiteral("%1 ms").arg(response.latency));
    }
    if (settings.udp && !cancel) report << QObject::tr("UDP connectivity: %1").arg(testUdp(process, cancel));
    if (settings.speed && !cancel) {
        const auto response = fetch(proxy, settings.downloadUrl, settings.downloadTimeout, cancel, true);
        report << QObject::tr("Download speed: %1").arg(response.error.isEmpty() ?
            ReadableSize(response.bytes * 1000 / response.elapsed) + "/s" : response.error);
    }
    if (settings.ips && !cancel) {
        const auto response = fetch(proxy, QUrl("https://cp.cloudflare.com/cdn-cgi/trace"), 10000, cancel, false, true);
        QString exitIp = response.error;
        for (const auto &line : response.body.split('\n')) {
            if (line.startsWith("ip=")) exitIp = QString::fromUtf8(line.mid(3)).trimmed();
        }
        report << QObject::tr("Server: %1").arg(address)
               << QObject::tr("Exit IP: %1").arg(exitIp.isEmpty() ? QObject::tr("Unavailable") : exitIp);
    }
    result.report = report.join('\n');
    return result;
}
}

void MainWindow::speedtest_current_group(int mode, bool test_group) {
    if (mode == 114514) {
        if (cancelSpeedtest) *cancelSpeedtest = true;
        return;
    }
    if (updateDialogOpen || NekoGui::dataStore->prepare_exit) return;
    if (speedtesting) {
        MessageBoxWarning(software_name, tr("The last speed test did not exit completely, please wait. If it persists, please restart the program."));
        return;
    }
    auto group = NekoGui::profileManager->CurrentGroup();
    if (!group || group->archive) return;
    const auto profiles = test_group ? group->ProfilesWithOrder() : get_selected_or_group();
    if (profiles.isEmpty()) return;

    TestSettings options;
    options.mode = mode;
    options.latencyUrl = QUrl(NekoGui::dataStore->test_latency_url);
    options.downloadUrl = QUrl(NekoGui::dataStore->test_download_url);
    options.downloadTimeout = std::clamp(NekoGui::dataStore->test_download_timeout, 1, 600) * 1000;
    QString program;
    if (mode != 0) {
        program = NekoGui::ManagedCoreProgram("sing-box");
        if (program.isEmpty()) {
            if (QMessageBox::question(this, tr("Core not installed"),
                                      tr("%1 is not installed. Open the update center?").arg("sing-box")) == QMessageBox::Yes)
                CheckUpdate();
            return;
        }
    }
    if (mode == 2) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Test Options"));
        QVBoxLayout layout(&dialog);
        QCheckBox latency(tr("Latency"), &dialog), udp(tr("UDP connectivity"), &dialog),
                  speed(tr("Download speed"), &dialog), ips(tr("Server and exit IP"), &dialog);
        QDialogButtonBox box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(&box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        for (auto widget : {&latency, &udp, &speed, &ips}) layout.addWidget(widget);
        layout.addWidget(&box);
        if (dialog.exec() != QDialog::Accepted) return;
        options.latency = latency.isChecked();
        options.udp = udp.isChecked();
        options.speed = speed.isChecked();
        options.ips = ips.isChecked();
        if (!(options.latency || options.udp || options.speed || options.ips)) return;
    }

    struct Job {
        int id;
        QString address;
        int port;
        std::shared_ptr<BuildResult> config;
    };
    auto jobs = std::make_shared<QList<Job>>();
    for (const auto &profile : profiles) {
        std::shared_ptr<BuildResult> config;
        if (mode != 0) {
            config = NekoGui::BuildConfig(profile, true, false);
            if (config->error.isEmpty() && config->nativeSingBox)
                config->error = configureNative(*config, false, true);
        }
        auto address = profile->bean->serverAddress;
        auto port = profile->bean->serverPort;
        if (mode == 0 && profile->type == "custom" && profile->CustomBean()->core == "internal") {
            const auto outbound = QString2QJsonObject(profile->CustomBean()->config_simple);
            if (outbound.value("server").isString()) address = outbound.value("server").toString();
            if (outbound.value("server_port").isDouble()) port = outbound.value("server_port").toInt();
        }
        jobs->append({profile->id, address, port, config});
    }
    const int concurrency = std::clamp(NekoGui::dataStore->test_concurrent, 1, static_cast<int>(jobs->size()));
    auto next = std::make_shared<std::atomic_int>(0);
    auto active = std::make_shared<std::atomic_int>(concurrency);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    cancelSpeedtest = cancel;
    speedtesting = true;
    for (int i = 0; i < concurrency; ++i) {
        runOnNewThread([=] {
            while (!*cancel) {
                const auto index = next->fetch_add(1);
                if (index >= jobs->size()) break;
                const auto &job = jobs->at(index);
                const auto result = runProfileTest(job.config.get(), program, job.address, job.port, options, *cancel);
                if (*cancel) break;
                runOnUiThread([this, id = job.id, result, options] {
                    const auto profile = NekoGui::profileManager->GetProfile(id);
                    if (!profile) return;
                    if (options.mode != 2 || options.latency) profile->latency = result.latency;
                    profile->full_test_report = result.report.isEmpty() ? result.error : result.report;
                    profile->Save();
                    refresh_proxy_list(id);
                    if (!result.error.isEmpty()) MW_show_log(tr("[%1] test error: %2").arg(profile->bean->DisplayTypeAndName(), result.error));
                });
            }
            if (active->fetch_sub(1) == 1) {
                runOnUiThread([this, cancel] {
                    speedtesting = false;
                    cancelSpeedtest.reset();
                    MW_show_log(*cancel ? tr("Speed test cancelled.") : tr("Speedtest finished."));
                    if (exitPending) on_menu_exit_triggered();
                });
            }
        });
    }
}

void MainWindow::speedtest_current() {
    if (!running || profileTransition) return;
    const auto generation = profileGeneration;
    const auto settings = NekoGui::dataStore;
    auto address = settings->inbound_address;
    if (address.isEmpty() || address == "0.0.0.0") address = "127.0.0.1";
    if (address == "::") address = "::1";
    if (!IsValidPort(settings->inbound_socks_port)) return;
    const auto auth = settings->inbound_auth;
    const QNetworkProxy proxy(QNetworkProxy::HttpProxy, address, settings->inbound_socks_port,
                              auth->username, auth->password);
    auto manager = new QNetworkAccessManager(this);
    manager->setProxy(proxy);
    last_test_time = QTime::currentTime();
    ui->label_running->setText(tr("Testing"));
    const auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto reply = manager->get(QNetworkRequest(QUrl(settings->test_latency_url)));
    QTimer::singleShot(10000, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [=] {
        manager->deleteLater();
        if (generation != profileGeneration) return;
        last_test_time = QTime::currentTime();
        if (reply->error() != QNetworkReply::NoError) {
            MW_show_log(tr("UrlTest error: %1").arg(reply->errorString()));
            ui->label_running->setText(tr("Test Result") + ": " + tr("Unavailable"));
        } else {
            ui->label_running->setText(tr("Test Result") + ": " + QStringLiteral("%1 ms").arg(elapsed->elapsed()));
        }
    });
}

void MainWindow::CheckUpdate() {
    if (updateDialogOpen || NekoGui::dataStore->prepare_exit) return;

    const auto root = NekoGui::ManagedComponentsRoot();
    const auto installDirectory = QApplication::applicationDirPath();
#ifdef Q_OS_WIN
    const bool portablePackage = QFileInfo(installDirectory + "/updater.exe").isFile();
#else
    const bool portablePackage = QFileInfo(installDirectory + "/launcher").isExecutable();
#endif
    const bool portable = portablePackage && QFileInfo(installDirectory).isWritable() &&
                          !NekoGui::dataStore->flag_many && !qEnvironmentVariableIsSet("APPIMAGE") &&
                          !qEnvironmentVariableIsSet("APPDIR");

    auto canApply = [this](const QString &id) {
        if (speedtesting || profileTransition) return false;
        if (!running || id == "nekoray") return true;
        const auto bean = running->type == "custom" ? running->CustomBean() : nullptr;
        return id != "sing-box" && id != "geoip" && id != "geosite" &&
               !(bean && bean->managed_core && bean->core == id);
    };

    updateDialogOpen = true;
    UpdateDialog dialog(root, portable, canApply, [this] { neko_stop(); }, this);
    dialog.exec();
    updateDialogOpen = false;
    if (dialog.appRevision().isEmpty()) return;

    // The detached copy must be outside the installation being replaced, especially on Windows.
    QFile source(NekoGui::UpdateHelperProgram());
#ifdef Q_OS_WIN
    const auto workerName = QStringLiteral("nekoray-updater-XXXXXX.exe");
#else
    const auto workerName = QStringLiteral("nekoray-updater-XXXXXX");
#endif
    QTemporaryFile worker(QDir::temp().absoluteFilePath(workerName));
    if (!source.open(QIODevice::ReadOnly) || !worker.open() ||
        !worker.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        MessageBoxWarning(tr("Update Center"), tr("Could not prepare the update helper."));
        return;
    }
    while (!source.atEnd()) {
        const auto chunk = source.read(64 * 1024);
        if (chunk.isEmpty() || worker.write(chunk) != chunk.size()) {
            MessageBoxWarning(tr("Update Center"), tr("Could not copy the update helper."));
            return;
        }
    }
    if (!worker.flush()) {
        MessageBoxWarning(tr("Update Center"), worker.errorString());
        return;
    }
    worker.close();
    worker.setAutoRemove(false);
    updateWorker = worker.fileName();
    const bool launcher = qEnvironmentVariable("NKR_FROM_LAUNCHER") == "1";
    updateArguments = QStringList{"components", "apply", root, dialog.appRevision(), installDirectory,
                                  QString::number(QCoreApplication::applicationPid()), launcher ? "launcher" : "gui"};
    auto originalArgs = NekoGui::dataStore->argv.mid(1);
    originalArgs.removeAll("-flag_restart_tun_on");
    originalArgs.removeAll("-flag_reorder");
    updateArguments += originalArgs;
    exit_reason = 1;
    on_menu_exit_triggered();
}
