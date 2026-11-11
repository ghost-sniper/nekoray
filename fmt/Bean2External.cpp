#include "db/ProxyEntity.hpp"
#include "fmt/includes.h"
#include "main/ManagedComponents.hpp"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcessEnvironment>

#ifndef NKR_NO_YAML
#include <yaml-cpp/yaml.h>
#endif

#define WriteTempFile(fn, data)                                   \
    QDir dir;                                                     \
    if (!dir.exists("temp")) dir.mkdir("temp");                   \
    QFile f(QStringLiteral("temp/") + fn);                               \
    bool ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate); \
    if (ok) {                                                     \
        f.write(data);                                            \
    } else {                                                      \
        result.error = f.errorString();                           \
    }                                                             \
    f.close();                                                    \
    auto TempFile = QFileInfo(f).absoluteFilePath();

namespace NekoGui_fmt {
    // -1: Cannot use this config
    // 0: Internal
    // 1: Mapping External
    // 2: Direct External

    int NaiveBean::NeedExternal(bool isFirstProfile) {
        if (isFirstProfile) {
            if (NekoGui::dataStore->spmode_vpn) {
                return 1;
            }
            return 2;
        }
        return 1;
    }

    int QUICBean::NeedExternal(bool isFirstProfile) {
        auto extCore = [=] {
            if (isFirstProfile) {
                if (NekoGui::dataStore->spmode_vpn && hopPort.trimmed().isEmpty()) {
                    return 1;
                }
                return 2;
            } else {
                if (!hopPort.trimmed().isEmpty()) {
                    return -1;
                }
            }
            return 1;
        };

        if (!forceExternal) {
            // sing-box support
            return 0;
        } else {
            // external core support
            return extCore();
        }
    }

    int CustomBean::NeedExternal(bool isFirstProfile) {
        if (managed_core) return 2;
        if (core == "internal" || core == "internal-full") return 0;
        return 1;
    }

    ExternalBuildResult NaiveBean::BuildExternal(int mapping_port, int socks_port, int external_stat) {
        ExternalBuildResult result{NekoGui::dataStore->extraCore->Get("naive")};

        auto is_direct = external_stat == 2;
        auto domain_address = sni.isEmpty() ? serverAddress : sni;
        auto connect_address = is_direct ? serverAddress : "127.0.0.1";
        auto connect_port = is_direct ? serverPort : mapping_port;
        domain_address = WrapIPV6Host(domain_address);
        connect_address = WrapIPV6Host(connect_address);

        auto proxy_url = QUrl();
        proxy_url.setScheme(protocol);
        proxy_url.setUserName(username);
        proxy_url.setPassword(password);
        proxy_url.setPort(connect_port);
        proxy_url.setHost(domain_address);

        if (!disable_log) result.arguments += "--log";
        result.arguments += "--listen=socks://127.0.0.1:" + Int2String(socks_port);
        result.arguments += "--proxy=" + proxy_url.toString(QUrl::FullyEncoded);
        if (domain_address != connect_address)
            result.arguments += "--host-resolver-rules=MAP " + domain_address + " " + connect_address;
        if (insecure_concurrency > 0) result.arguments += "--insecure-concurrency=" + Int2String(insecure_concurrency);
        if (!extra_headers.trimmed().isEmpty()) result.arguments += "--extra-headers=" + extra_headers;
        if (!certificate.trimmed().isEmpty()) {
            WriteTempFile("naive_" + GetRandomString(10) + ".crt", certificate.toUtf8());
            result.env += "SSL_CERT_FILE=" + TempFile;
        }

        auto config_export = QStringList{result.program};
        config_export += result.arguments;
        result.config_export = QStringList2Command(config_export);

        return result;
    }

    ExternalBuildResult QUICBean::BuildExternal(int mapping_port, int socks_port, int external_stat) {
        if (proxy_type == proxy_TUIC) {
            ExternalBuildResult result{NekoGui::dataStore->extraCore->Get("tuic")};

            QJsonObject relay;

            relay["uuid"] = uuid;
            relay["password"] = password;
            relay["udp_relay_mode"] = udpRelayMode;
            relay["congestion_control"] = congestionControl;
            relay["zero_rtt_handshake"] = zeroRttHandshake;
            relay["disable_sni"] = disableSni;
            if (!heartbeat.trimmed().isEmpty()) relay["heartbeat"] = heartbeat;
            if (!alpn.trimmed().isEmpty()) relay["alpn"] = QList2QJsonArray(alpn.split(","));

            if (!caText.trimmed().isEmpty()) {
                WriteTempFile("tuic_" + GetRandomString(10) + ".crt", caText.toUtf8());
                QJsonArray certificate;
                certificate.append(TempFile);
                relay["certificates"] = certificate;
            }

            // The most confused part of TUIC......
            if (serverAddress == sni) {
                relay["server"] = serverAddress + ":" + Int2String(serverPort);
            } else {
                relay["server"] = sni + ":" + Int2String(serverPort);
                relay["ip"] = serverAddress;
            }

            QJsonObject local{
                {"server", "127.0.0.1:" + Int2String(socks_port)},
            };

            QJsonObject config{
                {"relay", relay},
                {"local", local},
            };

            //

            result.config_export = QJsonObject2QString(config, false);
            WriteTempFile("tuic_" + GetRandomString(10) + ".json", result.config_export.toUtf8());
            result.arguments = QStringList{"-c", TempFile};

            return result;
        } else if (proxy_type == proxy_Hysteria2) {
            ExternalBuildResult result{NekoGui::dataStore->extraCore->Get("hysteria2")};

            QJsonObject config;

            auto server = serverAddress;
            if (!hopPort.trimmed().isEmpty()) {
                server = WrapIPV6Host(server) + ":" + hopPort;
            } else {
                server = WrapIPV6Host(server) + ":" + Int2String(serverPort);
            }

            QJsonObject transport;
            transport["type"] = "udp";
            transport["udp"] = QJsonObject{
                {"hopInterval", QString::number(hopInterval) + "s"},
            };
            config["transport"] = transport;

            config["server"] = server;
            config["socks5"] = QJsonObject{
                {"listen", "127.0.0.1:" + Int2String(socks_port)},
                {"disableUDP", false},
            };
            config["auth"] = password;

            QJsonObject bandwidth;
            if (uploadMbps > 0) bandwidth["up"] = Int2String(uploadMbps) + " mbps";
            if (downloadMbps > 0) bandwidth["down"] = Int2String(downloadMbps) + " mbps";
            config["bandwidth"] = bandwidth;

            QJsonObject quic;
            if (streamReceiveWindow > 0) quic["initStreamReceiveWindow"] = streamReceiveWindow;
            if (connectionReceiveWindow > 0) quic["initConnReceiveWindow"] = connectionReceiveWindow;
            if (disableMtuDiscovery) quic["disablePathMTUDiscovery"] = true;
            config["quic"] = quic;

            config["fastOpen"] = true;
            config["lazy"] = true;

            if (!obfsPassword.isEmpty()) {
                QJsonObject obfs;
                obfs["type"] = "salamander";
                obfs["salamander"] = QJsonObject{
                    {"password", obfsPassword},
                };

                config["obfs"] = obfs;
            }

            QJsonObject tls;
            auto sniGen = sni;
            if (sni.isEmpty() && !IsIpAddress(serverAddress)) sniGen = serverAddress;
            tls["sni"] = sniGen;
            if (allowInsecure) tls["insecure"] = true;
            if (!caText.trimmed().isEmpty()) {
                WriteTempFile("hysteria2_" + GetRandomString(10) + ".crt", caText.toUtf8());
                QJsonArray certificate;
                certificate.append(TempFile);
                tls["certificates"] = certificate;
            }
            config["tls"] = tls;

            result.config_export = QJsonObject2QString(config, false);
            WriteTempFile("hysteria2_" + GetRandomString(10) + ".json", result.config_export.toUtf8());
            result.arguments = QStringList{"-c", TempFile};

            return result;
        }
        ExternalBuildResult e;
        e.error = "unknown type";
        return e;
    }

    ExternalBuildResult CustomBean::BuildExternal(int mapping_port, int socks_port, int external_stat) {
        if (managed_core) return BuildManagedExternal(socks_port);
        ExternalBuildResult result{NekoGui::dataStore->extraCore->Get(core)};

        result.arguments = command; // TODO split?

        for (int i = 0; i < result.arguments.length(); i++) {
            auto arg = result.arguments[i];
            arg = arg.replace("%mapping_port%", Int2String(mapping_port));
            arg = arg.replace("%socks_port%", Int2String(socks_port));
            arg = arg.replace("%server_addr%", serverAddress);
            arg = arg.replace("%server_port%", Int2String(serverPort));
            result.arguments[i] = arg;
        }

        if (!config_simple.trimmed().isEmpty()) {
            auto config = config_simple;
            config = config.replace("%mapping_port%", Int2String(mapping_port));
            config = config.replace("%socks_port%", Int2String(socks_port));
            config = config.replace("%server_addr%", serverAddress);
            config = config.replace("%server_port%", Int2String(serverPort));

            // suffix
            QString suffix;
            if (!config_suffix.isEmpty()) {
                suffix = "." + config_suffix;
            } else if (!QString2QJsonObject(config).isEmpty()) {
                // trojan-go: unsupported config format: xxx.tmp. use .yaml or .json instead.
                suffix = ".json";
            }

            // write config
            WriteTempFile("custom_" + GetRandomString(10) + suffix, config.toUtf8());
            for (int i = 0; i < result.arguments.count(); i++) {
                result.arguments[i] = result.arguments[i].replace("%config%", TempFile);
            }

            result.config_export = config;
        }

        return result;
    }

    ExternalBuildResult CustomBean::BuildManagedExternal(int port) const {
        ExternalBuildResult result;
        if (core != "xray" && core != "mihomo" && core != "sing-box") {
            result.error = QObject::tr("Unknown external core.");
            return result;
        }
        if (!IsValidPort(port)) {
            result.error = QObject::tr("Could not allocate a local SOCKS port.");
            return result;
        }
        const QString tag = "nekoray-socks";
        if (core == "mihomo") {
#ifndef NKR_NO_YAML
            try {
                auto config = YAML::Load(config_simple.toStdString());
                if (!config.IsMap()) throw std::runtime_error("expected a YAML mapping");
                const auto tun = config["tun"];
                if (tun && tun["enable"].as<bool>(false)) {
                    result.error = QObject::tr("Disable TUN in the external core configuration.");
                    return result;
                }
                if (!config["listeners"]) config["listeners"] = YAML::Node(YAML::NodeType::Sequence);
                auto listeners = config["listeners"];
                if (!listeners.IsSequence()) throw std::runtime_error("listeners must be a sequence");
                for (const auto &listener : listeners) {
                    if (listener["name"].as<std::string>("") == tag.toStdString())
                        throw std::runtime_error("the listener name nekoray-socks is reserved");
                }
                YAML::Node listener;
                listener["name"] = tag.toStdString();
                listener["type"] = "socks";
                listener["listen"] = "127.0.0.1";
                listener["port"] = port;
                listener["udp"] = true;
                listener["users"] = YAML::Node(YAML::NodeType::Sequence);
                listeners.push_back(listener);
                result.config_export = QString::fromStdString(YAML::Dump(config));
            } catch (const std::exception &error) {
                result.error = QObject::tr("Invalid Mihomo YAML: %1").arg(QString::fromUtf8(error.what()));
                return result;
            }
#else
            result.error = QObject::tr("This build requires YAML support to run Mihomo configurations.");
            return result;
#endif
        } else {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(config_simple.toUtf8(), &error);
            if (!document.isObject()) {
                result.error = QObject::tr("Expected a JSON configuration object: %1").arg(error.errorString());
                return result;
            }
            auto config = document.object();
            if (config.contains("inbounds") && !config.value("inbounds").isArray()) {
                result.error = QObject::tr("The inbounds field must be an array.");
                return result;
            }
            auto inbounds = config.value("inbounds").toArray();
            const bool xray = core == "xray";
            for (const auto &inbound : inbounds) {
                const auto object = inbound.toObject();
                if (object.value(xray ? "protocol" : "type") == "tun") {
                    result.error = QObject::tr("Disable TUN in the external core configuration.");
                    return result;
                }
                if (object.value("tag") == tag) {
                    result.error = QObject::tr("The inbound tag %1 is reserved.").arg(tag);
                    return result;
                }
            }
            QJsonObject inbound{{"tag", tag}, {"listen", "127.0.0.1"}};
            if (xray) {
                inbound["protocol"] = "socks";
                inbound["port"] = port;
                inbound["settings"] = QJsonObject{{"auth", "noauth"}, {"udp", true}};
            } else {
                inbound["type"] = "socks";
                inbound["listen_port"] = port;
            }
            inbounds.append(inbound);
            config["inbounds"] = inbounds;
            result.config_export = QJsonObject2QString(config, false);
        }

        result.program = NekoGui::ManagedCoreProgram(core);
        if (result.program.isEmpty()) return result;
        result.workingDirectory = NekoGui::ManagedComponentsRoot() + "/runtime/" + core;
        if (!QDir().mkpath("temp")) {
            result.error = QObject::tr("Could not create the temporary configuration directory.");
            return result;
        }
        const auto suffix = core == "mihomo" ? ".yaml" : ".json";
        auto configFile = std::make_shared<QTemporaryFile>(QDir::current().absoluteFilePath("temp/" + core + "-XXXXXX" + suffix));
        const auto contents = result.config_export.toUtf8();
        if (!configFile->open() || configFile->write(contents) != contents.size() || !configFile->flush()) {
            result.error = configFile->errorString();
            return result;
        }
        configFile->close();
        result.temporaryConfig = configFile;
        const auto filename = configFile->fileName();
        auto env = QProcessEnvironment::systemEnvironment();
        if (core == "xray") {
            result.arguments = QStringList{"run", "-c", filename};
            result.checkArguments = QStringList{"run", "-test", "-c", filename};
            env.insert("XRAY_LOCATION_ASSET", result.workingDirectory);
            for (const auto &name : {QStringLiteral("geoip.dat"), QStringLiteral("geosite.dat")}) {
                auto source = NekoGui::FindCoreAsset(name);
                if (source.isEmpty()) source = NekoGui::ManagedComponentFile(core, name);
                if (!source.isEmpty()) result.assets.append(qMakePair(source, name));
            }
        } else if (core == "mihomo") {
            result.arguments = QStringList{"-d", result.workingDirectory, "-f", filename};
            result.checkArguments = QStringList{"-t"} + result.arguments;
            for (const auto &id : {QStringLiteral("geoip"), QStringLiteral("geosite")}) {
                const auto source = NekoGui::ManagedComponentFile(id, "mihomo-" + id + ".dat");
                const auto fallback = source.isEmpty() ? NekoGui::FindCoreAsset(id + ".dat") : source;
                if (!fallback.isEmpty()) result.assets.append(qMakePair(fallback, id + ".dat"));
            }
            const auto metadb = NekoGui::ManagedComponentFile("geoip", "geoip.metadb");
            if (!metadb.isEmpty()) result.assets.append(qMakePair(metadb, QStringLiteral("geoip.metadb")));
        } else {
            result.arguments = QStringList{"run", "-c", filename};
            result.checkArguments = QStringList{"check", "-c", filename};
#ifdef Q_OS_LINUX
            const auto binaryDir = QFileInfo(result.program).absolutePath();
            const auto inherited = env.value("LD_LIBRARY_PATH");
            env.insert("LD_LIBRARY_PATH", inherited.isEmpty() ? binaryDir : binaryDir + ":" + inherited);
#endif
        }
        result.env = env.toStringList();
        result.readyPort = port;
        return result;
    }

} // namespace NekoGui_fmt
