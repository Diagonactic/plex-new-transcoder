#pragma once;

//set to 1 to enable the ideling code
#define USE_SEGMENT_SOCKET 1

typedef struct PLEXClientVideoCaps {
    int profile;
    int aacProfile;
    unsigned int resolution;
    unsigned char level;
    unsigned char allowAudioCopy;
    unsigned char allowVideoCopy;
    
    //Supported audio codecs
    //-1=not supported, 0=no bitrate limit, >0 maximum bitrate for codec
    int mp3MaxBitrate;
    int aacMaxBitrate;
    int dtsMaxBitrate;
    int ac3MaxBitrate;
    int pcmMaxBitrate;
    
    int maxSegmentSizePerSecond;
    int playsAnamorphic;
    
    char* audioCodec;
} PLEXClientVideoCaps;


typedef struct PLEXContext {
    char autodetect_cpu_quality;
    PLEXClientVideoCaps videoCaps;
    int useFpsFilter;
		char* progressURL;
		int delay;
    
    //use this if you want to loop back to the original ffmpeg main method (for example if you widh to transcode to one single file)
    int transcodeToFileArgc;
    char** transcodeToFileArgv;
} PLEXContext;


//ffmpeg trancoder parameters
enum
{
    PROCESS_NAME,
    OPERATION_NAME,
    INPUT_PATH,
    OUTPUT_FILE_PATH_PREFIX,
    VARIANT_NAME,
    SEGMENT_DURATION,
    INITIAL_SEGMENT_INDEX,
    AUDIO_STREAM_SPECIFIER,
    SUBTITLE_STREAM_SPECIFIER,
    AUDIO_GAIN,
    ALBUM_ART,
    SRT_ENCODING,
    SRT_DIRECTION,
    USER_AGENT,
    COOKIES,
    SUBTITLE_SCALE,
    TRANSCODE_OPTIONS,
    PLEX_VIDEO_PROFILE,
    PLEX_VIDEO_RESOLUTION,
    PLEX_VIDEO_LEVEL,
    PLEX_AUDIO_COPY_BITRATE,
    PLEX_SEGSIZE_PER_SECOND,
    PLEX_PLAYS_ANAMORPHIC,
    PLEX_AUTO_CPU_QUALITY,
    PLEX_FPS_FILTER,
		PLEX_PROGRESS_URL,
    PHOTO_DURATION,
    NUM_ARGUMENTS
};

#define GOP_LENGTH 10.0
#define TRANSITION_REPEAT 0.8
#define REPEAT_COMPENSATION_RANGE 40.0

/* needed for usleep() */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#include <fontconfig/fontconfig.h>

#include "config.h"
#include <ctype.h>

#undef __STRICT_ANSI__
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavcodec/opt.h"
#include "libavcodec/audioconvert.h"
#include "libavutil/colorspace.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavformat/os_support.h"

#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/avfiltergraph.h"
# include "libavfilter/vsrc_buffer.h"
#endif

#if HAVE_SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#ifndef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_CONIO_H
#include <conio.h>
#endif

#include "cmdutils.h"

#include "libavutil/avassert.h"

#include "libavfilter/vf_inlineass.h"
#include "libavcodec/mpegvideo.h"
extern const char STMPathSeparator;
extern const char *STMConfFilename;
#ifdef WINDOWS
#include <dirent.h>
#else
#include <sys/dir.h>
#endif

#undef time //needed because HAVE_AV_CONFIG_H is defined on top
#include <time.h>
#include <pthread.h>
#undef NDEBUG
#include <assert.h>
#if defined(WINDOWS) || defined(__linux__) || defined(__FreeBSD__)
#if defined (__linux__) || defined(__FreeBSD__)
#define _GNU_SOURCE
#include <unistd.h> 
#include <stdio.h>
#else
#include <asprintf.h>
#endif
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#undef exit

extern char *executable_path;
extern char *subtitle_path;



static int64_t timer_start;

/* select an input stream for an output stream */
typedef struct AVStreamMap {
    int file_index;
    int stream_index;
    int sync_file_index;
    int sync_stream_index;
} AVStreamMap;

/**
 * select an input file for an output file
 */
typedef struct AVMetaDataMap {
    int  file;      //< file index
    char type;      //< type of metadata to copy -- (g)lobal, (s)tream, (c)hapter or (p)rogram
    int  index;     //< stream/chapter/program number
} AVMetaDataMap;

typedef struct AVChapterMap {
    int in_file;
    int out_file;
} AVChapterMap;

typedef enum
{
    QUALITY_VERY_LOW,
    QUALITY_LOW,
    QUALITY_MID_LOW,
    QUALITY_MID,
    QUALITY_MID_HIGH,
    QUALITY_HIGH,
    QUALITY_VERY_HIGH
} STMStreamQuality;

typedef struct STMTranscodeSettings {
    
    int64_t sts_input_start_time;
    char *sts_input_file_name;
    
    AVRational sts_source_video_frame_rate;
    float sts_source_video_aspect_ratio;
    float sts_source_audio_channel_layout;
    
    char *sts_output_file_format;
    const char *sts_output_file_base;
    char *sts_output_file_path;
    float sts_output_dts_delta_threshold;
    int64_t sts_output_duration;
    double sts_output_segment_length;
    int64_t sts_output_segment_index;
    int64_t sts_output_initial_segment;
    int sts_output_single_frame_only;
    int64_t sts_last_output_pts;
    int64_t sts_last_mux_pts;
    int sts_output_padding;
    int sts_output_muxrate;
    
    int sts_suppress_video;
    int sts_album_art_mode;
    int sts_still_image_mode;
    
    int sts_video_copy;
    char *sts_video_codec_name;
    AVRational sts_video_frame_rate;
    int sts_video_frame_width;
    int sts_video_frame_height;
    int sts_source_frame_width;
    int sts_source_frame_height;
    int sts_source_video_interlaced;
    
    STMStreamQuality sts_output_quality;
    int sts_max_bitrate;
    
    int sts_video_frame_refs;
    int sts_video_crf;
    int sts_video_level;
    char *sts_video_profile;
    int sts_video_me_range;
    char *sts_video_me_method;
    int sts_video_sc_threshold;
    int sts_video_qdiff;
    int sts_video_qmin;
    int sts_video_qmax;
    float sts_video_qcomp;
    int sts_video_g;
    float sts_video_subq;
    long long int sts_video_bufsize;
    long long int sts_video_minrate;
    long long int sts_video_maxrate;
    char *sts_video_flags;
    char *sts_video_flags2;
    char *sts_video_cmp;
    char *sts_video_partitions;
    AVBitStreamFilterContext *sts_video_bitstream_filter;
    
    char *sts_vfilters;
    
    int sts_audio_copy;
    char *sts_audio_codec_name;
    int sts_audio_sample_rate;
    int sts_audio_channels;
    float sts_audio_drift_threshold;
    int sts_audio_output_volume;
    int sts_audio_bitrate;
    char *sts_audio_stream_specifier;
    int sts_audio_profile;
    
    char *sts_subtitle_stream_specifier;
    int sts_subtitle_stream_index;

    int sts_force_i_frame;
} STMTranscodeSettings;

struct AVInputStream;

typedef struct AVOutputStream {
    int file_index;          /* file index */
    int index;               /* stream index in the output file */
    int source_index;        /* AVInputStream index */
    AVStream *st;            /* stream in the output file */
    int encoding_needed;     /* true if encoding needed for this stream */
    int frame_number;
    /* input pts and corresponding output pts
       for A/V sync */
    //double sync_ipts;        /* dts from the AVPacket of the demuxer in second units */
    struct AVInputStream *sync_ist; /* input stream to sync against */
    int64_t sync_opts;       /* output frame counter, could be changed to some true timestamp */ //FIXME look at frame_number
    AVBitStreamFilterContext *bitstream_filters;
    /* video only */
    int video_resample;
    AVFrame pict_tmp;      /* temporary image for resampling */
    struct SwsContext *img_resample_ctx; /* for image resampling */
    int resample_height;
    int resample_width;
    int resample_pix_fmt;

    /* full frame size of first frame */
    int original_height;
    int original_width;

    /* forced key frames */
    int64_t *forced_kf_pts;
    int forced_kf_count;
    int forced_kf_index;

    /* audio only */
    int audio_resample;
    ReSampleContext *resample; /* for audio resampling */
    int resample_sample_fmt;
    int resample_channels;
    int resample_sample_rate;
    int reformat_pair;
    AVAudioConvert *reformat_ctx;
    AVFifoBuffer *fifo;     /* for compression: one audio fifo per codec */
    FILE *logfile;

    char *avfilter;
    AVFilterGraph *graph;
} AVOutputStream;

typedef struct AVInputStream {
    int file_index;
    int index;
    AVStream *st;
    int discard;             /* true if stream data should be discarded */
    int decoding_needed;     /* true if the packets must be decoded in 'raw_fifo' */
    int64_t sample_index;      /* current sample */

    int64_t       start;     /* time when read started */
    int64_t       next_pts;  /* synthetic pts for cases where pkt.pts
                                is not defined */
    int64_t       pts;       /* current pts */
    PtsCorrectionContext pts_ctx;
    int is_start;            /* is 1 at the start and after a discontinuity */
    int showed_multi_packet_warning;
    int is_past_recording_time;
#if CONFIG_AVFILTER
    AVFilterContext *output_video_filter;
    AVFilterContext *input_video_filter;
    AVFilterBufferRef *picref;
    AVFrame *filter_frame;
    int has_filter_frame;
#endif
} AVInputStream;

typedef struct AVInputFile {
    int eof_reached;      /* true if eof reached */
    int ist_index;        /* index of first stream in ist_table */
    int buffer_size;      /* current total buffer size */
    int nb_streams;       /* nb streams we are aware of */
} AVInputFile;

typedef struct STMTranscodeContext {
    STMTranscodeSettings stc_settings;
    
    AVFormatContext **stc_input_files;
    int stc_nb_input_files;
    int stc_input_files_capacity;
    int64_t *stc_input_files_ts_offset;
    
    AVFormatContext **stc_output_files;
    int stc_nb_output_files;
    int stc_output_files_capacity;
    AVOutputStream ***stc_output_streams_for_file;
    int *stc_nb_output_streams_for_file;
    
    AVCodec **stc_input_codecs;
    int stc_nb_input_codecs;
    int stc_input_codecs_capacity;
    
    AVCodec **stc_output_codecs;
    int stc_nb_output_codecs;
    int stc_output_codecs_capacity;
    
    AVStreamMap *stc_stream_maps;
    int stc_nb_stream_maps;
    int stc_stream_maps_capacity;
    
    AVChapterMap *stc_chapter_maps;
    int stc_nb_chapter_maps;
    int stc_chapter_maps_capacity;
    
    AVMetaDataMap (*stc_meta_data_maps)[2];
    int stc_nb_meta_data_maps;
    int stc_meta_data_maps_capacity;
    
    AVMetadataTag *stc_metadata;
    int stc_metadata_count;
    
    AVFilterGraph *stc_filt_graph_all;
    AVFilterContext *stc_inlineass_context;
    
    short *stc_audio_samples;
    uint8_t *stc_audio_buffer;
    uint8_t *stc_audio_output;
    unsigned int stc_allocated_audio_output_size;
    unsigned int stc_allocated_audio_buffer_size;
    
    //Plex
    AVFilterContext *inlinepgs_context;
    PLEXContext plex;
} STMTranscodeContext;

extern STMTranscodeContext globalTranscodeContext;


#define graph globalTranscodeContext.stc_filt_graph_all
#define vfilters globalTranscodeContext.stc_settings.sts_vfilters
#define zero_start_time 0
#define audio_buf globalTranscodeContext.stc_audio_buffer
#define audio_out globalTranscodeContext.stc_audio_output
#define allocated_audio_out_size globalTranscodeContext.stc_allocated_audio_output_size
#define allocated_audio_buf_size globalTranscodeContext.stc_allocated_audio_buffer_size
#define samples globalTranscodeContext.stc_audio_samples
#define audio_volume globalTranscodeContext.stc_settings.sts_audio_output_volume
#define input_files_ts_offset globalTranscodeContext.stc_input_files_ts_offset
#define output_streams_for_file globalTranscodeContext.stc_output_streams_for_file
#define nb_output_codecs globalTranscodeContext.stc_nb_output_codecs
#define output_codecs globalTranscodeContext.stc_output_codecs
#define nb_input_codecs globalTranscodeContext.stc_nb_input_codecs
#define input_codecs globalTranscodeContext.stc_input_codecs
#define meta_data_maps globalTranscodeContext.stc_meta_data_maps
#define nb_meta_data_maps globalTranscodeContext.stc_nb_meta_data_maps
#define chapter_maps globalTranscodeContext.stc_chapter_maps
#define nb_chapter_maps globalTranscodeContext.stc_nb_chapter_maps
#define dts_delta_threshold globalTranscodeContext.stc_settings.sts_output_dts_delta_threshold
#define recording_time INT64_MAX
#define metadata_chapters_autocopy 1
typedef struct {
    int pix_fmt;
} FilterOutPriv;


extern char* HTTP_COOKIES;

extern int plex_video_sync_method;
extern int plex_audio_sync_method;
extern float plex_audio_drift_threshold;
static int top_field_first = -1;
static int rate_emu = 0;
static int exit_on_error = 0;
static int downmix_to_aac = 0;
