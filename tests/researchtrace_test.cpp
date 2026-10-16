#include "researchframe.h"
#include <QtTest>
#include <QTemporaryDir>
#include <vector>
class ResearchTraceTest : public QObject
{
    Q_OBJECT
private slots:
    void missingTimesStayNull()
    {
        const auto unknown = ResearchFrame{}.json();
        QVERIFY(unknown["capture_mono_us"].isNull());
        QVERIFY(unknown["host_receive_us"].isNull());
        QVERIFY(unknown["media_pts_us"].isNull());
        const auto known = ResearchFrame{12, 5, 100, 70, 3}.json();
        QCOMPARE(known["frame_id"].toInteger(), 12);
        QCOMPARE(known["host_receive_us"].toInteger(), 100);
        QVERIFY(known["capture_mono_us"].isNull());
    }
    void orderedConcurrentFlush()
    {
        QTemporaryDir directory;
        QString path;
        {
            ResearchTrace trace(directory.path(), 10000);
            path = trace.path();
            std::vector<std::thread> workers;
            for (int worker = 0; worker < 4; ++worker)
                workers.emplace_back([&trace,worker]() {
                    for (int i = 0; i < 150; ++i)
                        trace.append("test", {{"producer", worker}, {"index", i}});
                });
            for (auto &worker : workers) worker.join();
            QCOMPARE(trace.dropped(), quint64(0));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        qint64 previous = -1, previousTime = -1;
        int records = 0;
        QJsonObject last;
        while (!file.atEnd()) {
            const auto row = QJsonDocument::fromJson(file.readLine()).object();
            QCOMPARE(row["event_seq"].toInteger(), previous + 1);
            QVERIFY(row["host_mono_us"].toInteger() >= previousTime);
            previous = row["event_seq"].toInteger();
            previousTime = row["host_mono_us"].toInteger();
            last = row;
            ++records;
        }
        QCOMPARE(records, 602);
        QVERIFY(last["complete"].toBool());
    }
    void overflowIsExplicit()
    {
        QTemporaryDir directory;
        QString path;
        {
            ResearchTrace trace(directory.path(), 0);
            path = trace.path();
            trace.append("dropped");
            QCOMPARE(trace.dropped(), quint64(1));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto lines = file.readAll().trimmed().split(char(10));
        QCOMPARE(lines.size(), 2);
        const auto last = QJsonDocument::fromJson(lines.last()).object();
        QVERIFY(!last["complete"].toBool());
        QCOMPARE(last["dropped_events"].toInteger(), 1);
    }
    void unavailableDirectoryDoesNotCreateEvidence()
    {
        QTemporaryDir directory;
        ResearchTrace trace(directory.path() + "/missing");
        trace.append("test");
        QVERIFY(trace.path().isEmpty());
        QVERIFY(!QDir(directory.path() + "/missing").exists());
    }
    void existingFilesArePreserved()
    {
        QTemporaryDir directory;
        QString first, second;
        { ResearchTrace trace(directory.path()); first = trace.path(); trace.append("first"); }
        QFile file(first); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll(); file.close();
        { ResearchTrace trace(directory.path()); second = trace.path(); trace.append("second"); }
        QVERIFY(first != second);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
};
QTEST_GUILESS_MAIN(ResearchTraceTest)
#include "researchtrace_test.moc"
