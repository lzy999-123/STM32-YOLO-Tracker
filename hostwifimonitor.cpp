#include "hostwifimonitor.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QScopeGuard>
#include <QStringList>
#include <QStringDecoder>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wlanapi.h>
#endif

namespace {
QString connectedWifiName()
{
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    DWORD version = 0;
    const DWORD opened = WlanOpenHandle(2, nullptr, &version, &handle);
    if (opened != ERROR_SUCCESS) return QStringLiteral("Wi-Fi 信息不可用（%1）").arg(opened);
    const auto close = qScopeGuard([&] { WlanCloseHandle(handle, nullptr); });
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    const DWORD enumerated = WlanEnumInterfaces(handle, nullptr, &interfaces);
    if (enumerated != ERROR_SUCCESS) return QStringLiteral("Wi-Fi 信息不可用（%1）").arg(enumerated);
    const auto freeInterfaces = qScopeGuard([&] { WlanFreeMemory(interfaces); });
    QStringList names;
    bool denied = false;
    for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
        const auto &wifiInterface = interfaces->InterfaceInfo[index];
        if (wifiInterface.isState != wlan_interface_state_connected) continue;
        DWORD size = 0;
        PVOID raw = nullptr;
        const DWORD result = WlanQueryInterface(handle, &wifiInterface.InterfaceGuid,
            wlan_intf_opcode_current_connection, nullptr, &size, &raw, nullptr);
        if (result != ERROR_SUCCESS) { denied = true; continue; }
        const auto free = qScopeGuard([&] { WlanFreeMemory(raw); });
        if (size < sizeof(WLAN_CONNECTION_ATTRIBUTES)) continue;
        const auto &ssid = static_cast<WLAN_CONNECTION_ATTRIBUTES *>(raw)->wlanAssociationAttributes.dot11Ssid;
        if (ssid.uSSIDLength > 32) continue;
        const QByteArray bytes(reinterpret_cast<const char *>(ssid.ucSSID), int(ssid.uSSIDLength));
        QStringDecoder utf8(QStringDecoder::Utf8);
        QString name = utf8(bytes);
        if (utf8.hasError()) name = QString::fromLocal8Bit(bytes);
        if (name.isEmpty()) name = QStringLiteral("隐藏网络");
        if (!names.contains(name)) names.append(name);
    }
    if (!names.isEmpty()) return names.join(QStringLiteral(" / "));
    return denied ? QStringLiteral("Wi-Fi 已连接，名称不可读取") : QStringLiteral("未连接 Wi-Fi");
#else
    return QStringLiteral("当前系统暂不支持读取 Wi-Fi 名称");
#endif
}
}

HostWifiMonitor::HostWifiMonitor(QObject *parent) : QObject(parent)
{
    connect(&m_query, &QFutureWatcher<QString>::finished, this, [this] { emit nameChanged(m_query.result()); });
    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, &HostWifiMonitor::refresh);
    m_timer.start();
}
void HostWifiMonitor::refresh()
{
    if (!m_query.isRunning()) m_query.setFuture(QtConcurrent::run(connectedWifiName));
}
