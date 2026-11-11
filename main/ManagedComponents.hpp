#pragma once

#include <QString>

namespace NekoGui {
    QString ManagedComponentsRoot();
    QString ManagedComponentFile(const QString &component, const QString &name);
    QString ManagedCoreProgram(const QString &core);
    QString UpdateHelperProgram();
}
