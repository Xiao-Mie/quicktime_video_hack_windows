#pragma once

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <string>
#include <mutex>

class Mp4Recorder
{
public:
    Mp4Recorder();
    ~Mp4Recorder();

    bool Start(const std::string& filename);
    void Stop();

    bool InputVideoExtradata(const unsigned char* buffer, int length);
    bool InputVideoSlice(const unsigned char* buffer, int length);

    bool InputAudioInit(int bits, int channels, int samplerate);
    bool InputAudio(const unsigned char* buffer, int length);

private:
    bool CheckAndWriteHeader();
    void FlushAudio();

    std::mutex m_mutex;
    std::string m_filename;
    AVFormatContext* m_fmtCtx;
    
    AVStream* m_videoStream;
    bool m_bVideoExtradataReceived;
    int m_frameCount;
    
    AVStream* m_audioStream;
    AVCodecContext* m_audioCodecCtx;
    SwrContext* m_swrCtx;
    AVAudioFifo* m_audioFifo;
    bool m_bAudioInited;
    int64_t m_audioPts;
    int m_audioBits;
    int m_audioChannels;

    bool m_bStarted;
    bool m_bHeaderWritten;
    int64_t m_startTime;
};
