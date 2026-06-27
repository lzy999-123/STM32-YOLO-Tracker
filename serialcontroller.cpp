#include "serialcontroller.h"
#include <QStringList>
#include <QDebug>

SerialController::SerialController(QObject *parent) : QObject(parent)
{
    // 配置串口的基本通信参数：8位数据位，1位停止位，无校验，无流控
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
    
    // 绑定串口接收信号到槽函数
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialController::handleReadyRead);
}

SerialController::~SerialController()
{
    closeSerial(); // 析构时确保串口安全关闭
}

bool SerialController::openSerial(const QString &portName, int baudRate)
{
    closeSerial(); // 先关闭可能已打开的旧连接
    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    // 以读写模式打开串口，并返回是否成功
    return m_serial.open(QIODevice::ReadWrite);
}

void SerialController::closeSerial()
{
    if (m_serial.isOpen()) {
        m_serial.close();
    }
}

bool SerialController::isOpen() const
{
    return m_serial.isOpen();
}

void SerialController::sendCommand(uint8_t cmd)
{
    if (!m_serial.isOpen()) return;
    QByteArray cmdData; 
    // 构造单指令协议：包头 0xCC + 指令码 + 包尾 0xDD
    cmdData.append((char)0xCC); 
    cmdData.append((char)cmd); 
    cmdData.append((char)0xDD);
    m_serial.write(cmdData); // 下发到 STM32
}

void SerialController::sendTrackData(int16_t offsetX, int16_t offsetY)
{
    if (!m_serial.isOpen()) return;
    uint8_t txBuf[6];
    // 构造坐标偏差数据协议帧：
    // [0]: 包头 0xFF
    // [1-2]: X轴偏移量 (高位在前，低位在后)
    // [3-4]: Y轴偏移量 (高位在前，低位在后)
    // [5]: 包尾 0xFE
    txBuf[0] = 0xFF;
    txBuf[1] = static_cast<uint8_t>(offsetX >> 8);
    txBuf[2] = static_cast<uint8_t>(offsetX & 0xFF);
    txBuf[3] = static_cast<uint8_t>(offsetY >> 8);
    txBuf[4] = static_cast<uint8_t>(offsetY & 0xFF);
    txBuf[5] = 0xFE;
    m_serial.write(reinterpret_cast<char*>(txBuf), 6);
}

void SerialController::handleReadyRead()
{
    QByteArray new_data = m_serial.readAll();
    if (new_data.isEmpty()) return;
    m_rx_buffer.append(new_data);

    // 循环解析接收缓冲区中的完整数据帧
    while (true) {
        // 寻找帧头 "AA"
        int head = m_rx_buffer.indexOf("AA");
        if (head == -1) {
            // 如果没找到帧头，保留最后一个字节防断帧，其余丢弃
            if (m_rx_buffer.size() > 1) m_rx_buffer = m_rx_buffer.right(1);
            break;
        }
        if (head > 0) {
            // 如果帧头不在开头，说明前面的数据是无效杂波，直接清理
            m_rx_buffer.remove(0, head);
            head = 0;
        }
        
        // 寻找帧尾 "BB"
        int tail = m_rx_buffer.indexOf("BB", head + 2);
        if (tail == -1) {
            // 如果找到了头但还没找到尾，说明数据不完整，等待下次接收。
            // 加入防内存溢出机制：缓冲区过大时强制清空
            if (m_rx_buffer.size() > 4096) m_rx_buffer.truncate(4096);
            break;
        }
        
        // 提取出一个完整的帧 "AA...BB"
        QByteArray frame = m_rx_buffer.mid(head, tail - head + 2);
        QString s = QString::fromUtf8(frame); 
        
        // 剥去头尾 "AA" 和 "BB"，按逗号拆分出数据字段
        QStringList list = s.mid(2, s.length() - 4).split(",");
        
        // 校验字段数量，通常包含 3 个遥测参数：模式、参数1、参数2
        if (list.count() == 3) {
            int remoteMode = list[0].toInt();
            // 触发数据解析成功的信号，发送给主界面
            emit telemetryReceived(remoteMode, list[1], list[2]);
        }
        
        // 从缓冲区中移除已经处理过的这一帧
        m_rx_buffer.remove(0, tail + 2);
    }
}
