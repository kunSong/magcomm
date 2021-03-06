#ifndef _AUDIO_PRE_PROCESS_H
#define _AUDIO_PRE_PROCESS_H

#include <stdint.h>
#include <sys/types.h>
#include <utils/threads.h>

#include <hardware_legacy/AudioHardwareBase.h>
#include <system/audio.h>
#include <hardware/audio.h>
#include "AudioType.h"
#include "AudioSpeechEnhanceInfo.h"

#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <audio_effects/effect_aec.h>

#include "AudioLock.h"


namespace android
{

struct effect_info_s
{
    effect_handle_t effect_itfe;
    size_t num_channel_configs;
    channel_config_t *channel_configs;
};

class AudioPreProcess
{
    public:
        AudioPreProcess(const stream_attribute_t *streamIn_attribute);
        ~AudioPreProcess();

        void stop_echo_reference(struct echo_reference_itfe *reference);
        //tuna: void put_echo_reference(struct echo_reference_itfe *reference);

        struct echo_reference_itfe *start_echo_reference(audio_format_t format, uint32_t channel_count, uint32_t sampling_rate);
        //tuna: echo_reference_itfe *get_echo_reference(audio_format_t format,uint32_t channel_count,uint32_t sampling_rate);
        void push_echo_reference(size_t frames);
        int in_configure_reverse(uint32_t channel_count, uint32_t sampling_rate);

        status_t addAudioEffect(effect_handle_t effect);
        status_t removeAudioEffect(effect_handle_t effect);

        uint32_t NativePreprocess(void *buffer , uint32_t bytes, const time_info_struct_t *Time_Info);
        uint32_t WriteEchoRefData(void *buffer , uint32_t bytes, const time_info_struct_t *Time_Info);

        status_t CheckNativeEffect(void);

        int num_preprocessors;
        bool need_echo_reference;
        struct effect_info_s preprocessors[MAX_PREPROCESSORS];
        int16_t *proc_buf_in;
        int16_t *proc_buf_out;
        size_t proc_buf_size;
        size_t proc_buf_frames;

    private:

        void add_echo_reference(struct echo_reference_itfe *reference);
        void clear_echo_reference(struct echo_reference_itfe *reference);
        void remove_echo_reference(struct echo_reference_itfe *reference);
        int32_t update_echo_reference(size_t frames);
        int set_preprocessor_echo_delay(effect_handle_t handle, int32_t delay_us);
        int set_preprocessor_param(effect_handle_t handle, effect_param_t *param);
        void get_capture_delay(size_t frames, struct echo_reference_buffer *buffer);
        void get_echoref_delay(size_t frames, struct echo_reference_buffer *buffer);


        bool MutexLock(void);
        bool MutexUnlock(void);

        int16_t *ref_buf;
        size_t ref_buf_size;
        size_t ref_buf_frames;
        struct echo_reference_itfe *mEcho_Reference;


        /**
         * AudioPreProcess lock
         */
        AudioLock mLock;
        //Mutex   mLock;

        int mInChn;
        uint32_t mInSampleRate;
        AudioSpeechEnhanceInfo *mAudioSpeechEnhanceInfoInstance;
        const stream_attribute_t *mStreamInAttribute; // for stream in

        time_info_struct_t mTime_Info;
        time_info_struct_t mTime_Info_echoref;
        static const uint32_t mEchoRefChannelCount = 2;
};
}
#endif
