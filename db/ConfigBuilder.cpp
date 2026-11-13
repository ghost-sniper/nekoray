#include "db/ConfigBuilder.hpp"
#include "db/Database.hpp"
#include "fmt/includes.h"
#include "fmt/Preset.hpp"
#include "main/ManagedComponents.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryFile>
#include <QUrl>
#include <QRegularExpression>

namespace NekoGui {

    QStringList getAutoBypassExternalProcessPaths(const std::shared_ptr<BuildConfigResult> &result) {
        QStringList paths;
        for (const auto &extR: result->extRs) {
            auto path = extR->program;
            if (path.trimmed().isEmpty()) continue;
            paths << path.replace("\\", "/");
        }
        return paths;
    }

    QString genTunName() {
        auto tun_name = "neko-tun";
#ifdef Q_OS_MACOS
        tun_name = "utun9";
#endif
        return tun_name;
    }

    void MergeJson(QJsonObject &dst, const QJsonObject &src) {
        // 合并
        if (src.isEmpty()) return;
        for (const auto &key: src.keys()) {
            auto v_src = src[key];
            if (dst.contains(key)) {
                auto v_dst = dst[key];
                if (v_src.isObject() && v_dst.isObject()) { // isObject 则合并？
                    auto v_src_obj = v_src.toObject();
                    auto v_dst_obj = v_dst.toObject();
                    MergeJson(v_dst_obj, v_src_obj);
                    dst[key] = v_dst_obj;
                } else {
                    dst[key] = v_src;
                }
            } else if (v_src.isArray()) {
                if (key.startsWith("+")) {
                    auto key2 = SubStrAfter(key, "+");
                    auto v_dst = dst[key2];
                    auto v_src_arr = v_src.toArray();
                    auto v_dst_arr = v_dst.toArray();
                    QJSONARRAY_ADD(v_src_arr, v_dst_arr)
                    dst[key2] = v_src_arr;
                } else if (key.endsWith("+")) {
                    auto key2 = SubStrBefore(key, "+");
                    auto v_dst = dst[key2];
                    auto v_src_arr = v_src.toArray();
                    auto v_dst_arr = v_dst.toArray();
                    QJSONARRAY_ADD(v_dst_arr, v_src_arr)
                    dst[key2] = v_dst_arr;
                } else {
                    dst[key] = v_src;
                }
            } else {
                dst[key] = v_src;
            }
        }
    }

    // Common

    std::shared_ptr<BuildConfigResult> BuildConfig(const std::shared_ptr<ProxyEntity> &ent, bool forTest, bool forExport) {
        auto result = std::make_shared<BuildConfigResult>();
        auto status = std::make_shared<BuildConfigStatus>();
        status->ent = ent;
        status->result = result;
        status->forTest = forTest;
        status->forExport = forExport;

        auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
        if (customBean && (customBean->core == "internal-full" ||
                           (customBean->managed_core && customBean->core == "sing-box"))) {
            result->nativeSingBox = true;
            result->migrateLegacyConfig = customBean->core == "internal-full";
            const auto group = profileManager->GetGroup(ent->gid);
            if (group && group->front_proxy_id >= 0) {
                result->error = QObject::tr("Full sing-box configurations cannot be used in a proxy chain.");
                return result;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(customBean->config_simple.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                result->error = QObject::tr("Invalid sing-box JSON: %1").arg(parseError.errorString());
                return result;
            }
            result->coreConfig = document.object();
        } else {
            BuildConfigSingBox(status);
        }

        // apply custom config
        MergeJson(result->coreConfig, QString2QJsonObject(ent->bean->custom_config));

        return result;
    }

    QString BuildChain(int chainId, const std::shared_ptr<BuildConfigStatus> &status) {
        auto group = profileManager->GetGroup(status->ent->gid);
        if (group == nullptr) {
            status->result->error = QStringLiteral("This profile is not in any group, your data may be corrupted.");
            return {};
        }

        auto resolveChain = [=](const std::shared_ptr<ProxyEntity> &ent) {
            QList<std::shared_ptr<ProxyEntity>> resolved;
            if (ent->type == "chain") {
                auto list = ent->ChainBean()->list;
                std::reverse(std::begin(list), std::end(list));
                for (auto id: list) {
                    resolved += profileManager->GetProfile(id);
                    if (resolved.last() == nullptr) {
                        status->result->error = QStringLiteral("chain missing ent: %1").arg(id);
                        break;
                    }
                    if (resolved.last()->type == "chain") {
                        status->result->error = QStringLiteral("chain in chain is not allowed: %1").arg(id);
                        break;
                    }
                }
            } else {
                resolved += ent;
            };
            return resolved;
        };

        // Make list
        auto ents = resolveChain(status->ent);
        if (!status->result->error.isEmpty()) return {};

        if (group->front_proxy_id >= 0) {
            auto fEnt = profileManager->GetProfile(group->front_proxy_id);
            if (fEnt == nullptr) {
                status->result->error = QStringLiteral("front proxy ent not found.");
                return {};
            }
            ents += resolveChain(fEnt);
            if (!status->result->error.isEmpty()) return {};
        }

        return BuildChainInternal(0, ents, status);
    }

#define DOMAIN_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_domain)) {  \
        if (dataStore->routing->dns_routing) status->domainListDNSRemote += line;    \
        status->domainListRemote += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_domain)) { \
        if (dataStore->routing->dns_routing) status->domainListDNSDirect += line;    \
        status->domainListDirect += line;                                            \
    }                                                                                \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_domain)) {  \
        status->domainListBlock += line;                                             \
    }

#define IP_USER_RULE                                                             \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->block_ip)) {  \
        status->ipListBlock += line;                                             \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->proxy_ip)) {  \
        status->ipListRemote += line;                                            \
    }                                                                            \
    for (const auto &line: SplitLinesSkipSharp(dataStore->routing->direct_ip)) { \
        status->ipListDirect += line;                                            \
    }

    QString BuildChainInternal(int chainId, const QList<std::shared_ptr<ProxyEntity>> &ents,
                               const std::shared_ptr<BuildConfigStatus> &status) {
        QString chainTag = "c-" + Int2String(chainId);
        QString chainTagOut;
        bool muxApplied = false;

        QString pastTag;
        int pastExternalStat = 0;
        int index = 0;

        for (const auto &ent: ents) {
            // tagOut: v2ray outbound tag for a profile
            // profile2 (in) (global)   tag g-(id)
            // profile1                 tag (chainTag)-(id)
            // profile0 (out)           tag (chainTag)-(id) / single: chainTag=g-(id)
            auto tagOut = chainTag + "-" + Int2String(ent->id);

            // needGlobal: can only contain one?
            bool needGlobal = false;

            // first profile set as global
            auto isFirstProfile = index == ents.length() - 1;
            if (isFirstProfile) {
                needGlobal = true;
                tagOut = "g-" + Int2String(ent->id);
            }

            // last profile set as "proxy"
            if (chainId == 0 && index == 0) {
                needGlobal = false;
                tagOut = "proxy";
            }

            if (needGlobal) {
                if (status->globalProfiles.contains(ent->id)) {
                    continue;
                }
                status->globalProfiles += ent->id;
            }

            if (index > 0) {
                // chain rules: past
                if (pastExternalStat == 0) {
                    auto replaced = status->outbounds.last().toObject();
                    replaced["detour"] = tagOut;
                    status->outbounds.removeLast();
                    status->outbounds += replaced;
                } else {
                    status->routingRules += QJsonObject{
                        {"inbound", QJsonArray{pastTag + "-mapping"}},
                        {"outbound", tagOut},
                    };
                }
            } else {
                // index == 0 means last profile in chain / not chain
                chainTagOut = tagOut;
            }

            // chain rules: this
            const auto custom = ent->type == "custom" ? ent->CustomBean() : nullptr;
            const bool managedCore = custom != nullptr && custom->managed_core;
            if (custom && custom->core == "internal-full") {
                status->result->error = QObject::tr("Full sing-box configurations cannot be used in a proxy chain.");
                return {};
            }
            if (managedCore && (ents.length() != 1 || status->ent->type == "chain")) {
                status->result->error = QObject::tr("External core configurations cannot be used in a proxy chain.");
                return {};
            }
            if (managedCore && dataStore->spmode_vpn && !status->forTest) {
                status->result->error = QObject::tr("Disable Nekoray TUN before starting an external core configuration.");
                return {};
            }
            auto ext_mapping_port = 0;
            auto ext_socks_port = 0;
            auto thisExternalStat = ent->bean->NeedExternal(isFirstProfile);
            if (thisExternalStat < 0) {
                status->result->error = "This configuration cannot be set automatically, please try another.";
                return {};
            }

            // determine port
            if (thisExternalStat > 0) {
                if (managedCore) {
                    ext_socks_port = MkPort();
                } else if (ent->type == "custom") {
                    auto bean = ent->CustomBean();
                    if (IsValidPort(bean->mapping_port)) {
                        ext_mapping_port = bean->mapping_port;
                    } else {
                        ext_mapping_port = MkPort();
                    }
                    if (IsValidPort(bean->socks_port)) {
                        ext_socks_port = bean->socks_port;
                    } else {
                        ext_socks_port = MkPort();
                    }
                } else {
                    ext_mapping_port = MkPort();
                    ext_socks_port = MkPort();
                }
            }
            if (thisExternalStat == 2) status->result->requiresNoTun = true;
            if (thisExternalStat == 1) {
                // mapping
                status->inbounds += QJsonObject{
                    {"type", "direct"},
                    {"tag", tagOut + "-mapping"},
                    {"listen", "127.0.0.1"},
                    {"listen_port", ext_mapping_port},
                };
                status->routingRules += QJsonObject{
                    {"inbound", tagOut + "-mapping"},
                    {"action", "route-options"},
                    {"override_address", ent->bean->serverAddress},
                    {"override_port", ent->bean->serverPort},
                };
                // no chain rule and not outbound, so need to set to direct
                if (isFirstProfile) {
                    status->routingRules += QJsonObject{
                        {"inbound", QJsonArray{tagOut + "-mapping"}},
                        {"outbound", "direct"},
                    };
                }
            }

            // Outbound

            QJsonObject outbound;
            auto stream = GetStreamSettings(ent->bean.get());

            if (thisExternalStat > 0) {
                auto extR = ent->bean->BuildExternal(ext_mapping_port, ext_socks_port, thisExternalStat);
                if (managedCore && !extR.error.isEmpty()) {
                    status->result->error = extR.error;
                    return {};
                }
                if (extR.program.isEmpty()) {
                    if (managedCore) status->result->missingManagedCore = custom->core;
                    status->result->error = QObject::tr("Core not found: %1").arg(ent->bean->DisplayCoreType());
                    return {};
                }
                if (!extR.error.isEmpty()) { // rejected
                    status->result->error = extR.error;
                    return {};
                }
                extR.tag = ent->bean->DisplayType();
                status->result->extRs.emplace_back(std::make_shared<NekoGui_fmt::ExternalBuildResult>(extR));

                // SOCKS OUTBOUND
                outbound["type"] = "socks";
                outbound["server"] = "127.0.0.1";
                outbound["server_port"] = ext_socks_port;
            } else {
                const auto coreR = ent->bean->BuildCoreObjSingBox();
                if (coreR.outbound.isEmpty()) {
                    status->result->error = "unsupported outbound";
                    return {};
                }
                if (!coreR.error.isEmpty()) { // rejected
                    status->result->error = coreR.error;
                    return {};
                }
                outbound = coreR.outbound;
            }

            outbound["tag"] = tagOut;

            // mux common
            auto needMux = ent->type == "vmess" || ent->type == "trojan" || ent->type == "vless";
            needMux &= dataStore->mux_concurrency > 0;

            if (stream != nullptr) {
                if (stream->network == "grpc" || stream->network == "quic" || (stream->network == "http" && stream->security == "tls")) {
                    needMux = false;
                }
                if (stream->multiplex_status == 0) {
                    if (!dataStore->mux_default_on) needMux = false;
                } else if (stream->multiplex_status == 1) {
                    needMux = true;
                } else if (stream->multiplex_status == 2) {
                    needMux = false;
                }
            }
            if (ent->type == "vless" && outbound["flow"] != "") {
                needMux = false;
            }

            // common
            const auto strategy = dataStore->routing->outbound_domain_strategy;
            if (!outbound["server"].toString().isEmpty() && !IsIpAddress(outbound["server"].toString()) && !strategy.isEmpty()) {
                outbound["domain_resolver"] = QJsonObject{{"server", "dns-direct"}, {"strategy", strategy}};
            }
            // apply mux
            if (!muxApplied && needMux) {
                auto muxObj = QJsonObject{
                    {"enabled", true},
                    {"protocol", dataStore->mux_protocol},
                    {"padding", dataStore->mux_padding},
                    {"max_streams", dataStore->mux_concurrency},
                };
                outbound["multiplex"] = muxObj;
                muxApplied = true;
            }

            // apply custom outbound settings
            MergeJson(outbound, QString2QJsonObject(ent->bean->custom_outbound));

            // Bypass Lookup for the first profile
            auto serverAddress = ent->bean->serverAddress;

            auto customBean = dynamic_cast<NekoGui_fmt::CustomBean *>(ent->bean.get());
            if (customBean != nullptr && customBean->core == "internal") {
                auto server = QString2QJsonObject(customBean->config_simple)["server"].toString();
                if (!server.isEmpty()) serverAddress = server;
            }

            if (!IsIpAddress(serverAddress)) {
                status->domainListDNSDirect += "full:" + serverAddress;
            }

            status->outbounds += outbound;
            pastTag = tagOut;
            pastExternalStat = thisExternalStat;
            index++;
        }

        return chainTagOut;
    }

    static QJsonObject dnsServer(const QString &tag, QString address,
                                 const QString &resolver = {}, const QString &detour = {}) {
        if (address == "local" || address.isEmpty()) return {{"tag", tag}, {"type", "local"}};
        if (address == "local://") return {{"tag", tag}, {"type", "local"}};
        const QUrl url(address.contains("://") ? address : "udp://" + WrapIPV6Host(address));
        QJsonObject server{{"tag", tag}, {"type", url.scheme()}};
        if (url.scheme() == "dhcp") {
            if (url.host() != "auto") server["interface"] = url.host();
            return server;
        }
        server["server"] = url.host();
        if (url.port() > 0) server["server_port"] = url.port();
        if (url.scheme() == "https" || url.scheme() == "h3") {
            auto path = url.path(QUrl::FullyEncoded);
            if (path.isEmpty()) path = "/dns-query";
            if (url.hasQuery()) path += "?" + url.query(QUrl::FullyEncoded);
            server["path"] = path;
        }
        if (!resolver.isEmpty() && !IsIpAddress(url.host())) server["domain_resolver"] = resolver;
        if (!detour.isEmpty()) server["detour"] = detour;
        return server;
    }

    QString PrepareSingBoxConfig(QJsonObject &config, const QString &program) {
        auto route = config.value("route").toObject();
        auto dns = config.value("dns").toObject();
        const auto outbounds = config.value("outbounds").toArray();
        QMap<QString, QString> legacyActions;
        QSet<QString> outboundTags;
        QJsonArray modernOutbounds;
        for (const auto &value : outbounds) {
            const auto outbound = value.toObject();
            const auto type = outbound.value("type").toString();
            const auto tag = outbound.value("tag").toString();
            if (type == "block" || type == "dns")
                legacyActions[tag] = type == "block" ? "reject" : "hijack-dns";
            else {
                outboundTags.insert(tag);
                modernOutbounds.append(value);
            }
        }
        if (!outboundTags.contains("block")) legacyActions["block"] = "reject";
        if (!outboundTags.contains("dns-out")) legacyActions["dns-out"] = "hijack-dns";
        if (modernOutbounds.size() != outbounds.size()) config["outbounds"] = modernOutbounds;
        if (config.value("inbounds").isArray()) {
            QJsonArray updated;
            for (const auto &value : config.value("inbounds").toArray()) {
                auto inbound = value.toObject();
                if (inbound.value("type") == "tun") {
                    auto addresses = inbound.value("address").toArray();
                    if (inbound.value("address").isString()) addresses.append(inbound.value("address"));
                    for (const auto &field : {QStringLiteral("inet4_address"), QStringLiteral("inet6_address")}) {
                        const auto old = inbound.take(field);
                        if (old.isString()) addresses.append(old);
                        for (const auto &address : old.toArray()) addresses.append(address);
                    }
                    if (!addresses.isEmpty()) inbound["address"] = addresses;
                }
                updated.append(inbound);
            }
            config["inbounds"] = updated;
        }
        QMap<QString, QString> rcodeServers;
        const auto legacyFakeIp = dns.value("fakeip").toObject();
        bool modernizedDns = false;
        QJsonArray dnsServers;
        for (const auto &value : dns.value("servers").toArray()) {
            const auto server = value.toObject();
            const auto address = server.value("address").toString();
            if (server.contains("type") || address.isEmpty()) {
                dnsServers.append(value);
                continue;
            }
            modernizedDns = true;
            const auto tag = server.value("tag").toString();
            if (address.startsWith("rcode://")) {
                auto code = address.mid(8).toUpper();
                if (code == "SUCCESS") code = "NOERROR";
                else if (code == "NAME_ERROR") code = "NXDOMAIN";
                else if (code == "SERVER_FAILURE") code = "SERVFAIL";
                else if (code == "FORMAT_ERROR") code = "FORMERR";
                else if (code == "NOT_IMPLEMENTED") code = "NOTIMP";
                rcodeServers[tag] = code;
                continue;
            }
            if (address == "fakeip") {
                auto fakeIp = legacyFakeIp;
                fakeIp.remove("enabled");
                fakeIp["type"] = "fakeip";
                fakeIp["tag"] = tag;
                dnsServers.append(fakeIp);
                continue;
            }
            auto detour = server.value("detour").toString();
            if (detour == "direct" || detour == "bypass") detour.clear();
            auto modern = dnsServer(tag, address, server.value("address_resolver").toString(), detour);
            const auto resolver = modern.value("domain_resolver").toString();
            if (!resolver.isEmpty() && !server.value("address_strategy").toString().isEmpty())
                modern["domain_resolver"] = QJsonObject{{"server", resolver}, {"strategy", server.value("address_strategy")}};
            if (server.contains("client_subnet")) modern["client_subnet"] = server.value("client_subnet");
            dnsServers.append(modern);
        }
        if (modernizedDns) {
            dns["servers"] = dnsServers;
            dns.remove("fakeip");
        }
        auto sets = route.value("rule_set").toArray();
        QSet<QString> existing;
        for (const auto &value : sets) existing.insert(value.toObject()["tag"].toString());
        QMap<QString, QString> assets;
        QString error;

        auto convert = [&](const QString &kind, const QString &name) -> QString {
            const QString tag = "nekoray-" + kind + "-" + name;
            if (existing.contains(tag)) return tag;
            static const QRegularExpression valid(QStringLiteral("\\A[A-Za-z0-9_.@-]+\\z"));
            if (!valid.match(name).hasMatch() || name.startsWith('-')) {
                error = QObject::tr("Invalid Geo rule: %1").arg(kind + ":" + name);
                return {};
            }
            const auto asset = FindCoreAsset(kind + ".db");
            if (asset.isEmpty()) {
                error = QObject::tr("Install %1 resources from the update center.").arg(kind);
                return {};
            }
            if (!assets.contains(asset)) {
                QFile file(asset);
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
                    error = file.errorString();
                    return {};
                }
                assets[asset] = QString::fromLatin1(hash.result().toHex().left(16));
            }
            const auto directory = QDir::current().absoluteFilePath("geo-rule-sets/" + assets[asset]);
            if (!QDir().mkpath(directory)) {
                error = QObject::tr("Could not create the Geo rule-set cache.");
                return {};
            }
            const auto extension = QStringLiteral(".json");
            const auto path = directory + "/" + tag + extension;
            if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
                if (program.isEmpty()) {
                    error = QObject::tr("Install sing-box from the update center to convert Geo rules.");
                    return {};
                }
                QTemporaryFile staged(directory + "/rule-XXXXXX" + extension);
                if (!staged.open()) {
                    error = staged.errorString();
                    return {};
                }
                staged.close();
                QProcess process;
                process.setProcessEnvironment(SingBoxEnvironment(program));
                process.setProcessChannelMode(QProcess::MergedChannels);
                process.start(program, {"--disable-color", kind, "export", name, "-f", asset, "-o", staged.fileName()});
                if (!process.waitForFinished(8000) || process.exitStatus() != QProcess::NormalExit ||
                    process.exitCode() != 0 || QFileInfo(staged.fileName()).size() == 0) {
                    error = QObject::tr("Could not convert %1: %2").arg(kind + ":" + name, QString::fromUtf8(process.readAll()).trimmed());
                    return {};
                }
                if (!QFile::rename(staged.fileName(), path) && QFileInfo(path).size() == 0) {
                    error = QObject::tr("Could not save the Geo rule set: %1").arg(path);
                    return {};
                }
            }
            existing.insert(tag);
            sets += QJsonObject{{"tag", tag}, {"type", "local"}, {"format", "source"}, {"path", path}};
            return tag;
        };

        std::function<QJsonArray(const QJsonArray &, bool)> migrate = [&](const QJsonArray &rules, bool routeRules) {
            QJsonArray result;
            for (const auto &value : rules) {
                auto rule = value.toObject();
                if (rule.contains("rules")) rule["rules"] = migrate(rule["rules"].toArray(), routeRules);
                for (const auto &kind : {QStringLiteral("geoip"), QStringLiteral("geosite")}) {
                    auto legacy = rule.take(kind);
                    const auto names = legacy.isString() ? QJsonArray{legacy} : legacy.toArray();
                    if (names.isEmpty()) continue;
                    auto tags = rule.value("rule_set").isString() ? QJsonArray{rule.value("rule_set")} : rule.value("rule_set").toArray();
                    for (const auto &name : names) {
                        if (kind == "geoip" && name.toString() == "private") {
                            rule["ip_is_private"] = true;
                            continue;
                        }
                        const auto tag = convert(kind, name.toString());
                        if (tag.isEmpty()) return QJsonArray{};
                        tags.append(tag);
                    }
                    if (!tags.isEmpty()) rule["rule_set"] = tags;
                }
                if (!routeRules) {
                    const auto code = rcodeServers.value(rule.value("server").toString());
                    if (!code.isEmpty()) {
                        rule.remove("server");
                        rule["action"] = "predefined";
                        rule["rcode"] = code;
                    }
                    if (rule.contains("outbound")) {
                        rule.remove("outbound");
                        if (rule.size() == 1 && rule.contains("server")) continue;
                    }
                }
                const auto action = legacyActions.value(rule.value("outbound").toString());
                if (routeRules && !action.isEmpty()) {
                    rule.remove("outbound");
                    rule["action"] = action;
                }
                result.append(rule);
            }
            return result;
        };
        if (route.value("rules").isArray()) route["rules"] = migrate(route["rules"].toArray(), true);
        if (error.isEmpty() && dns.value("rules").isArray()) dns["rules"] = migrate(dns["rules"].toArray(), false);
        if (!error.isEmpty()) return error;
        if (!sets.isEmpty()) route["rule_set"] = sets;
        route.remove("geoip");
        route.remove("geosite");
        const auto finalAction = legacyActions.value(route.value("final").toString());
        if (!finalAction.isEmpty()) {
            auto rules = route.value("rules").toArray();
            rules.append(QJsonObject{{"network", QJsonArray{"tcp", "udp"}}, {"action", finalAction}});
            route["rules"] = rules;
            route.remove("final");
        }
        if (!route.isEmpty()) config["route"] = route;
        if (!dns.isEmpty()) config["dns"] = dns;
        return {};
    }

    // SingBox

    void BuildConfigSingBox(const std::shared_ptr<BuildConfigStatus> &status) {
        // Log
        status->result->coreConfig["log"] = QJsonObject{{"level", dataStore->log_level}};

        // Inbounds

        const bool tun = dataStore->spmode_vpn && !status->forTest;
        if (IsValidPort(dataStore->inbound_socks_port) && !status->forTest) {
            QJsonObject mixed{{"tag", "mixed-in"}, {"type", "mixed"},
                              {"listen", dataStore->inbound_address}, {"listen_port", dataStore->inbound_socks_port}};
            if (dataStore->inbound_auth->NeedAuth()) {
                mixed["users"] = QJsonArray{QJsonObject{{"username", dataStore->inbound_auth->username},
                                                       {"password", dataStore->inbound_auth->password}}};
            }
            status->inbounds += mixed;
        }
        if (tun) {
            QJsonArray address{"172.19.0.1/28"};
            if (dataStore->vpn_ipv6) address.append("fdfe:dcba:9876::1/126");
            status->inbounds += QJsonObject{
                {"tag", "tun-in"}, {"type", "tun"}, {"interface_name", genTunName()}, {"address", address},
                {"auto_route", true}, {"endpoint_independent_nat", true}, {"mtu", dataStore->vpn_mtu},
                {"stack", Preset::SingBox::VpnImplementation.value(dataStore->vpn_implementation)},
                {"strict_route", dataStore->vpn_strict_route},
            };
        }

        // Outbounds
        auto tagProxy = BuildChain(0, status);
        if (!status->result->error.isEmpty()) return;

        // Direct routing outbounds.
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "direct"},
        };
        status->outbounds += QJsonObject{
            {"type", "direct"},
            {"tag", "bypass"},
        };
        // custom inbound
        if (!status->forTest) QJSONARRAY_ADD(status->inbounds, QString2QJsonObject(dataStore->custom_inbound)["inbounds"].toArray())

        status->result->coreConfig.insert("inbounds", status->inbounds);
        status->result->coreConfig.insert("outbounds", status->outbounds);

        // user rule
        if (!status->forTest) {
            DOMAIN_USER_RULE
            IP_USER_RULE
        }

        // sing-box common rule object
        auto make_rule = [&](const QStringList &list, bool isIP = false) {
            QJsonObject rule;
            //
            QJsonArray ip_cidr;
            QJsonArray geoip;
            //
            QJsonArray domain_keyword;
            QJsonArray domain_subdomain;
            QJsonArray domain_regexp;
            QJsonArray domain_full;
            QJsonArray geosite;
            for (auto item: list) {
                if (isIP) {
                    if (item.startsWith("geoip:")) {
                        geoip += item.replace("geoip:", "");
                    } else {
                        ip_cidr += item;
                    }
                } else {
                    // https://www.v2fly.org/config/dns.html#dnsobject
                    if (item.startsWith("geosite:")) {
                        geosite += item.replace("geosite:", "");
                    } else if (item.startsWith("full:")) {
                        domain_full += item.replace("full:", "").toLower();
                    } else if (item.startsWith("domain:")) {
                        domain_subdomain += item.replace("domain:", "").toLower();
                    } else if (item.startsWith("regexp:")) {
                        domain_regexp += item.replace("regexp:", "").toLower();
                    } else if (item.startsWith("keyword:")) {
                        domain_keyword += item.replace("keyword:", "").toLower();
                    } else {
                        domain_subdomain += item.toLower();
                    }
                }
            }
            if (isIP) {
                if (ip_cidr.isEmpty() && geoip.isEmpty()) return rule;
                rule["ip_cidr"] = ip_cidr;
                rule["geoip"] = geoip;
            } else {
                if (domain_keyword.isEmpty() && domain_subdomain.isEmpty() && domain_regexp.isEmpty() && domain_full.isEmpty() && geosite.isEmpty()) {
                    return rule;
                }
                rule["domain"] = domain_full;
                rule["domain_suffix"] = domain_subdomain; // v2ray Subdomain => sing-box suffix
                rule["domain_keyword"] = domain_keyword;
                rule["domain_regex"] = domain_regexp;
                rule["geosite"] = geosite;
            }
            return rule;
        };

        QJsonArray dnsServers{
            dnsServer("dns-local", dataStore->core_box_underlying_dns.isEmpty() ? QStringLiteral("local") : dataStore->core_box_underlying_dns),
            dnsServer("dns-direct", dataStore->routing->direct_dns, "dns-local")
        };
        QJsonArray dnsRules;
        if (!status->forTest) {
            dnsServers += dnsServer("dns-remote", dataStore->routing->remote_dns, "dns-local", tagProxy);
            dnsRules += QJsonObject{{"query_type", QJsonArray{32, 33}}, {"action", "predefined"}, {"rcode", "NOERROR"}};
            dnsRules += QJsonObject{{"domain_suffix", ".lan"}, {"action", "predefined"}, {"rcode", "NOERROR"}};
            for (const auto &entry : {qMakePair(status->domainListDNSRemote, QStringLiteral("dns-remote")),
                                       qMakePair(status->domainListDNSDirect, QStringLiteral("dns-direct"))}) {
                auto rule = make_rule(entry.first, false);
                if (rule.isEmpty()) continue;
                rule["server"] = entry.second;
                const auto strategy = entry.second == "dns-remote" ? dataStore->routing->remote_dns_strategy
                                                                  : dataStore->routing->direct_dns_strategy;
                if (!strategy.isEmpty()) rule["strategy"] = strategy;
                dnsRules += rule;
            }
        }
        if (tun && dataStore->fake_dns) {
            dnsServers += QJsonObject{{"tag", "dns-fake"}, {"type", "fakeip"},
                                      {"inet4_range", "198.18.0.0/15"}, {"inet6_range", "fc00::/18"}};
            dnsRules += QJsonObject{{"inbound", "tun-in"}, {"query_type", QJsonArray{"A", "AAAA"}}, {"server", "dns-fake"}};
        }
        const bool directDNS = status->forTest || dataStore->routing->dns_final_out == "bypass";
        QJsonObject dns{{"servers", dnsServers}, {"rules", dnsRules}, {"independent_cache", true},
                        {"final", directDNS ? "dns-direct" : "dns-remote"}};
        const auto strategy = directDNS ? dataStore->routing->direct_dns_strategy : dataStore->routing->remote_dns_strategy;
        if (!strategy.isEmpty()) dns["strategy"] = strategy;
        if (dataStore->routing->use_dns_object) dns = QString2QJsonObject(dataStore->routing->dns_object);
        status->result->coreConfig.insert("dns", dns);

        if (!status->forTest) {
            QJsonArray sniffInbounds;
            if (tun) sniffInbounds.append("tun-in");
            if (IsValidPort(dataStore->inbound_socks_port) && dataStore->routing->sniffing_mode != SniffingMode::DISABLE)
                sniffInbounds.append("mixed-in");
            QJsonArray preliminaryRules;
            if (!sniffInbounds.isEmpty()) {
                preliminaryRules.append(QJsonObject{{"inbound", sniffInbounds}, {"action", "sniff"}});
                preliminaryRules.append(QJsonObject{{"protocol", "dns"}, {"action", "hijack-dns"}});
            }
            if (!dataStore->routing->domain_strategy.isEmpty())
                preliminaryRules.append(QJsonObject{{"inbound", QJsonArray{"mixed-in", "tun-in"}},
                                                     {"action", "resolve"}, {"strategy", dataStore->routing->domain_strategy}});
            QJSONARRAY_ADD(preliminaryRules, status->routingRules)
            status->routingRules = preliminaryRules;
        }

        // sing-box routing rule object
        auto add_rule_route = [&](const QStringList &list, bool isIP, const QString &out) {
            auto rule = make_rule(list, isIP);
            if (rule.isEmpty()) return;
            if (out == "block") rule["action"] = "reject";
            else rule["outbound"] = out;
            status->routingRules += rule;
        };

        // final add user rule
        add_rule_route(status->domainListBlock, false, "block");
        add_rule_route(status->domainListRemote, false, tagProxy);
        add_rule_route(status->domainListDirect, false, "bypass");
        add_rule_route(status->ipListBlock, true, "block");
        add_rule_route(status->ipListRemote, true, tagProxy);
        add_rule_route(status->ipListDirect, true, "bypass");

        // built-in rules
        status->routingRules += QJsonObject{
            {"network", "udp"},
            {"port", QJsonArray{135, 137, 138, 139, 5353}},
            {"action", "reject"},
        };
        status->routingRules += QJsonObject{
            {"ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"action", "reject"},
        };
        status->routingRules += QJsonObject{
            {"source_ip_cidr", QJsonArray{"224.0.0.0/3", "ff00::/8"}},
            {"action", "reject"},
        };

        // tun user rule
        if (tun) {
            auto match_out = dataStore->vpn_rule_white ? "proxy" : "bypass";

            QString process_name_rule = dataStore->vpn_rule_process.trimmed();
            if (!process_name_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(process_name_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"process_name", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            QString cidr_rule = dataStore->vpn_rule_cidr.trimmed();
            if (!cidr_rule.isEmpty()) {
                auto arr = SplitLinesSkipSharp(cidr_rule);
                QJsonObject rule{{"outbound", match_out},
                                 {"ip_cidr", QList2QJsonArray(arr)}};
                status->routingRules += rule;
            }

            auto autoBypassExternalProcessPaths = getAutoBypassExternalProcessPaths(status->result);
            if (!autoBypassExternalProcessPaths.isEmpty()) {
                QJsonObject rule{{"outbound", "bypass"},
                                 {"process_path", QList2QJsonArray(autoBypassExternalProcessPaths)}};
                status->routingRules += rule;
            }
        }

        auto routingRules = status->forTest ? QJsonArray{} : QString2QJsonObject(dataStore->routing->custom)["rules"].toArray();
        if (!status->forTest) QJSONARRAY_ADD(routingRules, QString2QJsonObject(dataStore->custom_route_global)["rules"].toArray())
        QJSONARRAY_ADD(routingRules, status->routingRules)
        QJsonObject routeObj{{"rules", routingRules},
                             {"final", status->forTest ? tagProxy : dataStore->routing->def_outbound}};
        for (const auto &server : dns.value("servers").toArray()) {
            if (server.toObject().value("tag") == "dns-direct") {
                routeObj["default_domain_resolver"] = "dns-direct";
                break;
            }
        }
        if (!routeObj.contains("default_domain_resolver")) {
            auto resolver = dns.value("final").toString();
            const auto servers = dns.value("servers").toArray();
            if (resolver.isEmpty() && !servers.isEmpty()) resolver = servers.first().toObject().value("tag").toString();
            if (!resolver.isEmpty()) routeObj["default_domain_resolver"] = resolver;
        }
        if (tun && !status->forExport) routeObj["auto_detect_interface"] = true;
        status->result->coreConfig.insert("route", routeObj);

        // experimental
        QJsonObject experimentalObj;

        if (!status->forTest && dataStore->core_box_clash_api > 0) {
            QJsonObject clash_api = {
                {"external_controller", "127.0.0.1:" + Int2String(dataStore->core_box_clash_api)},
                {"secret", dataStore->core_box_clash_api_secret},
                {"external_ui", "dashboard"},
            };
            experimentalObj["clash_api"] = clash_api;
        }

        if (!experimentalObj.isEmpty()) status->result->coreConfig.insert("experimental", experimentalObj);
    }

} // namespace NekoGui
