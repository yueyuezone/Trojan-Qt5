#include "addresstester.h"
#include "connection.h"
#include "confighelper.h"
#include "pacserver.h"
#include "portvalidator.h"
#include <QCoreApplication>
#include <QDir>
#include <QHostInfo>
#include <QHostAddress>

#include "logger.h"

Connection::Connection(QObject *parent) :
    QObject(parent),
    running(false),
    service(new ServiceThread(this))
{
#ifdef Q_OS_WIN
    configFile = QCoreApplication::applicationDirPath() + "/config.ini";
#else
    QDir configDir = QDir::homePath() + "/.config/trojan-qt5";
    configFile = configDir.absolutePath() + "/config.ini";
#endif
    connect(service, &ServiceThread::startFailed, this, &Connection::onStartFailed);
}

Connection::Connection(const TQProfile &_profile, QObject *parent) :
    Connection(parent)
{
    profile = _profile;
}

Connection::Connection(QString uri, QObject *parent) :
    Connection(parent)
{
    profile = TQProfile(uri);
}

Connection::~Connection()
{
    stop();
}

const TQProfile& Connection::getProfile() const
{
    return profile;
}

const QString& Connection::getName() const
{
    return profile.name;
}

QByteArray Connection::getURI() const
{
    QString uri = profile.toUri();
    return QByteArray(uri.toUtf8());
}

bool Connection::isValid() const
{
    if (profile.serverAddress.isEmpty() || profile.password.isEmpty() || profile.localAddress.isEmpty()) {
        return false;
    }
    else {
        return true;
    }
}

const bool &Connection::isRunning() const
{
    return running;
}

void Connection::latencyTest()
{
    QHostAddress serverAddr(profile.serverAddress);
    if (serverAddr.isNull()) {
        QHostInfo::lookupHost(profile.serverAddress, this, SLOT(onServerAddressLookedUp(QHostInfo)));
    } else {
        testAddressLatency(serverAddr);
    }
}

void Connection::start()
{
    profile.lastTime = QDateTime::currentDateTime();
    //perform a latency test if the latency is unknown
    if (profile.latency == TQProfile::LATENCY_UNKNOWN) {
        latencyTest();
    }

    /** MUST initial there otherwise privoxy will not listen port. */
    privoxy = new PrivoxyThread();

    /** Inital PAC class there. */
    PACServer *pac = new PACServer();

    ConfigHelper *conf = new ConfigHelper(configFile);

    // Generate Config File that trojan and privoxy will use
    ConfigHelper::connectionToJson(profile);
    ConfigHelper::generatePrivoxyConf(profile);

#ifdef Q_OS_WIN
    QString file = QCoreApplication::applicationDirPath() + "/config.json";
#else
    QDir configDir = QDir::homePath() + "/.config/trojan-qt5";
    QString file = configDir.absolutePath() + "/config.json";
#endif
    /** load service config first. */
    service->config().load(file.toStdString());

    /** Wait, let's check if port is in use. */
    PortValidator *pv = new PortValidator();
    if (pv->isInUse(profile.localPort) || pv->isInUse(profile.localHttpPort)) {
        qCritical() << QString("There is some thing listening on port %1 or %2").arg(QString::number(profile.localPort)).arg(QString::number(profile.localHttpPort));
        Logger::error(QString("There is some thing listening on port %1 or %2").arg(QString::number(profile.localPort)).arg(QString::number(profile.localHttpPort)));
        emit stateChanged(false);
        return;
    }

    /** Set running status to true before we start trojan. */
    running = true;
    service->start();

    /** Start privoxy if profile is configured to do so. */
    if (profile.dualMode) {
        privoxy->start();
    }

    /** Modify PAC File if user have enabled PAC Mode. */
    if (conf->isEnablePACMode()) {
        pac->modify(profile);
    }

    emit stateChanged(running);

    /** Set proxy settings after emit the signal. */
    if (conf->isAutoSetSystemProxy()) {
        if (conf->isEnablePACMode()) {
            SystemProxyHelper::setSystemProxy(profile, 2);
        } else {
            SystemProxyHelper::setSystemProxy(profile, 1);
        }
    }
}

void Connection::stop()
{
    ConfigHelper *conf = new ConfigHelper(configFile);

    if (running) {
        /** Set the running status to false first. */
        running = false;
        service->stop();

        /** If we have started privoxy, stop it. */
        if (profile.dualMode) {
            privoxy->stop();
        }

        emit stateChanged(running);

        /** Set proxy settings after emit the signal. */
        if (conf->isAutoSetSystemProxy()) {
            SystemProxyHelper::setSystemProxy(profile, 0);
        }
    }
}

void Connection::onStartFailed()
{
    ConfigHelper *conf = new ConfigHelper(configFile);

    running = false;
    emit stateChanged(running);
    emit startFailed();

    /** Set proxy settings if the setting is configured to do so. */
    if (conf->isAutoSetSystemProxy()) {
        SystemProxyHelper::setSystemProxy(profile, 0);
    }
}

void Connection::testAddressLatency(const QHostAddress &addr)
{
    AddressTester *addrTester = new AddressTester(addr, profile.serverPort, this);
    connect(addrTester, &AddressTester::lagTestFinished, this, &Connection::onLatencyAvailable, Qt::QueuedConnection);
    addrTester->startLagTest();
}

void Connection::onServerAddressLookedUp(const QHostInfo &host)
{
    if (host.error() == QHostInfo::NoError) {
        testAddressLatency(host.addresses().first());
    } else {
        onLatencyAvailable(TQProfile::LATENCY_ERROR);
    }
}

void Connection::onLatencyAvailable(const int latency)
{
    profile.latency = latency;
    emit latencyAvailable(latency);
}
