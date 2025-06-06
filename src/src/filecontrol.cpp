// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filecontrol.h"
#include "types.h"
#include "unionimage/unionimage_global.h"
#include "unionimage/unionimage.h"
#include "printdialog/printhelper.h"
#include "ocr/ocrinterface.h"
#include "imagedata/imageinfo.h"

#include <DSysInfo>

#include <QFileInfo>
#include <QDir>
#include <QMimeDatabase>
#include <QCollator>
#include <QUrl>
#include <QDBusInterface>
#include <QThread>
#include <QProcess>
#include <QGuiApplication>
#include <QScreen>
#include <QDesktopServices>
#include <QClipboard>
#include <QApplication>
#include <QUrl>
#include <QDebug>

#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

DCORE_USE_NAMESPACE

const QString SETTINGS_GROUP = "MAINWINDOW";
const QString SETTINGS_WINSIZE_W_KEY = "WindowWidth";
const QString SETTINGS_WINSIZE_H_KEY = "WindowHeight";
// 是否显示导航窗口
const QString SETTINGS_ENABLE_NAVIGATION = "EnableNavigation";
const int MAINWIDGET_MINIMUN_HEIGHT = 300;
const int MAINWIDGET_MINIMUN_WIDTH = 658;

bool compareByFileInfo(const QFileInfo &str1, const QFileInfo &str2)
{
    static QCollator sortCollator;
    sortCollator.setNumericMode(true);
    return sortCollator.compare(str1.baseName(), str2.baseName()) < 0;
}

// 转换路径
QUrl UrlInfo(QString path)
{
    QUrl url;
    // Just check if the path is an existing file.
    if (QFile::exists(path)) {
        url = QUrl::fromLocalFile(QDir::current().absoluteFilePath(path));
        return url;
    }

    const auto match = QRegularExpression(QStringLiteral(":(\\d+)(?::(\\d+))?:?$")).match(path);

    if (match.isValid()) {
        // cut away line/column specification from the path.
        path.chop(match.capturedLength());
    }

    // make relative paths absolute using the current working directory
    // prefer local file, if in doubt!
    url = QUrl::fromUserInput(path, QDir::currentPath(), QUrl::AssumeLocalFile);

    // in some cases, this will fail, e.g.
    // assume a local file and just convert it to an url.
    if (!url.isValid()) {
        // create absolute file path, we will e.g. pass this over dbus to other processes
        url = QUrl::fromLocalFile(QDir::current().absoluteFilePath(path));
    }
    return url;
}

FileControl::FileControl(QObject *parent)
    : QObject(parent)
{
    qDebug() << "Initializing FileControl";
    m_ocrInterface = new OcrInterface("com.deepin.Ocr", "/com/deepin/Ocr", QDBusConnection::sessionBus(), this);
    m_shortcutViewProcess = new QProcess(this);
    m_config = LibConfigSetter::instance();
    imageFileWatcher = ImageFileWatcher::instance();

    QObject::connect(imageFileWatcher, &ImageFileWatcher::imageFileChanged, this, &FileControl::imageFileChanged);

    // 在1000ms以内只保存一次配置信息
    if (!m_tSaveSetting) {
        m_tSaveSetting = new QTimer(this);
        connect(m_tSaveSetting, &QTimer::timeout, this, [=]() { saveSetting(); });
    }

    listsupportWallPaper << "bmp"
                         << "cod"
                         << "png"
                         << "gif"
                         << "ief"
                         << "jpe"
                         << "jpeg"
                         << "jpg"
                         << "jfif"
                         << "tif"
                         << "tiff";

    // 实时保存旋转后图片太卡，因此采用10ms后延时保存的问题
    if (!m_tSaveImage) {
        m_tSaveImage = new QTimer(this);
        connect(m_tSaveImage, &QTimer::timeout, this, [ = ]() {
            //保存旋转的图片
            slotRotatePixCurrent();
            emit callSavePicDone(QUrl::fromLocalFile(m_currentPath).toString());
        });
    }
}

FileControl::~FileControl()
{
    qDebug() << "Destroying FileControl";
    saveSetting();
}

QString FileControl::standardPicturesPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
}

QStringList FileControl::getDirImagePath(const QString &path)
{
    if (path.isEmpty()) {
        return QStringList();
    }

    QStringList image_list;
    QString DirPath = QFileInfo(LibUnionImage_NameSpace::localPath(path)).dir().path();

    QDir _dirinit(DirPath);
    QFileInfoList m_AllPath = _dirinit.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);

    // 修复Ｑt带后缀排序错误的问题
    std::sort(m_AllPath.begin(), m_AllPath.end(), compareByFileInfo);
    for (int i = 0; i < m_AllPath.size(); i++) {
        QString tmpPath = m_AllPath.at(i).filePath();
        if (tmpPath.isEmpty()) {
            continue;
        }
        // 判断是否图片格式
        if (isImage(tmpPath)) {
            image_list << QUrl::fromLocalFile(tmpPath).toString();
        }
    }
    return image_list;
}

/**
   @return 返回文件路径 \a path 所在的文件夹是否为当前监控的文件夹
 */
bool FileControl::isCurrentWatcherDir(const QUrl &path)
{
    return imageFileWatcher->isCurrentDir(path.toLocalFile());
}

QString FileControl::getNamePath(const  QString &oldPath, const QString &newName)
{
    QString old = LibUnionImage_NameSpace::localPath(oldPath);
    QString now = LibUnionImage_NameSpace::localPath(newName);

    QFileInfo info(old);
    QString path = info.path();
    QString suffix = info.suffix();
    QString newPath = path + "/" + newName + "." + suffix;
    return QUrl::fromLocalFile(newPath).toString();
}

bool FileControl::isImage(const QString &path)
{
    // 将传入path路径统一为绝对路径
    QString tmpPath = LibUnionImage_NameSpace::localPath(path);
    return LibUnionImage_NameSpace::isImage(tmpPath);
}

bool FileControl::isVideo(const QString &path)
{
    // 将传入path路径统一为绝对路径
    QString tmpPath = LibUnionImage_NameSpace::localPath(path);
    return LibUnionImage_NameSpace::isVideo(tmpPath);
}

void FileControl::setWallpaper(const QString &imgPath)
{
    QThread *th1 = QThread::create([=]() {
        if (!imgPath.isNull()) {
            qInfo() << "Setting wallpaper:" << imgPath;
            QString path = imgPath;
            // 202011/12 bug54279
            {
                // 设置壁纸代码改变，采用DBus,原方法保留
                if (/*!qEnvironmentVariableIsEmpty("FLATPAK_APPID")*/ 1) {
                    // gdbus call -e -d com.deepin.daemon.Appearance -o /com/deepin/daemon/Appearance -m
                    // com.deepin.daemon.Appearance.Set background /home/test/test.png
                    qDebug() << "Setting wallpaper via flatpak:" << path;
                    QDBusInterface interfaceV23(
                        "org.deepin.dde.Appearance1", "/org/deepin/dde/Appearance1", "org.deepin.dde.Appearance1");
                    QDBusInterface interfaceV20(
                        "com.deepin.daemon.Appearance", "/com/deepin/daemon/Appearance", "com.deepin.daemon.Appearance");

                    if (interfaceV23.isValid() || interfaceV20.isValid()) {
                        QString screenname;

                        // 判断环境是否是wayland
                        auto e = QProcessEnvironment::systemEnvironment();
                        QString XDG_SESSION_TYPE = e.value(QStringLiteral("XDG_SESSION_TYPE"));
                        QString WAYLAND_DISPLAY = e.value(QStringLiteral("WAYLAND_DISPLAY"));

                        bool isWayland = false;
                        if (XDG_SESSION_TYPE != QLatin1String("wayland") &&
                            !WAYLAND_DISPLAY.contains(QLatin1String("wayland"), Qt::CaseInsensitive)) {
                            isWayland = false;
                        } else {
                            isWayland = true;
                        }
                        // wayland下设置壁纸使用，2020/09/21
                        if (isWayland) {
                            QDBusInterface interfaceWaylandV23(
                                "org.deepin.dde.Display1", "/org/deepin/dde/Display1", "org.deepin.dde.Display1");
                            if (interfaceWaylandV23.isValid()) {
                                screenname = qvariant_cast<QString>(interfaceWaylandV23.property("Primary"));
                                qDebug() << "Using v23 wayland interface for wallpaper setting";

                            } else {
                                QDBusInterface interfaceWaylandV20(
                                    "com.deepin.daemon.Display", "/com/deepin/daemon/Display", "com.deepin.daemon.Display");
                                screenname = qvariant_cast<QString>(interfaceWaylandV20.property("Primary"));
                                qDebug() << "Using v20 wayland interface for wallpaper setting";
                            }
                        } else {
                            screenname = QGuiApplication::primaryScreen()->name();
                        }

                        bool settingSucc = false;
                        if (interfaceV23.isValid()) {
                            QDBusMessage reply = interfaceV23.call(QStringLiteral("SetMonitorBackground"), screenname, path);
                            settingSucc = reply.errorMessage().isEmpty();

                            qDebug() << "Using v23 interface for wallpaper setting";
                            qDebug() << "DBus parameters - screen:" << screenname << "path:" << path;
                            if (!settingSucc) {
                                qWarning() << "DBus error in v23 interface:" << reply.errorMessage();
                            }
                        }

                        if (interfaceV20.isValid() && !settingSucc) {
                            QDBusMessage reply = interfaceV20.call(QStringLiteral("SetMonitorBackground"), screenname, path);

                            qDebug() << "Using v20 interface for wallpaper setting";
                            qDebug() << "DBus parameters - screen:" << screenname << "path:" << path;
                            if (!reply.errorMessage().isEmpty()) {
                                qWarning() << "DBus error in v20 interface:" << reply.errorMessage();
                            }
                        }
                    } else {
                        qWarning() << "Failed to set wallpaper - both v23 and v20 interfaces are invalid";
                    }
                }
            }
        }
    });
    connect(th1, &QThread::finished, th1, &QObject::deleteLater);
    th1->start();
}

bool FileControl::deleteImagePath(const QString &path)
{
    QUrl displayUrl = QUrl(path);

    if (displayUrl.isValid()) {
        qInfo() << "Attempting to delete image:" << path;
        QStringList list;
        list << displayUrl.toString();
        QDBusInterface interface(QStringLiteral("org.freedesktop.FileManager1"),
                                 QStringLiteral("/org/freedesktop/FileManager1"),
                                 QStringLiteral("org.freedesktop.FileManager1"));
        // 默认超时时间大约25s, 修改为最大限制
        interface.setTimeout(INT_MAX);
        auto pendingCall = interface.asyncCall("Trash", list);
        while (!pendingCall.isFinished()) {
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }

        if (pendingCall.isError()) {
            auto error = pendingCall.error();
            qWarning() << "Failed to delete image via DBus:" << error.name() << error.message();
            return false;
        }

        // 删除信息未通过 DBus 返回，直接判断文件是否已被删除
        if (QFile::exists(displayUrl.toLocalFile())) {
            qWarning() << "Delete operation failed - file still exists:" << displayUrl.toLocalFile();
            return false;
        }

        qInfo() << "Successfully deleted image:" << path;
        return true;
    }
    qWarning() << "Invalid URL for deletion:" << path;
    return false;
}

bool FileControl::displayinFileManager(const QString &path)
{
    bool bRet = false;
    QUrl displayUrl = QUrl(path);

    QDBusInterface interface(QStringLiteral("org.freedesktop.FileManager1"),
                             QStringLiteral("/org/freedesktop/FileManager1"),
                             QStringLiteral("org.freedesktop.FileManager1"));

    if (interface.isValid()) {
        QStringList list;
        list << displayUrl.toString();
        bRet = interface.call("ShowItems", list, "").type() != QDBusMessage::ErrorMessage;
    }
    return bRet;
}

void FileControl::copyImage(const QString &path)
{
    QString localPath = QUrl(path).toLocalFile();

    QClipboard *cb = qApp->clipboard();

    // Ownership of the new data is transferred to the clipboard.
    QMimeData *newMimeData = new QMimeData();

    // Copy old mimedata
    //    const QMimeData* oldMimeData = cb->mimeData();
    //    for ( const QString &f : oldMimeData->formats())
    //        newMimeData->setData(f, oldMimeData->data(f));

    // Copy file (gnome)
    QByteArray gnomeFormat = QByteArray("copy\n");
    QString text;
    QList<QUrl> dataUrls;

    if (!localPath.isEmpty())
        text += localPath + '\n';
    dataUrls << QUrl::fromLocalFile(localPath);
    gnomeFormat.append(QUrl::fromLocalFile(localPath).toEncoded()).append("\n");

    newMimeData->setText(text.endsWith('\n') ? text.left(text.length() - 1) : text);
    newMimeData->setUrls(dataUrls);
    gnomeFormat.remove(gnomeFormat.length() - 1, 1);
    newMimeData->setData("x-special/gnome-copied-files", gnomeFormat);

    // Copy Image Date
    //    QImage img(paths.first());
    //    Q_ASSERT(!img.isNull());
    //    newMimeData->setImageData(img);

    // Set the mimedata
    //    cb->setMimeData(newMimeData);
    cb->setMimeData(newMimeData, QClipboard::Clipboard);
}

void FileControl::copyText(const QString &str)
{
    qApp->clipboard()->setText(str);
}

bool FileControl::isRotatable(const QString &path)
{
    bool bRet = false;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    QFileInfo info(localPath);
    if (!info.isFile() || !info.exists() || !info.isWritable() || !info.isReadable()) {
        bRet = false;
    } else {
        bRet = LibUnionImage_NameSpace::isImageSupportRotate(localPath);
    }
    return bRet;
}

bool FileControl::isCanWrite(const QString &path)
{
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    QFileInfo info(localPath);
    bool bRet = info.isWritable() && QFileInfo(info.dir(), info.dir().path()).isWritable();  // 是否可写
    return bRet;
}

bool FileControl::isCanDelete(const QStringList &pathList)
{
    bool bCanDelete = false;
    for (int i = 0; i < pathList.size(); i++) {
        if (!pathList[i].isEmpty() && isCanDelete(pathList[i])) {
            bCanDelete = true;
            break;
        }
    }

    return bCanDelete;
}

bool FileControl::isCanDelete(const QString &path)
{
    bool bRet = false;
    bool isAlbum = false;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    QFileInfo info(localPath);
    bool isWritable = info.isWritable() && QFileInfo(info.dir(), info.dir().path()).isWritable();  // 是否可写
    bool isReadable = info.isReadable();                                                           // 是否可读
    imageViewerSpace::PathType pathType = LibUnionImage_NameSpace::getPathType(localPath);
    if ((imageViewerSpace::PathTypeAPPLE != pathType && imageViewerSpace::PathTypeSAFEBOX != pathType &&
         imageViewerSpace::PathTypeRECYCLEBIN != pathType && imageViewerSpace::PathTypeMTP != pathType &&
         imageViewerSpace::PathTypePTP != pathType && isWritable && isReadable) ||
        (isAlbum && isWritable)) {
        bRet = true;
    } else {
        bRet = false;
    }
    return bRet;
}

void FileControl::ocrImage(const QString &path, int index)
{
    QString localPath = QUrl(path).toLocalFile();
    // 此处借用已取得的缓存信息，一般状态下，调用OCR前已完成图像的加载
    ImageInfo info(path);

    if (Types::MultiImage != info.type()) {  // 非多页图使用路径直接进行识别
        m_ocrInterface->openFile(localPath);
    } else {  // 多页图需要确定识别哪一页
        QImageReader imageReader(localPath);
        imageReader.jumpToImage(index);
        auto image = imageReader.read();
        auto tempDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir dir(tempDir);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
        auto tempFileName = tempDir + QDir::separator() + "rec.png";

        image.save(tempFileName);
        m_ocrInterface->openFile(tempFileName);
    }
}

bool FileControl::isCanPrint(const QVariantList &pathList)
{
    bool bCanPrint = true;
    for (int i = 0; i < pathList.size(); i++) {
        if (!pathList[i].toString().isEmpty() && !isCanPrint(pathList[i].toString())) {
            bCanPrint = false;
            break;
        }
    }

    return bCanPrint;
}

bool FileControl::isCanPrint(const QString &path)
{
    QFileInfo info(LibUnionImage_NameSpace::localPath(path));
    return isImage(path) && info.isReadable();
}

QStringList FileControl::parseCommandlineGetPaths()
{
    QStringList paths;
    QStringList validPaths;
    QString filepath = "";
    QStringList arguments = QCoreApplication::arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        QString path = LibUnionImage_NameSpace::localPath(UrlInfo(arguments[i]));
        if (QFileInfo(path).isFile()) {
            QString filepath = QUrl::fromLocalFile(path).toString();
            if (isImage(filepath) || isVideo(filepath)) {
                validPaths.push_back(filepath);
            }
            paths.push_back(filepath);
        }
    }

    if (validPaths.empty() && !paths.isEmpty())
        emit invalidFormat();

    return validPaths;
}

QString FileControl::slotGetFileName(const QString &path)
{
    QString tmppath = LibUnionImage_NameSpace::localPath(path);

    QFileInfo info(tmppath);
    return info.completeBaseName();
}

QString FileControl::slotGetFileNameSuffix(const QString &path)
{
    QString tmppath = LibUnionImage_NameSpace::localPath(path);

    QFileInfo info(tmppath);
    return info.fileName();
}

QString FileControl::slotGetInfo(const QString &key, const QString &path)
{
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    if (localPath != m_currentPath) {
        m_currentPath = localPath;
        m_currentAllInfo = LibUnionImage_NameSpace::getAllMetaData(localPath);
    }

    QString returnString = m_currentAllInfo.value(key);
    if (returnString.isEmpty()) {
        returnString = "-";
    }

    return returnString;
}

bool FileControl::slotFileReName(const QString &name, const QString &filepath, bool isSuffix)
{
    QString localPath = LibUnionImage_NameSpace::localPath(filepath);
    QFile file(localPath);
    if (file.exists()) {
        qInfo() << "Attempting to rename file from" << localPath << "to" << name;
        QFileInfo info(localPath);
        QString path = info.path();
        QString suffix = info.suffix();
        QString _newName;
        if (isSuffix) {
            _newName = path + "/" + name;
        } else {
            _newName = path + "/" + name + "." + suffix;
        }

        if (file.rename(_newName)) {
            qInfo() << "Successfully renamed file to:" << _newName;
            imageFileWatcher->fileRename(localPath, _newName);

            Q_EMIT imageRenamed(QUrl::fromLocalFile(localPath), QUrl::fromLocalFile(_newName));
            return true;
        }

        qWarning() << "Failed to rename file:" << localPath;
        return false;
    }
    qWarning() << "File does not exist for renaming:" << localPath;
    return false;
}

QString FileControl::slotFileSuffix(const QString &path, bool ret)
{
    QString returnSuffix = "";

    QString localPath = LibUnionImage_NameSpace::localPath(path);
    if (!path.isEmpty() && QFile::exists(localPath)) {
        QString tmppath = path;
        QFileInfo info(tmppath);
        if (ret) {
            returnSuffix = "." + info.completeSuffix();
        } else {
            returnSuffix = info.completeSuffix();
        }
    }

    return returnSuffix;
}

bool FileControl::isShowToolTip(const QString &oldPath, const QString &name)
{
    bool bRet = false;
    QString path = LibUnionImage_NameSpace::localPath(oldPath);
    QFileInfo fileinfo(path);
    QString DirPath = fileinfo.path();
    QString filename = fileinfo.completeBaseName();
    if (filename == name)
        return false;

    QString format = fileinfo.suffix();

    QString fileabname = DirPath + "/" + name + "." + format;
    QFile file(fileabname);
    if (file.exists() && fileabname != path) {
        bRet = true;
    } else {
        bRet = false;
    }
    return bRet;
}

void FileControl::showPrintDialog(const QString &path)
{
    QString oldPath = LibUnionImage_NameSpace::localPath(path);
    PrintHelper::getIntance()->showPrintDialog(QStringList(oldPath));
}

void FileControl::showPrintDialog(const QStringList &paths)
{
    QStringList localPaths ;
    for (QString path : paths) {
        localPaths << LibUnionImage_NameSpace::localPath(path);
    }
    PrintHelper::getIntance()->showPrintDialog(localPaths);
}

QVariant FileControl::getConfigValue(const QString &group, const QString &key, const QVariant &defaultValue)
{
    return m_config->value(group, key, defaultValue);
}

void FileControl::setConfigValue(const QString &group, const QString &key, const QVariant &value)
{
    m_config->setValue(group, key, value);
}

int FileControl::getlastWidth()
{
    int reWidth = 0;
    int defaultW = 0;

    // 多屏下仅采用单个屏幕处理， 使用主屏的参考宽度计算
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return MAINWIDGET_MINIMUN_WIDTH;
    }

    // 默认高度 在1920下为1150
    if (QGuiApplication::screens().size() > 1 && screen) {
        defaultW = int(double(screen->size().width()) * 0.599);
    } else {
        defaultW = int(double(screen->geometry().width()) * 0.599);
    }

    const int ww = getConfigValue(SETTINGS_GROUP, SETTINGS_WINSIZE_W_KEY, QVariant(defaultW)).toInt();

    reWidth = ww >= MAINWIDGET_MINIMUN_WIDTH ? ww : MAINWIDGET_MINIMUN_WIDTH;
    m_windowWidth = reWidth;
    return reWidth;
}

int FileControl::getlastHeight()
{
    int reHeight = 0;
    int defaultH = 0;

    // 多屏下仅采用单个屏幕处理， 使用主屏的参考高度计算
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return MAINWIDGET_MINIMUN_HEIGHT;
    }

    // 默认高度 在1080下为800
    if (QGuiApplication::screens().size() > 1 && screen) {
        defaultH = int(double(screen->size().height()) * 0.741);
    } else {
        defaultH = int(double(screen->geometry().height()) * 0.741);
    }

    const int wh = getConfigValue(SETTINGS_GROUP, SETTINGS_WINSIZE_H_KEY, QVariant(defaultH)).toInt();

    reHeight = wh >= MAINWIDGET_MINIMUN_HEIGHT ? wh : MAINWIDGET_MINIMUN_HEIGHT;
    m_windowHeight = reHeight;
    return reHeight;
}

void FileControl::setSettingWidth(int width)
{
    m_windowWidth = width;
    m_tSaveSetting->setSingleShot(true);
    m_tSaveSetting->start(1000);
}

void FileControl::setSettingHeight(int height)
{
    m_windowHeight = height;
    m_tSaveSetting->setSingleShot(true);
    m_tSaveSetting->start(1000);
}

void FileControl::setEnableNavigation(bool b)
{
    setConfigValue(SETTINGS_GROUP, SETTINGS_ENABLE_NAVIGATION, b);
}

bool FileControl::isEnableNavigation()
{
    return getConfigValue(SETTINGS_GROUP, SETTINGS_ENABLE_NAVIGATION, true).toBool();
}

void FileControl::saveSetting()
{
    if (m_lastSaveWidth != m_windowWidth) {
        setConfigValue(SETTINGS_GROUP, SETTINGS_WINSIZE_W_KEY, m_windowWidth);
        m_lastSaveWidth = m_windowWidth;
    }
    if (m_lastSaveHeight != m_windowHeight) {
        setConfigValue(SETTINGS_GROUP, SETTINGS_WINSIZE_H_KEY, m_windowHeight);
        m_lastSaveHeight = m_windowHeight;
    }
}

bool FileControl::isSupportSetWallpaper(const QString &path)
{
    QString path1 = LibUnionImage_NameSpace::localPath(path);
    QFileInfo fileinfo(path1);
    QString format = fileinfo.suffix().toLower();
    // 设置为壁纸需要判断是否有读取权限
    if (listsupportWallPaper.contains(format) && fileinfo.isReadable()) {
        return true;
    }
    return false;
}

bool FileControl::isCheckOnly()
{
    // single
    QString userName = QDir::homePath().section("/", -1, -1);
    QString appName = "deepin-image-viewer";
    if (m_viewerType == imageViewerSpace::ImgViewerTypeAlbum) {
        appName = "deepin-album";
    }
    std::string path = ("/home/" + userName + "/.cache/deepin/" + appName + "/").toStdString();
    QDir tdir(path.c_str());
    if (!tdir.exists()) {
        bool ret = tdir.mkpath(path.c_str());
        qDebug() << ret;
    }

    path += "single";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    int flock = lockf(fd, F_TLOCK, 0);

    if (fd == -1) {
        perror("open lockfile/n");
        return false;
    }
    if (flock == -1) {
        perror("lock file error/n");
        return false;
    }
    return true;
}

bool FileControl::isCanSupportOcr(const QString &path)
{
    bool bRet = false;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    QFileInfo info(localPath);
    imageViewerSpace::ImageType type = LibUnionImage_NameSpace::getImageType(localPath);
    if (imageViewerSpace::ImageTypeDynamic != type && info.isReadable()) {
        bRet = true;
    }
    return bRet;
}

bool FileControl::isCanRename(const QString &path)
{
    bool bRet = false;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    imageViewerSpace::PathType pathType = LibUnionImage_NameSpace::getPathType(localPath);  // 路径类型
    QFileInfo info(localPath);
    bool isWritable = info.isWritable() && QFileInfo(info.dir(), info.dir().path()).isWritable();  // 是否可写
    if (info.isReadable() && isWritable && imageViewerSpace::PathTypeMTP != pathType &&
        imageViewerSpace::PathTypePTP != pathType && imageViewerSpace::PathTypeAPPLE != pathType) {
        bRet = true;
    }
    return bRet;
}

bool FileControl::isCanReadable(const QString &path)
{
    bool bRet = false;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    QFileInfo info(localPath);
    if (info.isReadable()) {
        bRet = true;
    }
    return bRet;
}

/**
 * @brief 根据传入的文件路径列表 \a filePaths 重设缓存的文件信息，
 *      若在图片打开过程中文件被修改，将发送信号至界面或其它处理。
 */
void FileControl::resetImageFiles(const QStringList &filePaths)
{
    // 变更监控的文件
    imageFileWatcher->resetImageFiles(filePaths);
    // 清理缩略图缓存记录
    ImageInfo::clearCache();
}

/**
 * @return 返回公司Logo图标地址
 */
QUrl FileControl::getCompanyLogo()
{
    QString logoPath = DSysInfo::distributionOrgLogo(DSysInfo::Distribution, DSysInfo::Light, ":/assets/images/deepin-logo.svg");
    return QUrl::fromLocalFile(logoPath);
}

void FileControl::terminateShortcutPanelProcess()
{
    m_shortcutViewProcess->terminate();
    m_shortcutViewProcess->waitForFinished(2000);
}

void FileControl::showShortcutPanel(int windowCenterX, int windowCenterY)
{
    QPoint pos(windowCenterX, windowCenterY);
    QStringList shortcutString;
    auto json = createShortcutString();

    QString param1 = "-j=" + json;
    QString param2 = "-p=" + QString::number(pos.x()) + "," + QString::number(pos.y());
    shortcutString << param1 << param2;

    terminateShortcutPanelProcess();
    m_shortcutViewProcess->start("deepin-shortcut-viewer", shortcutString);
}

QString FileControl::createShortcutString()
{
    if (!m_shortcutString.isEmpty()) {
        return m_shortcutString;
    }

    QJsonObject shortcut1;
    shortcut1.insert("name", tr("Fullscreen"));
    shortcut1.insert("value", "F11");

    QJsonObject shortcut2;
    shortcut2.insert("name", tr("Exit fullscreen"));
    shortcut2.insert("value", "Esc");

    QJsonObject shortcut3;
    shortcut3.insert("name", tr("Extract text"));
    shortcut3.insert("value", "Alt + O");

    QJsonObject shortcut4;
    shortcut4.insert("name", tr("Slide show"));
    shortcut4.insert("value", "F5");

    QJsonObject shortcut5;
    shortcut5.insert("name", tr("Rename"));
    shortcut5.insert("value", "F2");

    QJsonObject shortcut6;
    shortcut6.insert("name", tr("Copy"));
    shortcut6.insert("value", "Ctrl + C");

    QJsonObject shortcut7;
    shortcut7.insert("name", tr("Delete"));
    shortcut7.insert("value", "Delete");

    QJsonObject shortcut8;
    shortcut8.insert("name", tr("Rotate clockwise"));
    shortcut8.insert("value", "Ctrl + R");

    QJsonObject shortcut9;
    shortcut9.insert("name", tr("Rotate counterclockwise"));
    shortcut9.insert("value", "Ctrl + Shift + R");

    QJsonObject shortcut10;
    shortcut10.insert("name", tr("Set as wallpaper"));
    shortcut10.insert("value", "Ctrl + F9");

    QJsonObject shortcut11;
    shortcut11.insert("name", tr("Display in file manager"));
    shortcut11.insert("value", "Alt + D");

    QJsonObject shortcut12;
    shortcut12.insert("name", tr("Image info"));
    shortcut12.insert("value", "Ctrl + I");

    QJsonObject shortcut13;
    shortcut13.insert("name", tr("Previous"));
    shortcut13.insert("value", "Left");

    QJsonObject shortcut14;
    shortcut14.insert("name", tr("Next"));
    shortcut14.insert("value", "Right");

    QJsonObject shortcut15;
    shortcut15.insert("name", tr("Zoom in"));
    shortcut15.insert("value", "Ctrl + '+'");

    QJsonObject shortcut16;
    shortcut16.insert("name", tr("Zoom out"));
    shortcut16.insert("value", "Ctrl + '-'");

    QJsonObject shortcut17;
    shortcut17.insert("name", tr("Open"));
    shortcut17.insert("value", "Ctrl + O");

    QJsonObject shortcut18;
    shortcut18.insert("name", tr("Print"));
    shortcut18.insert("value", "Ctrl + P");

    QJsonArray shortcutArray1;
    shortcutArray1.append(shortcut1);
    shortcutArray1.append(shortcut2);
    shortcutArray1.append(shortcut3);
    shortcutArray1.append(shortcut4);
    shortcutArray1.append(shortcut5);
    shortcutArray1.append(shortcut6);
    shortcutArray1.append(shortcut7);
    shortcutArray1.append(shortcut8);
    shortcutArray1.append(shortcut9);
    shortcutArray1.append(shortcut10);
    shortcutArray1.append(shortcut11);
    shortcutArray1.append(shortcut12);
    shortcutArray1.append(shortcut13);
    shortcutArray1.append(shortcut14);
    shortcutArray1.append(shortcut15);
    shortcutArray1.append(shortcut16);
    shortcutArray1.append(shortcut17);
    shortcutArray1.append(shortcut18);

    QJsonObject shortcut_group1;
    shortcut_group1.insert("groupName", tr("Image Viewing"));
    shortcut_group1.insert("groupItems", shortcutArray1);

    QJsonObject shortcut19;
    shortcut19.insert("name", tr("Help"));
    shortcut19.insert("value", "F1");

    QJsonObject shortcut20;
    shortcut20.insert("name", tr("Display shortcuts"));
    shortcut20.insert("value", "Ctrl + Shift + ?");

    QJsonArray shortcutArray2;
    shortcutArray2.append(shortcut19);
    shortcutArray2.append(shortcut20);

    QJsonObject shortcut_group2;
    shortcut_group2.insert("groupName", tr("Settings"));
    shortcut_group2.insert("groupItems", shortcutArray2);

    QJsonObject shortcut21;
    shortcut21.insert("name", tr("Copy"));
    shortcut21.insert("value", "Ctrl + C");

    QJsonObject shortcut22;
    shortcut22.insert("name", tr("Select all"));
    shortcut22.insert("value", "Ctrl + A");

    QJsonArray shortcutArray3;
    shortcutArray3.append(shortcut21);
    shortcutArray3.append(shortcut22);

    QJsonObject shortcut_group3;
    shortcut_group3.insert("groupName", tr("Live Text"));
    shortcut_group3.insert("groupItems", shortcutArray3);

    QJsonArray shortcutArrayall;
    shortcutArrayall.append(shortcut_group1);
    shortcutArrayall.append(shortcut_group3);
    shortcutArrayall.append(shortcut_group2);

    QJsonObject main_shortcut;
    main_shortcut.insert("shortcut", shortcutArrayall);

    m_shortcutString = QJsonDocument(main_shortcut).toJson();

    return m_shortcutString;
}

void FileControl::copyImage(const QStringList &paths)
{
    //  Get clipboard
    QClipboard *cb = qApp->clipboard();

    // Ownership of the new data is transferred to the clipboard.
    QMimeData *newMimeData = new QMimeData();
    QByteArray gnomeFormat = QByteArray("copy\n");
    QString text;
    QList<QUrl> dataUrls;
    for (QString path : paths) {
        if (!path.isEmpty())
            text += path + '\n';
        dataUrls << QUrl(path);
        gnomeFormat.append(QUrl(path).toEncoded()).append("\n");
    }

    newMimeData->setText(text.endsWith('\n') ? text.left(text.length() - 1) : text);
    newMimeData->setUrls(dataUrls);
    gnomeFormat.remove(gnomeFormat.length() - 1, 1);
    newMimeData->setData("x-special/gnome-copied-files", gnomeFormat);

    // Set the mimedata
    cb->setMimeData(newMimeData, QClipboard::Clipboard);
}

bool FileControl::isRotatable(const QStringList &pathList)
{
    bool bRotateable = true;
    for (int i = 0; i < pathList.size(); i++) {
        if (!pathList[i].isEmpty() && !isRotatable(pathList[i])) {
            bRotateable = false;
            break;
        }
    }

    return bRotateable;
}

QString FileControl::getDirPath(const QString &path)
{
    QFileInfo firstFileInfo(path);

    return firstFileInfo.dir().path();
}

bool FileControl::pathExists(const QString &path)
{
    QUrl url(path);
    return QFileInfo::exists(LibUnionImage_NameSpace::localPath(url));
}

bool FileControl::haveImage(const QVariantList &urls)
{
    for (auto &url : urls) {
        if (!url.isNull() && isImage(LibUnionImage_NameSpace::localPath(url.toString()))) {
            return true;
        }
    }
    return false;
}

bool FileControl::haveVideo(const QVariantList &urls)
{
    for (auto &url : urls) {
        if (!url.isNull() && isVideo(url.toString())) {
            return true;
        }
    }
    return false;
}

bool FileControl::isFile(const QString &path)
{
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    return QFileInfo(localPath).isFile();
}

bool FileControl::rotateFile(const QVariantList &pathList, const int &rotateAngel)
{
    bool bRet = true;
    for (int i = 0; i < pathList.size(); i++) {
        if (!pathList[i].toString().isEmpty()) {
            rotateFile(pathList[i].toString(), rotateAngel);
        }
    }

    return bRet;
}

bool FileControl::rotateFile(const QString &path, const int &rotateAngel)
{
    bool bRet = true;
    QString localPath = LibUnionImage_NameSpace::localPath(path);
    if (m_currentPath != localPath) {
        slotRotatePixCurrent(true);
        m_currentPath = localPath;
        m_rotateAngel = rotateAngel;
    } else {
        m_rotateAngel += rotateAngel;
    }

    // 减少频繁的触发旋转进行文件读取写入操作
    m_tSaveImage->setSingleShot(true);
    m_tSaveImage->start(100);

    return bRet;
}

/**
 * @brief 立即保存旋转图片，通过定时器延后触发或图片切换时手动触发
 * @note 当前通过保存图片后，监控文件变更触发更新图片的信号
 */
void FileControl::slotRotatePixCurrent(bool bNotifyExternal/* = false*/)
{
    // 由QML调用(切换图片)时，停止定时器，防止二次触发
    if (m_tSaveImage->isActive()) {
        m_tSaveImage->stop();
    }

    m_rotateAngel = m_rotateAngel % 360;
    if (0 != m_rotateAngel) {
        qInfo() << "Rotating image:" << m_currentPath << "by angle:" << m_rotateAngel;
        //20211019修改：特殊位置不执行写入操作
        imageViewerSpace::PathType pathType = LibUnionImage_NameSpace::getPathType(m_currentPath);

        if (pathType != imageViewerSpace::PathTypeMTP && pathType != imageViewerSpace::PathTypePTP && //安卓手机
                pathType != imageViewerSpace::PathTypeAPPLE && //苹果手机
                pathType != imageViewerSpace::PathTypeSAFEBOX && //保险箱
                pathType != imageViewerSpace::PathTypeRECYCLEBIN) { //回收站

            QString erroMsg;
            LibUnionImage_NameSpace::rotateImageFIle(m_rotateAngel, m_currentPath, erroMsg);

            // 保存文件后向外部(比如相册缩略图列表)发送图片更新信号，通知外部刷新图片内容
            if (bNotifyExternal)
                emit callSavePicDone(QUrl::fromLocalFile(m_currentPath).toString());
        } else {
            qDebug() << "Skipping rotation for special path type:" << pathType;
        }
    }
    m_rotateAngel = 0;
}

void FileControl::setViewerType(imageViewerSpace::ImgViewerType type)
{
    m_viewerType = type;
}

bool FileControl::isAlbum()
{
    bool bRet = false;
    if (m_viewerType == imageViewerSpace::ImgViewerTypeAlbum) {
        bRet = true;
    }
    return bRet;
}

bool FileControl::checkMimeUrls(const QList<QUrl> &urls)
{
    if (1 > urls.size()) {
        return false;
    }
    QList<QUrl> urlList = urls;
    for (QUrl url : urlList) {
        const QString path = LibUnionImage_NameSpace::localPath(url);
        QFileInfo fileinfo(path);
        if (fileinfo.isDir()) {
            auto finfos = LibUnionImage_NameSpace::getImagesAndVideoInfo(path, false);
            for (auto finfo : finfos) {
                if (LibUnionImage_NameSpace::imageSupportRead(finfo.absoluteFilePath()) || LibUnionImage_NameSpace::isVideo(finfo.absoluteFilePath())) {
                    return true;
                }
            }
        } else if (LibUnionImage_NameSpace::imageSupportRead(path) || LibUnionImage_NameSpace::isVideo(path)) {
            return true;
        }
    }
    return false;
}
