/*
  This file is part of FatCRM, a desktop application for SugarCRM written by KDAB.

  Copyright (C) 2015 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Authors: David Faure <david.faure@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "passwordhandler.h"

#include <settings.h>

#if USE_KWALLET
#include <KWallet/Wallet>
using KWallet::Wallet;

static const char s_walletFolderName[] = "SugarCRM";
#endif

// Implementation inspired from kdepim-runtime/resources/imap/settings.cpp branch KDE/4.14

PasswordHandler::PasswordHandler(const QString &resourceId, QObject *parent) :
    QObject(parent),
    mResourceId(resourceId)
{
#if USE_KWALLET
    // We have no GUI to use on startup
    // We could at least use the config dialog though.
    m_winId = 0;

    Wallet *wallet = Wallet::openWallet( Wallet::NetworkWallet(), m_winId, Wallet::Asynchronous );
    if ( wallet ) {
      connect( wallet, SIGNAL(walletOpened(bool)),
               this, SLOT(onWalletOpened(bool)) );
    }
#endif
}

bool PasswordHandler::isPasswordAvailable()
{
#if USE_KWALLET
    QScopedPointer<Wallet> wallet(Wallet::openWallet(Wallet::NetworkWallet(), m_winId));
    return wallet && wallet->isOpen();
#else
    return true;
#endif
}

QString PasswordHandler::password(bool *userRejected)
{
    if (userRejected != 0) {
        *userRejected = false;
    }

    if (!mPassword.isEmpty())
        return mPassword;
#if USE_KWALLET
    QScopedPointer<Wallet> wallet(Wallet::openWallet(Wallet::NetworkWallet(), m_winId));
    if (wallet && wallet->isOpen()) {
        if (wallet->hasFolder(QString(s_walletFolderName))) {
            wallet->setFolder(QString(s_walletFolderName));
            wallet->readPassword(mResourceId, mPassword);
        } else {
            wallet->createFolder(QString(s_walletFolderName));
        }
        // Initial migration: password not in wallet yet
        if (mPassword.isEmpty()) {
            mPassword = Settings::password();
            if (!mPassword.isEmpty()) {
                savePassword();
            }
        }
    } else if (userRejected != 0) {
        *userRejected = true;
    }
#else
    mPassword = Settings::password();
#endif
    return mPassword;

}

void PasswordHandler::setPassword(const QString &password)
{
    if (password == mPassword)
        return;

    mPassword = password;

#if USE_KWALLET
    savePassword();
#else
    Settings::setPassword(mPassword);
    Settings::self()->writeConfig();
#endif
}

void PasswordHandler::onWalletOpened(bool success)
{
#if USE_KWALLET
    Wallet *wallet = qobject_cast<Wallet*>( sender() );
    if (wallet && success) {
        // read and store the password
        password();
        emit passwordAvailable();
    }
    if (wallet) {
        wallet->deleteLater();
    }
#else
    Q_UNUSED(success);
#endif
}

#if USE_KWALLET
bool PasswordHandler::savePassword()
{
    QScopedPointer<Wallet> wallet(Wallet::openWallet(Wallet::NetworkWallet(), m_winId));
    if (wallet && wallet->isOpen()) {
        if (!wallet->hasFolder(QString(s_walletFolderName)))
            wallet->createFolder(QString(s_walletFolderName));
        wallet->setFolder(QString(s_walletFolderName));
        wallet->writePassword(mResourceId, mPassword);
        wallet->sync();
        Settings::setPassword(QString()); // ensure no plain-text password from before in the config file
        Settings::self()->writeConfig();
        return true;
    }
    return false;
}
#endif
