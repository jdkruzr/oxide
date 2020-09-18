#ifndef APPLICATION_H
#define APPLICATION_H

#include <QObject>
#include <QDebug>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QProcess>
#include <zlib.h>

#include <systemd/sd-journal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <cstdlib>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "dbussettings.h"
#include "inputmanager.h"
#include "mxcfb.h"

#define DISPLAYWIDTH 1404
#define DISPLAYHEIGHT 1872
#define TEMP_USE_REMARKABLE_DRAW 0x0018
#define DISPLAYSIZE DISPLAYWIDTH * DISPLAYHEIGHT * sizeof(uint16_t)

class Application : public QObject{
    Q_OBJECT
    Q_CLASSINFO("Version", OXIDE_INTERFACE_VERSION)
    Q_CLASSINFO("D-Bus Interface", OXIDE_APPLICATION_INTERFACE)
    Q_PROPERTY(QString name READ name)
    Q_PROPERTY(QString description READ description)
    Q_PROPERTY(QString call READ call)
    Q_PROPERTY(QString term READ term)
    Q_PROPERTY(bool autoStart READ autoStart)
    Q_PROPERTY(int type READ type)
    Q_PROPERTY(int state READ state)
public:
    Application(QDBusObjectPath path, QObject* parent) : Application(path.path(), parent) {}
    Application(QString path, QObject* parent) : QObject(parent), m_path(path), m_backgrounded(false) {
        m_process = new QProcess(this);
        connect(m_process, &QProcess::started, this, &Application::started);
        connect(m_process, QOverload<int>::of(&QProcess::finished), this, &Application::finished);
        connect(m_process, &QProcess::readyReadStandardError, this, &Application::readyReadStandardError);
        connect(m_process, &QProcess::readyReadStandardOutput, this, &Application::readyReadStandardOutput);
        connect(m_process, &QProcess::stateChanged, this, &Application::stateChanged);
        connect(m_process, &QProcess::errorOccurred, this, &Application::errorOccurred);
    }
    ~Application() {
        unregisterPath();
        m_process->kill();
        delete m_process;
        if(screenCapture != nullptr){
            delete screenCapture;
        }
    }

    QString path() { return m_path; }
    QDBusObjectPath qPath(){ return QDBusObjectPath(path()); }
    void registerPath(){
        auto bus = QDBusConnection::systemBus();
        bus.unregisterObject(path(), QDBusConnection::UnregisterTree);
        if(bus.registerObject(path(), this, QDBusConnection::ExportAllContents)){
            qDebug() << "Registered" << path() << OXIDE_APPLICATION_INTERFACE;
        }else{
            qDebug() << "Failed to register" << path();
        }
    }
    void unregisterPath(){
        auto bus = QDBusConnection::systemBus();
        if(bus.objectRegisteredAt(path()) != nullptr){
            qDebug() << "Unregistered" << path();
            bus.unregisterObject(path());
        }
    }
    enum ApplicationState { Inactive, InForeground, InBackground, Paused };
    Q_ENUM(ApplicationState)

    Q_INVOKABLE void launch();
    Q_INVOKABLE void pause(bool startIfNone = true);
    Q_INVOKABLE void resume();
    Q_INVOKABLE void signal(int signal);
    Q_INVOKABLE void unregister();

    QString name() { return m_name; }
    QString description() { return m_description; }
    QString call() { return m_call; }
    QString term() { return m_term; }
    bool autoStart() { return m_autoStart; }
    bool systemApp() { return m_systemApp; }
    int type() { return (int)m_type; }
    int state();

    void load(
        QString name, QString description, QString call, QString term,
        int type, bool autostart, bool systemApp
    );
    void saveScreen(){
        if(screenCapture != nullptr){
            return;
        }
        qDebug() << "Saving screen...";
        int frameBufferHandle = open("/dev/fb0", O_RDWR);
        char* frameBuffer = (char*)mmap(0, DISPLAYSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, frameBufferHandle, 0);
        qDebug() << "Compressing data...";
        auto compressedData = qCompress(QByteArray(frameBuffer, DISPLAYSIZE));
        close(frameBufferHandle);
        screenCapture = new QByteArray(compressedData);
    }
    void recallScreen(){
        if(screenCapture == nullptr){
            return;
        }
        qDebug() << "Uncompressing screen...";
        auto uncompressedData = qUncompress(*screenCapture);
        if(!uncompressedData.size()){
            qDebug() << "Screen capture was corrupt";
            qDebug() << screenCapture->size();
            delete screenCapture;
            return;
        }
        qDebug() << "Recalling screen...";
        int frameBufferHandle = open("/dev/fb0", O_RDWR);
        auto frameBuffer = (char*)mmap(0, DISPLAYSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, frameBufferHandle, 0);
        memcpy(frameBuffer, uncompressedData, DISPLAYSIZE);

        mxcfb_update_data update_data;
        mxcfb_rect update_rect;
        update_rect.top = 0;
        update_rect.left = 0;
        update_rect.width = DISPLAYWIDTH;
        update_rect.height = DISPLAYHEIGHT;
        update_data.update_marker = 0;
        update_data.update_region = update_rect;
        update_data.waveform_mode = WAVEFORM_MODE_AUTO;
        update_data.update_mode = UPDATE_MODE_FULL;
        update_data.dither_mode = EPDC_FLAG_USE_DITHERING_MAX;
        update_data.temp = TEMP_USE_REMARKABLE_DRAW;
        update_data.flags = 0;
        ioctl(frameBufferHandle, MXCFB_SEND_UPDATE, &update_data);

        close(frameBufferHandle);
        delete screenCapture;
        screenCapture = nullptr;
    }
    void waitForFinished(){
        if(m_process->processId()){
            m_process->waitForFinished();
        }
    }
signals:
    void launched();
    void paused();
    void resumed();
    void signaled(int);
    void unregistered();
    void exited(int);

private slots:
    void started();
    void finished(int exitCode);
    void readyReadStandardError(){
        const char* prefix = ("[" + name() + " " + QString::number(m_process->processId()) + "]").toUtf8();
        QString error = m_process->readAllStandardError();
        for(QString line : error.split(QRegExp("[\r\n]"), QString::SkipEmptyParts)){
            if(!line.isEmpty()){
                sd_journal_print(LOG_INFO, "%s %s", prefix, (const char*)line.toUtf8());
            }
        }
    }
    void readyReadStandardOutput(){
        const char* prefix = ("[" + name() + " " + QString::number(m_process->processId()) + "]").toUtf8();
        QString output = m_process->readAllStandardOutput();
        for(QString line : output.split(QRegExp("[\r\n]"), QString::SkipEmptyParts)){
            if(!line.isEmpty()){
                sd_journal_print(LOG_ERR, "%s %s", prefix, (const char*)line.toUtf8());
            }
        }
    }
    void stateChanged(QProcess::ProcessState state){
        switch(state){
            case QProcess::Starting:
                qDebug() << "Application" << name() << "is starting.";
            break;
            case QProcess::Running:
                qDebug() << "Application" << name() << "is running.";
            break;
            case QProcess::NotRunning:
                qDebug() << "Application" << name() << "is not running.";
            break;
            default:
                qDebug() << "Application" << name() << "unknown state" << state;
        }
    }
    void errorOccurred(QProcess::ProcessError error);

private:
    QString m_path;
    QString m_name;
    QString m_description;
    QString m_call;
    QString m_term;
    int m_type;
    bool m_autoStart;
    bool m_systemApp;
    QProcess* m_process;
    bool m_backgrounded;
    QByteArray* screenCapture = nullptr;
    size_t screenCaptureSize;
    InputManager* inputManager(){
        static InputManager* instance = new InputManager();
        return instance;
    }
};

#endif // APPLICATION_H