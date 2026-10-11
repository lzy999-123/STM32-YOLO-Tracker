#ifndef VIDEOPLAYOUT_H
#define VIDEOPLAYOUT_H

#include "rtspdecoder.h"
#include <deque>
#include <cmath>

// Called under CameraManager's frame mutex. No repeated frames and no catch-up
// bursts: every source timestamp has one 30 Hz deadline on a monotonic clock.
class VideoPlayout {
public:
    struct Stats {
        quint64 delivered = 0, missing = 0, late = 0, duplicate = 0, overflow = 0;
        size_t queued = 0;
    };
    void reset(int delayMs = 150) {
        queue.clear(); delayUs = qBound(0, delayMs, 300) * 1000LL;
        firstPts = lastPts = startUs = nextIndex = 0;
        minimumOffset = 0;
        anchored = false; metrics = {};
    }
    void push(RtspDecodedFrame frame, qint64 nowUs) {
        if (!anchored) {
            anchored = true; firstPts = frame.ptsUs; lastPts = frame.ptsUs - 1;
            minimumOffset = nowUs - frame.ptsUs;
            startUs = nowUs + delayUs;
        }
        const qint64 offset = nowUs - frame.ptsUs;
        if (offset < minimumOffset) {
            minimumOffset = offset;
            startUs = minimumOffset + firstPts + delayUs;
        }
        if (frame.ptsUs <= lastPts) { ++metrics.duplicate; return; }
        lastPts = frame.ptsUs;
        const qint64 index = sourceIndex(frame.ptsUs);
        if (index < nextIndex) { ++metrics.late; return; }
        if (queue.size() >= 24) { ++metrics.overflow; return; }
        queue.push_back(std::move(frame));
    }
    bool take(qint64 nowUs, RtspDecodedFrame &out) {
        if (!anchored || nowUs < deadline()) return false;
        // A stalled GUI must resume at the current deadline, never 45+ frames/s.
        const qint64 due = (nowUs - startUs) * 30 / 1000000;
        if (due > nextIndex) {
            metrics.missing += due - nextIndex;
            nextIndex = due;
        }
        while (!queue.empty() && sourceIndex(queue.front().ptsUs) < nextIndex) {
            queue.pop_front(); ++metrics.late;
        }
        bool available = !queue.empty() && sourceIndex(queue.front().ptsUs) == nextIndex;
        if (available) {
            out = std::move(queue.front()); queue.pop_front(); ++metrics.delivered;
        } else ++metrics.missing;
        ++nextIndex;
        return available;
    }
    qint64 waitUs(qint64 nowUs) const {
        return anchored ? qMax<qint64>(1, deadline() - nowUs) : 10000;
    }
    Stats stats() const { Stats copy = metrics; copy.queued = queue.size(); return copy; }
private:
    qint64 sourceIndex(qint64 ptsUs) const {
        return std::llround(static_cast<double>(ptsUs - firstPts) * 30 / 1000000.0);
    }
    qint64 deadline() const { return startUs + (nextIndex * 1000000 + 29) / 30; }
    std::deque<RtspDecodedFrame> queue;
    qint64 delayUs = 150000, firstPts = 0, lastPts = 0, startUs = 0, nextIndex = 0, minimumOffset = 0;
    bool anchored = false;
    Stats metrics;
};
#endif
