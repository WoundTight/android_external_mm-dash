/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Not a Contribution, Apache license notifications and license are retained
 * for attribution purposes only.
 *
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "DashCodec"

#include "DashCodec.h"
#include "QCMediaDefs.h"

#include <binder/MemoryDealer.h>

#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NativeWindowWrapper.h>
#include <media/stagefright/OMXClient.h>
#include <media/stagefright/OMXCodec.h>

#include <media/hardware/HardwareAPI.h>
#include <OMX_QCOMExtns.h>
#include <OMX_Component.h>
#include <cutils/properties.h>
#include "avc_utils.h"

#ifdef BFAMILY_TARGET /* Venus macros and dynamic mode support present only for B-family targets.*/
#include <media/msm_media_info.h>
#endif

//Smmoth streaming settings
//Max resolution 1080p
#define MAX_WIDTH 1920;
#define MAX_HEIGHT 1080;

//Min resolution QVGA
#define MIN_WIDTH 480;
#define MIN_HEIGHT 320;

#define DC_MSG_ERROR(...) ALOGE(__VA_ARGS__)
#define DC_MSG_HIGH(...) if(mLogLevel >= 1){ALOGE(__VA_ARGS__);}
#define DC_MSG_MEDIUM(...) if(mLogLevel >= 2){ALOGE(__VA_ARGS__);}
#define DC_MSG_LOW(...) if(mLogLevel >= 3){ALOGE(__VA_ARGS__);}

int mLogLevel = 0;

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

struct CodecObserver : public BnOMXObserver {
    CodecObserver() {}

    void setNotificationMessage(const sp<AMessage> &msg) {
        mNotify = msg;
    }

    // from IOMXObserver
    virtual void onMessage(const omx_message &omx_msg) {
        sp<AMessage> msg = mNotify->dup();

        msg->setInt32("type", omx_msg.type);
        msg->setInt32("node", omx_msg.node);

        switch (omx_msg.type) {
            case omx_message::EVENT:
            {
                msg->setInt32("event", omx_msg.u.event_data.event);
                msg->setInt32("data1", omx_msg.u.event_data.data1);
                msg->setInt32("data2", omx_msg.u.event_data.data2);
                break;
            }

            case omx_message::EMPTY_BUFFER_DONE:
            {
                msg->setInt32("buffer", omx_msg.u.buffer_data.buffer);
                break;
            }

            case omx_message::FILL_BUFFER_DONE:
            {
                msg->setInt32(
                        "buffer", omx_msg.u.extended_buffer_data.buffer);
                msg->setInt32(
                        "range_offset",
                        omx_msg.u.extended_buffer_data.range_offset);
                msg->setInt32(
                        "range_length",
                        omx_msg.u.extended_buffer_data.range_length);
                msg->setInt32(
                        "flags",
                        omx_msg.u.extended_buffer_data.flags);
                msg->setInt64(
                        "timestamp",
                        omx_msg.u.extended_buffer_data.timestamp);
                //msg->setPointer(
                //        "platform_private",
                //        omx_msg.u.extended_buffer_data.platform_private);
                //msg->setPointer(
                //        "data_ptr",
                //        omx_msg.u.extended_buffer_data.data_ptr);
                break;
            }

            default:
                TRESPASS();
                break;
        }

        msg->post();
    }

protected:
    virtual ~CodecObserver() {}

private:
    sp<AMessage> mNotify;

    DISALLOW_EVIL_CONSTRUCTORS(CodecObserver);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::BaseState : public AState {
    BaseState(DashCodec *codec, const sp<AState> &parentState = NULL);

protected:
    enum PortMode {
        KEEP_BUFFERS,
        RESUBMIT_BUFFERS,
        FREE_BUFFERS,
    };

    DashCodec *mCodec;

    virtual PortMode getPortMode(OMX_U32 portIndex);

    virtual bool onMessageReceived(const sp<AMessage> &msg);

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

    void postFillThisBuffer(BufferInfo *info);

private:
    bool onOMXMessage(const sp<AMessage> &msg);

    bool onOMXEmptyBufferDone(IOMX::buffer_id bufferID);

    bool onOMXFillBufferDone(
            IOMX::buffer_id bufferID,
            size_t rangeOffset, size_t rangeLength,
            OMX_U32 flags,
            int64_t timeUs,
            void *platformPrivate,
            void *dataPtr);

    void getMoreInputDataIfPossible();

    DISALLOW_EVIL_CONSTRUCTORS(BaseState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::UninitializedState : public DashCodec::BaseState {
    UninitializedState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

private:
    void onSetup(const sp<AMessage> &msg);
    bool onAllocateComponent(const sp<AMessage> &msg);

    DISALLOW_EVIL_CONSTRUCTORS(UninitializedState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::LoadedState : public DashCodec::BaseState {
    LoadedState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

private:
    friend struct DashCodec::UninitializedState;

    bool onConfigureComponent(const sp<AMessage> &msg);
    void onStart();
    void onShutdown(bool keepComponentAllocated);

    DISALLOW_EVIL_CONSTRUCTORS(LoadedState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::LoadedToIdleState : public DashCodec::BaseState {
    LoadedToIdleState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual void stateEntered();

private:
    status_t allocateBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(LoadedToIdleState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::IdleToExecutingState : public DashCodec::BaseState {
    IdleToExecutingState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);
    virtual void stateEntered();

private:
    DISALLOW_EVIL_CONSTRUCTORS(IdleToExecutingState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::ExecutingState : public DashCodec::BaseState {
    ExecutingState(DashCodec *codec);

    void submitRegularOutputBuffers();
    void submitOutputMetaBuffers();
    void submitOutputBuffers();

    // Submit output buffers to the decoder, submit input buffers to client
    // to fill with data.
    void resume();

    // Returns true iff input and output buffers are in play.
    bool active() const { return mActive; }

protected:
    virtual PortMode getPortMode(OMX_U32 portIndex);
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

private:
    bool mActive;

    DISALLOW_EVIL_CONSTRUCTORS(ExecutingState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::OutputPortSettingsChangedState : public DashCodec::BaseState {
    OutputPortSettingsChangedState(DashCodec *codec);

protected:
    virtual PortMode getPortMode(OMX_U32 portIndex);
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void enableOutputPort(OMX_U32 data1, OMX_U32 data2);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

private:
    DISALLOW_EVIL_CONSTRUCTORS(OutputPortSettingsChangedState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::ExecutingToIdleState : public DashCodec::BaseState {
    ExecutingToIdleState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

private:
    void changeStateIfWeOwnAllBuffers();

    bool mComponentNowIdle;

    DISALLOW_EVIL_CONSTRUCTORS(ExecutingToIdleState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::IdleToLoadedState : public DashCodec::BaseState {
    IdleToLoadedState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

private:
    DISALLOW_EVIL_CONSTRUCTORS(IdleToLoadedState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::FlushingState : public DashCodec::BaseState {
    FlushingState(DashCodec *codec);

protected:
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

private:
    bool mFlushComplete[2];

    void changeStateIfWeOwnAllBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(FlushingState);
};

////////////////////////////////////////////////////////////////////////////////

struct DashCodec::FlushingOutputState : public DashCodec::BaseState {
    FlushingOutputState(DashCodec *codec);

protected:
    virtual PortMode getPortMode(OMX_U32 portIndex);
    virtual bool onMessageReceived(const sp<AMessage> &msg);
    virtual void stateEntered();

    virtual bool onOMXEvent(OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2);

    virtual void onOutputBufferDrained(const sp<AMessage> &msg);
    virtual void onInputBufferFilled(const sp<AMessage> &msg);

private:
    bool mFlushComplete;

    void changeStateIfWeOwnAllBuffers();

    DISALLOW_EVIL_CONSTRUCTORS(FlushingOutputState);
};

////////////////////////////////////////////////////////////////////////////////

DashCodec::DashCodec()
    : mQuirks(0),
      mNode(0),
      mSentFormat(false),
      mPostFormat(false),
      mIsEncoder(false),
      mShutdownInProgress(false),
      mEncoderDelay(0),
      mEncoderPadding(0),
      mChannelMaskPresent(false),
      mChannelMask(0),
      mDequeueCounter(0),
      mStoreMetaDataInOutputBuffers(false),
      mMetaDataBuffersToSubmit(0),
      mCurrentWidth(0),
      mCurrentHeight(0),
      mAdaptivePlayback(false) {
    mUninitializedState = new UninitializedState(this);
    mLoadedState = new LoadedState(this);
    mLoadedToIdleState = new LoadedToIdleState(this);
    mIdleToExecutingState = new IdleToExecutingState(this);
    mExecutingState = new ExecutingState(this);

    mOutputPortSettingsChangedState =
        new OutputPortSettingsChangedState(this);

    mExecutingToIdleState = new ExecutingToIdleState(this);
    mIdleToLoadedState = new IdleToLoadedState(this);
    mFlushingState = new FlushingState(this);
    mFlushingOutputState = new FlushingOutputState(this);

    mPortEOS[kPortIndexInput] = mPortEOS[kPortIndexOutput] = false;
    mInputEOSResult = OK;

    changeState(mUninitializedState);

    char property_value[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.dash.debug.level", property_value, NULL);

    if(*property_value) {
        mLogLevel = atoi(property_value);
    }
}

DashCodec::~DashCodec() {
  clearCachedFormats();
}

void DashCodec::setNotificationMessage(const sp<AMessage> &msg) {
    mNotify = msg;
}

void DashCodec::initiateSetup(const sp<AMessage> &msg) {
    msg->setWhat(kWhatSetup);
    msg->setTarget(id());
    msg->post();
}

void DashCodec::initiateAllocateComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatAllocateComponent);
    msg->setTarget(id());
    msg->post();
}

void DashCodec::initiateConfigureComponent(const sp<AMessage> &msg) {
    msg->setWhat(kWhatConfigureComponent);
    msg->setTarget(id());
    msg->post();
}

void DashCodec::initiateStart() {
    (new AMessage(kWhatStart, id()))->post();
}

void DashCodec::signalFlush() {
    DC_MSG_LOW("[%s] signalFlush", mComponentName.c_str());
    (new AMessage(kWhatFlush, id()))->post();
}

void DashCodec::signalResume() {
    (new AMessage(kWhatResume, id()))->post();
}

void DashCodec::initiateShutdown(bool keepComponentAllocated) {
    sp<AMessage> msg = new AMessage(kWhatShutdown, id());
    msg->setInt32("keepComponentAllocated", keepComponentAllocated);
    msg->post();
}

void DashCodec::signalRequestIDRFrame() {
    (new AMessage(kWhatRequestIDRFrame, id()))->post();
}

status_t DashCodec::allocateBuffersOnPort(OMX_U32 portIndex) {
    CHECK(portIndex == kPortIndexInput || portIndex == kPortIndexOutput);

    CHECK(mDealer[portIndex] == NULL);
    CHECK(mBuffers[portIndex].isEmpty());

    status_t err;
    if (mNativeWindow != NULL && portIndex == kPortIndexOutput) {
        if (mStoreMetaDataInOutputBuffers) {
            err = allocateOutputMetaDataBuffers();
        } else {
        err = allocateOutputBuffersFromNativeWindow();
        }
    } else {
        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = portIndex;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err == OK) {
            DC_MSG_HIGH("[%s] Allocating %lu buffers of size %lu on %s port",
                    mComponentName.c_str(),
                    def.nBufferCountActual, def.nBufferSize,
                    portIndex == kPortIndexInput ? "input" : "output");

            size_t totalSize = def.nBufferCountActual * def.nBufferSize;
            mDealer[portIndex] = new MemoryDealer(totalSize, "DashCodec");

            for (OMX_U32 i = 0; i < def.nBufferCountActual; ++i) {
                sp<IMemory> mem = mDealer[portIndex]->allocate(def.nBufferSize);
                CHECK(mem.get() != NULL);

                BufferInfo info;
                info.mStatus = BufferInfo::OWNED_BY_US;

                uint32_t requiresAllocateBufferBit =
                    (portIndex == kPortIndexInput)
                        ? OMXCodec::kRequiresAllocateBufferOnInputPorts
                        : OMXCodec::kRequiresAllocateBufferOnOutputPorts;

                if (portIndex == kPortIndexInput && (mFlags & kFlagIsSecure)) {
                    mem.clear();

                    void *ptr;
                    err = mOMX->allocateBuffer(
                            mNode, portIndex, def.nBufferSize, &info.mBufferID,
                            &ptr);

                    info.mData = new ABuffer(ptr, def.nBufferSize);
                } else if (mQuirks & requiresAllocateBufferBit) {
                    err = mOMX->allocateBufferWithBackup(
                            mNode, portIndex, mem, &info.mBufferID);
                } else {
                    err = mOMX->useBuffer(mNode, portIndex, mem, &info.mBufferID);
                }

                if (mem != NULL) {
                    info.mData = new ABuffer(mem->pointer(), def.nBufferSize);
                }

                mBuffers[portIndex].push(info);
            }
        }
    }

    if (err != OK) {
        return err;
    }

    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", DashCodec::kWhatBuffersAllocated);

    notify->setInt32("portIndex", portIndex);

    sp<PortDescription> desc = new PortDescription;

    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex][i];

        desc->addBuffer(info.mBufferID, info.mData);
    }

    notify->setObject("portDesc", desc);
    notify->post();

    return OK;
}

status_t DashCodec::configureOutputBuffersFromNativeWindow(
OMX_U32 *bufferCount, OMX_U32 *bufferSize,
        OMX_U32 *minUndequeuedBuffers) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    err = native_window_set_buffers_dimensions(
            mNativeWindow.get(),
            def.format.video.nFrameWidth,
            def.format.video.nFrameHeight);

    if (err != 0) {
        DC_MSG_ERROR("native_window_set_buffers_dimensions failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_set_buffers_format(
            mNativeWindow.get(),
            def.format.video.eColorFormat);

    if (err != 0) {
        DC_MSG_ERROR("native_window_set_buffers_format failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    // Set up the native window.
    OMX_U32 usage = 0;
    err = mOMX->getGraphicBufferUsage(mNode, kPortIndexOutput, &usage);
    if (err != 0) {
        DC_MSG_HIGH("querying usage flags from OMX IL component failed: %d", err);
        // XXX: Currently this error is logged, but not fatal.
        usage = 0;
    }

    if (mFlags & kFlagIsSecure || mFlags & kFlagIsSecureOPOnly) {
        usage |= GRALLOC_USAGE_PROTECTED;
    }

    // Make sure to check whether either Stagefright or the video decoder
    // requested protected buffers.
    if (usage & GRALLOC_USAGE_PROTECTED) {
        // Verify that the ANativeWindow sends images directly to
        // SurfaceFlinger.
        int queuesToNativeWindow = 0;
        err = mNativeWindow->query(
                mNativeWindow.get(), NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER,
                &queuesToNativeWindow);
        if (err != 0) {
            DC_MSG_ERROR("error authenticating native window: %d", err);
            return err;
        }
        if (queuesToNativeWindow != 1) {
            DC_MSG_ERROR("native window could not be authenticated");
            return PERMISSION_DENIED;
        }
    }

    err = native_window_set_usage(
            mNativeWindow.get(),
            usage | GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);

    if (err != 0) {
        DC_MSG_ERROR("native_window_set_usage failed: %s (%d)", strerror(-err), -err);
        return err;
    }

    *minUndequeuedBuffers = 0;
    err = mNativeWindow->query(
            mNativeWindow.get(), NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
            (int *)minUndequeuedBuffers);

    if (err != 0) {
        DC_MSG_ERROR("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    //add an extra buffer to display queue to get around dequeue+wait
    //blocking too long (more than 1 Vsync) in case BufferQeuue is in
    //sync-mode and advertizes only 1 buffer
    (*minUndequeuedBuffers)++;
    DC_MSG_ERROR("NOTE: Overriding minUndequeuedBuffers to %lu",*minUndequeuedBuffers);

    // XXX: Is this the right logic to use?  It's not clear to me what the OMX
    // buffer counts refer to - how do they account for the renderer holding on
    // to buffers?
    if (def.nBufferCountActual < def.nBufferCountMin + *minUndequeuedBuffers) {
        OMX_U32 newBufferCount = def.nBufferCountMin + *minUndequeuedBuffers;
        def.nBufferCountActual = newBufferCount;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            DC_MSG_ERROR("[%s] setting nBufferCountActual to %lu failed: %d",
                    mComponentName.c_str(), newBufferCount, err);
            return err;
        }
    }


    err = native_window_set_buffer_count(
            mNativeWindow.get(), def.nBufferCountActual);

    if (err != 0) {
        DC_MSG_ERROR("native_window_set_buffer_count failed: %s (%d)", strerror(-err),
                -err);
        return err;
    }

    *bufferCount = def.nBufferCountActual;
    *bufferSize =  def.nBufferSize;
    return err;
}

status_t DashCodec::allocateOutputBuffersFromNativeWindow() {
    OMX_U32 bufferCount, bufferSize, minUndequeuedBuffers;
    status_t err = configureOutputBuffersFromNativeWindow(
            &bufferCount, &bufferSize, &minUndequeuedBuffers);
    if (err != 0)
        return err;

    DC_MSG_HIGH("[%s] Allocating %lu buffers from a native window of size %lu on "
         "output port",
         mComponentName.c_str(), bufferCount, bufferSize);

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        ANativeWindowBuffer *buf;
        err = native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf);
        if (err != 0) {
            DC_MSG_ERROR("dequeueBuffer failed: %s (%d)", strerror(-err), -err);
            break;
        }

        sp<GraphicBuffer> graphicBuffer(new GraphicBuffer(buf, false));
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_US;
        info.mData = new ABuffer(NULL /* data */, bufferSize /* capacity */);
        info.mGraphicBuffer = graphicBuffer;
        mBuffers[kPortIndexOutput].push(info);

        IOMX::buffer_id bufferId;
        err = mOMX->useGraphicBuffer(mNode, kPortIndexOutput, graphicBuffer,
                &bufferId);
        if (err != 0) {
            DC_MSG_ERROR("registering GraphicBuffer %lu with OMX IL component failed: "
                 "%d", i, err);
            break;
        }

        mBuffers[kPortIndexOutput].editItemAt(i).mBufferID = bufferId;

        DC_MSG_LOW("[%s] Registered graphic buffer with ID %p (pointer = %p)",
             mComponentName.c_str(),
             bufferId, graphicBuffer.get());
    }

    OMX_U32 cancelStart;
    OMX_U32 cancelEnd;

    if (err != 0) {
        // If an error occurred while dequeuing we need to cancel any buffers
        // that were dequeued.
        cancelStart = 0;
        cancelEnd = mBuffers[kPortIndexOutput].size();
    } else {
        // Return the required minimum undequeued buffers to the native window.
        cancelStart = bufferCount - minUndequeuedBuffers;
        cancelEnd = bufferCount;
    }

    for (OMX_U32 i = cancelStart; i < cancelEnd; i++) {
        BufferInfo *info = &mBuffers[kPortIndexOutput].editItemAt(i);
        cancelBufferToNativeWindow(info);
    }

    return err;
}

status_t DashCodec::allocateOutputMetaDataBuffers() {
    OMX_U32 bufferCount, bufferSize, minUndequeuedBuffers;
    status_t err = configureOutputBuffersFromNativeWindow(
            &bufferCount, &bufferSize, &minUndequeuedBuffers);
    if (err != 0)
        return err;

    DC_MSG_HIGH("[%s] Allocating %lu meta buffers on output port",
         mComponentName.c_str(), bufferCount);

    size_t totalSize = bufferCount * 8;
    mDealer[kPortIndexOutput] = new MemoryDealer(totalSize, "DashCodec");

    // Dequeue buffers and send them to OMX
    for (OMX_U32 i = 0; i < bufferCount; i++) {
        BufferInfo info;
        info.mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        info.mGraphicBuffer = NULL;
        info.mDequeuedAt = mDequeueCounter;

        sp<IMemory> mem = mDealer[kPortIndexOutput]->allocate(
                sizeof(struct VideoDecoderOutputMetaData));
        CHECK(mem.get() != NULL);
        info.mData = new ABuffer(mem->pointer(), mem->size());

        // we use useBuffer for metadata regardless of quirks
        err = mOMX->useBuffer(
                mNode, kPortIndexOutput, mem, &info.mBufferID);

        mBuffers[kPortIndexOutput].push(info);

        DC_MSG_HIGH("[%s] allocated meta buffer with ID %p (pointer = %p)",
             mComponentName.c_str(), info.mBufferID, mem->pointer());
    }

    mMetaDataBuffersToSubmit = bufferCount - minUndequeuedBuffers;
    return err;
}

status_t DashCodec::submitOutputMetaDataBuffer() {
    CHECK(mStoreMetaDataInOutputBuffers);
    if (mMetaDataBuffersToSubmit == 0)
        return OK;

    BufferInfo *info = dequeueBufferFromNativeWindow();
    if (info == NULL)
        return ERROR_IO;

    DC_MSG_HIGH("[%s] submitting output meta buffer ID %p for graphic buffer %p",
          mComponentName.c_str(), info->mBufferID, info->mGraphicBuffer.get());

    --mMetaDataBuffersToSubmit;
    CHECK_EQ(mOMX->fillBuffer(mNode, info->mBufferID),
             (status_t)OK);

    info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    return OK;
}

status_t DashCodec::cancelBufferToNativeWindow(BufferInfo *info) {
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    DC_MSG_LOW("[%s] Calling cancelBuffer on buffer %p",
         mComponentName.c_str(), info->mBufferID);

    int err = mNativeWindow->cancelBuffer(
        mNativeWindow.get(), info->mGraphicBuffer.get(), -1);

    CHECK_EQ(err, 0);

    info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;

    return OK;
}

DashCodec::BufferInfo *DashCodec::dequeueBufferFromNativeWindow() {
    ANativeWindowBuffer *buf;
    int fenceFd = -1;
    CHECK(mNativeWindow.get() != NULL);
    if (native_window_dequeue_buffer_and_wait(mNativeWindow.get(), &buf) != 0) {
        DC_MSG_ERROR("dequeueBuffer failed.");
        return NULL;
    }

    BufferInfo *oldest = NULL;
    for (size_t i = mBuffers[kPortIndexOutput].size(); i-- > 0;) {
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);

        if (info->mGraphicBuffer != NULL &&
            info->mGraphicBuffer->handle == buf->handle) {
            CHECK_EQ((int)info->mStatus,
                     (int)BufferInfo::OWNED_BY_NATIVE_WINDOW);

            info->mStatus = BufferInfo::OWNED_BY_US;

            return info;
        }

        if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW &&
            (oldest == NULL ||
             // avoid potential issues from counter rolling over
             mDequeueCounter - info->mDequeuedAt >
                    mDequeueCounter - oldest->mDequeuedAt)) {
            oldest = info;
        }
    }

    if (oldest) {
        CHECK(mStoreMetaDataInOutputBuffers);

        // discard buffer in LRU info and replace with new buffer
        oldest->mGraphicBuffer = new GraphicBuffer(buf, false);
        oldest->mStatus = BufferInfo::OWNED_BY_US;

        mOMX->updateGraphicBufferInMeta(
                  mNode, kPortIndexOutput, oldest->mGraphicBuffer,
                  oldest->mBufferID);
        VideoDecoderOutputMetaData *metaData =
                      reinterpret_cast<VideoDecoderOutputMetaData *>(
                      oldest->mData->base());
        CHECK_EQ(metaData->eType, kMetadataBufferTypeGrallocSource);

        DC_MSG_HIGH("replaced oldest buffer #%u with age %u (%p/%p stored in %p)",
                oldest - &mBuffers[kPortIndexOutput][0],
                mDequeueCounter - oldest->mDequeuedAt,
                metaData->pHandle,
                oldest->mGraphicBuffer->handle, oldest->mData->base());

        return oldest;
    }

    TRESPASS();

    return NULL;
}

status_t DashCodec::freeBuffersOnPort(OMX_U32 portIndex) {
    for (size_t i = mBuffers[portIndex].size(); i-- > 0;) {
        CHECK_EQ((status_t)OK, freeBuffer(portIndex, i));
    }

    mDealer[portIndex].clear();

    return OK;
}

status_t DashCodec::freeOutputBuffersNotOwnedByComponent() {
    for (size_t i = mBuffers[kPortIndexOutput].size(); i-- > 0;) {
        BufferInfo *info =
            &mBuffers[kPortIndexOutput].editItemAt(i);

        // At this time some buffers may still be with the component
        // or being drained.
        if (info->mStatus != BufferInfo::OWNED_BY_COMPONENT &&
            info->mStatus != BufferInfo::OWNED_BY_DOWNSTREAM) {
            CHECK_EQ((status_t)OK, freeBuffer(kPortIndexOutput, i));
        }
    }

    return OK;
}

status_t DashCodec::freeBuffer(OMX_U32 portIndex, size_t i) {
    BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

    CHECK(info->mStatus == BufferInfo::OWNED_BY_US
            || info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW);

    if (portIndex == kPortIndexOutput && mNativeWindow != NULL
            && info->mStatus == BufferInfo::OWNED_BY_US) {
        CHECK_EQ((status_t)OK, cancelBufferToNativeWindow(info));
    }

    CHECK_EQ(mOMX->freeBuffer(
                mNode, portIndex, info->mBufferID),
             (status_t)OK);

    mBuffers[portIndex].removeAt(i);

    return OK;
}

DashCodec::BufferInfo *DashCodec::findBufferByID(
        uint32_t portIndex, IOMX::buffer_id bufferID,
        ssize_t *index) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mBufferID == bufferID) {
            if (index != NULL) {
                *index = i;
            }
            return info;
        }
    }

    TRESPASS();

    return NULL;
}

status_t DashCodec::setComponentRole(
        bool isEncoder, const char *mime) {
    struct MimeToRole {
        const char *mime;
        const char *decoderRole;
        const char *encoderRole;
    };

    static const MimeToRole kMimeToRole[] = {
        { MEDIA_MIMETYPE_AUDIO_MPEG,
            "audio_decoder.mp3", "audio_encoder.mp3" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I,
            "audio_decoder.mp1", "audio_encoder.mp1" },
        { MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II,
            "audio_decoder.mp2", "audio_encoder.mp2" },
        { MEDIA_MIMETYPE_AUDIO_AMR_NB,
            "audio_decoder.amrnb", "audio_encoder.amrnb" },
        { MEDIA_MIMETYPE_AUDIO_AMR_WB,
            "audio_decoder.amrwb", "audio_encoder.amrwb" },
        // commenting out AMR_WM_PLUS for bringing up dash on MR2
        /* { MEDIA_MIMETYPE_AUDIO_AMR_WB_PLUS,
            "audio_decoder.amrwbplus", "audio_encoder.amrwbplus" }, */
        { MEDIA_MIMETYPE_AUDIO_AAC,
            "audio_decoder.aac", "audio_encoder.aac" },
        { MEDIA_MIMETYPE_AUDIO_VORBIS,
            "audio_decoder.vorbis", "audio_encoder.vorbis" },
        { MEDIA_MIMETYPE_AUDIO_G711_MLAW,
            "audio_decoder.g711mlaw", "audio_encoder.g711mlaw" },
        { MEDIA_MIMETYPE_AUDIO_G711_ALAW,
            "audio_decoder.g711alaw", "audio_encoder.g711alaw" },
        { MEDIA_MIMETYPE_VIDEO_AVC,
            "video_decoder.avc", "video_encoder.avc" },
        { MEDIA_MIMETYPE_VIDEO_MPEG4,
            "video_decoder.mpeg4", "video_encoder.mpeg4" },
        { MEDIA_MIMETYPE_VIDEO_H263,
            "video_decoder.h263", "video_encoder.h263" },
        { MEDIA_MIMETYPE_AUDIO_RAW,
            "audio_decoder.raw", "audio_encoder.raw" },
        { MEDIA_MIMETYPE_AUDIO_FLAC,
            "audio_decoder.flac", "audio_encoder.flac" },
        { MEDIA_MIMETYPE_VIDEO_HEVC,
            "video_decoder.hevc", "video_encoder.hevc" },
    };

    static const size_t kNumMimeToRole =
        sizeof(kMimeToRole) / sizeof(kMimeToRole[0]);

    size_t i;
    for (i = 0; i < kNumMimeToRole; ++i) {
        if (!strcasecmp(mime, kMimeToRole[i].mime)) {
            break;
        }
    }

    if (i == kNumMimeToRole) {
        return ERROR_UNSUPPORTED;
    }

    const char *role =
        isEncoder ? kMimeToRole[i].encoderRole
                  : kMimeToRole[i].decoderRole;

    if (role != NULL) {
        OMX_PARAM_COMPONENTROLETYPE roleParams;
        InitOMXParams(&roleParams);

        strncpy((char *)roleParams.cRole,
                role, OMX_MAX_STRINGNAME_SIZE - 1);

        roleParams.cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';

        status_t err = mOMX->setParameter(
                mNode, OMX_IndexParamStandardComponentRole,
                &roleParams, sizeof(roleParams));

        if (err != OK) {
            DC_MSG_HIGH("[%s] Failed to set standard component role '%s'.",
                 mComponentName.c_str(), role);

            return err;
        }
    }

    return OK;
}

status_t DashCodec::configureCodec(
        const char *mime, const sp<AMessage> &msg) {
    int32_t encoder;
    if (!msg->findInt32("encoder", &encoder)) {
        encoder = false;
    }

    mIsEncoder = encoder;

    status_t err = setComponentRole(encoder /* isEncoder */, mime);

    if (err != OK) {
        return err;
    }

    int32_t bitRate = 0;
    // FLAC encoder doesn't need a bitrate, other encoders do
    if (encoder && strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)
            && !msg->findInt32("bitrate", &bitRate)) {
        return INVALID_OPERATION;
    }

    int32_t storeMeta;
    if (encoder
            && msg->findInt32("store-metadata-in-buffers", &storeMeta)
            && storeMeta != 0) {
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexInput, OMX_TRUE);

        if (err != OK) {
            DC_MSG_ERROR("[%s] storeMetaDataInBuffers failed w/ err %d",
                  mComponentName.c_str(), err);

            return err;
        }
        else
        {
          DC_MSG_ERROR("[%s] storeMetaDataInBuffers succedded", mComponentName.c_str());
        }
    }

    int32_t prependSPSPPS;
    if (encoder
            && msg->findInt32("prepend-sps-pps-to-idr-frames", &prependSPSPPS)
            && prependSPSPPS != 0) {
        OMX_INDEXTYPE index;
        err = mOMX->getExtensionIndex(
                mNode,
                "OMX.google.android.index.prependSPSPPSToIDRFrames",
                &index);

        if (err == OK) {
            PrependSPSPPSToIDRFramesParams params;
            InitOMXParams(&params);
            params.bEnable = OMX_TRUE;

            err = mOMX->setParameter(
                    mNode, index, &params, sizeof(params));
        }

        if (err != OK) {
            DC_MSG_ERROR("Encoder could not be configured to emit SPS/PPS before "
                  "IDR frames. (err %d)", err);

            return err;
        }
    }

    // Always try to enable dynamic output buffers on native surface
    int32_t video = !strncasecmp(mime, "video/", 6);
      sp<RefBase> obj;
      int32_t haveNativeWindow = msg->findObject("native-window", &obj) &&
            obj != NULL;
    mStoreMetaDataInOutputBuffers = false;
      if (!encoder && video && haveNativeWindow) {
        err = mOMX->storeMetaDataInBuffers(mNode, kPortIndexOutput, OMX_TRUE);
        if (err != OK) {

            DC_MSG_ERROR("[%s] storeMetaDataInBuffers failed w/ err %d",
                  mComponentName.c_str(), err);

            // if adaptive playback has been requested, try JB fallback
            // NOTE: THIS FALLBACK MECHANISM WILL BE REMOVED DUE TO ITS
            // LARGE MEMORY REQUIREMENT

            // we will not do adaptive playback on software accessed
            // surfaces as they never had to respond to changes in the
            // crop window, and we don't trust that they will be able to.
            int usageBits = 0;

            sp<NativeWindowWrapper> windowWrapper(
                    static_cast<NativeWindowWrapper *>(obj.get()));
            sp<ANativeWindow> nativeWindow = windowWrapper->getNativeWindow();

            if (nativeWindow->query(
                    nativeWindow.get(),
                    NATIVE_WINDOW_CONSUMER_USAGE_BITS,
                    &usageBits) != OK) {
            } else {
                mAdaptivePlayback =
                    (usageBits &
                            (GRALLOC_USAGE_SW_READ_MASK |
                             GRALLOC_USAGE_SW_WRITE_MASK)) == 0;
            }
            int32_t maxWidth = MAX_WIDTH;
            int32_t maxHeight = MAX_HEIGHT;
            if (mAdaptivePlayback) {
                ALOGV("[%s] prepareForAdaptivePlayback(%ldx%ld)",
                      mComponentName.c_str(), maxWidth, maxHeight);

                err = mOMX->prepareForAdaptivePlayback(
                        mNode, kPortIndexOutput, OMX_TRUE, maxWidth, maxHeight);
                if (err != OK)
                {
                  ALOGE("[%s] prepareForAdaptivePlayback failed w/ err %d",
                         mComponentName.c_str(), err);
                }
                else
                {
                  ALOGV("[%s] prepareForAdaptivePlayback : Success",
                        mComponentName.c_str(), err);
                }
            }
            // allow failure
            err = OK;
        } else {
            DC_MSG_MEDIUM("[%s] storeMetaDataInBuffers succeeded", mComponentName.c_str());
            mStoreMetaDataInOutputBuffers = true;
        }

        int32_t push;
        if (msg->findInt32("push-blank-buffers-on-shutdown", &push)
                && push != 0) {
            mFlags |= kFlagPushBlankBuffersToNativeWindowOnShutdown;
        }
      }
    if (!strncasecmp(mime, "video/", 6)) {
        if (encoder) {
            err = setupVideoEncoder(mime, msg);
        } else {
            int32_t width, height;
            if (!msg->findInt32("width", &width)
                    || !msg->findInt32("height", &height)) {
                err = INVALID_OPERATION;
            } else {
                //override height & width with max for smooth streaming
                if (mAdaptivePlayback) {
                    width = MAX_WIDTH;
                    height = MAX_HEIGHT;
                }

                err = setupVideoDecoder(mime, width, height);

                //Enable component support to extract SEI extradata if present in AVC stream
                QOMX_ENABLETYPE extra_data;
                extra_data.bEnable = OMX_TRUE;

                status_t errVal = mOMX->setParameter(
                        mNode, (OMX_INDEXTYPE)OMX_QcomIndexEnableExtnUserData,
                        (OMX_PTR)&extra_data, sizeof(extra_data));

                if (errVal != OK) {
                        DC_MSG_ERROR("[%s] setting OMX_QcomIndexEnableExtnUserData failed: %d",
                                mComponentName.c_str(), err);
                    }
                else
                {
                  DC_MSG_ERROR("[%s] setting OMX_QcomIndexEnableExtnUserData success",
                                mComponentName.c_str());
                }
            }
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)) {
        int32_t numChannels, sampleRate;
        if (!msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            int32_t isADTS, aacProfile;
            if (!msg->findInt32("is-adts", &isADTS)) {
                isADTS = 0;
            }
            if (!msg->findInt32("aac-profile", &aacProfile)) {
                aacProfile = OMX_AUDIO_AACObjectNull;
            }

            err = setupAACCodec(
                    encoder, numChannels, sampleRate, bitRate, aacProfile, isADTS != 0);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_NB)) {
        err = setupAMRCodec(encoder, false /* isWAMR */, bitRate);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AMR_WB)) {
        err = setupAMRCodec(encoder, true /* isWAMR */, bitRate);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_G711_ALAW)
            || !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_G711_MLAW)) {
        // These are PCM-like formats with a fixed sample rate but
        // a variable number of channels.

        int32_t numChannels;
        if (!msg->findInt32("channel-count", &numChannels)) {
            err = INVALID_OPERATION;
        } else {
            err = setupG711Codec(encoder, numChannels);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)) {
        int32_t numChannels, sampleRate, compressionLevel = -1;
        if (encoder &&
                (!msg->findInt32("channel-count", &numChannels)
                        || !msg->findInt32("sample-rate", &sampleRate))) {
            DC_MSG_ERROR("missing channel count or sample rate for FLAC encoder");
            err = INVALID_OPERATION;
        } else {
            if (encoder) {
                if (!msg->findInt32("flac-compression-level", &compressionLevel)) {
                    compressionLevel = 5;// default FLAC compression level
                } else if (compressionLevel < 0) {
                    DC_MSG_HIGH("compression level %d outside [0..8] range, using 0", compressionLevel);
                    compressionLevel = 0;
                } else if (compressionLevel > 8) {
                    DC_MSG_HIGH("compression level %d outside [0..8] range, using 8", compressionLevel);
                    compressionLevel = 8;
                }
            }
            err = setupFlacCodec(encoder, numChannels, sampleRate, compressionLevel);
        }
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) {
        int32_t numChannels, sampleRate;
        if (encoder
                || !msg->findInt32("channel-count", &numChannels)
                || !msg->findInt32("sample-rate", &sampleRate)) {
            err = INVALID_OPERATION;
        } else {
            err = setupRawAudioFormat(kPortIndexInput, sampleRate, numChannels);
        }
    }

    if (!msg->findInt32("encoder-delay", &mEncoderDelay)) {
        mEncoderDelay = 0;
    }

    if (!msg->findInt32("encoder-padding", &mEncoderPadding)) {
        mEncoderPadding = 0;
    }

    if (msg->findInt32("channel-mask", &mChannelMask)) {
        mChannelMaskPresent = true;
    } else {
        mChannelMaskPresent = false;
    }

    int32_t maxInputSize;
    if (msg->findInt32("max-input-size", &maxInputSize)) {
        err = setMinBufferSize(kPortIndexInput, (size_t)maxInputSize);
    } else if (!strcmp("OMX.Nvidia.aac.decoder", mComponentName.c_str())) {
        err = setMinBufferSize(kPortIndexInput, 8192);  // XXX
    }

    return err;
}

status_t DashCodec::setMinBufferSize(OMX_U32 portIndex, size_t size) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    if (def.nBufferSize >= size) {
        return OK;
    }

    def.nBufferSize = size;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    CHECK(def.nBufferSize >= size);

    return OK;
}

status_t DashCodec::selectAudioPortFormat(
        OMX_U32 portIndex, OMX_AUDIO_CODINGTYPE desiredFormat) {
    OMX_AUDIO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);

    format.nPortIndex = portIndex;
    for (OMX_U32 index = 0;; ++index) {
        format.nIndex = index;

        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        if (format.eEncoding == desiredFormat) {
            break;
        }
    }

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioPortFormat, &format, sizeof(format));
}

status_t DashCodec::setupAACCodec(
        bool encoder, int32_t numChannels, int32_t sampleRate,
        int32_t bitRate, int32_t aacProfile, bool isADTS) {
    if (encoder && isADTS) {
        return -EINVAL;
    }
    status_t err = setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            sampleRate,
            numChannels);

    if (err != OK) {
        return err;
    }

    if (encoder) {
        err = selectAudioPortFormat(kPortIndexOutput, OMX_AUDIO_CodingAAC);

        if (err != OK) {
            return err;
        }

        OMX_PARAM_PORTDEFINITIONTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            return err;
        }

        def.format.audio.bFlagErrorConcealment = OMX_TRUE;
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

        if (err != OK) {
            return err;
        }

        OMX_AUDIO_PARAM_AACPROFILETYPE profile;
        InitOMXParams(&profile);
        profile.nPortIndex = kPortIndexOutput;

        err = mOMX->getParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            return err;
        }

        profile.nChannels = numChannels;

        profile.eChannelMode =
            (numChannels == 1)
                ? OMX_AUDIO_ChannelModeMono: OMX_AUDIO_ChannelModeStereo;

        profile.nSampleRate = sampleRate;
        profile.nBitRate = bitRate;
        profile.nAudioBandWidth = 0;
        profile.nFrameLength = 0;
        profile.nAACtools = OMX_AUDIO_AACToolAll;
        profile.nAACERtools = OMX_AUDIO_AACERNone;
        profile.eAACProfile = (OMX_AUDIO_AACPROFILETYPE) aacProfile;
        profile.eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;

        err = mOMX->setParameter(
                mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

        if (err != OK) {
            return err;
        }

        return err;
    }

    OMX_AUDIO_PARAM_AACPROFILETYPE profile;
    InitOMXParams(&profile);
    profile.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));

    if (err != OK) {
        return err;
    }

    profile.nChannels = numChannels;
    profile.nSampleRate = sampleRate;

    profile.eAACStreamFormat =
        isADTS
            ? OMX_AUDIO_AACStreamFormatMP4ADTS
            : OMX_AUDIO_AACStreamFormatMP4FF;

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioAac, &profile, sizeof(profile));
}

static OMX_AUDIO_AMRBANDMODETYPE pickModeFromBitRate(
        bool isAMRWB, int32_t bps) {
    if (isAMRWB) {
        if (bps <= 6600) {
            return OMX_AUDIO_AMRBandModeWB0;
        } else if (bps <= 8850) {
            return OMX_AUDIO_AMRBandModeWB1;
        } else if (bps <= 12650) {
            return OMX_AUDIO_AMRBandModeWB2;
        } else if (bps <= 14250) {
            return OMX_AUDIO_AMRBandModeWB3;
        } else if (bps <= 15850) {
            return OMX_AUDIO_AMRBandModeWB4;
        } else if (bps <= 18250) {
            return OMX_AUDIO_AMRBandModeWB5;
        } else if (bps <= 19850) {
            return OMX_AUDIO_AMRBandModeWB6;
        } else if (bps <= 23050) {
            return OMX_AUDIO_AMRBandModeWB7;
        }

        // 23850 bps
        return OMX_AUDIO_AMRBandModeWB8;
    } else {  // AMRNB
        if (bps <= 4750) {
            return OMX_AUDIO_AMRBandModeNB0;
        } else if (bps <= 5150) {
            return OMX_AUDIO_AMRBandModeNB1;
        } else if (bps <= 5900) {
            return OMX_AUDIO_AMRBandModeNB2;
        } else if (bps <= 6700) {
            return OMX_AUDIO_AMRBandModeNB3;
        } else if (bps <= 7400) {
            return OMX_AUDIO_AMRBandModeNB4;
        } else if (bps <= 7950) {
            return OMX_AUDIO_AMRBandModeNB5;
        } else if (bps <= 10200) {
            return OMX_AUDIO_AMRBandModeNB6;
        }

        // 12200 bps
        return OMX_AUDIO_AMRBandModeNB7;
    }
}

status_t DashCodec::setupAMRCodec(bool encoder, bool isWAMR, int32_t bitrate) {
    OMX_AUDIO_PARAM_AMRTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = encoder ? kPortIndexOutput : kPortIndexInput;

    status_t err =
        mOMX->getParameter(mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.eAMRFrameFormat = OMX_AUDIO_AMRFrameFormatFSF;
    def.eAMRBandMode = pickModeFromBitRate(isWAMR, bitrate);

    err = mOMX->setParameter(
            mNode, OMX_IndexParamAudioAmr, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    return setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            isWAMR ? 16000 : 8000 /* sampleRate */,
            1 /* numChannels */);
}

status_t DashCodec::setupG711Codec(bool encoder, int32_t numChannels) {
    CHECK(!encoder);  // XXX TODO

    return setupRawAudioFormat(
            kPortIndexInput, 8000 /* sampleRate */, numChannels);
}

status_t DashCodec::setupFlacCodec(
        bool encoder, int32_t numChannels, int32_t sampleRate, int32_t compressionLevel) {

    if (encoder) {
        OMX_AUDIO_PARAM_FLACTYPE def;
        InitOMXParams(&def);
        def.nPortIndex = kPortIndexOutput;

        // configure compression level
        status_t err = mOMX->getParameter(mNode, OMX_IndexParamAudioFlac, &def, sizeof(def));
        if (err != OK) {
            DC_MSG_ERROR("setupFlacCodec(): Error %d getting OMX_IndexParamAudioFlac parameter", err);
            return err;
        }
        def.nCompressionLevel = compressionLevel;
        err = mOMX->setParameter(mNode, OMX_IndexParamAudioFlac, &def, sizeof(def));
        if (err != OK) {
            DC_MSG_ERROR("setupFlacCodec(): Error %d setting OMX_IndexParamAudioFlac parameter", err);
            return err;
        }
    }

    return setupRawAudioFormat(
            encoder ? kPortIndexInput : kPortIndexOutput,
            sampleRate,
            numChannels);
}

status_t DashCodec::setupRawAudioFormat(
        OMX_U32 portIndex, int32_t sampleRate, int32_t numChannels) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    OMX_AUDIO_PARAM_PCMMODETYPE pcmParams;
    InitOMXParams(&pcmParams);
    pcmParams.nPortIndex = portIndex;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));

    if (err != OK) {
        return err;
    }

    pcmParams.nChannels = numChannels;
    pcmParams.eNumData = OMX_NumericalDataSigned;
    pcmParams.bInterleaved = OMX_TRUE;
    pcmParams.nBitPerSample = 16;
    pcmParams.nSamplingRate = sampleRate;
    pcmParams.ePCMMode = OMX_AUDIO_PCMModeLinear;

    if (getOMXChannelMapping(numChannels, pcmParams.eChannelMapping) != OK) {
        return OMX_ErrorNone;
    }

    return mOMX->setParameter(
            mNode, OMX_IndexParamAudioPcm, &pcmParams, sizeof(pcmParams));
}

status_t DashCodec::setVideoPortFormatType(
        OMX_U32 portIndex,
        OMX_VIDEO_CODINGTYPE compressionFormat,
        OMX_COLOR_FORMATTYPE colorFormat) {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = portIndex;
    format.nIndex = 0;
    bool found = false;

    OMX_U32 index = 0;
    for (;;) {
        format.nIndex = index;
        status_t err = mOMX->getParameter(
                mNode, OMX_IndexParamVideoPortFormat,
                &format, sizeof(format));

        if (err != OK) {
            return err;
        }

        // The following assertion is violated by TI's video decoder.
        // CHECK_EQ(format.nIndex, index);

        if (!strcmp("OMX.TI.Video.encoder", mComponentName.c_str())) {
            if (portIndex == kPortIndexInput
                    && colorFormat == format.eColorFormat) {
                // eCompressionFormat does not seem right.
                found = true;
                break;
            }
            if (portIndex == kPortIndexOutput
                    && compressionFormat == format.eCompressionFormat) {
                // eColorFormat does not seem right.
                found = true;
                break;
            }
        }

        if (format.eCompressionFormat == compressionFormat
            && format.eColorFormat == colorFormat) {
            found = true;
            break;
        }

        ++index;
    }

    if (!found) {
        return UNKNOWN_ERROR;
    }

    status_t err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));

    return err;
}

status_t DashCodec::setSupportedOutputFormat() {
    OMX_VIDEO_PARAM_PORTFORMATTYPE format;
    InitOMXParams(&format);
    format.nPortIndex = kPortIndexOutput;
    format.nIndex = 0;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));
    CHECK_EQ(err, (status_t)OK);
    CHECK_EQ((int)format.eCompressionFormat, (int)OMX_VIDEO_CodingUnused);

    CHECK(format.eColorFormat == OMX_COLOR_FormatYUV420Planar
           || format.eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar
           || format.eColorFormat == OMX_COLOR_FormatCbYCrY
           || format.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar
           || format.eColorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar
           || format.eColorFormat == OMX_QCOM_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka
           || format.eColorFormat == (OMX_COLOR_FORMATTYPE)QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m
           );

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoPortFormat,
            &format, sizeof(format));
}

static status_t GetVideoCodingTypeFromMime(
        const char *mime, OMX_VIDEO_CODINGTYPE *codingType) {
    if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
        *codingType = OMX_VIDEO_CodingAVC;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
        *codingType = OMX_VIDEO_CodingMPEG4;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
        *codingType = OMX_VIDEO_CodingH263;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG2, mime)) {
        *codingType = OMX_VIDEO_CodingMPEG2;
    } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_HEVC, mime)) {
        *codingType = (OMX_VIDEO_CODINGTYPE)QOMX_VIDEO_CodingHevc;
    } else {
        *codingType = OMX_VIDEO_CodingUnused;
        return ERROR_UNSUPPORTED;
    }

    return OK;
}

status_t DashCodec::setupVideoDecoder(
        const char *mime, int32_t width, int32_t height) {
    OMX_VIDEO_CODINGTYPE compressionFormat;
    status_t err = GetVideoCodingTypeFromMime(mime, &compressionFormat);

    if (err != OK) {
        return err;
    }

    err = setVideoPortFormatType(
            kPortIndexInput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        return err;
    }

    err = setSupportedOutputFormat();

    if (err != OK) {
        return err;
    }

    err = setVideoFormatOnPort(
            kPortIndexInput, width, height, compressionFormat);

    if (err != OK) {
        return err;
    }

    err = setVideoFormatOnPort(
            kPortIndexOutput, width, height, OMX_VIDEO_CodingUnused);

    if (err != OK) {
        return err;
    }

    return OK;
}

status_t DashCodec::setupVideoEncoder(const char *mime, const sp<AMessage> &msg) {
    int32_t tmp;
    if (!msg->findInt32("color-format", &tmp)) {
        return INVALID_OPERATION;
    }

    OMX_COLOR_FORMATTYPE colorFormat =
        static_cast<OMX_COLOR_FORMATTYPE>(tmp);

    status_t err = setVideoPortFormatType(
            kPortIndexInput, OMX_VIDEO_CodingUnused, colorFormat);

    if (err != OK) {
        DC_MSG_ERROR("[%s] does not support color format %d",
              mComponentName.c_str(), colorFormat);

        return err;
    }

    /* Input port configuration */

    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    def.nPortIndex = kPortIndexInput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    int32_t width, height, bitrate;
    if (!msg->findInt32("width", &width)
            || !msg->findInt32("height", &height)
            || !msg->findInt32("bitrate", &bitrate)) {
        return INVALID_OPERATION;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    int32_t stride;
    if (!msg->findInt32("stride", &stride)) {
        stride = width;
    }

    video_def->nStride = stride;

    int32_t sliceHeight;
    if (!msg->findInt32("slice-height", &sliceHeight)) {
        sliceHeight = height;
    }

    video_def->nSliceHeight = sliceHeight;

    def.nBufferSize = (video_def->nStride * video_def->nSliceHeight * 3) / 2;

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    video_def->xFramerate = (OMX_U32)(frameRate * 65536.0f);
    video_def->eCompressionFormat = OMX_VIDEO_CodingUnused;
    video_def->eColorFormat = colorFormat;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        DC_MSG_ERROR("[%s] failed to set input port definition parameters.",
              mComponentName.c_str());

        return err;
    }

    /* Output port configuration */

    OMX_VIDEO_CODINGTYPE compressionFormat;
    err = GetVideoCodingTypeFromMime(mime, &compressionFormat);

    if (err != OK) {
        return err;
    }

    err = setVideoPortFormatType(
            kPortIndexOutput, compressionFormat, OMX_COLOR_FormatUnused);

    if (err != OK) {
        DC_MSG_ERROR("[%s] does not support compression format %d",
             mComponentName.c_str(), compressionFormat);

        return err;
    }

    def.nPortIndex = kPortIndexOutput;

    err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        return err;
    }

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;
    video_def->xFramerate = 0;
    video_def->nBitrate = bitrate;
    video_def->eCompressionFormat = compressionFormat;
    video_def->eColorFormat = OMX_COLOR_FormatUnused;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    if (err != OK) {
        DC_MSG_ERROR("[%s] failed to set output port definition parameters.",
              mComponentName.c_str());

        return err;
    }

    switch (compressionFormat) {
        case OMX_VIDEO_CodingMPEG4:
            err = setupMPEG4EncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingH263:
            err = setupH263EncoderParameters(msg);
            break;

        case OMX_VIDEO_CodingAVC:
            err = setupAVCEncoderParameters(msg);
            break;

        default:
            break;
    }

    DC_MSG_HIGH("setupVideoEncoder succeeded");

    return err;
}

static OMX_U32 setPFramesSpacing(int32_t iFramesInterval, int32_t frameRate) {
    if (iFramesInterval < 0) {
        return 0xFFFFFFFF;
    } else if (iFramesInterval == 0) {
        return 0;
    }
    OMX_U32 ret = frameRate * iFramesInterval;
    CHECK(ret > 1);
    return ret;
}

static OMX_VIDEO_CONTROLRATETYPE getBitrateMode(const sp<AMessage> &msg) {
    int32_t tmp;
    if (!msg->findInt32("bitrate-mode", &tmp)) {
        return OMX_Video_ControlRateVariable;
    }

    return static_cast<OMX_VIDEO_CONTROLRATETYPE>(tmp);
}

status_t DashCodec::setupMPEG4EncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    OMX_VIDEO_PARAM_MPEG4TYPE mpeg4type;
    InitOMXParams(&mpeg4type);
    mpeg4type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));

    if (err != OK) {
        return err;
    }

    mpeg4type.nSliceHeaderSpacing = 0;
    mpeg4type.bSVH = OMX_FALSE;
    mpeg4type.bGov = OMX_FALSE;

    mpeg4type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    mpeg4type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
    if (mpeg4type.nPFrames == 0) {
        mpeg4type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    mpeg4type.nBFrames = 0;
    mpeg4type.nIDCVLCThreshold = 0;
    mpeg4type.bACPred = OMX_TRUE;
    mpeg4type.nMaxPacketSize = 256;
    mpeg4type.nTimeIncRes = 1000;
    mpeg4type.nHeaderExtension = 0;
    mpeg4type.bReversibleVLC = OMX_FALSE;

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            return err;
        }

        mpeg4type.eProfile = static_cast<OMX_VIDEO_MPEG4PROFILETYPE>(profile);
        mpeg4type.eLevel = static_cast<OMX_VIDEO_MPEG4LEVELTYPE>(level);
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoMpeg4, &mpeg4type, sizeof(mpeg4type));

    if (err != OK) {
        return err;
    }

    err = configureBitrate(bitrate, bitrateMode);

    if (err != OK) {
        return err;
    }

    return setupErrorCorrectionParameters();
}

status_t DashCodec::setupH263EncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    OMX_VIDEO_PARAM_H263TYPE h263type;
    InitOMXParams(&h263type);
    h263type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));

    if (err != OK) {
        return err;
    }

    h263type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    h263type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
    if (h263type.nPFrames == 0) {
        h263type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
    }
    h263type.nBFrames = 0;

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            return err;
        }

        h263type.eProfile = static_cast<OMX_VIDEO_H263PROFILETYPE>(profile);
        h263type.eLevel = static_cast<OMX_VIDEO_H263LEVELTYPE>(level);
    }

    h263type.bPLUSPTYPEAllowed = OMX_FALSE;
    h263type.bForceRoundingTypeToZero = OMX_FALSE;
    h263type.nPictureHeaderRepetition = 0;
    h263type.nGOBHeaderInterval = 0;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoH263, &h263type, sizeof(h263type));

    if (err != OK) {
        return err;
    }

    err = configureBitrate(bitrate, bitrateMode);

    if (err != OK) {
        return err;
    }

    return setupErrorCorrectionParameters();
}

status_t DashCodec::setupAVCEncoderParameters(const sp<AMessage> &msg) {
    int32_t bitrate, iFrameInterval;
    if (!msg->findInt32("bitrate", &bitrate)
            || !msg->findInt32("i-frame-interval", &iFrameInterval)) {
        return INVALID_OPERATION;
    }

    OMX_VIDEO_CONTROLRATETYPE bitrateMode = getBitrateMode(msg);

    float frameRate;
    if (!msg->findFloat("frame-rate", &frameRate)) {
        int32_t tmp;
        if (!msg->findInt32("frame-rate", &tmp)) {
            return INVALID_OPERATION;
        }
        frameRate = (float)tmp;
    }

    OMX_VIDEO_PARAM_AVCTYPE h264type;
    InitOMXParams(&h264type);
    h264type.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));

    if (err != OK) {
        return err;
    }

    h264type.nAllowedPictureTypes =
        OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;

    int32_t profile;
    if (msg->findInt32("profile", &profile)) {
        int32_t level;
        if (!msg->findInt32("level", &level)) {
            return INVALID_OPERATION;
        }

        err = verifySupportForProfileAndLevel(profile, level);

        if (err != OK) {
            return err;
        }

        h264type.eProfile = static_cast<OMX_VIDEO_AVCPROFILETYPE>(profile);
        h264type.eLevel = static_cast<OMX_VIDEO_AVCLEVELTYPE>(level);
    }

    // XXX
    if (h264type.eProfile != OMX_VIDEO_AVCProfileBaseline) {
        DC_MSG_HIGH("Use baseline profile instead of %d for AVC recording",
            h264type.eProfile);
        h264type.eProfile = OMX_VIDEO_AVCProfileBaseline;
    }

    if (h264type.eProfile == OMX_VIDEO_AVCProfileBaseline) {
        h264type.nSliceHeaderSpacing = 0;
        h264type.bUseHadamard = OMX_TRUE;
        h264type.nRefFrames = 1;
        h264type.nBFrames = 0;
        h264type.nPFrames = setPFramesSpacing(iFrameInterval, frameRate);
        if (h264type.nPFrames == 0) {
            h264type.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI;
        }
        h264type.nRefIdx10ActiveMinus1 = 0;
        h264type.nRefIdx11ActiveMinus1 = 0;
        h264type.bEntropyCodingCABAC = OMX_FALSE;
        h264type.bWeightedPPrediction = OMX_FALSE;
        h264type.bconstIpred = OMX_FALSE;
        h264type.bDirect8x8Inference = OMX_FALSE;
        h264type.bDirectSpatialTemporal = OMX_FALSE;
        h264type.nCabacInitIdc = 0;
    }

    if (h264type.nBFrames != 0) {
        h264type.nAllowedPictureTypes |= OMX_VIDEO_PictureTypeB;
    }

    h264type.bEnableUEP = OMX_FALSE;
    h264type.bEnableFMO = OMX_FALSE;
    h264type.bEnableASO = OMX_FALSE;
    h264type.bEnableRS = OMX_FALSE;
    h264type.bFrameMBsOnly = OMX_TRUE;
    h264type.bMBAFF = OMX_FALSE;
    h264type.eLoopFilterMode = OMX_VIDEO_AVCLoopFilterEnable;

    err = mOMX->setParameter(
            mNode, OMX_IndexParamVideoAvc, &h264type, sizeof(h264type));

    if (err != OK) {
        return err;
    }

    return configureBitrate(bitrate, bitrateMode);
}

status_t DashCodec::verifySupportForProfileAndLevel(
        int32_t profile, int32_t level) {
    OMX_VIDEO_PARAM_PROFILELEVELTYPE params;
    InitOMXParams(&params);
    params.nPortIndex = kPortIndexOutput;

    for (params.nProfileIndex = 0;; ++params.nProfileIndex) {
        status_t err = mOMX->getParameter(
                mNode,
                OMX_IndexParamVideoProfileLevelQuerySupported,
                &params,
                sizeof(params));

        if (err != OK) {
            return err;
        }

        int32_t supportedProfile = static_cast<int32_t>(params.eProfile);
        int32_t supportedLevel = static_cast<int32_t>(params.eLevel);

        if (profile == supportedProfile && level <= supportedLevel) {
            return OK;
        }
    }
}

status_t DashCodec::configureBitrate(
        int32_t bitrate, OMX_VIDEO_CONTROLRATETYPE bitrateMode) {
    OMX_VIDEO_PARAM_BITRATETYPE bitrateType;
    InitOMXParams(&bitrateType);
    bitrateType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));

    if (err != OK) {
        return err;
    }

    bitrateType.eControlRate = bitrateMode;
    bitrateType.nTargetBitrate = bitrate;

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoBitrate,
            &bitrateType, sizeof(bitrateType));
}

status_t DashCodec::setupErrorCorrectionParameters() {
    OMX_VIDEO_PARAM_ERRORCORRECTIONTYPE errorCorrectionType;
    InitOMXParams(&errorCorrectionType);
    errorCorrectionType.nPortIndex = kPortIndexOutput;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));

    if (err != OK) {
        return OK;  // Optional feature. Ignore this failure
    }

    errorCorrectionType.bEnableHEC = OMX_FALSE;
    errorCorrectionType.bEnableResync = OMX_TRUE;
    errorCorrectionType.nResynchMarkerSpacing = 256;
    errorCorrectionType.bEnableDataPartitioning = OMX_FALSE;
    errorCorrectionType.bEnableRVLC = OMX_FALSE;

    return mOMX->setParameter(
            mNode, OMX_IndexParamVideoErrorCorrection,
            &errorCorrectionType, sizeof(errorCorrectionType));
}

status_t DashCodec::setVideoFormatOnPort(
        OMX_U32 portIndex,
        int32_t width, int32_t height, OMX_VIDEO_CODINGTYPE compressionFormat) {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);
    def.nPortIndex = portIndex;

    OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &def.format.video;

    status_t err = mOMX->getParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    CHECK_EQ(err, (status_t)OK);

    if (portIndex == kPortIndexInput) {
        // XXX Need a (much) better heuristic to compute input buffer sizes.
        const size_t X = 64 * 1024;
        if (def.nBufferSize < X) {
            def.nBufferSize = X;
        }
    }

    CHECK_EQ((int)def.eDomain, (int)OMX_PortDomainVideo);

    video_def->nFrameWidth = width;
    video_def->nFrameHeight = height;

    if (portIndex == kPortIndexInput) {
        video_def->eCompressionFormat = compressionFormat;
        video_def->eColorFormat = OMX_COLOR_FormatUnused;
    }

    err = mOMX->setParameter(
            mNode, OMX_IndexParamPortDefinition, &def, sizeof(def));

    return err;
}

status_t DashCodec::initNativeWindow() {
    if (mNativeWindow != NULL) {
        return mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_TRUE);
    }

    mOMX->enableGraphicBuffers(mNode, kPortIndexOutput, OMX_FALSE);
    return OK;
}

size_t DashCodec::countBuffersOwnedByComponent(OMX_U32 portIndex) const {
    size_t n = 0;

    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        const BufferInfo &info = mBuffers[portIndex].itemAt(i);

        if (info.mStatus == BufferInfo::OWNED_BY_COMPONENT) {
            ++n;
        }
    }

    return n;
}

size_t DashCodec::countBuffersOwnedByNativeWindow() const {
    size_t n = 0;

    for (size_t i = 0; i < mBuffers[kPortIndexOutput].size(); ++i) {
        const BufferInfo &info = mBuffers[kPortIndexOutput].itemAt(i);

        if (info.mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
            ++n;
        }
    }

    return n;
}

void DashCodec::waitUntilAllPossibleNativeWindowBuffersAreReturnedToUs() {
    if (mNativeWindow == NULL) {
        return;
    }

    int minUndequeuedBufs = 0;
    status_t err = mNativeWindow->query(
            mNativeWindow.get(), NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS,
            &minUndequeuedBufs);

    if (err != OK) {
        DC_MSG_ERROR("[%s] NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                mComponentName.c_str(), strerror(-err), -err);

        minUndequeuedBufs = 0;
    }

    while (countBuffersOwnedByNativeWindow() > (size_t)minUndequeuedBufs
            && dequeueBufferFromNativeWindow() != NULL) {
        // these buffers will be submitted as regular buffers; account for this
        if (mStoreMetaDataInOutputBuffers && mMetaDataBuffersToSubmit > 0) {
            --mMetaDataBuffersToSubmit;
        }
    }
}

bool DashCodec::allYourBuffersAreBelongToUs(
        OMX_U32 portIndex) {
    for (size_t i = 0; i < mBuffers[portIndex].size(); ++i) {
        BufferInfo *info = &mBuffers[portIndex].editItemAt(i);

        if (info->mStatus != BufferInfo::OWNED_BY_US
                && info->mStatus != BufferInfo::OWNED_BY_NATIVE_WINDOW) {
            DC_MSG_LOW("[%s] Buffer %p on port %ld still has status %d",
                    mComponentName.c_str(),
                    info->mBufferID, portIndex, info->mStatus);
            return false;
        }
    }

    return true;
}

bool DashCodec::allYourBuffersAreBelongToUs() {
    return allYourBuffersAreBelongToUs(kPortIndexInput)
        && allYourBuffersAreBelongToUs(kPortIndexOutput);
}

void DashCodec::deferMessage(const sp<AMessage> &msg) {
    bool wasEmptyBefore = mDeferredQueue.empty();
    mDeferredQueue.push_back(msg);
}

void DashCodec::processDeferredMessages() {
    List<sp<AMessage> > queue = mDeferredQueue;
    mDeferredQueue.clear();

    List<sp<AMessage> >::iterator it = queue.begin();
    while (it != queue.end()) {
        onMessageReceived(*it++);
    }
}

void DashCodec::queueNextFormat() {
    OMX_PARAM_PORTDEFINITIONTYPE* def = new OMX_PARAM_PORTDEFINITIONTYPE();
    InitOMXParams(def);
    def->nPortIndex = kPortIndexOutput;

    CHECK_EQ(mOMX->getParameter(
                mNode, OMX_IndexParamPortDefinition, def, sizeof(*def)),
             (status_t)OK);

    CHECK_EQ((int)def->eDir, (int)OMX_DirOutput);
    mFormats.push_back(def);

    OMX_CONFIG_RECTTYPE* rect = new OMX_CONFIG_RECTTYPE();
    InitOMXParams(rect);
    rect->nPortIndex = kPortIndexOutput;
    if (mOMX->getConfig(mNode, OMX_IndexConfigCommonOutputCrop, rect, sizeof(*rect)) != OK) {
        mOutputCrops.push_back(NULL);
    } else {
        mOutputCrops.push_back(rect);
    }
}

void DashCodec::clearCachedFormats() {
    for (size_t i = 0 ; i < mOutputCrops.size(); i++)
    {
      if (mOutputCrops[i])
      {
        delete mOutputCrops[i];
      }
    }
    for (size_t i = 0 ; i < mFormats.size(); i++)
    {
      if (mFormats[i])
      {
        delete mFormats[i];
      }
    }
    mOutputCrops.clear();
    mFormats.clear();
}

void DashCodec::sendFormatChange() {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", kWhatOutputFormatChanged);
    bool useCachedConfig = false;
    OMX_PARAM_PORTDEFINITIONTYPE* def;
    if (mFormats.size() > 0) {
        useCachedConfig = true;
        def = mFormats[0];
        mFormats.removeAt(0);
    } else {
        def = new OMX_PARAM_PORTDEFINITIONTYPE();
        InitOMXParams(def);
        def->nPortIndex = kPortIndexOutput;

        CHECK_EQ(mOMX->getParameter(
                    mNode, OMX_IndexParamPortDefinition, def, sizeof(*def)),
                 (status_t)OK);

        CHECK_EQ((int)def->eDir, (int)OMX_DirOutput);
    }
    switch (def->eDomain) {
        case OMX_PortDomainVideo:
        {
            OMX_VIDEO_PORTDEFINITIONTYPE *videoDef = &def->format.video;

            notify->setString("mime", MEDIA_MIMETYPE_VIDEO_RAW);
            notify->setInt32("width", videoDef->nFrameWidth);
            notify->setInt32("height", videoDef->nFrameHeight);
            notify->setInt32("stride", videoDef->nStride);
            notify->setInt32("slice-height", videoDef->nSliceHeight);
            notify->setInt32("color-format", videoDef->eColorFormat);
            DC_MSG_HIGH("sendformatchange: %lu %lu", videoDef->nFrameWidth, videoDef->nFrameHeight);

            //If dynamic buffering mode cache the latest width and height. Will be used for VENUS macors to calculate filledLen in fbd's.
            if(mStoreMetaDataInOutputBuffers)
            {
              mCurrentWidth = videoDef->nFrameWidth;
              mCurrentHeight = videoDef->nFrameHeight;
            }

            OMX_CONFIG_RECTTYPE* rect;
            bool hasValidCrop = true;
            if (useCachedConfig) {
                rect = mOutputCrops[0];
                mOutputCrops.removeAt(0);
                if (rect == NULL) {
                    rect = new OMX_CONFIG_RECTTYPE();
                    hasValidCrop = false;
                }
            } else {
                rect = new OMX_CONFIG_RECTTYPE();
                InitOMXParams(rect);
                rect->nPortIndex = kPortIndexOutput;
                hasValidCrop = (mOMX->getConfig(
                    mNode, OMX_IndexConfigCommonOutputCrop,
                    rect, sizeof(*rect)) == OK);
            }

            if (!hasValidCrop) {
                rect->nLeft = 0;
                rect->nTop = 0;
                rect->nWidth = videoDef->nFrameWidth;
                rect->nHeight = videoDef->nFrameHeight;
            }

            CHECK_GE(rect->nLeft, 0);
            CHECK_GE(rect->nTop, 0);
            CHECK_GE(rect->nWidth, 0u);
            CHECK_GE(rect->nHeight, 0u);
            CHECK_LE(rect->nLeft + rect->nWidth - 1, videoDef->nFrameWidth);
            CHECK_LE(rect->nTop + rect->nHeight - 1, videoDef->nFrameHeight);

            notify->setRect(
                    "crop",
                    rect->nLeft,
                    rect->nTop,
                    rect->nLeft + rect->nWidth - 1,
                    rect->nTop + rect->nHeight - 1);

            if (mNativeWindow != NULL) {
                android_native_rect_t crop;
                crop.left = rect->nLeft;
                crop.top = rect->nTop;
                crop.right = rect->nLeft + rect->nWidth;
                crop.bottom = rect->nTop + rect->nHeight;

                CHECK_EQ(0, native_window_set_crop(
                            mNativeWindow.get(), &crop));
            }
            delete rect;
            break;
        }

        case OMX_PortDomainAudio:
        {
            OMX_AUDIO_PORTDEFINITIONTYPE *audioDef = &def->format.audio;
            CHECK_EQ((int)audioDef->eEncoding, (int)OMX_AUDIO_CodingPCM);

            OMX_AUDIO_PARAM_PCMMODETYPE params;
            InitOMXParams(&params);
            params.nPortIndex = kPortIndexOutput;

            CHECK_EQ(mOMX->getParameter(
                        mNode, OMX_IndexParamAudioPcm,
                        &params, sizeof(params)),
                     (status_t)OK);

            CHECK(params.nChannels == 1 || params.bInterleaved);
            CHECK_EQ(params.nBitPerSample, 16u);
            CHECK_EQ((int)params.eNumData, (int)OMX_NumericalDataSigned);
            CHECK_EQ((int)params.ePCMMode, (int)OMX_AUDIO_PCMModeLinear);

            notify->setString("mime", MEDIA_MIMETYPE_AUDIO_RAW);
            notify->setInt32("channel-count", params.nChannels);
            notify->setInt32("sample-rate", params.nSamplingRate);
            if (mEncoderDelay + mEncoderPadding) {
                size_t frameSize = params.nChannels * sizeof(int16_t);
                if (mSkipCutBuffer != NULL) {
                    size_t prevbufsize = mSkipCutBuffer->size();
                    if (prevbufsize != 0) {
                        DC_MSG_HIGH("Replacing SkipCutBuffer holding %d bytes", prevbufsize);
                    }
                }
                mSkipCutBuffer = new SkipCutBuffer(mEncoderDelay * frameSize,
                                                   mEncoderPadding * frameSize);
            }

            if (mChannelMaskPresent) {
                notify->setInt32("channel-mask", mChannelMask);
            }

            break;
        }

        default:
            TRESPASS();
    }

    notify->post();
    delete def;
    mSentFormat = true;
}

void DashCodec::signalError(OMX_ERRORTYPE error, status_t internalError) {
    sp<AMessage> notify = mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatError);
    notify->setInt32("omx-error", error);
    notify->setInt32("err", internalError);
    notify->post();
}

status_t DashCodec::PushBlankBuffersToNativeWindow(sp<ANativeWindow> nativeWindow) {
    status_t err = NO_ERROR;
    ANativeWindowBuffer* anb = NULL;
    int numBufs = 0;
    int minUndequeuedBufs = 0;

    // We need to reconnect to the ANativeWindow as a CPU client to ensure that
    // no frames get dropped by SurfaceFlinger assuming that these are video
    // frames.
    err = native_window_api_disconnect(nativeWindow.get(),
            NATIVE_WINDOW_API_MEDIA);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: api_disconnect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_api_connect(nativeWindow.get(),
            NATIVE_WINDOW_API_CPU);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: api_connect failed: %s (%d)",
                strerror(-err), -err);
        return err;
    }

    err = native_window_set_buffers_dimensions(nativeWindow.get(),
            1, 1);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: set_buffers_dimensions failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = native_window_set_buffers_format(nativeWindow.get(),
            HAL_PIXEL_FORMAT_RGBX_8888);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: set_buffers_format failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = native_window_set_scaling_mode(nativeWindow.get(),
                NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank_frames: set_scaling_mode failed: %s (%d)",
              strerror(-err), -err);
        goto error;
    }

    err = native_window_set_usage(nativeWindow.get(),
            GRALLOC_USAGE_SW_WRITE_OFTEN);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: set_usage failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    err = nativeWindow->query(nativeWindow.get(),
            NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minUndequeuedBufs);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: MIN_UNDEQUEUED_BUFFERS query "
                "failed: %s (%d)", strerror(-err), -err);
        goto error;
    }

    numBufs = minUndequeuedBufs + 1;
    err = native_window_set_buffer_count(nativeWindow.get(), numBufs);
    if (err != NO_ERROR) {
        DC_MSG_ERROR("error pushing blank frames: set_buffer_count failed: %s (%d)",
                strerror(-err), -err);
        goto error;
    }

    // We  push numBufs + 1 buffers to ensure that we've drawn into the same
    // buffer twice.  This should guarantee that the buffer has been displayed
    // on the screen and then been replaced, so an previous video frames are
    // guaranteed NOT to be currently displayed.
    for (int i = 0; i < numBufs + 1; i++) {
        int fenceFd = -1;
        err = native_window_dequeue_buffer_and_wait(nativeWindow.get(), &anb);
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: dequeueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        sp<GraphicBuffer> buf(new GraphicBuffer(anb, false));

        // Fill the buffer with the a 1x1 checkerboard pattern ;)
        uint32_t* img = NULL;
        err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, (void**)(&img));
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: lock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        *img = 0;

        err = buf->unlock();
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: unlock failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        err = nativeWindow->queueBuffer(nativeWindow.get(),
                buf->getNativeBuffer(), -1);
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: queueBuffer failed: %s (%d)",
                    strerror(-err), -err);
            goto error;
        }

        anb = NULL;
    }

error:

    if (err != NO_ERROR) {
        // Clean up after an error.
        if (anb != NULL) {
            nativeWindow->cancelBuffer(nativeWindow.get(), anb, -1);
        }

        native_window_api_disconnect(nativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        native_window_api_connect(nativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);

        return err;
    } else {
        // Clean up after success.
        err = native_window_api_disconnect(nativeWindow.get(),
                NATIVE_WINDOW_API_CPU);
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: api_disconnect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        err = native_window_api_connect(nativeWindow.get(),
                NATIVE_WINDOW_API_MEDIA);
        if (err != NO_ERROR) {
            DC_MSG_ERROR("error pushing blank frames: api_connect failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }

        return NO_ERROR;
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::PortDescription::PortDescription() {
}

status_t DashCodec::requestIDRFrame() {
    if (!mIsEncoder) {
        return ERROR_UNSUPPORTED;
    }

    OMX_CONFIG_INTRAREFRESHVOPTYPE params;
    InitOMXParams(&params);

    params.nPortIndex = kPortIndexOutput;
    params.IntraRefreshVOP = OMX_TRUE;

    return mOMX->setConfig(
            mNode,
            OMX_IndexConfigVideoIntraVOPRefresh,
            &params,
            sizeof(params));
}

void DashCodec::PortDescription::addBuffer(
        IOMX::buffer_id id, const sp<ABuffer> &buffer) {
    mBufferIDs.push_back(id);
    mBuffers.push_back(buffer);
}

size_t DashCodec::PortDescription::countBuffers() {
    return mBufferIDs.size();
}

IOMX::buffer_id DashCodec::PortDescription::bufferIDAt(size_t index) const {
    return mBufferIDs.itemAt(index);
}

sp<ABuffer> DashCodec::PortDescription::bufferAt(size_t index) const {
    return mBuffers.itemAt(index);
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::BaseState::BaseState(DashCodec *codec, const sp<AState> &parentState)
    : AState(parentState),
      mCodec(codec) {
}

DashCodec::BaseState::PortMode DashCodec::BaseState::getPortMode(OMX_U32 portIndex) {
    return KEEP_BUFFERS;
}

bool DashCodec::BaseState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatInputBufferFilled:
        {
            onInputBufferFilled(msg);
            break;
        }

        case kWhatOutputBufferDrained:
        {
            onOutputBufferDrained(msg);
            break;
        }

        case DashCodec::kWhatOMXMessage:
        {
            return onOMXMessage(msg);
        }

        case DashCodec::kWhatFlush:
        {
            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatFlushCompleted);
            notify->post();
            return true;
        }

        default:
            return false;
    }

    return true;
}

bool DashCodec::BaseState::onOMXMessage(const sp<AMessage> &msg) {
    int32_t type;
    CHECK(msg->findInt32("type", &type));

    IOMX::node_id nodeID = 0;
    CHECK(msg->findInt32("node", (int32_t*)&nodeID));
    CHECK_EQ(nodeID, mCodec->mNode);

    switch (type) {
        case omx_message::EVENT:
        {
            int32_t event, data1, data2;
            CHECK(msg->findInt32("event", &event));
            CHECK(msg->findInt32("data1", &data1));
            CHECK(msg->findInt32("data2", &data2));

            if (event == OMX_EventCmdComplete
                    && data1 == OMX_CommandFlush
                    && data2 == (int32_t)OMX_ALL) {
                // Use of this notification is not consistent across
                // implementations. We'll drop this notification and rely
                // on flush-complete notifications on the individual port
                // indices instead.

                return true;
            }

            return onOMXEvent(
                    static_cast<OMX_EVENTTYPE>(event),
                    static_cast<OMX_U32>(data1),
                    static_cast<OMX_U32>(data2));
        }

        case omx_message::EMPTY_BUFFER_DONE:
        {
            IOMX::buffer_id bufferID;
            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));

            return onOMXEmptyBufferDone(bufferID);
        }

        case omx_message::FILL_BUFFER_DONE:
        {
            IOMX::buffer_id bufferID;
            CHECK(msg->findInt32("buffer", (int32_t*)&bufferID));

            int32_t rangeOffset, rangeLength, flags;
            int64_t timeUs;
            void *platformPrivate = NULL;
            void *dataPtr = NULL;

            CHECK(msg->findInt32("range_offset", &rangeOffset));
            CHECK(msg->findInt32("range_length", &rangeLength));
            CHECK(msg->findInt32("flags", &flags));
            CHECK(msg->findInt64("timestamp", &timeUs));
            //CHECK(msg->findPointer("platform_private", (void **)&platformPrivate));
            //CHECK(msg->findPointer("data_ptr", (void **)&dataPtr));

            return onOMXFillBufferDone(
                    bufferID,
                    (size_t)rangeOffset, (size_t)rangeLength,
                    (OMX_U32)flags,
                    timeUs,
                    platformPrivate,
                    dataPtr);
        }

        default:
            TRESPASS();
            break;
    }
}

bool DashCodec::BaseState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    if (event != OMX_EventError) {
        DC_MSG_LOW("[%s] EVENT(%d, 0x%08lx, 0x%08lx)",
             mCodec->mComponentName.c_str(), event, data1, data2);

        return false;
    }

    DC_MSG_ERROR("[%s] ERROR(0x%08lx)", mCodec->mComponentName.c_str(), data1);

    mCodec->signalError((OMX_ERRORTYPE)data1);

    return true;
}

bool DashCodec::BaseState::onOMXEmptyBufferDone(IOMX::buffer_id bufferID) {
    DC_MSG_HIGH("[%s] onOMXEmptyBufferDone %p",
         mCodec->mComponentName.c_str(), bufferID);

    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexInput, bufferID);

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_COMPONENT);
    info->mStatus = BufferInfo::OWNED_BY_US;

    const sp<AMessage> &bufferMeta = info->mData->meta();
    void *mediaBuffer;
    if (bufferMeta->findPointer("mediaBuffer", &mediaBuffer)
            && mediaBuffer != NULL) {
        // We're in "store-metadata-in-buffers" mode, the underlying
        // OMX component had access to data that's implicitly refcounted
        // by this "mediaBuffer" object. Now that the OMX component has
        // told us that it's done with the input buffer, we can decrement
        // the mediaBuffer's reference count.

        DC_MSG_LOW("releasing mbuf %p", mediaBuffer);

        ((MediaBuffer *)mediaBuffer)->release();
        mediaBuffer = NULL;

        bufferMeta->setPointer("mediaBuffer", NULL);
    }

    PortMode mode = getPortMode(kPortIndexInput);

    switch (mode) {
        case KEEP_BUFFERS:
            break;

        case RESUBMIT_BUFFERS:
            postFillThisBuffer(info);
            break;

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);
            TRESPASS();  // Not currently used
            break;
        }
    }

    return true;
}

void DashCodec::BaseState::postFillThisBuffer(BufferInfo *info) {
    if (mCodec->mPortEOS[kPortIndexInput]) {
        return;
    }

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatFillThisBuffer);
    notify->setInt32("buffer-id", info->mBufferID);

    info->mData->meta()->clear();
    notify->setBuffer("buffer", info->mData);

    sp<AMessage> reply = new AMessage(kWhatInputBufferFilled, mCodec->id());
    reply->setInt32("buffer-id", info->mBufferID);

    notify->setMessage("reply", reply);

    notify->post();

    info->mStatus = BufferInfo::OWNED_BY_UPSTREAM;
}

void DashCodec::BaseState::onInputBufferFilled(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id",  (int32_t*)&bufferID));

    sp<ABuffer> buffer;
    int32_t err = OK;
    bool eos = false;

    if (!msg->findBuffer("buffer", &buffer)) {
        CHECK(msg->findInt32("err", &err));

        DC_MSG_LOW("[%s] saw error %d instead of an input buffer",
             mCodec->mComponentName.c_str(), err);

        buffer.clear();

        eos = true;
    }

    int32_t tmp;
    if (buffer != NULL && buffer->meta()->findInt32("eos", &tmp) && tmp) {
        eos = true;
        err = ERROR_END_OF_STREAM;
    }

    BufferInfo *info = mCodec->findBufferByID(kPortIndexInput, bufferID);
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_UPSTREAM);

    info->mStatus = BufferInfo::OWNED_BY_US;

    PortMode mode = getPortMode(kPortIndexInput);

    switch (mode) {
        case KEEP_BUFFERS:
        {
            if (eos) {
                if (!mCodec->mPortEOS[kPortIndexInput]) {
                    mCodec->mPortEOS[kPortIndexInput] = true;
                    mCodec->mInputEOSResult = err;
                }
            }
            break;
        }

        case RESUBMIT_BUFFERS:
        {
            if (buffer != NULL && !mCodec->mPortEOS[kPortIndexInput]) {
                int64_t timeUs;
                CHECK(buffer->meta()->findInt64("timeUs", &timeUs));

                OMX_U32 flags = OMX_BUFFERFLAG_ENDOFFRAME;

                int32_t isCSD;
                if (buffer->meta()->findInt32("csd", &isCSD) && isCSD != 0) {
                    flags |= OMX_BUFFERFLAG_CODECCONFIG;
                }

                if (eos) {
                    flags |= OMX_BUFFERFLAG_EOS;
                }

                if (buffer != info->mData) {
                    DC_MSG_LOW("[%s] Needs to copy input data for buffer %p. (%p != %p)",
                         mCodec->mComponentName.c_str(),
                         bufferID,
                         buffer.get(), info->mData.get());

                    CHECK_LE(buffer->size(), info->mData->capacity());
                    memcpy(info->mData->data(), buffer->data(), buffer->size());
                }

                if (flags & OMX_BUFFERFLAG_CODECCONFIG) {
                    DC_MSG_HIGH("[%s] calling emptyBuffer %p w/ codec specific data",
                         mCodec->mComponentName.c_str(), bufferID);
                } else if (flags & OMX_BUFFERFLAG_EOS) {
                    DC_MSG_HIGH("[%s] calling emptyBuffer %p w/ EOS",
                         mCodec->mComponentName.c_str(), bufferID);
                } else {
#if TRACK_BUFFER_TIMING
                    DC_MSG_HIGH("[%s] calling emptyBuffer %p w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, timeUs);
#else
                    DC_MSG_HIGH("[%s] calling emptyBuffer %p w/ time %lld us",
                         mCodec->mComponentName.c_str(), bufferID, timeUs);
#endif
                }

#if TRACK_BUFFER_TIMING
                DashCodec::BufferStats stats;
                stats.mEmptyBufferTimeUs = ALooper::GetNowUs();
                stats.mFillBufferDoneTimeUs = -1ll;
                mCodec->mBufferStats.add(timeUs, stats);
#endif

                if (mCodec->mStoreMetaDataInOutputBuffers) {
                    // try to submit an output buffer for each input buffer
                    PortMode outputMode = getPortMode(kPortIndexOutput);

                    DC_MSG_HIGH("MetaDataBuffersToSubmit=%u portMode=%s",
                            mCodec->mMetaDataBuffersToSubmit,
                            (outputMode == FREE_BUFFERS ? "FREE" :
                             outputMode == KEEP_BUFFERS ? "KEEP" : "RESUBMIT"));
                    if (outputMode == RESUBMIT_BUFFERS) {
                        CHECK_EQ(mCodec->submitOutputMetaDataBuffer(),
                                (status_t)OK);
                    }
                }

                CHECK_EQ(mCodec->mOMX->emptyBuffer(
                            mCodec->mNode,
                            bufferID,
                            0,
                            buffer->size(),
                            flags,
                            timeUs),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                if (!eos) {
                    getMoreInputDataIfPossible();
                } else {
                    DC_MSG_LOW("[%s] Signalled EOS on the input port",
                         mCodec->mComponentName.c_str());

                    mCodec->mPortEOS[kPortIndexInput] = true;
                    mCodec->mInputEOSResult = err;
                }
            } else if (!mCodec->mPortEOS[kPortIndexInput]) {
                if (err != ERROR_END_OF_STREAM) {
                    DC_MSG_LOW("[%s] Signalling EOS on the input port "
                         "due to error %d",
                         mCodec->mComponentName.c_str(), err);
                } else {
                    DC_MSG_LOW("[%s] Signalling EOS on the input port",
                         mCodec->mComponentName.c_str());
                }

                DC_MSG_HIGH("[%s] calling emptyBuffer %p signalling EOS",
                     mCodec->mComponentName.c_str(), bufferID);

                if (mCodec->mStoreMetaDataInOutputBuffers) {
                    // try to submit an output buffer for each input buffer
                    PortMode outputMode = getPortMode(kPortIndexOutput);

                    DC_MSG_HIGH("MetaDataBuffersToSubmit=%u portMode=%s",
                            mCodec->mMetaDataBuffersToSubmit,
                            (outputMode == FREE_BUFFERS ? "FREE" :
                             outputMode == KEEP_BUFFERS ? "KEEP" : "RESUBMIT"));
                    if (outputMode == RESUBMIT_BUFFERS) {
                        CHECK_EQ(mCodec->submitOutputMetaDataBuffer(),
                                (status_t)OK);
                    }
                }

                CHECK_EQ(mCodec->mOMX->emptyBuffer(
                            mCodec->mNode,
                            bufferID,
                            0,
                            0,
                            OMX_BUFFERFLAG_EOS,
                            0),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;

                mCodec->mPortEOS[kPortIndexInput] = true;
                mCodec->mInputEOSResult = err;
            }
            break;

            default:
                CHECK_EQ((int)mode, (int)FREE_BUFFERS);
                break;
        }
    }
}

void DashCodec::BaseState::getMoreInputDataIfPossible() {
    if (mCodec->mPortEOS[kPortIndexInput]) {
        return;
    }

    BufferInfo *eligible = NULL;

    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(i);

#if 0
        if (info->mStatus == BufferInfo::OWNED_BY_UPSTREAM) {
            // There's already a "read" pending.
            return;
        }
#endif

        if (info->mStatus == BufferInfo::OWNED_BY_US) {
            eligible = info;
        }
    }

    if (eligible == NULL) {
        return;
    }

    postFillThisBuffer(eligible);
}

bool DashCodec::BaseState::onOMXFillBufferDone(
        IOMX::buffer_id bufferID,
        size_t rangeOffset, size_t rangeLength,
        OMX_U32 flags,
        int64_t timeUs,
        void *platformPrivate,
        void *dataPtr) {
    DC_MSG_HIGH("[%s] onOMXFillBufferDone %p time %lld us, flags = 0x%08lx",
         mCodec->mComponentName.c_str(), bufferID, timeUs, flags);

    ssize_t index;

#if TRACK_BUFFER_TIMING
    index = mCodec->mBufferStats.indexOfKey(timeUs);
    if (index >= 0) {
        DashCodec::BufferStats *stats = &mCodec->mBufferStats.editValueAt(index);
        stats->mFillBufferDoneTimeUs = ALooper::GetNowUs();

        DC_MSG_HIGH("frame PTS %lld: %lld",
                timeUs,
                stats->mFillBufferDoneTimeUs - stats->mEmptyBufferTimeUs);

        mCodec->mBufferStats.removeItemsAt(index);
        stats = NULL;
    }
#endif

    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexOutput, bufferID, &index);

    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_COMPONENT);

    info->mDequeuedAt = ++mCodec->mDequeueCounter;
    info->mStatus = BufferInfo::OWNED_BY_US;

    PortMode mode = getPortMode(kPortIndexOutput);

    switch (mode) {
        case KEEP_BUFFERS:
            break;

        case RESUBMIT_BUFFERS:
        {
            if (rangeLength == 0 && !(flags & OMX_BUFFERFLAG_EOS)) {
                DC_MSG_HIGH("[%s] calling fillBuffer %p",
                     mCodec->mComponentName.c_str(), info->mBufferID);

                CHECK_EQ(mCodec->mOMX->fillBuffer(
                            mCodec->mNode, info->mBufferID),
                         (status_t)OK);

                info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                break;
            }

            if (flags & OMX_BUFFERFLAG_EOS) {
                DC_MSG_HIGH("[%s] saw output EOS", mCodec->mComponentName.c_str());

                sp<AMessage> notify = mCodec->mNotify->dup();
                notify->setInt32("what", CodecBase::kWhatEOS);
                notify->setInt32("err", mCodec->mInputEOSResult);
                notify->post();

                mCodec->mPortEOS[kPortIndexOutput] = true;
                break;
            }
            if (!mCodec->mIsEncoder && !mCodec->mSentFormat && !mCodec->mAdaptivePlayback) {
                mCodec->sendFormatChange();
            }

            if (mCodec->mNativeWindow == NULL) {
                info->mData->setRange(rangeOffset, rangeLength);

#if 0
                if (IsIDR(info->mData)) {
                    DC_MSG_HIGH("IDR frame");
                }
#endif
            }

            if (mCodec->mSkipCutBuffer != NULL) {
                mCodec->mSkipCutBuffer->submit(info->mData);
            }
            info->mData->meta()->setInt64("timeUs", timeUs);

            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatDrainThisBuffer);
            notify->setInt32("buffer-id", info->mBufferID);
            notify->setBuffer("buffer", info->mData);
            notify->setInt32("flags", flags);

            if (flags & OMX_BUFFERFLAG_EXTRADATA)
            {
              OMX_U8* bufferHandle = NULL;
              int64_t nFilledLen = 0;
              int64_t nAllocLen = 0;
              int64_t nStartOffset = 0;
              OMX_BUFFERHEADERTYPE *pBufHdr = (OMX_BUFFERHEADERTYPE *)(unsigned long)bufferID;
              nStartOffset = pBufHdr->nOffset;

              if(mCodec->mStoreMetaDataInOutputBuffers)
              {
                VideoDecoderOutputMetaData *metaData =
                              reinterpret_cast<VideoDecoderOutputMetaData *>(
                              pBufHdr->pBuffer);

                bufferHandle = const_cast<OMX_U8*>(
                           reinterpret_cast<const OMX_U8*>(metaData->pHandle));

#ifdef BFAMILY_TARGET /* Venus macros and dynamic mode support present only for B-family targets.*/
                nFilledLen = (VENUS_Y_STRIDE(COLOR_FMT_NV12, mCodec->mCurrentWidth)
                              * VENUS_Y_SCANLINES(COLOR_FMT_NV12, mCodec->mCurrentHeight))
                                    +  (VENUS_UV_STRIDE(COLOR_FMT_NV12, mCodec->mCurrentWidth)
                                        * VENUS_UV_SCANLINES(COLOR_FMT_NV12, mCodec->mCurrentHeight));

                nAllocLen = VENUS_BUFFER_SIZE(COLOR_FMT_NV12, mCodec->mCurrentWidth, mCodec->mCurrentHeight);
#endif
              }
              else
              {
                bufferHandle = const_cast<OMX_U8*>(
                           reinterpret_cast<const OMX_U8*>(pBufHdr->pBuffer));

                nFilledLen = pBufHdr->nFilledLen;
                nAllocLen = pBufHdr->nAllocLen;
              }

              DC_MSG_HIGH("[%s] Extradata present in decoded buffer."
                  "gralloc handle = %p, filled length = %llu, allocated length = %llu, start offset = %llu",
                  mCodec->mComponentName.c_str(), bufferHandle, nFilledLen, nAllocLen, nStartOffset);

              notify->setPointer("gralloc-handle", bufferHandle);
              notify->setInt64("filled-length", nFilledLen);
              notify->setInt64("alloc-length", nAllocLen);
              notify->setInt64("start-offset", nStartOffset);
            }

            sp<AMessage> reply =
                new AMessage(kWhatOutputBufferDrained, mCodec->id());

           if (!mCodec->mPostFormat && mCodec->mAdaptivePlayback){
                   ALOGV("Resolution will change from this buffer, set a flag");
                   reply->setInt32("resChange", 1);
                   mCodec->mPostFormat = true;
            }

            reply->setInt32("buffer-id", info->mBufferID);

            notify->setMessage("reply", reply);

            notify->post();

            info->mStatus = BufferInfo::OWNED_BY_DOWNSTREAM;

            break;
        }

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);

            CHECK_EQ((status_t)OK,
                     mCodec->freeBuffer(kPortIndexOutput, index));
            break;
        }
    }

    return true;
}

void DashCodec::BaseState::onOutputBufferDrained(const sp<AMessage> &msg) {
    IOMX::buffer_id bufferID;
    CHECK(msg->findInt32("buffer-id", (int32_t*)&bufferID));

    ssize_t index;
    BufferInfo *info =
        mCodec->findBufferByID(kPortIndexOutput, bufferID, &index);
    CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_DOWNSTREAM);
    if (mCodec->mAdaptivePlayback) {
        int32_t resChange = 0;
        if (msg->findInt32("resChange", &resChange) && resChange == 1) {
            ALOGV("Resolution change is sent to native window now ");
            mCodec->sendFormatChange();
            msg->setInt32("resChange", 0);
        }
    }
    int32_t render;
    if (mCodec->mNativeWindow != NULL
            && msg->findInt32("render", &render) && render != 0) {
        // The client wants this buffer to be rendered.

        status_t err;
        if ((err = mCodec->mNativeWindow->queueBuffer(
                    mCodec->mNativeWindow.get(),
                    info->mGraphicBuffer.get(), -1)) == OK) {
            info->mStatus = BufferInfo::OWNED_BY_NATIVE_WINDOW;
        } else {
            mCodec->signalError(OMX_ErrorUndefined, err);
            info->mStatus = BufferInfo::OWNED_BY_US;
        }
    } else {
        info->mStatus = BufferInfo::OWNED_BY_US;
    }

    PortMode mode = getPortMode(kPortIndexOutput);

    switch (mode) {
        case KEEP_BUFFERS:
        {
            // XXX fishy, revisit!!! What about the FREE_BUFFERS case below?

            if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                // We cannot resubmit the buffer we just rendered, dequeue
                // the spare instead.

                info = mCodec->dequeueBufferFromNativeWindow();
            }
            break;
        }

        case RESUBMIT_BUFFERS:
        {
            if (!mCodec->mPortEOS[kPortIndexOutput]) {
                if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                    // We cannot resubmit the buffer we just rendered, dequeue
                    // the spare instead.

                    info = mCodec->dequeueBufferFromNativeWindow();
                }

                if (info != NULL) {
                    DC_MSG_HIGH("[%s] calling fillBuffer %p",
                         mCodec->mComponentName.c_str(), info->mBufferID);

                    CHECK_EQ(mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID),
                             (status_t)OK);

                    info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
                }
            }
            break;
        }

        default:
        {
            CHECK_EQ((int)mode, (int)FREE_BUFFERS);

            CHECK_EQ((status_t)OK,
                     mCodec->freeBuffer(kPortIndexOutput, index));
            break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::UninitializedState::UninitializedState(DashCodec *codec)
    : BaseState(codec) {
}

void DashCodec::UninitializedState::stateEntered() {
    DC_MSG_LOW("Now uninitialized");
}

bool DashCodec::UninitializedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case DashCodec::kWhatSetup:
        {
            onSetup(msg);

            handled = true;
            break;
        }

        case DashCodec::kWhatAllocateComponent:
        {
            onAllocateComponent(msg);
            handled = true;
            break;
        }

        case DashCodec::kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));
            CHECK(!keepComponentAllocated);

            sp<AMessage> notify = mCodec->mNotify->dup();
            notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
            notify->post();

            handled = true;
            break;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }

    return handled;
}

void DashCodec::UninitializedState::onSetup(
        const sp<AMessage> &msg) {
    if (onAllocateComponent(msg)
            && mCodec->mLoadedState->onConfigureComponent(msg)) {
        mCodec->mLoadedState->onStart();
    }
}

bool DashCodec::UninitializedState::onAllocateComponent(const sp<AMessage> &msg) {
    DC_MSG_LOW("onAllocateComponent");

    CHECK(mCodec->mNode == 0);

    OMXClient client;
    CHECK_EQ(client.connect(), (status_t)OK);

    sp<IOMX> omx = client.interface();

    Vector<OMXCodec::CodecNameAndQuirks> matchingCodecs;

    AString mime;

    AString componentName;
    uint32_t quirks = 0;
    if (msg->findString("componentName", &componentName)) {
        ssize_t index = matchingCodecs.add();
        OMXCodec::CodecNameAndQuirks *entry = &matchingCodecs.editItemAt(index);
        entry->mName = String8(componentName.c_str());

        if (!OMXCodec::findCodecQuirks(
                    componentName.c_str(), &entry->mQuirks)) {
            entry->mQuirks = 0;
        }
    } else {
        CHECK(msg->findString("mime", &mime));

        int32_t encoder;
        if (!msg->findInt32("encoder", &encoder)) {
            encoder = false;
        }

        OMXCodec::findMatchingCodecs(
                mime.c_str(),
                encoder, // createEncoder
                NULL,  // matchComponentName
                0,     // flags
                &matchingCodecs);
    }

    sp<CodecObserver> observer = new CodecObserver;
    IOMX::node_id node = 0;

    for (size_t matchIndex = 0; matchIndex < matchingCodecs.size();
            ++matchIndex) {
        componentName = matchingCodecs.itemAt(matchIndex).mName.string();
        quirks = matchingCodecs.itemAt(matchIndex).mQuirks;

        pid_t tid = androidGetTid();
        int prevPriority = androidGetThreadPriority(tid);
        androidSetThreadPriority(tid, ANDROID_PRIORITY_FOREGROUND);
        status_t err = omx->allocateNode(componentName.c_str(), observer, &node);
        androidSetThreadPriority(tid, prevPriority);

        if (err == OK) {
            break;
        }

        node = 0;
    }

    if (node == 0) {
        if (!mime.empty()) {
            DC_MSG_ERROR("Unable to instantiate a decoder for type '%s'.",
                 mime.c_str());
        } else {
            DC_MSG_ERROR("Unable to instantiate decoder '%s'.", componentName.c_str());
        }

        mCodec->signalError(OMX_ErrorComponentNotFound);
        return false;
    }

    sp<AMessage> notify = new AMessage(kWhatOMXMessage, mCodec->id());
    observer->setNotificationMessage(notify);

    mCodec->mComponentName = componentName;
    mCodec->mFlags = 0;

    if (componentName.endsWith(".secure")) {
        mCodec->mFlags |= kFlagIsSecure;
        mCodec->mFlags |= kFlagPushBlankBuffersToNativeWindowOnShutdown;
    }

    mCodec->mQuirks = quirks;
    mCodec->mOMX = omx;
    mCodec->mNode = node;

    mCodec->mPortEOS[kPortIndexInput] =
        mCodec->mPortEOS[kPortIndexOutput] = false;

    mCodec->mInputEOSResult = OK;

    {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", DashCodec::kWhatComponentAllocated);
        notify->setString("componentName", mCodec->mComponentName.c_str());
        notify->post();
    }

    mCodec->changeState(mCodec->mLoadedState);

    return true;
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::LoadedState::LoadedState(DashCodec *codec)
    : BaseState(codec) {
}

void DashCodec::LoadedState::stateEntered() {
    DC_MSG_LOW("[%s] Now Loaded", mCodec->mComponentName.c_str());

    mCodec->mDequeueCounter = 0;
    mCodec->mMetaDataBuffersToSubmit = 0;

    if (mCodec->mShutdownInProgress) {
        bool keepComponentAllocated = mCodec->mKeepComponentAllocated;

        mCodec->mShutdownInProgress = false;
        mCodec->mKeepComponentAllocated = false;

        onShutdown(keepComponentAllocated);
    }
}

void DashCodec::LoadedState::onShutdown(bool keepComponentAllocated) {
    if (!keepComponentAllocated) {
        CHECK_EQ(mCodec->mOMX->freeNode(mCodec->mNode), (status_t)OK);

        mCodec->mNativeWindow.clear();
        mCodec->mNode = 0;
        mCodec->mOMX.clear();
        mCodec->mQuirks = 0;
        mCodec->mFlags = 0;
        mCodec->mComponentName.clear();

        mCodec->changeState(mCodec->mUninitializedState);
    }

    sp<AMessage> notify = mCodec->mNotify->dup();
    notify->setInt32("what", CodecBase::kWhatShutdownCompleted);
    notify->post();
}

bool DashCodec::LoadedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case DashCodec::kWhatConfigureComponent:
        {
            onConfigureComponent(msg);
            handled = true;
            break;
        }

        case DashCodec::kWhatStart:
        {
            onStart();
            handled = true;
            break;
        }

        case DashCodec::kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));

            onShutdown(keepComponentAllocated);

            handled = true;
            break;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }

    return handled;
}

bool DashCodec::LoadedState::onConfigureComponent(
        const sp<AMessage> &msg) {
    DC_MSG_LOW("onConfigureComponent");

    CHECK(mCodec->mNode != 0);

    int32_t value;

    if (msg->findInt32("secure-op", &value) && (value == 1)) {
        mCodec->mFlags |= kFlagIsSecureOPOnly;
    }

    AString mime;
    CHECK(msg->findString("mime", &mime));

    status_t err = mCodec->configureCodec(mime.c_str(), msg);

    if (err != OK) {
        DC_MSG_ERROR("[%s] configureCodec returning error %d",
              mCodec->mComponentName.c_str(), err);

        mCodec->signalError(OMX_ErrorUndefined, err);
        return false;
    }

    sp<RefBase> obj;
    if (msg->findObject("native-window", &obj)
            && strncmp("OMX.google.", mCodec->mComponentName.c_str(), 11)) {
        sp<NativeWindowWrapper> nativeWindow(
                static_cast<NativeWindowWrapper *>(obj.get()));
        CHECK(nativeWindow != NULL);
        mCodec->mNativeWindow = nativeWindow->getNativeWindow();

        native_window_set_scaling_mode(
                mCodec->mNativeWindow.get(),
                NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    }
    CHECK_EQ((status_t)OK, mCodec->initNativeWindow());

    {
        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", DashCodec::kWhatComponentConfigured);
        notify->post();
    }

    return true;
}

void DashCodec::LoadedState::onStart() {
    DC_MSG_LOW("onStart");

    CHECK_EQ(mCodec->mOMX->sendCommand(
                mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle),
             (status_t)OK);

    mCodec->changeState(mCodec->mLoadedToIdleState);
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::LoadedToIdleState::LoadedToIdleState(DashCodec *codec)
    : BaseState(codec) {
}

void DashCodec::LoadedToIdleState::stateEntered() {
    DC_MSG_LOW("[%s] Now Loaded->Idle", mCodec->mComponentName.c_str());

    status_t err;
    if ((err = allocateBuffers()) != OK) {
        DC_MSG_ERROR("Failed to allocate buffers after transitioning to IDLE state "
             "(error 0x%08x)",
             err);

        mCodec->signalError(OMX_ErrorUndefined, err);

        mCodec->changeState(mCodec->mLoadedState);
    }
}

status_t DashCodec::LoadedToIdleState::allocateBuffers() {
    status_t err = mCodec->allocateBuffersOnPort(kPortIndexInput);

    if (err != OK) {
        return err;
    }

    return mCodec->allocateBuffersOnPort(kPortIndexOutput);
}

bool DashCodec::LoadedToIdleState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            return true;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }
}

bool DashCodec::LoadedToIdleState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateIdle);

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandStateSet, OMX_StateExecuting),
                     (status_t)OK);

            mCodec->changeState(mCodec->mIdleToExecutingState);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::IdleToExecutingState::IdleToExecutingState(DashCodec *codec)
    : BaseState(codec) {
}

void DashCodec::IdleToExecutingState::stateEntered() {
    DC_MSG_LOW("[%s] Now Idle->Executing", mCodec->mComponentName.c_str());
}

bool DashCodec::IdleToExecutingState::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            return true;
        }

        default:
            return BaseState::onMessageReceived(msg);
    }
}

bool DashCodec::IdleToExecutingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateExecuting);

            mCodec->mExecutingState->resume();
            mCodec->changeState(mCodec->mExecutingState);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::ExecutingState::ExecutingState(DashCodec *codec)
    : BaseState(codec),
      mActive(false) {
}

DashCodec::BaseState::PortMode DashCodec::ExecutingState::getPortMode(
        OMX_U32 portIndex) {
    return RESUBMIT_BUFFERS;
}

void DashCodec::ExecutingState::submitOutputMetaBuffers() {
    // submit as many buffers as there are input buffers with the codec
    // in case we are in port reconfiguring
    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexInput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(i);

        if (info->mStatus == BufferInfo::OWNED_BY_COMPONENT) {
            if (mCodec->submitOutputMetaDataBuffer() != OK)
                break;
        }
    }
}

void DashCodec::ExecutingState::submitRegularOutputBuffers() {
    for (size_t i = 0; i < mCodec->mBuffers[kPortIndexOutput].size(); ++i) {
        BufferInfo *info = &mCodec->mBuffers[kPortIndexOutput].editItemAt(i);

        if (mCodec->mNativeWindow != NULL) {
            CHECK(info->mStatus == BufferInfo::OWNED_BY_US
                    || info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW);

            if (info->mStatus == BufferInfo::OWNED_BY_NATIVE_WINDOW) {
                continue;
            }
        } else {
            CHECK_EQ((int)info->mStatus, (int)BufferInfo::OWNED_BY_US);
        }

        DC_MSG_HIGH("[%s] calling fillBuffer %p",
             mCodec->mComponentName.c_str(), info->mBufferID);

        CHECK_EQ(mCodec->mOMX->fillBuffer(mCodec->mNode, info->mBufferID),
                 (status_t)OK);

        info->mStatus = BufferInfo::OWNED_BY_COMPONENT;
    }
}

void DashCodec::ExecutingState::submitOutputBuffers() {
    submitRegularOutputBuffers();
    if (mCodec->mStoreMetaDataInOutputBuffers) {
        submitOutputMetaBuffers();
    }
}

void DashCodec::ExecutingState::resume() {
    if (mActive) {
        DC_MSG_LOW("[%s] We're already active, no need to resume.",
             mCodec->mComponentName.c_str());

        return;
    }

    submitOutputBuffers();

    // Post the first input buffer.
    CHECK_GT(mCodec->mBuffers[kPortIndexInput].size(), 0u);
    BufferInfo *info = &mCodec->mBuffers[kPortIndexInput].editItemAt(0);

    postFillThisBuffer(info);

    mActive = true;
}

void DashCodec::ExecutingState::stateEntered() {
    DC_MSG_LOW("[%s] Now Executing", mCodec->mComponentName.c_str());

    mCodec->processDeferredMessages();
}

bool DashCodec::ExecutingState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            int32_t keepComponentAllocated;
            CHECK(msg->findInt32(
                        "keepComponentAllocated", &keepComponentAllocated));

            mCodec->mShutdownInProgress = true;
            mCodec->mKeepComponentAllocated = keepComponentAllocated;

            mActive = false;

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandStateSet, OMX_StateIdle),
                     (status_t)OK);

            mCodec->changeState(mCodec->mExecutingToIdleState);

            handled = true;
            break;
        }

        case kWhatFlush:
        {
            DC_MSG_LOW("[%s] ExecutingState flushing now "
                 "(codec owns %d/%d input, %d/%d output).",
                    mCodec->mComponentName.c_str(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexInput),
                    mCodec->mBuffers[kPortIndexInput].size(),
                    mCodec->countBuffersOwnedByComponent(kPortIndexOutput),
                    mCodec->mBuffers[kPortIndexOutput].size());

            mActive = false;

            CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandFlush, OMX_ALL),
                     (status_t)OK);

            mCodec->changeState(mCodec->mFlushingState);
            mCodec->clearCachedFormats();
            handled = true;
            break;
        }

        case kWhatResume:
        {
            resume();

            handled = true;
            break;
        }

        case kWhatRequestIDRFrame:
        {
            status_t err = mCodec->requestIDRFrame();
            if (err != OK) {
                DC_MSG_HIGH("Requesting an IDR frame failed.");
            }

            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

bool DashCodec::ExecutingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventPortSettingsChanged:
        {
            CHECK_EQ(data1, (OMX_U32)kPortIndexOutput);

            if (data2 == 0 || data2 == OMX_IndexParamPortDefinition) {
                if (mCodec->mStoreMetaDataInOutputBuffers) {
                    DC_MSG_ERROR("[%s] storeMetaDataInBuffers:Rcvd port settings changed event..disabling outputport",                          mCodec->mComponentName.c_str());
                mCodec->mMetaDataBuffersToSubmit = 0;
                    CHECK_EQ(mCodec->mOMX->sendCommand(
                                mCodec->mNode,
                                OMX_CommandPortDisable, kPortIndexOutput),
                             (status_t)OK);

                    mCodec->freeOutputBuffersNotOwnedByComponent();
                    mCodec->changeState(mCodec->mOutputPortSettingsChangedState);
                }else {
                  ALOGV("Flush output port before disable");
                  CHECK_EQ(mCodec->mOMX->sendCommand(
                        mCodec->mNode, OMX_CommandFlush, kPortIndexOutput),
                     (status_t)OK);

                   mCodec->changeState(mCodec->mFlushingOutputState);
                }

            } else if (data2 == OMX_IndexConfigCommonOutputCrop) {
                mCodec->mSentFormat = false;
                mCodec->mPostFormat = false;
                mCodec->queueNextFormat();
            } else {
                DC_MSG_LOW("[%s] OMX_EventPortSettingsChanged 0x%08lx",
                     mCodec->mComponentName.c_str(), data2);
            }

            return true;
        }
        case OMX_EventIndexsettingChanged:
        {
            DC_MSG_HIGH("[%s] Received OMX_EventIndexsettingChanged event ", mCodec->mComponentName.c_str());
            mCodec->mSentFormat = false;
            return true;
        }
        case OMX_EventBufferFlag:
        {
            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::OutputPortSettingsChangedState::OutputPortSettingsChangedState(
        DashCodec *codec)
    : BaseState(codec) {
}

DashCodec::BaseState::PortMode DashCodec::OutputPortSettingsChangedState::getPortMode(
        OMX_U32 portIndex) {
    if (portIndex == kPortIndexOutput) {
        return FREE_BUFFERS;
    }

    CHECK_EQ(portIndex, (OMX_U32)kPortIndexInput);

    return RESUBMIT_BUFFERS;
}

bool DashCodec::OutputPortSettingsChangedState::onMessageReceived(
        const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatFlush:
        case kWhatShutdown:
        case kWhatResume:
        {
            if (msg->what() == kWhatResume) {
                DC_MSG_LOW("[%s] Deferring resume", mCodec->mComponentName.c_str());
            }

            mCodec->deferMessage(msg);
            handled = true;
            break;
        }
        case kWhatWaitForPortEnable:
        {
            int32_t d1;
            int32_t d2;
            CHECK(msg->findInt32("data1", &d1));
            CHECK(msg->findInt32("data2", &d2));

            enableOutputPort((OMX_U32)d1, (OMX_U32)d2);

            handled = true;
            break;
        }
        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void DashCodec::OutputPortSettingsChangedState::enableOutputPort(OMX_U32 data1, OMX_U32 data2) {
     if (data1 == (OMX_U32)OMX_CommandPortDisable) {
          CHECK_EQ(data2, (OMX_U32)kPortIndexOutput);

          DC_MSG_ERROR("[%s] Output port now disabled.",
                      mCodec->mComponentName.c_str());
          // All buffers should get freed before sending re-enabling outport after
	  // portsetting change. OUT Buffers OWNED_BY_DOWNSTREAM, gets  freed
	  // only when renderer sends buffer for rendering.
	  // Due to timing issue, buffers OWNED_BY_DOWNSTREAM dosent get freed
	  // before exectution reaches here. hence wait for all such buffers
	  // to get free and then proceed for enabling outport.
          if(!mCodec->mBuffers[kPortIndexOutput].isEmpty())
          {
             DC_MSG_ERROR(" Wait for outport Queue to be empty before re-enable port ");
             sp<AMessage> msg = new AMessage(kWhatWaitForPortEnable, mCodec->id());
             msg->setInt32("data1", data1);
             msg->setInt32("data2", data2);
             msg->post(10000ll); // poll again after 10ms
             return;
          }

          mCodec->mDealer[kPortIndexOutput].clear();

          CHECK_EQ(mCodec->mOMX->sendCommand(
                   mCodec->mNode, OMX_CommandPortEnable, kPortIndexOutput),
                   (status_t)OK);

          status_t err;
          if ((err = mCodec->allocateBuffersOnPort(kPortIndexOutput)) != OK) {
            DC_MSG_ERROR("Failed to allocate output port buffers after "
                  "port reconfiguration (error 0x%08x)",
                   err);

            mCodec->signalError(OMX_ErrorUndefined, err);

            // This is technically not correct, but appears to be
            // the only way to free the component instance.
            // Controlled transitioning from excecuting->idle
            // and idle->loaded seem impossible probably because
            // the output port never finishes re-enabling.
            mCodec->mShutdownInProgress = true;
            mCodec->mKeepComponentAllocated = false;
            mCodec->changeState(mCodec->mLoadedState);
        }
    }

    return;
}

void DashCodec::OutputPortSettingsChangedState::stateEntered() {
    DC_MSG_LOW("[%s] Now handling output port settings change",
         mCodec->mComponentName.c_str());
}

bool DashCodec::OutputPortSettingsChangedState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            if (data1 == (OMX_U32)OMX_CommandPortDisable) {
                enableOutputPort(data1, data2);
                return true;
            } else if (data1 == (OMX_U32)OMX_CommandPortEnable) {
                CHECK_EQ(data2, (OMX_U32)kPortIndexOutput);

                mCodec->mSentFormat = false;

                DC_MSG_LOW("[%s] Output port now reenabled.",
                        mCodec->mComponentName.c_str());

                if (mCodec->mExecutingState->active()) {
                    mCodec->mExecutingState->submitOutputBuffers();
                }

                mCodec->changeState(mCodec->mExecutingState);

                return true;
            }

            return false;
        }

        default:
            return false;
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::ExecutingToIdleState::ExecutingToIdleState(DashCodec *codec)
    : BaseState(codec),
      mComponentNowIdle(false) {
}

bool DashCodec::ExecutingToIdleState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            // We're already doing that...

            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void DashCodec::ExecutingToIdleState::stateEntered() {
    DC_MSG_LOW("[%s] Now Executing->Idle", mCodec->mComponentName.c_str());

    mComponentNowIdle = false;
    mCodec->mSentFormat = false;
}

bool DashCodec::ExecutingToIdleState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateIdle);

            mComponentNowIdle = true;

            changeStateIfWeOwnAllBuffers();

            return true;
        }

        case OMX_EventPortSettingsChanged:
        case OMX_EventBufferFlag:
        {
            // We're shutting down and don't care about this anymore.
            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

void DashCodec::ExecutingToIdleState::changeStateIfWeOwnAllBuffers() {
    if (mComponentNowIdle && mCodec->allYourBuffersAreBelongToUs()) {
        CHECK_EQ(mCodec->mOMX->sendCommand(
                    mCodec->mNode, OMX_CommandStateSet, OMX_StateLoaded),
                 (status_t)OK);

        CHECK_EQ(mCodec->freeBuffersOnPort(kPortIndexInput), (status_t)OK);
        CHECK_EQ(mCodec->freeBuffersOnPort(kPortIndexOutput), (status_t)OK);

        if (mCodec->mFlags & kFlagPushBlankBuffersToNativeWindowOnShutdown
               && mCodec->mNativeWindow != NULL) {
            // We push enough 1x1 blank buffers to ensure that one of
            // them has made it to the display.  This allows the OMX
            // component teardown to zero out any protected buffers
            // without the risk of scanning out one of those buffers.
            PushBlankBuffersToNativeWindow(mCodec->mNativeWindow);
        }

        mCodec->changeState(mCodec->mIdleToLoadedState);
    }
}

void DashCodec::ExecutingToIdleState::onInputBufferFilled(
        const sp<AMessage> &msg) {
    BaseState::onInputBufferFilled(msg);

    changeStateIfWeOwnAllBuffers();
}

void DashCodec::ExecutingToIdleState::onOutputBufferDrained(
        const sp<AMessage> &msg) {
    BaseState::onOutputBufferDrained(msg);

    changeStateIfWeOwnAllBuffers();
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::IdleToLoadedState::IdleToLoadedState(DashCodec *codec)
    : BaseState(codec) {
}

bool DashCodec::IdleToLoadedState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            // We're already doing that...

            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

void DashCodec::IdleToLoadedState::stateEntered() {
    DC_MSG_LOW("[%s] Now Idle->Loaded", mCodec->mComponentName.c_str());
}

bool DashCodec::IdleToLoadedState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandStateSet);
            CHECK_EQ(data2, (OMX_U32)OMX_StateLoaded);

            mCodec->changeState(mCodec->mLoadedState);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::FlushingState::FlushingState(DashCodec *codec)
    : BaseState(codec) {
}

void DashCodec::FlushingState::stateEntered() {
    DC_MSG_LOW("[%s] Now Flushing", mCodec->mComponentName.c_str());

    mFlushComplete[kPortIndexInput] = mFlushComplete[kPortIndexOutput] = false;
}

bool DashCodec::FlushingState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            break;
        }

        case kWhatFlush:
        {
            // We're already doing this right now.
            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

bool DashCodec::FlushingState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    DC_MSG_LOW("[%s] FlushingState onOMXEvent(%d,%ld)",
            mCodec->mComponentName.c_str(), event, data1);

    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandFlush);

            if (data2 == kPortIndexInput || data2 == kPortIndexOutput) {
                CHECK(!mFlushComplete[data2]);
                mFlushComplete[data2] = true;

                if (mFlushComplete[kPortIndexInput]
                        && mFlushComplete[kPortIndexOutput]) {
                    changeStateIfWeOwnAllBuffers();
                }
            } else {
                CHECK_EQ(data2, OMX_ALL);
                CHECK(mFlushComplete[kPortIndexInput]);
                CHECK(mFlushComplete[kPortIndexOutput]);

                changeStateIfWeOwnAllBuffers();
            }

            return true;
        }

        case OMX_EventPortSettingsChanged:
        {
            sp<AMessage> msg = new AMessage(kWhatOMXMessage, mCodec->id());
            msg->setInt32("type", omx_message::EVENT);
            msg->setInt32("node", mCodec->mNode);
            msg->setInt32("event", event);
            msg->setInt32("data1", data1);
            msg->setInt32("data2", data2);

            DC_MSG_LOW("[%s] Deferring OMX_EventPortSettingsChanged",
                 mCodec->mComponentName.c_str());

            mCodec->deferMessage(msg);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }

    return true;
}

void DashCodec::FlushingState::onOutputBufferDrained(const sp<AMessage> &msg) {
    BaseState::onOutputBufferDrained(msg);

    changeStateIfWeOwnAllBuffers();
}

void DashCodec::FlushingState::onInputBufferFilled(const sp<AMessage> &msg) {
    BaseState::onInputBufferFilled(msg);

    changeStateIfWeOwnAllBuffers();
}

void DashCodec::FlushingState::changeStateIfWeOwnAllBuffers() {
    if (mFlushComplete[kPortIndexInput]
            && mFlushComplete[kPortIndexOutput]
            && mCodec->allYourBuffersAreBelongToUs()) {
        // We now own all buffers except possibly those still queued with
        // the native window for rendering. Let's get those back as well.
        mCodec->waitUntilAllPossibleNativeWindowBuffersAreReturnedToUs();

        sp<AMessage> notify = mCodec->mNotify->dup();
        notify->setInt32("what", CodecBase::kWhatFlushCompleted);
        notify->post();

        mCodec->mPortEOS[kPortIndexInput] =
            mCodec->mPortEOS[kPortIndexOutput] = false;

        mCodec->mInputEOSResult = OK;

        mCodec->changeState(mCodec->mExecutingState);
    }
}

////////////////////////////////////////////////////////////////////////////////

DashCodec::FlushingOutputState::FlushingOutputState(DashCodec *codec)
    : BaseState(codec) {
}

DashCodec::BaseState::PortMode DashCodec::FlushingOutputState::getPortMode(OMX_U32 portIndex) {
    if (portIndex == kPortIndexOutput)
    {
        return KEEP_BUFFERS;
    }
    return RESUBMIT_BUFFERS;
}

void DashCodec::FlushingOutputState::stateEntered() {
    DC_MSG_LOW("[%s] Now Flushing Output Port", mCodec->mComponentName.c_str());

    mFlushComplete = false;
}

bool DashCodec::FlushingOutputState::onMessageReceived(const sp<AMessage> &msg) {
    bool handled = false;

    switch (msg->what()) {
        case kWhatShutdown:
        {
            mCodec->deferMessage(msg);
            handled = true;
            break;
        }

        case kWhatFlush:
        {
            DC_MSG_LOW("Flush received during port reconfig, deferring it");
            mCodec->deferMessage(msg);
            handled = true;
            break;
        }

        case kWhatInputBufferFilled:
        {
            mCodec->deferMessage(msg);
            changeStateIfWeOwnAllBuffers();
            handled = true;
            break;
        }

        default:
            handled = BaseState::onMessageReceived(msg);
            break;
    }

    return handled;
}

bool DashCodec::FlushingOutputState::onOMXEvent(
        OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2) {
    switch (event) {
        case OMX_EventCmdComplete:
        {
            CHECK_EQ(data1, (OMX_U32)OMX_CommandFlush);
            CHECK_EQ(data2,(OMX_U32)kPortIndexOutput);
            DC_MSG_LOW("FlushingOutputState::onOMXEvent Output port flush complete");
            mFlushComplete = true;
            changeStateIfWeOwnAllBuffers();
            return true;
        }

        case OMX_EventPortSettingsChanged:
        {
            sp<AMessage> msg = new AMessage(kWhatOMXMessage, mCodec->id());
            msg->setInt32("type", omx_message::EVENT);
            msg->setInt32("node", mCodec->mNode);
            msg->setInt32("event", event);
            msg->setInt32("data1", data1);
            msg->setInt32("data2", data2);

            DC_MSG_LOW("[%s] Deferring OMX_EventPortSettingsChanged",
                 mCodec->mComponentName.c_str());

            mCodec->deferMessage(msg);

            return true;
        }

        default:
            return BaseState::onOMXEvent(event, data1, data2);
    }

    return true;
}

void DashCodec::FlushingOutputState::onOutputBufferDrained(const sp<AMessage> &msg) {
    BaseState::onOutputBufferDrained(msg);

    changeStateIfWeOwnAllBuffers();
}

void DashCodec::FlushingOutputState::onInputBufferFilled(const sp<AMessage> &msg) {
    BaseState::onInputBufferFilled(msg);

    changeStateIfWeOwnAllBuffers();
}

void DashCodec::FlushingOutputState::changeStateIfWeOwnAllBuffers() {
   DC_MSG_LOW("FlushingOutputState::ChangeState %d",mFlushComplete);

   if (mFlushComplete && mCodec->allYourBuffersAreBelongToUs( kPortIndexOutput )) {
        DC_MSG_LOW("FlushingOutputState Sending port disable ");
        CHECK_EQ(mCodec->mOMX->sendCommand(
                            mCodec->mNode,
                            OMX_CommandPortDisable, kPortIndexOutput),
                         (status_t)OK);

        mCodec->mPortEOS[kPortIndexInput] = false;
        mCodec->mPortEOS[kPortIndexOutput] = false;

        DC_MSG_LOW("FlushingOutputState Calling freeOutputBuffersNotOwnedByComponent");
        mCodec->freeOutputBuffersNotOwnedByComponent();

        DC_MSG_LOW("FlushingOutputState Change state to port settings changed");
        mCodec->changeState(mCodec->mOutputPortSettingsChangedState);
    }
}

}  // namespace android
