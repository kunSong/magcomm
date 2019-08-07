#include "AudioALSAStreamOut.h"

#include "AudioALSAStreamManager.h"
#include "AudioALSAPlaybackHandlerBase.h"
#include "AudioUtility.h"

#include "AudioALSASampleRateController.h"
#include "AudioALSAFMController.h"
#include "AudioALSAHardwareResourceManager.h"

#include <hardware/audio_mtk.h>

#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
#define LOW_POWER_HAL_BUFFER_SIZE   (45056)
#define LOW_LATENCY_HAL_BUFFER_SIZE (8192)
#endif

#define BUFFER_SIZE_PER_ACCESSS     (8192)
#define FRAME_COUNT_MIN_PER_ACCESSS (256)


#define LOG_TAG  "AudioALSAStreamOut"

namespace android
{

uint32_t AudioALSAStreamOut::mDumpFileNum = 0;

// TODO(Harvey): Query this
static const audio_format_t       kDefaultOutputSourceFormat      = AUDIO_FORMAT_PCM_16_BIT;
static const audio_channel_mask_t kDefaultOutputSourceChannelMask = AUDIO_CHANNEL_OUT_STEREO;
static const uint32_t             kDefaultOutputSourceSampleRate  = 44100;


uint32_t AudioALSAStreamOut::mSuspendCount = 0;
uint32_t AudioALSAStreamOut::mSuspendStreamOutHDMIStereoCount = 0;

AudioALSAStreamOut *AudioALSAStreamOut::mStreamOutHDMIStereo = NULL;


AudioALSAStreamOut::AudioALSAStreamOut() :
    mStreamManager(AudioALSAStreamManager::getInstance()),
    mPlaybackHandler(NULL),
    mPCMDumpFile(NULL),
    mIdentity(0xFFFFFFFF),
    mPresentedBytes(0),
    mStandby(true),
    mStreamOutType(STREAM_OUT_PRIMARY),
    mRoutingDevice(AUDIO_DEVICE_NONE),
    mCbkCookie(NULL),
    mOffloadVol(0x10000),
    mPaused(false),
    mLowLatencyMode(true),
    mLockCount(0)
{
    ALOGD("%s()", __FUNCTION__);

    memset(&mStreamAttributeSource, 0, sizeof(mStreamAttributeSource));
}


AudioALSAStreamOut::~AudioALSAStreamOut()
{
    ALOGD("%s()", __FUNCTION__);

    ASSERT(mStandby == true && mPlaybackHandler == NULL);

    if (mStreamOutHDMIStereo == this)
    {
        mStreamOutHDMIStereo = NULL;
    }
}


status_t AudioALSAStreamOut::set(
    uint32_t devices,
    int *format,
    uint32_t *channels,
    uint32_t *sampleRate,
    status_t *status,
    uint32_t flags)
{
    ALOGD("%s(), devices = 0x%x, format = 0x%x, channels = 0x%x, sampleRate = %d, flags = 0x%x",
          __FUNCTION__, devices, *format, *channels, *sampleRate, flags);

    AudioAutoTimeoutLock _l(mLock);

    *status = NO_ERROR;

    // device
    mStreamAttributeSource.output_devices = static_cast<audio_devices_t>(devices);


    // check format
    if (*format == AUDIO_FORMAT_PCM_16_BIT ||
        *format == AUDIO_FORMAT_PCM_8_24_BIT ||
        *format == AUDIO_FORMAT_PCM_32_BIT)
    {
        mStreamAttributeSource.audio_format = static_cast<audio_format_t>(*format);
    }
    else if(*format == AUDIO_FORMAT_MP3)
    {
        ALOGD("%s(), format mp3", __FUNCTION__ );
        mStreamAttributeSource.audio_format = static_cast<audio_format_t>(*format);
    }
    else
    {
        ALOGE("%s(), wrong format 0x%x, use 0x%x instead.", __FUNCTION__, *format, kDefaultOutputSourceFormat);

        *format = kDefaultOutputSourceFormat;
        *status = BAD_VALUE;
    }

    // check channel mask
    if (mStreamAttributeSource.output_devices == AUDIO_DEVICE_OUT_AUX_DIGITAL) // HDMI
    {
        if (*channels == AUDIO_CHANNEL_OUT_STEREO)
        {
            mStreamOutType = STREAM_OUT_HDMI_STEREO;

            mStreamAttributeSource.audio_channel_mask = *channels;
            mStreamAttributeSource.num_channels = android_audio_legacy::AudioSystem::popCount(*channels);

            mStreamOutHDMIStereo = this;
        }
        else if (*channels == AUDIO_CHANNEL_OUT_5POINT1 ||
                 *channels == AUDIO_CHANNEL_OUT_7POINT1)
        {
            mStreamOutType = STREAM_OUT_HDMI_MULTI_CHANNEL;

            mStreamAttributeSource.audio_channel_mask = *channels;
            mStreamAttributeSource.num_channels = android_audio_legacy::AudioSystem::popCount(*channels);
        }
        else
        {
            ALOGE("%s(), wrong channels 0x%x, use 0x%x instead.", __FUNCTION__, *channels, kDefaultOutputSourceChannelMask);

            *channels = kDefaultOutputSourceChannelMask;
            *status = BAD_VALUE;
        }
    }
    else if (*channels == kDefaultOutputSourceChannelMask) // Primary
    {
        mStreamAttributeSource.audio_channel_mask = *channels;
        mStreamAttributeSource.num_channels = android_audio_legacy::AudioSystem::popCount(*channels);
    }
    else
    {
        ALOGE("%s(), wrong channels 0x%x, use 0x%x instead.", __FUNCTION__, *channels, kDefaultOutputSourceChannelMask);

        *channels = kDefaultOutputSourceChannelMask;
        *status = BAD_VALUE;
    }

    // check sample rate
    if (SampleRateSupport(*sampleRate) == true)
    {
        mStreamAttributeSource.sample_rate = *sampleRate;

        if (mStreamOutType == STREAM_OUT_PRIMARY)
        {
            AudioALSASampleRateController::getInstance()->setPrimaryStreamOutSampleRate(*sampleRate);
        }
    }
    else
    {
        ALOGE("%s(), wrong sampleRate %d, use %d instead.", __FUNCTION__, *sampleRate, kDefaultOutputSourceSampleRate);

        *sampleRate = kDefaultOutputSourceSampleRate;
        *status = BAD_VALUE;
    }

    // set default value here. and change it when open by different type of handlers
    const uint8_t size_per_frame = mStreamAttributeSource.num_channels * audio_bytes_per_sample(mStreamAttributeSource.audio_format);

#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
    mStreamAttributeSource.buffer_size = LOW_POWER_HAL_BUFFER_SIZE; // TODO(Harvey): Query this
    mStreamAttributeSource.latency = (LOW_LATENCY_HAL_BUFFER_SIZE * 1000) / (mStreamAttributeSource.sample_rate * size_per_frame);
#else
    //mStreamAttributeSource.buffer_size = 6144 * 2 * 2; // TODO(Harvey): Query this
    mStreamAttributeSource.buffer_size = BUFFER_SIZE_PER_ACCESSS;
    mStreamAttributeSource.latency = (32768 * 1000) / (mStreamAttributeSource.sample_rate * size_per_frame);
#endif

#ifdef DOWNLINK_LOW_LATENCY
    mStreamAttributeSource.mAudioOutputFlags = (audio_output_flags_t)flags;
    // audio low latency param - playback - hal buffer size
    setBufferSize();
#endif

    return *status;
}


uint32_t AudioALSAStreamOut::sampleRate() const
{
    ALOGV("%s(), return %d", __FUNCTION__, mStreamAttributeSource.sample_rate);
    return mStreamAttributeSource.sample_rate;
}


size_t AudioALSAStreamOut::bufferSize() const
{
    ALOGV("%s(), return 0x%x", __FUNCTION__, mStreamAttributeSource.buffer_size);
    return mStreamAttributeSource.buffer_size;
}


uint32_t AudioALSAStreamOut::channels() const
{
    ALOGV("%s(), return 0x%x", __FUNCTION__, mStreamAttributeSource.audio_channel_mask);
    return mStreamAttributeSource.audio_channel_mask;
}


int AudioALSAStreamOut::format() const
{
    ALOGV("%s(), return 0x%x", __FUNCTION__, mStreamAttributeSource.audio_format);
    return mStreamAttributeSource.audio_format;
}


uint32_t AudioALSAStreamOut::latency() const
{
    uint32_t latency = 0;

    if (mPlaybackHandler == NULL)
    {
        latency = mStreamAttributeSource.latency;
    }
    else
    {
        const stream_attribute_t *pStreamAttributeTarget = mPlaybackHandler->getStreamAttributeTarget();
        const uint8_t size_per_channel = (pStreamAttributeTarget->audio_format == AUDIO_FORMAT_PCM_8_BIT ? 1 :
                                          (pStreamAttributeTarget->audio_format == AUDIO_FORMAT_PCM_16_BIT ? 2 :
                                           (pStreamAttributeTarget->audio_format == AUDIO_FORMAT_PCM_32_BIT ? 4 :
                                            (pStreamAttributeTarget->audio_format == AUDIO_FORMAT_PCM_8_24_BIT ? 4 :
                                             2))));
        const uint8_t size_per_frame = pStreamAttributeTarget->num_channels * size_per_channel;

        latency = (pStreamAttributeTarget->buffer_size * 1000) / (pStreamAttributeTarget->sample_rate * size_per_frame);
    }

    ALOGD("%s(), flags %d, return %d", __FUNCTION__, mStreamAttributeSource.mAudioOutputFlags, latency);
    return latency;
}


status_t AudioALSAStreamOut::setVolume(float left, float right)
{
    mOffloadVol = mStreamManager->GetOffloadGain(left);
	ALOGD("%s(), 0x%x", __FUNCTION__,mOffloadVol);

    if(mPlaybackHandler != NULL)
    {
        if(mOffloadVol != (uint32_t)-1)
            mPlaybackHandler->setVolume(mOffloadVol);
        return NO_ERROR;
    }

    ALOGE("%s(), playbackhandler NULL", __FUNCTION__);
    return NO_ERROR;
}


ssize_t AudioALSAStreamOut::write(const void *buffer, size_t bytes)
{
    ALOGV("%s(), buffer = %p, bytes = %d, flags %d", __FUNCTION__, buffer, bytes, mStreamAttributeSource.mAudioOutputFlags);

    size_t outputSize = 0;
    if (mSuspendCount > 0 ||
        (mStreamOutType == STREAM_OUT_HDMI_STEREO && mSuspendStreamOutHDMIStereoCount > 0) ||
         (mStreamManager->isModeInPhoneCall() == true && mStreamOutType  != STREAM_OUT_PRIMARY))
    {
        // here to sleep a buffer size latency and return.
        ALOGV("%s(), mStreamOutType = %d, mSuspendCount = %u, mSuspendStreamOutHDMIStereoCount = %d",
              __FUNCTION__, mStreamOutType, mSuspendCount, mSuspendStreamOutHDMIStereoCount);
        usleep(latency() * 1000);
        mPresentedBytes += bytes;
        return bytes;
    }

    /* fast output is RT thread and keep streamout lock for write kernel.
       so other thread can't get streamout lock. if necessary, output will active release CPU. */
    int tryCount = 10;
    while(mLockCount && tryCount--) {
        ALOGD("%s, free CPU, mLockCount %d, tryCount %d", __FUNCTION__, mLockCount, tryCount);
        usleep(300);
    }

    AudioAutoTimeoutLock _l(mLock);

    status_t status = NO_ERROR;

    // for routing
    if (AUDIO_DEVICE_NONE!=mRoutingDevice)
    {
        ALOGD("Routing : mStandby %d, mRoutingDevice %d", mStandby, mRoutingDevice);
        if(mStandby==false)
        {
            //ASSERT(mRoutingDevice != mStreamAttributeSource.output_devices); // TODO(Harvey): Could remove it after stress test
            ASSERT(mPlaybackHandler != NULL);
            //status = mPlaybackHandler->routing(output_devices);
            //if (status != NO_ERROR)
            if(mPlaybackHandler->getPlaybackHandlerType() != PLAYBACK_HANDLER_OFFLOAD)
            {
                status = close();
            }
        }

        mStreamAttributeSource.output_devices = mRoutingDevice;
        mRoutingDevice = AUDIO_DEVICE_NONE;
    }

    /// check open
    if (mStandby == true)
    {
        if (mStreamAttributeSource.mAudioOutputFlags & AUDIO_OUTPUT_FLAG_PRIMARY)
            usleep(22 * 1000);

        status = open();
        mStreamManager->setMasterVolume(mStreamManager->getMasterVolume());
//#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
        setLowLatencyMode(mLowLatencyMode);
//#endif
        mPlaybackHandler->setFirstDataWriteFlag(true);
    }
    else
    {
        ASSERT(mPlaybackHandler != NULL);
        mPlaybackHandler->setFirstDataWriteFlag(false);
    }

    WritePcmDumpData(buffer, bytes);

    /// write pcm data
    ASSERT(mPlaybackHandler != NULL);
    outputSize = mPlaybackHandler->write(buffer, bytes);
    mPresentedBytes += outputSize;
    //ALOGD("%s(), outputSize = %d, bytes = %d,mPresentedBytes=%d", __FUNCTION__, outputSize, bytes, mPresentedBytes);
    return outputSize;
}


status_t AudioALSAStreamOut::standby()
{
    ALOGD("%s()", __FUNCTION__);

    android_atomic_inc(&mLockCount);
    AudioAutoTimeoutLock _l(mLock);

    status_t status = NO_ERROR;

    /// check close
    if (mStandby == false)
    {
        status = close();
    }

    if (AUDIO_DEVICE_NONE != mRoutingDevice)
    {
        mStreamAttributeSource.output_devices = mRoutingDevice;
        mRoutingDevice = AUDIO_DEVICE_NONE;
    }

    android_atomic_dec(&mLockCount);
    return status;
}


status_t AudioALSAStreamOut::dump(int fd, const Vector<String16> &args)
{
    ALOGD("%s()", __FUNCTION__);
    return NO_ERROR;
}



bool AudioALSAStreamOut::SampleRateSupport(uint32_t sampleRate)
{
    if (sampleRate == 8000  || sampleRate == 11025 || sampleRate == 12000
        || sampleRate == 16000 || sampleRate == 22050 || sampleRate == 24000
        || sampleRate == 32000 || sampleRate == 44100 || sampleRate == 48000
        || sampleRate == 88200 || sampleRate == 96000 || sampleRate == 176400 || sampleRate == 192000)
    {
        return true;
    }
    else
    {
        return false;
    }
}

status_t AudioALSAStreamOut::UpdateSampleRate(int sampleRate)
{
    ALOGD("%s() sampleRate = %d", __FUNCTION__, sampleRate);
    // check sample rate
    if (SampleRateSupport(sampleRate) == true)
    {
        AudioALSASampleRateController::getInstance()->setPrimaryStreamOutSampleRate(sampleRate);
        mStreamAttributeSource.sample_rate = sampleRate;
        setBufferSize();
    }
    else
    {
        ALOGE("%s(), wrong sampleRate %d, use %d instead.", __FUNCTION__, sampleRate, kDefaultOutputSourceSampleRate);
        sampleRate = kDefaultOutputSourceSampleRate;
    }
    return NO_ERROR;
}



status_t AudioALSAStreamOut::setParameters(const String8 &keyValuePairs)
{
    ALOGD("+%s(): %s", __FUNCTION__, keyValuePairs.string());
    AudioParameter param = AudioParameter(keyValuePairs);

    /// keys
    const String8 keyRouting = String8(AudioParameter::keyRouting);
    const String8 keySampleRate = String8(AudioParameter::keySamplingRate);
    const String8 keyDynamicSampleRate = String8("DynamicSampleRate");
    const String8 keyLowLatencyMode = String8("LowLatencyMode");
#ifdef MTK_BASIC_PACKAGE
    const String8 keyRoutingToNone = String8(AUDIO_PARAMETER_KEY_ROUTING_TO_NONE);
    const String8 keyFmDirectControl = String8(AUDIO_PARAMETER_KEY_FM_DIRECT_CONTROL);
#else
    const String8 keyRoutingToNone = String8(AudioParameter::keyRoutingToNone);
    const String8 keyFmDirectControl = String8(AudioParameter::keyFmDirectControl);
#endif

    audio_devices_t mydevice = 0;


    /// parse key value pairs
    status_t status = NO_ERROR;
    int value = 0;

    /// routing
    if (param.getInt(keyRouting, value) == NO_ERROR)
    {
        param.remove(keyRouting);

        mydevice = static_cast<audio_devices_t>(value);
        ALOGD("%s(), mydevice 0x%x", __FUNCTION__, mydevice);

        android_atomic_inc(&mLockCount);
        AudioAutoTimeoutLock _l(mLock);
        if (mStreamOutType == STREAM_OUT_PRIMARY)
        {
            status = mStreamManager->routingOutputDevice(this, AudioALSAHardwareResourceManager::getInstance()->getOutputDevice(), static_cast<audio_devices_t>(value));
        }
        else if ((mStreamOutType == STREAM_OUT_HDMI_STEREO) || (mStreamOutType == STREAM_OUT_HDMI_MULTI_CHANNEL))
        {
            ALOGD("%s(), HDMI  \"%s\"", __FUNCTION__, param.toString().string());
            status = mStreamManager->routingOutputDevice(this, mStreamAttributeSource.output_devices, static_cast<audio_devices_t>(value));
        }
        else
        {
            ALOGW("%s(), NUM_STREAM_OUT_TYPE \"%s\"", __FUNCTION__, param.toString().string());
            status = INVALID_OPERATION;
        }

        // When device don't need routing, streamout need to be updated output_devices
        if(AudioALSAHardwareResourceManager::getInstance()->getOutputDevice() == (audio_devices_t)value)
        {
            mStreamAttributeSource.output_devices = (audio_devices_t)value;
        }
        android_atomic_dec(&mLockCount);
    }
    if (param.getInt(keyFmDirectControl, value) == NO_ERROR)
    {
        param.remove(keyFmDirectControl);

        android_atomic_inc(&mLockCount);
        AudioAutoTimeoutLock _l(mLock);
        AudioALSAFMController::getInstance()->setUseFmDirectConnectionMode(value?true:false);
        android_atomic_dec(&mLockCount);
    }
    // routing none, for no stream but has device change. e.g. vow path change
    if (param.getInt(keyRoutingToNone, value) == NO_ERROR)
    {
        param.remove(keyRoutingToNone);

        android_atomic_inc(&mLockCount);
        AudioAutoTimeoutLock _l(mLock);
        status = mStreamManager->DeviceNoneUpdate();
        android_atomic_dec(&mLockCount);
    }
    // samplerate
    if (param.getInt(keySampleRate, value) == NO_ERROR)
    {
        param.remove(keySampleRate);
        android_atomic_inc(&mLockCount);
        AudioAutoTimeoutLock _l(mLock);
        if (mPlaybackHandler == NULL)
        {
            UpdateSampleRate(value);
        }
        else
        {
            status = INVALID_OPERATION;
        }
        android_atomic_dec(&mLockCount);
    }

    /// sample rate
    if (param.getInt(keyDynamicSampleRate, value) == NO_ERROR)
    {
        param.remove(keyRouting);

        android_atomic_inc(&mLockCount);
        AudioAutoTimeoutLock _l(mLock);
        if (mStreamOutType == STREAM_OUT_PRIMARY)
        {
            status = NO_ERROR; //AudioALSASampleRateController::getInstance()->setPrimaryStreamOutSampleRate(value); // TODO(Harvey): enable it later
        }
        else
        {
            ALOGW("%s(), HDMI bypass \"%s\"", __FUNCTION__, param.toString().string());
            status = INVALID_OPERATION;
        }
        android_atomic_dec(&mLockCount);
    }

#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
    // set low latency
    if (param.getInt(keyLowLatencyMode, value) == NO_ERROR)
    {
        param.remove(keyLowLatencyMode);
        setLowLatencyMode(value);
    }
#endif

    if (param.size())
    {
        ALOGW("%s(), still have param.size() = %d, remain param = \"%s\"",
              __FUNCTION__, param.size(), param.toString().string());
        status = BAD_VALUE;
    }

    ALOGD("-%s(): %s ", __FUNCTION__, keyValuePairs.string());
    return status;
}


String8 AudioALSAStreamOut::getParameters(const String8 &keys)
{
#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
    ALOGD("%s, keyvalue %s", __FUNCTION__, keys.string());

    String8 value;
    String8 keyLowLatency = String8("LowLatency");

    AudioParameter param = AudioParameter(keys);
    AudioParameter returnParam = AudioParameter();

    if (param.get(keyLowLatency, value) == NO_ERROR)
    {
        param.remove(keyLowLatency);

        char buf[10];
        sprintf(buf, "%d", LOW_LATENCY_HAL_BUFFER_SIZE);
        returnParam.add(keyLowLatency, String8(buf));
    }

    const String8 keyValuePairs = returnParam.toString();
    ALOGD("-%s(), return \"%s\"", __FUNCTION__, keyValuePairs.string());
    return keyValuePairs;
#else
    ALOGD("%s()", __FUNCTION__);
    AudioParameter param = AudioParameter(keys);
    return param.toString();
#endif
}


status_t AudioALSAStreamOut::getRenderPosition(uint32_t *dspFrames)
{
	if(mPlaybackHandler == NULL)
    {
        ALOGE("%s() handler NULL, frames: %ld", __FUNCTION__, mPresentFrames);
        *dspFrames = mPresentFrames;
        return INVALID_OPERATION;
    }

    if(mPlaybackHandler->getPlaybackHandlerType() == PLAYBACK_HANDLER_OFFLOAD)
    {
        unsigned long codec_io_frame;
        unsigned int codec_samplerate;
        if(NO_ERROR == mPlaybackHandler->get_timeStamp(&codec_io_frame, &codec_samplerate) )
        {
            if(codec_samplerate == 0)
            {
                ALOGE("%s(), Compress Not Ready", __FUNCTION__);
                return INVALID_OPERATION;
            }
            *dspFrames = codec_io_frame;
            mPresentFrames = codec_io_frame;
            ALOGV("%s(), get_tstamp done:%d ", __FUNCTION__, *dspFrames);
            return NO_ERROR;
        }
        else
        {
            *dspFrames = mPresentFrames;
            ALOGE("%s(), get_tstamp fail, frame:%ld", __FUNCTION__, mPresentFrames);
            return INVALID_OPERATION;
        }
    }
    return NO_ERROR;
}

status_t AudioALSAStreamOut::getPresentationPosition(uint64_t *frames, struct timespec *timestamp)
{
    ALOGV("+%s()", __FUNCTION__);
    AudioAutoTimeoutLock _l(mLock);

    time_info_struct_t HW_Buf_Time_Info;
    memset(&HW_Buf_Time_Info, 0, sizeof(HW_Buf_Time_Info));

    const uint8_t size_per_channel = (mStreamAttributeSource.audio_format == AUDIO_FORMAT_PCM_8_BIT ? 1 :
                                      (mStreamAttributeSource.audio_format == AUDIO_FORMAT_PCM_16_BIT ? 2 :
                                       (mStreamAttributeSource.audio_format == AUDIO_FORMAT_PCM_32_BIT ? 4 :
                                        2)));
    ALOGV("%s(), size_per_channel = %u, mStreamAttributeSource.num_channels = %d, mStreamAttributeSource.audio_channel_mask = %x,mPresentedBytes = %lu",
          __FUNCTION__, size_per_channel, mStreamAttributeSource.num_channels, mStreamAttributeSource.audio_channel_mask, mPresentedBytes);

    if (mPlaybackHandler == NULL)
    {
        *frames = mPresentedBytes / (uint64_t)(mStreamAttributeSource.num_channels * size_per_channel);
        ALOGE("-%s(), no playback handler, *frames = %ld, return", __FUNCTION__, *frames);
        return INVALID_OPERATION;
    }


    //query remaining hardware buffer size
    status_t status = NO_ERROR;

    if(mPlaybackHandler->getPlaybackHandlerType() == PLAYBACK_HANDLER_OFFLOAD)
    {
        unsigned long codec_io_frame;
        unsigned int codec_samplerate;
        unsigned long time;
        if(NO_ERROR == mPlaybackHandler->get_timeStamp(&codec_io_frame, &codec_samplerate) )
        {
            if(codec_samplerate == 0)
            {
                *frames = 0;
                timestamp->tv_sec = 0;
                timestamp->tv_nsec = 0;
                ALOGE("%s(), Compress Not Ready", __FUNCTION__);
                return INVALID_OPERATION;
            }
            *frames = codec_io_frame;
            mPresentFrames = codec_io_frame;
            time = codec_io_frame / codec_samplerate;
            timestamp->tv_sec = time;
            time = codec_io_frame % codec_samplerate;
            timestamp->tv_nsec = time * 1000000000 / codec_samplerate;
            ALOGV("%s(), get_tstamp done:%ld ", __FUNCTION__, *frames);
        }
        else
        {
            *frames = mPresentFrames;
            ALOGD("%s(), get_tstamp fail, frames:%ld", __FUNCTION__, mPresentFrames);
            return INVALID_OPERATION;
        }
    }
    else
    {

        if (mPlaybackHandler->getHardwareBufferInfo(&HW_Buf_Time_Info) != NO_ERROR)
        {
            *frames = mPresentedBytes / (uint64_t)(mStreamAttributeSource.num_channels * size_per_channel);
            status = INVALID_OPERATION;

            ALOGV("-%s(), getHardwareBufferInfo fail, *frames = %ld, return INVALID_OPERATION", __FUNCTION__, *frames);
        }
        else
        {
            uint64_t PresentedFrames = mPresentedBytes / (uint64_t)(mStreamAttributeSource.num_channels * size_per_channel);
            *frames = (uint64_t)PresentedFrames - (uint64_t)(HW_Buf_Time_Info.buffer_per_time - HW_Buf_Time_Info.frameInfo_get);
            *timestamp = HW_Buf_Time_Info.timestamp_get;
            status = NO_ERROR;

            ALOGV("-%s(), *frames = %ld", __FUNCTION__, *frames);
        }
    }
    return status;
}

status_t AudioALSAStreamOut::getNextWriteTimestamp(int64_t *timestamp)
{
    return INVALID_OPERATION;
}

status_t AudioALSAStreamOut::setCallBack(stream_callback_t callback, void *cookie)
{
    //TODO : new for KK
    mStreamCbk = callback;
	mCbkCookie = cookie;
    return NO_ERROR;
}


status_t AudioALSAStreamOut::open()
{
    // call open() only when mLock is locked.
    ASSERT(mLock.tryLock() != 0);

    ALOGD("%s(), flags %d", __FUNCTION__, mStreamAttributeSource.mAudioOutputFlags);

    status_t status = NO_ERROR;

    if (mStandby == true)
    {
        mStandby = false;

        // HDMI stereo + HDMI multi-channel => disable HDMI stereo
        if (mStreamOutType == STREAM_OUT_HDMI_MULTI_CHANNEL)
        {
            ALOGD("Force disable mStreamOutHDMIStereo");
            AudioALSAStreamOut::setSuspendStreamOutHDMIStereo(true);
            if (mStreamOutHDMIStereo != NULL) { mStreamOutHDMIStereo->standby(); }
        }

        // create playback handler
        ASSERT(mPlaybackHandler == NULL);
        AudioALSASampleRateController::getInstance()->setScenarioStatus(PLAYBACK_SCENARIO_STREAM_OUT);
        mPlaybackHandler = mStreamManager->createPlaybackHandler(&mStreamAttributeSource);

        // open audio hardware
        status = mPlaybackHandler->open();

        // offload allow return fail
        if(mPlaybackHandler->getPlaybackHandlerType() == PLAYBACK_HANDLER_OFFLOAD)
        {
            if(status == NO_ERROR)
            {
                mPlaybackHandler->setComprCallback(mStreamCbk, mCbkCookie);
                mPlaybackHandler->setVolume(mOffloadVol);
            }
        }
        else
            ASSERT(status == NO_ERROR);


        OpenPCMDump(LOG_TAG);
    }

    return status;
}


status_t AudioALSAStreamOut::close()
{
    // call close() only when mLock is locked.
    ASSERT(mLock.tryLock() != 0);

    ALOGD("%s(), flags %d", __FUNCTION__, mStreamAttributeSource.mAudioOutputFlags);

    status_t status = NO_ERROR;

    if (mStandby == false)
    {
        mStandby = true;

        // HDMI stereo + HDMI multi-channel => disable HDMI stereo
        if (mStreamOutType == STREAM_OUT_HDMI_MULTI_CHANNEL)
        {
            ALOGD("Recover mStreamOutHDMIStereo");
            AudioALSAStreamOut::setSuspendStreamOutHDMIStereo(false);
        }

        ASSERT(mPlaybackHandler != NULL);

        // close audio hardware
        status = mPlaybackHandler->close();
        if (status != NO_ERROR)
        {
            ALOGE("%s(), close() fail!!", __FUNCTION__);
        }

        ClosePCMDump();

        // destroy playback handler
        mStreamManager->destroyPlaybackHandler(mPlaybackHandler);
        mPlaybackHandler = NULL;
        AudioALSASampleRateController::getInstance()->resetScenarioStatus(PLAYBACK_SCENARIO_STREAM_OUT);
    }

    ASSERT(mPlaybackHandler == NULL);
    return status;
}

status_t AudioALSAStreamOut::pause()
{
    ALOGD("%s() %p", __FUNCTION__, mPlaybackHandler);
    if(mPlaybackHandler != NULL)
    {
        mPaused = true;
        return mPlaybackHandler->pause();
    }
    return INVALID_OPERATION;
}

status_t AudioALSAStreamOut::resume()
{
    ALOGD("%s() %p", __FUNCTION__, mPlaybackHandler);
    if(mPlaybackHandler != NULL)
    {
        mPaused = false;
        return mPlaybackHandler->resume();
    }
    return INVALID_OPERATION;
}

status_t AudioALSAStreamOut::flush()
{
    ALOGD("%s() %p", __FUNCTION__, mPlaybackHandler);
    if(mPlaybackHandler != NULL)
        return mPlaybackHandler->flush();
    return INVALID_OPERATION;
}

int AudioALSAStreamOut::drain(audio_drain_type_t type)
{
    ALOGD("%s() %p", __FUNCTION__, mPlaybackHandler);
    if(mPlaybackHandler != NULL)
        return mPlaybackHandler->drain(type);
    return 0;
}

audio_devices_t AudioALSAStreamOut::getOutputDevice()
{
    if (mRoutingDevice != AUDIO_DEVICE_NONE)
        return mRoutingDevice;
    else
        return getStreamAttribute()->output_devices;
}

status_t AudioALSAStreamOut::routing(audio_devices_t output_devices)
{
    bool is_lock_in_this_function = false;
    if (mLock.tryLock() == 0) // from another stream out setParameter routing
    {
        ALOGD("%s(), is_lock_in_this_function = true", __FUNCTION__);
        is_lock_in_this_function = true;
    }

    ALOGD("+%s(), output_devices = 0x%x", __FUNCTION__, output_devices);

    status_t status = NO_ERROR;

    if (mStandby == false)
    {
        //ASSERT(output_devices != mStreamAttributeSource.output_devices); // TODO(Harvey): Could remove it after stress test

        ASSERT(mPlaybackHandler != NULL);
        //status = mPlaybackHandler->routing(output_devices);
        //if (status != NO_ERROR)

        if(mPlaybackHandler->getPlaybackHandlerType() == PLAYBACK_HANDLER_OFFLOAD)
        {
            status = pause();
            AudioALSAHardwareResourceManager::getInstance()->stopOutputDevice();
            mStreamAttributeSource.output_devices = output_devices;
        }
        else
        {
            mRoutingDevice = output_devices;
        }
    }
    else
    {
        mStreamAttributeSource.output_devices = output_devices;
    }

    if(mPaused == true)
    {
        if(mPlaybackHandler->getPlaybackHandlerType() == PLAYBACK_HANDLER_OFFLOAD)
        {
            const stream_attribute_t *StreamAttributeTarget = mPlaybackHandler->getStreamAttributeTarget();
            AudioALSAHardwareResourceManager::getInstance()->startOutputDevice(mStreamAttributeSource.output_devices, StreamAttributeTarget->sample_rate);
            status = resume();
        }
    }

    if (is_lock_in_this_function == true)
    {
        mLock.unlock();
    }

    ALOGD("-%s()", __FUNCTION__);
    return status;
}


status_t AudioALSAStreamOut::setLowLatencyMode(bool mode)
{
    ALOGD("+%s(), mode %d", __FUNCTION__, mode);

    mLowLatencyMode = mode;

    if (NULL != mPlaybackHandler)
    {
#ifdef MTK_DYNAMIC_CHANGE_HAL_BUFFER_SIZE
        mPlaybackHandler->setLowLatencyMode(mode != 0, LOW_LATENCY_HAL_BUFFER_SIZE, 4096);
#else
        mPlaybackHandler->setLowLatencyMode(mode != 0, mStreamAttributeSource.buffer_size, 8192);
#endif
    }
    return NO_ERROR;
}


status_t AudioALSAStreamOut::setSuspend(const bool suspend_on)
{
    ALOGD("+%s(), mSuspendCount = %u, suspend_on = %d", __FUNCTION__, mSuspendCount, suspend_on);

    if (suspend_on == true)
    {
        mSuspendCount++;
    }
    else if (suspend_on == false)
    {
        ASSERT(mSuspendCount > 0);
        mSuspendCount--;
    }

    ALOGD("-%s(), mSuspendCount = %u", __FUNCTION__, mSuspendCount);
    return NO_ERROR;
}


status_t AudioALSAStreamOut::setSuspendStreamOutHDMIStereo(const bool suspend_on)
{
    ALOGD("+%s(), mSuspendStreamOutHDMIStereoCount = %u, suspend_on = %d",
          __FUNCTION__, mSuspendStreamOutHDMIStereoCount, suspend_on);

    if (suspend_on == true)
    {
        mSuspendStreamOutHDMIStereoCount++;
    }
    else if (suspend_on == false)
    {
        ASSERT(mSuspendStreamOutHDMIStereoCount > 0);
        mSuspendStreamOutHDMIStereoCount--;
    }

    ALOGD("-%s(), mSuspendStreamOutHDMIStereoCount = %u", __FUNCTION__, mSuspendStreamOutHDMIStereoCount);
    return NO_ERROR;
}


void AudioALSAStreamOut::OpenPCMDump(const char *class_name)
{
    ALOGV("%s()", __FUNCTION__);
    char mDumpFileName[128];
    sprintf(mDumpFileName, "%s.%d.%s.pid%d.tid%d.pcm", streamout, mDumpFileNum, class_name, getpid(), gettid());

    mPCMDumpFile = NULL;
    mPCMDumpFile = AudioOpendumpPCMFile(mDumpFileName, streamout_propty);

    if (mPCMDumpFile != NULL)
    {
        ALOGD("%s DumpFileName = %s", __FUNCTION__, mDumpFileName);

        mDumpFileNum++;
        mDumpFileNum %= MAX_DUMP_NUM;
    }
}

void AudioALSAStreamOut::ClosePCMDump()
{
    ALOGV("%s()", __FUNCTION__);
    if (mPCMDumpFile)
    {
        AudioCloseDumpPCMFile(mPCMDumpFile);
        ALOGD("%s(), close it", __FUNCTION__);
    }
}

void  AudioALSAStreamOut::WritePcmDumpData(const void *buffer, ssize_t bytes)
{
    if (mPCMDumpFile)
    {
        //ALOGD("%s()", __FUNCTION__);
        AudioDumpPCMData((void *)buffer , bytes, mPCMDumpFile);
    }
}

void  AudioALSAStreamOut::setBufferSize()
{
#ifdef DOWNLINK_LOW_LATENCY
    if (mStreamAttributeSource.mAudioOutputFlags & AUDIO_OUTPUT_FLAG_FAST)
    {
        if (mStreamAttributeSource.sample_rate <= 48000)
            mStreamAttributeSource.buffer_size = FRAME_COUNT_MIN_PER_ACCESSS;
        else if (mStreamAttributeSource.sample_rate <= 96000)
            mStreamAttributeSource.buffer_size = FRAME_COUNT_MIN_PER_ACCESSS << 1;
        else if (mStreamAttributeSource.sample_rate <= 192000)
            mStreamAttributeSource.buffer_size = FRAME_COUNT_MIN_PER_ACCESSS << 2;
        else
        {
            ASSERT(0);
        }
        mStreamAttributeSource.latency = (mStreamAttributeSource.buffer_size * 2 * 1000) / mStreamAttributeSource.sample_rate;
        mStreamAttributeSource.buffer_size *= mStreamAttributeSource.num_channels
                                  * audio_bytes_per_sample(mStreamAttributeSource.audio_format);
    }
#endif
}

}
