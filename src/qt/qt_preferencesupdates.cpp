/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Update preferences UI module.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves
 */
#include <QLocale>

#include "qt_preferencesupdates.hpp"
#include "ui_qt_preferencesupdates.h"

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/version.h>
}

PreferencesUpdates::PreferencesUpdates(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PreferencesUpdates)
{
    ui->setupUi(this);

    ui->comboBoxInterval->addItem(tr("Every 6 hours"));
    ui->comboBoxInterval->addItem(tr("Every 12 hours"));
    ui->comboBoxInterval->addItem(tr("Daily"));
    ui->comboBoxInterval->addItem(tr("Weekly"));
    ui->comboBoxInterval->addItem(tr("Monthly"));

    ui->checkBoxAutoUpdate->setChecked(update_auto);
    ui->comboBoxInterval->setCurrentIndex(update_interval);
    ui->comboBoxInterval->setEnabled(update_auto);
    ui->labelInterval->setEnabled(update_auto);
    connect(ui->checkBoxAutoUpdate, &QCheckBox::toggled, ui->comboBoxInterval, &QWidget::setEnabled);
    connect(ui->checkBoxAutoUpdate, &QCheckBox::toggled, ui->labelInterval, &QWidget::setEnabled);

    ui->checkBoxRestartPrompt->setChecked(update_restart_prompt);

    ui->checkBoxPin->setText(tr("Stay on build %1 and don't install updates").arg(update_pin_build ? update_pin_build : EMU_BUILD_NUM));
    ui->checkBoxPin->setChecked(update_pin_build != 0);
    connect(ui->checkBoxPin, &QCheckBox::toggled, this, &PreferencesUpdates::updateButtons);
    connect(ui->pushButtonCheckNow, &QPushButton::clicked, this, &PreferencesUpdates::checkNow);

    ui->textChanges->setVisible(false);

    QString reason;
    supported = Updater::isSupported(&reason);
    ui->labelUnsupported->setText(reason);
    ui->labelUnsupported->setVisible(!supported);
    if (!supported) {
        ui->checkBoxAutoUpdate->setEnabled(false);
        ui->comboBoxInterval->setEnabled(false);
        ui->labelInterval->setEnabled(false);
        ui->checkBoxRestartPrompt->setEnabled(false);
        ui->checkBoxPin->setEnabled(false);
        ui->groupBoxStatus->setEnabled(false);
    }

    const auto updater = Updater::instance();
    connect(updater, &Updater::checkFinished, this, &PreferencesUpdates::checkFinished);
    connect(updater, &Updater::installFinished, this, &PreferencesUpdates::installFinished);
    if (updater->isBusy())
        ui->labelLatest->setText(tr("An update is in progress."));

    updateStatus();
    updateButtons();
}

PreferencesUpdates::~PreferencesUpdates()
{
    delete ui;
}

void
PreferencesUpdates::save()
{
    if (!supported)
        return;

    update_auto           = ui->checkBoxAutoUpdate->isChecked() ? 1 : 0;
    update_interval       = ui->comboBoxInterval->currentIndex();
    update_restart_prompt = ui->checkBoxRestartPrompt->isChecked() ? 1 : 0;
    if (!ui->checkBoxPin->isChecked())
        update_pin_build = 0;
    else if (!update_pin_build)
        update_pin_build = EMU_BUILD_NUM;
}

void
PreferencesUpdates::updateStatus()
{
    const auto    updater   = Updater::instance();
    const int     installed = updater->installedBuild();
    const QString current   = EMU_BUILD_NUM ? QString::number(EMU_BUILD_NUM) : tr("unknown");

    if (installed != EMU_BUILD_NUM)
        ui->labelCurrentBuild->setText(tr("Current build: %1 (build %2 is installed and will be used after a restart)").arg(current).arg(installed));
    else
        ui->labelCurrentBuild->setText(tr("Current build: %1").arg(current));

    const auto lastCheck = updater->lastCheck();
    ui->labelLastCheck->setText(tr("Last checked: %1").arg(lastCheck.isValid() ? QLocale().toString(lastCheck, QLocale::ShortFormat) : tr("never")));
}

void
PreferencesUpdates::updateButtons()
{
    const auto updater   = Updater::instance();
    const bool busy      = updater->isBusy();
    const bool available = (latest.number > updater->installedBuild());

    ui->pushButtonCheckNow->setEnabled(supported && !busy);

    ui->pushButtonInstall->setText(tr("Install build %1").arg(latest.number));
    ui->pushButtonInstall->setVisible(available);
    ui->pushButtonInstall->setEnabled(supported && !busy && !ui->checkBoxPin->isChecked());

    ui->pushButtonSkip->setText(tr("Skip build %1").arg(latest.number));
    ui->pushButtonSkip->setVisible(available);
    ui->pushButtonSkip->setEnabled(supported && !busy && (latest.number != update_skip_build));
}

void
PreferencesUpdates::checkNow()
{
    if (!supported || Updater::instance()->isBusy())
        return;

    ui->labelLatest->setText(tr("Checking for updates..."));
    Updater::instance()->checkNow();
    updateButtons();
}

void
PreferencesUpdates::on_pushButtonInstall_clicked()
{
    ui->labelLatest->setText(tr("Downloading and installing build %1...").arg(latest.number));
    Updater::instance()->install(latest);
    updateButtons();
}

void
PreferencesUpdates::on_pushButtonSkip_clicked()
{
    update_skip_build = latest.number;
    config_save_global_setting("Updates", "skip_build");

    ui->labelLatest->setText(tr("Build %1 will be skipped. Newer builds will still be installed.").arg(latest.number));
    updateButtons();
}

void
PreferencesUpdates::checkFinished(const Updater::Build &build, const QString &error)
{
    latest = error.isEmpty() ? build : Updater::Build();

    if (!error.isEmpty())
        ui->labelLatest->setText(tr("Unable to check for updates: %1").arg(error));
    else if (latest.number <= Updater::instance()->installedBuild())
        ui->labelLatest->setText(tr("You are using the latest build."));
    else if (latest.number == update_skip_build)
        ui->labelLatest->setText(tr("Build %1 is available, but you chose to skip it.").arg(latest.number));
    else
        ui->labelLatest->setText(tr("Build %1 is available.").arg(latest.number));

    ui->textChanges->setPlainText(latest.changes);
    ui->textChanges->setVisible(latest.number > Updater::instance()->installedBuild());

    updateStatus();
    updateButtons();
}

void
PreferencesUpdates::installFinished(int build, const QString &error)
{
    if (!error.isEmpty())
        ui->labelLatest->setText(tr("Unable to install build %1: %2").arg(build).arg(error));
    else
        ui->labelLatest->setText(tr("Build %1 has been installed.").arg(build));

    updateStatus();
    updateButtons();
}
