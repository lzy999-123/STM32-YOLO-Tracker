#include "networkcontroller.h"
#include <QtTest>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QMessageAuthenticationCode>

class NetworkControllerTest : public QObject
{
    Q_OBJECT
private slots:
    void telemetryGatesControlsAndRejectsOldStatus();
    void commandRetryKeepsRequestIdAndNetworkLossDisablesControls();
    void authSigningAndVerification();
    void telemetryFlagsAreReported();
    void bridgeOverWifi();
};

namespace {
const QByteArray kKey = QByteArray::fromHex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
}

void NetworkControllerTest::authSigningAndVerification()
{
    QString error;
    QCOMPARE(NetworkController::parseAuthKey("ab", &error), QByteArray());
    QVERIFY(!error.isEmpty());
    QCOMPARE(NetworkController::parseAuthKey(QByteArray(32, 'z'), &error), QByteArray());
    QCOMPARE(NetworkController::parseAuthKey(" " + QByteArray(32, 'A') + "\r\n", &error), QByteArray(16, char(0xaa)));

    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    NetworkController control;
    control.setAuthKey(kKey);
    QVERIFY(control.isAuthEnabled());
    control.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(server.hasPendingDatagrams());
    const auto hello = server.receiveDatagram();
    // 与板端 Python 相同的线格式：{"m": 内层文本, "auth": hex(HMAC-SHA256(key, utf8(m)))}。
    const auto envelope = QJsonDocument::fromJson(hello.data()).object();
    QCOMPARE(envelope.size(), 2);
    const QByteArray inner = envelope.value("m").toString().toUtf8();
    QCOMPARE(envelope.value("auth").toString().toLatin1(),
             QMessageAuthenticationCode::hash(inner, kKey, QCryptographicHash::Sha256).toHex());
    const auto session = QJsonDocument::fromJson(inner).object().value("session").toString();
    QVERIFY(!session.isEmpty());
    auto body = [&](const char *type) {
        return QJsonDocument(QJsonObject{{"type", type}, {"version", 1}, {"session", session}}).toJson(QJsonDocument::Compact);
    };
    // 未签名、错误密钥、篡改内容均被丢弃。
    server.writeDatagram(body("welcome"), hello.senderAddress(), hello.senderPort());
    server.writeDatagram(NetworkController::sealMessage(body("welcome"), QByteArray(32, 'x')), hello.senderAddress(), hello.senderPort());
    auto tampered = QJsonDocument::fromJson(NetworkController::sealMessage(body("status"), kKey)).object();
    tampered.insert("m", QString::fromUtf8(body("welcome")));
    server.writeDatagram(QJsonDocument(tampered).toJson(QJsonDocument::Compact), hello.senderAddress(), hello.senderPort());
    QTest::qWait(200);
    QVERIFY(!control.isConnected());
    server.writeDatagram(NetworkController::sealMessage(body("welcome"), kKey), hello.senderAddress(), hello.senderPort());
    QTRY_VERIFY(control.isConnected());

    QByteArray opened;
    // 板端 Python seal() 生成的报文（含 \u 转义的非 ASCII 字符），验证跨语言互通。
    const QByteArray fromPython = R"({"m":"{\"type\":\"welcome\",\"session\":\"中\"}","auth":"28225ff80b0594e82e061adedca6e1c9998dd62afbfa54f47ebb50228fe45c53"})";
    QVERIFY(NetworkController::openMessage(fromPython, kKey, &opened));
    QCOMPARE(QString::fromUtf8(opened), QStringLiteral("{\"type\":\"welcome\",\"session\":\"中\"}"));
    QVERIFY(NetworkController::openMessage(body("x"), QByteArray(), &opened));
    QCOMPARE(opened, body("x"));
    QVERIFY(!NetworkController::openMessage(body("x"), kKey, &opened));
    control.disconnectDevice();
}

void NetworkControllerTest::telemetryFlagsAreReported()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    NetworkController control;
    control.setAuthKey({});
    QSignalSpy flags(&control, &NetworkController::telemetryFlagsChanged);
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
    reply({{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 1}, {"horizontal", 0}, {"vertical", 0}});
    QTRY_VERIFY(control.isOpen());
    QCOMPARE(control.telemetryFlags(), 0); // 旧板端无 flags 字段
    QVERIFY(flags.isEmpty());
    reply({{"type", "status"}, {"seq", 2}, {"online", true}, {"mode", 1}, {"horizontal", 0}, {"vertical", 0},
           {"flags", NetworkController::FlagEmergencyStop | NetworkController::FlagLinkLost}});
    QTRY_COMPARE(flags.size(), 1);
    QCOMPARE(flags.takeFirst().at(0).toInt(), 6);
    QCOMPARE(control.telemetryFlags(), 6);
    control.disconnectDevice();
    QCOMPARE(flags.size(), 1);
    QCOMPARE(flags.takeFirst().at(0).toInt(), 0);
}

void NetworkControllerTest::telemetryGatesControlsAndRejectsOldStatus()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    NetworkController control;
    control.setAuthKey({});
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
    control.setAuthKey({});
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
