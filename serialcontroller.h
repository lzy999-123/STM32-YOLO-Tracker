#ifndef SERIALCONTROLLER_H
#define SERIALCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QString>

/**
 * @brief 串口控制器类
 * 负责与下位机（如 STM32）进行串口通信，发送云台控制指令和目标相对坐标，并接收遥测数据。
 */
class SerialController : public QObject
{
    Q_OBJECT
public:
    explicit SerialController(QObject *parent = nullptr);
    ~SerialController();

    /**
     * @brief 打开串口
     * @param portName 串口名称（例如 "COM3"）
     * @param baudRate 波特率（例如 115200）
     * @return 成功返回 true，失败返回 false
     */
    bool openSerial(const QString &portName, int baudRate);
    
    /// @brief 关闭当前串口连接
    void closeSerial();
    
    /// @brief 检查串口是否处于打开状态
    bool isOpen() const;

    /**
     * @brief 发送单字节控制命令
     * @param cmd 控制指令（具体代表的含义由下位机协议决定）
     */
    void sendCommand(uint8_t cmd);

    /**
     * @brief 发送目标的相对偏移量
     * @param offsetX 目标在画面中心 X 轴的偏移像素值
     * @param offsetY 目标在画面中心 Y 轴的偏移像素值
     */
    void sendTrackData(int16_t offsetX, int16_t offsetY);

signals:
    /**
     * @brief 遥测数据接收信号
     * @param remoteMode 下位机当前的模式状态
     * @param param1 遥测参数1
     * @param param2 遥测参数2
     */
    void telemetryReceived(int remoteMode, const QString& param1, const QString& param2);

private slots:
    /// @brief 串口数据到达时的读取槽函数
    void handleReadyRead();

private:
    QSerialPort m_serial;       ///< Qt 串口通信对象
    QByteArray m_rx_buffer;     ///< 串口接收数据缓冲区
};

#endif // SERIALCONTROLLER_H
