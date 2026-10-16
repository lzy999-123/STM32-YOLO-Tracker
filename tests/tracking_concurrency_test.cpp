#include "mainwindow.h"
#include "luckfoxnetworkroute.h"
#include "yoloinputgeometry.h"
#include <QSettings>
#include <QScopeGuard>
#include <QtTest>
#include <QMessageBox>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QComboBox>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QMenu>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <chrono>
#include <array>

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
    void staleTrackingSignalsAreDroppedByGeneration();
    void trackDataOnlyForNewResults();
    void engineHoldAndStaleDnnSessionGiveNoOffset();
    void yoloResultsReachControlWithoutAnotherVideoFrame();
    void discoveryPrefersEthernetAndFallsBackToWifi();
    void yoloLetterboxPreservesImageAndControlCoordinates();
    void yoloSizeChangesDoNotMoveControlCenter();
    void yoloPublicImageMatchesOfficialReference();
    void installationCorrectionPreservesControlOffsets();
    void installedCameraTrackingDirections_data();
    void installedCameraTrackingDirections();
    void displayedClockDoesNotModifyTrackingFrames();
    void manualButtonsUseEditableStepAndRespectMode();
    void calibrationLabelsUseReferenceAndSignedLiveAngles();



    void cameraErrorDiscardsPreviousTrackingFrame();
    void cameraCloseDoesNotWaitForOpeningNetworkWorker();
    void rtspFramePublicationIsIsolatedAcrossSessions();
    void rtspLatestFrameCoalescesBursts();
    void rtspWatchdogUsesHistoricalTimeout();
    void researchFrameFollowsCoalescedInput();
    void researchDnnPendingFrameKeepsIdentity();
    void researchRealInferenceCarriesFrame();

};

void TrackingConcurrencyTest::yoloSizeChangesDoNotMoveControlCenter()
{
    TrackingEngine engine;
    engine.m_isCapturing = true;
    engine.m_frameSize = cv::Size(1280, 720);
    for (const cv::Size size : {cv::Size(80, 100), cv::Size(240, 300),
                               cv::Size(60, 80), cv::Size(180, 200)}) {
        engine.onDnnResultReceived(cv::Rect2d(640 - size.width / 2,
                                              360 - size.height / 2,
                                              size.width, size.height), true, "cell phone");
        QCOMPARE(engine.currentOffsetX(), 0);
        QCOMPARE(engine.currentOffsetY(), 0);
        QCOMPARE(engine.currentTrackedRect().width, double(size.width));
    }
    engine.onDnnResultReceived(cv::Rect2d(720, 390, 80, 100), true, "cell phone");
    QCOMPARE(engine.currentOffsetX(), 120);
    QCOMPARE(engine.currentOffsetY(), 80);
}

void TrackingConcurrencyTest::yoloPublicImageMatchesOfficialReference()
{
    const QString root = qEnvironmentVariable("QT_YOLO_REFERENCE_ROOT");
    if (root.isEmpty()) QSKIP("Set QT_YOLO_REFERENCE_ROOT to test the real exported model.");
    const cv::Mat frame = cv::imread((root + "/tools/bus.jpg").toStdString());
    QVERIFY(!frame.empty());
    DnnThread thread(root + "/yolo26s.onnx");
    QSignalSpy results(&thread, &DnnThread::yoloDetectionResult);
    thread.start();
    auto cleanup = qScopeGuard([&] { thread.requestInterruption(); thread.stopDnn(); thread.wait(); });
    QTRY_VERIFY_WITH_TIMEOUT(thread.isWarmedUp() || thread.isLoadFailed(), 60000);
    QVERIFY(thread.isWarmedUp());
    thread.requestDetections(frame, 42);
    QTRY_VERIFY_WITH_TIMEOUT(!results.empty() && results.last().at(4).toBool(), 10000);
    // Ultralytics 8.4.80, ONNX Runtime CPU, public bus.jpg, imgsz=640.
    // Compare class, confidence and every edge through production preprocessing
    // and output decoding (GPU round-off and integer box rounding allowed).
    const std::array<cv::Rect2d, 5> boxes = {{
        {6.298, 229.608, 796.771, 511.847},
        {668.058, 392.172, 141.942, 488.223},
        {49.441, 398.939, 197.243, 504.024},
        {222.079, 406.977, 122.091, 454.394},
        {0, 550.283, 77.802, 324.589}}};
    const std::array<float, 5> confidence = {{0.969829f, 0.943062f, 0.937683f, 0.911363f, 0.629834f}};
    QCOMPARE(results.size(), 6);
    for (int i = 0; i < 5; ++i) {
        const auto result = results.at(i);
        QCOMPARE(result.at(1).toInt(), i == 0 ? 5 : 0);
        QVERIFY(std::abs(result.at(2).toFloat() - confidence[i]) < 0.015f);
        const cv::Rect2d actual = qvariant_cast<cv::Rect2d>(result.at(0));
        QVERIFY(std::abs(actual.x - boxes[i].x) < 2.0);
        QVERIFY(std::abs(actual.y - boxes[i].y) < 2.0);
        QVERIFY(std::abs(actual.width - boxes[i].width) < 2.0);
        QVERIFY(std::abs(actual.height - boxes[i].height) < 2.0);
    }

}

void TrackingConcurrencyTest::yoloLetterboxPreservesImageAndControlCoordinates()
{
    for (const cv::Size sourceSize : {cv::Size(1280, 720), cv::Size(720, 1280),
                                     cv::Size(640, 640), cv::Size(960, 541)}) {
        YoloInputGeometry geometry(sourceSize);
        cv::Mat image(sourceSize, CV_8UC3, cv::Scalar(11, 22, 33));
        const cv::Mat input = geometry.prepare(image);
        QCOMPARE(input.cols, 640); QCOMPARE(input.rows, 640);
        QCOMPARE(input.at<cv::Vec3b>(320, 320), cv::Vec3b(11, 22, 33));
        if (geometry.top) QCOMPARE(input.at<cv::Vec3b>(0, 320), cv::Vec3b(114, 114, 114));
        if (geometry.left) QCOMPARE(input.at<cv::Vec3b>(320, 0), cv::Vec3b(114, 114, 114));
        const cv::Rect expected(sourceSize.width / 3, sourceSize.height / 4, 80, 100);
        const cv::Rect restored = geometry.sourceRect(
            geometry.left + expected.x * geometry.scale,
            geometry.top + expected.y * geometry.scale,
            geometry.left + (expected.x + expected.width) * geometry.scale,
            geometry.top + (expected.y + expected.height) * geometry.scale);
        QCOMPARE(restored, expected);
        QCOMPARE(geometry.sourceRect(-20, -20, 660, 660), cv::Rect(0, 0, sourceSize.width, sourceSize.height));
        if (geometry.top) QVERIFY(geometry.sourceRect(0, 0, 80, geometry.top - 1).empty());
    }
    const YoloInputGeometry video(cv::Size(1280, 720));
    QCOMPARE(video.resizedSize, cv::Size(640, 360));
    QCOMPARE(video.top, 140);
    // A camera target 80px below center must still give +80px after inference.
    const cv::Rect restored = video.sourceRect(300, 340, 340, 380);
    QCOMPARE(restored.y + restored.height / 2 - 360, 80);
}

void TrackingConcurrencyTest::rtspFramePublicationIsIsolatedAcrossSessions()
{
    CameraManager camera;
    camera.m_cameraState = CameraManager::CameraState::Open;
    camera.m_rtspRunning = true;
    const quint64 oldSession = camera.m_rtspSessionId.load();
    QSignalSpy frames(&camera, &CameraManager::matReady);
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(11, 11, 11)), oldSession);
    QVERIFY(camera.m_rtspDispatchPending.load());
    camera.stopRtspStream(); // 旧 GUI 通知仍在事件队列中。
    camera.m_rtspRunning = true;
    const quint64 newSession = camera.m_rtspSessionId.load();
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(22, 22, 22)), oldSession);
    QVERIFY(!camera.m_rtspDispatchPending.load());
    QVERIFY(camera.m_latestRtspMat.empty());
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(33, 33, 33)), newSession);
    camera.notifyNewMat(oldSession); // 旧通知不能清除新会话的标志或取走新画面。
    QVERIFY(camera.m_rtspDispatchPending.load());
    QTRY_COMPARE(frames.count(), 1);
    QCOMPARE(qvariant_cast<cv::Mat>(frames.at(0).at(0)).at<cv::Vec3b>(0, 0)[0], uchar(33));
    QVERIFY(!camera.m_rtspDispatchPending.load());
    camera.closeCamera();
}

void TrackingConcurrencyTest::rtspLatestFrameCoalescesBursts()
{
    CameraManager camera;
    camera.m_cameraState = CameraManager::CameraState::Open;
    camera.m_isRtsp = camera.m_rtspRunning = true;
    const quint64 session = camera.m_rtspSessionId.load();
    QSignalSpy frames(&camera, &CameraManager::matReady);
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(11, 11, 11)), session);
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(22, 22, 22)), session);
    QCOMPARE(frames.count(), 0);
    QCoreApplication::processEvents();
    QCOMPARE(frames.count(), 1);
    QCOMPARE(qvariant_cast<cv::Mat>(frames.at(0).at(0)).at<cv::Vec3b>(0, 0)[0], uchar(22));
    QVERIFY(!camera.m_rtspDispatchPending.load());
    camera.publishRtspFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(33, 33, 33)), session);
    QCoreApplication::processEvents();
    QCOMPARE(frames.count(), 2);
    QCOMPARE(qvariant_cast<cv::Mat>(frames.at(1).at(0)).at<cv::Vec3b>(0, 0)[0], uchar(33));
    camera.closeCamera();
}

void TrackingConcurrencyTest::researchFrameFollowsCoalescedInput()
{
    if (!ResearchTrace::enabled()) QSKIP("Set QT_RESEARCH_LOG_DIR to exercise metadata");
    CameraManager camera;
    camera.m_cameraState = CameraManager::CameraState::Open;
    camera.m_rtspRunning = true;
    const auto session = camera.m_rtspSessionId.load();
    camera.publishRtspFrame(cv::Mat(2,2,CV_8UC3,cv::Scalar(1,1,1)),session,100,3);
    const auto first = camera.m_latestResearchFrame.id;
    camera.publishRtspFrame(cv::Mat(2,2,CV_8UC3,cv::Scalar(2,2,2)),session,200,1);
    camera.notifyNewMat(session);
    const auto delivered = camera.deliveredResearchFrame();
    QCOMPARE(delivered.id, first + 1);
    QCOMPARE(delivered.hostReceiveUs, 200);
    QCOMPARE(delivered.rotation, 1);
    camera.stopRtspStream();
    camera.m_rtspRunning = true;
    camera.publishRtspFrame(cv::Mat(2,2,CV_8UC3),session,300,0);
    QVERIFY(camera.m_latestRtspMat.empty());
    camera.closeCamera();
}

void TrackingConcurrencyTest::researchDnnPendingFrameKeepsIdentity()
{
    DnnThread dnn("nonexistent-test-model.onnx"); // Does not start model loading.
    dnn.m_isTracking = true;
    const cv::Mat frame(2,2,CV_8UC3,cv::Scalar(0,0,0));
    dnn.updateDnn(frame, ResearchFrame{4,2,100,-1,3});
    dnn.updateDnn(frame, ResearchFrame{9,2,200,-1,3});
    QCOMPARE(dnn.m_researchFrame.id, quint64(9));
    QCOMPARE(dnn.m_researchFrame.hostReceiveUs, 200);
    dnn.stopDnn();
    QVERIFY(!dnn.m_hasNewFrame);
}

void TrackingConcurrencyTest::researchRealInferenceCarriesFrame()
{
    if (!ResearchTrace::enabled()) QSKIP("Set QT_RESEARCH_LOG_DIR for inference trace test");
    const QString root = qEnvironmentVariable("QT_RESEARCH_TEST_ROOT");
    if (root.isEmpty()) QSKIP("Set QT_RESEARCH_TEST_ROOT for local public image and model");
    const cv::Mat frame = cv::imread((root + "/tools/bus.jpg").toStdString());
    QVERIFY(!frame.empty());
    DnnThread dnn(root + "/yolo26s.onnx");
    QSignalSpy results(&dnn, &DnnThread::dnnTrackedResult);
    dnn.start();
    auto cleanup = qScopeGuard([&] { dnn.requestInterruption(); dnn.stopDnn(); dnn.wait(); });
    QTRY_VERIFY_WITH_TIMEOUT(dnn.isWarmedUp() || dnn.isLoadFailed(), 60000);
    QVERIFY(dnn.isWarmedUp());
    dnn.initDnn(frame, cv::Rect2d(6,230,797,512));
    QTRY_VERIFY_WITH_TIMEOUT(!results.empty(), 10000);
    QVERIFY(results.last().at(1).toBool());
    const int before = results.count();
    const ResearchFrame stamp{1001,9001,ResearchTrace::nowUs(),-1,0};
    auto fields = stamp.json();
    fields.insert("width",frame.cols); fields.insert("height",frame.rows);
    fields.insert("source","public_image_test_not_camera");
    ResearchTrace::record("frame",fields);
    dnn.updateDnn(frame,stamp);
    QTRY_VERIFY_WITH_TIMEOUT(results.count() > before, 10000);
    QVERIFY(results.last().at(1).toBool());
    QVERIFY(results.last().at(2).toString() != "HOLD");
}

void TrackingConcurrencyTest::rtspWatchdogUsesHistoricalTimeout()
{
    CameraManager camera;
    camera.m_cameraState = CameraManager::CameraState::Open;
    camera.m_currentCameraId = QStringLiteral("rtsp://127.0.0.1:1/live/0");
    camera.m_isRtsp = camera.m_rtspRunning = true;
    camera.m_lastFrameTime = QDateTime::currentMSecsSinceEpoch() - 2000;
    camera.checkCameraStatus();
    QCOMPARE(camera.state(), CameraManager::CameraState::Open);
    QCOMPARE(camera.m_retryCount, 0);
    // Exhaust the retry budget to check the five-second gate without opening a device.
    camera.m_retryCount = 2;
    camera.m_lastFrameTime = QDateTime::currentMSecsSinceEpoch() - 6000;
    QSignalSpy errors(&camera, &CameraManager::cameraError);
    camera.checkCameraStatus();
    QCOMPARE(camera.state(), CameraManager::CameraState::Error);
    QCOMPARE(camera.m_retryCount, 3);
    QCOMPARE(errors.count(), 1);
    QVERIFY(!camera.m_rtspThread);
    QVERIFY(!camera.m_rtspRunning.load());
}
void TrackingConcurrencyTest::cameraErrorDiscardsPreviousTrackingFrame()
{
    MainWindow window(nullptr, false);
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Open;
    window.m_lastFrame = cv::Mat(720, 1280, CV_8UC3, cv::Scalar(1, 2, 3));
    window.m_pendingMat = window.m_lastFrame;
    window.m_hasPendingMat = true;
    window.m_isCapturing = true;
    window.m_offsetX = 200;
    window.m_offsetY = -50;
    window.m_trackResultPending = true;
    window.m_displayedFps = 30;
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Error;
    emit window.m_cameraManager.stateChanged(CameraManager::CameraState::Error);
    QVERIFY(window.m_lastFrame.empty());
    QVERIFY(window.m_pendingMat.empty());
    QVERIFY(!window.m_hasPendingMat);
    QVERIFY(!window.m_isCapturing);
    QVERIFY(!window.m_trackResultPending);
    QCOMPARE(window.m_offsetX, int16_t(0));
    QCOMPARE(window.m_offsetY, int16_t(0));
    QCOMPARE(window.m_displayedFps, 0);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("imageLabel"))->text().contains(QStringLiteral("视频中断")));
}

void TrackingConcurrencyTest::cameraCloseDoesNotWaitForOpeningNetworkWorker()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    CameraManager camera;
    camera.openCamera(QStringLiteral("rtsp://127.0.0.1:%1/live/0").arg(server.serverPort()));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 1500);
    // Keep the accepted socket open but send no RTSP response, so the worker is blocked in open.
    auto *peer = server.nextPendingConnection();
    QVERIFY(peer);
    QElapsedTimer elapsed;
    elapsed.start();
    camera.closeCamera();
    QVERIFY2(elapsed.elapsed() < 100, "Closing waited for a network worker");
    QCOMPARE(camera.state(), CameraManager::CameraState::Idle);
    QVERIFY(!camera.m_rtspThread);
    QVERIFY(!camera.m_retiredRtspThreads.empty());
    peer->disconnectFromHost();
    QTRY_VERIFY_WITH_TIMEOUT(camera.m_retiredRtspThreads.front().finished->load(), 6000);
    camera.checkCameraStatus();
    QVERIFY(camera.m_retiredRtspThreads.empty());
}

void TrackingConcurrencyTest::calibrationLabelsUseReferenceAndSignedLiveAngles()
{
    MainWindow window(nullptr, false);
    window.show();
    auto *horizontal = window.findChild<QLabel *>(QStringLiteral("label_16"));
    auto *vertical = window.findChild<QLabel *>(QStringLiteral("label_17"));
    auto *live = window.findChild<QLabel *>(QStringLiteral("gimbalStatus"));
    QVERIFY(horizontal && vertical && live);
    QCOMPARE(horizontal->text(), QStringLiteral("--"));
    QCOMPARE(vertical->text(), QStringLiteral("--"));
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    window.m_networkController.setAuthKey({});
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(server.hasPendingDatagrams());
    const auto hello = server.receiveDatagram();
    const auto session = QJsonDocument::fromJson(hello.data()).object().value("session").toString();
    auto reply = [&](QJsonObject message) {
        message.insert("version", 1); message.insert("session", session);
        server.writeDatagram(QJsonDocument(message).toJson(QJsonDocument::Compact), hello.senderAddress(), hello.senderPort());
    };
    reply({{"type", "welcome"}});
    QTRY_VERIFY(window.m_networkController.isConnected());
    QJsonObject profile{{"valid", true}, {"horizontal", 40}, {"vertical", 200},
        {"horizontal_min", -60}, {"horizontal_max", 70}, {"vertical_min", 140}, {"vertical_max", 235}};
    QJsonObject status{{"type", "status"}, {"seq", 1}, {"online", true}, {"mode", 0},
                       {"horizontal", -60}, {"vertical", 235}, {"calibration", profile}};
    reply(status);
    QTRY_COMPARE(horizontal->text(), QStringLiteral("+40.0°"));
    QCOMPARE(vertical->text(), QStringLiteral("+200.0°"));
    QVERIFY(live->text().contains(QStringLiteral("-60.0°")));
    QVERIFY(live->text().contains(QStringLiteral("+235.0°")));
    QVERIFY(!live->text().contains(QStringLiteral("300.0°")));
    QVERIFY(horizontal->toolTip().contains(QStringLiteral("-60.0°")));
    QVERIFY(horizontal->width() >= horizontal->fontMetrics().horizontalAdvance(horizontal->text()));
    QVERIFY(vertical->width() >= vertical->fontMetrics().horizontalAdvance(vertical->text()));
    // The user's saved Designer geometry remains intact, including after resizing.
    auto *group = window.findChild<QWidget *>(QStringLiteral("groupBox_3"));
    QCOMPARE(group->pos(), QPoint(870, 90));
    window.resize(1200, 750);
    QCoreApplication::processEvents();
    QVERIFY(!horizontal->geometry().intersects(group->geometry()));
    window.grab().save(QStringLiteral("build/manual-step-tests/angle-profile-ui.png"));
    status.insert("seq", 2);
    status.insert("horizontal", 65);
    status.insert("vertical", 150);
    reply(status);
    QTRY_VERIFY(live->text().contains(QStringLiteral("+65.0°")));
    QCOMPARE(horizontal->text(), QStringLiteral("+40.0°"));
    QCOMPARE(vertical->text(), QStringLiteral("+200.0°"));
    status.insert("seq", 3);
    status.remove("calibration");
    reply(status);
    QTRY_COMPARE(horizontal->text(), QStringLiteral("--"));
    QCOMPARE(vertical->text(), QStringLiteral("--"));
    status.insert("seq", 4);
    status.insert("calibration", profile);
    reply(status);
    QTRY_COMPARE(horizontal->text(), QStringLiteral("+40.0°"));
    window.m_networkController.disconnectDevice();
    QTRY_COMPARE(horizontal->text(), QStringLiteral("--"));
    QCOMPARE(vertical->text(), QStringLiteral("--"));
    window.close();
}

void TrackingConcurrencyTest::manualButtonsUseEditableStepAndRespectMode()
{
    MainWindow window(nullptr, false);
    window.show();
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    int mode = 0;
    int sequence = 0;
    QList<QJsonObject> steps;
    connect(&server, &QUdpSocket::readyRead, &window, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            const auto type = request.value("type").toString();
            auto reply = [&](QJsonObject body) {
                body.insert("version", 1); body.insert("session", request.value("session"));
                server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact),
                                     packet.senderAddress(), packet.senderPort());
            };
            if (type == "hello") reply({{"type", "welcome"}});
            if (type == "hello" || type == "heartbeat")
                reply({{"type", "status"}, {"seq", ++sequence}, {"online", true}, {"mode", mode},
                       {"horizontal", 0}, {"vertical", 0}});
            if (type == "manual_step") {
                steps.append(request);
                reply({{"type", "ack"}, {"request", request.value("request")}, {"cmd", request.value("cmd")}});
            }
        }
    });
    window.m_networkController.setAuthKey({});
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(window.m_networkController.isOpen());
    auto *step = window.findChild<QDoubleSpinBox *>(QStringLiteral("doubleSpinBox"));
    QVERIFY(step);
    QCOMPARE(step->minimum(), 0.1);
    QCOMPARE(step->maximum(), 10.0);
    QCOMPARE(step->value(), 0.5);
    QVERIFY(!step->isReadOnly());
    // Exercise the real editor, increment/decrement buttons and four direction buttons.
    step->setFocus();
    QTest::keyClick(step, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(step, "2.5");
    QTest::keyClick(step, Qt::Key_Enter);
    QCOMPARE(step->value(), 2.5);
    QTest::keyClick(step, Qt::Key_Up);
    QCOMPARE(step->value(), 2.6);
    QTest::keyClick(step, Qt::Key_Down);
    QCOMPARE(step->value(), 2.5);
    const char *names[] = {"pushButton_4", "pushButton_7", "pushButton_5", "pushButton_6"};
    const int commands[] = {0x20, 0x21, 0x22, 0x23};
    const int tenths[] = {1, 5, 25, 100};
    for (int i = 0; i < 4; ++i) {
        auto *button = window.findChild<QPushButton *>(QString::fromLatin1(names[i]));
        QVERIFY(button && button->isEnabled());
        step->setValue(tenths[i] / 10.0);
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_COMPARE(steps.size(), i + 1);
        QCOMPARE(steps.last().value("cmd").toInt(), commands[i]);
        QCOMPARE(steps.last().value("step_tenths").toInt(), tenths[i]);
    }
    auto *up = window.findChild<QPushButton *>(QStringLiteral("pushButton_4"));
    mode = 1;
    QTRY_VERIFY(!up->isEnabled());
    QTest::mouseClick(up, Qt::LeftButton);
    window.sendManualStep(NetworkController::CmdManualUp, QStringLiteral("向上"));
    QTest::qWait(150);
    QCOMPARE(steps.size(), 4);
    mode = 0;
    QTRY_VERIFY(up->isEnabled());
    window.m_networkController.disconnectDevice();
    QTRY_VERIFY(!up->isEnabled());
    window.close();
}

void TrackingConcurrencyTest::staleTrackingSignalsAreDroppedByGeneration()
{
    MainWindow window(nullptr, false);
    window.m_isCapturing = true;
    const quint64 oldGeneration = window.m_trackingEngine.trackingGeneration();
    // 模拟 CSRT 后台线程在 join 之前排队的旧结果。
    std::thread worker([&] {
        emit window.m_trackingEngine.targetTracked(cv::Rect2d(10, 10, 50, 50), 120, -80, oldGeneration, 1);
    });
    worker.join();
    // 停止并立即开始新会话（例如重新框选/切换模型后再次开始），旧信号随后才送达。
    window.stopTrackingSafely();
    window.m_isCapturing = true;
    QVERIFY(window.m_trackingEngine.trackingGeneration() != oldGeneration);
    QCoreApplication::processEvents();
    QVERIFY2(!window.m_isTargetTracked, "Stale targetTracked from an old session re-enabled tracking");
    QCOMPARE(int(window.m_offsetX), 0);
    QCOMPARE(int(window.m_offsetY), 0);
    QVERIFY(!window.m_trackResultPending);

    // 当前会话的信号仍然有效。
    const quint64 current = window.m_trackingEngine.trackingGeneration();
    emit window.m_trackingEngine.targetTracked(cv::Rect2d(10, 10, 50, 50), 30, 40, current, 2);
    QVERIFY(window.m_isTargetTracked);
    QCOMPARE(int(window.m_offsetX), 30);
    window.m_isCapturing = false;
}

void TrackingConcurrencyTest::trackDataOnlyForNewResults()
{
    MainWindow window(nullptr, false);
    window.m_isCapturing = true;
    const quint64 generation = window.m_trackingEngine.trackingGeneration();
    emit window.m_trackingEngine.targetTracked(cv::Rect2d(10, 10, 50, 50), 30, 40, generation, 5);
    QVERIFY(window.m_trackResultPending); // 控制通道未连接，结果保持待发送
    window.m_trackResultPending = false;  // 模拟已经发送
    emit window.m_trackingEngine.targetTracked(cv::Rect2d(10, 10, 50, 50), 30, 40, generation, 5);
    QVERIFY2(!window.m_trackResultPending, "Unchanged result was queued again for every video frame");
    emit window.m_trackingEngine.targetTracked(cv::Rect2d(12, 10, 50, 50), 32, 40, generation, 6);
    QVERIFY(window.m_trackResultPending);
    window.m_trackResultPending = false;
    emit window.m_trackingEngine.targetLost(generation);
    QVERIFY2(window.m_trackResultPending, "Loss should queue one zero-offset frame");
    QCOMPARE(int(window.m_offsetX), 0);
    window.m_isCapturing = false;
}

void TrackingConcurrencyTest::engineHoldAndStaleDnnSessionGiveNoOffset()
{
    TrackingEngine engine;
    engine.setFrameSize(QSize(1280, 720));
    {
        std::lock_guard<std::recursive_mutex> lock(engine.m_stateMutex);
        engine.m_isCapturing = true;
        engine.m_useFeatureTracking = false;
        engine.m_dnnSessionId = 42;
    }
    engine.onDnnTrackedResult(cv::Rect2d(900, 500, 60, 60), true, QStringLiteral("LOCK:cup"), 42);
    QVERIFY(engine.m_isTargetTracked);
    QVERIFY(engine.m_offsetX != 0);
    const quint64 seq = engine.m_resultSeq;
    // HOLD：沿用旧位置，偏移必须为 0，但仍是一次新结果（下发 0）。
    engine.onDnnTrackedResult(cv::Rect2d(900, 500, 60, 60), true, QStringLiteral("HOLD"), 42);
    QVERIFY(engine.m_isTargetTracked);
    QCOMPARE(engine.m_offsetX, 0);
    QCOMPARE(engine.m_offsetY, 0);
    QVERIFY(engine.m_resultSeq != seq);
    // 未检出但在容忍期内：保持显示，偏移为 0。
    engine.onDnnTrackedResult(cv::Rect2d(900, 500, 60, 60), true, QStringLiteral("cup"), 42);
    QVERIFY(engine.m_offsetX != 0);
    engine.onDnnTrackedResult(cv::Rect2d(), false, QString(), 42);
    QVERIFY(engine.m_isTargetTracked);
    QCOMPARE(engine.m_offsetX, 0);
    // 旧会话的结果被丢弃。
    const quint64 seqBeforeStale = engine.m_resultSeq;
    engine.onDnnTrackedResult(cv::Rect2d(100, 100, 60, 60), true, QStringLiteral("LOCK:cup"), 41);
    QCOMPARE(engine.m_resultSeq, seqBeforeStale);
    QCOMPARE(engine.m_offsetX, 0);
    engine.stopTracking();
    QVERIFY(!engine.m_isTargetTracked);
}

void TrackingConcurrencyTest::yoloResultsReachControlWithoutAnotherVideoFrame()
{
    MainWindow window(nullptr, false);
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QList<QJsonObject> tracks;
    int statusSequence = 0;
    connect(&server, &QUdpSocket::readyRead, &window, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            auto reply = [&](QJsonObject body) {
                body.insert("version", 1); body.insert("session", request.value("session"));
                server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact),
                                     packet.senderAddress(), packet.senderPort());
            };
            const auto type = request.value("type").toString();
            if (type == "hello") reply({{"type", "welcome"}});
            if (type == "hello" || type == "heartbeat")
                reply({{"type", "status"}, {"seq", ++statusSequence}, {"online", true},
                       {"mode", 1}, {"horizontal", 40}, {"vertical", 200}});
            if (type == "track") tracks.append(request);
        }
    });
    window.m_networkController.setAuthKey({});
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(window.m_networkController.isOpen());
    window.m_isCapturing = true;
    auto &engine = window.m_trackingEngine;
    engine.setFrameSize(QSize(1280, 720));
    engine.m_isCapturing = true;
    engine.m_useFeatureTracking = false;
    engine.m_dnnSessionId = 42;
    window.m_lastControlSendTime = QDateTime::currentMSecsSinceEpoch();
    engine.onDnnTrackedResult(cv::Rect2d(900, 500, 60, 60), true, QStringLiteral("LOCK:cup"), 42);
    QTRY_COMPARE_WITH_TIMEOUT(tracks.size(), 1, 250);
    QVERIFY(tracks.last().value("x").toInt() > 0);
    engine.onDnnTrackedResult(cv::Rect2d(900, 500, 60, 60), true, QStringLiteral("HOLD"), 42);
    QTRY_COMPARE_WITH_TIMEOUT(tracks.size(), 2, 250);
    QCOMPARE(tracks.last().value("x").toInt(), 0);
    QCOMPARE(tracks.last().value("y").toInt(), 0);
    engine.onDnnTrackedResult(cv::Rect2d(100, 100, 60, 60), true, QStringLiteral("LOCK:cup"), 41);
    QTest::qWait(30);
    QCOMPARE(tracks.size(), 2);
    window.stopLocalControl();
}

void TrackingConcurrencyTest::discoveryPrefersEthernetAndFallsBackToWifi()
{
    QSettings settings(QStringLiteral("LuckfoxTracker"), QStringLiteral("Camera"));
    const auto oldAddress = settings.value("lastAddress");
    const auto oldId = settings.value("deviceId");
    const auto restore = qScopeGuard([&] {
        if (oldAddress.isValid()) settings.setValue("lastAddress", oldAddress); else settings.remove("lastAddress");
        if (oldId.isValid()) settings.setValue("deviceId", oldId); else settings.remove("deviceId");
    });
    QNetworkAddressEntry ethernet;
    ethernet.setIp(QHostAddress("192.168.8.188")); ethernet.setPrefixLength(24);
    LuckfoxDiscovery discovery;
    discovery.m_ethernetAddresses = {ethernet};
    discovery.m_elapsed.start(); discovery.m_active = true;
    QSignalSpy found(&discovery, &LuckfoxDiscovery::found);
    discovery.considerCandidate(QHostAddress("192.168.110.144"), "luckfox-test");
    QCOMPARE(found.size(), 0);
    discovery.considerCandidate(QHostAddress("192.168.8.93"), "luckfox-test");
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.last().at(0).toString(), QStringLiteral("rtsp://192.168.8.93/live/0"));
    QTest::qWait(180);
    QCOMPARE(found.size(), 1);
    discovery.m_active = true;
    discovery.considerCandidate(QHostAddress("192.168.110.144"), "luckfox-test");
    QTRY_COMPARE_WITH_TIMEOUT(found.size(), 2, 500);
    QCOMPARE(found.last().at(0).toString(), QStringLiteral("rtsp://192.168.110.144/live/0"));
    discovery.m_active = true;
    discovery.considerCandidate(QHostAddress("192.168.110.144"), "luckfox-test");
    discovery.cancel();
    QTest::qWait(180);
    QCOMPARE(found.size(), 2);
    QVERIFY(!LuckfoxNetworkRoute::isOnEthernetSubnet(QHostAddress("192.168.8.188"), {ethernet}));
    QVERIFY(!LuckfoxNetworkRoute::isOnEthernetSubnet(QHostAddress("192.168.110.144"), {ethernet}));
}

void TrackingConcurrencyTest::installedCameraTrackingDirections_data()
{
    QTest::addColumn<bool>("featureTracking");
    QTest::addColumn<QPoint>("displayOffset");
    for (bool feature : {false, true}) {
        const QByteArray backend = feature ? "csrt-" : "yolo-";
        QTest::newRow((backend + "down").constData()) << feature << QPoint(0, 80);
        QTest::newRow((backend + "up").constData()) << feature << QPoint(0, -80);
        QTest::newRow((backend + "right").constData()) << feature << QPoint(80, 0);
        QTest::newRow((backend + "left").constData()) << feature << QPoint(-80, 0);
        QTest::newRow((backend + "center").constData()) << feature << QPoint(0, 0);
    }
}

void TrackingConcurrencyTest::installedCameraTrackingDirections()
{
    QFETCH(bool, featureTracking);
    QFETCH(QPoint, displayOffset);
    MainWindow window(nullptr, false);
    QCOMPARE(window.m_cameraManager.rotationMode(), CameraManager::RotationMode::MirrorV);
    auto *model = window.findChild<QComboBox *>(QStringLiteral("comboBox_4"));
    QVERIFY(model);
    model->setCurrentIndex(model->findData(featureTracking ? QStringLiteral("feature")
                                                          : QStringLiteral("yolo26s.onnx")));
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
    QList<QJsonObject> tracks;
    int statusSequence = 0;
    connect(&server, &QUdpSocket::readyRead, &window, [&] {
        while (server.hasPendingDatagrams()) {
            const auto packet = server.receiveDatagram();
            const auto request = QJsonDocument::fromJson(packet.data()).object();
            auto reply = [&](QJsonObject body) {
                body.insert("version", 1); body.insert("session", request.value("session"));
                server.writeDatagram(QJsonDocument(body).toJson(QJsonDocument::Compact),
                                     packet.senderAddress(), packet.senderPort());
            };
            if (request.value("type").toString() == QStringLiteral("hello")) {
                reply({{"type", "welcome"}});
            } else if (request.value("type").toString() == QStringLiteral("track")) {
                tracks.append(request);
            }
            if (request.value("type").toString() == QStringLiteral("hello") ||
                request.value("type").toString() == QStringLiteral("heartbeat")) {
                reply({{"type", "status"}, {"seq", ++statusSequence}, {"online", true}, {"mode", 1},
                       {"horizontal", 40}, {"vertical", 200}});
            }
        }
    });
    TrackingEngine engine;
    engine.setFrameSize(QSize(640, 360));
    engine.m_useFeatureTracking = featureTracking;
    const cv::Rect2d target(280 + displayOffset.x(), 140 + displayOffset.y(), 80, 80);
    // These are coordinates after the installation flip, exactly as consumed by both backends.
    cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(24, 24, 24));
    if (featureTracking) {
        cv::RNG rng(7124);
        cv::Mat texture(80, 80, CV_8UC3);
        rng.fill(texture, cv::RNG::UNIFORM, 0, 255);
        texture.copyTo(frame(cv::Rect(target)));
        QVERIFY(engine.initFeatureTracker(frame, target));
    }
    // Prepare CSRT before starting the mock link's telemetry lease. Keep this input fixture
    // separate from the window's engine, which intentionally resets during connection setup.
    window.m_networkController.setAuthKey({});
    window.m_networkController.connectToDevice(QStringLiteral("127.0.0.1"), server.localPort());
    QTRY_VERIFY(window.m_networkController.isOpen());
    QCOMPARE(window.m_currentMode, 1);
    window.m_isCapturing = true;
    engine.m_isCapturing = true;
    engine.m_trackingGeneration.store(window.m_trackingEngine.trackingGeneration());
    connect(&engine, &TrackingEngine::targetTracked,
            &window.m_trackingEngine, &TrackingEngine::targetTracked);
    if (!featureTracking) {
        engine.m_dnnSessionId = 42;
        engine.onDnnTrackedResult(target, true, QStringLiteral("LOCK:cell phone"), 42);
    }
    engine.processFrame(frame); // Real offset calculation, target signal and UDP serialization.
    QTRY_COMPARE(tracks.size(), 1);
    QVERIFY(engine.m_isTargetTracked);
    const QPoint measured(engine.currentOffsetX(), engine.currentOffsetY());
    QCOMPARE(measured, displayOffset);
    qInfo().noquote() << "TRACK_DIRECTION_CASE" << QTest::currentDataTag()
                      << QJsonDocument(tracks.last()).toJson(QJsonDocument::Compact);
    QCOMPARE(QPoint(tracks.last().value("x").toInt(), tracks.last().value("y").toInt()), displayOffset);
    window.stopTrackingSafely();
    window.m_networkController.disconnectDevice();
}

void TrackingConcurrencyTest::installationCorrectionPreservesControlOffsets()
{
    MainWindow window(nullptr, false);
    window.m_offsetX = 50;
    window.m_offsetY = -30;
    QCOMPARE(window.m_cameraManager.rotationMode(), CameraManager::RotationMode::MirrorV);
    QCOMPARE(window.controlOffsets(), QPoint(50, -30));
    QMenu *directions = nullptr;
    for (auto *menu : window.findChildren<QMenu *>()) {
        if (menu->actions().size() == 6 && menu->actions()[3]->text() == QStringLiteral("垂直翻转")) {
            directions = menu;
            break;
        }
    }
    QVERIFY(directions);
    QVERIFY(directions->actions()[3]->isChecked());
    directions->actions()[0]->trigger();
    QCOMPARE(window.m_cameraManager.rotationMode(), CameraManager::RotationMode::Normal);
    QVERIFY(directions->actions()[0]->isChecked());
    QVERIFY(!directions->actions()[3]->isChecked());
    directions->actions()[3]->trigger();
    QCOMPARE(window.m_cameraManager.rotationMode(), CameraManager::RotationMode::MirrorV);
    QCOMPARE(window.controlOffsets(), QPoint(0, 0)); // Direction changes discard old tracking offsets.
    window.m_offsetX = 50;
    window.m_offsetY = -30;
    QCOMPARE(window.controlOffsets(), QPoint(50, -30));
    window.m_cameraManager.setRotationMode(CameraManager::RotationMode::Normal);
    QCOMPARE(window.controlOffsets(), QPoint(50, -30));
    window.m_cameraManager.setRotationMode(CameraManager::RotationMode::MirrorH);
    QCOMPARE(window.controlOffsets(), QPoint(-50, -30));
    window.m_cameraManager.setRotationMode(CameraManager::RotationMode::MirrorV);
    QCOMPARE(window.controlOffsets(), QPoint(50, -30));
    window.m_cameraManager.setRotationMode(CameraManager::RotationMode::Rotate180);
    QCOMPARE(window.controlOffsets(), QPoint(50, -30));
    window.m_cameraManager.setRotationMode(CameraManager::RotationMode::Normal);
}

void TrackingConcurrencyTest::displayedClockDoesNotModifyTrackingFrames()
{
    MainWindow window(nullptr, false);
    window.show();
    QCoreApplication::processEvents();
    auto *imageLabel = window.findChild<QLabel *>(QStringLiteral("imageLabel"));
    QVERIFY(imageLabel);
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Open;
    window.m_cameraManager.m_currentCameraId = QStringLiteral("rtsp://127.0.0.1/live/0");
    const cv::Mat frame = cv::Mat::zeros(720, 1280, CV_8UC3);
    for (const auto mode : {CameraManager::RotationMode::Normal, CameraManager::RotationMode::MirrorV,
                            CameraManager::RotationMode::Rotate180}) {
        window.m_cameraManager.setRotationMode(mode);
        window.handleNewMatFrame(frame);
        QCOMPARE(cv::norm(window.m_lastFrame, frame, cv::NORM_INF), 0.0);
        const QSize videoSize = QSize(1280, 720).scaled(imageLabel->size(), Qt::KeepAspectRatio);
        const int left = (imageLabel->width() - videoSize.width()) / 2;
        const int top = (imageLabel->height() - videoSize.height()) / 2;
        const QImage canvas = imageLabel->pixmap().toImage();
        int yellowPixels = 0;
        for (int y = top; y < top + 40; ++y) {
            for (int x = left; x < left + qMin(videoSize.width(), 300); ++x) {
                const QColor color = canvas.pixelColor(x, y);
                if (color.red() > 180 && color.green() > 180 && color.blue() < 100) ++yellowPixels;
            }
        }
        QVERIFY2(yellowPixels > 20, "Clock should be drawn in the displayed video top-left corner");
    }
    window.m_cameraManager.m_cameraState = CameraManager::CameraState::Idle;
}

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
    QSignalSpy onlineChanges(&window.m_networkController, &NetworkController::deviceOnlineChanged);
    QTest::mouseClick(open, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!window.m_lastFrame.empty(), 20000);
    QCOMPARE(window.m_lastFrame.cols, 1280);
    QCOMPARE(window.m_lastFrame.rows, 720);
    qInfo() << "LIVE clicked_open_native_720p_frame_received";
    QTest::qWait(30000);
    int offlineTransitions = 0;
    for (const auto &change : onlineChanges) if (!change[0].toBool()) ++offlineTransitions;
    qInfo() << "LIVE video_and_control_30s offline_transitions=" << offlineTransitions;
    QVERIFY2(offlineTransitions == 0, "Control telemetry went offline while video was playing");
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
        else emit thread->dnnTrackedResult(cv::Rect2d(), false, QString(), 0);
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
    // 模型在 run() 中异步加载；构造函数不再阻塞调用线程。
    QVERIFY(thread.isModelLoading());
    std::atomic<bool> emitted{false};
    std::atomic<bool> unlocked{false};
    connect(&thread, &DnnThread::dnnTrackedResult, &thread,
        [&](const cv::Rect2d &, bool, const QString &) {
            unlocked = thread.m_mutex.tryLock();
            if (unlocked) thread.m_mutex.unlock();
            emitted = true;
        }, Qt::DirectConnection);
    thread.start();
    QTRY_VERIFY_WITH_TIMEOUT(!thread.isModelLoading(), 15000);
    QVERIFY(!thread.isNetEmpty());
    QVERIFY(thread.initDnn(cv::Mat::zeros(360, 640, CV_8UC3), cv::Rect2d(100, 100, 80, 80)) != 0);
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
