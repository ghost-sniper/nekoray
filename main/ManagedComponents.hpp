#pragma once

#include <QString>
#include <QProcessEnvironment>

namespace NekoGui {
    QString ManagedComponentsRoot();
    QString ManagedComponentFile(const QString &component, const QString &name);
    QString ManagedCoreProgram(const QString &core);
    QString ManagedComponentVersion(const QString &component);
    QProcessEnvironment SingBoxEnvironment(const QString &program);
    QString UpdateHelperProgram();
}
