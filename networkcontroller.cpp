#include "networkcontroller.h"
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QUuid>
#include <cmath>

NetworkController::NetworkController(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &NetworkController::tick);
    connect(&m_socket, &QUdpSocket::readyRead, this, &NetworkController::receive);
    m_clock.start();
}

void NetworkController::connectToDevice(const QString &address, quint16 port)
{
    const QHostAddress host(address);
    if (m_active && host == m_address && port == m_port) return;
    disconnectDevice();
    if (host.protocol() != QAbstractSocket::IPv4Protocol || port == 0) {
        emit connectionLost(QStringLiteral("无线控制需要有效的设备 IPv4 地址"));
        return;
    }
    m_address = host;
    m_port = port;
    if (!m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        emit connectionLost(QStringLiteral("无法打开网络控制通道：%1").arg(m_socket.errorString()));
        return;
    }
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sequence = 0;
    m_lastTelemetrySequence = 0;
    m_lastReply = m_clock.elapsed();
    m_handshakeStarted = m_lastReply;
    m_lastTelemetry = m_lastReply - 2000;
    m_active = true;
    m_reportedFailure = false;
    m_timer.start();
    send({{"type", "hello"}});
}

void NetworkController::send(QJsonObject message)
{
    if (!m_active) return;
    message.insert(QStringLiteral("version"), 1);
    message.insert(QStringLiteral("session"), m_session);
    m_socket.writeDatagram(QJsonDocument(message).toJson(QJsonDocument::Compact), m_address, m_port);
}

void NetworkController::disconnectDevice()
{
    if (m_active) send({{"type", "bye"}});
    m_active = false;
    m_connected = false;
    m_remoteOnline = false;
    m_timer.stop();
    m_commands.clear();
    m_socket.close();
    setOnline(false);
}

void NetworkController::setOnline(bool online)
{
    if (m_online == online) return;
    m_online = online;
    emit deviceOnlineChanged(online);
}

void NetworkController::loseConnection(const QString &reason)
{
    m_connected = false;
    m_remoteOnline = false;
    setOnline(false);
    while (!m_commands.isEmpty()) emit commandFailed(m_commands.dequeue().cmd);
    // 换会话后不能重放旧命令或旧坐标；需重新收到 STM32 遥测才能操作。
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sequence = 0;
    m_lastTelemetrySequence = 0;
    m_handshakeStarted = m_clock.elapsed();
    m_lastTelemetry = m_clock.elapsed() - 2000;
    if (!m_reportedFailure) {
        m_reportedFailure = true;
        emit connectionLost(reason);
    }
}

void NetworkController::sendTrackData(int16_t x, int16_t y)
{
    if (!isOpen()) return;
    send({{"type", "track"}, {"seq", double(++m_sequence)}, {"x", x}, {"y", y}});
}

void NetworkController::sendCommand(uint8_t cmd)
{
    if (!isOpen() || m_commands.size() >= 16) {
        emit commandFailed(cmd);
        return;
    }
    m_commands.enqueue({++m_sequence, cmd, 0});
    if (m_commands.size() == 1) transmitCommand();
}

void NetworkController::transmitCommand()
{
    if (m_commands.isEmpty()) return;
    const auto &command = m_commands.head();
    send({{"type", "command"}, {"request", double(command.request)}, {"cmd", command.cmd}});
    m_lastCommand = m_clock.elapsed();
}

void NetworkController::tick()
{
    if (!m_active) return;
    const auto now = m_clock.elapsed();
    if (m_connected && now - m_lastReply > 1000)
        loseConnection(QStringLiteral("Luckfox 网络控制超时，正在重新连接"));
    if (!m_connected) {
        if (!m_reportedFailure && now - m_lastReply > 1500) {
            m_reportedFailure = true;
            emit connectionLost(QStringLiteral("Luckfox 控制服务未响应，继续查找"));
        }
        // 板端会封存已超时的会话；长时间未收到 welcome 时换新会话重试。
        if (now - m_handshakeStarted >= 1500) {
            m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_handshakeStarted = now;
        }
        send({{"type", "hello"}});
        return;
    }
    setOnline(m_remoteOnline && now - m_lastTelemetry <= 1000);
    send({{"type", "heartbeat"}});
    if (!m_commands.isEmpty() && now - m_lastCommand >= 200) {
        if (++m_commands.head().retries > 3) {
            emit commandFailed(m_commands.dequeue().cmd);
            transmitCommand();
        } else {
            transmitCommand(); // 重发同一 request；Luckfox 去重，不重复驱动舵机。
        }
    }
}

void NetworkController::receive()
{
    while (m_socket.hasPendingDatagrams()) {
        const auto packet = m_socket.receiveDatagram(1400);
        if (!m_active || packet.senderAddress() != m_address || packet.senderPort() != m_port) continue;
        const auto reply = QJsonDocument::fromJson(packet.data()).object();
        if (reply.value("version").toInt() != 1 || reply.value("session").toString() != m_session) continue;
        const QString type = reply.value("type").toString();
        if (type == QStringLiteral("welcome")) {
            m_lastReply = m_clock.elapsed();
            if (!m_connected) {
                m_connected = true;
                m_reportedFailure = false;
                emit reconnected();
            }
        } else if (type == QStringLiteral("status") && m_connected) {
            const double rawSeq = reply.value("seq").toDouble(-1);
            if (rawSeq < 1 || rawSeq > 4294967295.0 || std::floor(rawSeq) != rawSeq) continue;
            const auto seq = quint32(rawSeq);
            if (seq <= m_lastTelemetrySequence) continue;
            m_lastTelemetrySequence = seq;
            m_lastReply = m_clock.elapsed();
            const bool online = reply.value("online").toBool(false);
            const int mode = reply.value("mode").toInt(-1);
            const double horizontal = reply.value("horizontal").toDouble(NAN);
            const double vertical = reply.value("vertical").toDouble(NAN);
            if (online && (mode == 0 || mode == 1) && std::isfinite(horizontal) && std::isfinite(vertical)
                && horizontal >= -3276.8 && horizontal <= 3276.7
                && vertical >= -3276.8 && vertical <= 3276.7) {
                m_lastTelemetry = m_clock.elapsed();
                m_remoteOnline = true;
                setOnline(true);
                emit telemetryReceived(mode, float(horizontal), float(vertical));
            } else {
                m_remoteOnline = false;
                setOnline(false);
            }
        } else if ((type == QStringLiteral("ack") || type == QStringLiteral("command_failed"))
                   && m_connected && !m_commands.isEmpty()) {
            const auto &pending = m_commands.head();
            if (reply.value("request").toDouble(-1) != pending.request || reply.value("cmd").toInt(-1) != pending.cmd) continue;
            m_lastReply = m_clock.elapsed();
            const auto command = m_commands.dequeue();
            if (type == QStringLiteral("command_failed")) emit commandFailed(command.cmd);
            transmitCommand();
        }
    }
}

void NetworkController::shutdownGimbal()
{
    disconnectDevice(); // bye 由板端执行停止；丢包时会话超时停止并断开 UART 心跳。
}
