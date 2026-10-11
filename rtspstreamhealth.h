#ifndef RTSPSTREAMHEALTH_H
#define RTSPSTREAMHEALTH_H

#include <QtGlobal>

// Relative receiver backlog is not optical latency. Keep its original PTS
// baseline; never make an old stream look fresh by re-anchoring after a stall.
class RtspStreamHealth {
public:
    struct Result { bool recover = false; bool resetBudget = false; };
    Result observe(qint64 nowUs, qint64 ptsUs, qint64 lagMs) {
        Result result;
        if (lagMs > 300) {
            if (staleSinceUs < 0) staleSinceUs = nowUs;
            result.recover = nowUs - staleSinceUs >= 500000;
        } else staleSinceUs = -1;

        const bool continuous = previousUs >= 0 && nowUs >= previousUs &&
            nowUs - previousUs <= 150000 &&
            ptsUs - previousPtsUs >= 20000 && ptsUs - previousPtsUs <= 50000;
        if (lagMs > 100) {
            healthySinceUs = -1;
        } else if (healthySinceUs < 0 || !continuous) {
            healthySinceUs = nowUs;
        } else if (!budgetReset && nowUs - healthySinceUs >= 2000000) {
            result.resetBudget = budgetReset = true;
        }
        previousUs = nowUs;
        previousPtsUs = ptsUs;
        return result;
    }
private:
    qint64 staleSinceUs = -1, healthySinceUs = -1, previousUs = -1, previousPtsUs = 0;
    bool budgetReset = false;
};
#endif
