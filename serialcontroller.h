#ifndef SERIALCONTROLLER_H
#define SERIALCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>
#include <QString>

class SerialController : public QObject
{
    Q_OBJECT
public:
    explicit SerialController(QObject *parent = nullptr);
    ~SerialController();

    bool openSerial(const QString &portName, int baudRate);
    void closeSerial();
    bool isOpen() const;

    void sendCommand(uint8_t cmd);
    void sendTrackData(int16_t offsetX, int16_t offsetY);

signals:
    void telemetryReceived(int remoteMode, const QString& param1, const QString& param2);

private slots:
    void handleReadyRead();

private:
    QSerialPort m_serial;
    QByteArray m_rx_buffer;
};

#endif // SERIALCONTROLLER_H
