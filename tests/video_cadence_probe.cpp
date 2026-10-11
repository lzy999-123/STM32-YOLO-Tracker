#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QtTest>
#include <iostream>
#include <chrono>

// Use the production window and production sources. Save timing metadata only.
// A paint event confirms Qt painted the widget, not the monitor's scanout time.
class TrackingConcurrencyTest : public QObject {
    QElapsedTimer clock;
    QJsonArray submitted, painted, states, errors;
    quint64 session = 0, sequence = 0;
    qint64 pts = -1, originNs = 0;
    bool measuring = false;
    double nowMs() const { return (clock.nsecsElapsed() - originNs) / 1000000.0; }
    QJsonArray sample() const {
        return {nowMs(), static_cast<qint64>(session), static_cast<qint64>(sequence), pts};
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (measuring && event->type() == QEvent::Paint && sequence) painted.append(sample());
        return QObject::eventFilter(object, event);
    }
public:
    int run(const QString &url, int seconds, const QString &output) {
        MainWindow window(nullptr, false);
        auto *label = window.findChild<QLabel *>(QStringLiteral("imageLabel"));
        label->installEventFilter(this);
        clock.start();
        int frameWidth = 0, frameHeight = 0;
        connect(&window.m_cameraManager, &CameraManager::matReady, this,
                [&](const cv::Mat &image) {
            frameWidth = image.cols; frameHeight = image.rows;
            session = window.m_cameraManager.m_rtspSessionId.load();
            ++sequence;
            // The restored OpenCV path does not expose source PTS. Keep it unknown.
            pts = -1;
            if (measuring) submitted.append(sample());
        });
        connect(&window.m_cameraManager, &CameraManager::stateChanged, this,
                [&](CameraManager::CameraState state) {
            states.append(QJsonArray{nowMs(), static_cast<int>(state)});
            if (state != CameraManager::CameraState::Open) sequence = 0;
        });
        connect(&window.m_cameraManager, &CameraManager::cameraError, this,
                [&](const QString &error) { errors.append(QJsonArray{nowMs(), error}); });
        window.setWindowTitle(QStringLiteral("ecd73bf 视频绘制节奏检查（视频与控制）"));
        window.resize(1280, 800);
        window.show();
        QTest::qWait(1000);
        window.m_cameraManager.openCamera(url);
        QElapsedTimer opening; opening.start();
        while (window.m_lastFrame.empty() && opening.elapsed() < 15000) QTest::qWait(10);
        const bool started = !window.m_lastFrame.empty();
        if (started) {
            QTest::qWait(2000);
            originNs = clock.nsecsElapsed(); measuring = true;
            // Pre-warmup events use the original origin. Normalize them so the
            // analyzer can distinguish startup from a reconnect during measurement.
            const double shiftMs = originNs / 1000000.0;
            for (int i = 0; i < states.size(); ++i) {
                auto event = states[i].toArray(); event[0] = event[0].toDouble() - shiftMs; states[i] = event;
            }
            for (int i = 0; i < errors.size(); ++i) {
                auto event = errors[i].toArray(); event[0] = event[0].toDouble() - shiftMs; errors[i] = event;
            }
            QElapsedTimer duration; duration.start();
            qint64 nextReport = 10000;
            while (duration.elapsed() < seconds * 1000LL) {
                QTest::qWait(5);
                if (duration.elapsed() >= nextReport) {
                    std::cout << "elapsed_s=" << duration.elapsed()/1000
                              << " submitted=" << submitted.size() << " paints=" << painted.size()
                              << " camera_state=" << static_cast<int>(window.m_cameraManager.state())
                              << " control_online=" << window.m_networkController.isOpen() << std::endl;
                    nextReport += 10000;
                }
            }
        }
        measuring = false;
        QJsonObject summary{{"startup_failed", !started}, {"seconds", nowMs()/1000},
            {"delay_ms", 0}, {"requested_transport", "tcp_first"},
            {"source_pts_available", false}, {"pipeline", "ecd73bf_opencv_latest_frame"},
            {"frame_width", frameWidth}, {"frame_height", frameHeight},
            {"control_online", window.m_networkController.isOpen()},
            {"camera_states", states}, {"camera_errors", errors},
            {"tracking_enabled", window.m_isCapturing}};
        QFile file(output);
        if (!file.open(QIODevice::WriteOnly)) return 3;
        file.write(QJsonDocument(QJsonObject{{"summary", summary}, {"submitted", submitted},
            {"painted", painted}}).toJson());
        window.m_cameraManager.closeCamera();
        return started ? 0 : 2;
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"url", "RTSP URL", "url"});
    parser.addOption({"seconds", "Duration after warmup", "seconds", "90"});
    parser.addOption({"output", "Timing JSON (no images)", "path"});
    parser.process(app);
    bool durationOk;
    int seconds = parser.value("seconds").toInt(&durationOk);
    if (!durationOk || seconds < 1 ||
        !parser.value("url").startsWith("rtsp://") || parser.value("output").isEmpty()) return 1;
    TrackingConcurrencyTest probe;
    return probe.run(parser.value("url"), seconds, parser.value("output"));
}
