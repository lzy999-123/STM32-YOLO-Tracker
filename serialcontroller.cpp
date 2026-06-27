#include "serialcontroller.h"
#include <QStringList>
#include <QDebug>

SerialController::SerialController(QObject *parent) : QObject(parent)
{
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
    
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialController::handleReadyRead);
}

SerialController::~SerialController()
{
    closeSerial();
}

bool SerialController::openSerial(const QString &portName, int baudRate)
{
    closeSerial();
    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
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
    cmdData.append((char)0xCC); 
    cmdData.append((char)cmd); 
    cmdData.append((char)0xDD);
    m_serial.write(cmdData);
}

void SerialController::sendTrackData(int16_t offsetX, int16_t offsetY)
{
    if (!m_serial.isOpen()) return;
    uint8_t txBuf[6];
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

    while (true) {
        int head = m_rx_buffer.indexOf("AA");
        if (head == -1) {
            if (m_rx_buffer.size() > 1) m_rx_buffer = m_rx_buffer.right(1);
            break;
        }
        if (head > 0) {
            m_rx_buffer.remove(0, head);
            head = 0;
        }
        int tail = m_rx_buffer.indexOf("BB", head + 2);
        if (tail == -1) {
            if (m_rx_buffer.size() > 4096) m_rx_buffer.truncate(4096);
            break;
        }
        QByteArray frame = m_rx_buffer.mid(head, tail - head + 2);
        QString s = QString::fromUtf8(frame); 
        QStringList list = s.mid(2, s.length() - 4).split(",");
        if (list.count() == 3) {
            int remoteMode = list[0].toInt();
            emit telemetryReceived(remoteMode, list[1], list[2]);
        }
        m_rx_buffer.remove(0, tail + 2);
    }
}
