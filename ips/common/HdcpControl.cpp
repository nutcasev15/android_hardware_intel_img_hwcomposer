/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#include <HwcTrace.h>
#include <DrmConfig.h>
#include <Hwcomposer.h>
#include <DisplayQuery.h>
#include <common/DrmControl.h>
#include <common/HdcpControl.h>
#include <cutils/properties.h>


namespace android {
namespace intel {

HdcpControl::HdcpControl()
    : mCallback(NULL),
      mUserData(NULL),
      mMutex(),
      mStoppedCondition(),
      mCompletedCondition(),
      mWaitForCompletion(false),
      mStopped(true),
      mAuthenticated(false),
      mActionDelay(0)
{
}

HdcpControl::~HdcpControl()
{
}

bool HdcpControl::startHdcp()
{
    if (!isHdcpSupported()) {
        WTRACE("HDCP is not supported");
        return false;
    }

    // this is a blocking and synchronous call
    Mutex::Autolock lock(mMutex);

    if (!mStopped) {
        WTRACE("HDCP has been started");
        return true;
    }

    mStopped = false;
    mAuthenticated = false;
    mWaitForCompletion = false;

    mThread = new HdcpControlThread(this);
    if (!mThread.get()) {
        ETRACE("failed to create hdcp control thread");
        return false;
    }

    if (!runHdcp()) {
        ETRACE("failed to run HDCP");
        mStopped = true;
        mThread = NULL;
        return false;
    }

    mWaitForCompletion = !mAuthenticated;
    if (mAuthenticated) {
        mActionDelay = HDCP_VERIFICATION_DELAY_MS;
    } else {
        mActionDelay = HDCP_AUTHENTICATION_DELAY_MS;
    }

    mThread->run("HdcpControl", PRIORITY_NORMAL);

    if (!mWaitForCompletion) {
        // HDCP is authenticated.
        return true;
    }
    status_t err = mCompletedCondition.waitRelative(mMutex, milliseconds(HDCP_AUTHENTICATION_TIMEOUT_MS));
    if (err == -ETIMEDOUT) {
        WTRACE("timeout waiting for completion");
    }
    mWaitForCompletion = false;
    return mAuthenticated;
}

bool HdcpControl::startHdcpAsync(HdcpStatusCallback cb, void *userData)
{
    char prop[PROPERTY_VALUE_MAX];
    if (property_get("debug.hwc.hdcp.enable", prop, "1") > 0) {
        if (atoi(prop) == 0) {
            WTRACE("HDCP is disabled");
            return false;
        }
    }

    if (cb == NULL || userData == NULL) {
        ETRACE("invalid callback or user data");
        return false;
    }

    if (!isHdcpSupported()) {
        WTRACE("HDCP is not supported");
        return false;
    }

    Mutex::Autolock lock(mMutex);

    if (!mStopped) {
        WTRACE("HDCP has been started");
        return true;
    }

    mThread = new HdcpControlThread(this);
    if (!mThread.get()) {
        ETRACE("failed to create hdcp control thread");
        return false;
    }

    mCallback = cb;
    mUserData = userData;
    mWaitForCompletion = false;
    mAuthenticated = false;
    mStopped = false;
    mActionDelay = HDCP_ASYNC_START_DELAY_MS;
    mThread->run("HdcpControl", PRIORITY_NORMAL);

    return true;
}

bool HdcpControl::stopHdcp()
{
    do {
        Mutex::Autolock lock(mMutex);
        if (mStopped) {
            return true;
        }

        mStopped = true;
        mStoppedCondition.signal();

        mAuthenticated = false;
        mWaitForCompletion = false;
        mCallback = NULL;
        mUserData = NULL;
        disableAuthentication();
    } while (0);

    if (mThread.get()) {
        mThread->requestExitAndWait();
        mThread = NULL;
    }

    return true;
}

bool HdcpControl::enableAuthentication()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    int ret = drmCommandNone(fd, DRM_PSB_ENABLE_HDCP);
    if (ret != 0) {
        ETRACE("failed to enable HDCP authentication");
        return false;
    }
    return true;
}

bool HdcpControl::disableAuthentication()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    int ret = drmCommandNone(fd, DRM_PSB_DISABLE_HDCP);
    if (ret != 0) {
        ETRACE("failed to stop disable authentication");
        return false;
    }
    return true;
}

bool HdcpControl::enableOverlay()
{
    return true;
}

bool HdcpControl::disableOverlay()
{
    return true;
}

bool HdcpControl::enableDisplayIED()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    int ret = drmCommandNone(fd, DRM_PSB_HDCP_DISPLAY_IED_ON);
    if (ret != 0) {
        ETRACE("failed to enable overlay IED");
        return false;
    }
    return true;
}

bool HdcpControl::disableDisplayIED()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    int ret = drmCommandNone(fd, DRM_PSB_HDCP_DISPLAY_IED_OFF);
    if (ret != 0) {
        ETRACE("failed to disable overlay IED");
        return false;
    }
    return true;
}

bool HdcpControl::isHdcpSupported()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    unsigned int caps = 0;
    int ret = drmCommandRead(fd, DRM_PSB_QUERY_HDCP, &caps, sizeof(caps));
    if (ret != 0) {
        ETRACE("failed to query HDCP capability");
        return false;
    }
    if (caps == 0) {
        WTRACE("HDCP is not supported");
        return false;
    } else {
        ITRACE("HDCP is supported");
        return true;
    }
}

bool HdcpControl::checkAuthenticated()
{
    int fd = Hwcomposer::getInstance().getDrm()->getDrmFd();
    unsigned int match = 0;
    int ret = drmCommandRead(fd, DRM_PSB_GET_HDCP_LINK_STATUS, &match, sizeof(match));
    if (ret != 0) {
        ETRACE("failed to get hdcp link status");
        return false;
    }
    if (match) {
        VTRACE("HDCP is authenticated");
        mAuthenticated = true;
    } else {
        ETRACE("HDCP is not authenticated");
        mAuthenticated = false;
    }
    return mAuthenticated;
}

bool HdcpControl::runHdcp()
{
    // Default return value is true so HDCP can be re-authenticated in the working thread
    bool ret = true;

    preRunHdcp();

    for (int i = 0; i < HDCP_INLOOP_RETRY_NUMBER; i++) {
        VTRACE("enable and verify HDCP, iteration# %d", i);
        if (mStopped) {
            WTRACE("HDCP authentication has been stopped");
            ret = false;
            break;
        }

        if (!enableAuthentication()) {
            ret = false;
            break;
        }

        if (checkAuthenticated()) {
            ITRACE("HDCP is authenticated");
            ret = true;
            break;
        }

        if (mStopped) {
            WTRACE("HDCP authentication has been stopped");
            ret = false;
            break;
        }

        // Adding delay to make sure panel receives video signal so it can start HDCP authentication.
        // (HDCP spec 1.3, section 2.3)
        usleep(HDCP_INLOOP_RETRY_DELAY_US);
    }

    postRunHdcp();

    return ret;
}

bool HdcpControl::preRunHdcp()
{
    // TODO: for CTP platform, IED needs to be disabled during HDCP authentication.
    return true;
}

bool HdcpControl::postRunHdcp()
{
    // TODO: for CTP platform, IED needs to be disabled during HDCP authentication.
    return true;
}


void HdcpControl::signalCompletion()
{
    if (mWaitForCompletion) {
        ITRACE("signal HDCP authentication completed, status = %d", mAuthenticated);
        mCompletedCondition.signal();
        mWaitForCompletion = false;
    }
}

bool HdcpControl::threadLoop()
{
    Mutex::Autolock lock(mMutex);
    status_t err = mStoppedCondition.waitRelative(mMutex, milliseconds(mActionDelay));
    if (err != -ETIMEDOUT) {
        ITRACE("Hdcp is stopped.");
        signalCompletion();
        return false;
    }

    // default is to keep thread active
    bool ret = true;
    if (!mAuthenticated) {
        ret = runHdcp();
    } else {
        checkAuthenticated();
    }

    // set next action delay
    if (mAuthenticated) {
        mActionDelay = HDCP_VERIFICATION_DELAY_MS;
    } else {
        mActionDelay = HDCP_AUTHENTICATION_DELAY_MS;
    }

    // TODO: move out of lock?
    if (!ret || mAuthenticated) {
        signalCompletion();
    }

    if (mCallback) {
        (*mCallback)(mAuthenticated, mUserData);
    }
    return ret;
}


} // namespace intel
} // namespace android
