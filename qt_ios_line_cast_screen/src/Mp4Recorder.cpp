#include "Mp4Recorder.h"
#include <QDebug>

Mp4Recorder::Mp4Recorder()
    : m_fmtCtx(nullptr)
    , m_videoStream(nullptr)
    , m_bVideoExtradataReceived(false)
    , m_frameCount(0)
    , m_audioStream(nullptr)
    , m_audioCodecCtx(nullptr)
    , m_swrCtx(nullptr)
    , m_audioFifo(nullptr)
    , m_bAudioInited(false)
    , m_audioPts(0)
    , m_audioBits(16)
    , m_audioChannels(2)
    , m_bStarted(false)
    , m_bHeaderWritten(false)
    , m_startTime(0)
{
}

Mp4Recorder::~Mp4Recorder()
{
    Stop();
}

bool Mp4Recorder::Start(const std::string& filename)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_bStarted) return true;

    m_filename = filename;
    m_fmtCtx = nullptr;
    m_videoStream = nullptr;
    m_bVideoExtradataReceived = false;
    m_frameCount = 0;

    m_audioStream = nullptr;
    m_audioCodecCtx = nullptr;
    m_swrCtx = nullptr;
    m_audioFifo = nullptr;
    m_bAudioInited = false;
    m_audioPts = 0;

    m_bHeaderWritten = false;

    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mp4", m_filename.c_str());
    if (ret < 0 || !m_fmtCtx) {
        qDebug() << "avformat_alloc_output_context2 failed";
        return false;
    }

    m_videoStream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_videoStream) {
        qDebug() << "avformat_new_stream failed";
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_videoStream->codecpar->codec_id = AV_CODEC_ID_H264;
    m_videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    m_videoStream->time_base = { 1, 1000 };

    m_bStarted = true;
    return true;
}

void Mp4Recorder::FlushAudio()
{
    if (!m_audioCodecCtx || !m_audioStream) return;
    avcodec_send_frame(m_audioCodecCtx, nullptr);
    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(m_audioCodecCtx, pkt) == 0) {
        av_packet_rescale_ts(pkt, m_audioCodecCtx->time_base, m_audioStream->time_base);
        pkt->stream_index = m_audioStream->index;
        av_interleaved_write_frame(m_fmtCtx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

void Mp4Recorder::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bStarted) return;

    if (m_fmtCtx) {
        if (m_bHeaderWritten) {
            FlushAudio();
            av_write_trailer(m_fmtCtx);
        }
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE) && m_fmtCtx->pb) {
            avio_closep(&m_fmtCtx->pb);
        }
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }

    if (m_audioCodecCtx) {
        avcodec_free_context(&m_audioCodecCtx);
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_audioFifo) {
        av_audio_fifo_free(m_audioFifo);
        m_audioFifo = nullptr;
    }

    m_bStarted = false;
    m_bHeaderWritten = false;
}

bool Mp4Recorder::CheckAndWriteHeader()
{
    if (m_bHeaderWritten) return true;
    if (m_bVideoExtradataReceived && m_bAudioInited) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            int ret = avio_open(&m_fmtCtx->pb, m_filename.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                qDebug() << "avio_open failed";
                return false;
            }
        }
        int ret = avformat_write_header(m_fmtCtx, nullptr);
        if (ret < 0) {
            qDebug() << "avformat_write_header failed";
            return false;
        }
        m_bHeaderWritten = true;
        m_startTime = av_gettime(); // reset start time to first frame
        return true;
    }
    return false;
}

bool Mp4Recorder::InputVideoExtradata(const unsigned char* buffer, int length)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bStarted || !m_fmtCtx || !m_videoStream) return false;
    if (m_bVideoExtradataReceived) return true; // Already initialized

    if (length <= 4) return false;

    // The iOS extradata starts with a 4-byte size or header, then the AVCDecoderConfigurationRecord.
    int extradata_size = length - 4;
    m_videoStream->codecpar->extradata = (uint8_t*)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(m_videoStream->codecpar->extradata, buffer + 4, extradata_size);
    m_videoStream->codecpar->extradata_size = extradata_size;

    m_bVideoExtradataReceived = true;
    CheckAndWriteHeader();
    return true;
}

bool Mp4Recorder::InputVideoSlice(const unsigned char* buffer, int length)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bStarted || !m_bHeaderWritten || !m_fmtCtx) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    av_new_packet(pkt, length);
    memcpy(pkt->data, buffer, length);
    pkt->stream_index = m_videoStream->index;

    // Calculate pts in milliseconds
    int64_t now = av_gettime();
    int64_t pts_ms = (now - m_startTime) / 1000;

    pkt->pts = av_rescale_q(pts_ms, { 1, 1000 }, m_videoStream->time_base);
    pkt->dts = pkt->pts;
    pkt->duration = av_rescale_q(1000 / 60, { 1, 1000 }, m_videoStream->time_base); // approx 60fps

    if (m_frameCount == 0) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    
    int ret = av_interleaved_write_frame(m_fmtCtx, pkt);
    
    av_packet_free(&pkt);

    if (ret < 0) {
        qDebug() << "av_interleaved_write_frame failed for video";
        return false;
    }

    m_frameCount++;
    return true;
}

bool Mp4Recorder::InputAudioInit(int bits, int channels, int samplerate)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bStarted || !m_fmtCtx) return false;
    if (m_bAudioInited) return true;

    m_audioBits = bits;
    m_audioChannels = channels;

    const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audioCodec) {
        qDebug() << "AAC encoder not found";
        return false;
    }

    m_audioCodecCtx = avcodec_alloc_context3(audioCodec);
    m_audioCodecCtx->sample_fmt = audioCodec->sample_fmts ? audioCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    m_audioCodecCtx->bit_rate = 128000;
    m_audioCodecCtx->sample_rate = samplerate;
    m_audioCodecCtx->channels = channels;
    m_audioCodecCtx->channel_layout = av_get_default_channel_layout(channels);
    
    if (avcodec_open2(m_audioCodecCtx, audioCodec, nullptr) < 0) {
        qDebug() << "Failed to open AAC encoder";
        return false;
    }

    m_audioStream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_audioStream) return false;

    avcodec_parameters_from_context(m_audioStream->codecpar, m_audioCodecCtx);
    m_audioStream->time_base = { 1, m_audioCodecCtx->sample_rate };

    m_swrCtx = swr_alloc();
    av_opt_set_int(m_swrCtx, "in_channel_layout",  m_audioCodecCtx->channel_layout, 0);
    av_opt_set_int(m_swrCtx, "in_sample_rate",     samplerate, 0);
    
    AVSampleFormat in_fmt = AV_SAMPLE_FMT_S16;
    if (bits == 8) in_fmt = AV_SAMPLE_FMT_U8;
    else if (bits == 32) in_fmt = AV_SAMPLE_FMT_S32;
    av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", in_fmt, 0); 
    
    av_opt_set_int(m_swrCtx, "out_channel_layout", m_audioCodecCtx->channel_layout, 0);
    av_opt_set_int(m_swrCtx, "out_sample_rate",    m_audioCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", m_audioCodecCtx->sample_fmt, 0);
    
    if (swr_init(m_swrCtx) < 0) {
        qDebug() << "Failed to initialize resampler";
        return false;
    }

    m_audioFifo = av_audio_fifo_alloc(m_audioCodecCtx->sample_fmt, channels, 1);
    if (!m_audioFifo) return false;

    m_bAudioInited = true;
    CheckAndWriteHeader();
    return true;
}

bool Mp4Recorder::InputAudio(const unsigned char* buffer, int length)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_bStarted || !m_bHeaderWritten || !m_fmtCtx || !m_bAudioInited) return false;

    int bytes_per_sample = m_audioBits / 8;
    int num_samples = length / (m_audioChannels * bytes_per_sample);
    uint8_t* in_data[1] = { (uint8_t*)buffer };
    
    int out_samples = swr_get_out_samples(m_swrCtx, num_samples);
    uint8_t** resampled_data = nullptr;
    av_samples_alloc_array_and_samples(&resampled_data, nullptr, m_audioCodecCtx->channels, out_samples, m_audioCodecCtx->sample_fmt, 0);
    
    int ret = swr_convert(m_swrCtx, resampled_data, out_samples, (const uint8_t**)in_data, num_samples);
    if (ret < 0) {
        if (resampled_data) {
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
        }
        return false;
    }
    
    av_audio_fifo_write(m_audioFifo, (void**)resampled_data, ret);
    
    av_freep(&resampled_data[0]);
    av_freep(&resampled_data);
    
    while (av_audio_fifo_size(m_audioFifo) >= m_audioCodecCtx->frame_size) {
        AVFrame* frame = av_frame_alloc();
        frame->nb_samples = m_audioCodecCtx->frame_size;
        frame->channel_layout = m_audioCodecCtx->channel_layout;
        frame->format = m_audioCodecCtx->sample_fmt;
        frame->sample_rate = m_audioCodecCtx->sample_rate;
        av_frame_get_buffer(frame, 0);
        
        av_audio_fifo_read(m_audioFifo, (void**)frame->data, m_audioCodecCtx->frame_size);
        
        frame->pts = m_audioPts;
        m_audioPts += frame->nb_samples;
        
        ret = avcodec_send_frame(m_audioCodecCtx, frame);
        if (ret >= 0) {
            AVPacket* pkt = av_packet_alloc();
            while (avcodec_receive_packet(m_audioCodecCtx, pkt) == 0) {
                av_packet_rescale_ts(pkt, m_audioCodecCtx->time_base, m_audioStream->time_base);
                pkt->stream_index = m_audioStream->index;
                av_interleaved_write_frame(m_fmtCtx, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        av_frame_free(&frame);
    }
    return true;
}
