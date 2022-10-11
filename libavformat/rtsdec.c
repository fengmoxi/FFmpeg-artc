/* Copyright 2020, Alibaba
 */

// Created @2020/02/15 Sat.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libavformat/url.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/aes.h"
#include <assert.h>
#include <stdbool.h>

#define BUILDDLL

#if defined(WIN32)
  #if defined(BUILDDLL)
    #define DLL_PUBLIC __declspec(dllexport)
  #else
    #define DLL_PUBLIC __declspec(dllimport)
  #endif
#else
  #if defined(BUILDDLL)
    #define DLL_PUBLIC __attribute__ ((visibility("default")))
  #else
    #define DLL_PUBLIC
  #endif
#endif


#define DEBUG

/*
 * on : no dependency with rtssdk library. app calls
 *      av_set_rts_demuxer_funcs(get_rts_funcs(2)) somewhere
 *      to link this demuxer with rtssdk library
 * off: build this demuxer with rtssdk library. in this case,
 *      connection to rtssdk library is implictely invoked (see
 *      rtc_read_header)
 */
#define BUILD_INDEPENDANT_WITH_RTSSDKLIB

/*
 * on : build this file independantly, but need copy some header files
 *      from ffmpeg. At runtime, call init_rtsdec_plugin() in your app to
 *      register this plugin
 * off: include this file to ffmpeg makefile system (libavformat/makefile)
 */
//#define BUILD_AS_PLUGIN


// aac decoder
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libavcodec/avcodec.h"

static void *aac_decoder_create(int sample_rate, int channels);
static int aac_decode_frame(void *pParam,
                     unsigned char *pData,
                     int nLen,
                     unsigned char *pPCM,
                     unsigned int *outLen);
static void aac_decode_close(void *pParam);


//aes decrypt
static void* aes_alloc_context(void);
static int aes_init(void* context, const uint8_t* key, int keySize);
static int aes_decrypt(void* context, uint8_t* dst, const uint8_t* src, int size, uint8_t* iv);
static void aes_free_context(void* context);
//
// copied from rts_messages.h
//

/* event callback
 *
 * Set callback to rtssdk: call preconfig("MessageCallback", ...) and
 * preconfig("MessageCbParam", ...)
 * rtssdk will invoke your callback when an event happens (async mode). and
 * your callback need handle the event ASAP otherwise following event
 * messages will be blocked
 *
 * callback prototype
 * int (*event_callback)(void *opaque,
 *                    int type,
 *                    void *data,  // a string: "when=<time_in_ms>,where=<place>,who=<id>,desc=<extra text>"
 *                                 // if type == E_PROFILING_REPORT, desc=\"key1:val1,key2:val2,...\"
 *                                 // ATTENTION: temp data, do not cache it for later use
 *                    long long data_size // bytes of 'data'
 *                    );
 *
 * type:
 *       100~104       help support id messages
 *       105           state report event (called periodically)
 *                     'data' points to a rtssdk_profiling_data object
 *       1100          opening an url
 *       1101          received first audio packet
 *       1102          received first video packet
 *       1103          output first video frame
 *       1104          output first audio frame
 *       20000~30000   error event
 */

/* help support IDs
 * app need record these IDs. provide them to aliyun when ask for
 * issue support
 */
#define E_HELP_SUPPORT_ID_LOAD       100 // issue for loading
#define E_HELP_SUPPORT_ID_CONNECT    102 // issue for connect operation
#define E_HELP_SUPPORT_ID_PUBLISH    103 // issue for publish operation
#define E_HELP_SUPPORT_ID_SUBSCRIBE  104 // issue for subscribe operation

/* profiling data. sent every 4 seconds by default
 * data format: key1:value1,key2:value2,key3:value3...
 */
#define E_PROFILING_REPORT  105
/* got the aes keyinfo in stream, send to app to
 * fetch the plaintext key
 */
#define E_GOT_AESKEYINFO    106

#define EVENT_ERROR_BASE 20000
// errors happening during opening stage
#define E_DNS_FAIL          (EVENT_ERROR_BASE + 1 )  // could not resolve host name
#define E_AUTH_FAIL         (EVENT_ERROR_BASE + 2 )  // bad auth code
#define E_CONN_OK           (EVENT_ERROR_BASE + 9 )  // fail to connect to sfu
#define E_CONN_FAIL         (EVENT_ERROR_BASE + 10)  // fail to connect to sfu
#define E_SUB_TIMEOUT       (EVENT_ERROR_BASE + 12)  // timeout for subscribe response
#define E_SUB_NO_STREAM     (EVENT_ERROR_BASE + 13)  // stream not exist
#define E_SUB_NO_AUDIO      (EVENT_ERROR_BASE + 14)  // audio track not found
#define E_SUB_NO_VIDEO      (EVENT_ERROR_BASE + 15)  // video track not found
#define E_SUB_UNKNOWN_ERROR (EVENT_ERROR_BASE + 20)  // other unknown error

// errors happening during running stage
#define E_CONGESTION_BEGIN  (EVENT_ERROR_BASE + 50)  // lost rate too high
#define E_CONGESTION_END    (EVENT_ERROR_BASE + 51)  // lost rate decrease to normal level
#define E_STREAM_BROKEN     (EVENT_ERROR_BASE + 52)  // no any audio and video packets
#define E_STREAM_RECOVERED  (EVENT_ERROR_BASE + 53)  // audio or video packets recovered
#define E_STREAM_EOF        (EVENT_ERROR_BASE + 54)  // EOF received from sfu. App need stop playback
#define E_CONNECT_LOST      (EVENT_ERROR_BASE + 55)  // require reconnection
#define E_STREAM_RESTARTED  (EVENT_ERROR_BASE + 56)  // stream restart detected
#define E_DOWNGRADE_RTMP    (EVENT_ERROR_BASE + 57)  // need downgrade to rtmp

// structure to store subscribed stream info
// use ioctl(..., "get_stream_info", ...) to fetch
struct rts_worker_demux_info
{
    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid
    int audio_channels; // 1
    int audio_sample_rate; // 48000

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
    int video_codec;      // 1 - h264  2 - hevc
    int video_width;
    int video_height;
    int video_profile;
    int video_level;

    unsigned char spspps[10 * 1024]; // large enough
    int spspps_len; // actual bytes used in spspps
};

struct pusher_delay{
    long long cap_time_ms;  //current utc time when capture the video frame
    long long enc_time_ms;  //current utc time when the videoframe send to encoder
};

struct player_delay{
    long long decoder_time_ms;  //current utc time when the video frame send to decoder
    long long render_time_ms;   //current utc time when the video frame send to render
    unsigned long long pts;     //the video frame's pts
};

struct rts_worker_demux_info2
{
    unsigned int uid; // 0: unspecified

    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid
    int audio_channels; // 1
    int audio_sample_rate; // 48000

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
    int video_codec;      // 1 - h264
    int video_width;
    int video_height;
    int video_profile;
    int video_level;

    unsigned char spspps[10 * 1024]; // large enough
    int spspps_len; // actual bytes used in spspps
};

// structure to store subscribed stream info
// use ioctl(..., "get_pub_info", ...) to fetch
// not implemented yet
struct rts_worker_mux_info {
    // audio part
    int audio_flag; // 1 - following audio info is valid; 0 - invalid

    // video part
    int video_flag; // 1 - following video info is valid; 0 - invalid
};

struct rts_frame {
    void *buf;              // where frame data is stored
    int size;               // size of frame data in bytes
    int is_audio;           // 1 for audio frame, 0 for video frame
    unsigned long long pts; // presentation time stamp, in ms
    unsigned long long dts; // decoding time stamp, in ms
    int flag;               // for video frame (is_audio == 0)
                            //     bit 0: key frame;
                            //     bit 1: corruption
                            //     bit 2: sps
                            //     bit 3: sps change
    int duration;           // in ms

    // use this function to free rts_frame object
    void (*free_ptr)(struct rts_frame *);

    unsigned int uid; // reserved. which user this stream attached
    struct pusher_delay delay;
};

#define API_VERSION 2

// ffrtsglue provided function collection
struct rts_glue_funcs {
    int api_version; // validation. must be 2

    /* configure globaly, before open is called
     * do not change values during an instance is running
     */
    void (* preconfig)(const char *key, const char *val);

    /* open a stream specified by url
     * url:   stream url. artc stream supported for now
     * mode:  "r" for subscribe, "w" for publish
     * return value: handle to the stream. NULL if open failed
     */
    void *(* open)(const char *url, const char *mode);

    /* close the stream
     * handle: returned by open
     */
    void (* close)(void *handle);

    /* runtime control (e.g. get/set parameters)
     * negative return value for error
     */
    long long (* ioctl)(void *handle, const char *cmd, void *arg);

    /* read one frame
     * caller need free the returned frame
     * return value: 1 for one frame read into '*frame'; 0 for try
     *               later; -1 for EOF; or other negative value for
     *               fatal error
     */
    int (* read)(struct rts_frame **frame, void *handle);

     /* write one frame. callee free the frame
     * return value: 1 for ok; 0 for try
     *               later; -1 for EOF; or other negative value for
     *               fatal error
      */
    int (* write)(struct rts_frame **frame, void *handle);
};

static const struct rts_glue_funcs *__rts_funcs = NULL;

#if !defined(BUILD_INDEPENDANT_WITH_RTSSDKLIB)
/* @brief Query file operation style functions
 * @param version    Specify compatible api version
 * @return Structure containing function pointers
 * @note Caller need check return value NULL or not
 */
const struct rts_glue_funcs *get_rts_funcs(int version);
#else
DLL_PUBLIC
void av_set_rts_demuxer_funcs(const struct rts_glue_funcs *funcs);

void av_set_rts_demuxer_funcs(const struct rts_glue_funcs *funcs)
{
    __rts_funcs = funcs;
}
#endif

typedef struct tagRtsDecContext {
    AVClass  *av_class;     // first element
    void     *rts_worker;  // instance of cdn worker, created by open()

    // sub config
    int    sub_audio;
    int    sub_video;

    // stream index allocated for video and audio
    int video_stream_index;
    int audio_stream_index;

    // alow blur picture?
    int no_blur_flag;
    int wait_good_key_frame;

#if defined(DEBUG)
    long long stalling_time;
    long long last_video_pts;
    long long first_video_pts;

    long long report_count;
    long long first_report_time;
    long long last_report_time;
#endif
} RtsDecContext;

DLL_PUBLIC
int artc_reload(AVFormatContext *ctx);

DLL_PUBLIC
void artc_set_rts_param(char* key, char* value);

DLL_PUBLIC
long long artc_get_state(AVFormatContext *ctx, int key);

DLL_PUBLIC
int artc_run_cmd(AVFormatContext *ctx, const char *cmd, void *arg);

int artc_reload(AVFormatContext *ctx)
{
    if(ctx == NULL) {
        return AVERROR(EINVAL);
    }
    RtsDecContext *artc = ctx->priv_data;

    if (artc == NULL || artc->rts_worker == NULL) {
        return AVERROR(EINVAL);
    }

    if(__rts_funcs == NULL) {
        return AVERROR(ENXIO);
    }

    return __rts_funcs->ioctl(artc->rts_worker, "reload", NULL);
}

long long artc_get_state(AVFormatContext *ctx, int key)
{
    if(ctx == NULL) {
        return AVERROR(EINVAL);
    }
    RtsDecContext *artc = ctx->priv_data;

    if (artc == NULL || artc->rts_worker == NULL) {
        return AVERROR(EINVAL);
    }

    if(__rts_funcs == NULL) {
        return AVERROR(ENXIO);
    }

    return __rts_funcs->ioctl(artc->rts_worker, "get_state", &key);
}

int artc_run_cmd(AVFormatContext *ctx, const char *cmd, void *arg)
{
    if(ctx == NULL) {
        return AVERROR(EINVAL);
    }
    RtsDecContext *artc = ctx->priv_data;

    if (artc == NULL || artc->rts_worker == NULL) {
        return AVERROR(EINVAL);
    }

    if(__rts_funcs == NULL) {
        return AVERROR(ENXIO);
    }
    return __rts_funcs->ioctl(artc->rts_worker, cmd, arg);
}

void artc_set_rts_param(char* key, char* value)
{
    if (__rts_funcs == NULL) {
        return ;
    }
    __rts_funcs->preconfig(key, value);
}


#define RECV_MEDIA_INFO_TIMEOUT 15000
#define MEDIA_INFO_QUERY_INTERVAL 3

static void set_stream_pts_info(AVStream *s, int pts_wrap_bits,
                         unsigned int pts_num, unsigned int pts_den)
{
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase %d/%d for st:%d\n",
               new_tb.num, new_tb.den,
               s->index);
        return;
    }
    s->time_base     = new_tb;
    /*
     void av_codec_set_pkt_timebase(AVCodecContext *s,AVRational v)
    {
        s -> pkt_timebase = v;
    }
     */
    av_codec_set_pkt_timebase(s->codec, new_tb);
//#if FF_API_LAVF_AVCTX
//    s->codec->pkt_timebase = new_tb;
//#endif
    s->pts_wrap_bits = pts_wrap_bits;
}

static int rtc_init_avstream_info(AVFormatContext *s, struct rts_worker_demux_info *stream_info)
{
    s->flags |= AVFMT_FLAG_GENPTS;
    s->fps_probe_size = 0;

    if(s == NULL) {
        return AVERROR(EINVAL);
    }
    RtsDecContext *artc = s->priv_data;
    artc->video_stream_index = -1;
    artc->audio_stream_index = -1;

    if (stream_info->video_flag) {
        unsigned int video_stream_index = s->nb_streams; // nb_streams: set by avformat_new_stream
        AVStream *vs = avformat_new_stream(s, NULL);

        if (vs == NULL) {
            av_log(s, AV_LOG_ERROR, "Allocate stream failed\n");
            return AVERROR(ENOMEM);
        }

        vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        set_stream_pts_info(vs, 64, 1, 1000); /* 64 bit pts in ms */
        s->streams[video_stream_index] = vs;
        enum AVCodecID codec = stream_info->video_codec == 1 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
        s->streams[video_stream_index]->codecpar->codec_id = s->video_codec_id = codec;
        s->streams[video_stream_index]->need_parsing       = AVSTREAM_PARSE_NONE;
        s->streams[video_stream_index]->codecpar->width    = stream_info->video_width;
        s->streams[video_stream_index]->codecpar->height   = stream_info->video_height;
        s->streams[video_stream_index]->codecpar->profile  = stream_info->video_profile;
        s->streams[video_stream_index]->codecpar->level    = stream_info->video_level;

        // add sps pps into extradata
        if (stream_info->spspps_len > 0) {
            int size = stream_info->spspps_len;
            s->streams[video_stream_index]->codecpar->extradata = av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
            memset(s->streams[video_stream_index]->codecpar->extradata + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            s->streams[video_stream_index]->codecpar->extradata_size = size;

            if (s->streams[video_stream_index]->codecpar->extradata != NULL) {
                memcpy(s->streams[video_stream_index]->codecpar->extradata,
                       stream_info->spspps,
                       stream_info->spspps_len);
                s->streams[video_stream_index]->codecpar->extradata_size = stream_info->spspps_len;
            }
        }

        artc->video_stream_index = (int)video_stream_index;
    }

    // audio part
    if (stream_info->audio_flag) {
        unsigned int audio_stream_index = s->nb_streams; // nb_streams: set by avformat_new_stream
        AVStream *as = avformat_new_stream(s, NULL);

        if (as == NULL) {
            av_log(s, AV_LOG_ERROR, "Allocate stream failed\n");
            return AVERROR(ENOMEM);
        }

        as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        set_stream_pts_info(as, 64, 1, 1000); /* 64 bit pts in ms */
        s->streams[audio_stream_index]                     = as;
        s->streams[audio_stream_index]->need_parsing          = AVSTREAM_PARSE_NONE;
        s->streams[audio_stream_index]->codecpar->codec_id    = s->audio_codec_id = AV_CODEC_ID_PCM_S16LE;
        s->streams[audio_stream_index]->codecpar->channels    = stream_info->audio_channels;
        s->streams[audio_stream_index]->codecpar->sample_rate = stream_info->audio_sample_rate;
        s->streams[audio_stream_index]->codecpar->format      = AV_SAMPLE_FMT_S16;
        artc->audio_stream_index = (int)audio_stream_index;
    }

    if (stream_info->video_flag && stream_info->audio_flag)
        s->probesize = (int64_t)1024 * 1024 * RECV_MEDIA_INFO_TIMEOUT / 1000; // 8 mbps, 15 seconds
    else if (stream_info->video_flag)
        s->probesize = (int64_t)1024 * 1024 * 4 / 1000; // 8 mbps, 4 seconds
    else
        s->probesize = (int64_t)32 * 1024 * 3 / 1000; // 32 kbps, 3 seconds

    s->max_analyze_duration = RECV_MEDIA_INFO_TIMEOUT / 1000 * AV_TIME_BASE; // 15 seconds
//    s->probesize = 16 * 1024;
//    s->max_analyze_duration = 50000;
    //    s->duration = 0;
    s->probesize = 1024 * 2;
    s->max_analyze_duration = 0.2 * AV_TIME_BASE;
    return AVERROR(0);
}

// this function relays messages from artc to app
// app call av_format_set_control_message_cb to set callback
// to receive artc messages
static int format_control_message(struct AVFormatContext *s,
                                  int type,
                                  void *data, // temp data, do not cache it for later use!
                                  long long data_size)
{
    if (s->control_message_cb == NULL)
        return AVERROR(ENOSYS);
    return s->control_message_cb(s, type, data, (size_t)data_size);
}

static int output_log(struct AVFormatContext *s, int level, const char *fmt, va_list args)
{
    int fflevel;

    switch (level) {
        case 0: // LOG_ERROR
            fflevel = AV_LOG_ERROR;
            break;

        case 1: // LOG_WARN
            fflevel = AV_LOG_WARNING;
            break;

        case 2: // LOG_INFO
            fflevel = AV_LOG_INFO;
            break;

        default:
            fflevel = AV_LOG_DEBUG;
            break;
    }

    av_vlog(s, fflevel, fmt, args);
    return 0;
}

static int rtc_read_close(AVFormatContext *s);

static char *addr_to_string(const void *v, char buf[64])
{
    sprintf(buf, "%llu", (unsigned long long)v);
    return buf;
}

//-----------------------------------------------------------------------------------------


static int rtc_probe(const AVProbeData *p)
{
    if (strstr(p->filename, "artc:")) {
        return AVPROBE_SCORE_MAX;
    }

    return 0;
}

static int rtc_read_header(AVFormatContext *s)
{
    if(s == NULL) {
        return AVERROR(EINVAL);
    }
    av_log(s, AV_LOG_INFO, "entering rtc_read_header ... @%lld\n", (long long)av_gettime() / 1000);
    RtsDecContext *artc = s->priv_data;
    int res = AVERROR(EAGAIN);
    char buf[64];
    int host_unreachable = 1;
    int64_t start_time = 0;

#if !defined(BUILD_INDEPENDANT_WITH_RTSSDKLIB)
    // connect with rtssdk library
    if (__rts_funcs == NULL) {
        __rts_funcs = get_rts_funcs(API_VERSION);
    }
#endif

    if (__rts_funcs == NULL) {
        av_log(s, AV_LOG_ERROR, "Please call av_set_rts_demuxer_funcs first!\n");
        res = AVERROR(ENXIO);
        goto out;
    }
    else if (__rts_funcs->api_version != API_VERSION) {
        av_log(s, AV_LOG_ERROR, "Bad api version %d, require %d!\n", __rts_funcs->api_version, API_VERSION);
        res = AVERROR(EPROTO);
        goto out;
    }

    artc->no_blur_flag = 1; // will drop corrupted pictures
    artc->wait_good_key_frame = 1;

#if defined(DEBUG)
    artc->stalling_time = 0;
    artc->last_video_pts = 0;
    artc->first_video_pts = 0;
    artc->report_count = 0;
    artc->first_report_time = 0;
    artc->last_report_time = 0;
#endif

    // start rts, and register callback 'format_control_message' & 's' to artc
    // stop rts first
    if (artc->rts_worker != NULL) {
        __rts_funcs->close(artc->rts_worker);
        artc->rts_worker = NULL;
    }

    // make use of support-id-prefix to setup the map between
    // app domain id and rtssdk domain id.
    // e.g. use UserID as support-id-prefix, it will be convenient to
    // query associated log/profiling data to that UserID
    // __rts_funcs->preconfig("HelpSupportIDPrefix", "yourInstanceID");
    //__rts_funcs->preconfig("SigTimeoutMs", "10000"); // default 10000ms
    __rts_funcs->preconfig("LogCallback", addr_to_string(output_log, buf));
    __rts_funcs->preconfig("LogLevel", "2"); // NONE:100, ERR:0, WARN:1, INFO:2, DEBUG:3 // default:2
    __rts_funcs->preconfig("LogCbParam", addr_to_string(s, buf));
    __rts_funcs->preconfig("MessageCallback", addr_to_string(format_control_message, buf));
    __rts_funcs->preconfig("MessageCbParam", addr_to_string(s, buf));
    __rts_funcs->preconfig("AacdCreateCallback", addr_to_string(aac_decoder_create, buf));
    __rts_funcs->preconfig("AacdDecodeCallback", addr_to_string(aac_decode_frame, buf));
    __rts_funcs->preconfig("AacdCloseCallback", addr_to_string(aac_decode_close, buf));
    __rts_funcs->preconfig("AesCreateCallback", addr_to_string(aes_alloc_context, buf));
    __rts_funcs->preconfig("AesInitCallback", addr_to_string(aes_init, buf));
    __rts_funcs->preconfig("AesDecryptCallback", addr_to_string(aes_decrypt, buf));
    __rts_funcs->preconfig("AesFreeCallback", addr_to_string(aes_free_context, buf));

#ifndef FF_API_FORMAT_FILENAME
    artc->rts_worker = __rts_funcs->open(s->filename, "r");
#else
    artc->rts_worker = __rts_funcs->open(s->url, "r");
#endif
    if(artc->rts_worker == NULL) {
        av_log(s, AV_LOG_ERROR, "Failed to open net sdk stack!\n");
        res = AVERROR(EIO);
        goto out;
    }

    host_unreachable = 1;
    start_time = av_gettime();

    // wait until stream info ready
    while(true) {
        if (ff_check_interrupt(&s->interrupt_callback)) {
            av_log(s, AV_LOG_INFO, "User interrupted\n");
            res = AVERROR_EXIT;
            goto out;
        }

        // get stream info
        struct rts_worker_demux_info stream_info;
        stream_info.spspps_len = 10 * 1024;
        int r = __rts_funcs->ioctl(artc->rts_worker, "get_stream_info", &stream_info);

        if (r == 0) {
            int rc = rtc_init_avstream_info(s, &stream_info);

            if (rc != AVERROR(0)) {

                res = rc;
                goto out;
            } else {
                // everything is ok, return
                res = AVERROR(0);
                goto out;
            }
        } else if (r == - EINVAL) {
            res = AVERROR(EINVAL);
            goto out;
        } else if (r != - ENOTCONN) {
            host_unreachable = 0;
        }

        av_usleep(MEDIA_INFO_QUERY_INTERVAL * 1000);

        if (((av_gettime() - start_time) / 1000) >=  RECV_MEDIA_INFO_TIMEOUT) {
            av_log(s, AV_LOG_ERROR, "Resource timeout time %" PRId64 "start_time %" PRId64 "\n",
                    av_gettime(), start_time);
            if (host_unreachable == 1)
                res = AVERROR(EHOSTUNREACH);
            else
                res = AVERROR(ETIMEDOUT);

            break;
        }
    }

out:

    if (res != AVERROR(0)) {
        rtc_read_close(s);
    }

    av_log(s, AV_LOG_INFO, "leaving rtc_read_header %d @%lld\n", res, (long long)av_gettime() / 1000);
    return res;
}

#if defined(DEBUG)
static void statistics(AVFormatContext *s, int is_audio_frame, int64_t dts)
{
    if(s == NULL) {
        return;
    }
    static int64_t audio_frame_count = 0;
    static int64_t video_frame_count = 0;
    static int64_t last_check_time = 0;
    static int64_t start_time = 0;

    if (is_audio_frame)
        audio_frame_count++;
    else
        video_frame_count++;

    int64_t now = av_gettime() / 1000;

    if (last_check_time == 0) {
        last_check_time = now;
        start_time = now;
    }

    if (now - last_check_time >= 4000) {
        av_log(s, AV_LOG_INFO,
               "DEBUG: audio frames %" PRId64 ", video frames %" PRId64 ", duration %" PRId64 " after %" PRId64 " seconds\n",
               audio_frame_count, video_frame_count, now - last_check_time, (now - start_time) / 1000);
        last_check_time = now;
    }

    // compare last audio dts and video dts
    static int64_t last_audio_dts = 0;
    static int64_t last_video_dts = 0;

    if (is_audio_frame) {
        // dts revert check
        if (dts <= last_audio_dts) {
            av_log(s, AV_LOG_ERROR,
                   "Bad audio dts: last audio dts %" PRId64 ", current audio dts %" PRId64 "\n",
                   last_audio_dts, dts);
        }

        last_audio_dts = dts;
    } else {
        // dts revert check
        if (dts <= last_video_dts) {
            av_log(s, AV_LOG_ERROR,
                   "Bad video dts: last video dts %" PRId64 ", current video dts %" PRId64 "\n",
                   last_video_dts, dts);
        }

        last_video_dts = dts;
        // should not be too late behind, otherwise it will be discarded by renderer
        RtsDecContext *artc = s->priv_data;
        if(artc->video_stream_index != -1 && artc->audio_stream_index != -1) {
            int64_t diff = last_video_dts - last_audio_dts;
            if(diff < -200 || diff > 200) {
                av_log(s, AV_LOG_INFO,
                       "DEBUG: last audio dts %" PRId64 ", last video dts %" PRId64 " @%" PRId64 "\n",
                       last_audio_dts, last_video_dts, now);
            }
        }
    }

//    av_log(s, AV_LOG_WARNING, " >> [%s dts %" PRIu64 " dts %" PRIu64 " dr %" PRIu64 " sz %d @%"PRIu64"]\n",
//           f->is_audio ? "audio" : "video",
//           pkt->dts, pkt->dts, pkt->duration,
//           (int)pkt->size, now);
}

static void statistics_video_stalling(AVFormatContext *s, int64_t pts)
{
    if(s == NULL) {
        return;
    }
    RtsDecContext *artc = s->priv_data;
    const long long stalling_threshold = 200;
    const int report_interval = 2000;
    int64_t now = av_gettime() / 1000;

    // first frame
    if(artc->last_video_pts == 0) {
        artc->first_video_pts = pts;
        artc->last_video_pts = pts;
        artc->stalling_time = 0;
        artc->report_count = 1; // as if one report has been sent
        artc->first_report_time = now;
        artc->last_report_time = now;

        av_log(s, AV_LOG_INFO, "FFMCDN: first video frame pts'=%" PRIu64 " @%"PRIu64"\n",
               pts, now);

        return;
    }

    // stalling detection
    long long diff = pts - artc->last_video_pts;
    if(diff > stalling_threshold) {
        artc->stalling_time += diff;
    }

    if(now - artc->last_report_time >= 2000) {
        artc->report_count += (now - artc->last_report_time) / report_interval;
        artc->last_report_time = now;
    }

    if(now - artc->first_report_time >= artc->report_count * report_interval) {
        artc->report_count++;

        av_log(s, AV_LOG_INFO, "Video stalling %lld / %lld ms\n",
               artc->stalling_time, now - artc->last_report_time);

        artc->stalling_time = 0;
        artc->last_report_time = now;
    }

    artc->last_video_pts = pts;
}

#endif

static int find_pps_pos(unsigned char* data, int length, bool hevc) {

    int index = 0;
    char startCode[4];
    startCode[0] = 0;
    startCode[1] = 0;
    startCode[2] = 0;
    startCode[3] = 1;
    int type = 0;
    bool findPps = false;
    while(index < length - 4) {
        if(memcmp(startCode, data + index, 4) == 0) {
            if(findPps)
                return index;
            index+=4;
            if(hevc) {
                type = (data[index] >> 1) & 0x3f;
            } else {
                type = (data[index] & 0x1F);
            }
            if((hevc && type == 34) || (!hevc && type == 8)) {
                findPps = true;
            }
        }

        index++;
    }
    return -1;
}

static int rtc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    if(s == NULL) {
        return AVERROR(EINVAL);
    }
    RtsDecContext *artc = s->priv_data;

    if (ff_check_interrupt(&s->interrupt_callback)) {
        av_log(s, AV_LOG_INFO, "User interrupted\n");
        return AVERROR_EXIT;
    }

    // get one frame and check error
    struct rts_frame *f = NULL;
    int r = __rts_funcs->read(&f, artc->rts_worker);

    if (r < 0) {
        av_log(s, AV_LOG_ERROR, "Fatal error received and stream stopped (err=%d)\n",
               r);
        assert(f == NULL);
        return AVERROR(EIO);
    }

    if (f == NULL) {
        return AVERROR(EAGAIN);
    }

#if defined(DEBUG)
    statistics(s, f->is_audio, f->dts);
#endif

    if (f->is_audio && artc->audio_stream_index >= 0) {
        pkt->stream_index  = artc->audio_stream_index;
        pkt->dts = f->dts;
        pkt->pts = f->pts;
        pkt->duration = f->duration;
    } else if (! f->is_audio && artc->video_stream_index >= 0) {
        if ((f->flag & 1) == 1) {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }

        if ((f->flag & 2) == 2) {
            pkt->flags |= AV_PKT_FLAG_CORRUPT;

            // if no blur flag set, skip until next good key frame
            if (artc->no_blur_flag) {
                artc->wait_good_key_frame = 1;
            }
        }

        if ((f->flag & 1) == 1 && (f->flag & 2) != 2) {
            // good frame, and key frame, reset wait_good_key_frame
            artc->wait_good_key_frame = 0;
        }

        if(f->flag & 0x08) { //sps change
            int size = find_pps_pos(f->buf, f->size, (s->video_codec_id == AV_CODEC_ID_HEVC));
            if(size > 0) {
                uint8_t* side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, size);
                if(side_data){
                    memcpy(side_data, f->buf, size);
                }
                else
                    printf("Failed to create side data size %d\n", size);
            } else {
                printf("Failed to find pps pos\n");
            }
        }

        if (artc->wait_good_key_frame) {
            pkt->flags = 0; // restore?
            f->free_ptr(f);
            return AVERROR(EAGAIN);
        }


        pkt->stream_index  = artc->video_stream_index;
        pkt->dts = f->dts;
        pkt->pts = f->pts;
        pkt->duration = f->duration;

#if defined(DEBUG)
        statistics_video_stalling(s, f->pts);
#endif
    }
    else {
        // unexpected packet, discard
        f->free_ptr(f);
        return AVERROR(EAGAIN);
    }

    // alloc buffer to hold frame data
    AVBufferRef *media_buf = av_buffer_alloc(f->size + AV_INPUT_BUFFER_PADDING_SIZE);

    if (media_buf == NULL) {
        av_log(s, AV_LOG_ERROR, "Not enough memory while allocating %d bytes\n",
               (int)(f->size + AV_INPUT_BUFFER_PADDING_SIZE));
        f->free_ptr(f);
        return AVERROR(ENOMEM);
    }

    // set pkt
    uint8_t *media_data = media_buf->data;
    memcpy(media_data, f->buf, f->size);
    pkt->buf  = media_buf;
    pkt->data = media_data;
    pkt->size = f->size;
    f->free_ptr(f);
    return AVERROR(0);
}

static int rtc_read_close(AVFormatContext *s)
{
    if(s == NULL) {
        return AVERROR(EINVAL);
    }
    av_log(s, AV_LOG_INFO, "entering rtc_read_close ...\n");
    RtsDecContext *artc = s->priv_data;

    // stop rts
    if (artc->rts_worker != NULL) {
        __rts_funcs->close(artc->rts_worker);
        artc->rts_worker = NULL;
    }

    memset(artc, 0, sizeof(RtsDecContext));
    av_log(s, AV_LOG_INFO, "leaving rtc_read_close ...\n");
    return 0;
}


static int rtc_read_seek(AVFormatContext *s,
                         int stream_index,
                         int64_t timestamp,
                         int flags)
{
    // not supported
    (void) s;
    (void) stream_index;
    (void) timestamp;
    (void) flags;
    return -1;
}

static int rtc_read_play(AVFormatContext *s)
{
    // not supported by now
    (void) s;
    return 0;
}

static int rtc_read_pause(AVFormatContext *s)
{
    // not supported by now
    (void) s;
    return 0;
}

static const AVOption ff_rtc_options[] = {
    { NULL },
};

static const AVClass rtc_demuxer_class = {
    .class_name     = "rts demuxer",
    .item_name      = av_default_item_name,
    .option         = ff_rtc_options,
    .version        = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_rtc_demuxer = {
    .name               = "rtc", // rtc stream
    .long_name          = "rtc stream input",
    .priv_data_size     = sizeof(RtsDecContext),
    .read_probe         = rtc_probe,
    .read_header        = rtc_read_header,
    .read_packet        = rtc_read_packet,
    .read_close         = rtc_read_close,
    .read_seek          = rtc_read_seek,
    .flags              = AVFMT_NOFILE,
    .read_play          = rtc_read_play,
    .read_pause         = rtc_read_pause,
    .priv_class         = &rtc_demuxer_class,
    .extensions         = "rtc",
};

#if defined(BUILD_AS_PLUGIN)
void init_rtsdec_plugin()
{
    av_log(NULL, AV_LOG_INFO, "init rtsdec plugin\n");
    av_register_input_format(&ff_rtc_demuxer);
}
#endif

typedef struct AACDFFmpeg {
    AVCodecContext *pCodecCtx;
    AVFrame *pFrame;
    struct SwrContext *au_convert_ctx;
    int out_buffer_size;
    int out_sample_rate;
    int out_channels;
} AACDFFmpeg;

static void *aac_decoder_create(int sample_rate, int channels)
{
    av_register_all();
    AACDFFmpeg *pComponent = (AACDFFmpeg *)malloc(sizeof(AACDFFmpeg));
    AVCodec *pCodec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (pCodec == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Failed to find aac decoder\n");
        return NULL;
    }

    pComponent->pCodecCtx = avcodec_alloc_context3(pCodec);
    pComponent->pCodecCtx->channels = channels;
    pComponent->pCodecCtx->sample_rate = sample_rate;
    if(avcodec_open2(pComponent->pCodecCtx, pCodec, NULL) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Failed to open aac decoder\n");
        return NULL;
    }

    pComponent->pFrame = av_frame_alloc();

    pComponent->au_convert_ctx = NULL;
    pComponent->out_sample_rate = sample_rate <= 24000 ? sample_rate * 2 : sample_rate; // must be HE AAC
    pComponent->out_channels = channels;

    return (void *)pComponent;
}

static int aac_decode_frame(void *pParam,
                     unsigned char *pData,
                     int nLen,
                     unsigned char *pPCM,
                     unsigned int *outLen)
{
    if (nLen <= 0 || pData == NULL || pPCM == NULL || outLen == NULL)
        return -1;

    *outLen = 0;

    AACDFFmpeg *pAACD = (AACDFFmpeg *)pParam;
    AVPacket packet;
    av_init_packet(&packet);
    packet.size = nLen;
    packet.data = pData;
    int r = avcodec_send_packet(pAACD->pCodecCtx, &packet);
    av_packet_unref(&packet);

    if (r == 0) {
        int sum = 0;
        do {
            r = avcodec_receive_frame(pAACD->pCodecCtx, pAACD->pFrame);
            if (r == EAGAIN)
                break;

            if (r == AVERROR_EOF)
                break;

            if (r < 0)
                break;

            if(pAACD->au_convert_ctx == NULL) {
                av_log(NULL, AV_LOG_INFO, "AAC: decoded sample rate: %d channels: %d."
                                          " required output sample rate: %d channels %d\n",
                       pAACD->pFrame->sample_rate, pAACD->pFrame->channels,
                       pAACD->out_sample_rate, pAACD->out_channels);
                pAACD->au_convert_ctx = swr_alloc();
                pAACD->au_convert_ctx = swr_alloc_set_opts(
                            pAACD->au_convert_ctx,
                            pAACD->out_channels < 2 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO,
                            AV_SAMPLE_FMT_S16,
                            pAACD->out_sample_rate,
                            pAACD->pFrame->channels < 2 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO,
                            AV_SAMPLE_FMT_FLTP,
                            pAACD->pFrame->sample_rate,
                            0,
                            NULL);
                swr_init(pAACD->au_convert_ctx);
            }

            uint8_t *dst = pPCM + sum;

            int samples_per_channel = swr_convert(
                        pAACD->au_convert_ctx,
                        &dst,
                        1024*4,
                        (const uint8_t **)&pAACD->pFrame->data,
                        pAACD->pFrame->nb_samples);
            if(samples_per_channel >= 0)
                sum += samples_per_channel * pAACD->out_channels * sizeof(short);

            // caller buffer may overflow
            break;
        } while(1);

        *outLen = sum;
    }

    return *outLen > 0 ? 0 : -1;
}

static void aac_decode_close(void *pParam)
{
    AACDFFmpeg *pComponent = (AACDFFmpeg *)pParam;
    if (pComponent == NULL) {
        return;
    }

    swr_free(&pComponent->au_convert_ctx);
    pComponent->au_convert_ctx = NULL;

    if (pComponent->pFrame != NULL) {
        av_frame_free(&pComponent->pFrame);
        pComponent->pFrame = NULL;
    }

    if (pComponent->pCodecCtx != NULL) {
        avcodec_close(pComponent->pCodecCtx);
        avcodec_free_context(&pComponent->pCodecCtx);
        pComponent->pCodecCtx = NULL;
    }

    free(pComponent);
}

//aes decrypt
static void* aes_alloc_context(void)
{
    return (void*)av_aes_alloc();
}

static int aes_init(void* context, const uint8_t* key, int size)
{
    if(context == NULL) {
        return -1;
    }
    return av_aes_init((struct AVAES *)context, key, size * 8, 1);
}

static int aes_decrypt(void* context, uint8_t* dst, const uint8_t* src, int size, uint8_t* iv)
{
    if(dst == NULL || src == NULL || size <= 0 || iv == NULL || context == NULL) {
        return -1;
    }
    if(size % 16 == 0)
        av_aes_crypt((struct AVAES *)context, dst, src, size / 16, iv, 1);
    else {
        int alignSize = (size + 15) / 16 * 16;
        uint8_t *alignSrc = (uint8_t *)malloc(alignSize * sizeof(uint8_t));
        memcpy(alignSrc, src, size);
        av_aes_crypt((struct AVAES *)context, alignSrc, alignSrc, alignSize / 16 + 1, iv, 1);
        memcpy(dst, alignSrc, size);
        free(alignSrc);
    }
    return 0;
}

static void aes_free_context(void* context)
{

    if(context != NULL)
        free(context);
}

