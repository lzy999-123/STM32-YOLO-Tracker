#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
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

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <vector>
#include <string>

using namespace cv;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


// ==========================================
// 大脑：DNN 深度学习纠错线程
// ==========================================
class DnnThread : public QThread
{
    Q_OBJECT
public:
    explicit DnnThread(QObject *parent = nullptr);
    ~DnnThread();

    void initDnn(const cv::Mat &frame, const cv::Rect2d &target);
    void updateDnn(const cv::Mat &frame);
    void stopDnn();
    bool isBusy(); // 判断大脑是否正在忙碌
    bool isNetEmpty() { return m_yoloNet.empty(); }

signals:
    void dnnTrackedResult(const cv::Rect2d &rect, bool success, const QString &className = "");

protected:
    void run() override;

private:
    QMutex m_mutex;
    cv::dnn::Net m_yoloNet;               // YOLO网络模型
    int m_lockedClassId;                  // 锁定的目标类别ID
    bool m_useYolo;                       // 是否使用YOLO跟踪
    std::vector<std::string> m_classNames;// COCO类别名称列表
    cv::Rect2d m_lastYoloRect;            // 上一帧的YOLO目标位置

    cv::Mat m_frame;
    cv::Rect2d m_initRect;
    bool m_isTracking;
    bool m_needInit;
    bool m_hasNewFrame;
    bool m_isComputing;
    
    double m_relX;
    double m_relY;
    double m_relW;
    double m_relH;
};



// ==========================================
// 主窗口类
// ==========================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class CameraState { Idle, Opening, Open, Closing, Error };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    static cv::Mat QImageToCvMat(const QImage& qImage);
    cv::Mat QVideoFrameToCvMat(const QVideoFrame &frame);
    static QImage CvMatToQImage(const cv::Mat& mat);

private slots:
    void emergencyCameraStop();
    void on_pushButton_clicked();
    void on_pushButton_8_clicked();
    void messlot();
    void onCameraChanged(int index);
    void on_pushButton_9_clicked();
    void handleNewVideoFrame(const QVideoFrame &frame);
    void on_btnSelectTarget_clicked();
    void on_btnStartTracking_clicked();
    void on_btnStopTracking_clicked();
    void on_pushButton_2_clicked();
    void on_pushButton_3_clicked();
    void onDnnResultReceived(const cv::Rect2d &dnnRect, bool success, const QString &className = "");

protected:
    void closeEvent(QCloseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void testDNN();
    void sendCommand(uint8_t cmd);

    Ui::MainWindow *ui;
    QSerialPort m_serial;
    QByteArray m_rx_buffer;

    QCamera *m_camera;
    QMediaCaptureSession *m_captureSession;
    QVideoWidget *m_videoWidget;
    QVideoSink *m_videoSink;
    cv::Mat m_lastFrame;

    bool m_isCapturing;
    bool m_isSelecting;
    bool m_hasSelectedTarget;
    QPoint m_selectStart;
    QPoint m_selectEnd;
    QPoint m_selectStartImg;
    QPoint m_selectEndImg;
    cv::Rect2d m_selectedRect;
    cv::Rect2d m_trackedRect;

    DnnThread *m_dnnThread;

    QMutex m_modeMutex;
    int m_currentMode;

    QMutex m_frameSizeMutex;
    QSize m_frameSize;
    bool m_isTargetTracked;

    int16_t m_offsetX;
    int16_t m_offsetY;

    QMutex m_cameraMutex;
    QTimer *m_cameraCheckTimer;
    cv::Rect2d m_lastSelectedRect;
    bool m_wasTrackingBeforeDisconn;
    qint64 m_lastFrameTime;
    QString m_currentCameraId;

    bool m_forceResetTracking;
    bool m_waitingForRecover;

    CameraState m_cameraState = CameraState::Idle;
    QPixmap m_renderCanvas;
    qint64 m_lastSerialSendTime;
    int m_dnnFrameSkipCounter = 0;
};

#endif // MAINWINDOW_H
