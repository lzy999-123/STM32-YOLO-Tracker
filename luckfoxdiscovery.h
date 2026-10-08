#ifndef LUCKFOXDISCOVERY_H
#define LUCKFOXDISCOVERY_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QList>
#include <QHostAddress>

class LuckfoxDiscovery : public QObject
{
    Q_OBJECT
public:
    explicit LuckfoxDiscovery(QObject *parent = nullptr);
    void start();
    void cancel();
    bool isActive() const { return m_active; }

signals:
    void found(const QString &url);
    void failed(const QString &message);

private:
    void probe();
    void receive();
    QUdpSocket m_socket;
    QTimer m_retry;
    QTimer m_deadline;
    QList<QHostAddress> m_broadcasts;
    QList<quint32> m_subnets;
    QString m_nonce;
    QString m_deviceId;
    QHostAddress m_cachedAddress;
    QByteArray m_request;
    bool m_active = false;
    int m_attempt = 0;
};
#endif
