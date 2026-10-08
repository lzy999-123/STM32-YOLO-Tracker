#ifndef LUCKFOXDISCOVERY_H
#define LUCKFOXDISCOVERY_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QList>
#include <QHostAddress>
#include <QElapsedTimer>

class LuckfoxDiscovery : public QObject
{
    Q_OBJECT
public:
    explicit LuckfoxDiscovery(QObject *parent = nullptr);
    void start();
    void cancel();
    bool isActive() const { return m_active; }
    // 原始密钥字节；空表示不验证签名。构造时已按 NetworkController::loadAuthKey() 加载。
    void setAuthKey(const QByteArray &key);
    // 当前锁定（已记住）的设备标识；空表示接受任意 Luckfox。
    QString lockedDeviceId() const;

public slots:
    // 清除记住的设备标识（含 QSettings），下次查找接受任意设备。
    void forgetDevice();

signals:
    void found(const QString &url);
    void failed(const QString &message);
    // 连续多轮查找未收到锁定设备回应、但收到了其他设备回应；只提示，不自动切换。
    void lockedDeviceMissing(const QString &otherId);

private:
    void probe();
    void receive();
    QUdpSocket m_socket;
    QTimer m_retry;
    QTimer m_deadline;
    QElapsedTimer m_elapsed;
    QList<QHostAddress> m_broadcasts;
    QList<quint32> m_subnets;
    QString m_nonce;
    QString m_deviceId;
    QHostAddress m_cachedAddress;
    QByteArray m_request;
    QByteArray m_authKey;
    QString m_authError;
    QString m_roundOtherId;
    int m_missingRounds = 0;
    bool m_active = false;
    int m_attempt = 0;
};
#endif
