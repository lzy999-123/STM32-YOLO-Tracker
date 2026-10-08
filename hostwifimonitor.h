#ifndef HOSTWIFIMONITOR_H
#define HOSTWIFIMONITOR_H
#include <QObject>
#include <QFutureWatcher>
#include <QTimer>
class HostWifiMonitor : public QObject
{
    Q_OBJECT
public:
    explicit HostWifiMonitor(QObject *parent = nullptr);
    void refresh();
signals:
    void nameChanged(const QString &name);
private:
    QFutureWatcher<QString> m_query;
    QTimer m_timer;
};
#endif
