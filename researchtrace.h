#ifndef RESEARCHTRACE_H
#define RESEARCHTRACE_H

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Opt-in metadata only. No frames, endpoints, keys or network payloads.
// Host timestamps are NOT acquisition timestamps; control never reads this log.
class ResearchTrace final
{
public:
    static qint64 nowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static bool enabled()
    {
        static const bool on = !qEnvironmentVariableIsEmpty("QT_RESEARCH_LOG_DIR");
        return on;
    }
    static ResearchTrace &instance()
    {
        static ResearchTrace trace(qEnvironmentVariable("QT_RESEARCH_LOG_DIR"));
        return trace;
    }
    static void record(const QString &kind, QJsonObject fields = {})
    {
        if (enabled()) instance().append(kind, std::move(fields));
    }

    explicit ResearchTrace(const QString &directory, size_t capacity = 4096)
        : m_capacity(capacity), m_runId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        if (directory.isEmpty()) return;
        if (!QDir(directory).exists()) {
            qWarning("[ResearchTrace] output directory missing; logging disabled");
            return;
        }
        m_path = QDir(directory).filePath(QStringLiteral("research-%1-%2.jsonl")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ"), m_runId));
        QJsonObject manifest{{"schema_version", 1}, {"kind", "run_start"},
            {"run_id", m_runId}, {"event_seq", 0}, {"host_mono_us", nowUs()},
            {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"clock", "host_steady_clock_us"}, {"timezone", "UTC"},
            {"capture_time_available", false}, {"encoder_sample_time_available", false},
            {"evidence_scope", "host_metadata_not_synchronized_measurement"},
            {"qt_version", QString::fromLatin1(qVersion())}};
        // Disk IO runs off producer threads. Bounded queue overflow is explicit.
        m_thread = std::thread([this, manifest]() { writeLoop(manifest); });
    }
    ~ResearchTrace()
    {
        { std::lock_guard<std::mutex> lock(m_mutex); m_stopping = true; }
        m_ready.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }
    ResearchTrace(const ResearchTrace &) = delete;
    ResearchTrace &operator=(const ResearchTrace &) = delete;
    QString path() const { return m_path; }
    quint64 dropped() const { return m_dropped.load(); }
    bool failed() const { return m_failed.load(); }
    void append(const QString &kind, QJsonObject fields = {})
    {
        if (m_path.isEmpty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || m_failed.load()) return;
        const quint64 seq = ++m_sequence;
        if (m_queue.size() >= m_capacity) { ++m_dropped; return; }
        fields.insert("schema_version", 1);
        fields.insert("kind", kind);
        fields.insert("run_id", m_runId);
        fields.insert("event_seq", qint64(seq));
        fields.insert("host_mono_us", nowUs());
        m_queue.push_back(std::move(fields));
        m_ready.notify_one();
    }
private:
    void writeLoop(QJsonObject manifest)
    {
        QFile file(m_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            m_failed = true;
            qWarning("[ResearchTrace] cannot create trace; no experiment evidence recorded");
            return;
        }
        QFile binary(QCoreApplication::applicationFilePath());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (binary.open(QIODevice::ReadOnly) && hash.addData(&binary))
            manifest.insert("executable_sha256", QString::fromLatin1(hash.result().toHex()));
        else manifest.insert("executable_sha256", QJsonValue::Null);
        auto write = [&](const QJsonObject &event) {
            const auto bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + char(10);
            if (file.write(bytes) != bytes.size()) {
                m_failed = true;
                qWarning("[ResearchTrace] write failed; trace incomplete");
                return false;
            }
            return true;
        };
        if (!write(manifest)) return;
        for (;;) {
            std::deque<QJsonObject> batch;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
                batch.swap(m_queue);
                if (batch.empty() && m_stopping) break;
            }
            for (const auto &event : batch) if (!write(event)) return;
            if (!file.flush()) { m_failed = true; return; }
        }
        write({{"schema_version", 1}, {"kind", "run_end"}, {"run_id", m_runId},
            {"event_seq", qint64(m_sequence + 1)}, {"host_mono_us", nowUs()},
            {"dropped_events", qint64(m_dropped.load())}, {"complete", m_dropped.load() == 0}});
        if (!file.flush()) m_failed = true;
        if (m_failed.load()) qWarning("[ResearchTrace] flush failed; trace incomplete");
    }
    size_t m_capacity;
    QString m_runId, m_path;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<QJsonObject> m_queue;
    std::thread m_thread;
    bool m_stopping = false;
    quint64 m_sequence = 0;
    std::atomic<quint64> m_dropped{0};
    std::atomic<bool> m_failed{false};
};

#endif
