#ifndef QT_PREFERENCESUPDATES_HPP
#define QT_PREFERENCESUPDATES_HPP

#include <QWidget>

#include "qt_updater.hpp"

namespace Ui {
class PreferencesUpdates;
}

/* The preferences of the automatic updater, with a way to check for and
   install, or skip, the latest build right away. */
class PreferencesUpdates : public QWidget {
    Q_OBJECT

public:
    explicit PreferencesUpdates(QWidget *parent = nullptr);
    ~PreferencesUpdates();

    void save();
    void checkNow();

private slots:
    void on_pushButtonInstall_clicked();
    void on_pushButtonSkip_clicked();
    void checkFinished(const Updater::Build &latest, const QString &error);
    void installFinished(int build, const QString &error);

private:
    void updateStatus();
    void updateButtons();

    Ui::PreferencesUpdates *ui;
    Updater::Build          latest;
    bool                    supported;
};

#endif // QT_PREFERENCESUPDATES_HPP
