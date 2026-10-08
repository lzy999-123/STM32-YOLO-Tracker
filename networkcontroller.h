#ifndef NETWORKCONTROLLER_H
#define NETWORKCONTROLLER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QQueue>
#include <QByteArray>

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
    // STM32 遥测 flags（TELEMETRY 第 6 字节；旧固件为 0）。
    enum TelemetryFlag { FlagFault = 0x01, FlagEmergencyStop = 0x02, FlagLinkLost = 0x04 };
    explicit NetworkController(QObject *parent = nullptr);
    void connectToDevice(const QString &address, quint16 port = 5005);
    void disconnectDevice();
    bool isOpen() const { return m_connected && m_online; }
    bool isConnected() const { return m_connected; }
    void sendCommand(uint8_t cmd);
    void sendTrackData(int16_t offsetX, int16_t offsetY);
    void shutdownGimbal();
    int telemetryFlags() const { return m_flags; }
    // 原始密钥字节；空表示旧格式（不签名、不验证）。构造时已按 loadAuthKey() 加载。
    void setAuthKey(const QByteArray &key);
    bool isAuthEnabled() const { return !m_authKey.isEmpty(); }
    QString authKeyError() const { return m_authError; }
    // 环境变量 LUCKFOX_CONTROL_KEY，否则读取程序目录的 luckfox-control.key（十六进制，至少 32 字符）。
    static QByteArray loadAuthKey(QString *error = nullptr);
    static QByteArray parseAuthKey(const QByteArray &hex, QString *error = nullptr);
    // 有密钥时包装为 {"m": 内层 JSON 文本, "auth": HMAC-SHA256 小写十六进制}；无密钥原样返回。
    static QByteArray sealMessage(const QByteArray &inner, const QByteArray &key);
    // 验证并取出内层 JSON；有密钥时拒绝未签名或签名错误的数据。
    static bool openMessage(const QByteArray &datagram, const QByteArray &key, QByteArray *inner);
signals:
    void telemetryReceived(int mode, float horizontalAngle, float verticalAngle);
    void commandFailed(uint8_t cmd);
    void connectionLost(const QString &reason);
    void reconnected();
    void deviceOnlineChanged(bool online);
    void telemetryFlagsChanged(int flags);
private:
    struct PendingCommand { quint32 request; uint8_t cmd; int retries = 0; };
    void send(QJsonObject message);
    void receive();
    void tick();
    void transmitCommand();
    void setOnline(bool online);
    void loseConnection(const QString &reason);
    void setFlags(int flags);
    QUdpSocket m_socket;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QHostAddress m_address;
    quint16 m_port = 5005;
    QString m_session;
    QByteArray m_authKey;
    QString m_authError;
    QQueue<PendingCommand> m_commands;
    quint32 m_sequence = 0;
    qint64 m_lastReply = 0;
    qint64 m_handshakeStarted = 0;
    qint64 m_lastTelemetry = -2000;
    qint64 m_lastCommand = 0;
    quint32 m_lastTelemetrySequence = 0;
    int m_flags = 0;
    bool m_connected = false;
    bool m_online = false;
    bool m_remoteOnline = false;
    bool m_active = false;
    bool m_reportedFailure = false;
};
#endif
