#ifndef TRACKINGDIAGNOSTICS_H
#define TRACKINGDIAGNOSTICS_H
#include <QtCore>
#include <chrono>

// Opt-in metadata only: no frames, credentials or network payloads.
inline bool trackingDiagnosticsEnabled()
{
    static const bool enabled = qEnvironmentVariableIntValue("QT_TRACK_CONTROL_TRACE") == 1;
    return enabled;
}
inline qint64 trackingClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif
