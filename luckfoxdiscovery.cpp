#include "luckfoxdiscovery.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSettings>
#include <QUuid>

namespace { constexpr quint16 kDiscoveryPort = 39093; }

LuckfoxDiscovery::LuckfoxDiscovery(QObject *parent) : QObject(parent)
{
    m_retry.setInterval(1000);
    m_deadline.setSingleShot(true);
    connect(&m_socket, &QUdpSocket::readyRead, this, &LuckfoxDiscovery::receive);
    connect(&m_retry, &QTimer::timeout, this, &LuckfoxDiscovery::probe);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        cancel();
        emit failed(QStringLiteral("未找到 Luckfox。请确认板子已开机、热点已开启，电脑连接同一网络；板子刚上电时请稍等约一分钟再试。"));
    });
}

void LuckfoxDiscovery::start()
{
    cancel();
    if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        emit failed(QStringLiteral("无法启动设备查找：%1").arg(m_socket.errorString()));
        return;
    }
    QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
    m_cachedAddress = QHostAddress(settings.value(QStringLiteral("lastAddress")).toString());
    m_deviceId = settings.value(QStringLiteral("deviceId")).toString();
    m_broadcasts.clear();
    m_subnets.clear();
    const auto interfaces = QNetworkInterface::allInterfaces();
    bool hasWifi = false;
    for (const auto &interface : interfaces) {
        if (interface.type() == QNetworkInterface::Wifi &&
            interface.flags().testFlag(QNetworkInterface::IsUp) &&
            interface.flags().testFlag(QNetworkInterface::IsRunning)) hasWifi = true;
    }
    for (const auto &interface : interfaces) {
        if (hasWifi && interface.type() != QNetworkInterface::Wifi) continue;
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                entry.prefixLength() < 1 || entry.prefixLength() > 30) continue;
            if (!entry.broadcast().isNull() && !m_broadcasts.contains(entry.broadcast()))
                m_broadcasts.append(entry.broadcast());
            // Unicast fallback for hotspots that suppress client broadcasts.
            // Keep discovery bounded to the local /24 even on larger networks.
            const quint32 subnet = entry.ip().toIPv4Address() & 0xffffff00U;
            if (!m_subnets.contains(subnet)) m_subnets.append(subnet);
        }
    }
    m_nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_request = QJsonDocument(QJsonObject{{"type", "luckfox-discover"},
                                        {"version", 1}, {"nonce", m_nonce}}).toJson(QJsonDocument::Compact);
    m_active = true;
    m_attempt = 0;
    m_deadline.start(6500);
    m_retry.start();
    probe();
}

void LuckfoxDiscovery::probe()
{
    if (!m_active) return;
    for (const auto &address : m_broadcasts)
        m_socket.writeDatagram(m_request, address, kDiscoveryPort);
    if (m_cachedAddress.protocol() == QAbstractSocket::IPv4Protocol &&
        m_subnets.contains(m_cachedAddress.toIPv4Address() & 0xffffff00U))
        m_socket.writeDatagram(m_request, m_cachedAddress, kDiscoveryPort);
    if (++m_attempt == 2 || m_attempt == 4) {
        for (quint32 subnet : m_subnets)
            for (quint32 host = 1; host < 255; ++host)
                m_socket.writeDatagram(m_request, QHostAddress(subnet | host), kDiscoveryPort);
    }
}

void LuckfoxDiscovery::receive()
{
    while (m_socket.hasPendingDatagrams()) {
        const auto datagram = m_socket.receiveDatagram(1024);
        if (!m_active || datagram.senderPort() != kDiscoveryPort) continue;
        const auto reply = QJsonDocument::fromJson(datagram.data()).object();
        const QString id = reply.value("device_id").toString();
        if (reply.value("type").toString() != QStringLiteral("luckfox-camera") ||
            reply.value("version").toInt() != 1 ||
            reply.value("nonce").toString() != m_nonce ||
            !id.startsWith(QStringLiteral("luckfox-")) ||
            (!m_deviceId.isEmpty() && id != m_deviceId) ||
            reply.value("rtsp_port").toInt() != 554 ||
            reply.value("path").toString() != QStringLiteral("/live/0")) continue;
        const QString address = datagram.senderAddress().toString();
        QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
        settings.setValue(QStringLiteral("lastAddress"), address);
        settings.setValue(QStringLiteral("deviceId"), id);
        cancel();
        emit found(QStringLiteral("rtsp://%1/live/0").arg(address));
        return;
    }
}

void LuckfoxDiscovery::cancel()
{
    m_active = false;
    m_retry.stop();
    m_deadline.stop();
    m_socket.close();
}
