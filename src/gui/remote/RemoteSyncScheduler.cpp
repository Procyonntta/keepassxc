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

#include "RemoteSyncScheduler.h"

#include "core/Config.h"

namespace
{
    constexpr int MaxRetryDelayMin = 30;
    constexpr int MinuteMsec = 60 * 1000;
} // namespace

int RemoteSyncScheduler::m_saveDelayMsec = 5000;

bool AutoSyncSettings::isEnabled() const
{
    return !remoteName.isEmpty() && (onUnlock || onSave || intervalMin > 0);
}

RemoteSyncScheduler::RemoteSyncScheduler(QObject* parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_retryTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, &RemoteSyncScheduler::requestSync);
    connect(&m_intervalTimer, &QTimer::timeout, this, &RemoteSyncScheduler::requestSync);
    connect(&m_retryTimer, &QTimer::timeout, this, &RemoteSyncScheduler::requestSync);
    connect(config(), &Config::changed, this, [this](Config::ConfigKey key) {
        if (key == Config::RemoteSync_AutoSync) {
            reloadSettings();
        }
    });
}

AutoSyncSettings RemoteSyncScheduler::settings(const QString& filePath)
{
    AutoSyncSettings settings;
    if (filePath.isEmpty()) {
        return settings;
    }

    auto values = config()->get(Config::RemoteSync_AutoSync).toHash().value(filePath).toMap();
    settings.remoteName = values.value("remote").toString();
    settings.onUnlock = values.value("onUnlock").toBool();
    settings.onSave = values.value("onSave").toBool();
    settings.intervalMin = values.value("intervalMin").toInt();
    return settings;
}

void RemoteSyncScheduler::setSettings(const QString& filePath, const AutoSyncSettings& settings)
{
    if (filePath.isEmpty()) {
        return;
    }

    auto allSettings = config()->get(Config::RemoteSync_AutoSync).toHash();
    if (settings.isEnabled()) {
        QVariantMap values;
        values.insert("remote", settings.remoteName);
        values.insert("onUnlock", settings.onUnlock);
        values.insert("onSave", settings.onSave);
        values.insert("intervalMin", settings.intervalMin);
        allSettings.insert(filePath, values);
    } else {
        allSettings.remove(filePath);
    }
    config()->set(Config::RemoteSync_AutoSync, allSettings);
}

void RemoteSyncScheduler::setFilePath(const QString& filePath)
{
    if (m_filePath != filePath) {
        m_filePath = filePath;
        reloadSettings();
    }
}

AutoSyncSettings RemoteSyncScheduler::currentSettings() const
{
    return m_settings;
}

bool RemoteSyncScheduler::isActive() const
{
    return m_active && m_settings.isEnabled();
}

bool RemoteSyncScheduler::isFailing() const
{
    return m_failures > 0;
}

void RemoteSyncScheduler::databaseUnlocked()
{
    m_active = true;
    m_failures = 0;
    reloadSettings();
    if (isActive() && m_settings.onUnlock) {
        // Let the unlock finish before syncing
        QTimer::singleShot(0, this, &RemoteSyncScheduler::requestSync);
    }
}

void RemoteSyncScheduler::databaseSaved()
{
    if (isActive() && m_settings.onSave) {
        // Coalesce bursts of saves (e.g. autosave after every change) into one sync
        m_saveTimer.start(m_saveDelayMsec);
    }
}

void RemoteSyncScheduler::databaseLocked()
{
    m_active = false;
    m_failures = 0;
    m_saveTimer.stop();
    m_intervalTimer.stop();
    m_retryTimer.stop();
}

void RemoteSyncScheduler::syncFinished(bool success)
{
    if (success) {
        m_failures = 0;
        m_retryTimer.stop();
        return;
    }

    ++m_failures;
    if (isActive()) {
        // Back off on repeated failures: 1, 2, 4, ... up to 30 minutes
        int delayMin = qMin(1 << qMin(m_failures - 1, 5), MaxRetryDelayMin);
        m_retryTimer.start(delayMin * MinuteMsec);
    }
}

void RemoteSyncScheduler::requestSync()
{
    if (!isActive()) {
        return;
    }
    m_saveTimer.stop();
    m_retryTimer.stop();
    emit syncRequested(m_settings.remoteName);
}

void RemoteSyncScheduler::setSaveDelay(int msec)
{
    m_saveDelayMsec = msec;
}

void RemoteSyncScheduler::reloadSettings()
{
    m_settings = settings(m_filePath);
    emit settingsChanged();

    if (!m_settings.isEnabled()) {
        m_saveTimer.stop();
        m_intervalTimer.stop();
        m_retryTimer.stop();
        return;
    }

    if (!m_settings.onSave) {
        m_saveTimer.stop();
    }

    if (m_active && m_settings.intervalMin > 0) {
        int interval = m_settings.intervalMin * MinuteMsec;
        if (!m_intervalTimer.isActive() || m_intervalTimer.interval() != interval) {
            m_intervalTimer.start(interval);
        }
    } else {
        m_intervalTimer.stop();
    }
}
