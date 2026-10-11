#include "videoplayout.h"
#include "rtspstreamhealth.h"
#include <QtTest>

class VideoPlayoutTest : public QObject {
    Q_OBJECT
private slots:
    void burstsWithinBudgetProduceDistinctThirtyHzFrames();
    void longOutageIsReportedWithoutRepeatingOrCatchingUp();
    void duplicateOverflowAndResetAreBounded();
    void staleFramesCannotResetRecoveryBudget();
    void briefJitterRecoversWithoutReconnect();
};
static RtspDecodedFrame packet(int index) {
    RtspDecodedFrame f;
    f.ptsUs = (index * 1000000LL + 15) / 30;
    f.sequence = index + 1;
    f.image = cv::Mat(2, 2, CV_8UC3, cv::Scalar(index % 255, 0, 0));
    return f;
}
void VideoPlayoutTest::burstsWithinBudgetProduceDistinctThirtyHzFrames() {
    VideoPlayout p; p.reset(200);
    int sent = 0, displayed = 0;
    quint64 previous = 0;
    for (qint64 now = 0; now < 5200000; now += 1000) {
        while (sent < 150 && now >= ((sent / 5) * 5 + 4) * 1000000LL / 30) {
            p.push(packet(sent), now); ++sent;
        }
        RtspDecodedFrame f;
        if (p.take(now, f)) {
            QCOMPARE(f.sequence, previous + 1);
            previous = f.sequence; ++displayed;
        }
    }
    QCOMPARE(displayed, 150);
    QCOMPARE(p.stats().delivered, quint64(150));
    QCOMPARE(p.stats().late, quint64(0));
    QCOMPARE(p.stats().overflow, quint64(0));
}
void VideoPlayoutTest::longOutageIsReportedWithoutRepeatingOrCatchingUp() {
    VideoPlayout p; p.reset(150);
    quint64 previous = 0;
    qint64 lastAt = -1;
    for (qint64 now = 0; now < 2000000; now += 1000) {
        if (now % 100000 == 0 && !(now >= 500000 && now < 1100000)) {
            const int n = static_cast<int>(now * 30 / 1000000);
            for (int i = n; i < n + 3; ++i) p.push(packet(i), now + 67000);
        }
        RtspDecodedFrame f;
        if (p.take(now, f)) {
            QVERIFY(f.sequence > previous);
            if (lastAt >= 0) QVERIFY(now - lastAt >= 32000);
            previous = f.sequence; lastAt = now;
        }
    }
    QVERIFY(p.stats().missing > 0);
    QVERIFY(p.stats().delivered < 60);
}
void VideoPlayoutTest::duplicateOverflowAndResetAreBounded() {
    VideoPlayout p; p.reset(300);
    p.push(packet(0), 0); p.push(packet(0), 0);
    for (int i = 1; i < 80; ++i) p.push(packet(i), i * 33334);
    QCOMPARE(p.stats().queued, size_t(24));
    QVERIFY(p.stats().overflow > 0);
    QCOMPARE(p.stats().duplicate, quint64(1));
    p.reset(150);
    QCOMPARE(p.stats().queued, size_t(0));
    QCOMPARE(p.stats().missing, quint64(0));
    RtspDecodedFrame f;
    QVERIFY(!p.take(5000000, f));
}
void VideoPlayoutTest::staleFramesCannotResetRecoveryBudget() {
    RtspStreamHealth health;
    for (int i = 0; i <= 90; ++i) {
        const auto result = health.observe(i * 33333LL, i * 33333LL, 2000);
        QVERIFY(!result.resetBudget);
        QCOMPARE(result.recover, i * 33333LL >= 500000);
    }
    // A source timestamp gap also invalidates the healthy interval.
    RtspStreamHealth discontinuous;
    for (int i = 0; i <= 100; ++i)
        QVERIFY(!discontinuous.observe(i * 33333LL, i * 66667LL, 0).resetBudget);
}
void VideoPlayoutTest::briefJitterRecoversWithoutReconnect() {
    RtspStreamHealth health;
    int resets = 0;
    for (int i = 0; i <= 160; ++i) {
        const bool jitter = i >= 55 && i < 62;
        const auto result = health.observe(i * 33333LL, i * 33333LL, jitter ? 350 : 0);
        QVERIFY(!result.recover);
        if (result.resetBudget) { QVERIFY(i >= 123); ++resets; }
    }
    QCOMPARE(resets, 1);
}
QTEST_GUILESS_MAIN(VideoPlayoutTest)
#include "videoplayout_test.moc"
