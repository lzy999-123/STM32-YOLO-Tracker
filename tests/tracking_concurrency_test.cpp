#include "mainwindow.h"
#include <QtTest>
#include <QMessageBox>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QComboBox>
#include <QPushButton>
#include <QDateTime>
#include <chrono>

class TrackingConcurrencyTest : public QObject
{
    Q_OBJECT
private slots:
    void modalWarningReleasesModeLock_data();
    void modalWarningReleasesModeLock();
    void inferenceCallbacksDoNotBlockGui_data();
    void inferenceCallbacksDoNotBlockGui();
    void initializationSignalReleasesInputLock();
    void trackingSurvivesFramesAndStopsOnControlLoss_data();
    void trackingSurvivesFramesAndStopsOnControlLoss();
    void controlConnectionSurvivesVideoCloseAndRefresh();
    void liveDeviceStartupVideoAndRefresh();
    void handshakeRetriesWithNewSessionAfterSilence();
};

void TrackingConcurrencyTest::handshakeRetriesWithNewSessionAfterSilence()
{
    NetworkController control;
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QString firstSession;
    bool acceptedNewSession = false;
    connect(&server, &QUdpSocket::readyRead, &control, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            const auto session = request.value("session").toString();
            if (firstSession.isEmpty()) firstSession = session;
            if (session == firstSession) continue; // Simulate an expired, retired handshake.
            acceptedNewSession = true;
            QJsonObject welcome{{"type", "welcome"}, {"version", 1}, {"session", session}};
            server.writeDatagram(QJsonDocument(welcome).toJson(QJsonDocument::Compact), packet.senderAddress(), packet.senderPort());
        }
    });
    control.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY_WITH_TIMEOUT(control.isConnected(), 3000);
    QVERIFY(acceptedNewSession);
    QVERIFY(!control.isOpen()); // UART telemetry is still required before enabling control.
}

void TrackingConcurrencyTest::liveDeviceStartupVideoAndRefresh()
{
    if (!qEnvironmentVariableIsSet("QT_LIVE_DEVICE_WORKFLOW"))
        QSKIP("Opt-in test requires the actual Luckfox and STM32 on the same network");
    MainWindow window;
    window.show();
    QTRY_VERIFY_WITH_TIMEOUT(window.m_networkController.isOpen(), 20000);
    QTest::qWait(2000);
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(window.m_lastFrame.empty());
    qInfo() << "LIVE startup_control_online_video_idle";
    auto *open = window.findChild<QPushButton *>(QStringLiteral("pushButton_9"));
    auto *refresh = window.findChild<QPushButton *>(QStringLiteral("pushButton_8"));
    QVERIFY(open && refresh);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_lastFrame.empty(), 20000);
    QCOMPARE(window.m_lastFrame.cols, 1280);
    QCOMPARE(window.m_lastFrame.rows, 720);
    qInfo() << "LIVE clicked_open_native_720p_frame_received";
    QTest::qWait(5000);
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QTRY_VERIFY(window.m_networkController.isOpen());
    qInfo() << "LIVE closed_video_control_still_online";
    QCOMPARE(window.centralWidget()->childAt(refresh->geometry().center()), refresh);
    QTest::mouseClick(refresh, Qt::LeftButton);
    QVERIFY(window.m_luckfoxDiscovery.isActive());
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_luckfoxDiscovery.isActive(), 10000);
    QVERIFY(!window.m_deviceUrl.isEmpty());
    QTRY_VERIFY(window.m_networkController.isOpen());
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(refresh->isEnabled());
    qInfo() << "LIVE refreshed_control_online_video_idle";
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_lastFrame.empty(), 20000);
    qInfo() << "LIVE reopened_video_frame_received";
    window.close();
}

void TrackingConcurrencyTest::controlConnectionSurvivesVideoCloseAndRefresh()
{
    MainWindow window(nullptr, false);
    window.show();
    QCoreApplication::processEvents();
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(5005)));
    int sequence = 0;
    int stops = 0;
    connect(&server, &QUdpSocket::readyRead, &window, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            const auto type = request.value("type").toString();
            auto reply = [&](QJsonObject body) {
                body.insert("version", 1); body.insert("session", request.value("session"));
                server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact), packet.senderAddress(), packet.senderPort());
            };
            if (type == "hello") reply({{"type", "welcome"}});
            if (type == "hello" || type == "heartbeat")
                reply({{"type", "status"}, {"seq", ++sequence}, {"online", true}, {"mode", 0}, {"horizontal", 0}, {"vertical", 0}});
            if (type == "command") {
                if (request.value("cmd").toInt() == NetworkController::CmdTrackOff) ++stops;
                reply({{"type", "ack"}, {"request", request.value("request")}, {"cmd", request.value("cmd")}});
            }
        }
    });
    // Deliver a discovered loopback address without probing or controlling real hardware.
    emit window.m_luckfoxDiscovery.found(QStringLiteral("rtsp://127.0.0.1/live/0"));
    QTRY_VERIFY(window.m_networkController.isOpen());
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(window.m_cameraManager.currentCameraId().isEmpty());
    auto *refresh = window.findChild<QPushButton *>(QStringLiteral("pushButton_8"));
    auto *address = window.findChild<QLabel *>(QStringLiteral("label_9"));
    auto *open = window.findChild<QPushButton *>(QStringLiteral("pushButton_9"));
    auto *sources = window.findChild<QComboBox *>(QStringLiteral("comboBox_3"));
    QVERIFY(refresh && address && open && sources);
    QVERIFY2(!refresh->geometry().intersects(address->geometry()), "Device address label covers the refresh button");
    QCOMPARE(window.centralWidget()->childAt(refresh->geometry().center()), refresh);
    sources->setEditText(QStringLiteral("rtsp://127.0.0.1/live/0"));
    QTest::mouseClick(refresh, Qt::LeftButton);
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(window.m_networkController.isOpen());
    // Simulate an already-open video channel, then exercise the real close button.
    window.m_cameraManager.m_currentCameraId = QStringLiteral("rtsp://127.0.0.1/live/0");
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Open;
    emit window.m_cameraManager.stateChanged(CameraManager::CameraState::Open);
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(window.m_networkController.isOpen());
    QTRY_VERIFY(stops > 0);
    QTest::mouseClick(refresh, Qt::LeftButton);
    QCOMPARE(window.m_cameraManager.state(), CameraManager::CameraState::Idle);
    QVERIFY(window.m_networkController.isOpen());
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Error;
    emit window.m_cameraManager.stateChanged(CameraManager::CameraState::Error);
    QVERIFY(window.m_networkController.isOpen());
}

void TrackingConcurrencyTest::modalWarningReleasesModeLock_data()
{
    QTest::addColumn<bool>("start");
    QTest::newRow("select") << false;
    QTest::newRow("start") << true;
}

void TrackingConcurrencyTest::modalWarningReleasesModeLock()
{
    QFETCH(bool, start);
    MainWindow window(nullptr, false);
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(server.hasPendingDatagrams());
    const auto hello = server.receiveDatagram();
    const auto session = QJsonDocument::fromJson(hello.data()).object().value("session").toString();
    auto reply = [&](QJsonObject body) {
        body.insert("version", 1); body.insert("session", session);
        server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact), hello.senderAddress(), hello.senderPort());
    };
    reply({{"type", "welcome"}});
    QTRY_VERIFY(window.m_networkController.isConnected());
    reply({{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 0}, {"horizontal", 0}, {"vertical", 0}});
    QTRY_VERIFY(window.m_networkController.isOpen());
    bool inspected = false;
    bool unlocked = false;
    QTimer::singleShot(20, &window, [&] {
        inspected = true;
        unlocked = window.m_modeMutex.tryLock();
        if (unlocked) window.m_modeMutex.unlock();
        // Queue actual telemetry while the modal event loop is still running.
        // On the unsafe baseline avoid entering the deadlock so the test can report failure.
        if (unlocked) reply({{"type", "status"}, {"seq", 2}, {"online", true}, {"mode", 1}, {"horizontal", 0}, {"vertical", 0}});
        QTimer::singleShot(50, &window, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets())
                if (auto *box = qobject_cast<QMessageBox *>(widget)) box->accept();
        });
    });
    if (start) window.on_btnStartTracking_clicked();
    else window.on_btnSelectTarget_clicked();
    QVERIFY(inspected);
    QVERIFY2(unlocked, "Modal dialog holds m_modeMutex: reentrant telemetry will deadlock the GUI");
    QCOMPARE(window.m_currentMode, 1);
}

void TrackingConcurrencyTest::inferenceCallbacksDoNotBlockGui_data()
{
    QTest::addColumn<bool>("detection");
    QTest::newRow("tracking-result") << false;
    QTest::newRow("recovery-result") << true;
}

void TrackingConcurrencyTest::inferenceCallbacksDoNotBlockGui()
{
    QFETCH(bool, detection);
    TrackingEngine engine;
    engine.setCurrentModel(QStringLiteral("missing-concurrency-test-model.onnx"));
    DnnThread *thread = engine.m_dnnThread;
    thread->requestInterruption();
    thread->stopDnn();
    thread->wait();
    QCoreApplication::processEvents();
    std::atomic<bool> held{false};
    std::thread slowTracker([&] {
        std::lock_guard<std::recursive_mutex> lock(engine.m_stateMutex);
        held = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    });
    while (!held) std::this_thread::yield();
    std::thread inference([&] {
        if (detection) emit thread->yoloDetectionResult(cv::Rect2d(), -1, 0, 1, true);
        else emit thread->dnnTrackedResult(cv::Rect2d(), false, QString());
    });
    QElapsedTimer clock;
    clock.start();
    qint64 heartbeatAt = -1;
    QTimer::singleShot(20, &engine, [&] { heartbeatAt = clock.elapsed(); });
    while (heartbeatAt < 0 && clock.elapsed() < 1000) {
        QCoreApplication::processEvents();
        QTest::qWait(1);
    }
    slowTracker.join();
    inference.join();
    qInfo() << "GUI heartbeat_ms=" << heartbeatAt << "injected_tracker_lock_ms=350";
    QVERIFY2(heartbeatAt >= 0 && heartbeatAt < 150,
        "Inference result waited for the tracking lock on the GUI thread");
}

void TrackingConcurrencyTest::initializationSignalReleasesInputLock()
{
    DnnThread thread(QStringLiteral("yolo26n.onnx"));
    QVERIFY(!thread.isNetEmpty());
    std::atomic<bool> emitted{false};
    std::atomic<bool> unlocked{false};
    connect(&thread, &DnnThread::dnnTrackedResult, &thread,
        [&](const cv::Rect2d &, bool, const QString &) {
            unlocked = thread.m_mutex.tryLock();
            if (unlocked) thread.m_mutex.unlock();
            emitted = true;
        }, Qt::DirectConnection);
    thread.start();
    thread.initDnn(cv::Mat::zeros(360, 640, CV_8UC3), cv::Rect2d(100, 100, 80, 80));
    QElapsedTimer deadline;
    deadline.start();
    while (!emitted && deadline.elapsed() < 15000) QTest::qWait(10);
    thread.requestInterruption();
    thread.stopDnn();
    thread.wait();
    QVERIFY(emitted);
    QVERIFY2(unlocked, "Initialization emits while holding the input mutex: direct callbacks can invert the state/input lock order");
}

void TrackingConcurrencyTest::trackingSurvivesFramesAndStopsOnControlLoss_data()
{
    QTest::addColumn<int>("backend");
    QTest::newRow("yolo26s") << 0;
    QTest::newRow("yolo26n") << 1;
    QTest::newRow("csrt-orb") << 2;
}

void TrackingConcurrencyTest::trackingSurvivesFramesAndStopsOnControlLoss()
{
    QFETCH(int, backend);
    MainWindow window(nullptr, false);
    auto *models = window.findChild<QComboBox *>(QStringLiteral("comboBox_4"));
    QVERIFY(models);
    models->setCurrentIndex(backend);
    QTRY_VERIFY_WITH_TIMEOUT(window.m_trackingEngine.yoloWarmedUp(), 10000);
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QString session;
    QHostAddress peer;
    quint16 peerPort = 0;
    int sequence = 0;
    bool repliesEnabled = true;
    auto reply = [&](QJsonObject body) {
        if (!repliesEnabled || session.isEmpty()) return;
        body.insert("version", 1); body.insert("session", session);
        server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact), peer, peerPort);
    };
    connect(&server, &QUdpSocket::readyRead, &window, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            if (request.value("type").toString() == QStringLiteral("hello")) {
                peer = packet.senderAddress(); peerPort = packet.senderPort();
                session = request.value("session").toString(); sequence = 0;
                reply({{"type", "welcome"}});
            } else if (request.value("type").toString() == QStringLiteral("command")) {
                reply({{"type", "ack"}, {"request", request.value("request")}, {"cmd", request.value("cmd")}});
            }
        }
    });
    QTimer telemetry;
    telemetry.setInterval(100);
    connect(&telemetry, &QTimer::timeout, &window, [&] {
        reply({{"type", "status"}, {"seq", ++sequence}, {"online", true}, {"mode", 1}, {"horizontal", 0}, {"vertical", 0}});
    });
    telemetry.start();
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(window.m_networkController.isOpen());
    // Synthetic 720p input exercises display and real inference/CSRT, with no camera or motion commands.
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Open;
    window.m_cameraManager.m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch());
    cv::Mat frame(720, 1280, CV_8UC3);
    cv::RNG rng(7124);
    rng.fill(frame, cv::RNG::UNIFORM, 0, 255);
    window.handleNewMatFrame(frame);
    QCoreApplication::processEvents();
    window.m_selectedRect = cv::Rect2d(400, 220, 90, 100);
    window.m_hasSelectedTarget = true;
    window.on_btnStartTracking_clicked();
    QVERIFY(window.m_isCapturing);
    QElapsedTimer clock;
    clock.start();
    qint64 lastHeartbeat = clock.elapsed();
    qint64 maxGap = 0;
    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setTimerType(Qt::PreciseTimer);
    heartbeat.setInterval(20);
    connect(&heartbeat, &QTimer::timeout, &window, [&] {
        const auto now = clock.elapsed();
        maxGap = qMax(maxGap, now - lastHeartbeat);
        lastHeartbeat = now;
        ++heartbeatCount;
    });
    heartbeat.start();
    QTimer video;
    video.setInterval(33);
    connect(&video, &QTimer::timeout, &window, [&] {
        window.m_cameraManager.m_lastFrameTime.store(QDateTime::currentMSecsSinceEpoch());
        window.handleNewMatFrame(frame);
    });
    video.start();
    QTest::qWait(650);
    if (backend == 2) QVERIFY(window.m_isTargetTracked);
    // Remove the target to exercise loss/recovery callbacks while rendering continues.
    frame = cv::Mat::zeros(720, 1280, CV_8UC3);
    QTest::qWait(650);
    QVERIFY(window.m_networkController.isOpen());
    QVERIFY(window.m_isCapturing);
    repliesEnabled = false;
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_networkController.isOpen(), 2500);
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_isCapturing, 2500);
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("btnStartTracking"))->isEnabled());
    qInfo() << "backend=" << backend << "GUI_max_heartbeat_gap_ms=" << maxGap << "heartbeats=" << heartbeatCount;
    QVERIFY(heartbeatCount > 20);
    QVERIFY2(maxGap < 1500, "Tracking or disconnect blocked the whole GUI for more than 1.5s");
    video.stop();
    heartbeat.stop();
    telemetry.stop();
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Idle;
}

QTEST_MAIN(TrackingConcurrencyTest)
#include "tracking_concurrency_test.moc"
