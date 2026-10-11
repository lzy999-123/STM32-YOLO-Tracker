#ifndef RTSPDECODER_H
#define RTSPDECODER_H

#include <QString>
#include <functional>
#include <memory>
#include <opencv2/core.hpp>

// FFmpeg metadata stays attached to the decoded frame, before GUI coalescing.
struct RtspDecodedFrame {
    cv::Mat image;
    qint64 ptsUs = 0;
    quint64 sequence = 0;
    bool keyFrame = false;
};

class RtspDecoder {
public:
    enum class ReadResult { Frame, Failed, Cancelled };
    explicit RtspDecoder(std::function<bool()> cancelled);
    ~RtspDecoder();
    bool open(const QString &url, bool tcp, QString &error);
    ReadResult read(RtspDecodedFrame &frame, QString &error);
    double sourceFps() const;
    quint64 damagedFrames() const;
    quint64 discardedFrames() const;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
#endif
