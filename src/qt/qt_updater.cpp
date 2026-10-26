/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Automatic updater module.
 *
 * Authors: Marcos Alves
 *
 *          Copyright 2026 Marcos Alves
 */
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <cstdio>
#include <zlib.h>

#include "qt_updater.hpp"

extern "C" {
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/ini.h>
#include <86box/plat.h>
#include <86box/version.h>
}

/* The same data the experimental builds page on the 86Box website shows,
   for the recent builds, with the changes each of them made. */
static const char *buildsUrl   = "https://ci.86box.net/job/86Box/api/json"
                                 "?tree=builds[number,result,artifacts[fileName,relativePath],changeSets[items[msg]]]{0,50}";
static const char *artifactUrl ="https://ci.86box.net/job/86Box/%1/artifact/%2";

bool    Updater::restartPending = false;
QString Updater::launchPath;

namespace {

/* The download for this operating system and architecture, empty if there is none. */
QString
artifactPattern()
{
#if defined(Q_OS_WINDOWS) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("^86Box-Windows-64-b\\d+\\.zip$");
#elif defined(Q_OS_WINDOWS) && defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("^86Box-Windows-ARM64-b\\d+\\.zip$");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("^86Box-Linux-x86_64-b\\d+\\.AppImage$");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("^86Box-Linux-arm64-b\\d+\\.AppImage$");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("^86Box-macOS-.+-b\\d+\\.zip$");
#else
    return {};
#endif
}

/* What an update replaces: the executable on Windows, the AppImage on Linux
   and the application bundle on macOS. */
QString
installTarget()
{
#if defined(Q_OS_LINUX)
    return qEnvironmentVariable("APPIMAGE");
#elif defined(Q_OS_MACOS)
    return QDir(QCoreApplication::applicationDirPath() + "/../..").absolutePath();
#else
    return QCoreApplication::applicationFilePath();
#endif
}

QString
globalConfigFile(const QString &name)
{
    char dir[1024];

    plat_get_global_config_dir(dir, sizeof(dir));
    return QDir(QString::fromUtf8(dir)).filePath(name);
}

/* Another instance may have changed the update preferences since they were loaded. */
void
reloadUpdateSettings()
{
    ini_t file = ini_read(global_cfg_path);

    if (file == nullptr)
        return;

    update_auto       = !!ini_get_int(file, "Updates", "auto_update", 0);
    update_interval   = ini_get_int(file, "Updates", "interval", UPDATE_INTERVAL_DAILY);
    update_skip_build = ini_get_int(file, "Updates", "skip_build", 0);
    update_pin_build  = ini_get_int(file, "Updates", "pin_build", 0);
    if ((update_interval < 0) || (update_interval >= UPDATE_INTERVAL_MAX))
        update_interval = UPDATE_INTERVAL_DAILY;

    ini_close(file);
}

QNetworkRequest
makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::UserAgentHeader, QString("%1/%2 (build %3)").arg(EMU_NAME, EMU_VERSION).arg(EMU_BUILD_NUM));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(60000);
#endif
    return request;
}

/* A minimal reader for the zip archives of the Windows and macOS builds. */
struct ZipEntry {
    QString name;
    quint16 flags;
    quint16 method;
    quint32 crc;
    quint32 compressedSize;
    quint32 size;
    quint32 localOffset;
};

quint16
read16(const QByteArray &data, qint64 offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData()) + offset;
    return p[0] | (p[1] << 8);
}

quint32
read32(const QByteArray &data, qint64 offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData()) + offset;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24);
}

[[maybe_unused]] bool
readZipDirectory(const QByteArray &zip, QVector<ZipEntry> &entries)
{
    const qint64 size = zip.size();
    qint64       end  = -1;

    if (size < 22)
        return false;

    /* The end of central directory record, followed by a comment of up to 64 KB. */
    for (qint64 offset = size - 22; offset >= qMax<qint64>(0, size - 22 - 0xffff); offset--) {
        if (read32(zip, offset) == 0x06054b50) {
            end = offset;
            break;
        }
    }
    if (end < 0)
        return false;

    const int count  = read16(zip, end + 10);
    qint64    offset = read32(zip, end + 16);
    for (int i = 0; i < count; i++) {
        if ((offset + 46 > size) || (read32(zip, offset) != 0x02014b50))
            return false;

        ZipEntry entry;
        entry.flags          = read16(zip, offset + 8);
        entry.method         = read16(zip, offset + 10);
        entry.crc            = read32(zip, offset + 16);
        entry.compressedSize = read32(zip, offset + 20);
        entry.size           = read32(zip, offset + 24);
        entry.localOffset    = read32(zip, offset + 42);

        const int nameLength    = read16(zip, offset + 28);
        const int extraLength   = read16(zip, offset + 30);
        const int commentLength = read16(zip, offset + 32);
        if (offset + 46 + nameLength > size)
            return false;
        /* ZIP64 is not needed for the size of a build. */
        if ((entry.compressedSize == 0xffffffff) || (entry.size == 0xffffffff) || (entry.localOffset == 0xffffffff))
            return false;

        entry.name = QString::fromUtf8(zip.constData() + offset + 46, nameLength).replace('\\', '/');
        entries.append(entry);

        offset += 46 + nameLength + extraLength + commentLength;
    }

    return true;
}

[[maybe_unused]] bool
extractZipEntry(const QByteArray &zip, const ZipEntry &entry, QByteArray &data)
{
    const qint64 offset = entry.localOffset;

    /* Encrypted entries are not supported. */
    if ((offset + 30 > zip.size()) || (read32(zip, offset) != 0x04034b50) || (entry.flags & 1))
        return false;

    const qint64 start = offset + 30 + read16(zip, offset + 26) + read16(zip, offset + 28);
    if (start + entry.compressedSize > zip.size())
        return false;

    if (entry.size == 0)
        data.clear();
    else if (entry.method == 0) {
        if (entry.compressedSize != entry.size)
            return false;
        data = QByteArray(zip.constData() + start, entry.size);
    } else if (entry.method == 8) {
        z_stream stream {};

        data.resize(entry.size);
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
            return false;
        stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(zip.constData() + start));
        stream.avail_in  = entry.compressedSize;
        stream.next_out  = reinterpret_cast<Bytef *>(data.data());
        stream.avail_out = entry.size;

        const int ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if ((ret != Z_STREAM_END) || (stream.total_out != entry.size))
            return false;
    } else
        return false;

    return crc32(0L, reinterpret_cast<const Bytef *>(data.constData()), data.size()) == entry.crc;
}

/* Also when the clock went back. */
bool
isDue(qint64 lastCheck, qint64 now, qint64 interval)
{
    return (lastCheck > now) || ((now - lastCheck) >= interval);
}

/* Keeps the files of an archive inside the folder they are extracted to. */
[[maybe_unused]] bool
isSafePath(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(':'))
        return false;

    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || (part == ".") || (part == ".."))
            return false;
    }
    return true;
}

/* Puts the downloaded build in place of the running one, which keeps running
   from the old files. Runs outside the UI thread. The replaced files are added
   to the cleanup list, to be deleted once they are no longer in use. */
QString
installArchive(const QString &archive, const QString &target, int build, QStringList &cleanup)
{
    const QString oldSuffix = QString(".b%1.updold").arg(EMU_BUILD_NUM);

#if defined(Q_OS_WINDOWS)
    QFile file(archive);
    if (!file.open(QIODevice::ReadOnly))
        return Updater::tr("Unable to open %1.").arg(QDir::toNativeSeparators(archive));
    const QByteArray zip = file.readAll();
    file.close();

    QVector<ZipEntry> entries;
    if (!readZipDirectory(zip, entries))
        return Updater::tr("The downloaded update is damaged.");

    /* The files are installed relative to the folder of the executable in the
       archive. The executable keeps the name of the running one. */
    QString mainExecutable;
    for (const auto &entry : entries) {
        if (!entry.name.endsWith('/') && !QFileInfo(entry.name).fileName().compare("86Box.exe", Qt::CaseInsensitive)
            && (mainExecutable.isEmpty() || (entry.name.size() < mainExecutable.size())))
            mainExecutable = entry.name;
    }
    if (mainExecutable.isEmpty())
        return Updater::tr("The downloaded update does not contain %1.").arg("86Box.exe");
    const QString prefix = mainExecutable.left(mainExecutable.size() - QFileInfo(mainExecutable).fileName().size());

    const QDir    appDir(QFileInfo(target).absolutePath());
    const QString staging = appDir.filePath(QString(".86box-update-%1").arg(build));
    QDir(staging).removeRecursively();

    QStringList files;
    for (const auto &entry : entries) {
        if (!entry.name.startsWith(prefix) || entry.name.endsWith('/'))
            continue;

        QString path = entry.name.mid(prefix.size());
        if (entry.name == mainExecutable)
            path = QFileInfo(target).fileName();

        QByteArray data;
        if (!isSafePath(path) || !extractZipEntry(zip, entry, data)) {
            QDir(staging).removeRecursively();
            return Updater::tr("The downloaded update is damaged.");
        }

        const QString staged = staging + "/" + path;
        QFile         out(staged);
        QDir().mkpath(QFileInfo(staged).absolutePath());
        if (!out.open(QIODevice::WriteOnly) || (out.write(data) != data.size())) {
            QDir(staging).removeRecursively();
            return Updater::tr("Unable to write %1.").arg(QDir::toNativeSeparators(staged));
        }
        out.close();
        files.append(path);
    }

    /* Running files cannot be overwritten, but they can be renamed. */
    QVector<QPair<QString, QString>> renamed;
    QStringList                      replaced;
    const auto                       rollback = [&renamed, &staging]() {
        for (auto it = renamed.crbegin(); it != renamed.crend(); ++it)
            QFile::rename(it->second, it->first);
        QDir(staging).removeRecursively();
    };

    for (const auto &path : files) {
        const QString dest = appDir.filePath(path);

        if (QFile::exists(dest)) {
            QString old = dest + oldSuffix;
            for (int i = 2; QFile::exists(old) && !QFile::remove(old); i++)
                old = dest + oldSuffix + QString::number(i);
            if (!QFile::rename(dest, old)) {
                rollback();
                return Updater::tr("Unable to replace %1.").arg(QDir::toNativeSeparators(dest));
            }
            renamed.append({ dest, old });
            replaced.append(old);
        }

        QDir().mkpath(QFileInfo(dest).absolutePath());
        if (!QFile::rename(staging + "/" + path, dest)) {
            rollback();
            return Updater::tr("Unable to replace %1.").arg(QDir::toNativeSeparators(dest));
        }
        renamed.append({ staging + "/" + path, dest });
    }

    QDir(staging).removeRecursively();
    cleanup += replaced;
    return {};
#elif defined(Q_OS_MACOS)
    const QFileInfo bundle(target);
    const QString   staging = bundle.absolutePath() + QString("/.86box-update-%1").arg(build);
    QDir(staging).removeRecursively();

    /* ditto keeps the symbolic links and permissions inside the bundle. */
    QProcess ditto;
    ditto.start("/usr/bin/ditto", { "-x", "-k", archive, staging });
    if (!ditto.waitForFinished(5 * 60 * 1000) || (ditto.exitStatus() != QProcess::NormalExit) || (ditto.exitCode() != 0)) {
        QDir(staging).removeRecursively();
        return Updater::tr("The downloaded update is damaged.");
    }

    const QStringList apps = QDir(staging).entryList({ "*.app" }, QDir::Dirs | QDir::NoDotAndDotDot);
    if (apps.isEmpty()) {
        QDir(staging).removeRecursively();
        return Updater::tr("The downloaded update does not contain %1.").arg("86Box.app");
    }

    const QString old = bundle.absolutePath() + "/." + bundle.fileName() + oldSuffix;
    QDir(old).removeRecursively();
    if (!QDir().rename(target, old)) {
        QDir(staging).removeRecursively();
        return Updater::tr("Unable to replace %1.").arg(target);
    }
    if (!QDir().rename(staging + "/" + apps.first(), target)) {
        QDir().rename(old, target);
        QDir(staging).removeRecursively();
        return Updater::tr("Unable to replace %1.").arg(target);
    }

    QDir(staging).removeRecursively();
    cleanup.append(old);
    return {};
#else
    Q_UNUSED(build)
    Q_UNUSED(oldSuffix)
    Q_UNUSED(cleanup)

    QFile file(archive);
    if (!file.open(QIODevice::ReadOnly) || (file.read(4) != QByteArray("\x7f" "ELF", 4)))
        return Updater::tr("The downloaded update is damaged.");
    file.close();

    /* Renaming over the AppImage is atomic, and the running one keeps its file. */
    const QFileInfo info(target);
    const QString   temp = info.absolutePath() + "/." + info.fileName() + ".update";
    QFile::remove(temp);
    if (!QFile::copy(archive, temp)) {
        QFile::remove(temp);
        return Updater::tr("Unable to write %1.").arg(temp);
    }
    QFile::setPermissions(temp, QFile::permissions(target) | QFile::ReadOwner | QFile::ExeOwner);
    if (std::rename(QFile::encodeName(temp).constData(), QFile::encodeName(target).constData()) != 0) {
        QFile::remove(temp);
        return Updater::tr("Unable to replace %1.").arg(target);
    }
    return {};
#endif
}

}

Updater::Updater(QObject *parent)
    : QObject(parent)
    , state(globalConfigFile("86box-update.ini"), QSettings::IniFormat)
    , startTime(QDateTime::currentSecsSinceEpoch())
{
    qRegisterMetaType<Updater::Build>();

#ifdef Q_OS_LINUX
    launchPath = installTarget();
#else
    launchPath = QCoreApplication::applicationFilePath();
#endif

    timer.setInterval(30 * 60 * 1000);
    connect(&timer, &QTimer::timeout, this, &Updater::scheduledCheck);
}

Updater *
Updater::instance()
{
    static Updater *updater = nullptr;

    if (updater == nullptr)
        updater = new Updater(qApp);
    return updater;
}

bool
Updater::isSupported(QString *reason)
{
    const auto unsupported = [reason](const QString &text) {
        if (reason)
            *reason = text;
        return false;
    };

#if !defined(UPDATE_CHECK)
    return unsupported(tr("Automatic updates are only available in the official experimental builds of %1.").arg(EMU_NAME));
#elif defined(RELEASE_BUILD)
    return unsupported(tr("This is a release build of %1. Automatic updates only install experimental builds, so they are not available in release builds.").arg(EMU_NAME));
#else
    if (artifactPattern().isEmpty())
        return unsupported(tr("Automatic updates are not available for this operating system or architecture."));

    const QString target = installTarget();
#    if defined(Q_OS_LINUX)
    if (target.isEmpty())
        return unsupported(tr("Automatic updates are only available in the AppImage version of %1.").arg(EMU_NAME));
#    elif defined(Q_OS_MACOS)
    if (!target.endsWith(".app"))
        return unsupported(tr("Automatic updates are only available when %1 runs from its application bundle.").arg(EMU_NAME));
#    endif

    const QString   dir = QFileInfo(target).absolutePath();
    QTemporaryFile probe(dir + "/.86box-update-XXXXXX");
    if (!probe.open())
        return unsupported(tr("Automatic updates are not available because %1 is installed in a folder that cannot be written to:\n%2").arg(EMU_NAME, QDir::toNativeSeparators(dir)));

    return true;
#endif
}

void
Updater::start(QWidget *mainWindow, Role newRole, std::function<void(const QString &)> statusCallback)
{
    window = mainWindow;
    role   = newRole;
    status = std::move(statusCallback);

    if (!isSupported())
        return;

    cleanUpOldFiles();

    /* Leave the network alone while 86Box is starting. */
    QTimer::singleShot(60 * 1000, this, &Updater::scheduledCheck);
    timer.start();
}

int
Updater::installedBuild()
{
    state.sync();

    /* An update installed after this instance started is not running yet. */
    const int    build = state.value("installed_build", 0).toInt();
    const qint64 time  = state.value("installed_time", 0).toLongLong();
    if ((build > EMU_BUILD_NUM) && (time >= startTime))
        return build;

    return EMU_BUILD_NUM;
}

QDateTime
Updater::lastCheck()
{
    state.sync();

    const qint64 time = state.value("last_check", 0).toLongLong();
    return time ? QDateTime::fromSecsSinceEpoch(time) : QDateTime();
}

qint64
Updater::intervalSeconds(int interval)
{
    switch (interval) {
        case UPDATE_INTERVAL_6_HOURS:
            return 6 * 60 * 60;
        case UPDATE_INTERVAL_12_HOURS:
            return 12 * 60 * 60;
        default:
        case UPDATE_INTERVAL_DAILY:
            return 24 * 60 * 60;
        case UPDATE_INTERVAL_WEEKLY:
            return 7 * 24 * 60 * 60;
        case UPDATE_INTERVAL_MONTHLY:
            return 30 * 24 * 60 * 60;
    }
}

bool
Updater::tryLock()
{
    if (!lockFile) {
        lockFile = std::make_unique<QLockFile>(globalConfigFile("86box-update.lock"));
        /* A download can take a while; a lock left by a crash is still
           recognized by its process being gone. */
        lockFile->setStaleLockTime(2 * 60 * 60 * 1000);
    }
    return lockFile->tryLock(0);
}

void
Updater::unlock()
{
    if (lockFile)
        lockFile->unlock();
}

void
Updater::cleanUpOldFiles()
{
    if (!tryLock())
        return;

    state.sync();
    QStringList remaining;
    for (const auto &path : state.value("cleanup").toStringList()) {
        const QFileInfo info(path);
        if (!info.exists())
            continue;
        /* Still in use by another instance. */
        if (!(info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path)))
            remaining.append(path);
    }
    if (remaining.isEmpty())
        state.remove("cleanup");
    else
        state.setValue("cleanup", remaining);
    state.sync();

    unlock();
}

void
Updater::scheduledCheck()
{
    if (busy)
        return;

    reloadUpdateSettings();
    if (!update_auto || update_pin_build || !isSupported())
        return;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const auto   due = [this, now]() {
        state.sync();
        return isDue(state.value("last_check", 0).toLongLong(), now, intervalSeconds(update_interval));
    };
    if (!due() || !tryLock())
        return;

    /* Another instance may have checked while this one waited for the lock. */
    if (!due()) {
        unlock();
        return;
    }

    busy      = true;
    automatic = true;
    fetchLatest([this, now](const Updater::Build &latest, const QString &error) {
        if (!error.isEmpty()) {
            qWarning() << "Update check failed:" << error;
            busy = false;
            unlock();
            return;
        }

        state.setValue("last_check", now);
        state.sync();

        busy = false;
        unlock();

        /* A build the user declined is not offered again. */
        if ((latest.number > installedBuild()) && (latest.number != update_skip_build))
            showUpdatePrompt(latest);
    });
}

void
Updater::checkNow()
{
    if (busy)
        return;

    busy = true;
    fetchLatest([this](const Updater::Build &latest, const QString &error) {
        busy = false;
        if (error.isEmpty()) {
            state.setValue("last_check", QDateTime::currentSecsSinceEpoch());
            state.sync();
        }
        emit checkFinished(latest, error);
    });
}

void
Updater::checkInteractively(QWidget *parent)
{
    const QString title = tr("%1 Update").arg(EMU_NAME);
    QString       reason;

    if (!isSupported(&reason)) {
        QMessageBox::information(parent, title, reason);
        return;
    }
    if (busy) {
        QMessageBox::information(parent, title, tr("An update is already in progress."));
        return;
    }

    /* The same check as the button in the preferences, with its result shown. */
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection     = connect(this, &Updater::checkFinished, this,
                              [this, connection, title, parent = QPointer<QWidget>(parent)](const Updater::Build &latest, const QString &error) {
        disconnect(*connection);

        if (!error.isEmpty()) {
            QMessageBox::warning(parent, title, tr("Unable to check for updates: %1").arg(error));
            return;
        }
        if (latest.number <= installedBuild()) {
            QMessageBox::information(parent, title, tr("You are using the latest build."));
            return;
        }

        switch (offerBuild(latest, parent, false)) {
            case Choice::Install:
                interactive = true;
                install(latest);
                break;
            case Choice::Skip:
                update_skip_build = latest.number;
                config_save_global_setting("Updates", "skip_build");
                break;
            case Choice::Later:
                break;
        }
    });
    checkNow();
}

Updater::Choice
Updater::offerBuild(const Updater::Build &build, QWidget *parent, bool scheduled)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("%1 Update").arg(EMU_NAME));

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Build %1 of %2 is available.").arg(build.number).arg(EMU_NAME)));
    layout->addWidget(new QLabel(tr("Changes:")));

    auto *changes = new QPlainTextEdit(build.changes);
    changes->setReadOnly(true);
    changes->setMinimumSize(480, 200);
    layout->addWidget(changes);

    QString note;
    if (scheduled)
        note = tr("If you choose not to update now, you will not be asked about this build again; it can still be installed from Preferences > Updates.");
    else if (update_pin_build)
        note = tr("You chose to stay on build %1. To install this build, uncheck that option in Preferences > Updates.").arg(update_pin_build);
    if (!note.isEmpty()) {
        auto *label = new QLabel(note);
        label->setWordWrap(true);
        layout->addWidget(label);
    }

    auto  *buttons       = new QDialogButtonBox;
    auto  *installButton = buttons->addButton(scheduled ? tr("Update now") : tr("Install build %1").arg(build.number), QDialogButtonBox::AcceptRole);
    auto  *skipButton    = scheduled ? nullptr : buttons->addButton(tr("Skip build %1").arg(build.number), QDialogButtonBox::ActionRole);
    auto  *laterButton   = buttons->addButton(scheduled ? tr("Not now") : tr("Close"), QDialogButtonBox::RejectRole);
    /* Declining a build offered on its own skips it. */
    Choice choice = scheduled ? Choice::Skip : Choice::Later;
    installButton->setEnabled(scheduled || !update_pin_build);
    if (skipButton)
        skipButton->setEnabled(build.number != update_skip_build);
    laterButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton *button) {
        if (button == installButton)
            choice = Choice::Install;
        else if (button == skipButton)
            choice = Choice::Skip;
        dialog.done(0);
    });
    layout->addWidget(buttons);

    dialog.exec();
    return choice;
}

void
Updater::install(const Updater::Build &build)
{
    if (busy) {
        emit installFinished(build.number, tr("An update is already in progress."));
        return;
    }
    if (!tryLock()) {
        emit installFinished(build.number, tr("Another instance of %1 is installing an update.").arg(EMU_NAME));
        return;
    }

    busy      = true;
    automatic = false;
    download(build);
}

void
Updater::fetchLatest(std::function<void(const Updater::Build &, const QString &)> callback)
{
    auto *buildsReply = network.get(makeRequest(QUrl(buildsUrl)));

    connect(buildsReply, &QNetworkReply::finished, this, [this, buildsReply, callback]() {
        buildsReply->deleteLater();
        if (buildsReply->error() != QNetworkReply::NoError) {
            callback({}, buildsReply->errorString());
            return;
        }

        /* The builds are listed newest first; the latest is the newest
           successful one with a download for this system. */
        const auto               builds = QJsonDocument::fromJson(buildsReply->readAll()).object()["builds"].toArray();
        const QRegularExpression pattern(artifactPattern());
        Build                    latest;
        int                      index = 0;
        for (; (index < builds.size()) && (latest.number <= 0); index++) {
            const auto build = builds[index].toObject();
            if (build["result"].toString() != "SUCCESS")
                continue;
            for (const auto &value : build["artifacts"].toArray()) {
                const auto artifact = value.toObject();
                if (pattern.match(artifact["fileName"].toString()).hasMatch()) {
                    latest.number   = build["number"].toInt();
                    latest.fileName = artifact["fileName"].toString();
                    latest.url      = QUrl(QString(artifactUrl).arg(latest.number).arg(artifact["relativePath"].toString()));
                    break;
                }
            }
        }
        if (latest.number <= 0) {
            callback({}, tr("Unable to determine release information"));
            return;
        }

        /* A build lists the commits since the build before it, whatever its
           result, so the changes of every build since the installed one are
           part of the update. */
        const int   installed = installedBuild();
        QStringList changes;
        for (index--; index < builds.size(); index++) {
            const auto build  = builds[index].toObject();
            const int  number = build["number"].toInt();
            if (number <= installed)
                break;

            QStringList messages;
            for (const auto &changeSet : build["changeSets"].toArray()) {
                for (const auto &item : changeSet.toObject()["items"].toArray()) {
                    const auto message = item.toObject()["msg"].toString().section('\n', 0, 0).trimmed();
                    if (!message.isEmpty())
                        messages.append(QString(QChar(0x2022)) + " " + message);
                }
            }
            if (!messages.isEmpty())
                changes.append(tr("Build %1:").arg(number) + "\n" + messages.join('\n'));
        }
        latest.changes = changes.isEmpty() ? tr("No changes were recorded for this build.") : changes.join("\n\n");

        callback(latest, {});
    });
}

void
Updater::download(const Updater::Build &build)
{
    pendingBuild = build;

    downloadFile.setFileName(QDir::temp().filePath(build.fileName));
    if (!downloadFile.open(QIODevice::WriteOnly)) {
        finishInstall(build, tr("Unable to write %1.").arg(QDir::toNativeSeparators(downloadFile.fileName())));
        return;
    }

    reply = network.get(makeRequest(build.url));
    connect(reply, &QNetworkReply::readyRead, this, [this]() {
        downloadFile.write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, &Updater::downloadFinished);
}

void
Updater::downloadFinished()
{
    reply->deleteLater();
    downloadFile.write(reply->readAll());
    downloadFile.close();

    const int    statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 expected   = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    QString      error;
    if (reply->error() != QNetworkReply::NoError)
        error = reply->errorString();
    else if (statusCode != 200)
        error = tr("The server returned HTTP status %1.").arg(statusCode);
    else if ((expected > 0) && (downloadFile.size() != expected))
        error = tr("The download of the update was interrupted.");

    if (!error.isEmpty()) {
        downloadFile.remove();
        finishInstall(pendingBuild, error);
        return;
    }

    applyUpdate(pendingBuild, downloadFile.fileName());
}

void
Updater::applyUpdate(const Updater::Build &build, const QString &archive)
{
    const QString target  = installTarget();
    auto          error   = std::make_shared<QString>();
    auto          cleanup = std::make_shared<QStringList>();

    /* Extracting a build takes a moment; keep the UI responsive. */
    QThread *thread = QThread::create([archive, target, build, error, cleanup]() {
        *error = installArchive(archive, target, build.number, *cleanup);
    });
    connect(thread, &QThread::finished, this, [this, thread, archive, build, error, cleanup]() {
        thread->deleteLater();
        QFile::remove(archive);

        if (error->isEmpty()) {
            state.sync();
            state.setValue("cleanup", state.value("cleanup").toStringList() + *cleanup);
            state.setValue("installed_build", build.number);
            state.setValue("installed_time", QDateTime::currentSecsSinceEpoch());
            state.sync();
        }
        finishInstall(build, *error);
    });
    thread->start();
}

void
Updater::finishInstall(const Updater::Build &build, const QString &error)
{
    busy = false;
    unlock();
    emit installFinished(build.number, error);

    if (!error.isEmpty()) {
        qWarning() << "Update to build" << build.number << "failed:" << error;
        if (automatic && status)
            status(tr("Unable to update %1 to build %2: %3").arg(EMU_NAME).arg(build.number).arg(error));
        if (interactive)
            QMessageBox::warning(window.data(), tr("%1 Update").arg(EMU_NAME), tr("Unable to install build %1: %2").arg(build.number).arg(error));
        interactive = false;
        return;
    }
    interactive = false;

    showRestartPrompt(build.number);
}

/* Don't interrupt a captured mouse, full screen, or another dialog. */
bool
Updater::canShowPrompt() const
{
    return !mouse_capture && !video_fullscreen && (!automatic || !QApplication::activeModalWidget());
}

void
Updater::showUpdatePrompt(const Updater::Build &build)
{
    if (!canShowPrompt()) {
        QTimer::singleShot(5000, this, [this, build]() { showUpdatePrompt(build); });
        return;
    }
    /* It may have been installed, or declined, from the preferences meanwhile. */
    if ((build.number <= installedBuild()) || (build.number == update_skip_build) || busy)
        return;

    if (offerBuild(build, window.data(), true) != Choice::Install) {
        update_skip_build = build.number;
        config_save_global_setting("Updates", "skip_build");
        return;
    }

    if (busy || !tryLock()) {
        if (status)
            status(tr("Another instance of %1 is installing an update.").arg(EMU_NAME));
        return;
    }
    busy      = true;
    automatic = true;
    download(build);
}

void
Updater::showRestartPrompt(int build)
{
    if (!update_restart_prompt) {
        if (status)
            status(tr("%1 has been updated to build %2. The update will take effect the next time it is started.").arg(EMU_NAME).arg(build));
        return;
    }

    if (!canShowPrompt()) {
        QTimer::singleShot(5000, this, [this, build]() { showRestartPrompt(build); });
        return;
    }

    QWidget    *parent = QApplication::activeModalWidget() ? QApplication::activeModalWidget() : window.data();
    QMessageBox box(QMessageBox::Icon::Question, tr("%1 Update").arg(EMU_NAME),
                    tr("%1 has just been updated to build %2.").arg(EMU_NAME).arg(build),
                    QMessageBox::NoButton, parent);

    QString info = tr("The update will only take effect the next time %1 is started. Do you want to restart it now, or restart it manually yourself later?").arg(EMU_NAME);
    if ((role == Role::Machine) && cpu_thread_run)
        info += "\n\n" + tr("Restarting now will close the emulated machine.");
    box.setInformativeText(info);

    const auto *restartButton = box.addButton(tr("Restart now"), QMessageBox::AcceptRole);
    box.setDefaultButton(box.addButton(tr("Restart later"), QMessageBox::RejectRole));
    auto *checkBox = new QCheckBox(tr("Don't show this message again"));
    box.setCheckBox(checkBox);
    box.exec();

    if (checkBox->isChecked()) {
        update_restart_prompt = 0;
        config_save_global_setting("Updates", "restart_prompt");
    }
    if (box.clickedButton() == restartButton)
        requestRestart();
}

void
Updater::requestRestart()
{
    /* Wait until the preferences, or any other dialog, are closed. */
    if (QApplication::activeModalWidget() || mouse_capture) {
        QTimer::singleShot(500, this, &Updater::requestRestart);
        return;
    }
    if (!window)
        return;

    restartPending = true;
    /* The window may ask whether to exit, and the answer may be no. */
    if (!window->close())
        restartPending = false;
}

void
Updater::relaunchIfRequested()
{
    if (!restartPending)
        return;

    QStringList arguments = QCoreApplication::arguments();
    if (!arguments.isEmpty())
        arguments.removeFirst();
    QProcess::startDetached(launchPath, arguments);
}
