#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "serialcontroller.h"
#include "cameramanager.h"
#include "trackingengine.h"
#include <QMouseEvent>
#include <QPaintEvent>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QCamera>
#include <QMediaCaptureSession>
#include <QVideoWidget>
#include <QVideoSink>
#include <QMediaDevices>
#include <QCameraDevice>
#include <QMutex>
#include <QThread>
#include <QRect>
#include <QCloseEvent>
#include <QHash>

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/tracking.hpp>
#include <onnxruntime_cxx_api.h>

using namespace cv;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


// ==========================================
// 大脑：DNN 深度学习纠错线程
// ==========================================




// ==========================================
// 主窗口类
// ==========================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class TrackingBackend { Yolo, Feature };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    static cv::Mat QImageToCvMat(const QImage& qImage);
    cv::Mat QVideoFrameToCvMat(const QVideoFrame &frame);
    static QImage CvMatToQImage(const cv::Mat& mat);

private slots:
    void on_pushButton_clicked();
    void on_pushButton_8_clicked();
    void onCameraChanged(int index);
    void on_pushButton_9_clicked();
    void handleNewVideoFrame(const QVideoFrame &frame);
    void processLatestVideoFrame();
    void on_btnSelectTarget_clicked();
    void on_btnStartTracking_clicked();
    void on_btnStopTracking_clicked();
    void on_pushButton_2_clicked();
    void on_pushButton_3_clicked();
    void onTrackingModelChanged(int index);

protected:
    void closeEvent(QCloseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void testDNN();
    void sendCommand(uint8_t cmd);
    QString currentTrackingModelFileName() const;
    TrackingBackend currentTrackingBackend() const;
    bool isFeatureTrackingSelected() const;

    Ui::MainWindow *ui;
    SerialController m_serialController;
    CameraManager m_cameraManager;
    TrackingEngine m_trackingEngine;
    cv::Mat m_lastFrame;
    QMutex m_pendingFrameMutex;
    QVideoFrame m_pendingVideoFrame;
    bool m_frameDispatchPending = false;

    bool m_isCapturing;
    bool m_isSelecting;
    bool m_hasSelectedTarget;
    QPoint m_selectStart;
    QPoint m_selectEnd;
    QPoint m_selectStartImg;
    QPoint m_selectEndImg;
    cv::Rect2d m_selectedRect;
    cv::Rect2d m_trackedRect;

    QMutex m_modeMutex;
    int m_currentMode;

    QMutex m_frameSizeMutex;
    QSize m_frameSize;
    bool m_isTargetTracked;

    int16_t m_offsetX;
    int16_t m_offsetY;
    cv::Rect2d m_lastSelectedRect;
    bool m_wasTrackingBeforeDisconn;

    bool m_forceResetTracking;
    bool m_waitingForRecover;
    QPixmap m_renderCanvas;
    qint64 m_lastSerialSendTime;
    int m_dnnFrameSkipCounter = 0;
    int m_lostFrameCount = 0;
};

#endif // MAINWINDOW_H
