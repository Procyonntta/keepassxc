/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
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

#ifndef KEEPASSXC_REMOTESYNCSCHEDULER_H
#define KEEPASSXC_REMOTESYNCSCHEDULER_H

#include <QObject>
#include <QTimer>

struct AutoSyncSettings
{
    QString remoteName;
    bool onUnlock = false;
    bool onSave = false;
    int intervalMin = 0;

    bool isEnabled() const;
};

/**
 * Decides when an unlocked database is synced with one of its remotes automatically.
 *
 * The settings are kept in the local config per database file and never inside the
 * database itself. Remote commands travel with the database file, so they must not
 * run without an explicit opt-in on this device.
 */
class RemoteSyncScheduler : public QObject
{
    Q_OBJECT

public:
    explicit RemoteSyncScheduler(QObject* parent = nullptr);
    ~RemoteSyncScheduler() override = default;

    static AutoSyncSettings settings(const QString& filePath);
    static void setSettings(const QString& filePath, const AutoSyncSettings& settings);

    void setFilePath(const QString& filePath);
    AutoSyncSettings currentSettings() const;
    bool isActive() const;
    bool isFailing() const;

    void databaseUnlocked();
    void databaseSaved();
    void databaseLocked();
    void syncFinished(bool success);
    void requestSync();

    // Used for testing only
    static void setSaveDelay(int msec);

signals:
    void syncRequested(const QString& remoteName);
    void settingsChanged();

private slots:
    void reloadSettings();

private:
    QString m_filePath;
    AutoSyncSettings m_settings;
    bool m_active = false;
    int m_failures = 0;

    QTimer m_saveTimer;
    QTimer m_intervalTimer;
    QTimer m_retryTimer;

    static int m_saveDelayMsec;

    Q_DISABLE_COPY(RemoteSyncScheduler)
};

#endif // KEEPASSXC_REMOTESYNCSCHEDULER_H
