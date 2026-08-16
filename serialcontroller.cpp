#include "serialcontroller.h"
#include <QDateTime>

namespace {
// 协议 v2 常量，详见 PROTOCOL.md
constexpr char kSync1 = char(0xA5);
constexpr char kSync2 = char(0x5A);
constexpr int kMaxLen = 32;                 // LEN 字段上限（TYPE+PAYLOAD）
constexpr qsizetype kMaxReceiveBufferSize = 4096;

constexpr uint8_t kTypeTrackData = 0x01;
constexpr uint8_t kTypeCmd       = 0x02;
constexpr uint8_t kTypeHeartbeat = 0x03;
constexpr uint8_t kTypeTelemetry = 0x81;
constexpr uint8_t kTypeAck       = 0x82;

constexpr int kHeartbeatIntervalMs = 200;
constexpr int kAckTimeoutMs        = 150;
constexpr int kAckMaxRetries       = 3;
constexpr int kReconnectIntervalMs = 2000;
constexpr int kOnlineCheckMs       = 500;
constexpr qint64 kTelemetryTimeoutMs = 1000;
}

SerialController::SerialController(QObject *parent) : QObject(parent)
{
    // 8位数据位，1位停止位，无校验，无流控
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    connect(&m_serial, &QSerialPort::readyRead, this, &SerialController::handleReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &SerialController::handleSerialError);

    m_heartbeatTimer.setInterval(kHeartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &SerialController::sendHeartbeat);

    m_ackTimer.setSingleShot(true);
    m_ackTimer.setInterval(kAckTimeoutMs);
    connect(&m_ackTimer, &QTimer::timeout, this, &SerialController::handleAckTimeout);

    m_reconnectTimer.setInterval(kReconnectIntervalMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SerialController::tryReconnect);

    m_onlineCheckTimer.setInterval(kOnlineCheckMs);
    connect(&m_onlineCheckTimer, &QTimer::timeout, this, &SerialController::checkDeviceOnline);
}

SerialController::~SerialController()
{
    closeSerial();
}

bool SerialController::openSerial(const QString &portName, int baudRate)
{
    closeSerial();
    m_portName = portName;
    m_baudRate = baudRate;
    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    if (!m_serial.open(QIODevice::ReadWrite)) {
        return false;
    }
    m_userClosed = false;
    m_lastTelemetryTime = QDateTime::currentMSecsSinceEpoch();
    m_deviceOnline = false;
    m_heartbeatTimer.start();
    m_onlineCheckTimer.start();
    return true;
}

void SerialController::closeSerial()
{
    m_userClosed = true;
    m_heartbeatTimer.stop();
    m_ackTimer.stop();
    m_reconnectTimer.stop();
    m_onlineCheckTimer.stop();
    m_pendingCommands.clear();
    m_ackRetries = 0;
    m_rx_buffer.clear();
    if (m_serial.isOpen()) {
        m_serial.close();
    }
}

bool SerialController::isOpen() const
{
    return m_serial.isOpen();
}

// ---------------- 帧构造与发送 ----------------

uint8_t SerialController::crc8(const uint8_t *data, int len)
{
    // CRC-8/ATM：多项式 0x07，初值 0x00（与下位机 main.c 实现一致）
    uint8_t crc = 0x00;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
        }
    }
    return crc;
}

QByteArray SerialController::buildFrame(uint8_t type, const QByteArray &payload) const
{
    QByteArray frame;
    frame.reserve(payload.size() + 5);
    frame.append(kSync1);
    frame.append(kSync2);
    frame.append(char(payload.size() + 1)); // LEN = TYPE + PAYLOAD
    frame.append(char(type));
    frame.append(payload);
    // CRC 覆盖 LEN、TYPE、PAYLOAD
    frame.append(char(crc8(reinterpret_cast<const uint8_t *>(frame.constData()) + 2,
                           frame.size() - 2)));
    return frame;
}

void SerialController::writeFrame(const QByteArray &frame)
{
    if (!m_serial.isOpen()) return;
    m_serial.write(frame);
}

void SerialController::sendTrackData(int16_t offsetX, int16_t offsetY)
{
    const quint16 x = static_cast<quint16>(offsetX);
    const quint16 y = static_cast<quint16>(offsetY);
    QByteArray payload;
    payload.append(char((x >> 8) & 0xFF));
    payload.append(char(x & 0xFF));
    payload.append(char((y >> 8) & 0xFF));
    payload.append(char(y & 0xFF));
    writeFrame(buildFrame(kTypeTrackData, payload));
}

void SerialController::sendHeartbeat()
{
    writeFrame(buildFrame(kTypeHeartbeat, QByteArray()));
}

// ---------------- 命令 ACK 队列 ----------------

void SerialController::sendCommand(uint8_t cmd)
{
    if (!m_serial.isOpen()) return;
    m_pendingCommands.append(cmd);
    // 队列中只有这一条时立即发出，否则等待前序命令 ACK 后依次发送
    if (m_pendingCommands.size() == 1) {
        m_ackRetries = 0;
        transmitFrontCommand();
    }
}

void SerialController::transmitFrontCommand()
{
    if (m_pendingCommands.isEmpty()) return;
    writeFrame(buildFrame(kTypeCmd, QByteArray(1, char(m_pendingCommands.first()))));
    m_ackTimer.start();
}

void SerialController::handleAckTimeout()
{
    if (m_pendingCommands.isEmpty()) return;
    if (++m_ackRetries <= kAckMaxRetries) {
        transmitFrontCommand();
        return;
    }
    const uint8_t failedCmd = m_pendingCommands.takeFirst();
    m_ackRetries = 0;
    emit commandFailed(failedCmd);
    transmitFrontCommand(); // 继续尝试队列中的后续命令
}

void SerialController::onAckReceived(uint8_t cmd)
{
    if (m_pendingCommands.isEmpty() || m_pendingCommands.first() != cmd) {
        return; // 迟到或乱序的 ACK，忽略
    }
    m_ackTimer.stop();
    m_pendingCommands.removeFirst();
    m_ackRetries = 0;
    transmitFrontCommand();
}

// ---------------- 接收与解析 ----------------

void SerialController::handleReadyRead()
{
    m_rx_buffer.append(m_serial.readAll());
    parseRxBuffer();
    if (m_rx_buffer.size() > kMaxReceiveBufferSize) {
        m_rx_buffer.clear(); // 长期无法同步的垃圾数据，整体丢弃
    }
}

void SerialController::parseRxBuffer()
{
    // 逐字节滑动重同步：CRC 失败只前进 1 字节，避免错锁在数据区的 A5 5A 上
    while (m_rx_buffer.size() >= 5) {
        if (m_rx_buffer.at(0) != kSync1 || m_rx_buffer.at(1) != kSync2) {
            m_rx_buffer.remove(0, 1);
            continue;
        }
        const int len = uint8_t(m_rx_buffer.at(2));
        if (len < 1 || len > kMaxLen) {
            m_rx_buffer.remove(0, 1);
            continue;
        }
        const int frameSize = len + 4;
        if (m_rx_buffer.size() < frameSize) {
            return; // 帧未收全，等待更多数据
        }
        const uint8_t expected = uint8_t(m_rx_buffer.at(frameSize - 1));
        const uint8_t actual = crc8(
            reinterpret_cast<const uint8_t *>(m_rx_buffer.constData()) + 2, len + 1);
        if (expected != actual) {
            m_rx_buffer.remove(0, 1);
            continue;
        }
        const uint8_t type = uint8_t(m_rx_buffer.at(3));
        const QByteArray payload = m_rx_buffer.mid(4, len - 1);
        m_rx_buffer.remove(0, frameSize);
        handleFrame(type, payload);
    }
}

void SerialController::handleFrame(uint8_t type, const QByteArray &payload)
{
    switch (type) {
    case kTypeTelemetry: {
        if (payload.size() != 5) return;
        const int mode = uint8_t(payload.at(0));
        const int16_t a1 = int16_t((uint8_t(payload.at(1)) << 8) | uint8_t(payload.at(2)));
        const int16_t a2 = int16_t((uint8_t(payload.at(3)) << 8) | uint8_t(payload.at(4)));
        m_lastTelemetryTime = QDateTime::currentMSecsSinceEpoch();
        if (!m_deviceOnline) {
            m_deviceOnline = true;
            emit deviceOnlineChanged(true);
        }
        emit telemetryReceived(mode, a1 / 10.0f, a2 / 10.0f); // 角度按 ×10 传输
        break;
    }
    case kTypeAck:
        if (payload.size() == 1) {
            onAckReceived(uint8_t(payload.at(0)));
        }
        break;
    default:
        break; // 未知类型帧，静默忽略（前向兼容）
    }
}

// ---------------- 断线检测与自动重连 ----------------

void SerialController::handleSerialError(QSerialPort::SerialPortError error)
{
    // ResourceError = USB 拔出等硬件错误；其余读写错误也按断线处理
    if (error == QSerialPort::NoError) return;
    if (error != QSerialPort::ResourceError &&
        error != QSerialPort::DeviceNotFoundError &&
        error != QSerialPort::ReadError &&
        error != QSerialPort::WriteError) {
        return;
    }
    if (m_userClosed) return;

    const QString reason = m_serial.errorString();
    m_heartbeatTimer.stop();
    m_ackTimer.stop();
    m_onlineCheckTimer.stop();
    m_pendingCommands.clear();
    m_rx_buffer.clear();
    if (m_serial.isOpen()) {
        m_serial.close();
    }
    if (m_deviceOnline) {
        m_deviceOnline = false;
        emit deviceOnlineChanged(false);
    }
    emit connectionLost(reason);
    m_reconnectTimer.start();
}

void SerialController::tryReconnect()
{
    if (m_userClosed) {
        m_reconnectTimer.stop();
        return;
    }
    m_serial.setPortName(m_portName);
    m_serial.setBaudRate(m_baudRate);
    if (m_serial.open(QIODevice::ReadWrite)) {
        m_reconnectTimer.stop();
        m_lastTelemetryTime = QDateTime::currentMSecsSinceEpoch();
        m_heartbeatTimer.start();
        m_onlineCheckTimer.start();
        emit reconnected();
    }
}

void SerialController::checkDeviceOnline()
{
    const bool online = m_serial.isOpen() &&
        (QDateTime::currentMSecsSinceEpoch() - m_lastTelemetryTime) < kTelemetryTimeoutMs;
    if (online != m_deviceOnline) {
        m_deviceOnline = online;
        emit deviceOnlineChanged(online);
    }
}

// ---------------- 退出收尾 ----------------

void SerialController::shutdownGimbal()
{
    if (!m_serial.isOpen()) return;
    // 直接写帧并等待写出，不走 ACK 队列（退出时事件循环即将结束）。
    // 命令若丢失，下位机 1s 链路超时保护会自动回中兜底。
    writeFrame(buildFrame(kTypeCmd, QByteArray(1, char(CmdTrackOff))));
    writeFrame(buildFrame(kTypeCmd, QByteArray(1, char(CmdCenter))));
    m_serial.waitForBytesWritten(300);
}
