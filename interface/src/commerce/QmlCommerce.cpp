//
//  QmlCommerce.cpp
//  interface/src/commerce
//
//  Created by Howard Stearns on 8/4/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "QmlCommerce.h"
#include "Application.h"
#include "DependencyManager.h"
#include "Ledger.h"
#include "Wallet.h"
#include <AccountManager.h>
#include "scripting/WalletScriptingInterface.h"

HIFI_QML_DEF(QmlCommerce)

QmlCommerce::QmlCommerce(QQuickItem* parent) : OffscreenQmlDialog(parent) {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    connect(ledger.data(), &Ledger::buyResult, this, &QmlCommerce::buyResult);
    connect(ledger.data(), &Ledger::balanceResult, this, &QmlCommerce::balanceResult);
    connect(ledger.data(), &Ledger::inventoryResult, this, &QmlCommerce::inventoryResult);
    connect(wallet.data(), &Wallet::securityImageResult, this, &QmlCommerce::securityImageResult);
    connect(ledger.data(), &Ledger::historyResult, this, &QmlCommerce::historyResult);
    connect(wallet.data(), &Wallet::keyFilePathIfExistsResult, this, &QmlCommerce::keyFilePathIfExistsResult);
    connect(ledger.data(), &Ledger::accountResult, this, &QmlCommerce::accountResult);
    connect(ledger.data(), &Ledger::accountResult, this, [&]() {
        auto wallet = DependencyManager::get<Wallet>();
        auto walletScriptingInterface = DependencyManager::get<WalletScriptingInterface>();
        uint status;

        if (wallet->getKeyFilePath() == "" || !wallet->getSecurityImage()) {
            status = (uint)WalletStatus::WALLET_STATUS_NOT_SET_UP;
        } else if (!wallet->walletIsAuthenticatedWithPassphrase()) {
            status = (uint)WalletStatus::WALLET_STATUS_NOT_AUTHENTICATED;
        } else {
            status = (uint)WalletStatus::WALLET_STATUS_READY;
        }

        walletScriptingInterface->setWalletStatus(status);
        emit walletStatusResult(status);
    });
}

void QmlCommerce::getWalletStatus() {
    auto walletScriptingInterface = DependencyManager::get<WalletScriptingInterface>();
    uint status;

    if (DependencyManager::get<AccountManager>()->isLoggedIn()) {
        // This will set account info for the wallet, allowing us to decrypt and display the security image.
        account();
    } else {
        status = (uint)WalletStatus::WALLET_STATUS_NOT_LOGGED_IN;
        emit walletStatusResult(status);
        walletScriptingInterface->setWalletStatus(status);
        return;
    }
}

void QmlCommerce::getLoginStatus() {
    emit loginStatusResult(DependencyManager::get<AccountManager>()->isLoggedIn());
}

void QmlCommerce::getKeyFilePathIfExists() {
    auto wallet = DependencyManager::get<Wallet>();
    emit keyFilePathIfExistsResult(wallet->getKeyFilePath());
}

void QmlCommerce::getWalletAuthenticatedStatus() {
    auto wallet = DependencyManager::get<Wallet>();
    emit walletAuthenticatedStatusResult(wallet->walletIsAuthenticatedWithPassphrase());
}

void QmlCommerce::getSecurityImage() {
    auto wallet = DependencyManager::get<Wallet>();
    wallet->getSecurityImage();
}

void QmlCommerce::chooseSecurityImage(const QString& imageFile) {
    auto wallet = DependencyManager::get<Wallet>();
    wallet->chooseSecurityImage(imageFile);
}

void QmlCommerce::buy(const QString& assetId, int cost, const bool controlledFailure) {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    QStringList keys = wallet->listPublicKeys();
    if (keys.count() == 0) {
        QJsonObject result{ { "status", "fail" }, { "message", "Uninitialized Wallet." } };
        return emit buyResult(result);
    }
    QString key = keys[0];
    // For now, we receive at the same key that pays for it.
    ledger->buy(key, cost, assetId, key, controlledFailure);
}

void QmlCommerce::balance() {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    ledger->balance(wallet->listPublicKeys());
}

void QmlCommerce::inventory() {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    ledger->inventory(wallet->listPublicKeys());
}

void QmlCommerce::history() {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    ledger->history(wallet->listPublicKeys());
}

void QmlCommerce::changePassphrase(const QString& oldPassphrase, const QString& newPassphrase) {
    auto wallet = DependencyManager::get<Wallet>();
    if (wallet->getPassphrase()->isEmpty()) {
        emit changePassphraseStatusResult(wallet->setPassphrase(newPassphrase));
    } else if (wallet->getPassphrase() == oldPassphrase && !newPassphrase.isEmpty()) {
        emit changePassphraseStatusResult(wallet->changePassphrase(newPassphrase));
    } else {
        emit changePassphraseStatusResult(false);
    }
}

void QmlCommerce::setPassphrase(const QString& passphrase) {
    auto wallet = DependencyManager::get<Wallet>();
    wallet->setPassphrase(passphrase);
    getWalletAuthenticatedStatus();
}

void QmlCommerce::generateKeyPair() {
    auto wallet = DependencyManager::get<Wallet>();
    wallet->generateKeyPair();
    getWalletAuthenticatedStatus();
}

void QmlCommerce::reset() {
    auto ledger = DependencyManager::get<Ledger>();
    auto wallet = DependencyManager::get<Wallet>();
    ledger->reset();
    wallet->reset();
}

void QmlCommerce::account() {
    auto ledger = DependencyManager::get<Ledger>();
    ledger->account();
}
