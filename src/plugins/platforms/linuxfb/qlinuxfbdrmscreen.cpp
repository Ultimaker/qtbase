
/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

// Experimental DRM dumb buffer backend.
//
// TODO:
// Multiscreen: QWindow-QScreen(-output) association. Needs some reorg (device cannot be owned by screen)
// Find card via devicediscovery like in eglfs_kms.
// Mode restore like QEglFSKmsInterruptHandler.
// Formats other then 32 bpp?
// grabWindow

#include "qlinuxfbdrmscreen.h"
#include <QLoggingCategory>
#include <QGuiApplication>
#include <QPainter>
#include <QtFbSupport/private/qfbcursor_p.h>
#include <QtFbSupport/private/qfbbackingstore_p.h>
#include <QtFbSupport/private/qfbwindow_p.h>
#include <QtKmsSupport/private/qkmsdevice_p.h>
#include <QtCore/private/qcore_unix_p.h>
#include <sys/mman.h>

#include <qpa/qwindowsysteminterface.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(qLcFbDrm, "qt.qpa.fb")
Q_LOGGING_CATEGORY(qLcFbDrmTiming, "qt.qpa.drmtiming")

static const int BUFFER_COUNT = 3;

#define FRAME_TIME_HEIGHT 5
#define FRAME_RATE 61.3         

class QLinuxFbDevice : public QKmsDevice
{
public:
    struct Framebuffer {
        Framebuffer() : handle(0), pitch(0), size(0), fb(0), p(MAP_FAILED) { }
        uint32_t handle;
        uint32_t pitch;
        uint64_t size;
        uint32_t fb;
        void *p;
        QImage wrapper;
    };

    struct Output {
        Output() : backFb(0), flipped(false), lastSequence(0), lastFramesDropped(0), lastRenderFinished(0) { }
        QKmsOutput kmsOutput;
        Framebuffer fb[BUFFER_COUNT];
        QRegion dirty[BUFFER_COUNT];
        int backFb;
        bool flipped;
        unsigned int lastSequence;
        unsigned int lastFramesDropped;
        qint64 lastRenderFinished;

        QSize currentRes() const {
            const drmModeModeInfo &modeInfo(kmsOutput.modes[kmsOutput.mode]);
            return QSize(modeInfo.hdisplay, modeInfo.vdisplay);
        }
    };

    QLinuxFbDevice(QKmsScreenConfig *screenConfig);

    bool open() override;
    void close() override;

    void createFramebuffers();
    void destroyFramebuffers();
    void setMode();

    void swapBuffers(Output *output);

    int outputCount() const { return m_outputs.count(); }
    Output *output(int idx) { return &m_outputs[idx]; }

private:
    void *nativeDisplay() const override;
    QPlatformScreen *createScreen(const QKmsOutput &output) override;
    void registerScreen(QPlatformScreen *screen,
                        bool isPrimary,
                        const QPoint &virtualPos,
                        const QList<QPlatformScreen *> &virtualSiblings) override;

    bool createFramebuffer(Output *output, int bufferIdx);
    void destroyFramebuffer(Output *output, int bufferIdx);

    static void pageFlipHandler(int fd, unsigned int sequence,
                                unsigned int tv_sec, unsigned int tv_usec, void *user_data);

    QRegion drawFrameTimeBar(Output *output, qint64 waitTime);

    QVector<Output> m_outputs;
    QElapsedTimer m_timer;

    bool m_showDroppedFrames;
};

QLinuxFbDevice::QLinuxFbDevice(QKmsScreenConfig *screenConfig)
    : QKmsDevice(screenConfig, QStringLiteral("/dev/dri/card0"))
{
    m_timer.start();

    m_showDroppedFrames = qEnvironmentVariableIntValue("QT_QPA_FB_DRM_SHOWDROPPEDFRAMES") != 0;
}

bool QLinuxFbDevice::open()
{
    int fd = qt_safe_open(devicePath().toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        qErrnoWarning("Could not open DRM device %s", qPrintable(devicePath()));
        return false;
    }

    uint64_t hasDumbBuf = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &hasDumbBuf) == -1 || !hasDumbBuf) {
        qWarning("Dumb buffers not supported");
        qt_safe_close(fd);
        return false;
    }

    setFd(fd);

    qCDebug(qLcFbDrm, "DRM device %s opened", qPrintable(devicePath()));

    return true;
}

void QLinuxFbDevice::close()
{
    for (Output &output : m_outputs)
        output.kmsOutput.cleanup(this); // restore mode

    m_outputs.clear();

    if (fd() != -1) {
        qCDebug(qLcFbDrm, "Closing DRM device");
        qt_safe_close(fd());
        setFd(-1);
    }
}

void *QLinuxFbDevice::nativeDisplay() const
{
    Q_UNREACHABLE();
    return nullptr;
}

QPlatformScreen *QLinuxFbDevice::createScreen(const QKmsOutput &output)
{
    qCDebug(qLcFbDrm, "Got a new output: %s", qPrintable(output.name));
    Output o;
    o.kmsOutput = output;
    m_outputs.append(o);
    return nullptr; // no platformscreen, we are not a platform plugin
}

void QLinuxFbDevice::registerScreen(QPlatformScreen *screen,
                                    bool isPrimary,
                                    const QPoint &virtualPos,
                                    const QList<QPlatformScreen *> &virtualSiblings)
{
    Q_UNUSED(screen);
    Q_UNUSED(isPrimary);
    Q_UNUSED(virtualPos);
    Q_UNUSED(virtualSiblings);
    Q_UNREACHABLE();
}

bool QLinuxFbDevice::createFramebuffer(QLinuxFbDevice::Output *output, int bufferIdx)
{
    const QSize size = output->currentRes();
    const uint32_t w = size.width();
    const uint32_t h = size.height();
    drm_mode_create_dumb creq = {
        h,
        w,
        32,
        0, 0, 0, 0
    };
    if (drmIoctl(fd(), DRM_IOCTL_MODE_CREATE_DUMB, &creq) == -1) {
        qErrnoWarning(errno, "Failed to create dumb buffer");
        return false;
    }

    Framebuffer &fb(output->fb[bufferIdx]);
    fb.handle = creq.handle;
    fb.pitch = creq.pitch;
    fb.size = creq.size;
    qCDebug(qLcFbDrm, "Got a dumb buffer for size %dx%d, handle %u, pitch %u, size %u",
            w, h, fb.handle, fb.pitch, (uint) fb.size);

    if (drmModeAddFB(fd(), w, h, 24, 32, fb.pitch, fb.handle, &fb.fb) == -1) {
        qErrnoWarning(errno, "Failed to add FB");
        return false;
    }

    drm_mode_map_dumb mreq = {
        fb.handle,
        0, 0
    };
    if (drmIoctl(fd(), DRM_IOCTL_MODE_MAP_DUMB, &mreq) == -1) {
        qErrnoWarning(errno, "Failed to map dumb buffer");
        return false;
    }
    fb.p = mmap(0, fb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd(), mreq.offset);
    if (fb.p == MAP_FAILED) {
        qErrnoWarning(errno, "Failed to mmap dumb buffer");
        return false;
    }

    qCDebug(qLcFbDrm, "FB is %u, mapped at %p", fb.fb, fb.p);
    memset(fb.p, 0, fb.size);

    fb.wrapper = QImage(static_cast<uchar *>(fb.p), w, h, fb.pitch, QImage::Format_RGB32);

    return true;
}

void QLinuxFbDevice::createFramebuffers()
{
    for (Output &output : m_outputs) {
        for (int i = 0; i < BUFFER_COUNT; ++i) {
            if (!createFramebuffer(&output, i))
                return;
        }
        output.backFb = 0;
        output.flipped = true;
    }
}

void QLinuxFbDevice::destroyFramebuffer(QLinuxFbDevice::Output *output, int bufferIdx)
{
    Framebuffer &fb(output->fb[bufferIdx]);
    if (fb.p != MAP_FAILED)
        munmap(fb.p, fb.size);
    if (fb.fb) {
        if (drmModeRmFB(fd(), fb.fb) == -1)
            qErrnoWarning("Failed to remove fb");
    }
    if (fb.handle) {
        drm_mode_destroy_dumb dreq = { fb.handle };
        if (drmIoctl(fd(), DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) == -1)
            qErrnoWarning(errno, "Failed to destroy dumb buffer %u", fb.handle);
    }
    fb = Framebuffer();
}

void QLinuxFbDevice::destroyFramebuffers()
{
    for (Output &output : m_outputs) {
        for (int i = 0; i < BUFFER_COUNT; ++i)
            destroyFramebuffer(&output, i);
    }
}

void QLinuxFbDevice::setMode()
{
    for (Output &output : m_outputs) {
        drmModeModeInfo &modeInfo(output.kmsOutput.modes[output.kmsOutput.mode]);
        if (drmModeSetCrtc(fd(), output.kmsOutput.crtc_id, output.fb[0].fb, 0, 0,
                           &output.kmsOutput.connector_id, 1, &modeInfo) == -1) {
            qErrnoWarning(errno, "Failed to set mode");
            return;
        }

        output.kmsOutput.mode_set = true; // have cleanup() to restore the mode
        output.kmsOutput.setPowerState(this, QPlatformScreen::PowerStateOn);
    }
}

void QLinuxFbDevice::pageFlipHandler(int fd, unsigned int sequence,
                                     unsigned int tv_sec, unsigned int tv_usec,
                                     void *user_data)
{
    Q_UNUSED(fd);
    Q_UNUSED(tv_sec);
    Q_UNUSED(tv_usec);

    Output *output = static_cast<Output *>(user_data);
    output->flipped = true;

    unsigned int framesDropped = sequence - output->lastSequence - 1;
    if (framesDropped > 0)
        qCDebug(qLcFbDrmTiming) << "Frames dropped: " << framesDropped;            

    output->lastFramesDropped = framesDropped;
    output->lastSequence = sequence;
}

QRegion QLinuxFbDevice::drawFrameTimeBar(Output *output, qint64 frameTime)
{
    const uint32_t width = output->currentRes().width();
    int *p = (int*)(output->fb[output->backFb].p);
    
    // full width is the time it takes to display two frames
    // reason: because of triple buffering, it is possible to render a (single) frame in twice the frame time and still be in time if the
    // previous frame was very fast
    float frameTimeFraction = (float)frameTime / (2 * 1000000000 / FRAME_RATE);
    if (frameTimeFraction > 1)
        frameTimeFraction = 1;

    uint32_t len = (int)(width * frameTimeFraction);
    for (uint32_t y= 0; y<FRAME_TIME_HEIGHT; y++)
    {
        int offset = y * width;
        for (uint32_t x=0; x<len; x++)
            p[offset++] = 0xffffffff;
    }
    QRegion dirtyRegion(0, 0, len, FRAME_TIME_HEIGHT);

    if (output->lastFramesDropped > 0)
    {
        for (uint32_t y= 0; y<50; y++)
        {
            int offset = y * width + width - 50;      
            for (uint32_t x=0; x<50; x++)
                p[offset++] = 0x00ff0000;
        }
        dirtyRegion += QRect(width-50, 0, 50, 50);
    }

    return dirtyRegion;
}

void QLinuxFbDevice::swapBuffers(Output *output)
{
    // qCDebug(qLcFbDrm, "SwapBuffers enter");
    // qCDebug(qLcFbDrm, "SwapBuffers wait start");

    // qCDebug(qLcFbDrmTiming) << "SwapBuffers wait start";    

    qint64 frameTime = m_timer.nsecsElapsed() - output->lastRenderFinished;       

    while (!output->flipped) {
        drmEventContext drmEvent;
        memset(&drmEvent, 0, sizeof(drmEvent));
        drmEvent.version = 2;
        drmEvent.vblank_handler = nullptr;
        drmEvent.page_flip_handler = pageFlipHandler;
        // Blocks until there is something to read on the drm fd
        // and calls back pageFlipHandler once the flip completes.
        drmHandleEvent(fd(), &drmEvent);
    }
    
    // qCDebug(qLcFbDrmTiming) << "Frame time: " << (frameTime / 1000000);            

    output->flipped = false;

    // qCDebug(qLcFbDrm, "SwapBuffers wait finished");

    // Sleep seems to be necessary, otherwise page flips are not always executed propertly. To be investigated.
    usleep(1000);
    
    if (m_showDroppedFrames)
    {
        QRegion frameTimeRegion = drawFrameTimeBar(output, frameTime);
        output->dirty[output->backFb] += frameTimeRegion;
    }

    // schedule page flip
    Framebuffer &fb(output->fb[output->backFb]);
    if (drmModePageFlip(fd(), output->kmsOutput.crtc_id, fb.fb, DRM_MODE_PAGE_FLIP_EVENT, output) == -1) {
        qErrnoWarning(errno, "Page flip failed");
        return;
    }

    // immediately advance back buffer, because there are three buffers
    output->backFb = (output->backFb + 1) % BUFFER_COUNT;

    output->lastRenderFinished = m_timer.nsecsElapsed();
}

QLinuxFbDrmScreen::QLinuxFbDrmScreen(const QStringList &args)
    : m_screenConfig(nullptr),
      m_device(nullptr),
      m_lastPos(0, 0)
{
    Q_UNUSED(args);

    m_clearFrames = qEnvironmentVariableIntValue("QT_QPA_FB_DRM_CLEARFRAMES") != 0;
}

QLinuxFbDrmScreen::~QLinuxFbDrmScreen()
{
    if (m_device) {
        m_device->destroyFramebuffers();
        m_device->close();
        delete m_device;
    }
    delete m_screenConfig;
}

bool QLinuxFbDrmScreen::initialize()
{
    m_screenConfig = new QKmsScreenConfig;
    m_device = new QLinuxFbDevice(m_screenConfig);
    if (!m_device->open())
        return false;

    // Discover outputs. Calls back Device::createScreen().
    m_device->createScreens();
    // Now off to dumb buffer specifics.
    m_device->createFramebuffers();
    // Do the modesetting.
    m_device->setMode();

    QLinuxFbDevice::Output *output(m_device->output(0));

    mGeometry = QRect(QPoint(0, 0), output->currentRes());
    mDepth = 32;
    mFormat = QImage::Format_RGB32;
    mPhysicalSize = output->kmsOutput.physical_size;
    qCDebug(qLcFbDrm) << mGeometry << mPhysicalSize;

    QFbScreen::initializeCompositor();

    mCursor = new QFbCursor(this);

    m_timer.start();
    m_lastFrameTime = 0;
    m_lastFrameSetTime = 0;
    m_frameCounter = 0;

    return true;
}

QRegion QLinuxFbDrmScreen::doRedrawFromBackingStores(const QRegion& prevFramesDirtyRegion, QImage &destination)
{
    qCDebug(qLcFbDrm) << "prevFramesDirtyRegion" << prevFramesDirtyRegion;
    qCDebug(qLcFbDrm) << "mRepaintRegion" << mRepaintRegion;

    const QPoint screenOffset = mGeometry.topLeft();
    QRegion touchedRegion;
    
    // cursor disabled

    // if (mCursor && mCursor->isDirty() && mCursor->isOnScreen()) {
    //     const QRect lastCursor = mCursor->dirtyRect();
    //     mRepaintRegion += lastCursor;
    // }

     if (mRepaintRegion.isEmpty()) // && (!mCursor || !mCursor->isDirty()))
    {
         qCDebug(qLcFbDrm) << "empty repaint region";

         qCDebug(qLcFbDrm) << "touchedRegion" << touchedRegion;
         return touchedRegion;
    }

    QPainter mPainter(&destination);
    mPainter.setCompositionMode(QPainter::CompositionMode_Source);

    if (m_clearFrames)
    {
        mPainter.fillRect(destination.rect(), Qt::white);
    }

    touchedRegion += mRepaintRegion;
    mRepaintRegion += prevFramesDirtyRegion;

    qCDebug(qLcFbDrm) << "draw region to framebuffer" << mRepaintRegion;

    const QVector<QRect> rects = mRepaintRegion.rects();
    const QRect screenRect = mGeometry.translated(-screenOffset);
    for (int rectIndex = 0; rectIndex < mRepaintRegion.rectCount(); rectIndex++) {
        const QRect rect = rects[rectIndex].intersected(screenRect);
        if (rect.isEmpty())
            continue;

        // background clearing disabled (performance benefit)
            
        // mPainter.fillRect(rect, mScreenImage.hasAlphaChannel() ? Qt::transparent : Qt::black);

        for (int layerIndex = mWindowStack.size() - 1; layerIndex != -1; layerIndex--) {
            if (!mWindowStack[layerIndex]->window()->isVisible())
                continue;

            const QRect windowRect = mWindowStack[layerIndex]->geometry().translated(-screenOffset);
            const QRect windowIntersect = rect.translated(-windowRect.left(), -windowRect.top());
            QFbBackingStore *backingStore = mWindowStack[layerIndex]->backingStore();
            if (backingStore) {
                backingStore->lock();
                mPainter.drawImage(rect, backingStore->image(), windowIntersect);
                backingStore->unlock();
            }
        }
    }

    // cursor disabled

    // if (mCursor && (mCursor->isDirty() || mRepaintRegion.intersects(mCursor->lastPainted()))) {
    //     mPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    //     touchedRegion += mCursor->drawCursor(mPainter);
    // }
    
    mRepaintRegion = QRegion();
    
    qCDebug(qLcFbDrm) << "touchedRegion" << touchedRegion;
    return touchedRegion;
}

QRegion QLinuxFbDrmScreen::doRedraw()
{
    auto doRedrawStart = m_timer.nsecsElapsed();

    QLinuxFbDevice::Output *output(m_device->output(0));

    qCDebug(qLcFbDrm, "drawing into buffer %d", output->backFb);
    
    const QRegion dirty = doRedrawFromBackingStores(output->dirty[output->backFb], output->fb[output->backFb].wrapper);
    if (dirty.isEmpty())
        return dirty;

    // qCDebug(qLcFbDrm, "doRedraw after QFbScreen::doRedraw");

    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        QRegion newDirty = output->dirty[i] + dirty;

        if (i != output->backFb)
        {
            qCDebug(qLcFbDrm) << "Updating dirty region of buffer" << i << "from" << output->dirty[i] << "to" << newDirty;           
        }

        output->dirty[i] = newDirty;
    }

    if (output->fb[output->backFb].wrapper.isNull())
        return dirty;


    QRegion newDirtyRegion;

    // always redraw frame time bar area
    // QRect frameTimeRect(0, 0, output->currentRes().width(), FRAME_TIME_HEIGHT);
    // newDirtyRegion += QRegion(frameTimeRect);

    output->dirty[output->backFb] = newDirtyRegion;

    m_device->swapBuffers(output);

    auto thisTime = m_timer.nsecsElapsed();
    // auto frameTime = thisTime - m_lastFrameTime;
    m_lastFrameTime = thisTime;

    // qCDebug(qLcFbDrmTiming) << "Draw to framebuffer complete, realFrameDelta" << frameTime / 1000000 << "ms";           

    const int frameSetSize = 100;

    m_frameCounter++;
    if (m_frameCounter % frameSetSize == 0)
    {
        auto fps = 1000000000.0 / ((thisTime - m_lastFrameSetTime) / frameSetSize);
        qCDebug(qLcFbDrmTiming) << "FPS: " << fps;           
        m_lastFrameSetTime = thisTime;
    }

    qCDebug(qLcFbDrm) << "QLinuxFbDrmScreen::doRedraw executed in " << (m_timer.nsecsElapsed() - doRedrawStart) / 1000000 << "ms";           

    return dirty;
}

QPixmap QLinuxFbDrmScreen::grabWindow(WId wid, int x, int y, int width, int height) const
{
    Q_UNUSED(wid);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);

    return QPixmap();
}

QT_END_NAMESPACE
