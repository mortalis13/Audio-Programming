#include <signal.h>
#include <stdint.h>

#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"
#include "opt_common.h"


const char program_name[] = "ffplay";
const int program_birth_year = 2003;

#define SHOW_STATUS 1

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE SAMPLE_QUEUE_SIZE


typedef struct FrameQueue {
    AVFrame* queue[FRAME_QUEUE_SIZE];
    
    int read_index;
    int write_index;
    int size;
    int max_size;
    
    int abort_request;
    
    SDL_mutex *mutex;
    SDL_cond *cond;
} FrameQueue;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    int paused;
} Clock;

typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct VideoState {
    char *filename;
    
    SDL_Thread *read_tid;
    SDL_cond *continue_read_thread;
    
    int abort_request;
    int paused;
    int last_paused;
    
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    
    int audio_stream_index;
    
    AVFormatContext *avformat;
    AVCodecContext *avcontext;
    AVStream *avstream;
    
    FrameQueue frame_queue;
    
    Clock audio_clock;
    double audio_clock_pts;
    
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    
    struct AudioParams audio_src;
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;

    int eof;
} VideoState;


#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)


static const char *input_filename;
static int64_t audio_callback_time;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;


static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);
static int read_thread(void *arg);
static void stream_close(VideoState *is);
static void set_clock(Clock *c, double pts, double time);

void show_help_default(const char *opt, const char *arg) {}


// ==== Frame Queue ====
static int frame_queue_init(FrameQueue *fq, int max_size) {
    int i;
    memset(fq, 0, sizeof(FrameQueue));
    
    if (!(fq->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    if (!(fq->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    fq->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    
    for (i = 0; i < fq->max_size; i++) {
        fq->queue[i] = av_frame_alloc();
        if (!fq->queue[i]) return AVERROR(ENOMEM);
    }
    return 0;
}

static void frame_queue_unref_item(AVFrame *frame) {
    av_frame_unref(frame);
}

static void frame_queue_destroy(FrameQueue *fq) {
    int i;
    for (i = 0; i < fq->max_size; i++) {
        AVFrame *frame = fq->queue[i];
        frame_queue_unref_item(frame);
        av_frame_free(&frame);
    }
    
    SDL_DestroyMutex(fq->mutex);
    SDL_DestroyCond(fq->cond);
}

static AVFrame *frame_queue_peek_writable(FrameQueue *fq) {
    /* wait until we have space to put a new frame */
    SDL_LockMutex(fq->mutex);
    while (fq->size >= fq->max_size && !fq->abort_request) {
        SDL_CondWait(fq->cond, fq->mutex);
    }
    SDL_UnlockMutex(fq->mutex);
    
    if (fq->abort_request) return NULL;
    return fq->queue[fq->write_index];
}

static AVFrame *frame_queue_peek_readable(FrameQueue *fq) {
    /* wait until we have a readable a new frame */
    SDL_LockMutex(fq->mutex);
    while (fq->size - 1 <= 0 && !fq->abort_request) {
        SDL_CondWait(fq->cond, fq->mutex);
    }
    SDL_UnlockMutex(fq->mutex);
    
    if (fq->abort_request) return NULL;
    return fq->queue[(fq->read_index + 1) % fq->max_size];
}

static void frame_queue_push(FrameQueue *fq) {
    if (++fq->write_index == fq->max_size) {
        fq->write_index = 0;
    }
    
    SDL_LockMutex(fq->mutex);
    fq->size++;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

static void frame_queue_next(FrameQueue *fq) {
    frame_queue_unref_item(fq->queue[fq->read_index]);
    if (++fq->read_index == fq->max_size) {
        fq->read_index = 0;
    }
    
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

static void frame_queue_flush(FrameQueue *fq) {
    int i;
    SDL_LockMutex(fq->mutex);
    
    for (i = 0; i < fq->max_size; i++) {
        frame_queue_unref_item(fq->queue[i]);
    }
    fq->read_index = 0;
    fq->write_index = 0;
    fq->size = 0;

    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}


// ==== Clock ====
static void init_clock(Clock *c) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock(c, NAN, time);
    c->paused = 0;
}

static void set_clock(Clock *c, double pts, double time) {
    c->pts = pts;
    c->pts_drift = c->pts - time;
}

static double get_clock(Clock *c) {
    double time;
    if (c->paused) return c->pts;

    time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time;
}

static double get_master_clock(VideoState *is) {
    return get_clock(&is->audio_clock);
}


/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is) {
    AVFrame *frame;
    int data_size, resampled_data_size;
    double pts;

    if (is->paused) return -1;

    frame = frame_queue_peek_readable(&is->frame_queue);
    if (!frame) return -1;
    frame_queue_next(&is->frame_queue);

    data_size = av_samples_get_buffer_size(NULL, frame->ch_layout.nb_channels, frame->nb_samples, frame->format, 1);

    if (frame->format != is->audio_src.fmt || frame->sample_rate != is->audio_src.freq || av_channel_layout_compare(&frame->ch_layout, &is->audio_src.ch_layout)) {
        swr_free(&is->swr_ctx);
        swr_alloc_set_opts2(&is->swr_ctx, &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                            &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
        
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    frame->sample_rate, av_get_sample_fmt_name(frame->format), frame->ch_layout.nb_channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &frame->ch_layout) < 0) return -1;
        
        is->audio_src.freq = frame->sample_rate;
        is->audio_src.fmt = frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **) frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t) frame->nb_samples * is->audio_tgt.freq / frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) return AVERROR(ENOMEM);
        
        len2 = swr_convert(is->swr_ctx, out, out_count, in, frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0) {
                swr_free(&is->swr_ctx);
            }
        }
        
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    }
    else {
        is->audio_buf = frame->data[0];
        resampled_data_size = data_size;
    }

    /* update the audio clock with the pts */
    is->audio_clock_pts = NAN;
    pts = frame->pts;
    if (pts != AV_NOPTS_VALUE) {
        is->audio_clock_pts = pts / frame->sample_rate + (double) frame->nb_samples / frame->sample_rate;
    }
    
    return resampled_data_size;
}

static int audio_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    
    SDL_AudioSpec wanted_spec, spec;
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
    int wanted_nb_channels = wanted_channel_layout->nb_channels;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    
    if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
    }
    
    wanted_nb_channels = wanted_channel_layout->nb_channels;
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
        next_sample_rate_idx--;
    }
    
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    
    while ( !(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE)) ) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
    }
    
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    
    if (spec.channels != wanted_spec.channels) {
        av_channel_layout_uninit(wanted_channel_layout);
        av_channel_layout_default(wanted_channel_layout, spec.channels);
        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
            av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    
    if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0) {
        return -1;
    }
    
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *avformat = is->avformat;
    AVCodecContext *avcontext;
    const AVCodec *codec;
    
    AVChannelLayout ch_layout = { 0 };
    
    int ret = 0;

    if (stream_index < 0 || stream_index >= avformat->nb_streams) return -1;

    if (!(avcontext = avcodec_alloc_context3(NULL))) return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(avcontext, avformat->streams[stream_index]->codecpar)) < 0) goto fail;
    
    avcontext->pkt_timebase = avformat->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avcontext->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avcontext->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avcontext->codec_id = codec->id;

    if ((ret = avcodec_open2(avcontext, codec, NULL)) < 0) goto fail;

    is->eof = 0;
    avformat->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    
    if ((ret = av_channel_layout_copy(&ch_layout, &avcontext->ch_layout)) < 0) goto fail;

    /* prepare audio output */
    if ((ret = audio_open(is, &ch_layout, avcontext->sample_rate, &is->audio_tgt)) < 0) goto fail;
    
    is->audio_hw_buf_size = ret;
    is->audio_src = is->audio_tgt;
    is->audio_buf_size  = 0;
    is->audio_buf_index = 0;

    is->audio_stream_index = stream_index;
    is->avstream = avformat->streams[stream_index];

    is->avcontext = avcontext;
    
    SDL_PauseAudioDevice(audio_dev, 0);
    goto out;

fail:
    avcodec_free_context(&avcontext);

out:
    av_channel_layout_uninit(&ch_layout);
    return ret;
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *avformat = is->avformat;
    if (stream_index < 0 || stream_index >= avformat->nb_streams) return;
    
    SDL_CloseAudioDevice(audio_dev);

    swr_free(&is->swr_ctx);
    av_freep(&is->audio_buf1);
    is->audio_buf1_size = 0;
    is->audio_buf = NULL;

    avformat->streams[stream_index]->discard = AVDISCARD_ALL;
    
    is->avstream = NULL;
    is->audio_stream_index = -1;
}

static VideoState *stream_open(const char *filename) {
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is) return NULL;
    
    is->filename = av_strdup(filename);
    if (!is->filename) goto fail;
    
    if (frame_queue_init(&is->frame_queue, SAMPLE_QUEUE_SIZE) < 0) goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->audio_clock);
    
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }

    return is;
}

static void stream_close(VideoState *is) {
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    is->frame_queue.abort_request = 1;
    SDL_CondSignal(is->frame_queue.cond);
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream_index >= 0) stream_component_close(is, is->audio_stream_index);
    avformat_close_input(&is->avformat);

    frame_queue_destroy(&is->frame_queue);
    SDL_DestroyCond(is->continue_read_thread);

    av_free(is->filename);
    av_free(is);
}


static int decode_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = opaque;
    int audio_size, bytes_to_write;

    audio_callback_time = av_gettime_relative();
    
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            is->audio_buf_size = audio_size;
            is->audio_buf_index = 0;
            
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            }
        }
        
        bytes_to_write = is->audio_buf_size - is->audio_buf_index;
        if (bytes_to_write > len) bytes_to_write = len;
        
        memset(stream, 0, bytes_to_write);
        if (is->audio_buf) {
            memcpy(stream, (uint8_t*) is->audio_buf + is->audio_buf_index, bytes_to_write);
        }
        
        len -= bytes_to_write;
        stream += bytes_to_write;
        is->audio_buf_index += bytes_to_write;
    }
    
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock_pts)) {
        double pts = is->audio_clock_pts - (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec;
        set_clock(&is->audio_clock, pts, audio_callback_time / 1000000.0);
    }
}


static int read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *avformat = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;

    AVFrame *queue_frame;
    AVRational tb;
    
    int err, i, ret;
    int stream_index = -1;

    SDL_mutex *wait_mutex = SDL_CreateMutex();
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate frame.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    avformat = avformat_alloc_context();
    if (!avformat) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    avformat->interrupt_callback.callback = decode_interrupt_cb;
    avformat->interrupt_callback.opaque = is;
    
    ret = avformat_open_input(&avformat, is->filename, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not open input file %s: %s.\n", is->filename, av_err2str(err));
        goto fail;
    }
    
    is->avformat = avformat;

    av_format_inject_global_side_data(avformat);

    ret = avformat_find_stream_info(avformat, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not find stream info: %s.\n", av_err2str(err));
        goto fail;
    }

    if (avformat->pb) {
        avformat->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    }

    for (i = 0; i < avformat->nb_streams; i++) {
        avformat->streams[i]->discard = AVDISCARD_ALL;
    }

    stream_index = av_find_best_stream(avformat, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (stream_index >= 0) {
        stream_component_open(is, stream_index);
    }

    if (is->audio_stream_index < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to a stream in the file %s\n", is->filename);
        ret = -1;
        goto fail;
    }
    
    printf("Start read loop...\n");

    for (;;) {
        if (is->abort_request) break;
        
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) {
                av_read_pause(avformat);
            }
            else {
                av_read_play(avformat);
            }
        }

        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            if (avformat_seek_file(is->avformat, -1, seek_min, seek_target, seek_max, is->seek_flags) < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->avformat->url);
            }
            else {
                avcodec_flush_buffers(is->avcontext);
                frame_queue_flush(&is->frame_queue);
            }
            
            is->seek_req = 0;
            is->eof = 0;
        }
        
        /* if the queue is full, no need to read more */
        if (is->frame_queue.size > MAX_QUEUE_SIZE) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);  // 10ms
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        ret = av_read_frame(avformat, pkt);
        
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(avformat->pb)) && !is->eof) {
                if (is->audio_stream_index >= 0) {
                    printf("EOF - put null packet\n");
                    avcodec_send_packet(is->avcontext, pkt);
                }
                is->eof = 1;
            }
            if (avformat->pb && avformat->pb->error) break;
            
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);  // 10ms
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        else {
            is->eof = 0;
        }
        
        if (pkt->stream_index != is->audio_stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        avcodec_send_packet(is->avcontext, pkt);
        
        ret = 0;
        while (ret >= 0) {
            if (is->abort_request) break;
            
            ret = avcodec_receive_frame(is->avcontext, frame);
            if (ret == AVERROR_EOF) {
                printf("Frame EOF\n");
                avcodec_flush_buffers(is->avcontext);
                break;
            }
            
            if (ret == AVERROR(EAGAIN)) break;
            
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Frame receive error: %d -> %s", ret, av_err2str(ret));
                break;
            }
            
            tb = (AVRational){1, frame->sample_rate};
            frame->pts = av_rescale_q(frame->pts, is->avcontext->pkt_timebase, tb);
            
            queue_frame = frame_queue_peek_writable(&is->frame_queue);
            if (!queue_frame) break;
            av_frame_move_ref(queue_frame, frame);
            frame_queue_push(&is->frame_queue);
        }
        
        av_packet_unref(pkt);
    }

    ret = 0;
 
 fail:
    if (avformat && !is->avformat) {
        avformat_close_input(&avformat);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    
    if (ret != 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    
    SDL_DestroyMutex(wait_mutex);
    return 0;
}


static void stream_seek(VideoState *is, int64_t pos, int64_t rel) {
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

static void toggle_pause(VideoState *is) {
    is->paused = is->audio_clock.paused = !is->paused;
}


static void do_exit(VideoState *is) {
    if (is) stream_close(is);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    
    uninit_opts();
    avformat_network_deinit();
    printf("\n");
    
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig) {
    exit(123);
}

static void show_status(VideoState *is) {
    AVBPrint buf;
    static int64_t last_time;
    int64_t cur_time;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(&buf, "%7.2f\r", get_master_clock(is));
        av_log(NULL, AV_LOG_INFO, "%s", buf.str);

        fflush(stderr);
        av_bprint_finalize(&buf, NULL);

        last_time = cur_time;
    }
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    SDL_PumpEvents();
    
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (remaining_time > 0.0) {
            av_usleep((int64_t)(remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;

        if (SHOW_STATUS) show_status(is);
        SDL_PumpEvents();
    }
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *is) {
    SDL_Event event;
    double incr, pos;

    for (;;) {
        refresh_loop_wait_event(is, &event);
        
        switch (event.type) {
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                do_exit(is);
                break;
            
            case SDLK_SPACE:
                toggle_pause(is);
                break;
            
            case SDLK_LEFT:
                incr = -10.0;
                goto do_seek;
            
            case SDLK_RIGHT:
                incr = 10.0;
                goto do_seek;
            
            do_seek:
                pos = get_master_clock(is);
                if (isnan(pos)) {
                    pos = (double) is->seek_pos / AV_TIME_BASE;
                }

                pos += incr;
                if (is->avformat->start_time != AV_NOPTS_VALUE && pos < is->avformat->start_time / (double) AV_TIME_BASE) {
                    pos = is->avformat->start_time / (double) AV_TIME_BASE;
                }
                
                stream_seek(is, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE));
                break;
            default:
                break;
            }
            break;

        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit(is);
            break;
        default:
            break;
        }
    }
}


/* Called from the main */
int main(int argc, char **argv) {
    int flags;
    VideoState *is;

    init_dynload();
    opt_loglevel(NULL, "loglevel", "verbose");

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
    
    input_filename = argv[1];
    
    /* Try to work around an occasional ALSA buffer underflow issue when the
     * period size is NPOT due to ALSA resampling by forcing the buffer size. */
    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE")) {
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }

    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    
    window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (window) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        
        if (renderer && !SDL_GetRendererInfo(renderer, &renderer_info)) {
            av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    
    if (!window || !renderer || !renderer_info.num_texture_formats) {
        av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        do_exit(NULL);
    }

    is = stream_open(input_filename);
    
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    event_loop(is);

    return 0;
}
