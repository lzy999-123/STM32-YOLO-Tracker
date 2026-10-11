#include "rtspdecoder.h"
#include <chrono>
#include <algorithm>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

namespace {
qint64 steadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
QString ffError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}
}

struct RtspDecoder::Impl {
    std::function<bool()> cancelled;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *convert = nullptr;
    int video = -1;
    qint64 deadline = 0;
    quint64 sequence = 0, damaged = 0, discarded = 0;
    bool needKeyFrame = false;
    qint64 lastPts = AV_NOPTS_VALUE;
    double fps = 0;
    bool closing = false;
    static int interrupt(void *opaque) {
        const auto *self = static_cast<Impl *>(opaque);
        return (!self->closing && self->cancelled()) || (self->deadline && steadyUs() >= self->deadline);
    }
    ~Impl() {
        sws_freeContext(convert);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        // A cancelled/expired read must not pre-cancel RTSP TEARDOWN. Give
        // cleanup a separate bounded opportunity to release server resources.
        closing = true;
        deadline = steadyUs() + 200000;
        avformat_close_input(&format);
    }
};

RtspDecoder::RtspDecoder(std::function<bool()> cancelled) : d(new Impl) {
    d->cancelled = std::move(cancelled);
}
RtspDecoder::~RtspDecoder() = default;

bool RtspDecoder::open(const QString &url, bool tcp, QString &error) {
    if ((avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
        (avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
        (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
        (swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR) {
        error = QStringLiteral("FFmpeg 运行库与编译头文件版本不匹配");
        return false;
    }
    d->format = avformat_alloc_context();
    if (!d->format) { error = QStringLiteral("无法分配解复用器"); return false; }
    d->deadline = steadyUs() + 2500000;
    d->format->interrupt_callback = {&Impl::interrupt, d.get()};
    AVDictionary *options = nullptr;
    av_dict_set(&options, "rtsp_transport", tcp ? "tcp" : "udp", 0);
    av_dict_set(&options, "allowed_media_types", "video", 0);
    av_dict_set(&options, "buffer_size", "1048576", 0);
    av_dict_set(&options, "max_delay", tcp ? "0" : "150000", 0);
    av_dict_set(&options, "reorder_queue_size", tcp ? "0" : "512", 0);
    av_dict_set(&options, "probesize", "32768", 0);
    av_dict_set(&options, "analyzeduration", "100000", 0);
    av_dict_set(&options, "fpsprobesize", "0", 0);
    const QByteArray address = url.toUtf8();
    int ret = avformat_open_input(&d->format, address.constData(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) { error = ffError(ret); return false; }
    ret = avformat_find_stream_info(d->format, nullptr);
    if (ret < 0) { error = ffError(ret); return false; }
    const AVCodec *decoder = nullptr;
    d->video = av_find_best_stream(d->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (d->video < 0) { error = ffError(d->video); return false; }
    const auto *stream = d->format->streams[d->video];
    d->fps = av_q2d(av_guess_frame_rate(d->format, const_cast<AVStream *>(stream), nullptr));
    d->codec = avcodec_alloc_context3(decoder);
    if (!d->codec) { error = QStringLiteral("无法分配解码器"); return false; }
    ret = avcodec_parameters_to_context(d->codec, stream->codecpar);
    if (ret < 0) { error = ffError(ret); return false; }
    // One thread avoids frame-thread lookahead; 720p decode is already fast enough.
    d->codec->thread_count = 1;
    d->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->codec->pkt_timebase = stream->time_base;
    ret = avcodec_open2(d->codec, decoder, nullptr);
    if (ret < 0) { error = ffError(ret); return false; }
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->packet || !d->frame) { error = QStringLiteral("无法分配解码缓冲"); return false; }
    d->deadline = 0;
    return true;
}

RtspDecoder::ReadResult RtspDecoder::read(RtspDecodedFrame &out, QString &error) {
    d->deadline = steadyUs() + 1500000;
    while (!d->cancelled()) {
        if (steadyUs() >= d->deadline) { error = QStringLiteral("1.5 秒内没有完整新帧"); return ReadResult::Failed; }
        int ret = avcodec_receive_frame(d->codec, d->frame);
        if (ret >= 0) {
            const bool key = (d->frame->flags & AV_FRAME_FLAG_KEY) != 0;
            if ((d->frame->flags & AV_FRAME_FLAG_CORRUPT) || d->frame->decode_error_flags) {
                ++d->damaged;
                d->needKeyFrame = true;
                av_frame_unref(d->frame);
                continue;
            }
            if (d->needKeyFrame && !key) {
                ++d->discarded;
                av_frame_unref(d->frame);
                continue;
            }
            d->needKeyFrame = false;
            qint64 pts = d->frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                error = QStringLiteral("视频帧缺少源时间戳，无法验证恒定帧率");
                return ReadResult::Failed;
            }
            pts = av_rescale_q(pts, d->format->streams[d->video]->time_base, AVRational{1, 1000000});
            if (d->lastPts != AV_NOPTS_VALUE && pts <= d->lastPts) {
                ++d->discarded;
                av_frame_unref(d->frame);
                continue;
            }
            if (d->frame->width != 1280 || d->frame->height != 720) {
                error = QStringLiteral("要求原生1280×720，实际为%1×%2").arg(d->frame->width).arg(d->frame->height);
                return ReadResult::Failed;
            }
            // YUVJ420P is YUV420P with full range; normalize the format and set
            // range explicitly so swscale can reuse its context for every frame.
            const auto pixelFormat = d->frame->format == AV_PIX_FMT_YUVJ420P
                ? AV_PIX_FMT_YUV420P : static_cast<AVPixelFormat>(d->frame->format);
            d->convert = sws_getCachedContext(d->convert, d->frame->width, d->frame->height,
                pixelFormat, 1280, 720, AV_PIX_FMT_BGR24,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!d->convert) { error = QStringLiteral("图像颜色转换失败"); return ReadResult::Failed; }
            const int colorSpace = d->frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
            const int *coefficients = sws_getCoefficients(colorSpace);
            const bool fullRange = d->frame->color_range == AVCOL_RANGE_JPEG || d->frame->format == AV_PIX_FMT_YUVJ420P;
            sws_setColorspaceDetails(d->convert, coefficients, fullRange,
                coefficients, 1, 0, 1 << 16, 1 << 16);
            // Every queued image owns its storage; subsequent decode cannot overwrite it.
            out.image = cv::Mat(720, 1280, CV_8UC3);
            uint8_t *planes[4]{out.image.data, nullptr, nullptr, nullptr};
            int strides[4]{static_cast<int>(out.image.step), 0, 0, 0};
            sws_scale(d->convert, d->frame->data, d->frame->linesize, 0, 720, planes, strides);
            out.ptsUs = pts;
            out.sequence = ++d->sequence;
            out.keyFrame = key;
            d->lastPts = pts;
            av_frame_unref(d->frame);
            d->deadline = 0;
            return ReadResult::Frame;
        }
        if (ret != AVERROR(EAGAIN)) {
            if (ret == AVERROR_EOF) {
                error = QStringLiteral("视频解码流已结束");
                return ReadResult::Failed;
            }
            ++d->damaged;
            d->needKeyFrame = true;
        }
        ret = av_read_frame(d->format, d->packet);
        if (ret < 0) {
            if (d->cancelled()) return ReadResult::Cancelled;
            if (ret == AVERROR(EAGAIN)) continue;
            error = ffError(ret);
            return ReadResult::Failed;
        }
        if (d->packet->stream_index == d->video) {
            if (d->packet->flags & AV_PKT_FLAG_CORRUPT) d->needKeyFrame = true;
            ret = avcodec_send_packet(d->codec, d->packet);
            if (ret == AVERROR(EAGAIN)) {
                // receive_frame returned EAGAIN immediately above; the API
                // forbids both ends being blocked. Never silently lose a packet.
                av_packet_unref(d->packet);
                error = QStringLiteral("解码器输入输出状态异常");
                return ReadResult::Failed;
            }
            if (ret < 0) { ++d->damaged; d->needKeyFrame = true; }
        }
        av_packet_unref(d->packet);
    }
    return ReadResult::Cancelled;
}
double RtspDecoder::sourceFps() const { return d->fps; }
quint64 RtspDecoder::damagedFrames() const { return d->damaged; }
quint64 RtspDecoder::discardedFrames() const { return d->discarded; }
