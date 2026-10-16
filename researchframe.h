#ifndef RESEARCHFRAME_H
#define RESEARCHFRAME_H
#include "researchtrace.h"
struct ResearchFrame
{
    quint64 id = 0, stream = 0;
    qint64 hostReceiveUs = -1, mediaPtsUs = -1;
    int rotation = -1;
    QJsonObject json() const
    {
        return {{"frame_id", qint64(id)}, {"stream_id", qint64(stream)},
            {"host_receive_us", hostReceiveUs >= 0 ? QJsonValue(hostReceiveUs) : QJsonValue(QJsonValue::Null)},
            {"media_pts_us", mediaPtsUs >= 0 ? QJsonValue(mediaPtsUs) : QJsonValue(QJsonValue::Null)},
            {"capture_mono_us", QJsonValue::Null}, {"clock_mapping_id", QJsonValue::Null},
            {"rotation", rotation}};
    }
};
#endif
