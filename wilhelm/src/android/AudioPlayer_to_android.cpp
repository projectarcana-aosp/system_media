/*
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

#include "sles_allinclusive.h"
#include "android/AndroidBufferQueueSource.h"
#include "utils/RefBase.h"
#include "android_prompts.h"

template class android::KeyedVector<SLuint32, android::AudioEffect* > ;

#define KEY_STREAM_TYPE_PARAMSIZE  sizeof(SLint32)

//-----------------------------------------------------------------------------
// FIXME this method will be absorbed into android_audioPlayer_setPlayState() once
//       bufferqueue and uri/fd playback are moved under the GenericPlayer C++ object
SLresult aplayer_setPlayState(const android::sp<android::GenericPlayer> &ap, SLuint32 playState,
        AndroidObject_state* pObjState) {
    SLresult result = SL_RESULT_SUCCESS;
    AndroidObject_state objState = *pObjState;

    switch (playState) {
     case SL_PLAYSTATE_STOPPED:
         SL_LOGV("setting GenericPlayer to SL_PLAYSTATE_STOPPED");
         ap->stop();
         break;
     case SL_PLAYSTATE_PAUSED:
         SL_LOGV("setting GenericPlayer to SL_PLAYSTATE_PAUSED");
         switch(objState) {
         case ANDROID_UNINITIALIZED:
             *pObjState = ANDROID_PREPARING;
             ap->prepare();
             break;
         case ANDROID_PREPARING:
             break;
         case ANDROID_READY:
             ap->pause();
             break;
         default:
             SL_LOGE(ERROR_PLAYERSETPLAYSTATE_INVALID_OBJECT_STATE_D, playState);
             result = SL_RESULT_INTERNAL_ERROR;
             break;
         }
         break;
     case SL_PLAYSTATE_PLAYING: {
         SL_LOGV("setting GenericPlayer to SL_PLAYSTATE_PLAYING");
         switch(objState) {
         case ANDROID_UNINITIALIZED:
             *pObjState = ANDROID_PREPARING;
             ap->prepare();
             // intended fall through
         case ANDROID_PREPARING:
             // intended fall through
         case ANDROID_READY:
             ap->play();
             break;
         default:
             SL_LOGE(ERROR_PLAYERSETPLAYSTATE_INVALID_OBJECT_STATE_D, playState);
             result = SL_RESULT_INTERNAL_ERROR;
             break;
         }
         }
         break;
     default:
         // checked by caller, should not happen
         SL_LOGE(ERROR_SHOULDNT_BE_HERE_S, "aplayer_setPlayState");
         result = SL_RESULT_INTERNAL_ERROR;
         break;
     }

    return result;
}


//-----------------------------------------------------------------------------
// Callback associated with a AudioToCbRenderer of an SL ES AudioPlayer that gets its data
// from a URI or FD, to write the decoded audio data to a buffer queue
static size_t adecoder_writeToBufferQueue(const uint8_t *data, size_t size, void* user) {
    size_t sizeConsumed = 0;
    if (NULL == user) {
        return sizeConsumed;
    }
    SL_LOGI("received %d bytes from decoder", size);
    CAudioPlayer *ap = (CAudioPlayer *)user;
    slBufferQueueCallback callback = NULL;
    void * callbackPContext = NULL;

    // push decoded data to the buffer queue
    object_lock_exclusive(&ap->mObject);

    if (ap->mBufferQueue.mState.count != 0) {
        assert(ap->mBufferQueue.mFront != ap->mBufferQueue.mRear);

        BufferHeader *oldFront = ap->mBufferQueue.mFront;
        BufferHeader *newFront = &oldFront[1];

        uint8_t *pDest = (uint8_t *)oldFront->mBuffer + ap->mBufferQueue.mSizeConsumed;
        if (ap->mBufferQueue.mSizeConsumed + size < oldFront->mSize) {
            // room to consume the whole or rest of the decoded data in one shot
            ap->mBufferQueue.mSizeConsumed += size;
            // consume data but no callback to the BufferQueue interface here
            memcpy (pDest, data, size);
            sizeConsumed = size;
        } else {
            // push as much as possible of the decoded data into the buffer queue
            sizeConsumed = oldFront->mSize - ap->mBufferQueue.mSizeConsumed;

            // the buffer at the head of the buffer queue is full, update the state
            ap->mBufferQueue.mSizeConsumed = 0;
            if (newFront ==  &ap->mBufferQueue.mArray[ap->mBufferQueue.mNumBuffers + 1]) {
                newFront = ap->mBufferQueue.mArray;
            }
            ap->mBufferQueue.mFront = newFront;

            ap->mBufferQueue.mState.count--;
            ap->mBufferQueue.mState.playIndex++;
            // consume data
            memcpy (pDest, data, sizeConsumed);
            // data has been copied to the buffer, and the buffer queue state has been updated
            // we will notify the client if applicable
            callback = ap->mBufferQueue.mCallback;
            // save callback data
            callbackPContext = ap->mBufferQueue.mContext;
        }

    } else {
        // no available buffers in the queue to write the decoded data
        sizeConsumed = 0;
    }

    object_unlock_exclusive(&ap->mObject);
    // notify client
    if (NULL != callback) {
        (*callback)(&ap->mBufferQueue.mItf, callbackPContext);
    }

    return sizeConsumed;
}

//-----------------------------------------------------------------------------
int android_getMinFrameCount(uint32_t sampleRate) {
    int afSampleRate;
    if (android::AudioSystem::getOutputSamplingRate(&afSampleRate,
            ANDROID_DEFAULT_OUTPUT_STREAM_TYPE) != android::NO_ERROR) {
        return ANDROID_DEFAULT_AUDIOTRACK_BUFFER_SIZE;
    }
    int afFrameCount;
    if (android::AudioSystem::getOutputFrameCount(&afFrameCount,
            ANDROID_DEFAULT_OUTPUT_STREAM_TYPE) != android::NO_ERROR) {
        return ANDROID_DEFAULT_AUDIOTRACK_BUFFER_SIZE;
    }
    uint32_t afLatency;
    if (android::AudioSystem::getOutputLatency(&afLatency,
            ANDROID_DEFAULT_OUTPUT_STREAM_TYPE) != android::NO_ERROR) {
        return ANDROID_DEFAULT_AUDIOTRACK_BUFFER_SIZE;
    }
    // minimum nb of buffers to cover output latency, given the size of each hardware audio buffer
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSampleRate);
    if (minBufCount < 2) minBufCount = 2;
    // minimum number of frames to cover output latency at the sample rate of the content
    return (afFrameCount*sampleRate*minBufCount)/afSampleRate;
}


//-----------------------------------------------------------------------------
#define LEFT_CHANNEL_MASK  0x1 << 0
#define RIGHT_CHANNEL_MASK 0x1 << 1

static void android_audioPlayer_updateStereoVolume(CAudioPlayer* ap) {
    float leftVol = 1.0f, rightVol = 1.0f;

    if (NULL == ap->mAudioTrack) {
        return;
    }
    // should not be used when muted
    if (SL_BOOLEAN_TRUE == ap->mMute) {
        return;
    }

    int channelCount = ap->mNumChannels;

    // mute has priority over solo
    int leftAudibilityFactor = 1, rightAudibilityFactor = 1;

    if (channelCount >= STEREO_CHANNELS) {
        if (ap->mMuteMask & LEFT_CHANNEL_MASK) {
            // left muted
            leftAudibilityFactor = 0;
        } else {
            // left not muted
            if (ap->mSoloMask & LEFT_CHANNEL_MASK) {
                // left soloed
                leftAudibilityFactor = 1;
            } else {
                // left not soloed
                if (ap->mSoloMask & RIGHT_CHANNEL_MASK) {
                    // right solo silences left
                    leftAudibilityFactor = 0;
                } else {
                    // left and right are not soloed, and left is not muted
                    leftAudibilityFactor = 1;
                }
            }
        }

        if (ap->mMuteMask & RIGHT_CHANNEL_MASK) {
            // right muted
            rightAudibilityFactor = 0;
        } else {
            // right not muted
            if (ap->mSoloMask & RIGHT_CHANNEL_MASK) {
                // right soloed
                rightAudibilityFactor = 1;
            } else {
                // right not soloed
                if (ap->mSoloMask & LEFT_CHANNEL_MASK) {
                    // left solo silences right
                    rightAudibilityFactor = 0;
                } else {
                    // left and right are not soloed, and right is not muted
                    rightAudibilityFactor = 1;
                }
            }
        }
    }

    // compute amplification as the combination of volume level and stereo position
    //   amplification from volume level
    ap->mAmplFromVolLevel = sles_to_android_amplification(ap->mVolume.mLevel);
    //   amplification from direct level (changed in SLEffectSendtItf and SLAndroidEffectSendItf)
    leftVol  *= ap->mAmplFromVolLevel * ap->mAmplFromDirectLevel;
    rightVol *= ap->mAmplFromVolLevel * ap->mAmplFromDirectLevel;

    // amplification from stereo position
    if (ap->mVolume.mEnableStereoPosition) {
        // panning law depends on number of channels of content: stereo panning vs 2ch. balance
        if(1 == channelCount) {
            // stereo panning
            double theta = (1000+ap->mVolume.mStereoPosition)*M_PI_4/1000.0f; // 0 <= theta <= Pi/2
            ap->mAmplFromStereoPos[0] = cos(theta);
            ap->mAmplFromStereoPos[1] = sin(theta);
        } else {
            // stereo balance
            if (ap->mVolume.mStereoPosition > 0) {
                ap->mAmplFromStereoPos[0] = (1000-ap->mVolume.mStereoPosition)/1000.0f;
                ap->mAmplFromStereoPos[1] = 1.0f;
            } else {
                ap->mAmplFromStereoPos[0] = 1.0f;
                ap->mAmplFromStereoPos[1] = (1000+ap->mVolume.mStereoPosition)/1000.0f;
            }
        }
        leftVol  *= ap->mAmplFromStereoPos[0];
        rightVol *= ap->mAmplFromStereoPos[1];
    }

    ap->mAudioTrack->setVolume(leftVol * leftAudibilityFactor, rightVol * rightAudibilityFactor);

    // changes in the AudioPlayer volume must be reflected in the send level:
    //  in SLEffectSendItf or in SLAndroidEffectSendItf?
    // FIXME replace interface test by an internal API once we have one.
    if (NULL != ap->mEffectSend.mItf) {
        for (unsigned int i=0 ; i<AUX_MAX ; i++) {
            if (ap->mEffectSend.mEnableLevels[i].mEnable) {
                android_fxSend_setSendLevel(ap,
                        ap->mEffectSend.mEnableLevels[i].mSendLevel + ap->mVolume.mLevel);
                // there's a single aux bus on Android, so we can stop looking once the first
                // aux effect is found.
                break;
            }
        }
    } else if (NULL != ap->mAndroidEffectSend.mItf) {
        android_fxSend_setSendLevel(ap, ap->mAndroidEffectSend.mSendLevel + ap->mVolume.mLevel);
    }
}

//-----------------------------------------------------------------------------
void audioTrack_handleMarker_lockPlay(CAudioPlayer* ap) {
    //SL_LOGV("received event EVENT_MARKER from AudioTrack");
    slPlayCallback callback = NULL;
    void* callbackPContext = NULL;

    interface_lock_shared(&ap->mPlay);
    callback = ap->mPlay.mCallback;
    callbackPContext = ap->mPlay.mContext;
    interface_unlock_shared(&ap->mPlay);

    if (NULL != callback) {
        // getting this event implies SL_PLAYEVENT_HEADATMARKER was set in the event mask
        (*callback)(&ap->mPlay.mItf, callbackPContext, SL_PLAYEVENT_HEADATMARKER);
    }
}

//-----------------------------------------------------------------------------
void audioTrack_handleNewPos_lockPlay(CAudioPlayer* ap) {
    //SL_LOGV("received event EVENT_NEW_POS from AudioTrack");
    slPlayCallback callback = NULL;
    void* callbackPContext = NULL;

    interface_lock_shared(&ap->mPlay);
    callback = ap->mPlay.mCallback;
    callbackPContext = ap->mPlay.mContext;
    interface_unlock_shared(&ap->mPlay);

    if (NULL != callback) {
        // getting this event implies SL_PLAYEVENT_HEADATNEWPOS was set in the event mask
        (*callback)(&ap->mPlay.mItf, callbackPContext, SL_PLAYEVENT_HEADATNEWPOS);
    }
}


//-----------------------------------------------------------------------------
void audioTrack_handleUnderrun_lockPlay(CAudioPlayer* ap) {
    slPlayCallback callback = NULL;
    void* callbackPContext = NULL;

    interface_lock_shared(&ap->mPlay);
    callback = ap->mPlay.mCallback;
    callbackPContext = ap->mPlay.mContext;
    bool headStalled = (ap->mPlay.mEventFlags & SL_PLAYEVENT_HEADSTALLED) != 0;
    interface_unlock_shared(&ap->mPlay);

    if ((NULL != callback) && headStalled) {
        (*callback)(&ap->mPlay.mItf, callbackPContext, SL_PLAYEVENT_HEADSTALLED);
    }
}


//-----------------------------------------------------------------------------
/**
 * post-condition: play state of AudioPlayer is SL_PLAYSTATE_PAUSED if setPlayStateToPaused is true
 *
 * note: a conditional flag, setPlayStateToPaused, is used here to specify whether the play state
 *       needs to be changed when the player reaches the end of the content to play. This is
 *       relative to what the specification describes for buffer queues vs the
 *       SL_PLAYEVENT_HEADATEND event. In the OpenSL ES specification 1.0.1:
 *        - section 8.12 SLBufferQueueItf states "In the case of starvation due to insufficient
 *          buffers in the queue, the playing of audio data stops. The player remains in the
 *          SL_PLAYSTATE_PLAYING state."
 *        - section 9.2.31 SL_PLAYEVENT states "SL_PLAYEVENT_HEADATEND Playback head is at the end
 *          of the current content and the player has paused."
 */
void audioPlayer_dispatch_headAtEnd_lockPlay(CAudioPlayer *ap, bool setPlayStateToPaused,
        bool needToLock) {
    //SL_LOGV("ap=%p, setPlayStateToPaused=%d, needToLock=%d", ap, setPlayStateToPaused,
    //        needToLock);
    slPlayCallback playCallback = NULL;
    void * playContext = NULL;
    // SLPlayItf callback or no callback?
    if (needToLock) {
        interface_lock_exclusive(&ap->mPlay);
    }
    if (ap->mPlay.mEventFlags & SL_PLAYEVENT_HEADATEND) {
        playCallback = ap->mPlay.mCallback;
        playContext = ap->mPlay.mContext;
    }
    if (setPlayStateToPaused) {
        ap->mPlay.mState = SL_PLAYSTATE_PAUSED;
    }
    if (needToLock) {
        interface_unlock_exclusive(&ap->mPlay);
    }
    // callback with no lock held
    if (NULL != playCallback) {
        (*playCallback)(&ap->mPlay.mItf, playContext, SL_PLAYEVENT_HEADATEND);
    }

}


//-----------------------------------------------------------------------------
/**
 * pre-condition: AudioPlayer has SLPrefetchStatusItf initialized
 * post-condition:
 *  - ap->mPrefetchStatus.mStatus == status
 *  - the prefetch status callback, if any, has been notified if a change occurred
 *
 */
void audioPlayer_dispatch_prefetchStatus_lockPrefetch(CAudioPlayer *ap, SLuint32 status,
        bool needToLock) {
    slPrefetchCallback prefetchCallback = NULL;
    void * prefetchContext = NULL;

    if (needToLock) {
        interface_lock_exclusive(&ap->mPrefetchStatus);
    }
    // status change?
    if (ap->mPrefetchStatus.mStatus != status) {
        ap->mPrefetchStatus.mStatus = status;
        // callback or no callback?
        if (ap->mPrefetchStatus.mCallbackEventsMask & SL_PREFETCHEVENT_STATUSCHANGE) {
            prefetchCallback = ap->mPrefetchStatus.mCallback;
            prefetchContext  = ap->mPrefetchStatus.mContext;
        }
    }
    if (needToLock) {
        interface_unlock_exclusive(&ap->mPrefetchStatus);
    }

    // callback with no lock held
    if (NULL != prefetchCallback) {
        (*prefetchCallback)(&ap->mPrefetchStatus.mItf, prefetchContext, status);
    }
}


//-----------------------------------------------------------------------------
SLresult audioPlayer_setStreamType(CAudioPlayer* ap, SLint32 type) {
    SLresult result = SL_RESULT_SUCCESS;
    SL_LOGV("type %ld", type);

    int newStreamType = ANDROID_DEFAULT_OUTPUT_STREAM_TYPE;
    switch(type) {
    case SL_ANDROID_STREAM_VOICE:
        newStreamType = android::AudioSystem::VOICE_CALL;
        break;
    case SL_ANDROID_STREAM_SYSTEM:
        newStreamType = android::AudioSystem::SYSTEM;
        break;
    case SL_ANDROID_STREAM_RING:
        newStreamType = android::AudioSystem::RING;
        break;
    case SL_ANDROID_STREAM_MEDIA:
        newStreamType = android::AudioSystem::MUSIC;
        break;
    case SL_ANDROID_STREAM_ALARM:
        newStreamType = android::AudioSystem::ALARM;
        break;
    case SL_ANDROID_STREAM_NOTIFICATION:
        newStreamType = android::AudioSystem::NOTIFICATION;
        break;
    default:
        SL_LOGE(ERROR_PLAYERSTREAMTYPE_SET_UNKNOWN_TYPE);
        result = SL_RESULT_PARAMETER_INVALID;
        break;
    }

    // stream type needs to be set before the object is realized
    // (ap->mAudioTrack is supposed to be NULL until then)
    if (SL_OBJECT_STATE_UNREALIZED != ap->mObject.mState) {
        SL_LOGE(ERROR_PLAYERSTREAMTYPE_REALIZED);
        result = SL_RESULT_PRECONDITIONS_VIOLATED;
    } else {
        ap->mStreamType = newStreamType;
    }

    return result;
}


//-----------------------------------------------------------------------------
SLresult audioPlayer_getStreamType(CAudioPlayer* ap, SLint32 *pType) {
    SLresult result = SL_RESULT_SUCCESS;

    switch(ap->mStreamType) {
    case android::AudioSystem::VOICE_CALL:
        *pType = SL_ANDROID_STREAM_VOICE;
        break;
    case android::AudioSystem::SYSTEM:
        *pType = SL_ANDROID_STREAM_SYSTEM;
        break;
    case android::AudioSystem::RING:
        *pType = SL_ANDROID_STREAM_RING;
        break;
    case android::AudioSystem::DEFAULT:
    case android::AudioSystem::MUSIC:
        *pType = SL_ANDROID_STREAM_MEDIA;
        break;
    case android::AudioSystem::ALARM:
        *pType = SL_ANDROID_STREAM_ALARM;
        break;
    case android::AudioSystem::NOTIFICATION:
        *pType = SL_ANDROID_STREAM_NOTIFICATION;
        break;
    default:
        result = SL_RESULT_INTERNAL_ERROR;
        *pType = SL_ANDROID_STREAM_MEDIA;
        break;
    }

    return result;
}


//-----------------------------------------------------------------------------
void audioPlayer_auxEffectUpdate(CAudioPlayer* ap) {
    if ((NULL != ap->mAudioTrack) && (ap->mAuxEffect != 0)) {
        android_fxSend_attach(ap, true, ap->mAuxEffect, ap->mVolume.mLevel + ap->mAuxSendLevel);
    }
}


//-----------------------------------------------------------------------------
void audioPlayer_setInvalid(CAudioPlayer* ap) {
    ap->mAndroidObjType = INVALID_TYPE;
    ap->mpLock = NULL;
    ap->mPlaybackRate.mCapabilities = 0;
}


//-----------------------------------------------------------------------------
/*
 * returns true if the given data sink is supported by AudioPlayer that don't
 *   play to an OutputMix object, false otherwise
 *
 * pre-condition: the locator of the audio sink is not SL_DATALOCATOR_OUTPUTMIX
 */
bool audioPlayer_isSupportedNonOutputMixSink(const SLDataSink* pAudioSink) {
    bool result = true;
    const SLuint32 sinkLocatorType = *(SLuint32 *)pAudioSink->pLocator;
    const SLuint32 sinkFormatType = *(SLuint32 *)pAudioSink->pFormat;

    switch (sinkLocatorType) {

    case SL_DATALOCATOR_BUFFERQUEUE:
    case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        if (SL_DATAFORMAT_PCM != sinkFormatType) {
            SL_LOGE("Unsupported sink format 0x%x, expected SL_DATAFORMAT_PCM",
                    (unsigned)sinkFormatType);
            result = false;
        }
        // it's no use checking the PCM format fields because additional characteristics
        // such as the number of channels, or sample size are unknown to the player at this stage
        break;

    default:
        SL_LOGE("Unsupported sink locator type 0x%x", (unsigned)sinkLocatorType);
        result = false;
        break;
    }

    return result;
}


//-----------------------------------------------------------------------------
/*
 * returns the Android object type if the locator type combinations for the source and sinks
 *   are supported by this implementation, INVALID_TYPE otherwise
 */
AndroidObject_type audioPlayer_getAndroidObjectTypeForSourceSink(CAudioPlayer *ap) {

    const SLDataSource *pAudioSrc = &ap->mDataSource.u.mSource;
    const SLDataSink *pAudioSnk = &ap->mDataSink.u.mSink;
    const SLuint32 sourceLocatorType = *(SLuint32 *)pAudioSrc->pLocator;
    const SLuint32 sinkLocatorType = *(SLuint32 *)pAudioSnk->pLocator;
    AndroidObject_type type = INVALID_TYPE;

    //--------------------------------------
    // Sink / source matching check:
    // the following source / sink combinations are supported
    //     SL_DATALOCATOR_BUFFERQUEUE                / SL_DATALOCATOR_OUTPUTMIX
    //     SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE   / SL_DATALOCATOR_OUTPUTMIX
    //     SL_DATALOCATOR_URI                        / SL_DATALOCATOR_OUTPUTMIX
    //     SL_DATALOCATOR_ANDROIDFD                  / SL_DATALOCATOR_OUTPUTMIX
    //     SL_DATALOCATOR_ANDROIDBUFFERQUEUE         / SL_DATALOCATOR_OUTPUTMIX
    //     SL_DATALOCATOR_URI                        / SL_DATALOCATOR_BUFFERQUEUE
    //     SL_DATALOCATOR_ANDROIDFD                  / SL_DATALOCATOR_BUFFERQUEUE
    //     SL_DATALOCATOR_URI                        / SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
    //     SL_DATALOCATOR_ANDROIDFD                  / SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
    switch (sinkLocatorType) {

    case SL_DATALOCATOR_OUTPUTMIX: {
        switch (sourceLocatorType) {

        //   Buffer Queue to AudioTrack
        case SL_DATALOCATOR_BUFFERQUEUE:
        case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
            type = A_PLR_PCM_BQ;
            break;

        //   URI or FD to MediaPlayer
        case SL_DATALOCATOR_URI:
        case SL_DATALOCATOR_ANDROIDFD:
            type = A_PLR_URIFD;
            break;

        //   Android BufferQueue to MediaPlayer (shared memory streaming)
        case SL_DATALOCATOR_ANDROIDBUFFERQUEUE:
            type = A_PLR_TS_ABQ;
            break;

        default:
            SL_LOGE("Source data locator 0x%x not supported with SL_DATALOCATOR_OUTPUTMIX sink",
                    (unsigned)sourceLocatorType);
            break;
        }
        }
        break;

    case SL_DATALOCATOR_BUFFERQUEUE:
    case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        switch (sourceLocatorType) {

        //   URI or FD decoded to PCM in a buffer queue
        case SL_DATALOCATOR_URI:
        case SL_DATALOCATOR_ANDROIDFD:
            type = A_PLR_URIFD_ASQ;
            break;

        default:
            SL_LOGE("Source data locator 0x%x not supported with SL_DATALOCATOR_BUFFERQUEUE sink",
                    (unsigned)sourceLocatorType);
            break;
        }
        break;

    default:
        SL_LOGE("Sink data locator 0x%x not supported", (unsigned)sinkLocatorType);
        break;
    }

    return type;
}


//-----------------------------------------------------------------------------
static void sfplayer_prepare(CAudioPlayer *ap, bool lockAP) {

    if (lockAP) { object_lock_exclusive(&ap->mObject); }
    ap->mAndroidObjState = ANDROID_PREPARING;
    if (lockAP) { object_unlock_exclusive(&ap->mObject); }

    if (ap->mSfPlayer != 0) {
        ap->mSfPlayer->prepare();
    }
}

//-----------------------------------------------------------------------------
// Callback associated with an SfPlayer of an SL ES AudioPlayer that gets its data
// from a URI or FD, for prepare and prefetch events
static void sfplayer_handlePrefetchEvent(int event, int data1, void* user) {
    if (NULL == user) {
        return;
    }

    CAudioPlayer *ap = (CAudioPlayer *)user;
    //SL_LOGV("received event %d, data %d from SfAudioPlayer", event, data1);
    switch(event) {

    case android::GenericPlayer::kEventPrepared: {

        if (PLAYER_SUCCESS != data1) {
            object_lock_exclusive(&ap->mObject);

            ap->mAudioTrack = NULL;
            ap->mNumChannels = 0;
            ap->mSampleRateMilliHz = 0;
            ap->mAndroidObjState = ANDROID_UNINITIALIZED;

            object_unlock_exclusive(&ap->mObject);

            // SfPlayer prepare() failed prefetching, there is no event in SLPrefetchStatus to
            //  indicate a prefetch error, so we signal it by sending simulataneously two events:
            //  - SL_PREFETCHEVENT_FILLLEVELCHANGE with a level of 0
            //  - SL_PREFETCHEVENT_STATUSCHANGE with a status of SL_PREFETCHSTATUS_UNDERFLOW
            SL_LOGE(ERROR_PLAYER_PREFETCH_d, data1);
            if (!IsInterfaceInitialized(&(ap->mObject), MPH_PREFETCHSTATUS)) {
                break;
            }

            slPrefetchCallback callback = NULL;
            void* callbackPContext = NULL;

            interface_lock_exclusive(&ap->mPrefetchStatus);
            ap->mPrefetchStatus.mLevel = 0;
            ap->mPrefetchStatus.mStatus = SL_PREFETCHSTATUS_UNDERFLOW;
            if ((ap->mPrefetchStatus.mCallbackEventsMask & SL_PREFETCHEVENT_FILLLEVELCHANGE)
                    && (ap->mPrefetchStatus.mCallbackEventsMask & SL_PREFETCHEVENT_STATUSCHANGE)) {
                callback = ap->mPrefetchStatus.mCallback;
                callbackPContext = ap->mPrefetchStatus.mContext;
            }
            interface_unlock_exclusive(&ap->mPrefetchStatus);

            // callback with no lock held
            if (NULL != callback) {
                (*callback)(&ap->mPrefetchStatus.mItf, callbackPContext,
                        SL_PREFETCHEVENT_FILLLEVELCHANGE | SL_PREFETCHEVENT_STATUSCHANGE);
            }


        } else {
            object_lock_exclusive(&ap->mObject);

            if (A_PLR_URIFD == ap->mAndroidObjType) {
                ap->mAudioTrack = ap->mSfPlayer->getAudioTrack();
                ap->mNumChannels = ap->mSfPlayer->getNumChannels();
                ap->mSampleRateMilliHz =
                        android_to_sles_sampleRate(ap->mSfPlayer->getSampleRateHz());
                ap->mSfPlayer->startPrefetch_async();
                // update the new track with the current settings
                audioPlayer_auxEffectUpdate(ap);
                android_audioPlayer_useEventMask(ap);
                android_audioPlayer_volumeUpdate(ap);
                android_audioPlayer_setPlayRate(ap, ap->mPlaybackRate.mRate, false /*lockAP*/);
            } else if (A_PLR_PCM_BQ == ap->mAndroidObjType) {
                ((android::AudioToCbRenderer*)ap->mAPlayer.get())->startPrefetch_async();
            } else if (A_PLR_TS_ABQ == ap->mAndroidObjType) {
                SL_LOGI("Received SfPlayer::kEventPrepared from AVPlayer for CAudioPlayer %p", ap);
            }

            ap->mAndroidObjState = ANDROID_READY;

            object_unlock_exclusive(&ap->mObject);
        }

    }
    break;

    case android::SfPlayer::kEventNewAudioTrack: {
        object_lock_exclusive(&ap->mObject);
#if 1
        // SfPlayer has a new AudioTrack, update our pointer copy and configure the new one before
        // starting to use it
#else
        // SfPlayer has a new AudioTrack, delete the old one and configure the new one before
        // starting to use it

        if (NULL != ap->mAudioTrack) {
            delete ap->mAudioTrack;
            ap->mAudioTrack = NULL;
        }
#endif
        ap->mAudioTrack = ap->mSfPlayer->getAudioTrack();
        ap->mNumChannels = ap->mSfPlayer->getNumChannels();
        ap->mSampleRateMilliHz = android_to_sles_sampleRate(ap->mSfPlayer->getSampleRateHz());

        // update the new track with the current settings
        audioPlayer_auxEffectUpdate(ap);
        android_audioPlayer_useEventMask(ap);
        android_audioPlayer_volumeUpdate(ap);
        android_audioPlayer_setPlayRate(ap, ap->mPlaybackRate.mRate, false /*lockAP*/);

        object_unlock_exclusive(&ap->mObject);
    }
    break;

    case android::SfPlayer::kEventPrefetchFillLevelUpdate : {
        if (!IsInterfaceInitialized(&(ap->mObject), MPH_PREFETCHSTATUS)) {
            break;
        }
        slPrefetchCallback callback = NULL;
        void* callbackPContext = NULL;

        // SLPrefetchStatusItf callback or no callback?
        interface_lock_exclusive(&ap->mPrefetchStatus);
        if (ap->mPrefetchStatus.mCallbackEventsMask & SL_PREFETCHEVENT_FILLLEVELCHANGE) {
            callback = ap->mPrefetchStatus.mCallback;
            callbackPContext = ap->mPrefetchStatus.mContext;
        }
        ap->mPrefetchStatus.mLevel = (SLpermille)data1;
        interface_unlock_exclusive(&ap->mPrefetchStatus);

        // callback with no lock held
        if (NULL != callback) {
            (*callback)(&ap->mPrefetchStatus.mItf, callbackPContext,
                    SL_PREFETCHEVENT_FILLLEVELCHANGE);
        }
    }
    break;

    case android::SfPlayer::kEventPrefetchStatusChange: {
        if (!IsInterfaceInitialized(&(ap->mObject), MPH_PREFETCHSTATUS)) {
            break;
        }
        slPrefetchCallback callback = NULL;
        void* callbackPContext = NULL;

        // SLPrefetchStatusItf callback or no callback?
        object_lock_exclusive(&ap->mObject);
        if (ap->mPrefetchStatus.mCallbackEventsMask & SL_PREFETCHEVENT_STATUSCHANGE) {
            callback = ap->mPrefetchStatus.mCallback;
            callbackPContext = ap->mPrefetchStatus.mContext;
        }
        if (data1 >= android::SfPlayer::kStatusIntermediate) {
            ap->mPrefetchStatus.mStatus = SL_PREFETCHSTATUS_SUFFICIENTDATA;
            // FIXME estimate fill level better?
            ap->mPrefetchStatus.mLevel = 1000;
            ap->mAndroidObjState = ANDROID_READY;
        } else if (data1 < android::SfPlayer::kStatusIntermediate) {
            ap->mPrefetchStatus.mStatus = SL_PREFETCHSTATUS_UNDERFLOW;
            // FIXME estimate fill level better?
            ap->mPrefetchStatus.mLevel = 0;
        }
        object_unlock_exclusive(&ap->mObject);

        // callback with no lock held
        if (NULL != callback) {
            (*callback)(&ap->mPrefetchStatus.mItf, callbackPContext, SL_PREFETCHEVENT_STATUSCHANGE);
        }
        }
        break;

    case android::SfPlayer::kEventEndOfStream: {
        audioPlayer_dispatch_headAtEnd_lockPlay(ap, true /*set state to paused?*/, true);
        if ((NULL != ap->mAudioTrack) && (!ap->mSeek.mLoopEnabled)) {
            ap->mAudioTrack->stop();
        }
        }
        break;

    default:
        break;
    }
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_checkSourceSink(CAudioPlayer *pAudioPlayer)
{
    // verify that the locator types for the source / sink combination is supported
    pAudioPlayer->mAndroidObjType = audioPlayer_getAndroidObjectTypeForSourceSink(pAudioPlayer);
    if (INVALID_TYPE == pAudioPlayer->mAndroidObjType) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    const SLDataSource *pAudioSrc = &pAudioPlayer->mDataSource.u.mSource;
    const SLDataSink *pAudioSnk = &pAudioPlayer->mDataSink.u.mSink;

    // format check:
    const SLuint32 sourceLocatorType = *(SLuint32 *)pAudioSrc->pLocator;
    const SLuint32 sinkLocatorType = *(SLuint32 *)pAudioSnk->pLocator;
    const SLuint32 sourceFormatType = *(SLuint32 *)pAudioSrc->pFormat;
    const SLuint32 sinkFormatType = *(SLuint32 *)pAudioSnk->pFormat;

    switch (sourceLocatorType) {
    //------------------
    //   Buffer Queues
    case SL_DATALOCATOR_BUFFERQUEUE:
    case SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE:
        {
        SLDataLocator_BufferQueue *dl_bq =  (SLDataLocator_BufferQueue *) pAudioSrc->pLocator;

        // Buffer format
        switch (sourceFormatType) {
        //     currently only PCM buffer queues are supported,
        case SL_DATAFORMAT_PCM: {
            SLDataFormat_PCM *df_pcm = (SLDataFormat_PCM *) pAudioSrc->pFormat;
            switch (df_pcm->numChannels) {
            case 1:
            case 2:
                break;
            default:
                // this should have already been rejected by checkDataFormat
                SL_LOGE("Cannot create audio player: unsupported " \
                    "PCM data source with %u channels", (unsigned) df_pcm->numChannels);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            switch (df_pcm->samplesPerSec) {
            case SL_SAMPLINGRATE_8:
            case SL_SAMPLINGRATE_11_025:
            case SL_SAMPLINGRATE_12:
            case SL_SAMPLINGRATE_16:
            case SL_SAMPLINGRATE_22_05:
            case SL_SAMPLINGRATE_24:
            case SL_SAMPLINGRATE_32:
            case SL_SAMPLINGRATE_44_1:
            case SL_SAMPLINGRATE_48:
                break;
            case SL_SAMPLINGRATE_64:
            case SL_SAMPLINGRATE_88_2:
            case SL_SAMPLINGRATE_96:
            case SL_SAMPLINGRATE_192:
            default:
                SL_LOGE("Cannot create audio player: unsupported sample rate %u milliHz",
                    (unsigned) df_pcm->samplesPerSec);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            switch (df_pcm->bitsPerSample) {
            case SL_PCMSAMPLEFORMAT_FIXED_8:
                // FIXME We should support this
                //SL_LOGE("Cannot create audio player: unsupported 8-bit data");
                //return SL_RESULT_CONTENT_UNSUPPORTED;
            case SL_PCMSAMPLEFORMAT_FIXED_16:
                break;
                // others
            default:
                // this should have already been rejected by checkDataFormat
                SL_LOGE("Cannot create audio player: unsupported sample bit depth %lu",
                        (SLuint32)df_pcm->bitsPerSample);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            switch (df_pcm->containerSize) {
            case 8:
            case 16:
                break;
                // others
            default:
                SL_LOGE("Cannot create audio player: unsupported container size %u",
                    (unsigned) df_pcm->containerSize);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            switch (df_pcm->channelMask) {
                // FIXME needs work
            default:
                break;
            }
            switch (df_pcm->endianness) {
            case SL_BYTEORDER_LITTLEENDIAN:
                break;
            case SL_BYTEORDER_BIGENDIAN:
                SL_LOGE("Cannot create audio player: unsupported big-endian byte order");
                return SL_RESULT_CONTENT_UNSUPPORTED;
                // native is proposed but not yet in spec
            default:
                SL_LOGE("Cannot create audio player: unsupported byte order %u",
                    (unsigned) df_pcm->endianness);
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
            } //case SL_DATAFORMAT_PCM
            break;
        case SL_DATAFORMAT_MIME:
        case XA_DATAFORMAT_RAWIMAGE:
            SL_LOGE("Cannot create audio player with buffer queue data source "
                "without SL_DATAFORMAT_PCM format");
            return SL_RESULT_CONTENT_UNSUPPORTED;
        default:
            // invalid data format is detected earlier
            assert(false);
            return SL_RESULT_INTERNAL_ERROR;
        } // switch (sourceFormatType)
        } // case SL_DATALOCATOR_BUFFERQUEUE or SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
        break;
    //------------------
    //   URI
    case SL_DATALOCATOR_URI:
        {
        SLDataLocator_URI *dl_uri =  (SLDataLocator_URI *) pAudioSrc->pLocator;
        if (NULL == dl_uri->URI) {
            return SL_RESULT_PARAMETER_INVALID;
        }
        // URI format
        switch (sourceFormatType) {
        case SL_DATAFORMAT_MIME:
            break;
        case SL_DATAFORMAT_PCM:
        case XA_DATAFORMAT_RAWIMAGE:
            SL_LOGE("Cannot create audio player with SL_DATALOCATOR_URI data source without "
                "SL_DATAFORMAT_MIME format");
            return SL_RESULT_CONTENT_UNSUPPORTED;
        } // switch (sourceFormatType)
        // decoding format check
        if ((sinkLocatorType != SL_DATALOCATOR_OUTPUTMIX) &&
                !audioPlayer_isSupportedNonOutputMixSink(pAudioSnk)) {
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        } // case SL_DATALOCATOR_URI
        break;
    //------------------
    //   File Descriptor
    case SL_DATALOCATOR_ANDROIDFD:
        {
        // fd is already non null
        switch (sourceFormatType) {
        case SL_DATAFORMAT_MIME:
            break;
        case SL_DATAFORMAT_PCM:
            // FIXME implement
            SL_LOGD("[ FIXME implement PCM FD data sources ]");
            break;
        case XA_DATAFORMAT_RAWIMAGE:
            SL_LOGE("Cannot create audio player with SL_DATALOCATOR_ANDROIDFD data source "
                "without SL_DATAFORMAT_MIME or SL_DATAFORMAT_PCM format");
            return SL_RESULT_CONTENT_UNSUPPORTED;
        default:
            // invalid data format is detected earlier
            assert(false);
            return SL_RESULT_INTERNAL_ERROR;
        } // switch (sourceFormatType)
        if ((sinkLocatorType != SL_DATALOCATOR_OUTPUTMIX) &&
                !audioPlayer_isSupportedNonOutputMixSink(pAudioSnk)) {
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        } // case SL_DATALOCATOR_ANDROIDFD
        break;
    //------------------
    //   Stream
    case SL_DATALOCATOR_ANDROIDBUFFERQUEUE:
    {
        switch (sourceFormatType) {
        case SL_DATAFORMAT_MIME:
        {
            SLDataFormat_MIME *df_mime = (SLDataFormat_MIME *) pAudioSrc->pFormat;
            if (SL_CONTAINERTYPE_MPEG_TS != df_mime->containerType) {
                SL_LOGE("Cannot create player with SL_DATALOCATOR_ANDROIDBUFFERQUEUE data source "
                        "that is not fed MPEG-2 TS data");
                return SL_RESULT_CONTENT_UNSUPPORTED;
            }
        }
        break;
        default:
            SL_LOGE("Cannot create player with SL_DATALOCATOR_ANDROIDBUFFERQUEUE data source "
                    "without SL_DATAFORMAT_MIME format");
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
    }
    break; // case SL_DATALOCATOR_ANDROIDBUFFERQUEUE
    //------------------
    //   Address
    case SL_DATALOCATOR_ADDRESS:
    case SL_DATALOCATOR_IODEVICE:
    case SL_DATALOCATOR_OUTPUTMIX:
    case XA_DATALOCATOR_NATIVEDISPLAY:
    case SL_DATALOCATOR_MIDIBUFFERQUEUE:
        SL_LOGE("Cannot create audio player with data locator type 0x%x",
                (unsigned) sourceLocatorType);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    default:
        SL_LOGE("Cannot create audio player with invalid data locator type 0x%x",
                (unsigned) sourceLocatorType);
        return SL_RESULT_PARAMETER_INVALID;
    }// switch (locatorType)

    return SL_RESULT_SUCCESS;
}



//-----------------------------------------------------------------------------
static void audioTrack_callBack_uri(int event, void* user, void *info) {
    // EVENT_MORE_DATA needs to be handled with priority over the other events
    // because it will be called the most often during playback
    if (event == android::AudioTrack::EVENT_MORE_DATA) {
        //SL_LOGV("received event EVENT_MORE_DATA from AudioTrack");
        // set size to 0 to signal we're not using the callback to write more data
        android::AudioTrack::Buffer* pBuff = (android::AudioTrack::Buffer*)info;
        pBuff->size = 0;
    } else if (NULL != user) {
        switch (event) {
            case android::AudioTrack::EVENT_MARKER :
                audioTrack_handleMarker_lockPlay((CAudioPlayer *)user);
                break;
            case android::AudioTrack::EVENT_NEW_POS :
                audioTrack_handleNewPos_lockPlay((CAudioPlayer *)user);
                break;
            case android::AudioTrack::EVENT_UNDERRUN :
                audioTrack_handleUnderrun_lockPlay((CAudioPlayer *)user);
                break;
            case android::AudioTrack::EVENT_BUFFER_END :
            case android::AudioTrack::EVENT_LOOP_END :
                break;
            default:
                SL_LOGE("Encountered unknown AudioTrack event %d for CAudioPlayer %p", event,
                        (CAudioPlayer *)user);
                break;
        }
    }
}

//-----------------------------------------------------------------------------
// Callback associated with an AudioTrack of an SL ES AudioPlayer that gets its data
// from a buffer queue.
static void audioTrack_callBack_pullFromBuffQueue(int event, void* user, void *info) {
    CAudioPlayer *ap = (CAudioPlayer *)user;
    void * callbackPContext = NULL;
    switch(event) {

    case android::AudioTrack::EVENT_MORE_DATA: {
        //SL_LOGV("received event EVENT_MORE_DATA from AudioTrack");
        slBufferQueueCallback callback = NULL;
        android::AudioTrack::Buffer* pBuff = (android::AudioTrack::Buffer*)info;
        // retrieve data from the buffer queue
        interface_lock_exclusive(&ap->mBufferQueue);
        if (ap->mBufferQueue.mState.count != 0) {
            //SL_LOGV("nbBuffers in queue = %lu",ap->mBufferQueue.mState.count);
            assert(ap->mBufferQueue.mFront != ap->mBufferQueue.mRear);

            BufferHeader *oldFront = ap->mBufferQueue.mFront;
            BufferHeader *newFront = &oldFront[1];

            // FIXME handle 8bit based on buffer format
            short *pSrc = (short*)((char *)oldFront->mBuffer
                    + ap->mBufferQueue.mSizeConsumed);
            if (ap->mBufferQueue.mSizeConsumed + pBuff->size < oldFront->mSize) {
                // can't consume the whole or rest of the buffer in one shot
                ap->mBufferQueue.mSizeConsumed += pBuff->size;
                // leave pBuff->size untouched
                // consume data
                // FIXME can we avoid holding the lock during the copy?
                memcpy (pBuff->i16, pSrc, pBuff->size);
            } else {
                // finish consuming the buffer or consume the buffer in one shot
                pBuff->size = oldFront->mSize - ap->mBufferQueue.mSizeConsumed;
                ap->mBufferQueue.mSizeConsumed = 0;

                if (newFront ==
                        &ap->mBufferQueue.mArray
                            [ap->mBufferQueue.mNumBuffers + 1])
                {
                    newFront = ap->mBufferQueue.mArray;
                }
                ap->mBufferQueue.mFront = newFront;

                ap->mBufferQueue.mState.count--;
                ap->mBufferQueue.mState.playIndex++;

                // consume data
                // FIXME can we avoid holding the lock during the copy?
                memcpy (pBuff->i16, pSrc, pBuff->size);

                // data has been consumed, and the buffer queue state has been updated
                // we will notify the client if applicable
                callback = ap->mBufferQueue.mCallback;
                // save callback data
                callbackPContext = ap->mBufferQueue.mContext;
            }
        } else { // empty queue
            // signal no data available
            pBuff->size = 0;

            // signal we're at the end of the content, but don't pause (see note in function)
            audioPlayer_dispatch_headAtEnd_lockPlay(ap, false /*set state to paused?*/, false);

            // signal underflow to prefetch status itf
            if (IsInterfaceInitialized(&(ap->mObject), MPH_PREFETCHSTATUS)) {
                audioPlayer_dispatch_prefetchStatus_lockPrefetch(ap, SL_PREFETCHSTATUS_UNDERFLOW,
                    false);
            }

            // stop the track so it restarts playing faster when new data is enqueued
            ap->mAudioTrack->stop();
        }
        interface_unlock_exclusive(&ap->mBufferQueue);
        // notify client
        if (NULL != callback) {
            (*callback)(&ap->mBufferQueue.mItf, callbackPContext);
        }
    }
    break;

    case android::AudioTrack::EVENT_MARKER:
        audioTrack_handleMarker_lockPlay(ap);
        break;

    case android::AudioTrack::EVENT_NEW_POS:
        audioTrack_handleNewPos_lockPlay(ap);
        break;

    case android::AudioTrack::EVENT_UNDERRUN:
        audioTrack_handleUnderrun_lockPlay(ap);
        break;

    default:
        // FIXME where does the notification of SL_PLAYEVENT_HEADMOVING fit?
        SL_LOGE("Encountered unknown AudioTrack event %d for CAudioPlayer %p", event,
                (CAudioPlayer *)user);
        break;
    }
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_create(CAudioPlayer *pAudioPlayer) {

    SLresult result = SL_RESULT_SUCCESS;
    // pAudioPlayer->mAndroidObjType has been set in audioPlayer_getAndroidObjectTypeForSourceSink()
    if (INVALID_TYPE == pAudioPlayer->mAndroidObjType) {
        audioPlayer_setInvalid(pAudioPlayer);
        result = SL_RESULT_PARAMETER_INVALID;
    } else {

        pAudioPlayer->mpLock = new android::Mutex();
        pAudioPlayer->mAndroidObjState = ANDROID_UNINITIALIZED;
        pAudioPlayer->mStreamType = ANDROID_DEFAULT_OUTPUT_STREAM_TYPE;
        pAudioPlayer->mAudioTrack = NULL;
        // not needed, as placement new (explicit constructor) already does this
        // pAudioPlayer->mSfPlayer.clear();

        pAudioPlayer->mSessionId = android::AudioSystem::newAudioSessionId();

        pAudioPlayer->mAmplFromVolLevel = 1.0f;
        pAudioPlayer->mAmplFromStereoPos[0] = 1.0f;
        pAudioPlayer->mAmplFromStereoPos[1] = 1.0f;
        pAudioPlayer->mDirectLevel = 0; // no attenuation
        pAudioPlayer->mAmplFromDirectLevel = 1.0f; // matches initial mDirectLevel value
        pAudioPlayer->mAuxSendLevel = 0;

        // initialize interface-specific fields that can be used regardless of whether the
        // interface is exposed on the AudioPlayer or not
        // (empty section, as all initializations are the same as the defaults)
    }

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_setConfig(CAudioPlayer *ap, const SLchar *configKey,
        const void *pConfigValue, SLuint32 valueSize) {

    SLresult result = SL_RESULT_SUCCESS;

    if (NULL == ap) {
        result = SL_RESULT_INTERNAL_ERROR;
    } else if (NULL == pConfigValue) {
        SL_LOGE(ERROR_CONFIG_NULL_PARAM);
        result = SL_RESULT_PARAMETER_INVALID;

    } else if(strcmp((const char*)configKey, (const char*)SL_ANDROID_KEY_STREAM_TYPE) == 0) {

        // stream type
        if (KEY_STREAM_TYPE_PARAMSIZE > valueSize) {
            SL_LOGE(ERROR_CONFIG_VALUESIZE_TOO_LOW);
            result = SL_RESULT_PARAMETER_INVALID;
        } else {
            result = audioPlayer_setStreamType(ap, *(SLuint32*)pConfigValue);
        }

    } else {
        SL_LOGE(ERROR_CONFIG_UNKNOWN_KEY);
        result = SL_RESULT_PARAMETER_INVALID;
    }

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_getConfig(CAudioPlayer* ap, const SLchar *configKey,
        SLuint32* pValueSize, void *pConfigValue) {

    SLresult result = SL_RESULT_SUCCESS;

    if (NULL == ap) {
        return SL_RESULT_INTERNAL_ERROR;
    } else if (NULL == pValueSize) {
        SL_LOGE(ERROR_CONFIG_NULL_PARAM);
        result = SL_RESULT_PARAMETER_INVALID;

    } else if(strcmp((const char*)configKey, (const char*)SL_ANDROID_KEY_STREAM_TYPE) == 0) {

        // stream type
        if (KEY_STREAM_TYPE_PARAMSIZE > *pValueSize) {
            SL_LOGE(ERROR_CONFIG_VALUESIZE_TOO_LOW);
            result = SL_RESULT_PARAMETER_INVALID;
        } else {
            *pValueSize = KEY_STREAM_TYPE_PARAMSIZE;
            if (NULL != pConfigValue) {
                result = audioPlayer_getStreamType(ap, (SLint32*)pConfigValue);
            }
        }

    } else {
        SL_LOGE(ERROR_CONFIG_UNKNOWN_KEY);
        result = SL_RESULT_PARAMETER_INVALID;
    }

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_realize(CAudioPlayer *pAudioPlayer, SLboolean async) {

    SLresult result = SL_RESULT_SUCCESS;
    SL_LOGV("Realize pAudioPlayer=%p", pAudioPlayer);

    switch (pAudioPlayer->mAndroidObjType) {
    //-----------------------------------
    // AudioTrack
    case A_PLR_PCM_BQ:
        {
        // initialize platform-specific CAudioPlayer fields

        SLDataLocator_BufferQueue *dl_bq =  (SLDataLocator_BufferQueue *)
                pAudioPlayer->mDynamicSource.mDataSource;
        SLDataFormat_PCM *df_pcm = (SLDataFormat_PCM *)
                pAudioPlayer->mDynamicSource.mDataSource->pFormat;

        uint32_t sampleRate = sles_to_android_sampleRate(df_pcm->samplesPerSec);

        pAudioPlayer->mAudioTrack = new android::AudioTrack(
                pAudioPlayer->mStreamType,                           // streamType
                sampleRate,                                          // sampleRate
                sles_to_android_sampleFormat(df_pcm->bitsPerSample), // format
                sles_to_android_channelMaskOut(df_pcm->numChannels, df_pcm->channelMask),
                                                                     //channel mask
                0,                                                   // frameCount (here min)
                0,                                                   // flags
                audioTrack_callBack_pullFromBuffQueue,               // callback
                (void *) pAudioPlayer,                               // user
                0      // FIXME find appropriate frame count         // notificationFrame
                , pAudioPlayer->mSessionId
                );
        android::status_t status = pAudioPlayer->mAudioTrack->initCheck();
        if (status != android::NO_ERROR) {
            SL_LOGE("AudioTrack::initCheck status %u", status);
            result = SL_RESULT_CONTENT_UNSUPPORTED;
        }

        // initialize platform-independent CAudioPlayer fields

        pAudioPlayer->mNumChannels = df_pcm->numChannels;
        pAudioPlayer->mSampleRateMilliHz = df_pcm->samplesPerSec; // Note: bad field name in SL ES

        pAudioPlayer->mAndroidObjState = ANDROID_READY;
        }
        break;
    //-----------------------------------
    // MediaPlayer
    case A_PLR_URIFD: {
        object_lock_exclusive(&pAudioPlayer->mObject);

        pAudioPlayer->mAndroidObjState = ANDROID_UNINITIALIZED;
        pAudioPlayer->mNumChannels = 0;
        pAudioPlayer->mSampleRateMilliHz = 0;
        pAudioPlayer->mAudioTrack = NULL;

        AudioPlayback_Parameters app;
        app.sessionId = pAudioPlayer->mSessionId;
        app.streamType = pAudioPlayer->mStreamType;
        app.trackcb = audioTrack_callBack_uri;
        app.trackcbUser = (void *) pAudioPlayer;

        pAudioPlayer->mSfPlayer = new android::SfPlayer(&app);
        pAudioPlayer->mSfPlayer->setNotifListener(sfplayer_handlePrefetchEvent,
                        (void*)pAudioPlayer /*notifUSer*/);
        pAudioPlayer->mSfPlayer->armLooper();

        object_unlock_exclusive(&pAudioPlayer->mObject);

        switch (pAudioPlayer->mDataSource.mLocator.mLocatorType) {
            case SL_DATALOCATOR_URI:
                pAudioPlayer->mSfPlayer->setDataSource(
                        (const char*)pAudioPlayer->mDataSource.mLocator.mURI.URI);
                break;
            case SL_DATALOCATOR_ANDROIDFD: {
                int64_t offset = (int64_t)pAudioPlayer->mDataSource.mLocator.mFD.offset;
                pAudioPlayer->mSfPlayer->setDataSource(
                        (int)pAudioPlayer->mDataSource.mLocator.mFD.fd,
                        offset == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE ?
                                (int64_t)PLAYER_FD_FIND_FILE_SIZE : offset,
                        (int64_t)pAudioPlayer->mDataSource.mLocator.mFD.length);
                }
                break;
            default:
                SL_LOGE(ERROR_PLAYERREALIZE_UNKNOWN_DATASOURCE_LOCATOR);
                break;
        }

        }
        break;
    //-----------------------------------
    // StreamPlayer
    case A_PLR_TS_ABQ: {
        object_lock_exclusive(&pAudioPlayer->mObject);

        android_StreamPlayer_realize_l(pAudioPlayer, sfplayer_handlePrefetchEvent,
                (void*)pAudioPlayer);

        object_unlock_exclusive(&pAudioPlayer->mObject);
        }
        break;
    //-----------------------------------
    // AudioToCbRenderer
    case A_PLR_URIFD_ASQ: {
        object_lock_exclusive(&pAudioPlayer->mObject);

        AudioPlayback_Parameters app;
        app.sessionId = pAudioPlayer->mSessionId;
        app.streamType = pAudioPlayer->mStreamType;

        android::AudioToCbRenderer* decoder = new android::AudioToCbRenderer(&app);
        pAudioPlayer->mAPlayer = decoder;
        decoder->setDataPushListener(adecoder_writeToBufferQueue, (void*)pAudioPlayer);
        decoder->init(sfplayer_handlePrefetchEvent, (void*)pAudioPlayer);

        switch (pAudioPlayer->mDataSource.mLocator.mLocatorType) {
        case SL_DATALOCATOR_URI:
            decoder->setDataSource(
                    (const char*)pAudioPlayer->mDataSource.mLocator.mURI.URI);
            break;
        case SL_DATALOCATOR_ANDROIDFD: {
            int64_t offset = (int64_t)pAudioPlayer->mDataSource.mLocator.mFD.offset;
            decoder->setDataSource(
                    (int)pAudioPlayer->mDataSource.mLocator.mFD.fd,
                    offset == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE ?
                            (int64_t)PLAYER_FD_FIND_FILE_SIZE : offset,
                            (int64_t)pAudioPlayer->mDataSource.mLocator.mFD.length);
            }
            break;
        default:
            SL_LOGE(ERROR_PLAYERREALIZE_UNKNOWN_DATASOURCE_LOCATOR);
            break;
        }

        object_unlock_exclusive(&pAudioPlayer->mObject);
        }
        break;
    //-----------------------------------
    default:
        SL_LOGE(ERROR_PLAYERREALIZE_UNEXPECTED_OBJECT_TYPE_D, pAudioPlayer->mAndroidObjType);
        result = SL_RESULT_INTERNAL_ERROR;
        break;
    }


    // proceed with effect initialization
    // initialize EQ
    // FIXME use a table of effect descriptors when adding support for more effects
    if (memcmp(SL_IID_EQUALIZER, &pAudioPlayer->mEqualizer.mEqDescriptor.type,
            sizeof(effect_uuid_t)) == 0) {
        SL_LOGV("Need to initialize EQ for AudioPlayer=%p", pAudioPlayer);
        android_eq_init(pAudioPlayer->mSessionId, &pAudioPlayer->mEqualizer);
    }
    // initialize BassBoost
    if (memcmp(SL_IID_BASSBOOST, &pAudioPlayer->mBassBoost.mBassBoostDescriptor.type,
            sizeof(effect_uuid_t)) == 0) {
        SL_LOGV("Need to initialize BassBoost for AudioPlayer=%p", pAudioPlayer);
        android_bb_init(pAudioPlayer->mSessionId, &pAudioPlayer->mBassBoost);
    }
    // initialize Virtualizer
    if (memcmp(SL_IID_VIRTUALIZER, &pAudioPlayer->mVirtualizer.mVirtualizerDescriptor.type,
               sizeof(effect_uuid_t)) == 0) {
        SL_LOGV("Need to initialize Virtualizer for AudioPlayer=%p", pAudioPlayer);
        android_virt_init(pAudioPlayer->mSessionId, &pAudioPlayer->mVirtualizer);
    }

    // initialize EffectSend
    // FIXME initialize EffectSend

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_destroy(CAudioPlayer *pAudioPlayer) {
    SLresult result = SL_RESULT_SUCCESS;
    SL_LOGV("android_audioPlayer_destroy(%p)", pAudioPlayer);
    switch (pAudioPlayer->mAndroidObjType) {
    //-----------------------------------
    // AudioTrack
    case A_PLR_PCM_BQ:
        // We own the audio track for PCM buffer queue players
        if (pAudioPlayer->mAudioTrack != NULL) {
            pAudioPlayer->mAudioTrack->stop();
            delete pAudioPlayer->mAudioTrack;
            pAudioPlayer->mAudioTrack = NULL;
        }
        break;
    //-----------------------------------
    // MediaPlayer
    case A_PLR_URIFD:
        // We don't own this audio track, SfPlayer does
        pAudioPlayer->mAudioTrack = NULL;
        // FIXME might no longer be needed since we call explicit destructor
        pAudioPlayer->mSfPlayer.clear();
        break;
    //-----------------------------------
    // StreamPlayer
    case A_PLR_TS_ABQ:                   // intended fall-through
    //-----------------------------------
    // AudioToCbRenderer
    case A_PLR_URIFD_ASQ:
        pAudioPlayer->mAPlayer.clear();
        break;
    //-----------------------------------
    default:
        SL_LOGE(ERROR_PLAYERDESTROY_UNEXPECTED_OBJECT_TYPE_D, pAudioPlayer->mAndroidObjType);
        result = SL_RESULT_INTERNAL_ERROR;
        break;
    }

    // FIXME might not be needed
    pAudioPlayer->mAndroidObjType = INVALID_TYPE;

    // explicit destructor
    pAudioPlayer->mSfPlayer.~sp();
    pAudioPlayer->mAuxEffect.~sp();
    pAudioPlayer->mAPlayer.~sp();

    if (pAudioPlayer->mpLock != NULL) {
        delete pAudioPlayer->mpLock;
        pAudioPlayer->mpLock = NULL;
    }

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_setPlayRate(CAudioPlayer *ap, SLpermille rate, bool lockAP) {
    SLresult result = SL_RESULT_SUCCESS;
    uint32_t contentRate = 0;
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
    case A_PLR_URIFD: {
        // get the content sample rate
        if (lockAP) { object_lock_shared(&ap->mObject); }
        uint32_t contentRate = sles_to_android_sampleRate(ap->mSampleRateMilliHz);
        if (lockAP) { object_unlock_shared(&ap->mObject); }
        // apply the SL ES playback rate on the AudioTrack as a factor of its content sample rate
        if (ap->mAudioTrack != NULL) {
            ap->mAudioTrack->setSampleRate(contentRate * (rate/1000.0f));
        }
        }
        break;

    default:
        SL_LOGE("Unexpected object type %d", ap->mAndroidObjType);
        result = SL_RESULT_INTERNAL_ERROR;
        break;
    }
    return result;
}


//-----------------------------------------------------------------------------
// called with no lock held
SLresult android_audioPlayer_setPlaybackRateBehavior(CAudioPlayer *ap,
        SLuint32 constraints) {
    SLresult result = SL_RESULT_SUCCESS;
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
    case A_PLR_URIFD:
        if (constraints != (constraints & SL_RATEPROP_NOPITCHCORAUDIO)) {
            result = SL_RESULT_FEATURE_UNSUPPORTED;
        }
        break;
    default:
        SL_LOGE("Unexpected object type %d", ap->mAndroidObjType);
        result = SL_RESULT_INTERNAL_ERROR;
        break;
    }
    return result;
}


//-----------------------------------------------------------------------------
// called with no lock held
SLresult android_audioPlayer_getCapabilitiesOfRate(CAudioPlayer *ap,
        SLuint32 *pCapabilities) {
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
    case A_PLR_URIFD:
        *pCapabilities = SL_RATEPROP_NOPITCHCORAUDIO;
        break;
    default:
        *pCapabilities = 0;
        break;
    }
    return SL_RESULT_SUCCESS;
}


//-----------------------------------------------------------------------------
void android_audioPlayer_setPlayState(CAudioPlayer *ap, bool lockAP) {

    if (lockAP) { object_lock_shared(&ap->mObject); }
    SLuint32 playState = ap->mPlay.mState;
    AndroidObject_state objState = ap->mAndroidObjState;
    if (lockAP) { object_unlock_shared(&ap->mObject); }

    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
        switch (playState) {
        case SL_PLAYSTATE_STOPPED:
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_STOPPED");
            if (NULL != ap->mAudioTrack) {
                ap->mAudioTrack->stop();
            }
            break;
        case SL_PLAYSTATE_PAUSED:
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_PAUSED");
            if (NULL != ap->mAudioTrack) {
                ap->mAudioTrack->pause();
            }
            break;
        case SL_PLAYSTATE_PLAYING:
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_PLAYING");
            if (NULL != ap->mAudioTrack) {
                ap->mAudioTrack->start();
            }
            break;
        default:
            // checked by caller, should not happen
            break;
        }
        break;

    case A_PLR_URIFD:
        switch (playState) {
        case SL_PLAYSTATE_STOPPED: {
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_STOPPED");
            if (ap->mSfPlayer != 0) {
                ap->mSfPlayer->stop();
            }
            }
            break;
        case SL_PLAYSTATE_PAUSED: {
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_PAUSED");
            switch(objState) {
                case ANDROID_UNINITIALIZED:
                    sfplayer_prepare(ap, lockAP);
                    break;
                caseANDROID_PREPARING:
                    break;
                case ANDROID_READY:
                    if (ap->mSfPlayer != 0) {
                        ap->mSfPlayer->pause();
                    }
                    break;
                default:
                    break;
            }
            }
            break;
        case SL_PLAYSTATE_PLAYING: {
            SL_LOGV("setting AudioPlayer to SL_PLAYSTATE_PLAYING");
            switch(objState) {
                case ANDROID_UNINITIALIZED:
                    sfplayer_prepare(ap, lockAP);
                    // fall through
                case ANDROID_PREPARING:
                case ANDROID_READY:
                    if (ap->mSfPlayer != 0) {
                        ap->mSfPlayer->play();
                    }
                    break;
                default:
                    break;
            }
            }
            break;

        default:
            // checked by caller, should not happen
            break;
        }
        break;

    case A_PLR_TS_ABQ:     // intended fall-through
    case A_PLR_URIFD_ASQ:
        // FIXME report and use the return code to the lock mechanism, which is where play state
        //   changes are updated (see object_unlock_exclusive_attributes())
        aplayer_setPlayState(ap->mAPlayer, playState, &(ap->mAndroidObjState));
        break;
    default:
        SL_LOGE(ERROR_PLAYERSETPLAYSTATE_UNEXPECTED_OBJECT_TYPE_D, ap->mAndroidObjType);
        break;
    }
}


//-----------------------------------------------------------------------------
void android_audioPlayer_useEventMask(CAudioPlayer *ap) {
    IPlay *pPlayItf = &ap->mPlay;
    SLuint32 eventFlags = pPlayItf->mEventFlags;
    /*switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:*/

    if (NULL == ap->mAudioTrack) {
        return;
    }

    if (eventFlags & SL_PLAYEVENT_HEADATMARKER) {
        ap->mAudioTrack->setMarkerPosition((uint32_t)((((int64_t)pPlayItf->mMarkerPosition
                * sles_to_android_sampleRate(ap->mSampleRateMilliHz)))/1000));
    } else {
        // clear marker
        ap->mAudioTrack->setMarkerPosition(0);
    }

    if (eventFlags & SL_PLAYEVENT_HEADATNEWPOS) {
         ap->mAudioTrack->setPositionUpdatePeriod(
                (uint32_t)((((int64_t)pPlayItf->mPositionUpdatePeriod
                * sles_to_android_sampleRate(ap->mSampleRateMilliHz)))/1000));
    } else {
        // clear periodic update
        ap->mAudioTrack->setPositionUpdatePeriod(0);
    }

    if (eventFlags & SL_PLAYEVENT_HEADATEND) {
        // nothing to do for SL_PLAYEVENT_HEADATEND, callback event will be checked against mask
    }

    if (eventFlags & SL_PLAYEVENT_HEADMOVING) {
        // FIXME support SL_PLAYEVENT_HEADMOVING
        SL_LOGD("[ FIXME: IPlay_SetCallbackEventsMask(SL_PLAYEVENT_HEADMOVING) on an "
            "SL_OBJECTID_AUDIOPLAYER to be implemented ]");
    }
    if (eventFlags & SL_PLAYEVENT_HEADSTALLED) {
        // nothing to do for SL_PLAYEVENT_HEADSTALLED, callback event will be checked against mask
    }

}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_getDuration(IPlay *pPlayItf, SLmillisecond *pDurMsec) {
    CAudioPlayer *ap = (CAudioPlayer *)pPlayItf->mThis;
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
        *pDurMsec = SL_TIME_UNKNOWN;
        // FIXME if the data source is not a buffer queue, and the audio data is saved in
        //       shared memory with the mixer process, the duration is the size of the buffer
        SL_LOGD("FIXME: android_audioPlayer_getDuration() verify if duration can be retrieved");
        break;
    case A_PLR_URIFD: {
        int64_t durationUsec = SL_TIME_UNKNOWN;
        if (ap->mSfPlayer != 0) {
            durationUsec = ap->mSfPlayer->getDurationUsec();
            *pDurMsec = durationUsec == -1 ? SL_TIME_UNKNOWN : durationUsec / 1000;
        }
        }
        break;
    default:
        break;
    }
    return SL_RESULT_SUCCESS;
}


//-----------------------------------------------------------------------------
void android_audioPlayer_getPosition(IPlay *pPlayItf, SLmillisecond *pPosMsec) {
    CAudioPlayer *ap = (CAudioPlayer *)pPlayItf->mThis;
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
        if ((ap->mSampleRateMilliHz == 0) || (NULL == ap->mAudioTrack)) {
            *pPosMsec = 0;
        } else {
            uint32_t positionInFrames;
            ap->mAudioTrack->getPosition(&positionInFrames);
            *pPosMsec = ((int64_t)positionInFrames * 1000) /
                    sles_to_android_sampleRate(ap->mSampleRateMilliHz);
        }
        break;
    case A_PLR_URIFD:
        if (ap->mSfPlayer != 0) {
            *pPosMsec = ap->mSfPlayer->getPositionMsec();
        } else {
            *pPosMsec = 0;
        }
        break;
    default:
        break;
    }
}


//-----------------------------------------------------------------------------
void android_audioPlayer_seek(CAudioPlayer *ap, SLmillisecond posMsec) {

    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
        break;
    case A_PLR_URIFD:
        if (ap->mSfPlayer != 0) {
            ap->mSfPlayer->seek(posMsec);
        }
        break;
    default:
        break;
    }
}


//-----------------------------------------------------------------------------
void android_audioPlayer_loop(CAudioPlayer *ap, SLboolean loopEnable) {

    if ((A_PLR_URIFD == ap->mAndroidObjType) && (ap->mSfPlayer != 0)) {
        ap->mSfPlayer->loop((bool)loopEnable);
    }
}


//-----------------------------------------------------------------------------
/*
 * Mutes or unmutes the Android media framework object associated with the CAudioPlayer that carries
 * the IVolume interface.
 * Pre-condition:
 *   if ap->mMute is SL_BOOLEAN_FALSE, a call to this function was preceded by a call
 *   to android_audioPlayer_volumeUpdate()
 */
static void android_audioPlayer_setMute(CAudioPlayer* ap) {
    android::AudioTrack *t = NULL;
    switch(ap->mAndroidObjType) {
    case A_PLR_PCM_BQ:
    case A_PLR_URIFD:
        t = ap->mAudioTrack;
        break;
    default:
        break;
    }
    // when unmuting: volume levels have already been updated in IVolume_SetMute
    if (NULL != t) {
        t->mute(ap->mMute);
    }
}


//-----------------------------------------------------------------------------
SLresult android_audioPlayer_volumeUpdate(CAudioPlayer* ap) {
    android_audioPlayer_updateStereoVolume(ap);
    android_audioPlayer_setMute(ap);
    return SL_RESULT_SUCCESS;
}


//-----------------------------------------------------------------------------
void android_audioPlayer_bufferQueue_onRefilled(CAudioPlayer *ap) {
    // the AudioTrack associated with the AudioPlayer receiving audio from a PCM buffer
    // queue was stopped when the queue become empty, we restart as soon as a new buffer
    // has been enqueued since we're in playing state
    if (NULL != ap->mAudioTrack) {
        ap->mAudioTrack->start();
    }

    // when the queue became empty, an underflow on the prefetch status itf was sent. Now the queue
    // has received new data, signal it has sufficient data
    if (IsInterfaceInitialized(&(ap->mObject), MPH_PREFETCHSTATUS)) {
        audioPlayer_dispatch_prefetchStatus_lockPrefetch(ap, SL_PREFETCHSTATUS_SUFFICIENTDATA,
            true);
    }
}


//-----------------------------------------------------------------------------
/*
 * BufferQueue::Clear
 */
SLresult android_audioPlayer_bufferQueue_onClear(CAudioPlayer *ap) {
    SLresult result = SL_RESULT_SUCCESS;

    switch (ap->mAndroidObjType) {
    //-----------------------------------
    // AudioTrack
    case A_PLR_PCM_BQ:
        if (NULL != ap->mAudioTrack) {
            ap->mAudioTrack->flush();
        }
        break;
    default:
        result = SL_RESULT_INTERNAL_ERROR;
        break;
    }

    return result;
}


//-----------------------------------------------------------------------------
// abqSrc_callBack_pullFromBuffQueue is implemented in AndroidBufferQueueSource.cpp
void android_audioPlayer_androidBufferQueue_registerCallback_l(CAudioPlayer *ap) {
    if ((ap->mAndroidObjType == A_PLR_TS_ABQ) && (ap->mAPlayer != 0)) {
        android::StreamPlayer* splr = static_cast<android::StreamPlayer*>(ap->mAPlayer.get());
        splr->registerQueueCallback(
                ap->mAndroidBufferQueue.mCallback,
                abqSrc_callBack_pullFromBuffQueue,
                (const void*)ap, true /*userIsAudioPlayer*/,
                ap->mAndroidBufferQueue.mContext,
                (const void*)&(ap->mAndroidBufferQueue.mItf));
    }
}

//-----------------------------------------------------------------------------
void android_audioPlayer_androidBufferQueue_clear_l(CAudioPlayer *ap) {
    if (ap->mAndroidObjType == A_PLR_TS_ABQ) {
        android_StreamPlayer_clear_l(ap);
    }
}

void android_audioPlayer_androidBufferQueue_enqueue_l(CAudioPlayer *ap,
        SLuint32 bufferId, SLuint32 length, SLuint32 event, void *pData) {
    if (ap->mAndroidObjType == A_PLR_TS_ABQ) {
        android_StreamPlayer_enqueue_l(ap, bufferId, length, event, pData);
    }
}