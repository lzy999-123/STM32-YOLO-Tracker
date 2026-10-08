#ifndef SERIALCONTROLLER_H
#define SERIALCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QList>

/**
 * @brief 串口控制器类（协议 v2，见 PROTOCOL.md）
 *
 * 负责与下位机（STM32）通信：
 *  - 统一二进制帧 A5 5A | LEN | TYPE | PAYLOAD | CRC8，双向 CRC 校验；
 *  - 周期心跳维持链路，供下位机做失控保护判断；
 *  - 控制命令带 ACK 确认与超时重传；
 *  - 检测串口硬件错误（拔出等），自动重连并向 UI 上报状态；
 *  - 监测下位机遥测超时，判断下位机是否离线。
 */
class SerialController : public QObject
{
    Q_OBJECT
public:
    // 协议命令码（与下位机 main.c 保持一致）
    enum Command : uint8_t {
        CmdSwitchMode = 0x01, ///< 切换手动/自动模式并回中
        CmdCenter     = 0x02, ///< 回中并停止跟踪
        CmdTrackOn    = 0x11, ///< 使能自动跟踪
        CmdTrackOff   = 0x12, ///< 停止自动跟踪
        CmdManualUp   = 0x20, ///< 手动模式：垂直轴向上步进 0.5°
        CmdManualDown = 0x21, ///< 手动模式：垂直轴向下步进 0.5°
        CmdManualLeft = 0x22, ///< 手动模式：水平轴向左步进 0.5°
        CmdManualRight = 0x23, ///< 手动模式：水平轴向右步进 0.5°
    };

    explicit SerialController(QObject *parent = nullptr);
    ~SerialController();

    /**
     * @brief 打开串口并启动心跳/看护定时器
     * @param portName 串口名称（例如 "COM3"）
     * @param baudRate 波特率（例如 115200）
     * @return 成功返回 true，失败返回 false
     */
    bool openSerial(const QString &portName, int baudRate);

    /// @brief 关闭串口，停止心跳与重连
    void closeSerial();

    /// @brief 检查串口是否处于打开状态
    bool isOpen() const;

    /**
     * @brief 发送控制命令（带 ACK 确认，超时自动重传，最终失败发 commandFailed）
     * @param cmd 命令码，见 Command 枚举
     */
    void sendCommand(uint8_t cmd);

    /**
     * @brief 发送目标相对偏移量（高频数据帧，不做 ACK）
     * @param offsetX 目标相对画面中心 X 轴的偏移像素值
     * @param offsetY 目标相对画面中心 Y 轴的偏移像素值
     */
    void sendTrackData(int16_t offsetX, int16_t offsetY);

    /**
     * @brief 程序退出前的收尾：发送停止跟踪+回中命令并等待字节写出
     *
     * 阻塞最多约 300ms。即使命令丢失，下位机链路超时保护也会兜底回中。
     */
    void shutdownGimbal();

signals:
    /**
     * @brief 遥测数据接收信号（每帧遥测触发一次）
     * @param remoteMode 下位机当前模式：0=手动 1=自动
     * @param angle1 舵机1当前角度（度）
     * @param angle2 舵机2当前角度（度）
     */
    void telemetryReceived(int remoteMode, float angle1, float angle2);

    /// @brief 命令重传 3 次仍未收到 ACK
    void commandFailed(uint8_t cmd);

    /// @brief 串口硬件错误导致断开（已进入自动重连）
    void connectionLost(const QString &reason);

    /// @brief 自动重连成功
    void reconnected();

    /// @brief 下位机遥测超时（>1s 无遥测帧）/恢复
    void deviceOnlineChanged(bool online);

private slots:
    void handleReadyRead();
    void handleSerialError(QSerialPort::SerialPortError error);
    void sendHeartbeat();
    void handleAckTimeout();
    void tryReconnect();
    void checkDeviceOnline();

private:
    // 帧构造与解析
    static uint8_t crc8(const uint8_t *data, int len);
    QByteArray buildFrame(uint8_t type, const QByteArray &payload) const;
    void writeFrame(const QByteArray &frame);
    void parseRxBuffer();
    void handleFrame(uint8_t type, const QByteArray &payload);

    // 命令 ACK 队列
    void transmitFrontCommand();
    void onAckReceived(uint8_t cmd);

    QSerialPort m_serial;
    QByteArray m_rx_buffer;

    QTimer m_heartbeatTimer;   ///< 200ms 心跳
    QTimer m_ackTimer;         ///< 命令 ACK 超时（150ms 单次）
    QTimer m_reconnectTimer;   ///< 断线后每 2s 重连
    QTimer m_onlineCheckTimer; ///< 遥测超时巡检（500ms 周期）

    QList<uint8_t> m_pendingCommands; ///< 待确认命令队列（队首为在途命令）
    int m_ackRetries = 0;

    QString m_portName;        ///< 记录参数用于自动重连
    int m_baudRate = 115200;
    bool m_userClosed = true;  ///< true 表示用户主动关闭，不做自动重连

    qint64 m_lastTelemetryTime = 0;
    bool m_deviceOnline = false;
};

#endif // SERIALCONTROLLER_H
