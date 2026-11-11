#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcess>
#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    UpdateDialog(const QString &root, bool portable, std::function<bool(const QString &)> canApply,
                 std::function<void()> stopProfile, QWidget *parent);

    QString appRevision() const { return restartRevision; }

protected:
    void reject() override;

private:
    struct Row {
        QString id;
        QString title;
        QString installed;
        QString available;
        QString state;
        QString revision;
        QString error;
        QPushButton *action = nullptr;
    };

    QString root;
    QString helper;
    bool portable;
    std::function<bool(const QString &)> canApply;
    std::function<void()> stopProfile;
    QList<Row> rows;
    QProcess *process;
    QTableWidget *table;
    QPushButton *checkButton;
    QPushButton *updateButton;
    QPushButton *closeButton;
    QLabel *status;
    QString operation;
    QString restartRevision;
    QByteArray diagnostics;
    QStringList pending;
    QStringList autoActivate;

    void check();
    void updateSelected();
    void run(const QString &command, const QList<int> &indexes);
    void readEvents();
    void finish(int code, QProcess::ExitStatus exitStatus);
    void performAction(int index);
    void refresh();
};
