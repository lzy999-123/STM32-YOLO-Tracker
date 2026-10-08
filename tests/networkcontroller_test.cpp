#include "networkcontroller.h"
#include <QtTest>
#include <QJsonDocument>
#include <QNetworkDatagram>

class NetworkControllerTest : public QObject
{
    Q_OBJECT
private slots:
    void telemetryGatesControlsAndRejectsOldStatus();
    void commandRetryKeepsRequestIdAndNetworkLossDisablesControls();
    void bridgeOverWifi();
};

void NetworkControllerTest::telemetryGatesControlsAndRejectsOldStatus()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    NetworkController control;
    control.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(server.hasPendingDatagrams());
    const auto hello = server.receiveDatagram();
    const auto session = QJsonDocument::fromJson(hello.data()).object().value("session").toString();
    auto reply = [&](QJsonObject body) {
        body.insert("version", 1); body.insert("session", session);
        server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact), hello.senderAddress(), hello.senderPort());
    };
    reply({{"type", "welcome"}});
    QTRY_VERIFY(control.isConnected());
    QVERIFY(!control.isOpen());
    reply({{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 1}, {"horizontal", -15}, {"vertical", 230}});
    QTRY_VERIFY(control.isOpen());
    reply({{"type", "status"}, {"seq", 2}, {"online", false}});
    QTRY_VERIFY(!control.isOpen());
    reply({{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 1}, {"horizontal", 90}, {"vertical", 85}});
    QTest::qWait(250);
    QVERIFY(!control.isOpen());
    control.disconnectDevice();
}

void NetworkControllerTest::commandRetryKeepsRequestIdAndNetworkLossDisablesControls()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    NetworkController control;
    QSignalSpy loss(&control, &NetworkController::connectionLost);
    control.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(server.hasPendingDatagrams());
    const auto hello = server.receiveDatagram();
    const auto session = QJsonDocument::fromJson(hello.data()).object().value("session").toString();
    auto reply = [&](QJsonObject body) {
        body.insert("version", 1); body.insert("session", session);
        server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact), hello.senderAddress(), hello.senderPort());
    };
    reply({{"type", "welcome"}});
    QTRY_VERIFY(control.isConnected());
    reply({{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 0}, {"horizontal", 90}, {"vertical", 90}});
    QTRY_VERIFY(control.isOpen());
    control.sendCommand(NetworkController::CmdManualLeft);
    QList<QJsonObject> commands;
    QElapsedTimer deadline; deadline.start();
    while (deadline.elapsed() < 550 && commands.size() < 2) {
        QCoreApplication::processEvents();
        while (server.hasPendingDatagrams()) {
            auto packet = QJsonDocument::fromJson(server.receiveDatagram().data()).object();
            if (packet.value("type").toString() == QStringLiteral("command")) commands.append(packet);
        }
        QTest::qWait(10);
    }
    QCOMPARE(commands.size(), 2);
    QCOMPARE(commands[0].value("request"), commands[1].value("request"));
    reply({{"type", "ack"}, {"request", commands[0].value("request")}, {"cmd", int(NetworkController::CmdManualLeft)}});
    QTest::qWait(30);
    QTRY_VERIFY_WITH_TIMEOUT(!control.isOpen(), 1600);
    QTRY_VERIFY_WITH_TIMEOUT(!loss.isEmpty(), 500);
    control.disconnectDevice();
}

void NetworkControllerTest::bridgeOverWifi()
{
    const QString host = qEnvironmentVariable("LUCKFOX_TEST_HOST");
    if (host.isEmpty()) QSKIP("Wireless integration requires the isolated board test endpoint on UDP 5006");
    NetworkController control;
    int mode = -1;
    float horizontal = -1;
    connect(&control, &NetworkController::telemetryReceived, this, [&](int value, float angle, float) {
        mode = value; horizontal = angle;
    });
    control.connectToDevice(host, 5006);
    QTRY_VERIFY_WITH_TIMEOUT(control.isOpen(), 3000);
    QCOMPARE(mode, 0);
    control.sendCommand(NetworkController::CmdManualLeft);
    QTRY_VERIFY_WITH_TIMEOUT(horizontal == 89.5f, 2000);
    QTest::qWait(300); // 板端故意丢弃首次网络 ACK，必须重发同一 request 而不重复步进。
    QCOMPARE(horizontal, 89.5f);
    control.sendCommand(NetworkController::CmdSwitchMode);
    QTRY_COMPARE_WITH_TIMEOUT(mode, 1, 2000);
    control.sendTrackData(-100, 50);
    QTRY_VERIFY_WITH_TIMEOUT(horizontal != 89.5f, 2000);
    control.sendCommand(NetworkController::CmdCenter);
    QTRY_COMPARE_WITH_TIMEOUT(horizontal, 90.0f, 2000);
    control.disconnectDevice();
}

QTEST_GUILESS_MAIN(NetworkControllerTest)
#include "networkcontroller_test.moc"
