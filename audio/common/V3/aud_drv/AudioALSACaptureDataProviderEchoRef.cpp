#include "AudioALSACaptureDataProviderEchoRef.h"

#include <pthread.h>

#include <linux/rtpm_prio.h>
#include <sys/prctl.h>

#include "AudioType.h"

#include "AudioALSADriverUtility.h"
#include "AudioALSASampleRateController.h"
#include "AudioALSAStreamManager.h"


#define LOG_TAG "AudioALSACaptureDataProviderEchoRef"

#define calc_time_diff(x,y) ((x.tv_sec - y.tv_sec )+ (double)( x.tv_nsec - y.tv_nsec ) / (double)1000000000)

namespace android
{


/*==============================================================================
 *                     Constant
 *============================================================================*/

//static const uint32_t kReadBufferSize = (((uint32_t)(44.1*20*2*2)) & 0xFFFFFFC0); // (DL1)44.1K\20ms data\stereo\2byte\(Align64byte)
static uint32_t kReadBufferSize = 0 ;


/*==============================================================================
 *                     Implementation
 *============================================================================*/

AudioALSACaptureDataProviderEchoRef *AudioALSACaptureDataProviderEchoRef::mAudioALSACaptureDataProviderEchoRef = NULL;
AudioALSACaptureDataProviderEchoRef *AudioALSACaptureDataProviderEchoRef::getInstance()
{
    AudioLock mGetInstanceLock;
    AudioAutoTimeoutLock _l(mGetInstanceLock);

    if (mAudioALSACaptureDataProviderEchoRef == NULL)
    {
        mAudioALSACaptureDataProviderEchoRef = new AudioALSACaptureDataProviderEchoRef();
    }
    ASSERT(mAudioALSACaptureDataProviderEchoRef != NULL);
    return mAudioALSACaptureDataProviderEchoRef;
}

AudioALSACaptureDataProviderEchoRef::AudioALSACaptureDataProviderEchoRef()
{
    ALOGD("%s()", __FUNCTION__);

    mCaptureDataProviderType = CAPTURE_PROVIDER_ECHOREF;
}

AudioALSACaptureDataProviderEchoRef::~AudioALSACaptureDataProviderEchoRef()
{
    ALOGD("%s()", __FUNCTION__);
}


status_t AudioALSACaptureDataProviderEchoRef::open()
{
    ALOGD("%s()", __FUNCTION__);
    ASSERT(mClientLock.tryLock() != 0); // lock by base class attach
    AudioAutoTimeoutLock _l(mEnableLock);
    AudioAutoTimeoutLock _l2(*AudioALSADriverUtility::getInstance()->getStreamSramDramLock());

    ASSERT(mEnable == false);

    AudioALSASampleRateController *pAudioALSASampleRateController = AudioALSASampleRateController::getInstance();
    pAudioALSASampleRateController->setScenarioStatus(PLAYBACK_SCENARIO_ECHO_REF);

    // config attribute (will used in client SRC/Enh/... later) // TODO(Sam): query the mConfig?
    mStreamAttributeSource.audio_format = AUDIO_FORMAT_PCM_16_BIT;
    mStreamAttributeSource.audio_channel_mask = AUDIO_CHANNEL_IN_STEREO;
    mStreamAttributeSource.num_channels = android_audio_legacy::AudioSystem::popCount(mStreamAttributeSource.audio_channel_mask);
    mStreamAttributeSource.sample_rate = pAudioALSASampleRateController->getPrimaryStreamOutSampleRate();
    uint32_t latency = getLatencyTime();

    mConfig.rate = mStreamAttributeSource.sample_rate;
    mConfig.channels = mStreamAttributeSource.num_channels;
    mConfig.format = PCM_FORMAT_S16_LE;
    kReadBufferSize = (((uint32_t)((mStreamAttributeSource.sample_rate / 1000) * latency * mConfig.channels * (pcm_format_to_bits(mConfig.format) / 8))) & 0xFFFFFFC0); // (DL1)44.1K\20ms data\stereo\2byte\(Align64byte)
    // Buffer size: 2048(period_size) * 2(ch) * 2(byte) * 8(period_count) = 64 kb
    mConfig.period_size = kReadBufferSize / mConfig.channels / (pcm_format_to_bits(mConfig.format) / 8);
    mConfig.period_count = 4;

    if (latency == UPLINK_LOW_LATENCY_MS)
    {
        mConfig.period_count = 8; // 2*(20ms/5ms);
    }

    mConfig.start_threshold = 0;
    mConfig.stop_threshold = 0;
    mConfig.silence_threshold = 0;


#if 0
    //latency time, set as DataProvider buffer size
    mStreamAttributeSource.latency = (kReadBufferSize * 1000) / (mStreamAttributeSource.num_channels * mStreamAttributeSource.sample_rate *
                                                                 (mStreamAttributeSource.audio_format == AUDIO_FORMAT_PCM_8_BIT ? 1 :    //8  1byte/frame
                                                                  (mStreamAttributeSource.audio_format == AUDIO_FORMAT_PCM_32_BIT ? 4 :   //24bit 3bytes/frame
                                                                   2)));   //default 2bytes/sample
#else
    //latency time, set as hardware buffer size
    mStreamAttributeSource.latency = (mConfig.period_size * mConfig.period_count * 1000) / mConfig.rate;
#endif

    ALOGD("%s(), audio_format = %d, audio_channel_mask=%x, num_channels=%d, sample_rate=%d, latency=%dms", __FUNCTION__,
          mStreamAttributeSource.audio_format, mStreamAttributeSource.audio_channel_mask, mStreamAttributeSource.num_channels, mStreamAttributeSource.sample_rate, mStreamAttributeSource.latency);

    ALOGD("%s(), format = %d, channels=%d, rate=%d, period_size=%d, period_count=%d,latency=%d,kReadBufferSize=%d", __FUNCTION__,
          mConfig.format, mConfig.channels, mConfig.rate,mConfig.period_size,mConfig.period_count,latency,kReadBufferSize);

    OpenPCMDump(LOG_TAG);

    // enable pcm
    ASSERT(mPcm == NULL);
    int pcmIdx = AudioALSADeviceParser::getInstance()->GetPcmIndexByString(keypcmDl1AwbCapture);
    int cardIdx = AudioALSADeviceParser::getInstance()->GetCardIndexByString(keypcmDl1AwbCapture);
    mPcm = pcm_open(cardIdx, pcmIdx, PCM_IN | PCM_MONOTONIC, &mConfig);
    ASSERT(mPcm != NULL && pcm_is_ready(mPcm) == true);

    pcm_start(mPcm);

    // create reading thread
    mEnable = true;
    int ret = pthread_create(&hReadThread, NULL, AudioALSACaptureDataProviderEchoRef::readThread, (void *)this);
    if (ret != 0)
    {
        ALOGE("%s() create thread fail!!", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    return NO_ERROR;
}

uint32_t AudioALSACaptureDataProviderEchoRef::getLatencyTime()
{
    mlatency = UPLINK_NORMAL_LATENCY_MS ; //20ms
    bool audiomode = AudioALSAStreamManager::getInstance()->isModeInVoipCall();
    ALOGD("%s(), audiomode=%d", __FUNCTION__, audiomode);
#ifdef UPLINK_LOW_LATENCY
    //VOIP not use fast record
    if ((audiomode == false) && HasLowLatencyCapture())
    {
        mlatency = UPLINK_LOW_LATENCY_MS; //5ms
    }
#endif
    return mlatency;

}

status_t AudioALSACaptureDataProviderEchoRef::close()
{
    ALOGD("%s()", __FUNCTION__);
    ASSERT(mClientLock.tryLock() != 0); // lock by base class detach

    mEnable = false;
    AudioAutoTimeoutLock _l(mEnableLock);
    AudioAutoTimeoutLock _l2(*AudioALSADriverUtility::getInstance()->getStreamSramDramLock());

    ClosePCMDump();

    pcm_stop(mPcm);
    pcm_close(mPcm);
    mPcm = NULL;

    AudioALSASampleRateController *pAudioALSASampleRateController = AudioALSASampleRateController::getInstance();
    pAudioALSASampleRateController->resetScenarioStatus(PLAYBACK_SCENARIO_ECHO_REF);

    return NO_ERROR;
}

void *AudioALSACaptureDataProviderEchoRef::readThread(void *arg)
{
    pthread_detach(pthread_self());

    status_t retval = NO_ERROR;
    AudioALSACaptureDataProviderEchoRef *pDataProvider = static_cast<AudioALSACaptureDataProviderEchoRef *>(arg);

    uint32_t open_index = pDataProvider->mOpenIndex;

    char nameset[32];
    sprintf(nameset, "%s%d", __FUNCTION__, pDataProvider->mCaptureDataProviderType);
    prctl(PR_SET_NAME, (unsigned long)nameset, 0, 0, 0);

#ifdef MTK_AUDIO_ADJUST_PRIORITY
    // force to set priority
    struct sched_param sched_p;
    sched_getparam(0, &sched_p);
    sched_p.sched_priority = RTPM_PRIO_AUDIO_RECORD + 5;
    if (0 != sched_setscheduler(0, SCHED_RR, &sched_p))
    {
        ALOGE("[%s] failed, errno: %d", __FUNCTION__, errno);
    }
    else
    {
        sched_p.sched_priority = RTPM_PRIO_AUDIO_RECORD + 5;
        sched_getparam(0, &sched_p);
        ALOGD("sched_setscheduler ok, priority: %d", sched_p.sched_priority);
    }
#endif
    ALOGD("+%s(), pid: %d, tid: %d, kReadBufferSize=%x", __FUNCTION__, getpid(), gettid(), kReadBufferSize);


    // read raw data from alsa driver
    char linear_buffer[kReadBufferSize];
    while (pDataProvider->mEnable == true)
    {
        if (open_index != pDataProvider->mOpenIndex)
        {
            ALOGD("%s(), open_index(%d) != mOpenIndex(%d), return", __FUNCTION__, open_index, pDataProvider->mOpenIndex);
            break;
        }

        retval = pDataProvider->mEnableLock.lock_timeout(300);
        ASSERT(retval == NO_ERROR);
        if (pDataProvider->mEnable == false)
        {
            pDataProvider->mEnableLock.unlock();
            break;
        }

        ASSERT(pDataProvider->mPcm != NULL);
        clock_gettime(CLOCK_REALTIME, &pDataProvider->mNewtime);
        pDataProvider->timerec[0] = calc_time_diff(pDataProvider->mNewtime, pDataProvider->mOldtime);
        pDataProvider->mOldtime = pDataProvider->mNewtime;

        int retval = pcm_read(pDataProvider->mPcm, linear_buffer, kReadBufferSize);
        if (retval != 0)
        {
            ALOGE("%s(), pcm_read() error, retval = %d", __FUNCTION__, retval);
        }

        clock_gettime(CLOCK_REALTIME, &pDataProvider->mNewtime);
        pDataProvider->timerec[1] = calc_time_diff(pDataProvider->mNewtime, pDataProvider->mOldtime);
        pDataProvider->mOldtime = pDataProvider->mNewtime;

        pDataProvider->GetCaptureTimeStamp(&pDataProvider->mStreamAttributeSource.Time_Info, kReadBufferSize);

        // use ringbuf format to save buffer info
        pDataProvider->mPcmReadBuf.pBufBase = linear_buffer;
        pDataProvider->mPcmReadBuf.bufLen   = kReadBufferSize + 1; // +1: avoid pRead == pWrite
        pDataProvider->mPcmReadBuf.pRead    = linear_buffer;
        pDataProvider->mPcmReadBuf.pWrite   = linear_buffer + kReadBufferSize;
        pDataProvider->mEnableLock.unlock();

        //Provide EchoRef data
#if 0
        pDataProvider->provideCaptureDataToAllClients(open_index);
#else
        pDataProvider->provideEchoRefCaptureDataToAllClients(open_index);
#endif
        clock_gettime(CLOCK_REALTIME, &pDataProvider->mNewtime);
        pDataProvider->timerec[2] = calc_time_diff(pDataProvider->mNewtime, pDataProvider->mOldtime);
        pDataProvider->mOldtime = pDataProvider->mNewtime;

        if (pDataProvider->mPCMDumpFile)
        {
            ALOGD("%s, latency_in_us,%1.6lf,%1.6lf,%1.6lf", __FUNCTION__, pDataProvider->timerec[0], pDataProvider->timerec[1], pDataProvider->timerec[2]);
        }
    }

    ALOGD("-%s(), pid: %d, tid: %d", __FUNCTION__, getpid(), gettid());
    pthread_exit(NULL);
    return NULL;
}

} // end of namespace android
