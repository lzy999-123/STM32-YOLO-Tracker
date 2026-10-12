#include "luckfoxdiscovery.h"
#include "networkcontroller.h"
#include "luckfoxnetworkroute.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSettings>
#include <QUuid>
#include <QDebug>

namespace {
constexpr quint16 kDiscoveryPort = 39093;
constexpr int kMissingRoundsBeforeNotice = 3;
}

LuckfoxDiscovery::LuckfoxDiscovery(QObject *parent) : QObject(parent)
{
    m_retry.setInterval(250);
    m_deadline.setSingleShot(true);
    m_candidateWait.setSingleShot(true);
    m_candidateWait.setInterval(150);
    connect(&m_candidateWait, &QTimer::timeout, this, &LuckfoxDiscovery::acceptCandidate);
    connect(&m_socket, &QUdpSocket::readyRead, this, &LuckfoxDiscovery::receive);
    connect(&m_retry, &QTimer::timeout, this, &LuckfoxDiscovery::probe);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        const QString locked = m_deviceId;
        const QString other = m_roundOtherId;
        cancel();
        if (!locked.isEmpty() && !other.isEmpty()) {
            // 只统计“锁定设备无回应但有其他设备回应”的连续轮次；达到阈值提示一次后重新计数。
            if (++m_missingRounds >= kMissingRoundsBeforeNotice) {
                m_missingRounds = 0;
                emit lockedDeviceMissing(other);
            }
            emit failed(QStringLiteral("已记住的 Luckfox（%1）未响应，但网络中有其他 Luckfox（%2）。如已更换设备，请选择忘记旧设备后重新查找。")
                            .arg(locked, other));
            return;
        }
        m_missingRounds = 0;
        emit failed(QStringLiteral("未找到 Luckfox。请确认板子已开机、热点已开启，电脑连接同一网络；板子刚上电时请稍等约一分钟再试。"));
    });
    m_authKey = NetworkController::loadAuthKey(&m_authError);
}

void LuckfoxDiscovery::setAuthKey(const QByteArray &key)
{
    m_authKey = key;
    m_authError.clear();
}

QString LuckfoxDiscovery::lockedDeviceId() const
{
    if (m_active) return m_deviceId;
    return QSettings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera")).value(QStringLiteral("deviceId")).toString();
}

void LuckfoxDiscovery::forgetDevice()
{
    m_deviceId.clear();
    m_roundOtherId.clear();
    m_missingRounds = 0;
    QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
    settings.remove(QStringLiteral("deviceId"));
}

void LuckfoxDiscovery::start()
{
    cancel();
    m_elapsed.start();
    m_roundOtherId.clear();
    if (!m_authError.isEmpty()) {
        emit failed(m_authError);
        return;
    }
    if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        emit failed(QStringLiteral("无法启动设备查找：%1").arg(m_socket.errorString()));
        return;
    }
    QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
    m_cachedAddress = QHostAddress(settings.value(QStringLiteral("lastAddress")).toString());
    m_deviceId = settings.value(QStringLiteral("deviceId")).toString();
    m_broadcasts.clear();
    m_subnets.clear();
    m_ethernetAddresses = LuckfoxNetworkRoute::ethernetAddresses();
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const auto &interface : interfaces) {
        if (interface.type() != QNetworkInterface::Wifi &&
            interface.type() != QNetworkInterface::Ethernet) continue;
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
    // 缓存和广播先尝试 250ms；热点隔离广播时及时进行有限单播扫描。
    if (++m_attempt == 2 || m_attempt == 6) {
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
        QByteArray inner;
        // 配置密钥后拒绝未签名或签名错误的回应；签名覆盖 nonce，旧回应不能重放。
        if (!NetworkController::openMessage(datagram.data(), m_authKey, &inner)) continue;
        const auto reply = QJsonDocument::fromJson(inner).object();
        const QString id = reply.value("device_id").toString();
        if (reply.value("type").toString() != QStringLiteral("luckfox-camera") ||
            reply.value("version").toInt() != 1 ||
            reply.value("nonce").toString() != m_nonce ||
            !id.startsWith(QStringLiteral("luckfox-")) ||
            reply.value("rtsp_port").toInt() != 554 ||
            reply.value("path").toString() != QStringLiteral("/live/0")) continue;
        if (!m_deviceId.isEmpty() && id != m_deviceId) {
            m_roundOtherId = id; // 不自动切换，只在本轮超时时报告
            continue;
        }
        considerCandidate(datagram.senderAddress(), id);
        if (!m_active) return;
    }
}

void LuckfoxDiscovery::considerCandidate(const QHostAddress &sender, const QString &id)
{
    const QString address = sender.toString();
    const bool wired = LuckfoxNetworkRoute::isOnEthernetSubnet(sender, m_ethernetAddresses);
    if (m_candidateAddress.isEmpty() || wired) {
        m_candidateAddress = address;
        m_candidateId = id;
    }
    if (wired || m_ethernetAddresses.isEmpty()) {
        acceptCandidate();
        return;
    }
    // Give a simultaneous Ethernet response a short chance to win over Wi-Fi.
    if (!m_candidateWait.isActive()) m_candidateWait.start();
}

void LuckfoxDiscovery::acceptCandidate()
{
    if (!m_active || m_candidateAddress.isEmpty()) return;
    const QString address = m_candidateAddress;
    QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
    settings.setValue(QStringLiteral("lastAddress"), address);
    settings.setValue(QStringLiteral("deviceId"), m_candidateId);
    m_missingRounds = 0;
    qDebug() << "[Discovery] found_ms=" << m_elapsed.elapsed();
    cancel();
    emit found(QStringLiteral("rtsp://%1/live/0").arg(address));
}

void LuckfoxDiscovery::cancel()
{
    m_active = false;
    m_retry.stop();
    m_deadline.stop();
    m_candidateWait.stop();
    m_candidateAddress.clear();
    m_candidateId.clear();
    m_socket.close();
}
