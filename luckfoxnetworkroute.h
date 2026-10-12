#ifndef LUCKFOXNETWORKROUTE_H
#define LUCKFOXNETWORKROUTE_H

#include <QNetworkInterface>

namespace LuckfoxNetworkRoute {
inline QList<QNetworkAddressEntry> ethernetAddresses()
{
    QList<QNetworkAddressEntry> result;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (interface.type() != QNetworkInterface::Ethernet ||
            !flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                entry.prefixLength() > 0 && entry.prefixLength() <= 30)
                result.append(entry);
        }
    }
    return result;
}

inline bool isOnEthernetSubnet(const QHostAddress &address,
                              const QList<QNetworkAddressEntry> &entries)
{
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) return false;
    for (const auto &entry : entries) {
        if (address != entry.ip() && address.isInSubnet(entry.ip(), entry.prefixLength())) return true;
    }
    return false;
}
}
#endif
