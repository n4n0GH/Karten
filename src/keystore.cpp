// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keystore.h"

#include <KLocalizedString>
#include <KWallet>

#include <QGuiApplication>
#include <QSettings>
#include <QWindow>

using namespace Qt::StringLiterals;

namespace
{
const QString Folder = u"Karten"_s;
const QString GoogleEntry = u"Google Places API key"_s;
}

KeyStore::KeyStore(QObject *parent)
    : QObject(parent)
{
}

KeyStore::~KeyStore() = default;

QString KeyStore::googleKey() const
{
    return m_googleKey;
}

bool KeyStore::isReady() const
{
    return m_ready;
}

QString KeyStore::error() const
{
    return m_error;
}

void KeyStore::fail(const QString &error)
{
    m_wallet.reset();
    m_ready = false;
    m_error = error;
    Q_EMIT stateChanged();
}

void KeyStore::open()
{
    if (m_wallet) {
        return;
    }
    if (!KWallet::Wallet::isEnabled()) {
        fail(i18nc("@info", "The wallet system (KWallet) is switched off, so the key cannot be kept safely. Switch it on in System Settings."));
        return;
    }
    // The wallet's own dialogs belong to the window that asked.
    const QWindowList windows = QGuiApplication::topLevelWindows();
    const WId window = windows.isEmpty() ? 0 : windows.first()->winId();
    m_wallet.reset(KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), window, KWallet::Wallet::Asynchronous));
    if (!m_wallet) {
        fail(i18nc("@info", "The wallet could not be opened."));
        return;
    }
    connect(m_wallet.get(), &KWallet::Wallet::walletOpened, this, &KeyStore::opened);
    connect(m_wallet.get(), &KWallet::Wallet::walletClosed, this, [this] {
        // Closed from outside; what was read stays in use until Karten quits.
        m_wallet.release()->deleteLater();
        m_ready = false;
        Q_EMIT stateChanged();
    });
}

void KeyStore::opened(bool success)
{
    if (!success || !(m_wallet->hasFolder(Folder) || m_wallet->createFolder(Folder)) || !m_wallet->setFolder(Folder)) {
        fail(i18nc("@info", "The wallet was not opened, so the key is not available."));
        return;
    }
    QString key;
    m_wallet->readPassword(GoogleEntry, key);

    // Versions before 0.2 kept the key in the settings file. Move it over.
    QSettings settings;
    settings.beginGroup(u"Google"_s);
    const QString legacy = settings.value(u"key"_s).toString();
    if (settings.contains(u"key"_s)) {
        if (key.isEmpty() && !legacy.trimmed().isEmpty() && m_wallet->writePassword(GoogleEntry, legacy.trimmed()) == 0) {
            key = legacy.trimmed();
        }
        // Only once it is safe in the wallet, or was empty anyway.
        if (legacy.trimmed().isEmpty() || key == legacy.trimmed()) {
            settings.remove(u"key"_s);
            settings.sync();
        }
    }

    m_ready = true;
    m_error.clear();
    Q_EMIT stateChanged();
    if (key != m_googleKey) {
        m_googleKey = key;
        Q_EMIT googleKeyChanged();
    }
}

void KeyStore::setGoogleKey(const QString &key)
{
    const QString value = key.trimmed();
    if (!m_ready || value == m_googleKey) {
        return;
    }
    const int result = value.isEmpty() ? m_wallet->removeEntry(GoogleEntry) : m_wallet->writePassword(GoogleEntry, value);
    if (result != 0 && !value.isEmpty()) {
        m_error = i18nc("@info", "The key could not be saved in the wallet.");
        Q_EMIT stateChanged();
        return;
    }
    m_googleKey = value;
    Q_EMIT googleKeyChanged();
}
