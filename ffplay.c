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


typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;
typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    double pts;           /* presentation timestamp for the frame */
    int serial;
} Frame;
typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    PacketQueue *pktq;
    
    int read_index;
    int write_index;
    int size;
    int max_size;
    
    SDL_mutex *mutex;
    SDL_cond *cond;
} FrameQueue;

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    
    int pkt_serial;
    int packet_pending;
    
    SDL_cond *empty_queue_cond;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
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
    
    AVFormatContext *ic;
    AVStream *audio_st;
    int audio_stream_index;
    
    Decoder auddec;
    Clock audclk;
    PacketQueue audioq;
    FrameQueue sampq;
    
    double audio_clock;
    int audio_clock_serial;
    
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

static AVPacket flush_pkt;


static void sdl_audio_callback(void *opaque, Uint8 *stream, int len);
static int audio_thread(void *arg);
static int read_thread(void *arg);
static void stream_close(VideoState *is);
void show_help_default(const char *opt, const char *arg) {}


// ==== Packet Queue ====
static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    
    if (!(q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW))) return AVERROR(ENOMEM);
    
    if (!(q->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    if (!(q->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    q->abort_request = 1;
    return 0;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList pkt1;
    int ret;

    if (q->abort_request) return -1;

    pkt1.pkt = pkt;
    if (pkt == &flush_pkt) q->serial++;
    pkt1.serial = q->serial;

    if ((ret = av_fifo_write(q->pkt_list, &pkt1, 1)) < 0) return ret;
    
    q->nb_packets++;
    q->size += pkt1.pkt->size + sizeof(pkt1);
    q->duration += pkt1.pkt->duration;

    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacket *pkt1;
    int ret;

    pkt1 = av_packet_alloc();
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if (pkt1 != &flush_pkt && ret < 0) {
        av_packet_free(&pkt1);
    }

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index) {
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList pkt1;
    SDL_LockMutex(q->mutex);
    
    while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
        av_packet_free(&pkt1.pkt);
    }
    
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    q->serial++;
    
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    av_fifo_freep2(&q->pkt_list);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 1;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
            q->nb_packets--;
            q->size -= pkt1.pkt->size + sizeof(pkt1);
            q->duration -= pkt1.pkt->duration;
            
            av_packet_move_ref(pkt, pkt1.pkt);
            
            if (serial) {
                *serial = pkt1.serial;
            }
            
            av_packet_free(&pkt1.pkt);
            ret = 1;
            break;
        }
        else if (!block) {
            ret = 0;
            break;
        }
        else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    
    SDL_UnlockMutex(q->mutex);
    return ret;
}


// ==== Frame Queue ====
static int frame_queue_init(FrameQueue *fq, PacketQueue *pktq, int max_size) {
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
    
    fq->pktq = pktq;
    fq->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    
    for (i = 0; i < fq->max_size; i++) {
        fq->queue[i].frame = av_frame_alloc();
        if (!fq->queue[i].frame) return AVERROR(ENOMEM);
    }
    return 0;
}

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
}

static void frame_queue_destroy(FrameQueue *fq) {
    int i;
    for (i = 0; i < fq->max_size; i++) {
        Frame *vp = &fq->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    
    SDL_DestroyMutex(fq->mutex);
    SDL_DestroyCond(fq->cond);
}

static void frame_queue_signal(FrameQueue *fq) {
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

static Frame *frame_queue_peek_writable(FrameQueue *fq) {
    /* wait until we have space to put a new frame */
    SDL_LockMutex(fq->mutex);
    while (fq->size >= fq->max_size && !fq->pktq->abort_request) {
        SDL_CondWait(fq->cond, fq->mutex);
    }
    SDL_UnlockMutex(fq->mutex);

    if (fq->pktq->abort_request) return NULL;

    return &fq->queue[fq->write_index];
}

static Frame *frame_queue_peek_readable(FrameQueue *fq) {
    /* wait until we have a readable a new frame */
    SDL_LockMutex(fq->mutex);
    while (fq->size - 1 <= 0 && !fq->pktq->abort_request) {
        SDL_CondWait(fq->cond, fq->mutex);
    }
    SDL_UnlockMutex(fq->mutex);

    if (fq->pktq->abort_request) return NULL;

    return &fq->queue[(fq->read_index + 1) % fq->max_size];
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
    frame_queue_unref_item(&fq->queue[fq->read_index]);
    if (++fq->read_index == fq->max_size) {
        fq->read_index = 0;
    }
    
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}


// ==== Clock ====
static double get_clock(Clock *c) {
    double time;
    if (*c->queue_serial != c->serial) return NAN;
    if (c->paused) return c->pts;

    time = av_gettime_relative() / 1000000.0;
    return c->pts_drift + time;
}

static void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void init_clock(Clock *c, int *queue_serial) {
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static double get_master_clock(VideoState *is) {
    return get_clock(&is->audclk);
}


// ==== Decoder ====
static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg) {
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->pkt = av_packet_alloc();
    if (!d->pkt) return AVERROR(ENOMEM);
    
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->pkt_serial = -1;
    return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame) {
    int ret = AVERROR(EAGAIN);
    for (;;) {

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request) return -1;

                ret = avcodec_receive_frame(d->avctx, frame);
                if (ret >= 0) {
                    AVRational tb = (AVRational){1, frame->sample_rate};
                    if (frame->pts != AV_NOPTS_VALUE) {
                        frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                    }
                }
                
                if (ret == AVERROR_EOF) {
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                
                if (ret >= 0) return 1;
            }
            while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0) {
                SDL_CondSignal(d->empty_queue_cond);
            }
            
            if (d->packet_pending) {
                d->packet_pending = 0;
            }
            else {
                if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0) return -1;
            }
            
            if (d->queue->serial == d->pkt_serial) break;
            av_packet_unref(d->pkt);
        }
        while (1);
        
        if (d->pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            continue;
        }

        if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
            av_log(d->avctx, AV_LOG_ERROR, "receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
            d->packet_pending = 1;
        }
        else {
            av_packet_unref(d->pkt);
        }

    } // for(;;)
}

static void decoder_destroy(Decoder *d) {
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}


/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused) return -1;

    do {
        if (!(af = frame_queue_peek_readable(&is->sampq))) return -1;
        frame_queue_next(&is->sampq);
    }
    while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels, af->frame->nb_samples, af->frame->format, 1);

    wanted_nb_samples = af->frame->nb_samples;

    if (af->frame->format != is->audio_src.fmt || af->frame->sample_rate != is->audio_src.freq || av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout)) {
        swr_free(&is->swr_ctx);
        swr_alloc_set_opts2(&is->swr_ctx, &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                            &af->frame->ch_layout, af->frame->format, af->frame->sample_rate, 0, NULL);
        
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->ch_layout.nb_channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        
        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0) return -1;
        
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **) af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t) wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1) return AVERROR(ENOMEM);
        
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
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
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    /* update the audio clock with the pts */
    if (!isnan(af->pts)) {
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    }
    else {
        is->audio_clock = NAN;
    }
    is->audio_clock_serial = af->serial;

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
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    
    AVChannelLayout ch_layout = { 0 };
    
    int ret = 0;

    if (stream_index < 0 || stream_index >= ic->nb_streams) return -1;

    if (!(avctx = avcodec_alloc_context3(NULL))) return AVERROR(ENOMEM);
    if ((ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar)) < 0) goto fail;
    
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING, "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;

    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) goto fail;

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    
    if ((ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout)) < 0) goto fail;

    /* prepare audio output */
    if ((ret = audio_open(is, &ch_layout, avctx->sample_rate, &is->audio_tgt)) < 0) goto fail;
    
    is->audio_hw_buf_size = ret;
    is->audio_src = is->audio_tgt;
    is->audio_buf_size  = 0;
    is->audio_buf_index = 0;

    is->audio_stream_index = stream_index;
    is->audio_st = ic->streams[stream_index];

    if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0) goto fail;
    if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0) goto out;
    
    SDL_PauseAudioDevice(audio_dev, 0);
    goto out;

fail:
    avcodec_free_context(&avctx);

out:
    av_channel_layout_uninit(&ch_layout);
    return ret;
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    if (stream_index < 0 || stream_index >= ic->nb_streams) return;
    
    decoder_abort(&is->auddec, &is->sampq);
    SDL_CloseAudioDevice(audio_dev);
    decoder_destroy(&is->auddec);
    swr_free(&is->swr_ctx);
    av_freep(&is->audio_buf1);
    is->audio_buf1_size = 0;
    is->audio_buf = NULL;

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    
    is->audio_st = NULL;
    is->audio_stream_index = -1;
}

static VideoState *stream_open(const char *filename) {
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is) return NULL;
    
    is->filename = av_strdup(filename);
    if (!is->filename) goto fail;
    
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE) < 0) goto fail;
    if (packet_queue_init(&is->audioq) < 0) goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->audclk, &is->audioq.serial);
    is->audio_clock_serial = -1;
    
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
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream_index >= 0) stream_component_close(is, is->audio_stream_index);
    avformat_close_input(&is->ic);
    packet_queue_destroy(&is->audioq);

    frame_queue_destroy(&is->sampq);
    SDL_DestroyCond(is->continue_read_thread);

    av_free(is->filename);
    av_free(is);
}


static int decode_interrupt_cb(void *ctx) {
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 || queue->abort_request ||
           queue->nb_packets > MIN_FRAMES &&
           (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}


static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    VideoState *is = opaque;
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            }
            else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) len1 = len;
        
        memset(stream, 0, len1);
        if (is->audio_buf) {
            memcpy(stream, (uint8_t*) is->audio_buf + is->audio_buf_index, len1);
        }
        
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        double pts = is->audio_clock - (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec;
        set_clock_at(&is->audclk, pts, is->audio_clock_serial, audio_callback_time / 1000000.0);
    }
}


static int audio_thread(void *arg) {
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();
    
    Frame *af;
    int got_frame = 0;
    AVRational tb;

    if (!frame) return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame)) < 0) break;

        if (got_frame) {
            tb = (AVRational){1, frame->sample_rate};

            if (!(af = frame_queue_peek_writable(&is->sampq))) break;

            af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            af->serial = is->auddec.pkt_serial;

            av_frame_move_ref(af->frame, frame);
            frame_queue_push(&is->sampq);
        }
    }
    while (1);
 
    av_frame_free(&frame);
    return 0;
}


/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg) {
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    AVPacket *pkt = NULL;
    
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
    
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    
    ret = avformat_open_input(&ic, is->filename, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not open input file %s: %s.\n", is->filename, av_err2str(err));
        goto fail;
    }
    
    is->ic = ic;

    av_format_inject_global_side_data(ic);

    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not find stream info: %s.\n", av_err2str(err));
        goto fail;
    }

    if (ic->pb) {
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    }

    for (i = 0; i < ic->nb_streams; i++) {
        ic->streams[i]->discard = AVDISCARD_ALL;
    }

    stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (stream_index >= 0) {
        stream_component_open(is, stream_index);
    }

    if (is->audio_stream_index < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to a stream in the file %s\n", is->filename);
        ret = -1;
        goto fail;
    }

    for (;;) {
        if (is->abort_request) break;
        
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused) {
                av_read_pause(ic);
            }
            else {
                av_read_play(ic);
            }
        }

        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            if (avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags) < 0) {
                av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", is->ic->url);
            }
            else {
                packet_queue_flush(&is->audioq);
                packet_queue_put(&is->audioq, &flush_pkt);
            }
            
            is->seek_req = 0;
            is->eof = 0;
        }
        
        /* if the queue is full, no need to read more */
        if (is->audioq.size > MAX_QUEUE_SIZE || stream_has_enough_packets(is->audio_st, is->audio_stream_index, &is->audioq)) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);  // 10ms
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        ret = av_read_frame(ic, pkt);
        
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->audio_stream_index >= 0) {
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream_index);
                }
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error) break;
            
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

        packet_queue_put(&is->audioq, pkt);
    }

    ret = 0;
 
 fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
    }

    av_packet_free(&pkt);
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
    is->paused = is->audclk.paused = !is->paused;
}

static void show_status(VideoState *is) {
    AVBPrint buf;
    static int64_t last_time;
    int64_t cur_time;
    int aqsize;

    cur_time = av_gettime_relative();
    if (!last_time || (cur_time - last_time) >= 30000) {
        aqsize = 0;
        if (is->audio_st) aqsize = is->audioq.size;

        av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
        av_bprintf(&buf, "%7.2f M-A aq=%5dKB  \r", get_master_clock(is), aqsize / 1024);
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

        show_status(is);
        SDL_PumpEvents();
    }
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


/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream) {
    SDL_Event event;
    double incr, pos;

    for (;;) {
        refresh_loop_wait_event(cur_stream, &event);
        
        switch (event.type) {
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                do_exit(cur_stream);
                break;
            
            case SDLK_SPACE:
                toggle_pause(cur_stream);
                break;
            
            case SDLK_LEFT:
                incr = -10.0;
                goto do_seek;
            
            case SDLK_RIGHT:
                incr = 10.0;
                goto do_seek;
            
            do_seek:
                pos = get_master_clock(cur_stream);
                
                if (isnan(pos)) {
                    pos = (double) cur_stream->seek_pos / AV_TIME_BASE;
                }
                pos += incr;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double) AV_TIME_BASE) {
                    pos = cur_stream->ic->start_time / (double) AV_TIME_BASE;
                }
                
                stream_seek(cur_stream, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE));
                break;
            default:
                break;
            }
            break;

        case SDL_QUIT:
        case FF_QUIT_EVENT:
            do_exit(cur_stream);
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
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t*) &flush_pkt;

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
