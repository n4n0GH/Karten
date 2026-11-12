// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QQmlEngine>

#include <memory>

namespace KWallet
{
class Wallet;
}

/// Keeps secrets in the user's wallet (KWallet) rather than in a settings
/// file: for now the key for Google's Places API.
///
/// The wallet is only opened when asked to, as that may prompt for the
/// wallet's password.
class KeyStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// Empty until open() has succeeded, or when none is stored.
    Q_PROPERTY(QString googleKey READ googleKey WRITE setGoogleKey NOTIFY googleKeyChanged)
    /// Whether the wallet is open, so that the key can be read and changed.
    Q_PROPERTY(bool ready READ isReady NOTIFY stateChanged)
    /// Why the wallet cannot be used, for the user; empty when it can.
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit KeyStore(QObject *parent = nullptr);
    ~KeyStore() override;

    QString googleKey() const;
    void setGoogleKey(const QString &key);
    bool isReady() const;
    QString error() const;

    /// Opens the wallet and reads the key. Does nothing when that has
    /// happened already or is under way.
    Q_INVOKABLE void open();

Q_SIGNALS:
    void googleKeyChanged();
    void stateChanged();

private:
    void opened(bool success);
    void fail(const QString &error);

    std::unique_ptr<KWallet::Wallet> m_wallet;
    bool m_ready = false;
    QString m_googleKey;
    QString m_error;
};
