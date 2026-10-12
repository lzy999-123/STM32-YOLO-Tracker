#ifndef YOLOINPUTGEOMETRY_H
#define YOLOINPUTGEOMETRY_H

#include <opencv2/imgproc.hpp>
#include <algorithm>

// Static square ONNX exports use the same centered letterbox as Ultralytics.
// Keep the inverse transform beside preprocessing so offsets use source pixels.
struct YoloInputGeometry {
    cv::Size sourceSize;
    cv::Size resizedSize;
    int inputSize = 640;
    int left = 0;
    int top = 0;
    double scale = 1.0;

    explicit YoloInputGeometry(cv::Size source, int input = 640)
        : sourceSize(source), inputSize(input)
    {
        scale = std::min(static_cast<double>(input) / source.width,
                         static_cast<double>(input) / source.height);
        resizedSize = cv::Size(cvRound(source.width * scale), cvRound(source.height * scale));
        left = (input - resizedSize.width) / 2;
        top = (input - resizedSize.height) / 2;
    }

    cv::Mat prepare(const cv::Mat &source) const
    {
        cv::Mat resized, input;
        if (source.size() == resizedSize) resized = source;
        else cv::resize(source, resized, resizedSize, 0, 0, cv::INTER_LINEAR);
        cv::copyMakeBorder(resized, input, top, inputSize - resizedSize.height - top,
                           left, inputSize - resizedSize.width - left,
                           cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
        return input;
    }

    cv::Rect sourceRect(double x1, double y1, double x2, double y2) const
    {
        const double l = std::clamp((x1 - left) / scale, 0.0, double(sourceSize.width));
        const double t = std::clamp((y1 - top) / scale, 0.0, double(sourceSize.height));
        const double r = std::clamp((x2 - left) / scale, 0.0, double(sourceSize.width));
        const double b = std::clamp((y2 - top) / scale, 0.0, double(sourceSize.height));
        if (r <= l || b <= t) return {};
        return cv::Rect(cvRound(l), cvRound(t), cvRound(r) - cvRound(l), cvRound(b) - cvRound(t));
    }
};
#endif
