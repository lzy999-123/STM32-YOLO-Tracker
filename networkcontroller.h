#ifndef NETWORKCONTROLLER_H
#define NETWORKCONTROLLER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QQueue>

// 电脑端只使用网络；UART 及协议 v2 的帧处理在 Luckfox 上完成。
class NetworkController : public QObject
{
    Q_OBJECT
public:
    enum Command : uint8_t {
        CmdSwitchMode = 0x01, CmdCenter = 0x02,
        CmdTrackOn = 0x11, CmdTrackOff = 0x12,
        CmdManualUp = 0x20, CmdManualDown = 0x21,
        CmdManualLeft = 0x22, CmdManualRight = 0x23
    };
    explicit NetworkController(QObject *parent = nullptr);
    void connectToDevice(const QString &address, quint16 port = 5005);
    void disconnectDevice();
    bool isOpen() const { return m_connected && m_online; }
    bool isConnected() const { return m_connected; }
    void sendCommand(uint8_t cmd);
    void sendTrackData(int16_t offsetX, int16_t offsetY);
    void shutdownGimbal();
signals:
    void telemetryReceived(int mode, float horizontalAngle, float verticalAngle);
    void commandFailed(uint8_t cmd);
    void connectionLost(const QString &reason);
    void reconnected();
    void deviceOnlineChanged(bool online);
private:
    struct PendingCommand { quint32 request; uint8_t cmd; int retries = 0; };
    void send(QJsonObject message);
    void receive();
    void tick();
    void transmitCommand();
    void setOnline(bool online);
    void loseConnection(const QString &reason);
    QUdpSocket m_socket;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QHostAddress m_address;
    quint16 m_port = 5005;
    QString m_session;
    QQueue<PendingCommand> m_commands;
    quint32 m_sequence = 0;
    qint64 m_lastReply = 0;
    qint64 m_handshakeStarted = 0;
    qint64 m_lastTelemetry = -2000;
    qint64 m_lastCommand = 0;
    quint32 m_lastTelemetrySequence = 0;
    bool m_connected = false;
    bool m_online = false;
    bool m_remoteOnline = false;
    bool m_active = false;
    bool m_reportedFailure = false;
};
#endif
