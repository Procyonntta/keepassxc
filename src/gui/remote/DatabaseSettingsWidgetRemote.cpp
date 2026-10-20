/*
 *  Copyright (C) 2025 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "DatabaseSettingsWidgetRemote.h"
#include "ui_DatabaseSettingsWidgetRemote.h"

#include "core/Database.h"
#include "core/Global.h"
#include "core/Metadata.h"

#include "RemoteHandler.h"
#include "RemoteSettings.h"
#include "RemoteSyncScheduler.h"
#include "gui/MessageBox.h"

#include <QFile>
#include <QFileInfo>

namespace
{
    enum CommandTemplate
    {
        NoTemplate,
        RcloneTemplate,
        ScpTemplate
    };

    // Cloud storage is often slower than the default timeout allows
    constexpr int TemplateTimeoutSec = 30;
} // namespace

DatabaseSettingsWidgetRemote::DatabaseSettingsWidgetRemote(QWidget* parent)
    : DatabaseSettingsWidget(parent)
    , m_remoteSettings(new RemoteSettings(nullptr, this))
    , m_ui(new Ui::DatabaseSettingsWidgetRemote())
{
    m_ui->setupUi(this);
    m_ui->messageWidget->setHidden(true);

    connect(m_ui->saveSettingsButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::saveCurrentSettings);
    connect(
        m_ui->removeSettingsButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::removeCurrentSettings);
    connect(m_ui->settingsListWidget,
            &QListWidget::itemSelectionChanged,
            this,
            &DatabaseSettingsWidgetRemote::editCurrentSettings);
    connect(m_ui->testDownloadCommandButton, &QPushButton::clicked, this, &DatabaseSettingsWidgetRemote::testDownload);

    m_ui->templateComboBox->addItem(tr("Choose a template…"), NoTemplate);
    m_ui->templateComboBox->addItem(tr("rclone (Google Drive, Dropbox, OneDrive, WebDAV, S3, …)"), RcloneTemplate);
    m_ui->templateComboBox->addItem(tr("scp (SSH server)"), ScpTemplate);
    connect(m_ui->templateComboBox,
            QOverload<int>::of(&QComboBox::activated),
            this,
            &DatabaseSettingsWidgetRemote::applyTemplate);

    auto setModified = [this]() { m_modified = true; };
    connect(m_ui->nameLineEdit, &QLineEdit::textChanged, setModified);
    connect(m_ui->downloadCommand, &QLineEdit::textChanged, setModified);
    connect(m_ui->inputForDownload, &QPlainTextEdit::textChanged, setModified);
    connect(m_ui->downloadTimeoutSec, QOverload<int>::of(&QSpinBox::valueChanged), setModified);
    connect(m_ui->uploadCommand, &QLineEdit::textChanged, setModified);
    connect(m_ui->inputForUpload, &QPlainTextEdit::textChanged, setModified);
    connect(m_ui->uploadTimeoutSec, QOverload<int>::of(&QSpinBox::valueChanged), setModified);
}

DatabaseSettingsWidgetRemote::~DatabaseSettingsWidgetRemote() = default;

void DatabaseSettingsWidgetRemote::initialize()
{
    clearFields();
    m_remoteSettings->setDatabase(m_db);
    updateSettingsList();
    if (m_ui->settingsListWidget->count() > 0) {
        m_ui->settingsListWidget->setCurrentRow(0);
        m_ui->removeSettingsButton->setEnabled(true);
    } else {
        m_ui->removeSettingsButton->setDisabled(true);
    }
    loadAutoSyncSettings();
}

void DatabaseSettingsWidgetRemote::uninitialize()
{
}

bool DatabaseSettingsWidgetRemote::saveSettings()
{
    if (m_modified) {
        auto ans = MessageBox::question(this,
                                        tr("Save Remote Settings"),
                                        tr("You have unsaved changes. Do you want to save them?"),
                                        MessageBox::Save | MessageBox::Discard | MessageBox::Cancel,
                                        MessageBox::Save);
        if (ans == MessageBox::Save) {
            saveCurrentSettings();
        } else if (ans == MessageBox::Cancel) {
            return false;
        }
    }

    if (!saveAutoSyncSettings()) {
        return false;
    }
    m_remoteSettings->saveSettings();
    return true;
}

void DatabaseSettingsWidgetRemote::saveCurrentSettings()
{
    QString name = m_ui->nameLineEdit->text();
    if (name.isEmpty()) {
        m_ui->messageWidget->showMessage(tr("Name cannot be empty."), MessageWidget::Warning);
        return;
    }

    auto* params = new RemoteParams();
    params->name = m_ui->nameLineEdit->text();
    params->downloadCommand = m_ui->downloadCommand->text();
    params->downloadInput = m_ui->inputForDownload->toPlainText();
    params->downloadTimeoutMsec = m_ui->downloadTimeoutSec->value() * 1000;
    params->uploadCommand = m_ui->uploadCommand->text();
    params->uploadInput = m_ui->inputForUpload->toPlainText();
    params->uploadTimeoutMsec = m_ui->uploadTimeoutSec->value() * 1000;

    m_remoteSettings->addRemoteParams(params);
    updateSettingsList();

    auto item = findItemByName(name);
    m_ui->settingsListWidget->setCurrentItem(item);
    m_ui->removeSettingsButton->setEnabled(true);
    m_modified = false;
}

QListWidgetItem* DatabaseSettingsWidgetRemote::findItemByName(const QString& name)
{
    return m_ui->settingsListWidget->findItems(name, Qt::MatchExactly).first();
}

void DatabaseSettingsWidgetRemote::removeCurrentSettings()
{
    m_remoteSettings->removeRemoteParams(m_ui->nameLineEdit->text());
    updateSettingsList();
    if (!m_remoteSettings->getAllRemoteParams().empty()) {
        m_ui->settingsListWidget->setCurrentRow(0);
        m_ui->removeSettingsButton->setEnabled(true);
    } else {
        clearFields();
        m_ui->removeSettingsButton->setDisabled(true);
    }
}

void DatabaseSettingsWidgetRemote::editCurrentSettings()
{
    if (!m_ui->settingsListWidget->currentItem()) {
        return;
    }

    QString name = m_ui->settingsListWidget->currentItem()->text();
    auto* params = m_remoteSettings->getRemoteParams(name);
    if (!params) {
        return;
    }

    m_ui->nameLineEdit->setText(params->name);
    m_ui->downloadCommand->setText(params->downloadCommand);
    m_ui->inputForDownload->setPlainText(params->downloadInput);
    m_ui->downloadTimeoutSec->setValue(params->downloadTimeoutMsec / 1000);
    m_ui->uploadCommand->setText(params->uploadCommand);
    m_ui->inputForUpload->setPlainText(params->uploadInput);
    m_ui->uploadTimeoutSec->setValue(params->uploadTimeoutMsec / 1000);
    m_modified = false;
}

void DatabaseSettingsWidgetRemote::updateSettingsList()
{
    m_ui->settingsListWidget->clear();
    for (auto params : m_remoteSettings->getAllRemoteParams()) {
        auto* item = new QListWidgetItem(m_ui->settingsListWidget);
        item->setText(params->name);
        m_ui->settingsListWidget->addItem(item);
    }
    updateAutoSyncRemotes();
}

void DatabaseSettingsWidgetRemote::updateAutoSyncRemotes()
{
    auto current = m_ui->autoSyncRemoteComboBox->currentText();
    m_ui->autoSyncRemoteComboBox->clear();
    for (auto params : m_remoteSettings->getAllRemoteParams()) {
        m_ui->autoSyncRemoteComboBox->addItem(params->name);
    }

    int index = m_ui->autoSyncRemoteComboBox->findText(current);
    if (index >= 0) {
        m_ui->autoSyncRemoteComboBox->setCurrentIndex(index);
    }
}

void DatabaseSettingsWidgetRemote::loadAutoSyncSettings()
{
    // A database that was never saved has no file to bind the setting to
    auto filePath = m_db ? m_db->canonicalFilePath() : QString();
    m_ui->autoSyncGroupBox->setEnabled(!filePath.isEmpty());

    auto settings = RemoteSyncScheduler::settings(filePath);
    m_ui->autoSyncGroupBox->setChecked(settings.isEnabled());
    if (!settings.isEnabled()) {
        return;
    }

    int index = m_ui->autoSyncRemoteComboBox->findText(settings.remoteName);
    if (index >= 0) {
        m_ui->autoSyncRemoteComboBox->setCurrentIndex(index);
    }
    m_ui->autoSyncOnUnlockCheckBox->setChecked(settings.onUnlock);
    m_ui->autoSyncOnSaveCheckBox->setChecked(settings.onSave);
    m_ui->autoSyncIntervalSpinBox->setValue(settings.intervalMin);
}

bool DatabaseSettingsWidgetRemote::saveAutoSyncSettings()
{
    if (!m_db || m_db->canonicalFilePath().isEmpty()) {
        return true;
    }

    AutoSyncSettings settings;
    if (m_ui->autoSyncGroupBox->isChecked()) {
        settings.remoteName = m_ui->autoSyncRemoteComboBox->currentText();
        settings.onUnlock = m_ui->autoSyncOnUnlockCheckBox->isChecked();
        settings.onSave = m_ui->autoSyncOnSaveCheckBox->isChecked();
        settings.intervalMin = m_ui->autoSyncIntervalSpinBox->value();
        if (!settings.isEnabled()) {
            m_ui->messageWidget->showMessage(tr("Choose a sync command and at least one moment to sync automatically."),
                                             MessageWidget::Warning);
            return false;
        }
    }

    RemoteSyncScheduler::setSettings(m_db->canonicalFilePath(), settings);
    return true;
}

void DatabaseSettingsWidgetRemote::applyTemplate(int index)
{
    auto commandTemplate = m_ui->templateComboBox->itemData(index).toInt();
    m_ui->templateComboBox->setCurrentIndex(0);
    if (commandTemplate == NoTemplate) {
        return;
    }

    // Name the remote file like the local one, the user adjusts the location
    auto fileName = m_db ? QFileInfo(m_db->filePath()).fileName() : QString();
    if (fileName.isEmpty()) {
        fileName = QStringLiteral("Database.kdbx");
    }

    QString name;
    QString remoteFile;
    if (commandTemplate == RcloneTemplate) {
        name = QStringLiteral("rclone");
        remoteFile = QStringLiteral("remote:%1").arg(fileName);
        m_ui->downloadCommand->setText(QStringLiteral("rclone copyto \"%1\" \"{TEMP_DATABASE}\"").arg(remoteFile));
        m_ui->uploadCommand->setText(QStringLiteral("rclone copyto \"{TEMP_DATABASE}\" \"%1\"").arg(remoteFile));
    } else {
        name = QStringLiteral("scp");
        remoteFile = QStringLiteral("user@hostname:%1").arg(fileName);
        m_ui->downloadCommand->setText(QStringLiteral("scp \"%1\" \"{TEMP_DATABASE}\"").arg(remoteFile));
        m_ui->uploadCommand->setText(QStringLiteral("scp \"{TEMP_DATABASE}\" \"%1\"").arg(remoteFile));
    }

    m_ui->inputForDownload->clear();
    m_ui->inputForUpload->clear();
    m_ui->downloadTimeoutSec->setValue(TemplateTimeoutSec);
    m_ui->uploadTimeoutSec->setValue(TemplateTimeoutSec);
    if (m_ui->nameLineEdit->text().isEmpty()) {
        m_ui->nameLineEdit->setText(name);
    }

    m_ui->messageWidget->showMessage(
        tr("Replace %1 with the location of your database in both commands, then press Save.").arg(remoteFile),
        MessageWidget::Information);
}

void DatabaseSettingsWidgetRemote::clearFields()
{
    m_ui->nameLineEdit->setText("");
    m_ui->downloadCommand->setText("");
    m_ui->inputForDownload->setPlainText("");
    m_ui->downloadTimeoutSec->setValue(10);
    m_ui->uploadCommand->setText("");
    m_ui->inputForUpload->setPlainText("");
    m_ui->uploadTimeoutSec->setValue(10);
    m_modified = false;
}

void DatabaseSettingsWidgetRemote::testDownload()
{
    RemoteParams params;
    params.name = m_ui->nameLineEdit->text();
    params.downloadCommand = m_ui->downloadCommand->text();
    params.downloadInput = m_ui->inputForDownload->toPlainText();
    params.downloadTimeoutMsec = m_ui->downloadTimeoutSec->value() * 1000;

    QScopedPointer<RemoteHandler> remoteHandler(new RemoteHandler());
    if (params.downloadCommand.isEmpty()) {
        m_ui->messageWidget->showMessage(tr("Download command cannot be empty."), MessageWidget::Warning);
        return;
    }

    RemoteHandler::RemoteResult result = remoteHandler->download(&params);
    if (!result.success) {
        m_ui->messageWidget->showMessage(tr("Download failed with error: %1").arg(result.errorMessage),
                                         MessageWidget::Error);
        return;
    }

    if (!QFile::exists(result.filePath)) {
        m_ui->messageWidget->showMessage(tr("Download finished, but file %1 could not be found.").arg(result.filePath),
                                         MessageWidget::Error);
        return;
    }

    // Only checking that the command works, the copy is not needed
    QFile::remove(result.filePath);
    m_ui->messageWidget->showMessage(tr("Download successful."), MessageWidget::Positive);
}
