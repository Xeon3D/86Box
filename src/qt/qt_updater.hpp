#ifndef QT_UPDATER_HPP
#define QT_UPDATER_HPP

#include <QDateTime>
#include <QFile>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkReply;
class QWidget;

/* Offers the experimental builds from the 86Box CI, and installs them over
   the running copy.

   Updates are owned by the VM manager, or by a machine that runs without one;
   a lock file keeps several of them from updating at the same time. An update
   only takes effect once 86Box is started again. */
class Updater : public QObject {
    Q_OBJECT

public:
    struct Build {
        int     number = 0;
        QString fileName;
        QUrl    url;
        /* The changes since the installed build, newest first. */
        QString changes;
    };

    enum class Role {
        Manager,
        Machine
    };

    static Updater *instance();

    /* Whether this copy of 86Box can update itself; if not, the reason why. */
    static bool isSupported(QString *reason = nullptr);

    /* Starts the scheduled checks. The status callback shows a message in
       the window's status bar. */
    void start(QWidget *window, Role role, std::function<void(const QString &)> statusCallback);

    /* The build that runs the next time 86Box is started. */
    int       installedBuild();
    QDateTime lastCheck();
    bool      isBusy() const { return busy; }

    /* Finds the latest build; emits checkFinished(). */
    void checkNow();
    /* Checks for updates like checkNow(), and shows the result: offers to
       install or skip a new build. */
    void checkInteractively(QWidget *parent);
    /* Downloads and installs a build; emits installFinished(), then offers
       to restart. */
    void install(const Updater::Build &build);

    /* Called once the application has quit: starts the new build if a
       restart was requested. */
    static void relaunchIfRequested();

signals:
    void checkFinished(const Updater::Build &latest, const QString &error);
    void installFinished(int build, const QString &error);

private:
    explicit Updater(QObject *parent = nullptr);

    void scheduledCheck();
    void fetchLatest(std::function<void(const Updater::Build &, const QString &)> callback);
    void download(const Updater::Build &build);
    void downloadFinished();
    void applyUpdate(const Updater::Build &build, const QString &archive);
    void finishInstall(const Updater::Build &build, const QString &error);
    void showUpdatePrompt(const Updater::Build &build);
    enum class Choice {
        Install,
        Skip,
        Later
    };
    Choice offerBuild(const Updater::Build &build, QWidget *parent, bool scheduled);
    bool canShowPrompt() const;
    void showRestartPrompt(int build);
    void requestRestart();
    bool tryLock();
    void unlock();
    void cleanUpOldFiles();

    static qint64 intervalSeconds(int interval);

    QSettings                         state;
    std::unique_ptr<QLockFile>        lockFile;
    QNetworkAccessManager             network;
    QTimer                            timer;
    QPointer<QWidget>                 window;
    Role                              role = Role::Machine;
    std::function<void(const QString &)> status;

    QPointer<QNetworkReply> reply;
    QFile                   downloadFile;
    Build                   pendingBuild;
    bool                    busy      = false;
    bool                    automatic = false;
    bool                    interactive = false;
    qint64                  startTime;

    static bool    restartPending;
    static QString launchPath;
};

Q_DECLARE_METATYPE(Updater::Build)

#endif // QT_UPDATER_HPP
