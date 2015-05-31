/*
 *  ffmpeg-servetome.c
 *  ffmpeg_servetome
 *
 *  Created by Matt Gallagher on 2010/01/13.
 *  Copyright 2010 Matt Gallagher. All rights reserved.
 *
 */
#ifdef __linux__
#define _GNU_SOURCE
#include <stdio.h>
#endif
 
#ifdef __FreeBSD__
#undef _ISOC99_SOURCE
#undef _POSIX_C_SOURCE
#include <unistd.h>
#endif

//#define _GNU_SOURCE
#include <stdio.h>

#include <limits.h>
#include "ffmpeg.h"
#include "ffmpeg_plex.h"
#include "libavfilter/vf_inlinepgs.h"
#include "stringconversions.h"
#include "libavfilter/vsink_buffer.h"
#include "libavfilter/avcodec.h"
#ifndef _WIN32
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

//define some functions we will need (stupid windows does not like implicit functions)
void set_common_formats(AVFilterContext *ctx, AVFilterFormats *fmts,
	enum AVMediaType type, int offin, int offout);

int plex_video_sync_method= -1;
int plex_audio_sync_method= -1;
float plex_audio_drift_threshold= 0.1;

/* Global logging */
static int verbose = 0;

static int64_t file_size = 0;
static int64_t video_size = 0;
static int64_t audio_size = 0;
static int64_t extra_size = 0;
static int nb_frames_dup = 0;
static int nb_frames_drop = 0;

static double  plex_totalSegmentTime = 0;
static int plex_totalSegmentCount = 0;
const int PLEX_MIN_NUM_ARGUMENTS = USER_AGENT+1;
static clock_t tnow = 0;

#ifdef WINDOWS
const char STMPathSeparator = '\\';
const char *STMConfFilename = "fonts-windows.conf";

#else
const char STMPathSeparator = '/';
const char *STMConfFilename = "fonts.conf";
#endif

STMTranscodeContext globalTranscodeContext = {0};
static STMTranscodeContext *STMGlobalTranscodeContext()
{
    return &globalTranscodeContext;
}

#if CONFIG_AVFILTER

static int configure_filters(AVFormatContext* is, AVInputStream *ist, AVOutputStream *ost)
{
    AVFilterContext *last_filter, *filter;
    /** filter graph containing all filters including input & output */
    AVCodecContext *codec = ost->st->codec;
    AVCodecContext *icodec = ist->st->codec;
    enum PixelFormat pix_fmts[] = { codec->pix_fmt, PIX_FMT_NONE };
    AVRational sample_aspect_ratio;
    char args[255];
    int ret;
    
    graph = avfilter_graph_alloc();
    
    if (ist->st->sample_aspect_ratio.num){
        sample_aspect_ratio = ist->st->sample_aspect_ratio;
    }else
        sample_aspect_ratio = ist->st->codec->sample_aspect_ratio;
    
    snprintf(args, 255, "%d:%d:%d:%d:%d:%d:%d", ist->st->codec->width,
             ist->st->codec->height, ist->st->codec->pix_fmt, 1, AV_TIME_BASE,
             sample_aspect_ratio.num, sample_aspect_ratio.den);
    
    ret = avfilter_graph_create_filter(&ist->input_video_filter, avfilter_get_by_name("buffer"),
                                       "src", args, NULL, graph);
    if (ret < 0)
        return ret;
    ret = avfilter_graph_create_filter(&ist->output_video_filter, avfilter_get_by_name("buffersink"),
                                       "out", NULL, pix_fmts, graph);
    if (ret < 0)
        return ret;
    last_filter = ist->input_video_filter;
    
    // PLEX: Compute the real size we want.
    double stream_width;
    stream_width = icodec->width;
    double stream_height;
    stream_height = icodec->height;
    if (icodec->sample_aspect_ratio.num > 0 && icodec->sample_aspect_ratio.den > 0)
        stream_width *= icodec->sample_aspect_ratio.num / (double)icodec->sample_aspect_ratio.den;
    if (codec->width / codec->height > stream_width / stream_height)
    {
        codec->width = codec->height * stream_width / stream_height;
        codec->width += codec->width % 2;
    } 
    else 
    {
        codec->height = codec->width * stream_height / stream_width;
        codec->height += codec->height % 2;
    }
    // PLEX: Compute the real size we want.
    
    
    
    if (globalTranscodeContext.stc_settings.sts_subtitle_stream_index>=0){
        AVStream *stream = globalTranscodeContext.stc_input_files[0]->streams[globalTranscodeContext.stc_settings.sts_subtitle_stream_index];
        if (stream->codec->codec_id == CODEC_ID_HDMV_PGS_SUBTITLE){
            AVFilterContext *pgs_overlay;
            AVFilterContext *pgs_src;
            char args[255];
            AVRational sample_aspect_ratio;
            
            if (ist->st->sample_aspect_ratio.num){
                sample_aspect_ratio = ist->st->sample_aspect_ratio;
            }else
                sample_aspect_ratio = ist->st->codec->sample_aspect_ratio;
            
            /*if (avfilter_open(&pgs_src, avfilter_get_by_name("buffer"), NULL))
             return -1;
             snprintf(args, 255, "%d:%d:%d:%d:%d:%d:%d", stream->codec->width, stream->codec->height, stream->codec->pix_fmt, stream->codec->time_base.num, stream->codec->time_base.den, sample_aspect_ratio.num, sample_aspect_ratio.den);
             if(avfilter_init_filter(pgs_src, args, NULL))
             return -1;*/
            pgs_src = last_filter;
            stream->discard = 0;
            
            if (avfilter_open(&pgs_overlay, avfilter_get_by_name("inlinepgs"), NULL))
                return -1;
            snprintf(args, 255, "");
            if (avfilter_init_filter(pgs_overlay, args, NULL))
                return -1;
            
            
            
            
            if (avfilter_link(last_filter, 0, pgs_overlay, 0))
                return -1;
            AVFormatContext* is = NULL;
            /*for (int=0; i<globalTranscodeContext.stc_nb_input_files; i++){
             AVFormatContext* s = globalTranscodeContext.stc_input_files[i];
             if (s->codec->codec_type == AVMEDIA_TYPE_SUBTITLE){
             is = s;
             }
             }*/
            
            vf_inlinepgs_set_subtitle_stream(is, pgs_overlay, stream);
            InlinePGSContext *pgsctx;
            pgsctx = pgs_overlay->priv;
            
            
            last_filter = pgs_overlay;
            avfilter_graph_add_filter(graph, last_filter);
        }
    }
    
    if (codec->width  != icodec->width || codec->height != icodec->height) {
        snprintf(args, 255, "%d:%d:flags=0x%X",
                 codec->width,
                 codec->height,
                 globalTranscodeContext.stc_settings.sts_output_quality > QUALITY_MID_HIGH ? SWS_LANCZOS : SWS_FAST_BILINEAR);
        if ((ret = avfilter_open(&filter, avfilter_get_by_name("scale"), NULL)) < 0)
            return ret;
        if ((ret = avfilter_init_filter(filter, args, NULL)) < 0)
            return ret;
        if ((ret = avfilter_link(last_filter, 0, filter, 0)) < 0)
            return ret;
        last_filter = filter;
        avfilter_graph_add_filter(graph, last_filter);
    }
    
    if (globalTranscodeContext.stc_settings.sts_vfilters) {
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs  = avfilter_inout_alloc();
        
        outputs->name    = av_strdup("in");
        outputs->filter_ctx = last_filter;
        outputs->pad_idx = 0;
        outputs->next    = NULL;
        
        inputs->name    = av_strdup("out");
        inputs->filter_ctx = ist->output_video_filter;
        inputs->pad_idx = 0;
        inputs->next    = NULL;
        
        if ((ret = avfilter_graph_parse(graph, globalTranscodeContext.stc_settings.sts_vfilters, &inputs, &outputs, NULL)) < 0)
            return ret;
        av_freep(&globalTranscodeContext.stc_settings.sts_vfilters);
    } else {
        if ((ret = avfilter_link(last_filter, 0, ist->output_video_filter, 0)) < 0)
            return ret;
    }
    
    
    if ((ret = avfilter_graph_config(graph, NULL)) < 0)
        return ret;
    
    for (int i = 0; i < globalTranscodeContext.stc_filt_graph_all->filter_count; i++)
    {
        if (strcmp(globalTranscodeContext.stc_filt_graph_all->filters[i]->filter->name, "inlineass") == 0)
        {
            globalTranscodeContext.stc_inlineass_context = globalTranscodeContext.stc_filt_graph_all->filters[i];
            vf_inlineass_set_aspect_ratio(globalTranscodeContext.stc_inlineass_context,
                                          av_q2d(ost->st->codec->sample_aspect_ratio));
        } 
        /*       //InlinePGS is set up by code, not by hack
         else if (strcmp(globalTranscodeContext.stc_filt_graph_all->filters[i]->filter->name, "inlinepgs") == 0 )
         {
         globalTranscodeContext.inlinepgs_context = globalTranscodeContext.stc_filt_graph_all->filters[i];
         
         AVStream *stream = NULL;
         if (globalTranscodeContext.stc_settings.sts_subtitle_stream_index>=0){
         stream = globalTranscodeContext.stc_input_files[0]->streams[globalTranscodeContext.stc_settings.sts_subtitle_stream_index];
         }
         vf_inlinepgs_set_subtitle_stream(is, globalTranscodeContext.inlinepgs_context, stream);
         }*/
    }
    
    codec->width  = ist->output_video_filter->inputs[0]->w;
    codec->height = ist->output_video_filter->inputs[0]->h;
    
    return 0;
}
#endif /* CONFIG_AVFILTER */



static int output_init(AVFilterContext *ctx, const char *args, void *opaque)
{
    FilterOutPriv *priv = ctx->priv;
    
    if(!opaque) return -1;
    
    priv->pix_fmt = *((int *)opaque);
    
    return 0;
}

static void output_end_frame(AVFilterLink *link)
{
}

static int output_query_formats(AVFilterContext *ctx)
{
    FilterOutPriv *priv = ctx->priv;
    enum PixelFormat pix_fmts[] = { priv->pix_fmt, PIX_FMT_NONE };
    
    set_common_formats(ctx, avfilter_make_format_list(pix_fmts), AVMEDIA_TYPE_AUDIO, offsetof(AVFilterLink, in_formats), offsetof(AVFilterLink, out_formats));
    set_common_formats(ctx, avfilter_make_format_list(pix_fmts), AVMEDIA_TYPE_SUBTITLE, offsetof(AVFilterLink, in_formats), offsetof(AVFilterLink, out_formats));
    set_common_formats(ctx, avfilter_make_format_list(pix_fmts), AVMEDIA_TYPE_VIDEO, offsetof(AVFilterLink, in_formats), offsetof(AVFilterLink, out_formats));
    
    return 0;
}

static int get_filtered_video_pic(AVFilterContext *ctx,
                                  AVFilterBufferRef **picref, AVFrame *pic2,
                                  uint64_t *pts)
{
    AVFilterBufferRef *pic;
    
    if(avfilter_request_frame(ctx->inputs[0]))
        return -1;
    if(!(pic = ctx->inputs[0]->cur_buf))
        return -1;
    *picref = pic;
    ctx->inputs[0]->cur_buf = NULL;
    
    *pts          = pic->pts;
    
    memcpy(pic2->data,     pic->data,     sizeof(pic->data));
    memcpy(pic2->linesize, pic->linesize, sizeof(pic->linesize));
    
    return 1;
}

static AVFilter output_filter =
{
    .name      = "ffmpeg_output",
    
    .priv_size = sizeof(FilterOutPriv),
    .init      = output_init,
    
    .query_formats = output_query_formats,
    
    .inputs    = (AVFilterPad[]) {{ .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .end_frame     = output_end_frame,
        .min_perms     = AV_PERM_READ, },
        { .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name = NULL }},
};



char *inmemory_buffer;
int64_t inmemory_buffer_size;
URLProtocol inmemory_protocol;

static int in_memory_url_open(URLContext *h, const char *filename, int flags)
{
    h->priv_data = 0;
    return 0;
}
static int in_memory_url_read(URLContext *h, unsigned char *buf, int size)
{
    if ((int)h->priv_data + size > inmemory_buffer_size)
    {
        size = inmemory_buffer_size - (int)h->priv_data;
        if (size == 0)
        {
            return 0;
        }
    }
    memcpy(buf, &inmemory_buffer[(int)h->priv_data], size);
    h->priv_data = (void *)((int)h->priv_data + size);
    return size;
}


static int in_memory_url_write(URLContext *h, const unsigned char *buf, int size)
{
    if ((int)h->priv_data + size > inmemory_buffer_size)
    {
        size = inmemory_buffer_size - (int)h->priv_data;
    }
    memcpy(&inmemory_buffer[(int)h->priv_data], buf, size);
    h->priv_data = (void *)((int)h->priv_data + size);
    return size;
}

static int64_t in_memory_url_seek(URLContext *h, int64_t pos, int whence)
{
    if (whence == AVSEEK_SIZE)
    {
        return inmemory_buffer_size;
    }
    else if (whence == SEEK_END)
    {
        h->priv_data = (void *)((int)inmemory_buffer_size);
        return inmemory_buffer_size;
    }
    else if (whence == SEEK_CUR)
    {
        h->priv_data = (void *)((int)inmemory_buffer_size);
        return (int)h->priv_data;
    }
    
    h->priv_data = (void *)((int)pos);
    return 0;
}


static int in_memory_url_close(URLContext *h)
{
    return 0;
}


static int in_memory_url_read_pause(URLContext *h, int pause)
{
    return 0;
}

static int64_t in_memory_url_read_seek(URLContext *h, int stream_index, int64_t timestamp, int flags)
{
    return 0;
}


static int in_memory_url_get_file_handle(URLContext *h)
{
    return (int)inmemory_buffer;
}

static void register_inmemory_protocol()
{
    inmemory_protocol.name = "inmemory";
    inmemory_protocol.next = NULL;
    inmemory_protocol.url_open = in_memory_url_open;
    inmemory_protocol.url_read = in_memory_url_read;
    inmemory_protocol.url_write = in_memory_url_write;
    inmemory_protocol.url_seek = in_memory_url_seek;
    inmemory_protocol.url_close = in_memory_url_close;
    inmemory_protocol.url_read_pause = in_memory_url_read_pause;
    inmemory_protocol.url_read_seek = in_memory_url_read_seek;
    inmemory_protocol.url_get_file_handle = in_memory_url_get_file_handle;
    
    av_register_protocol2(&inmemory_protocol, sizeof(inmemory_protocol));
}

static int threadCount()
{
    static int threadCount = 0;
    
    if (threadCount == 0)
    {
#ifdef __APPLE__
        int mib[2];
        size_t len;
        
        mib[0] = CTL_HW;
        mib[1] = HW_NCPU;
        len = sizeof(threadCount);
        
        sysctl((int *)mib, (u_int)2, &threadCount, &len, NULL, 0);
#elif defined(_WIN32)
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        threadCount = sysInfo.dwNumberOfProcessors;
#else
        threadCount = sysconf( _SC_NPROCESSORS_ONLN );
#endif
        threadCount = MIN(MAX_THREADS, MAX(2, threadCount));
        
        //if(verbose > 0)
        PMS_Log(LOG_LEVEL_DEBUG, "Thread count: %d\n", threadCount);
    }
    
    if (threadCount > MAX_THREADS)
    {
        threadCount = MAX_THREADS;
    }
    
    return threadCount;
}

static size_t FilenameSuffixLength()
{
    // Length of blah excluding %s when %d is expanded... "%s-%05d.ts"
    return 9;
}

static void UpdateFilePathForSegment(STMTranscodeSettings *settings)
{
    // Make sure the directory exists.
    char dir[4096];
    strcpy(dir, settings->sts_output_file_base);
    
    // Back up to the directory.
    int i=strlen(dir);
    while (dir[i] != '/' && dir[i] != '\\')
        i--;
    
    dir[i] = '\0';
    
    snprintf(settings->sts_output_file_path, 4095, "%s-tmp.ts", dir);
}

static int ffmpeg_exit(int ret)
{
    STMTranscodeContext *context = STMGlobalTranscodeContext();
    
    int i;
    
    /* close files */
    for(i=0;i<context->stc_nb_output_files;i++) {
        /* maybe av_close_output_file ??? */
        AVFormatContext *s = context->stc_output_files[i];
        int j;
        if (!(s->oformat->flags & AVFMT_NOFILE) && s->pb)
        {
            url_fclose(s->pb);
            
            // Compute the final filename.
            char path[4096];
            snprintf(path, 4095, "%s-%05lld.ts", globalTranscodeContext.stc_settings.sts_output_file_base,
                     globalTranscodeContext.stc_settings.sts_output_segment_index);
            // Move it over.
            rename(globalTranscodeContext.stc_settings.sts_output_file_path, path);
        }
        
        for(j=0;j<s->nb_streams;j++) {
            av_metadata_free(&s->streams[j]->metadata);
            av_free(s->streams[j]->codec);
            av_free(s->streams[j]->info);
            av_free(s->streams[j]);
        }
        for(j=0;j<s->nb_programs;j++) {
            av_metadata_free(&s->programs[j]->metadata);
        }
        for(j=0;j<s->nb_chapters;j++) {
            av_metadata_free(&s->chapters[j]->metadata);
        }
        av_metadata_free(&s->metadata);
        av_free(s);
    }
    for(i=0;i<context->stc_nb_input_files;i++) {
        av_close_input_file(context->stc_input_files[i]);
    }
    
    av_free(context->stc_input_codecs);
    av_free(context->stc_output_codecs);
    av_free(context->stc_stream_maps);
    av_free(context->stc_meta_data_maps);
    
    av_free(context->stc_audio_buffer);
    av_free(context->stc_audio_output);
    context->stc_allocated_audio_buffer_size= context->stc_allocated_audio_output_size= 0;
    av_free(context->stc_audio_samples);
    
#if CONFIG_AVFILTER
    avfilter_uninit();
#endif
    
    exit(ret); /* not all OS-es handle main() return value */
    return ret;
}

static int64_t SegmentEnd(STMTranscodeSettings *settings)
{
    int64_t segment_end = 
    AV_TIME_BASE * settings->sts_output_segment_length *
    (settings->sts_output_segment_index + 1);
    return segment_end;
}

static int64_t BaseTime(STMTranscodeSettings *settings, int64_t stream_pts, AVRational stream_time_base)
{
    int64_t current_pts =
    av_rescale_q(stream_pts, stream_time_base, AV_TIME_BASE_Q) +
    settings->sts_input_start_time;
    return current_pts;
}

static void SignalHandler(int signal)
{
    //
    // A signal handler which catches exceptions only to quit might seem
    // pointless but it stops error dialogs about uncaught exceptions on
    // Windows.
    //
    ffmpeg_exit(1);
}

static void* WatchForParentTermination (void* arg) {	
#ifdef __APPLE__
    pid_t ppid = getppid ();	// get parent pid 
    
    int kq = kqueue (); 
    if (kq != -1) { 
        struct kevent procEvent;	// wait for parent to exit 
        EV_SET (&procEvent,	 // kevent 
                ppid,	 // ident 
                EVFILT_PROC,	// filter 
                EV_ADD,	 // flags 
                NOTE_EXIT,	 // fflags 
                0,	 // data 
                0);	 // udata 
        kevent (kq, &procEvent, 1, &procEvent, 1, 0); 
    }
#elif defined(_WIN32)
    HANDLE parentProcess = (HANDLE)arg;
    WaitForSingleObject(parentProcess, INFINITE);
    CloseHandle(parentProcess);
#else
    for (int status=0; status == 0;)
    {
        status = kill(getppid(), 0);
        usleep(1000000);
    }
    
    fprintf(stderr, "The parent process existed, I'm exiting too.\n");
#endif
    ffmpeg_exit(0);	
    return 0;
}

/* similar to ff_dynarray_add() and av_fast_realloc() */
static void *grow_array(void *array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        fprintf(stderr, "Array too big.\n");
        ffmpeg_exit(1);
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc(array, new_size*elem_size);
        if (!tmp) {
            fprintf(stderr, "Could not alloc buffer.\n");
            ffmpeg_exit(1);
        }
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}


static void AddNewInputCodec(STMTranscodeContext *context, AVCodec *codec)
{
    context->stc_nb_input_codecs++;
    context->stc_input_codecs = grow_array(context->stc_input_codecs, sizeof(AVCodec *), &context->stc_input_codecs_capacity, context->stc_nb_input_codecs);
    context->stc_input_codecs[context->stc_nb_input_codecs - 1] = codec;
}
static void AddNewOutputCodec(STMTranscodeContext *context, AVCodec *codec)
{
    context->stc_nb_output_codecs++;
    context->stc_output_codecs = grow_array(context->stc_output_codecs, sizeof(AVCodec *), &context->stc_output_codecs_capacity, context->stc_nb_output_codecs);
    context->stc_output_codecs[context->stc_nb_output_codecs - 1] = codec;
}
static void AddNewInputFile(STMTranscodeContext *context, AVFormatContext *file, int64_t ts_offset)
{
    context->stc_nb_input_files++;
    int old_capacity = context->stc_input_files_capacity;
    context->stc_input_files_ts_offset = grow_array(context->stc_input_files_ts_offset, sizeof(int64_t), &old_capacity, context->stc_nb_input_files);
    context->stc_input_files = grow_array(context->stc_input_files, sizeof(AVFormatContext *), &context->stc_input_files_capacity, context->stc_nb_input_files);
    context->stc_input_files[context->stc_nb_input_files - 1] = file;
    context->stc_input_files_ts_offset[context->stc_nb_input_files - 1] = ts_offset;
}
static void RemoveLastInputFile(STMTranscodeContext *context)
{
    av_close_input_file(context->stc_input_files[context->stc_nb_input_files - 1]);
    context->stc_nb_input_files--;
}
static void AddNewOutputFile(STMTranscodeContext *context, AVFormatContext *file)
{
    context->stc_nb_output_files++;
    
    int old_capacity = context->stc_output_files_capacity;
    context->stc_output_streams_for_file = grow_array(context->stc_output_streams_for_file, sizeof(AVOutputStream **), &old_capacity, context->stc_nb_output_files);
    context->stc_output_streams_for_file[context->stc_nb_output_files - 1] = NULL;
    
    old_capacity = context->stc_output_files_capacity;
    context->stc_nb_output_streams_for_file = grow_array(context->stc_nb_output_streams_for_file, sizeof(int), &old_capacity, context->stc_nb_output_files);
    context->stc_nb_output_streams_for_file[context->stc_nb_output_files - 1] = 0;
    
    context->stc_output_files = grow_array(context->stc_output_files, sizeof(AVFormatContext *), &context->stc_output_files_capacity, context->stc_nb_output_files);
    context->stc_output_files[context->stc_nb_output_files - 1] = file;
}
static void AddNewStreamMap(STMTranscodeContext *context, AVStreamMap map)
{
    context->stc_nb_stream_maps++;
    context->stc_stream_maps = grow_array(context->stc_stream_maps, sizeof(AVStreamMap), &context->stc_stream_maps_capacity, context->stc_nb_stream_maps);
    context->stc_stream_maps[context->stc_nb_stream_maps - 1] = map;
}
static void AddNewChapterMap(STMTranscodeContext *context, AVChapterMap map)
{
    context->stc_nb_chapter_maps++;
    context->stc_chapter_maps = grow_array(context->stc_chapter_maps, sizeof(AVChapterMap), &context->stc_chapter_maps_capacity, context->stc_nb_chapter_maps);
    context->stc_chapter_maps[context->stc_nb_chapter_maps - 1] = map;
}
static void AddNewMetaDataMapping(STMTranscodeContext *context, AVMetaDataMap input, AVMetaDataMap output)
{
    context->stc_nb_meta_data_maps++;
    context->stc_meta_data_maps = grow_array(context->stc_meta_data_maps, sizeof(AVMetaDataMap) * 2, &context->stc_meta_data_maps_capacity, context->stc_nb_meta_data_maps);
    context->stc_meta_data_maps[context->stc_nb_meta_data_maps - 1][1] = input;
    context->stc_meta_data_maps[context->stc_nb_meta_data_maps - 1][0] = output;
}

static AVOutputStream *new_output_stream(AVFormatContext *oc, int file_idx)
{
    STMTranscodeContext *context = STMGlobalTranscodeContext();
    
    int idx = oc->nb_streams - 1;
    AVOutputStream *ost;
    
    context->stc_output_streams_for_file[file_idx] =
    grow_array(context->stc_output_streams_for_file[file_idx],
               sizeof(*context->stc_output_streams_for_file[file_idx]),
               &context->stc_nb_output_streams_for_file[file_idx],
               oc->nb_streams);
    ost = context->stc_output_streams_for_file[file_idx][idx] =
    av_mallocz(sizeof(AVOutputStream));
    if (!ost) {
        fprintf(stderr, "Could not alloc output stream\n");
        ffmpeg_exit(1);
    }
    ost->file_index = file_idx;
    ost->index = idx;
    return ost;
}

static double
get_sync_ipts(const AVOutputStream *ost)
{
    const AVInputStream *ist = ost->sync_ist;
    return (double)(ist->pts - zero_start_time)/AV_TIME_BASE;
}

static int process_segment_change(STMTranscodeSettings *settings, AVFormatContext *s, AVPacket *pkt)
{
    if (settings->sts_output_segment_length == 0 || pkt->pts == AV_NOPTS_VALUE)
    {
        return 0;
    }
    //output_segment_index (current segment) FRANK,Plex
    int64_t end_ts = settings->sts_output_duration;
    int64_t segment_end = SegmentEnd(settings);
    int64_t current_pts = BaseTime(settings,  pkt->pts, s->streams[pkt->stream_index]->time_base);
    
    segment_end = MIN(segment_end, end_ts);
    current_pts = MIN(end_ts, current_pts);
    
    settings->sts_last_output_pts = current_pts;
    
    //	fprintf(stderr, "outputting pts %lld for stream %d\n", current_pts, pkt->stream_index);
    
    if (current_pts >= segment_end && current_pts <= end_ts)
    {
        const int output_size = url_ftell(s->pb);
        if (output_size>0)
        {
            put_flush_packet(s->pb);
            
            file_size += output_size;
            
            url_fclose(s->pb);
            
            clock_t t = clock();
            double passedTime = (double)(t - tnow) / CLOCKS_PER_SEC;
            tnow = t;
            plex_totalSegmentTime += passedTime;
            plex_totalSegmentCount++;
            
            if (verbose > 0 || settings->sts_output_segment_index < 10)
                PMS_Log(LOG_LEVEL_DEBUG, "Wrote segment %lld (%0.2fs, %0.2fs, %0.2fmb)", settings->sts_output_segment_index, passedTime, plex_totalSegmentTime/plex_totalSegmentCount, output_size/(1024.0f*1024.0f));
            
#if !defined(VERBOSE) || VERBOSE > 0
            if (verbose == 0 || (getenv("NoAcknowledgements") == NULL ||
                                 strcmp(getenv("NoAcknowledgements"), "1") != 0))
            {
#endif
                
#define ACK_LENGTH 3
                //PLEX
                /*		char ackRead[ACK_LENGTH];
                 size_t read = fread(ackRead, ACK_LENGTH, 1, stdin);
                 if (read != 1 || strncmp(ackRead, "ack", ACK_LENGTH) != 0)
                 {
                 fprintf(stderr, "Acknowledgment failure: %ld, %.3s\n", read, ackRead);
                 ffmpeg_exit(0);
                 }*/
                
#if !defined(VERBOSE) || VERBOSE > 0
            }
#endif
            
            // Compute the final filename.
            char path[4096];
            snprintf(path, 4095, "%s-%05lld.ts", globalTranscodeContext.stc_settings.sts_output_file_base,
                     globalTranscodeContext.stc_settings.sts_output_segment_index);
            
            // Move it over.
            rename(settings->sts_output_file_path, path);
            
            PLEXwaitForSegmentAck(settings->sts_output_segment_index);
            settings->sts_output_segment_index++;
            UpdateFilePathForSegment(settings);

            // Make sure the MPEG-TS encoder starts every file with PAT/PMT.
            // And an I-Frame for the video stream.
            s->ctx_flags |= AVFMTCTX_TS_FORCE_PAT;
            settings->sts_force_i_frame = 1;

            if (url_fopen(&s->pb, settings->sts_output_file_path, URL_WRONLY) < 0)
            {
                PMS_Log(LOG_LEVEL_ERROR, "Couldn't open '%s'", settings->sts_output_file_path);
                s->pb = NULL;
                return -1;
            }
        }
    }
    return 0;
}

int compute_pkt_fields2(AVFormatContext *s, AVStream *st, AVPacket *pkt);

static int write_frame(AVFormatContext *s, AVPacket *pkt, AVCodecContext *avctx, AVBitStreamFilterContext *bsfc)
{
    while(bsfc){
        AVPacket new_pkt= *pkt;
        int a= av_bitstream_filter_filter(bsfc, avctx, bsfc->args,
                                          &new_pkt.data, &new_pkt.size,
                                          pkt->data, pkt->size,
                                          pkt->flags & AV_PKT_FLAG_KEY);
        if(a>0){
            av_free_packet(pkt);
            new_pkt.destruct= av_destruct_packet;
        } else if(a<0){
            fprintf(stderr, "%s failed for stream %d, codec %s",
                    bsfc->filter->name, pkt->stream_index,
                    avctx->codec ? avctx->codec->name : "copy");
            return -1;
        }
        *pkt= new_pkt;
        
        bsfc= bsfc->next;
    }
    
    AVStream *st= s->streams[ pkt->stream_index];
    
    //FIXME/XXX/HACK drop zero sized packets
    if(st->codec->codec_type == AVMEDIA_TYPE_AUDIO && pkt->size==0)
        return 0;
    
    //av_log(NULL, AV_LOG_DEBUG, "av_interleaved_write_frame %d %"PRId64" %"PRId64"\n", pkt->size, pkt->dts, pkt->pts);
    if(compute_pkt_fields2(s, st, pkt) < 0 && !(s->oformat->flags & AVFMT_NOTIMESTAMPS))
        return -1;
    
    if(pkt->dts == AV_NOPTS_VALUE && !(s->oformat->flags & AVFMT_NOTIMESTAMPS))
        return -1;
    
    STMTranscodeSettings *settings = &globalTranscodeContext.stc_settings;
    int64_t current_pts = BaseTime(settings, pkt->pts, s->streams[pkt->stream_index]->time_base);
    
    for(;;){
        int flush =
        (current_pts > settings->sts_last_mux_pts + settings->sts_output_segment_length * AV_TIME_BASE) ||
        settings->sts_output_padding;
        
        AVPacket opkt;
        int ret= av_interleave_packet_per_dts(s, &opkt, pkt, flush);
        if(ret<=0) //FIXME cleanup needed for ret<0 ?
            return ret;
        
        if (process_segment_change(settings, s, &opkt) != 0)
        {
            return -1;
        }
        
        ret= s->oformat->write_packet(s, &opkt);
        
        settings->sts_last_mux_pts =
        av_rescale_q(opkt.pts, s->streams[opkt.stream_index]->time_base, AV_TIME_BASE_Q);
        
        av_free_packet(&opkt);
        pkt= NULL;
        
        if(ret<0)
            return ret;
        if(url_ferror(s->pb))
            return url_ferror(s->pb);
    }
    
    return 0;
}

#define MAX_AUDIO_PACKET_SIZE (1 * 1024 * 1024)

static int do_audio_out(AVFormatContext *s,
                        AVOutputStream *ost,
                        AVInputStream *ist,
                        unsigned char *buf, int size)
{
	uint8_t *buftmp;
	int64_t audio_out_size, audio_buf_size;
	int64_t allocated_for_size= size;
    
	int size_out, frame_bytes, ret, resample_changed;
	AVCodecContext *enc= ost->st->codec;
	AVCodecContext *dec= ist->st->codec;
	int osize = av_get_bytes_per_sample(enc->sample_fmt);
	int isize = av_get_bytes_per_sample(dec->sample_fmt);
	const int coded_bps = av_get_bits_per_sample(enc->codec->id);
    
need_realloc:
	audio_buf_size= (allocated_for_size + isize*dec->channels - 1) / (isize*dec->channels);
	audio_buf_size= (audio_buf_size*enc->sample_rate + dec->sample_rate) / dec->sample_rate;
	audio_buf_size= audio_buf_size*2 + 10000; //safety factors for the deprecated resampling API
	audio_buf_size= FFMAX(audio_buf_size, enc->frame_size);
	audio_buf_size*= osize*enc->channels;
    
	audio_out_size= FFMAX(audio_buf_size, enc->frame_size * osize * enc->channels);
	if(coded_bps > 8*osize)
		audio_out_size= audio_out_size * coded_bps / (8*osize);
	audio_out_size += FF_MIN_BUFFER_SIZE;
    
	if(audio_out_size > INT_MAX || audio_buf_size > INT_MAX){
		fprintf(stderr, "Buffer sizes too large\n");
		ffmpeg_exit(1);
	}
    
	av_fast_malloc(&audio_buf, &allocated_audio_buf_size, audio_buf_size);
	av_fast_malloc(&audio_out, &allocated_audio_out_size, audio_out_size);
	if (!audio_buf || !audio_out){
		fprintf(stderr, "Out of memory in do_audio_out\n");
		ffmpeg_exit(1);
	}
    
	if (enc->channels != dec->channels)
		ost->audio_resample = 1;
    
	resample_changed = ost->resample_sample_fmt  != dec->sample_fmt ||
    ost->resample_channels    != dec->channels   ||
    ost->resample_sample_rate != dec->sample_rate;
    
	if ((ost->audio_resample && !ost->resample) || resample_changed) {
		if (resample_changed) {
			av_log(NULL, AV_LOG_INFO, "Input stream #%d.%d frame changed from rate:%d fmt:%s ch:%d to rate:%d fmt:%s ch:%d\n",
                   ist->file_index, ist->st->index,
                   ost->resample_sample_rate, av_get_sample_fmt_name(ost->resample_sample_fmt), ost->resample_channels,
                   dec->sample_rate, av_get_sample_fmt_name(dec->sample_fmt), dec->channels);
			ost->resample_sample_fmt  = dec->sample_fmt;
			ost->resample_channels    = dec->channels;
			ost->resample_sample_rate = dec->sample_rate;
			if (ost->resample)
				audio_resample_close(ost->resample);
		}
		/* if plex_audio_sync_method is >1 the resampler is needed for audio drift compensation */
		if (plex_audio_sync_method <= 1 &&
			ost->resample_sample_fmt  == enc->sample_fmt &&
			ost->resample_channels    == enc->channels   &&
            ost->resample_sample_rate == enc->sample_rate) {
			ost->resample = NULL;
			ost->audio_resample = 0;
		} else {
			if (dec->sample_fmt != AV_SAMPLE_FMT_S16)
				fprintf(stderr, "Warning, using s16 intermediate sample format for resampling\n");
			ost->resample = av_audio_resample_init(enc->channels,    dec->channels,
                                                   enc->sample_rate, dec->sample_rate,
                                                   enc->sample_fmt,  dec->sample_fmt,
                                                   16, 10, 0, 0.8);
			if (!ost->resample) {
				fprintf(stderr, "Can not resample %d channels @ %d Hz to %d channels @ %d Hz\n",
                        dec->channels, dec->sample_rate,
                        enc->channels, enc->sample_rate);
				ffmpeg_exit(1);
			}
		}
	}
    
#define MAKE_SFMT_PAIR(a,b) ((a)+AV_SAMPLE_FMT_NB*(b))
	if (!ost->audio_resample && dec->sample_fmt!=enc->sample_fmt &&
        MAKE_SFMT_PAIR(enc->sample_fmt,dec->sample_fmt)!=ost->reformat_pair) {
		if (ost->reformat_ctx)
			av_audio_convert_free(ost->reformat_ctx);
		ost->reformat_ctx = av_audio_convert_alloc(enc->sample_fmt, 1,
                                                   dec->sample_fmt, 1, NULL, 0);
		if (!ost->reformat_ctx) {
			fprintf(stderr, "Cannot convert %s sample format to %s sample format\n",
                    av_get_sample_fmt_name(dec->sample_fmt),
                    av_get_sample_fmt_name(enc->sample_fmt));
			ffmpeg_exit(1);
		}
		ost->reformat_pair=MAKE_SFMT_PAIR(enc->sample_fmt,dec->sample_fmt);
	}
    
	if(plex_audio_sync_method){
		double delta = get_sync_ipts(ost) * enc->sample_rate - ost->sync_opts
        - av_fifo_size(ost->fifo)/(enc->channels * 2);
		double idelta= delta*dec->sample_rate / enc->sample_rate;
		int byte_delta= ((int)idelta)*2*dec->channels;
        
		//FIXME resample delay
		if(fabs(delta) > 50){
			if(ist->is_start || fabs(delta) > plex_audio_drift_threshold*enc->sample_rate){
				if(byte_delta < 0){
					byte_delta= FFMAX(byte_delta, -size);
					size += byte_delta;
					buf  -= byte_delta;
					if(verbose > 2)
						fprintf(stderr, "discarding %d audio samples\n", (int)-delta);
					if(!size)
						return 0;
					ist->is_start=0;
				}else{
					static uint8_t *input_tmp= NULL;
					input_tmp= av_realloc(input_tmp, byte_delta + size);
                    
					if(byte_delta > allocated_for_size - size){
						allocated_for_size= byte_delta + (int64_t)size;
						goto need_realloc;
					}
					ist->is_start=0;
                    
					memset(input_tmp, 0, byte_delta);
					memcpy(input_tmp + byte_delta, buf, size);
					buf= input_tmp;
					size += byte_delta;
					if(verbose > 2)
						fprintf(stderr, "adding %d audio samples of silence\n", (int)delta);
				}
			}else if(plex_audio_sync_method>1){
				int comp= av_clip(delta, -plex_audio_sync_method, plex_audio_sync_method);
				av_assert0(ost->audio_resample);
				if(verbose > 2)
					fprintf(stderr, "compensating audio timestamp drift:%f compensation:%d in:%d\n", delta, comp, enc->sample_rate);
                //                fprintf(stderr, "drift:%f len:%d opts:%"PRId64" ipts:%"PRId64" fifo:%d\n", delta, -1, ost->sync_opts, (int64_t)(get_sync_ipts(ost) * enc->sample_rate), av_fifo_size(ost->fifo)/(ost->st->codec->channels * 2));
				av_resample_compensate(*(struct AVResampleContext**)ost->resample, comp, enc->sample_rate);
			}
		}
    }else
        ost->sync_opts= lrintf(get_sync_ipts(ost) * enc->sample_rate)
        - av_fifo_size(ost->fifo)/(enc->channels * 2); //FIXME wrong
    
    if (ost->audio_resample) {
        buftmp = audio_buf;
        size_out = audio_resample(ost->resample,
                                  (short *)buftmp, (short *)buf,
                                  size / (dec->channels * isize));
        size_out = size_out * enc->channels * osize;
    } else {
        buftmp = buf;
        size_out = size;
    }
    
    if (!ost->audio_resample && dec->sample_fmt!=enc->sample_fmt) {
        const void *ibuf[6]= {buftmp};
        void *obuf[6]= {audio_buf};
        int istride[6]= {isize};
        int ostride[6]= {osize};
        int len= size_out/istride[0];
        if (av_audio_convert(ost->reformat_ctx, obuf, ostride, ibuf, istride, len)<0) {
            printf("av_audio_convert() failed\n");
            if (exit_on_error)
                ffmpeg_exit(1);
            return 0;
        }
        buftmp = audio_buf;
        size_out = len*osize;
    }
    
	/* now encode as many frames as possible */
    if (enc->frame_size > 1) {
		/* output resampled raw samples */
        if (av_fifo_realloc2(ost->fifo, av_fifo_size(ost->fifo) + size_out) < 0) {
            fprintf(stderr, "av_fifo_realloc2() failed\n");
            ffmpeg_exit(1);
        }
        av_fifo_generic_write(ost->fifo, buftmp, size_out, NULL);
        
        frame_bytes = enc->frame_size * osize * enc->channels;
        
        while (av_fifo_size(ost->fifo) >= frame_bytes) {
            AVPacket pkt;
            av_init_packet(&pkt);
            
            av_fifo_generic_read(ost->fifo, audio_buf, frame_bytes, NULL);
            
			//FIXME pass ost->sync_opts as AVFrame.pts in avcodec_encode_audio()
            
            ret = avcodec_encode_audio(enc, audio_out, audio_out_size,
                                       (short *)audio_buf);
            if (ret < 0) {
                fprintf(stderr, "Audio encoding failed\n");
                ffmpeg_exit(1);
            }
            audio_size += ret;
            pkt.stream_index= ost->index;
            pkt.data= audio_out;
            pkt.size= ret;
            if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
                pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
            pkt.flags |= AV_PKT_FLAG_KEY;
            if (write_frame(s, &pkt, enc, ost->bitstream_filters)){
                return -1;
            }
            
            ost->sync_opts += enc->frame_size;
        }
    } else {
        AVPacket pkt;
        av_init_packet(&pkt);
        
        ost->sync_opts += size_out / (osize * enc->channels);
        
		/* output a pcm frame */
		/* determine the size of the coded buffer */
        size_out /= osize;
        if (coded_bps)
            size_out = size_out*coded_bps/8;
        
        if(size_out > audio_out_size){
            fprintf(stderr, "Internal error, buffer size too small\n");
            ffmpeg_exit(1);
        }
        
		//FIXME pass ost->sync_opts as AVFrame.pts in avcodec_encode_audio()
        ret = avcodec_encode_audio(enc, audio_out, size_out,
                                   (short *)buftmp);
        if (ret < 0) {
            fprintf(stderr, "Audio encoding failed\n");
            ffmpeg_exit(1);
        }
        audio_size += ret;
        pkt.stream_index= ost->index;
        pkt.data= audio_out;
        pkt.size= ret;
        if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
            pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
        pkt.flags |= AV_PKT_FLAG_KEY;
        if (write_frame(s, &pkt, enc, ost->bitstream_filters)){
            return -1;
        }
    }
    return 0;
}

static void pre_process_video_frame(AVInputStream *ist, AVPicture *picture, void **bufp)
{
    AVCodecContext *dec;
    AVPicture *picture2;
    AVPicture picture_tmp;
    uint8_t *buf = 0;
    
    dec = ist->st->codec;
    
    /* deinterlace : must be done before any resize */
    if (/*0 &&*/ ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
        dec->coded_frame &&
        dec->coded_frame->interlaced_frame &&
        globalTranscodeContext.stc_settings.sts_source_video_interlaced &&
        globalTranscodeContext.stc_settings.sts_video_copy == 0) {
        int size;
        
        /* create temporary picture */
        size = avpicture_get_size(dec->pix_fmt, dec->width, dec->height);
        buf = av_malloc(size);
        if (!buf)
            return;
        
        picture2 = &picture_tmp;
        avpicture_fill(picture2, buf, dec->pix_fmt, dec->width, dec->height);
        
        if(avpicture_deinterlace(picture2, picture,
                                 dec->pix_fmt, dec->width, dec->height) < 0) {
            /* if error, do not deinterlace */
            fprintf(stderr, "Deinterlacing failed\n");
            av_free(buf);
            buf = NULL;
            picture2 = picture;
        }
    } else {
        picture2 = picture;
    }
    
    if (picture != picture2)
        *picture = *picture2;
    *bufp = buf;
}

/* we begin to correct av delay at this threshold */
#define AV_DELAY_MAX 0.100

static int do_subtitle_out(AVFormatContext *s,
                           AVOutputStream *ost,
                           AVInputStream *ist,
                           AVSubtitle *sub,
                           int64_t pts)
{
    static uint8_t *subtitle_out = NULL;
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i;
    AVCodecContext *enc;
    AVPacket pkt;
    
    if (pts == AV_NOPTS_VALUE) {
        if (verbose > 0)
            fprintf(stderr, "Subtitle packets must have a pts\n");
        return 0;
    }
    
    enc = ost->st->codec;
    
    if (!subtitle_out) {
        subtitle_out = av_malloc(subtitle_out_max_size);
    }
    
    /* Note: DVB subtitle need one packet to draw them and one other
     packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else
        nb = 1;
    
    for(i = 0; i < nb; i++) {
        sub->pts = av_rescale_q(pts, ist->st->time_base, AV_TIME_BASE_Q);
        // start_display_time is required to be 0
        sub->pts              += av_rescale_q(sub->start_display_time, (AVRational){1, 1000}, AV_TIME_BASE_Q);
        sub->end_display_time -= sub->start_display_time;
        sub->start_display_time = 0;
        subtitle_out_size = avcodec_encode_subtitle(enc, subtitle_out,
                                                    subtitle_out_max_size, sub);
        if (subtitle_out_size < 0) {
            fprintf(stderr, "Subtitle encoding failed\n");
            return -1;
        }
        
        av_init_packet(&pkt);
        pkt.stream_index = ost->index;
        pkt.data = subtitle_out;
        pkt.size = subtitle_out_size;
        pkt.pts = av_rescale_q(sub->pts, AV_TIME_BASE_Q, ost->st->time_base);
        if (enc->codec_id == CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
             it in the codec would be better */
            if (i == 0)
                pkt.pts += 90 * sub->start_display_time;
            else
                pkt.pts += 90 * sub->end_display_time;
        }
        
        if (write_frame(s, &pkt, ost->st->codec, ost->bitstream_filters))
        {
            return -1;
        }
    }
    
    return 0;
}

static int bit_buffer_size= 1024*256;
static uint8_t *bit_buffer= NULL;



static int do_video_out(AVFormatContext *s,
                        AVOutputStream *ost,
                        AVInputStream *ist,
                        AVFrame *in_picture,
                        int *frame_size)
{
	int nb_frames, i, ret, av_unused resample_changed;
	AVFrame *final_picture, *formatted_picture;
	AVCodecContext *enc, *dec;
	double sync_ipts;
    
	enc = ost->st->codec;
	dec = ist->st->codec;
    
	sync_ipts = get_sync_ipts(ost) / av_q2d(enc->time_base);
    //fprintf(stderr, "ipts %f\n", sync_ipts);
	/* by default, we output a single frame */
	nb_frames = 1;
    
	*frame_size = 0;
    
	if(plex_video_sync_method){
		double vdelta = sync_ipts - ost->sync_opts;
		//FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
		if (vdelta < -1.1)
			nb_frames = 0;
		else if (plex_video_sync_method == 2 || (plex_video_sync_method<0 && (s->oformat->flags & AVFMT_VARIABLE_FPS))){
			if(vdelta<=-0.6){
				nb_frames=0;
            }else if(vdelta>0.6)
                ost->sync_opts= lrintf(sync_ipts);
        }else if (vdelta > 1.1)
            nb_frames = lrintf(vdelta);
        //fprintf(stderr, "vdelta:%f, ost->sync_opts:%"PRId64", ost->sync_ipts:%f nb_frames:%d\n", vdelta, ost->sync_opts, get_sync_ipts(ost), nb_frames);
        if (nb_frames == 0){
            ++nb_frames_drop;
            if (verbose>2)
                fprintf(stderr, "*** drop!\n");
        }else if (nb_frames > 1) {
            nb_frames_dup += nb_frames - 1;
            if (verbose>2)
                fprintf(stderr, "*** %d dup!\n", nb_frames-1);
        }
    }else
        ost->sync_opts= lrintf(sync_ipts);
    
    STMTranscodeSettings *settings = &globalTranscodeContext.stc_settings;
    nb_frames= FFMIN(nb_frames, (settings->sts_output_single_frame_only ? 1 : INT_MAX) - ost->frame_number);
    if (nb_frames <= 0)
        return 0;
    
    formatted_picture = in_picture;
    final_picture = formatted_picture;
    
#if !CONFIG_AVFILTER
    resample_changed = ost->resample_width   != dec->width  ||
    ost->resample_height  != dec->height ||
    ost->resample_pix_fmt != dec->pix_fmt;
    
    if (resample_changed) {
        av_log(NULL, AV_LOG_INFO,
               "Input stream #%d.%d frame changed from size:%dx%d fmt:%s to size:%dx%d fmt:%s\n",
               ist->file_index, ist->st->index,
               ost->resample_width, ost->resample_height, av_get_pix_fmt_name(ost->resample_pix_fmt),
               dec->width         , dec->height         , av_get_pix_fmt_name(dec->pix_fmt));
        ost->resample_width   = dec->width;
        ost->resample_height  = dec->height;
        ost->resample_pix_fmt = dec->pix_fmt;
    }
    
    ost->video_resample = dec->width   != enc->width  ||
    dec->height  != enc->height ||
    dec->pix_fmt != enc->pix_fmt;
    
    if (ost->video_resample) {
        final_picture = &ost->resample_frame;
        if (!ost->img_resample_ctx || resample_changed) {
			/* initialize the destination picture */
            if (!ost->resample_frame.data[0]) {
                avcodec_get_frame_defaults(&ost->resample_frame);
                if (avpicture_alloc((AVPicture *)&ost->resample_frame, enc->pix_fmt,
                                    enc->width, enc->height)) {
                    fprintf(stderr, "Cannot allocate temp picture, check pix fmt\n");
                    ffmpeg_exit(1);
                }
            }
			/* initialize a new scaler context */
            sws_freeContext(ost->img_resample_ctx);
            ost->img_resample_ctx = sws_getContext(dec->width, dec->height, dec->pix_fmt,
                                                   enc->width, enc->height, enc->pix_fmt,
                                                   ost->sws_flags, NULL, NULL, NULL);
            if (ost->img_resample_ctx == NULL) {
                fprintf(stderr, "Cannot get resampling context\n");
                return -1;
            }
        }
        sws_scale(ost->img_resample_ctx, formatted_picture->data, formatted_picture->linesize,
                  0, ost->resample_height, final_picture->data, final_picture->linesize);
    }
#endif
    
	/* duplicates frame if needed */
    for(i=0;i<nb_frames;i++) {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.stream_index= ost->index;
        
        if (s->oformat->flags & AVFMT_RAWPICTURE) {
			/* raw pictures are written as AVPicture structure to
             avoid any copies. We support temorarily the older
             method. */
			AVFrame* old_frame = enc->coded_frame;
            enc->coded_frame = dec->coded_frame; //FIXME/XXX remove this hack
            pkt.data= (uint8_t *)final_picture;
            pkt.size=  sizeof(AVPicture);
            pkt.pts= av_rescale_q(ost->sync_opts, enc->time_base, ost->st->time_base);
            pkt.flags |= AV_PKT_FLAG_KEY;
            
            if (write_frame(s, &pkt, ost->st->codec, ost->bitstream_filters))
            {
                return -1;
            }
            enc->coded_frame = old_frame;
        } else {
            AVFrame big_picture;
            
            big_picture= *final_picture;
			/* better than nothing: use input picture interlaced
             settings */
            big_picture.interlaced_frame = in_picture->interlaced_frame;
			if (ost->st->codec->flags & (CODEC_FLAG_INTERLACED_DCT|CODEC_FLAG_INTERLACED_ME)) {
				if(top_field_first == -1)
					big_picture.top_field_first = in_picture->top_field_first;
				else
					big_picture.top_field_first = top_field_first;
			}
            
			/* handles sameq here. This is not correct because it may
             not be a global option */
            big_picture.quality = ost->st->quality;
			big_picture.pict_type = 0;
			//            big_picture.pts = AV_NOPTS_VALUE;
			big_picture.pts= ost->sync_opts;
			//            big_picture.pts= av_rescale(ost->sync_opts, AV_TIME_BASE*(int64_t)enc->time_base.num, enc->time_base.den);
			//av_log(NULL, AV_LOG_DEBUG, "%"PRId64" -> encoder\n", ost->sync_opts);
			if (ost->forced_kf_index < ost->forced_kf_count &&
                big_picture.pts >= ost->forced_kf_pts[ost->forced_kf_index]) {
				big_picture.pict_type = AV_PICTURE_TYPE_I;
				ost->forced_kf_index++;
			}
                        if (settings->sts_force_i_frame) {
                          big_picture.pict_type = AV_PICTURE_TYPE_I;
                          big_picture.key_frame = 1;
                          settings->sts_force_i_frame = 0;
                        }
			ret = avcodec_encode_video(enc,
                                       bit_buffer, bit_buffer_size,
                                       &big_picture);
			if (ret < 0) {
				fprintf(stderr, "Video encoding failed\n");
				return ret;
			}
            
			if(ret>0){
				pkt.data= bit_buffer;
				pkt.size= ret;
				if(enc->coded_frame->pts != AV_NOPTS_VALUE)
					pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
				/*av_log(NULL, AV_LOG_DEBUG, "encoder -> %"PRId64"/%"PRId64"\n",
                 pkt.pts != AV_NOPTS_VALUE ? av_rescale(pkt.pts, enc->time_base.den, AV_TIME_BASE*(int64_t)enc->time_base.num) : -1,
                 pkt.dts != AV_NOPTS_VALUE ? av_rescale(pkt.dts, enc->time_base.den, AV_TIME_BASE*(int64_t)enc->time_base.num) : -1);*/
                
				if(enc->coded_frame->key_frame)
                    pkt.flags |= AV_PKT_FLAG_KEY;
                if (write_frame(s, &pkt, ost->st->codec, ost->bitstream_filters))
                {
                    return -1;
                }
                *frame_size = ret;
                video_size += ret;
				//fprintf(stderr,"\nFrame: %3d size: %5d type: %d",
				//        enc->frame_number-1, ret, enc->pict_type);
				/* if two pass, output log */
                if (ost->logfile && enc->stats_out) {
                    fprintf(ost->logfile, "%s", enc->stats_out);
                }
            }
        }
        ost->sync_opts++;
        ost->frame_number++;
    }
    return 0;
}

static double psnr(double d){
    return -10.0*log(d)/log(10.0);
}

char* PMS_IssueHttpRequest(const char* url, const char* verb, int timeout, int* httpCode);

static void print_report(AVFormatContext **output_files,
                         AVOutputStream **ost_table, int nb_ostreams,
                         int is_last_report)
{
    char buf[1024];
    AVOutputStream *ost = ost_table[0];
    AVFormatContext *oc;
    int64_t total_size;
    AVCodecContext *enc;
    int frame_number, vid, i;
    double bitrate, ti1, pts;
    static int64_t last_time = -1;
    static int64_t run_time = 0;
    static int64_t start_time = 0;
    
    if (!is_last_report) {
        int64_t cur_time;
        /* display the report every 1 seconds */
        cur_time = av_gettime();
        if (last_time == -1) {
            last_time = cur_time;
            return;
        }
        if ((cur_time - last_time) < 2000000)
            return;
            
        run_time = cur_time-last_time;
        last_time = cur_time;
        
        /// PLEX
        
        if (ost->st->pts.val != AV_NOPTS_VALUE)
  			{
  			  pts = FFMAX(pts, av_rescale_q(ost->st->pts.val, ost->st->time_base, AV_TIME_BASE_Q));
  			  
  			  static int64_t last_pts = 0;
  			  static int lastRemaining = 0;
  			  
  			  // Compute speed of transcode as a multiple of real-time.
  			  float speed = (float)(pts - last_pts) / (float)run_time;
  			  
  				// Notify about progress.
  				int64_t secs = av_rescale_q(ost->st->pts.val, ost->st->time_base, AV_TIME_BASE_Q)/AV_TIME_BASE;
  				int64_t totalSecs = globalTranscodeContext.stc_input_files[0]->duration / AV_TIME_BASE;
  				
  				// Compute estimated time remaining.
          int remainingSecs = (totalSecs - secs) / speed;
          int smoothedRemaining = remainingSecs;
          if (lastRemaining != 0)
            smoothedRemaining = lastRemaining*0.5 + remainingSecs*0.5;
  				
  				char url[1024];

          if (globalTranscodeContext.plex.progressURL)
          {
            if (globalTranscodeContext.plex.delay == 0)
            	sprintf(url, "%s?progress=%.1f&size=0&speed=%.1f&remaining=%d", globalTranscodeContext.plex.progressURL, (float)secs*100.0/totalSecs, speed, smoothedRemaining);
            else
              sprintf(url, "%s?progress=%.1f&size=0&remaining=%d", globalTranscodeContext.plex.progressURL, (float)secs*100.0/totalSecs, smoothedRemaining);
            	
    				char* reply = PMS_IssueHttpRequest(url, "PUT", 1, NULL);
  				
    				if (strstr(reply, "canThrottle"))
    				{
    				  if (globalTranscodeContext.plex.delay == 0)
    				    PMS_Log(LOG_LEVEL_DEBUG, "Throttle - Going into sloth mode."); 
  				  
              globalTranscodeContext.plex.delay = 100;
            }
            else
            {
              if (globalTranscodeContext.plex.delay == 100)
    				    PMS_Log(LOG_LEVEL_DEBUG, "Throttle - Getting back to work.");
            
              globalTranscodeContext.plex.delay = 0;
            }
  				
            av_free(reply);
            lastRemaining = remainingSecs;
          }
          
          last_pts = pts;
  			}
        /// PLEX
    }

    // Don't bother with this, valgrind complains, and it's not really needed.
    return;
    
    oc = output_files[0];
    
    total_size = url_fsize(oc->pb);
    if(total_size<0) // FIXME improve url_fsize() so it works with non seekable output too
        total_size= url_ftell(oc->pb);
    
    buf[0] = '\0';
    ti1 = 1e10;
    vid = 0;
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        enc = ost->st->codec;
        if (vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "q=%2.1f ",
                     !ost->st->stream_copy ?
                     enc->coded_frame->quality/(float)FF_QP2LAMBDA : -1);
        }
        if (!vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            float t = (av_gettime()-timer_start) / 1000000.0;
            
            frame_number = ost->frame_number;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "frame=%5d fps=%3d q=%3.1f ",
                     frame_number, (t>1)?(int)(frame_number/t+0.5) : 0,
                     !ost->st->stream_copy ?
                     enc->coded_frame->quality/(float)FF_QP2LAMBDA : -1);
            if(is_last_report)
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "L");
            if (enc->flags&CODEC_FLAG_PSNR){
                int j;
                double error, error_sum=0;
                double scale, scale_sum=0;
                char type[3]= {'Y','U','V'};
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "PSNR=");
                for(j=0; j<3; j++){
                    if(is_last_report){
                        error= enc->error[j];
                        scale= enc->width*enc->height*255.0*255.0*frame_number;
                    }else{
                        error= enc->coded_frame->error[j];
                        scale= enc->width*enc->height*255.0*255.0;
                    }
                    if(j) scale/=4;
                    error_sum += error;
                    scale_sum += scale;
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%c:%2.2f ", type[j], psnr(error/scale));
                }
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "*:%2.2f ", psnr(error_sum/scale_sum));
            }
            vid = 1;
        }
        /* compute min output value */
        pts = (double)ost->st->pts.val * av_q2d(ost->st->time_base);
        if ((pts < ti1) && (pts > 0))
            ti1 = pts;
    }
    if (ti1 < 0.01)
        ti1 = 0.01;
    
    if (verbose || is_last_report) {
        bitrate = (double)(total_size * 8) / ti1 / 1000.0;
        
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
                 "size=%8.0fkB time=%0.2f bitrate=%6.1fkbits/s pts=%f",
                 (double)total_size / 1024, ti1, bitrate, pts);
        
        if (nb_frames_dup || nb_frames_drop)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " dup=%d drop=%d",
                     nb_frames_dup, nb_frames_drop);
        
        if (verbose >= 0)
            fprintf(stderr, "%s    \r", buf);
        
        fflush(stderr);
    }
    
    if (is_last_report && verbose >= 0){
        int64_t raw= audio_size + video_size + extra_size;
        fprintf(stderr, "\n");
        fprintf(stderr, "video:%1.0fkB audio:%1.0fkB global headers:%1.0fkB muxing overhead %f%%\n",
                video_size/1024.0,
                audio_size/1024.0,
                extra_size/1024.0,
                100.0*(total_size - raw)/raw
                );
    }
}


/* pkt = NULL means EOF (needed to flush decoder buffers) */
static int output_packet(AVInputStream *ist, int ist_index,
                         AVOutputStream **ost_table, int nb_ostreams,
                         const AVPacket *pkt)
{
    AVFormatContext **output_files = globalTranscodeContext.stc_output_files;
    AVFormatContext *os;
    AVOutputStream *ost;
    int ret, i;
    int got_output;
    AVFrame picture;
    void *buffer_to_free = NULL;
    static unsigned int samples_size= 0;
    AVSubtitle subtitle, *subtitle_to_free;
    int64_t pkt_pts = AV_NOPTS_VALUE;
#if CONFIG_AVFILTER
    int frame_available;
#endif
    
    AVPacket avpkt;
    int bps = av_get_bytes_per_sample(ist->st->codec->sample_fmt);
    
    if(ist->next_pts == AV_NOPTS_VALUE)
        ist->next_pts= ist->pts;
    
    if (pkt == NULL) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
        goto handle_eof;
    } else {
        avpkt = *pkt;
    }
    
    if(pkt->dts != AV_NOPTS_VALUE){
        ist->next_pts = ist->pts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);
        //fprintf(stderr, "SET pts = %f\n", (double)ist->pts);
    } else {
        //fprintf(stderr, "ERR\n");
    }
    if(pkt->pts != AV_NOPTS_VALUE)
        pkt_pts = av_rescale_q(pkt->pts, ist->st->time_base, AV_TIME_BASE_Q);
    
    //while we have more to decode or while the decoder did output something on EOF
    while (avpkt.size > 0 || (!pkt && got_output)) {
        uint8_t *data_buf, *decoded_data_buf;
        int data_size, decoded_data_size;
    handle_eof:
        ist->pts= ist->next_pts;
        
        if(avpkt.size && avpkt.size != pkt->size &&
           ((!ist->showed_multi_packet_warning && verbose>0) || verbose>1)){
            fprintf(stderr, "Multiple frames in a packet from stream %d\n", pkt->stream_index);
            ist->showed_multi_packet_warning=1;
        }
        
        /* decode the packet if needed */
        decoded_data_buf = NULL; /* fail safe */
        decoded_data_size= 0;
        data_buf  = avpkt.data;
        data_size = avpkt.size;
        
        subtitle_to_free = NULL;
        if (ist->decoding_needed) {
            switch(ist->st->codec->codec_type) {
                case AVMEDIA_TYPE_AUDIO:{
                    if(pkt && samples_size < FFMAX(pkt->size*sizeof(*samples), AVCODEC_MAX_AUDIO_FRAME_SIZE)) {
                        samples_size = FFMAX(pkt->size*sizeof(*samples), AVCODEC_MAX_AUDIO_FRAME_SIZE);
                        av_free(samples);
                        samples= av_malloc(samples_size);
                    }
                    decoded_data_size= samples_size;
                    /* XXX: could avoid copy if PCM 16 bits with same
                     endianness as CPU */
                    ret = avcodec_decode_audio3(ist->st->codec, samples, &decoded_data_size,
                                                &avpkt);
                    if (ret < 0)
                        goto fail_decode;
                    avpkt.data += ret;
                    avpkt.size -= ret;
                    data_size   = ret;
                    got_output  = decoded_data_size > 0;
                    
                    /* Some bug in mpeg audio decoder gives */
                    /* decoded_data_size < 0, it seems they are overflows */
                    if (!got_output) {
                        /* no audio frame */
                        continue;
                    }
                    decoded_data_buf = (uint8_t *)samples;
                    ist->next_pts += ((int64_t)AV_TIME_BASE/bps * decoded_data_size) /
                    (ist->st->codec->sample_rate * ist->st->codec->channels);
                    break;}
                case AVMEDIA_TYPE_VIDEO:
                    decoded_data_size = (ist->st->codec->width * ist->st->codec->height * 3) / 2;
                    /* XXX: allocate picture correctly */
                    avcodec_get_frame_defaults(&picture);
                    avpkt.pts = pkt_pts;
                    avpkt.dts = ist->pts;
                    ist->st->codec->reordered_opaque = pkt_pts;
                    pkt_pts = AV_NOPTS_VALUE;
                    
                    ret = avcodec_decode_video2(ist->st->codec,
                                                &picture, &got_output, &avpkt);
                    ist->st->quality= picture.quality;
                    if (ret < 0)
                        goto fail_decode;
                    if (!got_output) {
                        /* no picture yet */
                        goto discard_packet;
                    }
                    //ist->next_pts = ist->pts = guess_correct_pts(&ist->pts_ctx, picture.reordered_opaque, ist->pts); 
                    ist->next_pts = ist->pts = picture.best_effort_timestamp;
                    if (ist->st->codec->time_base.num != 0) {
                        int ticks= ist->st->parser ? ist->st->parser->repeat_pict+1 : ist->st->codec->ticks_per_frame;
                        ist->next_pts += ((int64_t)AV_TIME_BASE *
                                          ist->st->codec->time_base.num * ticks) /
                        ist->st->codec->time_base.den;
                    }
                    avpkt.size = 0;
                    buffer_to_free = NULL;
                    pre_process_video_frame(ist, (AVPicture *)&picture, &buffer_to_free);
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    ret = avcodec_decode_subtitle2(ist->st->codec,
                                                   &subtitle, &got_output, &avpkt);
                    if (ret < 0)
                        goto fail_decode;
                    if (!got_output) {
                        goto discard_packet;
                    }
                    subtitle_to_free = &subtitle;
                    avpkt.size = 0;
                    break;
                default:
                    goto fail_decode;
            }
        } else {
            switch(ist->st->codec->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    ist->next_pts += ((int64_t)AV_TIME_BASE * ist->st->codec->frame_size) /
                    ist->st->codec->sample_rate;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    //PLEX - Start (some streams started with no video)
                    decoded_data_size = (ist->st->codec->width * ist->st->codec->height * 3) / 2;
                    avcodec_get_frame_defaults(&picture);
                    
                    ist->st->codec->reordered_opaque = pkt_pts;
                    pkt_pts = AV_NOPTS_VALUE;
                    //ist->next_pts = ist->pts = guess_correct_pts(&ist->pts_ctx, picture.reordered_opaque, ist->pts);
                    ist->next_pts = ist->pts = picture.best_effort_timestamp;
                    //PLEX - End
                    if (ist->st->codec->time_base.num != 0) {
                        int ticks= ist->st->parser ? ist->st->parser->repeat_pict+1 : ist->st->codec->ticks_per_frame;
                        ist->next_pts += ((int64_t)AV_TIME_BASE *
                                          ist->st->codec->time_base.num * ticks) /
                        ist->st->codec->time_base.den;
                    }
                    break;
            }
            ret = avpkt.size;
            avpkt.size = 0;
        }
        
        /*buffer_to_free = NULL;
         if (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
         pre_process_video_frame(ist, (AVPicture *)&picture,
         &buffer_to_free);
         }*/
        
#if CONFIG_AVFILTER
        /*if (ist->pts >= 0 && ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO && ist->input_video_filter) {
         // add it to be filtered
         av_vsrc_buffer_add_frame(ist->input_video_filter, &picture,
         ist->pts,
         ist->st->codec->sample_aspect_ratio);
         }*/
        if(ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            if (zero_start_time == 0 || ist->pts >= zero_start_time) {
                for(i=0;i<nb_ostreams;i++) {
                    ost = ost_table[i];
                    if (ist->input_video_filter && ost->source_index == ist_index) {
                        if (!picture.sample_aspect_ratio.num)
                            picture.sample_aspect_ratio = ist->st->sample_aspect_ratio;
                        picture.pts = ist->pts;
                        
                        av_vsrc_buffer_add_frame(ist->input_video_filter, &picture, AV_VSRC_BUF_FLAG_OVERWRITE);
                    }
                }
            }
#endif
        
        // preprocess audio (volume)
        if (ist->st->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (audio_volume != 256) {
                short *volp;
                volp = samples;
                for(i=0;i<(decoded_data_size / sizeof(short));i++) {
                    int v = ((*volp) * audio_volume + 128) >> 8;
                    if (v < -32768) v = -32768;
                    if (v >  32767) v = 32767;
                    *volp++ = v;
                }
            }
        }
        
        /* frame rate emulation */
        if (rate_emu) {
            int64_t pts = av_rescale(ist->pts, 1000000, AV_TIME_BASE);
            int64_t now = av_gettime() - ist->start;
            if (pts > now)
                usleep(pts - now);
        }
        /* if output time reached then transcode raw format,
         encode packets and output them */
        if (zero_start_time == 0 || ist->pts >= zero_start_time)
            for(i=0;i<nb_ostreams;i++) {
                int frame_size;
                
                ost = ost_table[i];
                if (ost->source_index == ist_index) {
                    
#if CONFIG_AVFILTER
                    frame_available = ist->st->codec->codec_type != AVMEDIA_TYPE_VIDEO ||
                    !ist->output_video_filter || avfilter_poll_frame(ist->output_video_filter->inputs[0]);
                    while (frame_available) {
                        if (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO && ist->output_video_filter) {
                            AVRational ist_pts_tb = ist->output_video_filter->inputs[0]->time_base;
                            if (av_vsink_buffer_get_video_buffer_ref(ist->output_video_filter, &ist->picref, 0) < 0)
                                goto cont;
                            if (ist->picref) {
                                avfilter_fill_frame_from_video_buffer_ref(&picture, ist->picref);
                                ist->pts = av_rescale_q(ist->picref->pts, ist_pts_tb, AV_TIME_BASE_Q);
                            }
                        }
#endif
                        os = output_files[ost->file_index];
                        
                        /* set the input output pts pairs */
                        //ost->sync_ipts = (double)(ist->pts + input_files[ist->file_index].ts_offset - start_time)/ AV_TIME_BASE;
                        
                        if (ost->encoding_needed) {
                            av_assert0(ist->decoding_needed);
                            switch(ost->st->codec->codec_type) {
                                case AVMEDIA_TYPE_AUDIO:
                                    if (do_audio_out(os, ost, ist, decoded_data_buf, decoded_data_size))
                                        return -1;
                                    break;
                                case AVMEDIA_TYPE_VIDEO:
#if CONFIG_AVFILTER
                                    if (ist->picref->video)
                                        ost->st->codec->sample_aspect_ratio = ist->picref->video->sample_aspect_ratio;
#endif
                                    if (do_video_out(os, ost, ist, &picture, &frame_size))
                                        return -1;
                                    break;
                                case AVMEDIA_TYPE_SUBTITLE:
                                    if (do_subtitle_out(os, ost, ist, &subtitle,
                                                        pkt->pts))
                                        return -1;
                                    break;
                                default:
                                    abort();
                            }
                        } else {
                            /* If we're outputting subtitles, pass discarded subtitle packets of the
                             appropriate stream index to the subtitle renderer */
                            if (globalTranscodeContext.stc_inlineass_context &&
                                ist->st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE &&
                                pkt->stream_index == globalTranscodeContext.stc_settings.sts_subtitle_stream_index)
                            {
                                vf_inlineass_append_data(
                                                         globalTranscodeContext.stc_inlineass_context,
                                                         ist->st->codec->codec_id,
                                                         (char *)pkt->data,
                                                         pkt->size,
                                                         av_rescale_q(pkt->pts, ist->st->time_base, AV_TIME_BASE_Q) + globalTranscodeContext.stc_settings.sts_input_start_time,
                                                         av_rescale_q(pkt->convergence_duration ? pkt->convergence_duration : pkt->duration, ist->st->time_base, AV_TIME_BASE_Q),
                                                         AV_TIME_BASE_Q);
                            }
                            
                            AVFrame avframe; //FIXME/XXX remove this
                            AVPicture pict;
                            AVPacket opkt;
                            int64_t ost_tb_start_time= av_rescale_q(zero_start_time, AV_TIME_BASE_Q, ost->st->time_base);
                            
                            av_init_packet(&opkt);
                            
                            if ((!ost->frame_number && !(pkt->flags & AV_PKT_FLAG_KEY)) /*&& !copy_initial_nonkeyframes*/)
#if !CONFIG_AVFILTER
                                continue;
#else
                            goto cont;
#endif
                            
                            /* no reencoding needed : output the packet directly */
                            /* force the input stream PTS */
                            
                            avcodec_get_frame_defaults(&avframe);
                            ost->st->codec->coded_frame= &avframe;
                            avframe.key_frame = pkt->flags & AV_PKT_FLAG_KEY;
                            
                            if(ost->st->codec->codec_type == AVMEDIA_TYPE_AUDIO)
                                audio_size += data_size;
                            else if (ost->st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                                video_size += data_size;
                                ost->sync_opts++;
                            }
                            
                            opkt.stream_index= ost->index;
                            if(pkt->pts != AV_NOPTS_VALUE)
                                opkt.pts= av_rescale_q(pkt->pts, ist->st->time_base, ost->st->time_base) - ost_tb_start_time;
                            else
                                opkt.pts= AV_NOPTS_VALUE;
                            
                            if (pkt->dts == AV_NOPTS_VALUE)
                                opkt.dts = av_rescale_q(ist->pts, AV_TIME_BASE_Q, ost->st->time_base);
                            else
                                opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->st->time_base);
                            opkt.dts -= ost_tb_start_time;
                            
                            opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->st->time_base);
                            opkt.flags= pkt->flags;
                            
                            //FIXME remove the following 2 lines they shall be replaced by the bitstream filters
                            if(   ost->st->codec->codec_id != CODEC_ID_H264
                               && ost->st->codec->codec_id != CODEC_ID_MPEG1VIDEO
                               && ost->st->codec->codec_id != CODEC_ID_MPEG2VIDEO
                               ) {
                                if(av_parser_change(ist->st->parser, ost->st->codec, &opkt.data, &opkt.size, data_buf, data_size, pkt->flags & AV_PKT_FLAG_KEY))
                                    opkt.destruct= av_destruct_packet;
                            } else {
                                opkt.data = data_buf;
                                opkt.size = data_size;
                            }
                            
                            if (os->oformat->flags & AVFMT_RAWPICTURE) {
                                /* store AVPicture in AVPacket, as expected by the output format */
                                avpicture_fill(&pict, opkt.data, ost->st->codec->pix_fmt, ost->st->codec->width, ost->st->codec->height);
                                opkt.data = (uint8_t *)&pict;
                                opkt.size = sizeof(AVPicture);
                                opkt.flags |= AV_PKT_FLAG_KEY;
                            }
                            write_frame(os, &opkt, ost->st->codec, ost->bitstream_filters);
                            ost->st->codec->frame_number++;
                            ost->frame_number++;
                            av_free_packet(&opkt);
                        }
                        
#if CONFIG_AVFILTER
                    cont:
                        frame_available = (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO) &&
                        ist->output_video_filter && avfilter_poll_frame(ist->output_video_filter->inputs[0]);
                        if(ist->picref)
                            avfilter_unref_buffer(ist->picref);
                    }
#endif
                }
            }
        if (buffer_to_free)
            av_free(buffer_to_free);
        /* XXX: allocate the subtitles in the codec ? */
        if (subtitle_to_free) {
            if (subtitle_to_free->rects != NULL) {
                for (i = 0; i < subtitle_to_free->num_rects; i++) {
                    av_freep(&subtitle_to_free->rects[i]->pict.data[0]);
                    av_freep(&subtitle_to_free->rects[i]->pict.data[1]);
                    av_freep(&subtitle_to_free->rects[i]);
                }
                av_freep(&subtitle_to_free->rects);
            }
            subtitle_to_free->num_rects = 0;
            subtitle_to_free = NULL;
        }
    }
discard_packet:
    if (pkt == NULL) {
        /* EOF handling */
        
        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            if (ost->source_index == ist_index) {
                AVCodecContext *enc= ost->st->codec;
                os = output_files[ost->file_index];
                
                if(ost->st->codec->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <=1)
                    continue;
                if(ost->st->codec->codec_type == AVMEDIA_TYPE_VIDEO && (os->oformat->flags & AVFMT_RAWPICTURE))
                    continue;
                
                if (ost->encoding_needed) {
                    for(;;) {
                        AVPacket pkt;
                        int fifo_bytes;
                        av_init_packet(&pkt);
                        pkt.stream_index= ost->index;
                        
                        switch(ost->st->codec->codec_type) {
                            case AVMEDIA_TYPE_AUDIO:
                                fifo_bytes = av_fifo_size(ost->fifo);
                                ret = 0;
                                /* encode any samples remaining in fifo */
                                if (fifo_bytes > 0) {
                                    int osize = av_get_bits_per_sample_fmt(enc->sample_fmt) >> 3;
                                    int fs_tmp = enc->frame_size;
                                    
                                    av_fifo_generic_read(ost->fifo, audio_buf, fifo_bytes, NULL);
                                    if (enc->codec->capabilities & CODEC_CAP_SMALL_LAST_FRAME) {
                                        enc->frame_size = fifo_bytes / (osize * enc->channels);
                                    } else { /* pad */
                                        int frame_bytes = enc->frame_size*osize*enc->channels;
                                        if (allocated_audio_buf_size < frame_bytes)
                                            ffmpeg_exit(1);
                                        memset(audio_buf+fifo_bytes, 0, frame_bytes - fifo_bytes);
                                    }
                                    
                                    ret = avcodec_encode_audio(enc, bit_buffer, bit_buffer_size, (short *)audio_buf);
                                    pkt.duration = av_rescale((int64_t)enc->frame_size*ost->st->time_base.den,
                                                              ost->st->time_base.num, enc->sample_rate);
                                    enc->frame_size = fs_tmp;
                                }
                                if(ret <= 0) {
                                    ret = avcodec_encode_audio(enc, bit_buffer, bit_buffer_size, NULL);
                                }
                                if (ret < 0) {
                                    fprintf(stderr, "Audio encoding failed\n");
                                    ffmpeg_exit(1);
                                }
                                audio_size += ret;
                                pkt.flags |= AV_PKT_FLAG_KEY;
                                break;
                            case AVMEDIA_TYPE_VIDEO:
                                ret = avcodec_encode_video(enc, bit_buffer, bit_buffer_size, NULL);
                                if (ret < 0) {
                                    fprintf(stderr, "Video encoding failed\n");
                                    ffmpeg_exit(1);
                                }
                                video_size += ret;
                                if(enc->coded_frame && enc->coded_frame->key_frame)
                                    pkt.flags |= AV_PKT_FLAG_KEY;
                                if (ost->logfile && enc->stats_out) {
                                    fprintf(ost->logfile, "%s", enc->stats_out);
                                }
                                break;
                            default:
                                ret=-1;
                        }
                        
                        if(ret<=0)
                            break;
                        pkt.data= bit_buffer;
                        pkt.size= ret;
                        if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
                            pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
                        if (write_frame(os, &pkt, ost->st->codec, ost->bitstream_filters))
                        {
                            return -1;
                        }
                    }
                }
            }
        }
    }
    
    return 0;
fail_decode:
    return -1;
}

static char *newTitleForFile(AVFormatContext *input_file, int *isFilename)
{
    char *filename = input_file->filename;
    int filenameLength = strlen(filename);
    
    AVMetadata *metadata = input_file->metadata;
    AVMetadataTag *itemTag =
    av_metadata_get(metadata, "title", NULL, 0);
    if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
    {
        itemTag =
        av_metadata_get(metadata, "TIT2", NULL, 0);
    }
    if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
    {
        itemTag =
        av_metadata_get(metadata, "TT2", NULL, 0);
    }
    int itemLength;
    char *item;
    if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
    {
        int i = filenameLength - 1;
        while (filename[i] != STMPathSeparator)
        {
            i--;
        }
        item = &filename[i+1];
        itemLength = filenameLength - i - 1;
        
        if (isFilename)
        {
            *isFilename = 1;
        }
    }
    else
    {
        item = itemTag->value;
        itemLength = strlen(item);
        
        if (isFilename)
        {
            *isFilename = 0;
        }
    }
    
    char *result = av_malloc(sizeof(char) * itemLength + 1);
    memcpy(result, item, itemLength);
    result[itemLength] = '\0';
    
    return result;
}

static char *newArtistForFile(AVFormatContext *input_file, int *isDirname)
{
    char *filename = input_file->filename;
    int filenameLength = strlen(filename);
    
    AVMetadata *metadata = input_file->metadata;
    if (metadata == NULL)
    {
        for (int prog_num = 0; prog_num < input_file->nb_programs; prog_num++)
        {
            if (input_file->programs[prog_num]->nb_stream_indexes > 0)
            {
                metadata = input_file->programs[prog_num]->metadata;
            }
        }
        
        if (metadata)
        {
            AVMetadataTag *providerTag = av_metadata_get(metadata, "provider_name", NULL, 0);
            if (!providerTag || !providerTag->value || strlen(providerTag->value) == 0)
            {
                providerTag = NULL;
            }
            AVMetadataTag *serviceTag = av_metadata_get(metadata, "name", NULL, 0);
            if (!serviceTag || !serviceTag->value || strlen(serviceTag->value) == 0)
            {
                serviceTag = NULL;
            }
            
            char *result = NULL;
            if (providerTag && !serviceTag)
            {
                asprintf(&result, "%s", providerTag->value);
            }
            else if (serviceTag && !providerTag)
            {
                asprintf(&result, "%s", serviceTag->value);
            }
            else
            {
                asprintf(&result, "%s/%s", providerTag->value, serviceTag->value);
            }
            if (result)
            {
                if (isDirname)
                {
                    *isDirname = 0;
                }
                return result;
            }
        }
    }
    
    AVMetadataTag *groupTag =
    av_metadata_get(metadata, "author", NULL, 0);
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        groupTag =
        av_metadata_get(metadata, "TPE1", NULL, 0);
    }
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        groupTag =
        av_metadata_get(metadata, "TP1", NULL, 0);
    }
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        groupTag =
        av_metadata_get(metadata, "show", NULL, 0);
    }
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        groupTag =
        av_metadata_get(metadata, "artist", NULL, 0);
    }
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        groupTag =
        av_metadata_get(metadata, "composer", NULL, 0);
    }
    int groupLength;
    char *group;
    if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
    {
        int i = filenameLength - 1;
        while (filename[i] != STMPathSeparator)
        {
            i--;
        }
        int j = i - 1;
        while (j > 0 && filename[j] != STMPathSeparator)
        {
            j--;
        }
        if (j == 0)
        {
            group = "";
            groupLength = 0;
        }
        else
        {
            group = &filename[j+1];
            groupLength = i - j - 1;
        }
        
        if (isDirname)
        {
            *isDirname = 1;
        }
    }
    else
    {
        group = groupTag->value;
        groupLength = strlen(group);
        
        if (isDirname)
        {
            *isDirname = 0;
        }
    }
    
    char *result = av_malloc(sizeof(char) * groupLength + 1);
    memcpy(result, group, groupLength);
    result[groupLength] = '\0';
    
    return result;
}

static void print_sdp(AVFormatContext **avc, int n)
{
    char sdp[2048];
    
    avf_sdp_create(avc, n, sdp, sizeof(sdp));
    printf("SDP:\n%s\n", sdp);
    fflush(stdout);
}

static int copy_chapters(int infile, int outfile)
{
    AVFormatContext **input_files = globalTranscodeContext.stc_input_files;
    AVFormatContext **output_files = globalTranscodeContext.stc_output_files;
    
    AVFormatContext *is = input_files[infile];
    AVFormatContext *os = output_files[outfile];
    int i;
    
    for (i = 0; i < is->nb_chapters; i++) {
        AVChapter *in_ch = is->chapters[i], *out_ch;
        AVMetadataTag *t = NULL;
        int64_t ts_off   = av_rescale_q(zero_start_time - input_files_ts_offset[infile],
                                        AV_TIME_BASE_Q, in_ch->time_base);
        int64_t rt       = (recording_time == INT64_MAX) ? INT64_MAX :
        av_rescale_q(recording_time, AV_TIME_BASE_Q, in_ch->time_base);
        
        
        if (in_ch->end < ts_off)
            continue;
        if (rt != INT64_MAX && in_ch->start > rt + ts_off)
            break;
        
        out_ch = av_mallocz(sizeof(AVChapter));
        if (!out_ch)
            return AVERROR(ENOMEM);
        
        out_ch->id        = in_ch->id;
        out_ch->time_base = in_ch->time_base;
        out_ch->start     = FFMAX(0,  in_ch->start - ts_off);
        out_ch->end       = FFMIN(rt, in_ch->end   - ts_off);
        
        if (metadata_chapters_autocopy)
            av_dict_copy(&out_ch->metadata, in_ch->metadata, 0);

        os->nb_chapters++;
        os->chapters = av_realloc(os->chapters, sizeof(AVChapter)*os->nb_chapters);
        if (!os->chapters)
            return AVERROR(ENOMEM);
        os->chapters[os->nb_chapters - 1] = out_ch;
    }
    return 0;
}



/*
 * The following code is the main loop of the file converter
 */
static int transcode(AVFormatContext **output_files,
                     int nb_output_files,
                     AVFormatContext **input_files,
                     int nb_input_files,
                     AVStreamMap *stream_maps, int nb_stream_maps)
{
    int ret = 0, i, j, k, n, nb_istreams = 0, nb_ostreams = 0;
    AVFormatContext *is, *os;
    AVCodecContext *codec, *icodec;
    AVOutputStream *ost, **ost_table = NULL;
    AVInputStream *ist, **ist_table = NULL;
    AVInputFile *file_table;
    char error[1024];
    int want_sdp = 1;
    
    // Compute the number of bytes to alloc, but at least allocate 4, otherwise Valgrind complains.
    size_t bytesToAlloc = sizeof(uint8_t) * MAX(globalTranscodeContext.stc_nb_input_files, globalTranscodeContext.stc_nb_output_files);
    if (bytesToAlloc < 4)
        bytesToAlloc = 4;
    
    uint8_t *no_packet=av_mallocz(bytesToAlloc);
    int no_packet_count=0;
    
    file_table= av_mallocz(nb_input_files * sizeof(AVInputFile));
    if (!file_table)
        goto fail;
    
    /* input stream init */
    j = 0;
    for(i=0;i<nb_input_files;i++) {
        is = input_files[i];
        file_table[i].ist_index = j;
        file_table[i].nb_streams = is->nb_streams;
        j += is->nb_streams;
        //fprintf(stderr, "j=%i\n", j);
    }
    nb_istreams = j;
    
    ist_table = av_mallocz(nb_istreams * sizeof(AVInputStream *));
    if (!ist_table)
        goto fail;
    
    for(i=0;i<nb_istreams;i++) {
        ist = av_mallocz(sizeof(AVInputStream));
        if (!ist)
            goto fail;
        ist_table[i] = ist;
    }
    j = 0;
    for(i=0;i<nb_input_files;i++) {
        is = input_files[i];
        for(k=0;k<is->nb_streams;k++) {
            ist = ist_table[j++];
            ist->st = is->streams[k];
            ist->file_index = i;
            ist->index = k;
            ist->discard = 1; /* the stream is discarded by default
                               (changed later) */
        }
    }
    
    /* output stream init */
    nb_ostreams = 0;
    for(i=0;i<nb_output_files;i++) {
        os = output_files[i];
        if (!os->nb_streams) {
            dump_format(output_files[i], i, output_files[i]->filename, 1);
            fprintf(stderr, "Output file #%d does not contain any stream\n", i);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        nb_ostreams += os->nb_streams;
    }
    if (nb_stream_maps > 0 && nb_stream_maps != nb_ostreams) {
        fprintf(stderr, "Number of stream maps must match number of output streams\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    
    /* Sanity check the mapping args -- do the input files & streams exist? */
    for(i=0;i<nb_stream_maps;i++) {
        int fi = stream_maps[i].file_index;
        int si = stream_maps[i].stream_index;
        
        if (fi < 0 || fi > nb_input_files - 1 ||
            si < 0 || si > file_table[fi].nb_streams - 1) {
            fprintf(stderr,"Could not find input stream #%d.%d\n", fi, si);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        fi = stream_maps[i].sync_file_index;
        si = stream_maps[i].sync_stream_index;
        if (fi < 0 || fi > nb_input_files - 1 ||
            si < 0 || si > file_table[fi].nb_streams - 1) {
            fprintf(stderr,"Could not find sync stream #%d.%d\n", fi, si);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    
    ost_table = av_mallocz(sizeof(AVOutputStream *) * nb_ostreams);
    if (!ost_table)
        goto fail;
    n = 0;
    for(k=0;k<nb_output_files;k++) {
        os = output_files[k];
        for(i=0;i<os->nb_streams;i++,n++) {
            int found;
            ost = ost_table[n] = output_streams_for_file[k][i];
            ost->st = os->streams[i];
            if (nb_stream_maps > 0) {
                ost->source_index = file_table[stream_maps[n].file_index].ist_index + stream_maps[n].stream_index;
                //fprintf(stderr, "lala %i: %i, %i, %i\n", n, file_table[stream_maps[n].file_index].ist_index, stream_maps[n].file_index, stream_maps[n].stream_index);
                //ost->source_index = (i+1)%2;
                
                /* Sanity check that the stream types match */
                if (ist_table[ost->source_index]->st->codec->codec_type != ost->st->codec->codec_type) {
                    int i= ost->file_index;
                    dump_format(output_files[i], i, output_files[i]->filename, 1);
                    fprintf(stderr, "Codec type mismatch for mapping #%d.%d -> #%d.%d\n",
                            stream_maps[n].file_index, stream_maps[n].stream_index,
                            ost->file_index, ost->index);
                    ffmpeg_exit(1);
                }
                
            } else {
                int best_nb_frames=-1;
                /* get corresponding input stream index : we select the first one with the right type */
                found = 0;
                for(j=0;j<nb_istreams;j++) {
                    int skip=0;
                    ist = ist_table[j];
                    if (ist->discard && ist->st->discard != AVDISCARD_ALL && !skip &&
                        ist->st->codec->codec_type == ost->st->codec->codec_type) {
                        if(best_nb_frames < ist->st->codec_info_nb_frames){
                            best_nb_frames= ist->st->codec_info_nb_frames;
                            ost->source_index = j;
                            found = 1;
                        }
                    }
                }
                
                if (!found) {
                    /* try again and reuse existing stream */
                    for(j=0;j<nb_istreams;j++) {
                        ist = ist_table[j];
                        if (   ist->st->codec->codec_type == ost->st->codec->codec_type
                            && ist->st->discard != AVDISCARD_ALL) {
                            ost->source_index = j;
                            found = 1;
                        }
                    }
                    if (!found) {
                        int i= ost->file_index;
                        dump_format(output_files[i], i, output_files[i]->filename, 1);
                        fprintf(stderr, "Could not find input stream matching output stream #%d.%d\n",
                                ost->file_index, ost->index);
                        return -1;
                    }
                }
            }
            ist = ist_table[ost->source_index];
            ist->discard = 0;
            ost->sync_ist = (nb_stream_maps > 0) ?
            ist_table[file_table[stream_maps[n].sync_file_index].ist_index +
                      stream_maps[n].sync_stream_index] : ist;
        }
    }
    
    /* for each output stream, we compute the right encoding parameters */
    for(i=0;i<nb_ostreams;i++) {
        AVMetadataTag *lang;
        
        ost = ost_table[i];
        os = output_files[ost->file_index];
        ist = ist_table[ost->source_index];
        
        codec = ost->st->codec;
        icodec = ist->st->codec;
        
        if ((lang=av_metadata_get(ist->st->metadata, "language", NULL, 0))
            &&   !av_metadata_get(ost->st->metadata, "language", NULL, 0))
            av_metadata_set2(&ost->st->metadata, "language", lang->value, 0);
        
        ost->st->disposition = ist->st->disposition;
        codec->bits_per_raw_sample= icodec->bits_per_raw_sample;
        codec->chroma_sample_location = icodec->chroma_sample_location;
        
        if (ost->st->stream_copy) {
            uint64_t extra_size = (uint64_t)icodec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE;
            
            if (extra_size > INT_MAX)
                goto fail;
            
            /* if stream_copy is selected, no need to decode or encode */
            codec->codec_id = icodec->codec_id;
            codec->codec_type = icodec->codec_type;
            
            if(!codec->codec_tag){
                if(   !os->oformat->codec_tag
                   || av_codec_get_id (os->oformat->codec_tag, icodec->codec_tag) == codec->codec_id
                   || av_codec_get_tag(os->oformat->codec_tag, icodec->codec_id) <= 0)
                    codec->codec_tag = icodec->codec_tag;
            }
            
            codec->bit_rate = icodec->bit_rate;
            codec->rc_max_rate    = icodec->rc_max_rate;
            codec->rc_buffer_size = icodec->rc_buffer_size;
            codec->extradata= av_mallocz(extra_size);
            if (!codec->extradata)
                goto fail;
            memcpy(codec->extradata, icodec->extradata, icodec->extradata_size);
            codec->extradata_size= icodec->extradata_size;
            if(av_q2d(icodec->time_base)*icodec->ticks_per_frame > av_q2d(ist->st->time_base) && av_q2d(ist->st->time_base) < 1.0/1000){
                codec->time_base = icodec->time_base;
                codec->time_base.num *= icodec->ticks_per_frame;
                av_reduce(&codec->time_base.num, &codec->time_base.den,
                          codec->time_base.num, codec->time_base.den, INT_MAX);
            }else
                codec->time_base = ist->st->time_base;
            switch(codec->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if(audio_volume != 256) {
                        fprintf(stderr,"-acodec copy and -vol are incompatible (frames are not decoded)\n");
                        ffmpeg_exit(1);
                    }
                    codec->channel_layout = icodec->channel_layout;
                    codec->sample_rate = icodec->sample_rate;
                    codec->channels = icodec->channels;
                    codec->frame_size = icodec->frame_size;
                    codec->block_align= icodec->block_align;
                    if(codec->block_align == 1 && codec->codec_id == CODEC_ID_MP3)
                        codec->block_align= 0;
                    if(codec->codec_id == CODEC_ID_AC3)
                        codec->block_align= 0;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    codec->pix_fmt = icodec->pix_fmt;
                    codec->width = icodec->width;
                    codec->height = icodec->height;
                    codec->has_b_frames = icodec->has_b_frames;
                    codec->block_align = icodec->block_align;
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    codec->width = icodec->width;
                    codec->height = icodec->height;
                    break;
                default:
                    abort();
            }
        } else {
            switch(codec->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    ost->fifo= av_fifo_alloc(1024);
                    if(!ost->fifo)
                        goto fail;
                    ost->reformat_pair = MAKE_SFMT_PAIR(AV_SAMPLE_FMT_NONE,AV_SAMPLE_FMT_NONE);
                    ost->audio_resample = 1;
                    icodec->request_channels = codec->channels;
                    ist->decoding_needed = 1;
                    ost->encoding_needed = 1;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    if (ost->st->codec->pix_fmt == PIX_FMT_NONE) {
                        fprintf(stderr, "Video pixel format is unknown, stream cannot be encoded\n");
                        ffmpeg_exit(1);
                    }
                    ost->video_resample = (codec->width != icodec->width   ||
                                           codec->height != icodec->height ||
                                           (codec->pix_fmt != icodec->pix_fmt));
                    if (ost->video_resample) {
                        avcodec_get_frame_defaults(&ost->pict_tmp);
                        if(avpicture_alloc((AVPicture*)&ost->pict_tmp, codec->pix_fmt,
                                           codec->width, codec->height)) {
                            fprintf(stderr, "Cannot allocate temp picture, check pix fmt\n");
                            ffmpeg_exit(1);
                        }
                        ost->img_resample_ctx = sws_getContext(
                                                               icodec->width,
                                                               icodec->height,
                                                               icodec->pix_fmt,
                                                               codec->width,
                                                               codec->height,
                                                               codec->pix_fmt,
                                                               globalTranscodeContext.stc_settings.sts_output_quality > QUALITY_MID_HIGH ? SWS_LANCZOS : SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        if (ost->img_resample_ctx == NULL) {
                            ost->img_resample_ctx = sws_getContext(
                                                                   icodec->width,
                                                                   icodec->height,
                                                                   icodec->pix_fmt,
                                                                   codec->width,
                                                                   codec->height,
                                                                   codec->pix_fmt,
                                                                   SWS_BILINEAR, NULL, NULL, NULL);
                            if (ost->img_resample_ctx == NULL) {
                                fprintf(stderr, "Cannot get resampling context for %d, %d, %d, %d, %d, %d\n",
                                        icodec->width, icodec->height, icodec->pix_fmt,
                                        codec->width, codec->height, codec->pix_fmt);
                                return -1;
                            }
                        }
                        
#if !CONFIG_AVFILTER
                        ost->original_height = icodec->height;
                        ost->original_width  = icodec->width;
#endif
                        codec->bits_per_raw_sample= 0;
                    }
                    ost->resample_height = icodec->height;
                    ost->resample_width  = icodec->width;
                    ost->resample_pix_fmt= icodec->pix_fmt;
                    ost->encoding_needed = 1;
                    ist->decoding_needed = 1;
                    
#if CONFIG_AVFILTER
                    if (configure_filters(ist, ist, ost)) {
                        fprintf(stderr, "Error opening filters!\n");
                        ffmpeg_exit(1);
                    }
#endif
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    ost->encoding_needed = 0;
                    ist->decoding_needed = 0;
                    break;
                default:
                    abort();
                    break;
            }
        }
        if(codec->codec_type == AVMEDIA_TYPE_VIDEO){
            int size= codec->width * codec->height;
            bit_buffer_size= FFMAX(bit_buffer_size, 6*size + 200);
        }
    }
    
    if (!bit_buffer)
        bit_buffer = av_malloc(bit_buffer_size);
    if (!bit_buffer) {
        fprintf(stderr, "Cannot allocate %d bytes output buffer\n",
                bit_buffer_size);
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    
    /* open each encoder */
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        if (ost->encoding_needed) {
            AVCodec *codec = i < nb_output_codecs ? output_codecs[i] : NULL;
            AVCodecContext *dec = ist_table[ost->source_index]->st->codec;
            if (!codec)
                codec = avcodec_find_encoder(ost->st->codec->codec_id);
            if (!codec) {
                snprintf(error, sizeof(error), "Encoder (codec id %d) not found for output stream #%d.%d",
                         ost->st->codec->codec_id, ost->file_index, ost->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            if (dec->subtitle_header) {
                ost->st->codec->subtitle_header = av_malloc(dec->subtitle_header_size);
                if (!ost->st->codec->subtitle_header) {
                    ret = AVERROR(ENOMEM);
                    goto dump_format;
                }
                memcpy(ost->st->codec->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
                ost->st->codec->subtitle_header_size = dec->subtitle_header_size;
            }
            if (avcodec_open(ost->st->codec, codec) < 0) {
                snprintf(error, sizeof(error), "Error while opening encoder for output stream #%d.%d - maybe incorrect parameters such as bit_rate, rate, width or height",
                         ost->file_index, ost->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            extra_size += ost->st->codec->extradata_size;
        }
    }

    {
        AVFormatContext* ic = output_files[0];
        AVStream *st = ic->streams[0];
        fprintf(stdout, "Resolution: %dx%d\n", st->codec->width, st->codec->height);
        fflush(stdout);
    }
    
    /* open each decoder */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            AVCodec *codec = i < nb_input_codecs ? input_codecs[i] : NULL;
            if (!codec)
                codec = avcodec_find_decoder(ist->st->codec->codec_id);
            if (!codec) {
                snprintf(error, sizeof(error), "Decoder (codec id %d) not found for input stream #%d.%d",
                         ist->st->codec->codec_id, ist->file_index, ist->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            if (avcodec_open(ist->st->codec, codec) < 0) {
                snprintf(error, sizeof(error), "Error while opening decoder for input stream #%d.%d",
                         ist->file_index, ist->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            //if (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            //    ist->st->codec->flags |= CODEC_FLAG_REPEAT_FIELD;
        }
    }
    
    if (globalTranscodeContext.stc_inlineass_context)
    {
        if (globalTranscodeContext.stc_settings.sts_album_art_mode == 1)
        {
            int isDirname;
            int isFilename;
            char *artist = newArtistForFile(globalTranscodeContext.stc_input_files[0], &isDirname);
            char *title = newTitleForFile(globalTranscodeContext.stc_input_files[0], &isFilename);
            
            char *subtitle;
            int length;
            
            if (isDirname)
            {
                length = asprintf(&subtitle, "%s//%s", artist, title);
            }
            else
            {
                length = asprintf(&subtitle, "%s\n%s", artist, title);
            }
            
            vf_inlineass_append_data(
                                     globalTranscodeContext.stc_inlineass_context,
                                     0,
                                     subtitle,
                                     length,
                                     globalTranscodeContext.stc_settings.sts_output_duration,
                                     0,
                                     AV_TIME_BASE_Q);
            
            av_free(subtitle);
            av_free(artist);
            av_free(title);
        }
        else
        {
            for(i=0;i<globalTranscodeContext.stc_nb_input_files;i++) {
                is = globalTranscodeContext.stc_input_files[i];
                for(k=0;k<is->nb_streams;k++) {
                    if (i == 0 && k == globalTranscodeContext.stc_settings.sts_subtitle_stream_index &&
                        is->streams[k]->codec->extradata_size)
                    {
                        vf_inlineass_append_data(
                                                 globalTranscodeContext.stc_inlineass_context,
                                                 is->streams[k]->codec->codec_id,
                                                 (char *)(is->streams[k]->codec->extradata),
                                                 is->streams[k]->codec->extradata_size,
                                                 -1,
                                                 0,
                                                 is->streams[k]->time_base);
                    }
                }
            }
        }
    }
    
    /* init pts */
    for(i=0;i<nb_istreams;i++) {
        AVStream *st;
        ist = ist_table[i];
        st= ist->st;
        
        if (st->avg_frame_rate.den != 0)
          ist->pts = st->avg_frame_rate.num ? - st->codec->has_b_frames*AV_TIME_BASE / av_q2d(st->avg_frame_rate) : 0;
        else
          ist->pts = - st->codec->has_b_frames*AV_TIME_BASE / av_q2d(st->r_frame_rate);
        
        ist->next_pts = AV_NOPTS_VALUE;
        init_pts_correction(&ist->pts_ctx);
        ist->is_start = 1;
    }
    
    /* set meta data information from input file if required */
    for (i=0;i<nb_meta_data_maps;i++) {
        AVFormatContext *files[2];
        AVMetadata      **meta[2];
        AVMetadataTag *mtag;
        int j;
        
#define METADATA_CHECK_INDEX(index, nb_elems, desc)\
if ((index) < 0 || (index) >= (nb_elems)) {\
snprintf(error, sizeof(error), "Invalid %s index %d while processing metadata maps\n",\
(desc), (index));\
ret = AVERROR(EINVAL);\
goto dump_format;\
}
        
        int out_file_index = meta_data_maps[i][0].file;
        int in_file_index = meta_data_maps[i][1].file;
        if (in_file_index < 0 || out_file_index < 0)
            continue;
        METADATA_CHECK_INDEX(out_file_index, nb_output_files, "output file")
        METADATA_CHECK_INDEX(in_file_index, nb_input_files, "input file")
        
        files[0] = output_files[out_file_index];
        files[1] = input_files[in_file_index];
        
        for (j = 0; j < 2; j++) {
            AVMetaDataMap *map = &meta_data_maps[i][j];
            
            switch (map->type) {
                case 'g':
                    meta[j] = &files[j]->metadata;
                    break;
                case 's':
                    METADATA_CHECK_INDEX(map->index, files[j]->nb_streams, "stream")
                    meta[j] = &files[j]->streams[map->index]->metadata;
                    break;
                case 'c':
                    METADATA_CHECK_INDEX(map->index, files[j]->nb_chapters, "chapter")
                    meta[j] = &files[j]->chapters[map->index]->metadata;
                    break;
                case 'p':
                    METADATA_CHECK_INDEX(map->index, files[j]->nb_programs, "program")
                    meta[j] = &files[j]->programs[map->index]->metadata;
                    break;
            }
        }
        
        mtag=NULL;
        while((mtag=av_metadata_get(*meta[1], "", mtag, AV_METADATA_IGNORE_SUFFIX)))
            av_metadata_set2(meta[0], mtag->key, mtag->value, AV_METADATA_DONT_OVERWRITE);
    }
    
    /* copy chapters according to chapter maps */
    for (i = 0; i < nb_chapter_maps; i++) {
        int infile  = chapter_maps[i].in_file;
        int outfile = chapter_maps[i].out_file;
        
        if (infile < 0 || outfile < 0)
            continue;
        if (infile >= nb_input_files) {
            snprintf(error, sizeof(error), "Invalid input file index %d in chapter mapping.\n", infile);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }
        if (outfile >= nb_output_files) {
            snprintf(error, sizeof(error), "Invalid output file index %d in chapter mapping.\n",outfile);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }
        copy_chapters(infile, outfile);
    }
    
    /* copy chapters from the first input file that has them*/
    if (!nb_chapter_maps)
        for (i = 0; i < nb_input_files; i++) {
            if (!input_files[i]->nb_chapters)
                continue;
            
            for (j = 0; j < nb_output_files; j++)
                if ((ret = copy_chapters(i, j)) < 0)
                    goto dump_format;
            break;
        }

    /* PLEX: init bitstream filters */
    for (i = 0; i < nb_ostreams; i++) {
      AVStream *st = ost_table[i]->st;
      if (st->stream_copy) {
        AVBitStreamFilterContext *bsfc = ost_table[i]->bitstream_filters;
        while (bsfc) {
          uint8_t *obuf = 0;
          int osize = 0;
          bsfc->filter->filter(bsfc, st->codec, bsfc->args, &obuf, &osize, 0, 0, 0);
          bsfc = bsfc->next;
        }
      }
    }
    
    /* open files and write file headers */
    for(i=0;i<nb_output_files;i++) {
        os = output_files[i];
        if (av_write_header(os) < 0) {
            snprintf(error, sizeof(error), "Could not write header for output file #%d (incorrect codec parameters ?)", i);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }
        if (strcmp(output_files[i]->oformat->name, "rtp")) {
            want_sdp = 0;
        }
    }
    
dump_format:
    /* dump the file output parameters - cannot be done before in case
     of stream copy */
    for(i=0;i<nb_output_files;i++) {
        dump_format(output_files[i], i, output_files[i]->filename, 1);
    }
    
    /* dump the stream mapping */
    if (verbose >= 0) {
        fprintf(stderr, "Stream mapping:\n");
        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            fprintf(stderr, "  Stream #%d.%d -> #%d.%d",
                    ist_table[ost->source_index]->file_index,
                    ist_table[ost->source_index]->index,
                    ost->file_index,
                    ost->index);
            if (ost->sync_ist != ist_table[ost->source_index])
                fprintf(stderr, " [sync #%d.%d]",
                        ost->sync_ist->file_index,
                        ost->sync_ist->index);
            fprintf(stderr, "\n");
        }
    }
    
    if (ret) {
        fprintf(stderr, "%s\n", error);
        goto fail;
    }
    
    if (want_sdp) {
        print_sdp(output_files, nb_output_files);
    }
    
    timer_start = av_gettime();
    
    PMS_Log(LOG_LEVEL_DEBUG, "The verbose setting is %d and PLEX_DEBUG is %d.", verbose, PLEX_DEBUG); 
    if (verbose == 0 && PLEX_DEBUG==0)
    {
        // We need to close these as we're not reading on the other end of the pipe and it can get clogged.
        fprintf(stderr, "Closing down stderr.\n");
        fflush(stderr);

#ifdef __WIN32__
        freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stderr);
#endif
    }
    
    for(;;) {
        int file_index, ist_index;
        AVPacket pkt;
        double ipts_min;
        double opts_min;
        
    redo:
        ipts_min= 1e100;
        opts_min= 1e100;
        
        /* select the stream that we must read now by looking at the
         smallest output pts */
        file_index = -1;
        for(i=0;i<nb_ostreams;i++) {
            double ipts, opts;
            ost = ost_table[i];
            os = output_files[ost->file_index];
            ist = ist_table[ost->source_index];
            if(ist->is_past_recording_time || no_packet[ist->file_index])
                continue;
            opts = ost->st->pts.val * av_q2d(ost->st->time_base);
            ipts = (double)ist->pts;
            if (!file_table[ist->file_index].eof_reached){
                if(ipts < ipts_min) {
                    ipts_min = ipts;
                    file_index = ist->file_index;
                }
                if(opts < opts_min) {
                    opts_min = opts;
                }
            }
            // Limit to a single frame for thumbnails
            if(ost->st->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
               ost->frame_number >= (globalTranscodeContext.stc_settings.sts_output_single_frame_only ? 1 : INT_MAX)){
                file_index= -1;
                break;
            }
        }
        /* if none, if is finished */
        if (file_index < 0) {
            if(no_packet_count){
                no_packet_count=0;
                memset(no_packet, 0, sizeof(no_packet));
                usleep(10000);
                continue;
            }
            break;
        }
        
        /* read a frame from it and output it in the fifo */
        is = input_files[file_index];
        ret= av_read_frame(is, &pkt);
        /*if (pkt.stream_index==2){
         printf("index=%i;\n", pkt.stream_index);
         }*/
        if(ret == AVERROR(EAGAIN)){
            no_packet[file_index]=1;
            no_packet_count++;
            continue;
        }
        if (ret < 0) {
            file_table[file_index].eof_reached = 1;
            continue;
        }
        
        no_packet_count=0;
        memset(no_packet, 0, sizeof(no_packet));
        
        /* the following test is needed in case new streams appear
         dynamically in stream : we ignore them */
        if (pkt.stream_index >= file_table[file_index].nb_streams)
            goto discard_packet;
        ist_index = file_table[file_index].ist_index + pkt.stream_index;
        ist = ist_table[ist_index];
        
        //printf("index:%i; codec:%i==%i, sidx:%i\n", pkt.stream_index, ist->st->codec->codec_type, AVMEDIA_TYPE_SUBTITLE, globalTranscodeContext.stc_settings.sts_subtitle_stream_index);
        
        
        if (ist->discard)
            goto discard_packet;
        
        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts += av_rescale_q(input_files_ts_offset[ist->file_index], AV_TIME_BASE_Q, ist->st->time_base);
        if (pkt.pts != AV_NOPTS_VALUE) {
            int64_t temp;
            temp = av_rescale_q(input_files_ts_offset[ist->file_index], AV_TIME_BASE_Q, ist->st->time_base);
            //temp = av_rescale_q(0, AV_TIME_BASE_Q, ist->st->time_base);
            pkt.pts += temp;
        }
        
        if (pkt.dts != AV_NOPTS_VALUE && ist->next_pts != AV_NOPTS_VALUE
            && (is->iformat->flags & AVFMT_TS_DISCONT)) {
            int64_t pkt_dts= av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);
            int64_t delta= pkt_dts - ist->next_pts;
            if(FFABS(delta) > 1LL*dts_delta_threshold*AV_TIME_BASE || pkt_dts+1<ist->pts){
                input_files_ts_offset[ist->file_index]-= delta;
                if (verbose > 2)
                    fprintf(stderr, "timestamp discontinuity %"PRId64", new offset= %"PRId64"\n", delta, input_files_ts_offset[ist->file_index]);
                pkt.dts-= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
                if(pkt.pts != AV_NOPTS_VALUE)
                    pkt.pts-= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            }
        }
        
        /* finish if recording time exhausted */
        if (recording_time != INT64_MAX &&
            av_compare_ts(pkt.pts, ist->st->time_base, recording_time + zero_start_time, (AVRational){1, 1000000}) >= 0) {
            ist->is_past_recording_time = 1;
            goto discard_packet;
        }
        
        if (globalTranscodeContext.stc_settings.sts_album_art_mode && file_table[0].eof_reached ||
            globalTranscodeContext.stc_settings.sts_still_image_mode && globalTranscodeContext.stc_settings.sts_last_output_pts > is->duration)
        {
            globalTranscodeContext.stc_settings.sts_output_padding = 1;
            int64_t end_ts;
            end_ts = globalTranscodeContext.stc_settings.sts_output_duration;
            if (globalTranscodeContext.stc_settings.sts_last_output_pts >= end_ts)
            {
                fprintf(stderr, "End of stream.\n");
                break;
            }
        }
        
        /*if (pkt.dts != AV_NOPTS_VALUE || pkt.pts != AV_NOPTS_VALUE){
         fprintf(stderr, "next:%"PRId64" dts:%"PRId64" pts:%"PRId64" off:%"PRId64" %d, idx=%d, %"PRId64"\n", 
         ist->next_pts, 
         pkt.dts, 
         pkt.pts,
         input_files_ts_offset[ist->file_index], 
         ist->st->codec->codec_type,
         ist->file_index,
         AV_NOPTS_VALUE);
         }*/
        
        //fprintf(stderr,"read #%d.%d size=%d\n", ist->file_index, ist->index, pkt.size);
        if (output_packet(ist, ist_index, ost_table, nb_ostreams, &pkt) < 0) {
            
            if (verbose >= 0)
                fprintf(stderr, "Error while decoding stream #%d.%d\n",
                        ist->file_index, ist->index);
            av_free_packet(&pkt);
            goto redo;
        }
        
    discard_packet:
        av_free_packet(&pkt);
        
        /* dump report by using the output first video and audio streams */
        print_report(output_files, ost_table, nb_ostreams, 0);
        
        /* Delay if needed. */
        if (globalTranscodeContext.plex.delay > 0)
          usleep(1000*globalTranscodeContext.plex.delay);
    }
    
    /* at the end of stream, we must flush the decoder buffers */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            output_packet(ist, i, ost_table, nb_ostreams, NULL);
        }
    }
    
    
    if (verbose > 0)
    {
        fprintf(stderr, "End of regular packets\n");
    }
    if (!globalTranscodeContext.stc_settings.sts_output_single_frame_only &&
        globalTranscodeContext.stc_settings.sts_output_segment_index == 0 &&
        globalTranscodeContext.stc_settings.sts_last_output_pts < AV_TIME_BASE &&
        globalTranscodeContext.stc_settings.sts_last_output_pts < globalTranscodeContext.stc_settings.sts_output_duration - 0.5 * AV_TIME_BASE)
    {
        fprintf(stderr, "Reached end of file without finding 1 second worth of useable packets\n");
        return -1;
    }
    
    if (!globalTranscodeContext.stc_settings.sts_output_single_frame_only &&
        (video_size > 0 || audio_size > 0))
    {
        AVFormatContext *s = globalTranscodeContext.stc_output_files[0];
        
        int64_t end_ts = globalTranscodeContext.stc_settings.sts_output_duration;
        
        while (globalTranscodeContext.stc_settings.sts_last_output_pts <= end_ts)
        {
            //
            // Generate packets for each of the output streams
            //
            for (i = 0; i < s->nb_streams; i++)
            {
                AVStream *stream = s->streams[i];
                
                AVPacket pkt;
                av_init_packet(&pkt);
                
                //
                // Send a NULL pack that will be picked up by the custom code in mpegtsenc to
                // generate appropriate NULL packets.
                //
                unsigned char data[5] = {0, 0, 0, 1, 0};
                pkt.data = data;
                pkt.size = 5;
                pkt.stream_index = i;
                
                //
                // We need a PCR every 10th of a second, so output at twice that to
                // remain reliable.
                //
                pkt.pts = av_rescale_q(globalTranscodeContext.stc_settings.sts_last_output_pts + 0.05 * AV_TIME_BASE,
                                       AV_TIME_BASE_Q, stream->time_base);
                
                if (s->oformat->write_packet(s, &pkt) != 0)
                {
                    fprintf(stderr, "Failed to write blank packet\n");
                    return -1;
                }
                
                globalTranscodeContext.stc_settings.sts_last_output_pts = av_rescale_q(pkt.pts, stream->time_base, AV_TIME_BASE_Q);
                
                int64_t segment_end = SegmentEnd(&globalTranscodeContext.stc_settings);
                
                if (globalTranscodeContext.stc_settings.sts_last_output_pts >= segment_end &&
                    globalTranscodeContext.stc_settings.sts_last_output_pts <= end_ts)
                {
                    put_flush_packet(s->pb);
                    url_fclose(s->pb);
                    
                    clock_t t = clock();
                    double passedTime = (double)(t - tnow) / CLOCKS_PER_SEC;
                    tnow = t;
                    plex_totalSegmentTime += passedTime;
                    plex_totalSegmentCount++;
                    
                    fprintf(stderr, "Wrote segment %lld (%0.2fs, %0.2fs)\n", globalTranscodeContext.stc_settings.sts_output_segment_index, passedTime, plex_totalSegmentTime/plex_totalSegmentCount);
                    //					fflush(stdout);
                    
                    // Compute the final filename.
                    char path[4096];
                    snprintf(path, 4095, "%s-%05lld.ts", globalTranscodeContext.stc_settings.sts_output_file_base,
                             globalTranscodeContext.stc_settings.sts_output_segment_index);
                    
                    // Move it over.
                    rename(globalTranscodeContext.stc_settings.sts_output_file_path, path);
                    
                    globalTranscodeContext.stc_settings.sts_output_segment_index++;
                    UpdateFilePathForSegment(&globalTranscodeContext.stc_settings);
                    
                    if (url_fopen(&s->pb, globalTranscodeContext.stc_settings.sts_output_file_path, URL_WRONLY) < 0)
                    {
                        fprintf(stderr, "Couldn't open '%s'\n", globalTranscodeContext.stc_settings.sts_output_file_path);
                        s->pb = NULL;
                        return -1;
                    }
                }
            }
        }
    }
    
    if (!globalTranscodeContext.stc_settings.sts_output_single_frame_only &&
        globalTranscodeContext.stc_settings.sts_last_output_pts != 0)
    {
        clock_t t = clock();
        double passedTime = (double)(t - tnow) / CLOCKS_PER_SEC;
        tnow = t;
        plex_totalSegmentTime += passedTime;
        plex_totalSegmentCount++;
        
        fprintf(stderr, "Wrote segment %lld (%0.2fs, %0.2fs)\n", globalTranscodeContext.stc_settings.sts_output_segment_index, passedTime, plex_totalSegmentTime/plex_totalSegmentCount);
        //fflush(stdout);
    }		
    
    /* write the trailer if needed and close file */
    for(i=0;i<nb_output_files;i++) {
        os = output_files[i];
        av_write_trailer(os);
    }
    
    /* dump report by using the first video and audio streams */
    print_report(output_files, ost_table, nb_ostreams, 1);
    
    /* close each encoder */
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        if (ost->encoding_needed) {
            av_freep(&ost->st->codec->stats_in);
            avcodec_close(ost->st->codec);
        }
    }
    
    /* close each decoder */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->st->codec);
        }
    }
    
    
    /* Get out early, avoids crash in avfilter_graph_close() */
    return 0;
    
#if CONFIG_AVFILTER
    if (graph) {
        avfilter_graph_free(graph);
        av_freep(&graph);
    }
#endif
    
    /* finished ! */
    ret = 0;
    
fail:
    av_freep(&bit_buffer);
    av_free(file_table);
    
    if (ist_table) {
        for(i=0;i<nb_istreams;i++) {
            ist = ist_table[i];
            av_free(ist);
        }
        av_free(ist_table);
    }
    if (ost_table) {
        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            if (ost) {
                if (ost->st->stream_copy)
                    av_freep(&ost->st->codec->extradata);
                if (ost->logfile) {
                    fclose(ost->logfile);
                    ost->logfile = NULL;
                }
                av_fifo_free(ost->fifo); /* works even if fifo is not
                                          initialized but set to zero */
                av_freep(&ost->st->codec->subtitle_header);
                av_free(ost->pict_tmp.data[0]);
                av_free(ost->forced_kf_pts);
                if (ost->video_resample)
                    sws_freeContext(ost->img_resample_ctx);
                if (ost->resample)
                    audio_resample_close(ost->resample);
                if (ost->reformat_ctx)
                    av_audio_convert_free(ost->reformat_ctx);
                av_free(ost);
            }
        }
        av_free(ost_table);
    }
    return ret;
}

static enum CodecID find_codec_or_die(const char *name, int type, int encoder)
{
    const char *codec_string = encoder ? "encoder" : "decoder";
    AVCodec *codec;
    
    if(!name)
        return CODEC_ID_NONE;
    codec = encoder ?
    avcodec_find_encoder_by_name(name) :
    avcodec_find_decoder_by_name(name);
    if(!codec) {
        fprintf(stderr, "Unknown %s '%s'\n", codec_string, name);
        ffmpeg_exit(1);
    }
    if(codec->type != type) {
        fprintf(stderr, "Invalid %s type '%s'\n", codec_string, name);
        ffmpeg_exit(1);
    }
    return codec->id;
}

static const char *pathExtension(const char *filename)
{
    int i;
    for (i = strlen(filename) - 1; i >= 0; i--)
    {
        if (filename[i] == '.')
        {
            break;
        }
    }
    if (i <= 0)
    {
        return "";
    }
    return &filename[i + 1];
}

static int configureInputFiles(STMTranscodeContext *context, float photoDuration)
{
    const char *filename = context->stc_settings.sts_input_file_name;
    AVFormatParameters params, *ap = &params;
    AVInputFormat *file_iformat = NULL;
    int err, i, ret, rfps, rfps_base;
    int64_t timestamp;
    
    if (verbose > 1)
        fprintf(stderr, "Configuring input file.\n");
    
    /* get default parameters from command line */
    AddNewInputFile(context, avformat_alloc_context(), 0);
    
    AVFormatContext *ic = context->stc_input_files[context->stc_nb_input_files - 1];
    
    if (!ic) {
        return -1;
    }
    
    memset(ap, 0, sizeof(*ap));
    ap->prealloced_context = 1;
    
    ic->video_codec_id   = CODEC_ID_NONE;
    ic->audio_codec_id   = CODEC_ID_NONE;
    ic->subtitle_codec_id= CODEC_ID_NONE;
    ic->flags |= AVFMT_FLAG_NONBLOCK;
    
    /* open the input file with generic libav function */
    if (verbose > 0) fprintf(stderr, "Opening input file.\n");
    err = av_open_input_file(&ic, filename, file_iformat, 0, ap);
    if (err < 0) {
        fprintf(stderr, "Unable to open file at path\n%s\n", filename);
        av_free(ic);
        context->stc_nb_input_files--;
        return -1;
    }
    
    ic->loop_input = context->stc_settings.sts_album_art_mode;
    
    if (verbose > 0) fprintf(stderr, "Finding stream information.\n");
    ret = av_find_stream_info(ic);
    if (ret < 0 && verbose >= 0) {
        fprintf(stderr, "%s: could not find codec parameters\n", filename);
        return -1;
    }
    
    if (strcmp(ic->iformat->name, "image2") == 0)
    {
        context->stc_settings.sts_still_image_mode = 1;
    }
    
    int video_stream_index = -1;
    int audio_stream_index = -1;
    int subtitle_stream_index = -1;
    
    /* update the current parameters so that they match the one of the input stream */
    for(i=0;i<ic->nb_streams;i++) {
        AVStream *st = ic->streams[i];
        AVCodecContext *enc = st->codec;
        
        switch(enc->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (context->stc_settings.sts_subtitle_stream_index != -1 ||
                    st->nb_frames > 0 && st->nb_frames <= 50)
                {
                    AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
                    st->discard = AVDISCARD_ALL;
                    break;
                }
                
                avcodec_thread_init(enc, threadCount());
                //fprintf(stderr, "\nInput Audio channels: %d", enc->channels);
                context->stc_settings.sts_source_audio_channel_layout = enc->channel_layout;
                AddNewInputCodec(context, avcodec_find_decoder_by_name(context->stc_settings.sts_audio_codec_name));
                
                if (audio_stream_index == -1) audio_stream_index = i;
                break;
            case AVMEDIA_TYPE_VIDEO:
                if (context->stc_settings.sts_subtitle_stream_index != -1 ||
                    video_stream_index != -1 ||
                    context->stc_settings.sts_suppress_video ||
                    (st->nb_frames != 0 && st->nb_frames < 2 &&
                     !context->stc_settings.sts_still_image_mode &&
                     !context->stc_settings.sts_album_art_mode))
                {
                    AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
                    st->discard = AVDISCARD_ALL;
                    break;
                }
                
                if (enc->codec_id != CODEC_ID_SVQ3 &&
                    enc->codec_id != CODEC_ID_SVQ3)
                {
                    avcodec_thread_init(enc, threadCount());
                }
                else
                {
                    avcodec_thread_init(enc, 1);
                }
                
                if (context->stc_settings.sts_album_art_mode ||
                    context->stc_settings.sts_still_image_mode)
                {
                    //
                    // Set a flag so that we can enable "single frame" mode in the
                    // image decoders
                    //
                    enc->flags |= CODEC_FLAG2_NO_OUTPUT;
                }
                
                if (enc->width == 0 || enc->height == 0 || enc->width > 65535 || enc->height > 65535)
                {
                    if (verbose > 0)
                    {
                        fprintf(stderr,"Invalid frame size detected\n");
                    }
                    AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
                    st->discard = AVDISCARD_ALL;
                    continue;
                }
                
                context->stc_settings.sts_source_frame_width = enc->width;
                context->stc_settings.sts_source_frame_height = enc->height;
                
                if(st->sample_aspect_ratio.num)
                {
                    context->stc_settings.sts_source_video_aspect_ratio=av_q2d(st->sample_aspect_ratio);
                }
                else
                {
                    context->stc_settings.sts_source_video_aspect_ratio=av_q2d(enc->sample_aspect_ratio);
                    if (context->stc_settings.sts_source_video_aspect_ratio == 0)
                    {
                        context->stc_settings.sts_source_video_aspect_ratio = 1.0;
                    }
                }
                context->stc_settings.sts_source_video_aspect_ratio *= (float) enc->width / enc->height;
                fprintf(stderr, "Source AR: %f\n", context->stc_settings.sts_source_video_aspect_ratio);
                
                if (enc->ticks_per_frame == 2)
                {
                    fprintf(stderr,"\nDetected interlacing.\n");
                    context->stc_settings.sts_source_video_interlaced = 1;
                }
                if (enc->time_base.den != st->r_frame_rate.num * enc->ticks_per_frame ||
                    enc->time_base.num != st->r_frame_rate.den) {
                    
                    float codec_rate = (float)enc->time_base.den / enc->time_base.num;
                    float container_rate = (float)st->r_frame_rate.num / st->r_frame_rate.den;
                    
                    if (verbose >= 0)
                        fprintf(stderr,"\nSeems stream %d codec frame rate differs from container frame rate: %2.2f (%d/%d) -> %2.2f (%d/%d)\n",
                                i, codec_rate, enc->time_base.den, enc->time_base.num,
                                container_rate, rfps, rfps_base);
                }
                
                context->stc_settings.sts_source_video_frame_rate.num = ic->streams[i]->r_frame_rate.num;
                context->stc_settings.sts_source_video_frame_rate.den = ic->streams[i]->r_frame_rate.den;
                
                AddNewInputCodec(context, avcodec_find_decoder_by_name(context->stc_settings.sts_video_codec_name));
                
                AVStreamMap video_map;
                video_map.file_index = context->stc_nb_input_files - 1;
                video_map.stream_index = i;
                video_map.sync_file_index = context->stc_nb_input_files - 1;
                video_map.sync_stream_index = i;
                AddNewStreamMap(context, video_map);
                
                video_stream_index = i;
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
                if (context->stc_settings.sts_subtitle_stream_index != i)
                {
                    st->discard = AVDISCARD_ALL;
                    break;
                }
                AVStreamMap subtitle_map;
                subtitle_map.file_index = context->stc_nb_input_files - 1;
                subtitle_map.stream_index = i;
                subtitle_map.sync_file_index = context->stc_nb_input_files - 1;
                subtitle_map.sync_stream_index = i;
                AddNewStreamMap(context, subtitle_map);
                
                subtitle_stream_index = i;
                break;
            case AVMEDIA_TYPE_DATA:
            case AVMEDIA_TYPE_ATTACHMENT:
            case AVMEDIA_TYPE_UNKNOWN:
                AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
                st->discard = AVDISCARD_ALL;
                break;
            default:
                abort();
        }
    }
    
    if (video_stream_index == -1 && audio_stream_index == -1 && subtitle_stream_index == -1)
    {
        fprintf(stderr, "%s: did not find a valid stream\n", filename);
        return -1;
    }
    
    if (audio_stream_index >= 0)
    {
        int use_stream = (video_stream_index == -1 || (audio_stream_index >= 0 && ic->streams[audio_stream_index]->duration != AV_NOPTS_VALUE)) ?
        audio_stream_index : video_stream_index;
        
        //PLEX: using audio returned the wrong duration for some media, so we make sure we use the longer duration (if both are available)
        if (video_stream_index >= 0 && audio_stream_index >= 0){
            int64_t vid_stream_duration = av_rescale_q(ic->streams[video_stream_index]->duration, ic->streams[video_stream_index]->time_base, AV_TIME_BASE_Q);
            int64_t aud_stream_duration = av_rescale_q(ic->streams[audio_stream_index]->duration, ic->streams[audio_stream_index]->time_base, AV_TIME_BASE_Q);
            if (video_stream_index>=0 && audio_stream_index>=0){
                if (aud_stream_duration < vid_stream_duration && ic->streams[video_stream_index]->duration != AV_NOPTS_VALUE){
                    use_stream = video_stream_index;
                }
            }
        }
        //PLEX
        
        int64_t stream_duration =
        av_rescale_q(ic->streams[use_stream]->duration, ic->streams[use_stream]->time_base, AV_TIME_BASE_Q);
        int64_t stream_start_time =
        av_rescale_q(ic->streams[use_stream]->start_time, ic->streams[use_stream]->time_base, AV_TIME_BASE_Q);
        
        const int64_t MaxSaneDuration = 10LL * 3600LL * AV_TIME_BASE;
        if (stream_duration > 0 && stream_duration < MaxSaneDuration)
        {
            //fprintf(stderr, "Updating Duration (stream %i) from %lld to %lld (%lld) (tb=%i/%i)\n", use_stream, ic->duration , stream_duration, ic->streams[use_stream]->duration, ic->streams[use_stream]->time_base.num, ic->streams[use_stream]->time_base.den);
            ic->duration = stream_duration;
            ic->start_time = stream_start_time;
        }
        else if (ic->duration > MaxSaneDuration)
        {
            fprintf(stderr, "%s: insane duration (%d)\n", filename, ic->duration);
            if (strcmp(filename, "pipe:")!=0) return -1;
        }
    }
    else if (context->stc_settings.sts_still_image_mode)
    {
        const int64_t StillImageDuration = photoDuration * AV_TIME_BASE;
        ic->duration = StillImageDuration;
        ic->loop_input = 1;
    }
    
    if (context->stc_settings.sts_output_single_frame_only && ic->duration > AV_TIME_BASE)
    {
        if (ic->duration > 6 * 60 * AV_TIME_BASE)
        {
            context->stc_settings.sts_input_start_time = 3 * 60 * AV_TIME_BASE;
        }
        else
        {
            context->stc_settings.sts_input_start_time = ic->duration >> 1;
        }
    }
    
    timestamp = context->stc_settings.sts_input_start_time;
    
    if(strcmp(ic->iformat->name, "mpegts")==0 && strncasecmp(pathExtension(filename), "mpg", 3) == 0)
    {
        //
        // Workaround hack for EyeTV issues -- skip the first 128k of the file.
        //
        int default_index = av_find_default_stream_index(ic);
        av_seek_frame(ic, default_index, 128 * 1024, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_FRAME);
        AVPacket pkt;
        while (av_read_packet(ic, &pkt) >= 0 && pkt.pts == AV_NOPTS_VALUE)
        {
            // do nothing
        }
        if (pkt.pts != AV_NOPTS_VALUE)
        {
            int64_t start_time = 0;
            if (ic->start_time != AV_NOPTS_VALUE && ic->start_time > 0)
            {
                start_time = ic->start_time;
            }
            
            int64_t excess = 
            pkt.pts * AV_TIME_BASE  *
            ic->streams[default_index]->time_base.num
            / ic->streams[default_index]->time_base.den;
            timestamp += excess;
            ic->duration -= excess - start_time;
        }
    }
    
    /* add the stream start time */
    else if (ic->start_time != AV_NOPTS_VALUE && ic->start_time > 0)
    {
        timestamp += ic->start_time;
    }
    
    /* if seeking requested, we execute it */
    if (timestamp != 0) {
        int64_t stream_timestamp = timestamp;
        int stream_index = -1;
        if (!context->stc_settings.sts_album_art_mode)
        {
            stream_index = video_stream_index;
        }
        if (stream_index == -1)
        {
            stream_index = audio_stream_index;
        }
        if (stream_index != -1)
        {
            stream_timestamp =
            av_rescale(timestamp,
                       ic->streams[stream_index]->time_base.den,
                       AV_TIME_BASE * (int64_t)ic->streams[stream_index]->time_base.num);
        }
        
        ret = av_seek_frame(ic, stream_index, stream_timestamp,
                            context->stc_settings.sts_output_single_frame_only ? 0 : AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            fprintf(stderr, "%s: could not seek to position %0.3f\n",
                    filename, (double)timestamp / AV_TIME_BASE);
        }
    }
    
    context->stc_input_files_ts_offset[context->stc_nb_input_files - 1] = -timestamp;
    
    if (verbose >= 0) {
        dump_format(ic, context->stc_nb_input_files - 1, filename, 0);
    }
    
    return 0;
}


static void check_audio_video_sub_inputs(int *has_video_ptr, int *has_audio_ptr,
                                         int *has_subtitle_ptr)
{
    AVFormatContext **input_files = globalTranscodeContext.stc_input_files;
    int nb_input_files = globalTranscodeContext.stc_nb_input_files;
    
    int has_video, has_audio, has_subtitle, i, j;
    AVFormatContext *ic;
    
    has_video = 0;
    has_audio = 0;
    has_subtitle = 0;
    for(j=0;j<nb_input_files;j++) {
        ic = input_files[j];
        for(i=0;i<ic->nb_streams;i++) {
            AVCodecContext *enc = ic->streams[i]->codec;
            switch(enc->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    has_audio = 1;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    has_video = 1;
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    has_subtitle = 1;
                    break;
                case AVMEDIA_TYPE_DATA:
                case AVMEDIA_TYPE_ATTACHMENT:
                case AVMEDIA_TYPE_UNKNOWN:
                    break;
                default:
                    abort();
            }
        }
    }
    *has_video_ptr = has_video;
    *has_audio_ptr = has_audio;
    *has_subtitle_ptr = has_subtitle;
}

static void choose_pixel_fmt(AVStream *st, AVCodec *codec)
{
    if(codec && codec->pix_fmts){
        const enum PixelFormat *p= codec->pix_fmts;
        for(; *p!=-1; p++){
            if(*p == st->codec->pix_fmt)
                break;
        }
        if(*p == -1
           && !(   st->codec->codec_id==CODEC_ID_MJPEG
                && st->codec->strict_std_compliance <= FF_COMPLIANCE_UNOFFICIAL
                && (   st->codec->pix_fmt == PIX_FMT_YUV420P
                    || st->codec->pix_fmt == PIX_FMT_YUV422P)))
            st->codec->pix_fmt = codec->pix_fmts[0];
    }
}

static int new_video_stream(AVFormatContext *oc, int file_idx)
{
    STMTranscodeContext *context = &globalTranscodeContext;
    AVStream *st;
    AVOutputStream *ost;
    AVCodecContext *video_enc;
    enum CodecID codec_id;
    AVCodec *codec= NULL;
    
    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        return -1;
    }
    
    ost = new_output_stream(oc, file_idx);
    
    if (!context->stc_settings.sts_video_copy) {
        if (context->stc_settings.sts_video_codec_name) {
            codec_id = find_codec_or_die(context->stc_settings.sts_video_codec_name, AVMEDIA_TYPE_VIDEO, 1);
            codec = avcodec_find_encoder_by_name(context->stc_settings.sts_video_codec_name);
        } else {
            codec_id = av_guess_codec(oc->oformat, NULL, oc->filename, NULL, AVMEDIA_TYPE_VIDEO);
            codec = avcodec_find_encoder(codec_id);
        }
    }
    
    AddNewOutputCodec(context, codec);
    
    avcodec_get_context_defaults3(st->codec, codec);
    ost->bitstream_filters = context->stc_settings.sts_video_bitstream_filter;
    
    if (context->stc_settings.sts_video_codec_name &&
        strncmp(context->stc_settings.sts_video_codec_name, "mjpeg", 5) != 0)
    {
        avcodec_thread_init(st->codec, threadCount());
    }
    else
    {
        avcodec_thread_init(st->codec, 1);
    }
    
    video_enc = st->codec;
    
    if (context->stc_settings.sts_video_copy) {
        st->stream_copy = 1;
        video_enc->codec_type = AVMEDIA_TYPE_VIDEO;
        strncpy(video_enc->codec_name, "passthrough", sizeof(video_enc->codec_name));
        video_enc->sample_aspect_ratio =
        st->sample_aspect_ratio = av_d2q(
                                         context->stc_settings.sts_source_video_aspect_ratio*
                                         context->stc_settings.sts_source_frame_height/context->stc_settings.sts_source_frame_width, 255);
        video_enc->codec_id = CODEC_ID_NONE;
        return 0;
    } else {
        AVRational fps =
        context->stc_settings.sts_video_frame_rate.num ?
        context->stc_settings.sts_video_frame_rate :
        context->stc_settings.sts_source_video_frame_rate.num ?
        context->stc_settings.sts_source_video_frame_rate :
        (AVRational){25,1};
        video_enc->codec_id = codec_id;
        
        if (codec && codec->supported_framerates)
            fps = codec->supported_framerates[av_find_nearest_q_idx(fps, codec->supported_framerates)];
        video_enc->time_base.den = fps.num;
        video_enc->time_base.num = fps.den;
        
        video_enc->width = context->stc_settings.sts_video_frame_width + context->stc_settings.sts_video_frame_width%2;
        video_enc->height = context->stc_settings.sts_video_frame_height + context->stc_settings.sts_video_frame_height%2;
        
        // Plex: Don't use the source AR, we'll compute actual pixels later.
        video_enc->sample_aspect_ratio = av_d2q(1, 1);
        //video_enc->sample_aspect_ratio = av_d2q(context->stc_settings.sts_source_video_aspect_ratio*video_enc->height/video_enc->width, 255);
        fprintf(stderr, "ENCODING: [%dx%d] %d:%d\n", video_enc->width, video_enc->height, video_enc->sample_aspect_ratio.num, video_enc->sample_aspect_ratio.den);
        
        video_enc->pix_fmt = PIX_FMT_NONE;
        st->sample_aspect_ratio = video_enc->sample_aspect_ratio;
        
        choose_pixel_fmt(st, codec);
        
        video_enc->rc_override_count=0;
        if (!video_enc->rc_initial_buffer_occupancy)
            video_enc->rc_initial_buffer_occupancy = video_enc->rc_buffer_size*3/4;
        video_enc->me_threshold= 0;
        video_enc->intra_dc_precision= 0;
    }
    
    STMTranscodeSettings *settings = &context->stc_settings;
    
    if (settings->sts_video_crf > 0)
    {
        av_set_int(video_enc, "crf", settings->sts_video_crf);
    }
    else
    {
        if (settings->sts_video_bufsize > 0)
        {
            av_set_int(video_enc, "bufsize", settings->sts_video_bufsize);
        }
        
        if (settings->sts_video_maxrate > 0)
        {
            av_set_int(video_enc, "minrate", 0);
            av_set_int(video_enc, "maxrate", settings->sts_video_maxrate + settings->sts_audio_bitrate);
            av_set_int(video_enc, "bt", 0.6 * settings->sts_video_maxrate);
            av_set_int(video_enc, "b", 0.9 * settings->sts_video_maxrate);
        }
    }
    av_set_int(video_enc, "level", settings->sts_video_level);
    
    if (settings->sts_video_profile)
    {
        av_set_int(video_enc, "profile", context->plex.videoCaps.profile);
        if (strcmp("main", settings->sts_video_profile) == 0)
        {
            av_set_int(video_enc, "bf", 1);
            av_set_int(video_enc, "coder", FF_CODER_TYPE_AC);
        }
        else
        {
            av_set_int(video_enc, "bf", 0);
            av_set_int(video_enc, "coder", 0);
        }
    }
    av_set_int(video_enc, "me_range", settings->sts_video_me_range);
    if (settings->sts_video_me_method)
    {
        av_set_string3(video_enc, "me_method", settings->sts_video_me_method, 0, NULL);
    }
    av_set_int(video_enc, "refs", settings->sts_video_frame_refs);
    av_set_int(video_enc, "sc_threshold", settings->sts_video_sc_threshold);
    av_set_int(video_enc, "qdiff", settings->sts_video_qdiff);
    av_set_int(video_enc, "qmin", settings->sts_video_qmin);
    av_set_int(video_enc, "qmax", settings->sts_video_qmax);
    //    	av_set_double(video_enc, "qcomp", settings->sts_video_qcomp);
    av_set_int(video_enc, "g", settings->sts_video_g);
    av_set_int(video_enc, "keyint_min", settings->sts_video_g / 4.0);
    //    	av_set_double(video_enc, "i_qfactor", 0.71428571429);
    av_set_double(video_enc, "subq", settings->sts_video_subq);
    
    av_set_string3(video_enc, "flags", settings->sts_video_flags, 0, NULL);
    av_set_string3(video_enc, "flags2", settings->sts_video_flags2, 0, NULL);
    //av_set_string3(video_enc, "cmp", settings->sts_video_cmp, 0, NULL);
    //av_set_string3(video_enc, "partitions", settings->sts_video_partitions, 0, NULL);
    
    //    av_set_string3(video_enc, "partitions", "-parti8x8-parti4x4-partp8x8-partp4x4-partb8x8", 0, NULL);
    /* av_set_string3(video_enc, "me_method", "dia", 0, NULL);
     av_set_int(video_enc, "g", 250);
     av_set_int(video_enc, "keyint_min", 15);
     av_set_int(video_enc, "sc_threshold", 40);
     av_set_double(video_enc, "i_qfactor", 0.71);
     av_set_double(video_enc, "subq", 0);
     av_set_int(video_enc, "me_range", 16);
     av_set_int(video_enc, "b_strategy", 1);
     av_set_double(video_enc, "qcomp", 0.6);
     av_set_int(video_enc, "qdiff", 4);
     av_set_int(video_enc, "directpred", 1);
     av_set_int(video_enc, "coder", 1);
     av_set_int(video_enc, "qmin", 0);
     av_set_int(video_enc, "qmax", 69);
     av_set_int(video_enc, "bf", 3);*/
    av_set_int(video_enc, "trellis", 0);
    av_set_int(video_enc, "wpredp", 0); 
    av_set_int(video_enc, "rc_lookahead", 1);

    //av_set_int(video_enc, "threads", threadCount());
    //av_set_string3(video_enc, "threads", "auto", 0, NULL); 
    //av_set_string3(video_enc, "thread-input", "", 0, NULL);
    
    //av_set_int(video_enc, "b_strategy", 0);
    //av_set_int(video_enc, "trellis", 0);
    
    return 0;
}

static int new_audio_stream(AVFormatContext *oc, int file_idx)
{
    STMTranscodeContext *context = &globalTranscodeContext;
    AVStream *st;
    AVOutputStream *ost;
    AVCodec *codec = NULL;
    AVCodecContext *audio_enc;
    enum CodecID codec_id;
    
    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        return -1;
    }
    ost = new_output_stream(oc, file_idx);
    
    if (!context->stc_settings.sts_audio_copy) {
        if (context->stc_settings.sts_audio_codec_name) {
            codec_id = find_codec_or_die(context->stc_settings.sts_audio_codec_name, AVMEDIA_TYPE_AUDIO, 1);
            codec = avcodec_find_encoder_by_name(context->stc_settings.sts_audio_codec_name);
        } else {
            codec_id = av_guess_codec(oc->oformat, NULL, oc->filename, NULL, AVMEDIA_TYPE_AUDIO);
            codec = avcodec_find_encoder(codec_id);
        }
    }
    
    AddNewOutputCodec(context, codec);
    
    avcodec_get_context_defaults2(st->codec, AVMEDIA_TYPE_AUDIO);
    
    avcodec_thread_init(st->codec, threadCount());
    
    audio_enc = st->codec;
    audio_enc->codec_type = AVMEDIA_TYPE_AUDIO;
    
    if (context->stc_settings.sts_audio_copy) {
        st->stream_copy = 1;
        strncpy(audio_enc->codec_name, "passthrough", sizeof(audio_enc->codec_name));
        audio_enc->channels = context->stc_settings.sts_audio_channels;
    } else {
        audio_enc->codec_id = codec_id;
        if (context->stc_settings.sts_audio_profile>=0) 
            audio_enc->profile = context->stc_settings.sts_audio_profile;
        audio_enc->channels = context->stc_settings.sts_audio_channels;
        audio_enc->channel_layout = context->stc_settings.sts_source_audio_channel_layout;
        if (av_get_channel_layout_nb_channels(context->stc_settings.sts_source_audio_channel_layout) != context->stc_settings.sts_audio_channels)
            audio_enc->channel_layout = 0;
        audio_enc->sample_fmt = AV_SAMPLE_FMT_S16;
    }
    
    audio_enc->sample_rate = context->stc_settings.sts_audio_sample_rate;
    audio_enc->time_base= (AVRational){1, context->stc_settings.sts_audio_sample_rate};
    
    av_set_int(audio_enc, "ab", context->stc_settings.sts_audio_bitrate);
    
    return 0;
}

static int new_subtitle_stream(AVFormatContext *oc, int file_idx)
{
    STMTranscodeContext *context = &globalTranscodeContext;
    AVStream *st;
    AVOutputStream *ost;
    AVCodec *codec=NULL;
    AVCodecContext *subtitle_enc;
    
    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        return -1;
    }
    ost = new_output_stream(oc, file_idx);
    
    subtitle_enc = st->codec;
    subtitle_enc->codec_id = find_codec_or_die(NULL, AVMEDIA_TYPE_SUBTITLE, 1);
    AddNewOutputCodec(context, NULL);
    codec= output_codecs[nb_output_codecs-1] = avcodec_find_encoder_by_name(NULL);
    
    avcodec_get_context_defaults3(st->codec, codec);
    avcodec_thread_init(st->codec, 1);
    
    subtitle_enc->codec_type = AVMEDIA_TYPE_SUBTITLE;
    subtitle_enc->codec_id = CODEC_ID_TEXT;
    
    return 0;
}

static int configureOutputFiles(STMTranscodeContext *context, char *filename)
{
    int use_video, use_audio;
    int input_has_video, input_has_audio, input_has_subtitle;
    AVFormatParameters params, *ap = &params;
    AVOutputFormat *file_oformat;
    
    if (context->stc_settings.sts_output_segment_length != 0)
    {
        context->stc_settings.sts_output_file_base = filename;
        context->stc_settings.sts_output_file_path = av_malloc(4096);
        UpdateFilePathForSegment(&context->stc_settings);
    }
    else
    {
        context->stc_settings.sts_output_file_path = av_malloc(strlen(filename) + 1);
        strcpy(context->stc_settings.sts_output_file_path, filename);
    }
    
    AddNewOutputFile(context, avformat_alloc_context());
    AVFormatContext *oc = context->stc_output_files[context->stc_nb_output_files - 1];
    
    if (!oc) {
        return -1;
    }
    
    file_oformat = av_guess_format(context->stc_settings.sts_output_file_format, context->stc_settings.sts_output_file_path, NULL);
    if (!file_oformat) {
        fprintf(stderr, "Unable to find a suitable output format for '%s'\n",
                context->stc_settings.sts_output_file_path);
        context->stc_nb_output_files--;
        av_free(context->stc_output_files[context->stc_nb_output_files]);
        
        return -1;
    }
    
    oc->oformat = file_oformat;
    oc->mux_rate = context->stc_settings.sts_output_muxrate;
    av_strlcpy(oc->filename, context->stc_settings.sts_output_file_path, sizeof(oc->filename));
    
    use_video = context->stc_settings.sts_video_codec_name != NULL;
    use_audio = context->stc_settings.sts_audio_codec_name != NULL;
    
    /* disable if no corresponding type found and at least one
     input file */
    if (context->stc_nb_input_files > 0) {
        check_audio_video_sub_inputs(&input_has_video, &input_has_audio, &input_has_subtitle);
        if (!input_has_video)
            use_video = 0;
        if (!input_has_audio)
            use_audio = 0;
    }
    
    if (strncmp(context->stc_settings.sts_audio_stream_specifier, "nil", 4) == 0) {
        use_audio = 0;
    }
    
    int nb_output_files = globalTranscodeContext.stc_nb_output_files - 1;
    
    if (use_video)
    {
        if (new_video_stream(oc, nb_output_files))
        {
            return -1;
        }
    }
    
    if (use_audio)
    {
        if (new_audio_stream(oc, nb_output_files))
        {
            return -1;
        }
    }
    
    if (context->stc_settings.sts_subtitle_stream_index != -1)
    {
        if (new_subtitle_stream(oc, nb_output_files))
        {
            return -1;
        }
    }
    
    oc->timestamp = 0;
    
    for(; context->stc_metadata_count>0; context->stc_metadata_count--){
        av_metadata_set2(&oc->metadata, context->stc_metadata[context->stc_metadata_count-1].key,
                         context->stc_metadata[context->stc_metadata_count-1].value, 0);
    }
    
    /* check filename in case of an image number is expected */
    if (oc->oformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(oc->filename)) {
            return -1;
        }
    }
    
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        /* open the file */
        if (url_fopen(&oc->pb, context->stc_settings.sts_output_file_path, URL_WRONLY) < 0) {
            fprintf(stderr, "Could not open '%s'\n", context->stc_settings.sts_output_file_path);
            return -1;
        }
    }
    
    memset(ap, 0, sizeof(*ap));
    if (av_set_parameters(oc, ap) < 0) {
        fprintf(stderr, "%s: Invalid encoding parameters\n",
                oc->filename);
        return -1;
    }
    
    oc->preload= 0.5;
    oc->max_delay= context->stc_settings.sts_input_start_time/2;
    oc->loop_output = 0;
    oc->flags |= AVFMT_FLAG_NONBLOCK;
    
    return 0;
}

static int configureMappings(STMTranscodeContext *context)
{
    return 0;
}

static int applyInitialSettings(STMTranscodeSettings *settings,
                                int64_t segment_duration, int64_t initial_segment,
                                int suppress_video,
                                char *input_file_name)
{
    if (segment_duration == 0)
    {
        settings->sts_output_single_frame_only = 1;
    }
    else
    {
        settings->sts_output_segment_length = (double)segment_duration;
        settings->sts_output_segment_index = initial_segment;
        settings->sts_output_initial_segment = initial_segment;
        settings->sts_input_start_time =
        (int64_t)(segment_duration * initial_segment * AV_TIME_BASE);
    }
    settings->sts_subtitle_stream_index = -1;
    settings->sts_suppress_video = suppress_video;
    settings->sts_input_file_name = input_file_name;
    
    return 0;
}

static char *newFileStringWithDifferentExtension(char *filename, char *ext)
{
    int i;
    int ext_length = strlen(ext);
    int filename_length = strlen(filename);
    char *other = av_malloc(filename_length + ext_length + 1 + 1);
    
    for (i = strlen(filename) - 1; i >= 0; i--)
    {
        if (filename[i] == '.')
        {
            break;
        }
    }
    if (i <= 0)
    {
        return NULL;
    }
    strncpy(other, filename, i);
    other[i] = '.';
    strncpy(&other[i+1], ext, ext_length);
    other[i + 1 + ext_length] = '\0';
    return other;
}

static char *newFileStringWithAdditionalExtension(char *filename, char *ext)
{
    char *other;
    asprintf(&other, "%s.%s", filename, ext);
    if (!other)
    {
        return 0;
    }
    
    return other;
}



static int fileExistsWithDifferentExtension(char *filename, char *ext)
{
    char *other = newFileStringWithDifferentExtension(filename, ext);
    if (!other)
    {
        return 0;
    }
    FILE *file = fopen(other, "r");
    int result = 0;
    if (file)
    {
        fclose(file);
        result = 1;
    }
    
    av_free(other);
    return result;
}

static int fileExistsWithAdditionalExtension(char *filename, char *ext)
{
    char *other;
    asprintf(&other, "%s.%s", filename, ext);
    if (!other)
    {
        return 0;
    }
    
    FILE *file = fopen(other, "r");
    int result = 0;
    if (file)
    {
        fclose(file);
        result = 1;
    }
    
    av_free(other);
    return result;
}

static int newComponentsForFilename(const char *filename, char **directory, char **basename, char **extension)
{
    int filenameLength = strlen(filename);
    int lastPeriodIndex = filenameLength - 1;
    int lastPathSeparatorIndex = filenameLength - 1;
    while (lastPeriodIndex >= 0 && filename[lastPeriodIndex] != '.' && filename[lastPeriodIndex] != STMPathSeparator)
    {
        lastPeriodIndex--;
    }
    if (lastPeriodIndex == filenameLength - 1 || filename[lastPeriodIndex] == STMPathSeparator)
    {
        return 0;
    }
    lastPathSeparatorIndex = lastPeriodIndex - 2;
    while (lastPathSeparatorIndex >= 0 && filename[lastPathSeparatorIndex] != STMPathSeparator)
    {
        lastPathSeparatorIndex--;
    }
    if (lastPathSeparatorIndex < 0)
    {
        return 0;
    }
    
    *extension = av_strdup(&filename[lastPeriodIndex + 1]);
    asprintf(basename, "%.*s", lastPeriodIndex - lastPathSeparatorIndex - 1, &filename[lastPathSeparatorIndex + 1]);
#ifdef __WIN32__
    // On Windows, we need to append a backslash to top-level drive letter paths
    // of the format '\\?\C:'. Since we assume all paths are long UNC paths, then
    // testing for length is sufficient.
    if (lastPathSeparatorIndex == 6)
    {
        asprintf(directory, "%.*s", lastPathSeparatorIndex + 1, filename);
    }
    else
    {
#endif
        asprintf(directory, "%.*s", lastPathSeparatorIndex, filename);
#ifdef __WIN32__
    }
#endif
    return 1;
}

static int newSubtitleIdentifiersForInputFilename(const char *filename, char ***subtitleIdentifiers, char ***subtitleFilenames)
{
    char *directory;
    char *basename;
    char *extension;
    if (!newComponentsForFilename(filename, &directory, &basename, &extension))
    {
        return 0;
    }
    
#ifdef __WIN32__
    wchar_t *wdirectory = utf8_to_new_utf16le(directory);
#endif
    
#ifndef __WIN32__
    DIR * directoryEnumerator;
    struct dirent *fileEntry;
    if ((directoryEnumerator = opendir(directory)) == NULL)
#else
        _WDIR * directoryEnumerator;
    struct _wdirent *fileEntry;
    if ((directoryEnumerator = _wopendir(wdirectory)) == NULL)
#endif
    {
        av_free(directory);
        av_free(basename);
        av_free(extension);
#ifdef __WIN32__
        av_free(wdirectory);
#endif
        
        return 0;
    }
    
    const int SubtitleExtensionLength = 3;
    const char *SubtitleExtensions[] = {"ssa", "ass", "srt", "sub"};
    const int NumSubtitleExtensions = sizeof(SubtitleExtensions) / sizeof(char **);
    
    *subtitleIdentifiers = NULL;
    int numResults = 0;
    int resultsCapacity = 0;
    int filenameResultsCapacity = 0;
    
    int basenameLength = strlen(basename);
#ifndef __WIN32__
    while ((fileEntry = readdir(directoryEnumerator)) != NULL)
#else
        while ((fileEntry = _wreaddir(directoryEnumerator)) != NULL)
#endif
        {
#ifndef __WIN32__
            const char *d_name = fileEntry->d_name;
#ifdef __linux__
            int name_length = strlen(fileEntry->d_name);
#else
            int name_length = fileEntry->d_namlen;
#endif
#else
            char *d_name = utf16le_withbytelength_to_new_utf8(fileEntry->d_name, fileEntry->d_namlen * 2);
            int name_length = strlen(d_name);
#endif
            
            if (verbose > 0) {fprintf(stderr, "Subtitle search traversed file: %s\n", d_name); fflush(stderr);}
            
            if (strncasecmp(basename, d_name, basenameLength) != 0)
            {
#ifdef __WIN32__
                av_free(d_name);
#endif
                continue;
            }
            
            if (name_length < basenameLength + SubtitleExtensionLength + 1)
            {
#ifdef __WIN32__
                av_free(d_name);
#endif
                continue;
            }
            
            int lastPeriodIndex = name_length - 1;
            while (lastPeriodIndex >= basenameLength && d_name[lastPeriodIndex] != '.')
            {
                lastPeriodIndex--;
            }
            if (lastPeriodIndex < basenameLength || name_length - lastPeriodIndex < 4)
            {
#ifdef __WIN32__
                av_free(d_name);
#endif
                continue;
            }
            
            char *extension = (char *)&d_name[lastPeriodIndex] + 1;
            int i;
            for (i = 0; i < NumSubtitleExtensions; i++)
            {
                if (strncasecmp(SubtitleExtensions[i], extension, 3) == 0)
                {
                    break;
                }
            }
            if (i == NumSubtitleExtensions)
            {
#ifdef __WIN32__
                av_free(d_name);
#endif
                continue;
            }
            
            char *matchIdentifier;
            if (lastPeriodIndex - basenameLength > 1)
            {
                asprintf(&matchIdentifier, "%s-%.*s",
                         extension,
                         lastPeriodIndex - basenameLength - 1,
                         &d_name[basenameLength + 1]);
            }
            else
            {
                matchIdentifier = av_strdup(extension);
            }
            
            if (verbose > 0) {fprintf(stderr, "Subtitle search matched file: %s\n", d_name); fflush(stderr);}
            
            numResults++;
            *subtitleIdentifiers = grow_array(*subtitleIdentifiers, sizeof(char*), &resultsCapacity, numResults);
            (*subtitleIdentifiers)[numResults - 1] = matchIdentifier;
            if (subtitleFilenames)
            {
                *subtitleFilenames = grow_array(*subtitleFilenames, sizeof(char*), &filenameResultsCapacity, numResults);
                asprintf(&((*subtitleFilenames)[numResults - 1]), "%s%c%s", directory, STMPathSeparator, d_name);
            }
            
#ifdef __WIN32__
            av_free(d_name);
#endif
        }
    
#ifdef __WIN32__
    av_free(wdirectory);
#endif
    av_free(directory);
    av_free(basename);
    av_free(extension);
    return numResults;
}

static char *newLanguageFromStreamSpecifier(char *specifier, long *indexOut)
{
    char *language = av_malloc(strlen(specifier) + 1);
    char *languageCopy = language;
    while (*specifier != '-' && *specifier != '\0')
    {
        *languageCopy++ = *specifier++;
    }
    if (languageCopy == language)
    {
        av_free(language);
        return NULL;
    }
    *languageCopy = '\0';
    
    if (*specifier == '-')
    {
        specifier++;
    }
    
    char *outSpecifier;
    long languageIndex = strtol(specifier, &outSpecifier, 10);
    
    if (outSpecifier != specifier)
    {
        *indexOut = languageIndex;
    }
    else
    {
        *indexOut = -1;
    }
    return language;
}

static int isParseableSubtitleCodec(AVCodecContext *codec)
{
    if (codec->codec_type != AVMEDIA_TYPE_SUBTITLE)
    {
        return 0;
    }
    if (codec->codec_id == CODEC_ID_SSA)
    {
        return 1;
    }
    if (codec->codec_id == CODEC_ID_TEXT)
    {
        return 1;
    }
    if (codec->codec_id == CODEC_ID_HDMV_PGS_SUBTITLE)
    {
        return 1;
    }
    if (codec->codec_id == CODEC_ID_MOV_TEXT && codec->codec_tag == MKTAG('t', 'x', '3', 'g'))
    {
        return 1;
    }
    return 0;
}

static int applyOutputSettings(STMTranscodeContext *context,
                               char *stream_quality_name,
                               char *audio_stream_specifier,
                               char *subtitle_stream_specifier,
                               float audio_gain,
                               int album_art,
                               char *encoding,
                               int flip_srt,
                               float subtitle_scale,
                               const char* output_path)
{
    if (context->stc_input_files[0]->duration != AV_NOPTS_VALUE)
    {
        fprintf(stdout, "Duration: %g\n", context->stc_input_files[0]->duration / (double)AV_TIME_BASE);
        fflush(stdout);
    }
    else
    {
        fprintf(stdout, "Duration: -1\n");
        fflush(stdout);
    }
    
    int videoStreamIndex = -1;
    for (int i = 0; i < context->stc_nb_stream_maps; i++)
    {
        AVStreamMap streamMap = context->stc_stream_maps[i];
        
        if (context->stc_input_files[streamMap.file_index]->streams[streamMap.stream_index]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = streamMap.stream_index;
            break;
        }
    }
    
    if (videoStreamIndex == -1 && album_art)
    {
        AVMetadata *metadata = context->stc_input_files[0]->metadata;
        AVMetadataTag *tag =
        av_metadata_get(metadata, "covr", NULL, AV_METADATA_IGNORE_SUFFIX);
        if (!tag)
        {
            tag = av_metadata_get(metadata, "PIC", NULL, AV_METADATA_IGNORE_SUFFIX);
        }
        if (!tag)
        {
            tag = av_metadata_get(metadata, "APIC", NULL, AV_METADATA_IGNORE_SUFFIX);
        }
        if (tag /*&& tag->data_size > 0*/)
        {
            context->stc_settings.sts_album_art_mode = 1;
            fprintf(stderr, "Not implemented!\n");
            assert(0);
            /*			inmemory_buffer = av_malloc(tag->data_size);
             inmemory_buffer_size = tag->data_size;
             memcpy(inmemory_buffer, tag->data, tag->data_size);*/
            
            register_inmemory_protocol();
            
            char *filename = context->stc_settings.sts_input_file_name;
            
            if (strncmp(inmemory_buffer, "\x89PNG", 4)==0)
            {
                context->stc_settings.sts_input_file_name = "inmemory://file.png";
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    context->stc_settings.sts_album_art_mode = 0;
                    context->stc_settings.sts_input_file_name = filename;
                    RemoveLastInputFile(context);
                }
                else
                {
                    videoStreamIndex = 0;
                }
            }
            else if (strncmp(inmemory_buffer, "BM", 2)==0)
            {
                context->stc_settings.sts_input_file_name = "inmemory://file.bmp";
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    context->stc_settings.sts_album_art_mode = 0;
                    context->stc_settings.sts_input_file_name = filename;
                    RemoveLastInputFile(context);
                }
                else
                {
                    videoStreamIndex = 0;
                }
            }
            else
            {
                context->stc_settings.sts_input_file_name = "inmemory://file.jpg";
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    context->stc_settings.sts_album_art_mode = 0;
                    context->stc_settings.sts_input_file_name = filename;
                    RemoveLastInputFile(context);
                }
                else
                {
                    videoStreamIndex = 0;
                }
            }
        }
    }
    
    //audio_stream_specifier = "deu";
    int audioStreamIndex = -1;
    if (strncmp(audio_stream_specifier, "nil", 4) != 0)
    {
        long languageIndex;
        char *language = newLanguageFromStreamSpecifier(audio_stream_specifier, &languageIndex);
        long numMatchingStreams = 0;
        long firstIndex = -1;
        long firstLangIndex = -1;
        
        //PLEX
        const int isNumeric = PLEXisNumeric(audio_stream_specifier);
        
        long i;
        int selectedStream = 0;
        if (isNumeric){
            //PLEX
            i = atoi(audio_stream_specifier);
            if (i<context->stc_input_files[0]->nb_streams){
                AVStream *stream = context->stc_input_files[0]->streams[i];
                if (stream->codec->codec_type == AVMEDIA_TYPE_AUDIO && !stream->discard){
                    firstLangIndex = i;
                    firstIndex = i;
                    selectedStream = 1;
                }
            }
            
            if (!selectedStream){
                PMS_Log(LOG_LEVEL_WARNING, "Error: Did not find audio Stream %i", i);
            }
        } 
        
        if (!selectedStream){
            for (i = 0; i < context->stc_input_files[0]->nb_streams; i++)
            {
                AVStream *stream = context->stc_input_files[0]->streams[i];
                if (stream->codec->codec_type != AVMEDIA_TYPE_AUDIO ||
                    stream->discard)
                {
                    continue;
                }
                if (firstIndex == -1)
                {
                    firstIndex = i;
                }
                if (languageIndex != -1 && strncmp(language, "any", 4) != 0)
                {
                    break;
                }
                AVMetadataTag *tag = av_metadata_get(stream->metadata, "language", NULL, 0);
                if (((!tag || !tag->value) && strcmp("und", language) == 0) ||
                    (tag && tag->value && strcmp(tag->value, language) == 0))
                {
                    if (firstLangIndex == -1)
                    {
                        firstLangIndex = i;
                    }
                    numMatchingStreams++;
                    if (numMatchingStreams == languageIndex)
                    {
                        break;
                    }
                }
            }
        }
        
        if (i == context->stc_input_files[0]->nb_streams)
        {
            if (firstLangIndex != -1)
            {
                i = firstLangIndex;
            }
            else if (firstIndex != -1)
            {
                i = firstIndex;
            }
        }
        
        if (i != context->stc_input_files[0]->nb_streams)
        {
            AVStreamMap audio_map;
            audio_map.file_index = 0;
            audio_map.stream_index = i;
            audio_map.sync_file_index = 0;
            audio_map.sync_stream_index = i;
            AddNewStreamMap(context, audio_map);
            audioStreamIndex = i;
        }
        
        av_free(language);
    }
    
    if (audio_gain >= 0)
    {
        audio_gain += 1.0;
    }
    else
    {
        audio_gain = 1.0 / (1.0 - audio_gain);
    }
    
    STMTranscodeSettings *settings = &context->stc_settings;
    
    PLEXsetupBaseTranscoderSettings(context, audio_stream_specifier, subtitle_stream_specifier, audio_gain, encoding, audioStreamIndex, videoStreamIndex, settings, output_path);
    
    double nominal_fps = settings->sts_video_frame_rate.num / (double)settings->sts_video_frame_rate.den;
    double avg_fps = 0;
    if (videoStreamIndex != -1)
    {
        // Be careful about how we compute the FPS, the average could be zero.
        PMS_Log(LOG_LEVEL_DEBUG, "Frames per second is either %f or %f.", 
            context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.den != 0 ? av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate) : 0.0,
            context->stc_input_files[0]->streams[videoStreamIndex]->r_frame_rate.den != 0 ? av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->r_frame_rate) : 0.0);
        
        // Start with the average frame rate.
        if (context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.den != 0)
          avg_fps = av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate);
        
        // Don't trust any super high framerates.    
        if (context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.den == 0 || avg_fps > 150.0)
          avg_fps = av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->r_frame_rate);
        
        if (!settings->sts_album_art_mode &&
            !settings->sts_still_image_mode &&
            nominal_fps < 12 || nominal_fps > 31)
        {
            if (nominal_fps > 31.0 &&
                nominal_fps > avg_fps * 1.9 && nominal_fps < avg_fps * 2.1)
            {
                if (verbose > 0)
                {
                    fprintf(stderr,"\nAssuming interlaced. Halving frame rate.\n");
                }
                settings->sts_video_frame_rate.num = lround(0.5 * nominal_fps);
                settings->sts_video_frame_rate.den = 1;
                settings->sts_source_video_interlaced = 2;
            }
            else if (avg_fps >= 12 && avg_fps <= 30)
            {
                settings->sts_video_frame_rate.num = lround(avg_fps);
                settings->sts_video_frame_rate.den = 1;
            }
            else if (nominal_fps > 31)
            {
                settings->sts_video_frame_rate.num = 30;
                settings->sts_video_frame_rate.den = 1;
            }
            else if (nominal_fps < 12)
            {
                settings->sts_video_frame_rate.num = 12;
                settings->sts_video_frame_rate.den = 1;
            }
        }
    }
    
    double subtitle_scale_factor = 1.0;
    
    int qualityLength = strlen(stream_quality_name);
    settings->sts_max_bitrate = 0;
    if (qualityLength > 4 && strcmp(&stream_quality_name[qualityLength - 4], "_max") == 0)
    {
        stream_quality_name[qualityLength - 4] = '\0';
        settings->sts_max_bitrate = 1;
    }
    
    if (strcmp(stream_quality_name, "veryhigh") == 0)
    {
        settings->sts_output_quality = QUALITY_VERY_HIGH;
    }
    else if (strcmp(stream_quality_name, "high") == 0)
    {
        settings->sts_output_quality = QUALITY_HIGH;
    }
    else if (strcmp(stream_quality_name, "midhigh") == 0)
    {
        settings->sts_output_quality = QUALITY_MID_HIGH;
    }
    else if (strcmp(stream_quality_name, "mid") == 0)
    {
        settings->sts_output_quality = QUALITY_MID;
    }
    else if (strcmp(stream_quality_name, "midlow") == 0)
    {
        settings->sts_output_quality = QUALITY_MID_LOW;
    }
    else if (strcmp(stream_quality_name, "low") == 0)
    {
        settings->sts_output_quality = QUALITY_LOW;
    }
    else if (strcmp(stream_quality_name, "verylow") == 0)
    {
        settings->sts_output_quality = QUALITY_VERY_LOW;
    }
    
    if (videoStreamIndex == -1 || context->stc_settings.sts_album_art_mode)
    {
        if (!context->stc_settings.sts_album_art_mode)
        {
            settings->sts_video_codec_name = NULL;
        }
        
        settings->sts_video_frame_rate.num = 7;
        settings->sts_video_frame_rate.den = 2;
        settings->sts_video_maxrate = 0;
        settings->sts_video_bufsize = 0;
        
        if (settings->sts_output_quality >= QUALITY_MID_HIGH)
        {
            settings->sts_video_frame_width = 480;
            settings->sts_video_frame_height = 480;
            settings->sts_audio_bitrate = 256 * 1000;
            settings->sts_video_crf = 27;
        }
        else if (settings->sts_output_quality == QUALITY_MID)
        {
            settings->sts_audio_bitrate = 128 * 1000;
            settings->sts_video_frame_width = 320;
            settings->sts_video_frame_height = 320;
            settings->sts_video_crf = 31;
        }
        else if(settings->sts_output_quality == QUALITY_MID_LOW || settings->sts_output_quality == QUALITY_LOW)
        {
            settings->sts_audio_bitrate = 96 * 1000;
            settings->sts_video_frame_width = 320;
            settings->sts_video_frame_height = 320;
            settings->sts_video_crf = 31;
        }
        else // verylow
        {
            //			settings->sts_output_muxrate = 8 * 64000;
            settings->sts_audio_bitrate = 48 * 1000;
            settings->sts_video_frame_width = 192;
            settings->sts_video_frame_height = 192;
            settings->sts_video_crf = 34;
            settings->sts_video_frame_rate.num = 3;
            settings->sts_video_frame_rate.den = 1;
        }
        
        settings->sts_video_frame_refs = 8;
        settings->sts_video_qcomp = 0.9;
        settings->sts_video_subq = 8;
        settings->sts_video_me_method = "zero";
    }
    else if (context->stc_settings.sts_still_image_mode)
    {
        settings->sts_video_frame_rate.num = 7;
        settings->sts_video_frame_rate.den = 2;
        settings->sts_video_crf = 18;
        settings->sts_video_maxrate = 0;
        settings->sts_video_bufsize = 0;
        
        if (settings->sts_output_quality == QUALITY_VERY_HIGH)
        {
            settings->sts_video_frame_width = 1280;
            settings->sts_video_frame_height = 720;
        }
        else
        {
            settings->sts_video_frame_width = 720;
            settings->sts_video_frame_height = 480;
        }
    }
    
    /////// TRANSCODE qualities.
    else {
        PLEXsetupAudioTranscoderSettingsForQuality(context, audio_stream_specifier, stream_quality_name, audio_gain, audioStreamIndex, settings);
        PLEXsetupVideoTranscoderSettingsForQuality(context, videoStreamIndex, stream_quality_name, &subtitle_scale_factor, settings);
    }
    
    const double GOPLength = 2.5;
    settings->sts_video_g =
    ceil(GOPLength * settings->sts_video_frame_rate.num /
         (double)settings->sts_video_frame_rate.den);
    
    if (videoStreamIndex!= -1){
        AVStream *stream = context->stc_input_files[0]->streams[videoStreamIndex];
        float scale_factor = (float)stream->codec->height /settings->sts_video_frame_height;
        subtitle_scale_factor = (1 + 1.0/4.2 * scale_factor) * 1.2 * subtitle_scale;
        PMS_Log(LOG_LEVEL_DEBUG, "Final font scale: %f (%f) -> %f", scale_factor, subtitle_scale, subtitle_scale_factor);
    }
    
    if (videoStreamIndex != -1 && settings->sts_album_art_mode)
    {
        asprintf(&context->stc_settings.sts_vfilters,
                 "inlineass=pts_offset:%lld|font_scale:%f",
                 (int64_t)(context->stc_settings.sts_output_segment_length * context->stc_settings.sts_output_initial_segment) * AV_TIME_BASE,
                 subtitle_scale_factor);
    }
    //Changes for PLEX
    else if (videoStreamIndex != -1 && strncmp(subtitle_stream_specifier, "nil", 4) != 0)
    {
        long languageIndex = -1;
        char* language = 0;
        //char *language = newLanguageFromStreamSpecifier(subtitle_stream_specifier, &languageIndex);
        long numMatchingStreams = 0;
        long firstIndex = -1;
        long firstLangIndex = -1;
        
        //PLEX
        const int isNumeric = PLEXisNumeric(subtitle_stream_specifier);
        int file_exists = !isNumeric && PLEXfileExists(subtitle_stream_specifier);
        
        char **fileIdentifiers = NULL;
        char **filenames = NULL;
        
        //PLEX
        if (file_exists)
        {
          subtitle_path = av_strdup(subtitle_stream_specifier);
          fprintf(stdout, "\n\n!!! Subtitle path: %s !!!\n", subtitle_path);
        }
        else
        {
            long i = 0;
            if (isNumeric)
            {
                //PLEX
                i = atoi(subtitle_stream_specifier);
                firstLangIndex = i;
                firstIndex = i;
            }
            
            if (i == context->stc_input_files[0]->nb_streams)
            {
                if (firstLangIndex != -1)
                {
                    i = firstLangIndex;
                }
                else if (firstIndex != -1)
                {
                    i = firstIndex;
                }
            }
            
            if (i != context->stc_input_files[0]->nb_streams)
            {
                context->stc_settings.sts_subtitle_stream_index = i;
                configureInputFiles(context, 0);
            }
        }
        av_free(language);
        
        AVStream *sttl_stream = 0;
        if (globalTranscodeContext.stc_settings.sts_subtitle_stream_index != -1)
            sttl_stream = globalTranscodeContext.stc_input_files[0]->streams[globalTranscodeContext.stc_settings.sts_subtitle_stream_index];
        
        if ((subtitle_path || context->stc_settings.sts_subtitle_stream_index != -1) && 
            (!sttl_stream || sttl_stream->codec->codec_id != CODEC_ID_HDMV_PGS_SUBTITLE))
        {
            asprintf(&settings->sts_vfilters,
                     "inlineass=pts_offset:%lld|font_scale:%f|encoding:%s|flip_srt:%d|fps:%f",
                     (int64_t)(settings->sts_output_segment_length * settings->sts_output_initial_segment) * AV_TIME_BASE,
                     subtitle_scale_factor, encoding, flip_srt,
                     settings->sts_source_video_frame_rate.num / (double)settings->sts_source_video_frame_rate.den);
            
            fprintf(stdout, "!!! %s !!!\n", settings->sts_vfilters);
        }
    }
    
    //
    // Look to see if it is possible to use the audio track without conversion
    //
    if (PLEXcanCopyAudioStream(context, audioStreamIndex, audio_gain, settings))
    {
        PMS_Log(LOG_LEVEL_DEBUG, "!!! Will copy the audio stream\n");
        AVCodecContext *audioCodec = context->stc_input_files[0]->streams[audioStreamIndex]->codec;
        
        settings->sts_audio_copy = 1;
        settings->sts_audio_codec_name = audioCodec->codec_name;
        settings->sts_audio_sample_rate = audioCodec->sample_rate;
        settings->sts_audio_channels = audioCodec->channels;
        settings->sts_audio_bitrate = audioCodec->bit_rate;
    }else {
        fprintf(stderr, "!!! Will NOT copy the audio stream\n");
    }
    
    //
    // Look to see if it is possible to use the video track without conversion
    //
    if (PLEXcanCopyVideoStream(context, audioStreamIndex, videoStreamIndex, avg_fps, settings))
    {
        PMS_Log(LOG_LEVEL_DEBUG, "!!! Will copy the video stream\n");
        AVCodecContext *videoCodec = context->stc_input_files[0]->streams[videoStreamIndex]->codec;
        
        settings->sts_video_copy = 1;
        settings->sts_video_codec_name = videoCodec->codec_name;
        settings->sts_video_frame_width = settings->sts_source_frame_width;
        settings->sts_video_frame_height = settings->sts_source_frame_height;
        settings->sts_video_frame_rate.num = 0;
        settings->sts_video_frame_rate.den = 0;
        //			settings->sts_output_muxrate = videoCodec->bit_rate + settings->sts_audio_bitrate;
        
        if (videoCodec->codec_id == CODEC_ID_H264)
        {
            AVStream *st = NULL;
            AVBitStreamFilterContext* plexBSF = 0;

            settings->sts_video_bitstream_filter =
            av_bitstream_filter_init("h264_mp4toannexb");
            if(!settings->sts_video_bitstream_filter)
            {
                fprintf(stderr, "Unknown bitstream filter h264_mp4toannexb\n");
                ffmpeg_exit(1);
            }

            plexBSF = av_bitstream_filter_init("h264_plex");
            if (plexBSF == 0)
            {
                fprintf(stderr, "Unknown bitstream filter h264_plex\n");
                ffmpeg_exit(1);
            }
            
            settings->sts_video_bitstream_filter->next = plexBSF;

            st = context->stc_input_files[0]->streams[videoStreamIndex];
            
            // See if things are sane.
            long int averageFPS = 0;
            if (context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.den != 0)
              averageFPS = (int)(av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate)+0.5);
              
            long int framerateFPS = 0;
            if (context->stc_input_files[0]->streams[videoStreamIndex]->r_frame_rate.den != 0)
              framerateFPS = (int)(av_q2d(context->stc_input_files[0]->streams[videoStreamIndex]->r_frame_rate)+0.5);

            PMS_Log(LOG_LEVEL_DEBUG, "Average FPS ~ %d fps, Frame rate ~ %d fps.", averageFPS, framerateFPS);

            // Don't trust any super high framerates.    
            if (context->plex.useFpsFilter && averageFPS != 0 && framerateFPS != 0 && abs(averageFPS - framerateFPS) > 10)
            {
                char *fps_params;
                AVBitStreamFilterContext *bsfc = NULL;

                PMS_Log(LOG_LEVEL_DEBUG, "Codec frame rate differs from container rate, attempting to fix with bitstream filter\n");
                
                if (st->avg_frame_rate.den != 0)
                {
                  asprintf(&fps_params, "h264_changefps=fps=%d:%d", st->avg_frame_rate.num, st->avg_frame_rate.den);
                  PMS_Log(3, "Forcing fps to frame rate of %f using %s\n", av_q2d(st->avg_frame_rate), fps_params);
                }

                if (st->avg_frame_rate.den == 0 || av_q2d(st->avg_frame_rate) > 150.0)
                {
                  asprintf(&fps_params, "h264_changefps=fps=%d:%d", st->r_frame_rate.num, st->r_frame_rate.den);
                  PMS_Log(3, "Forcing fps to frame rate of %f using %s\n", av_q2d(st->r_frame_rate), fps_params);
                }
                
                bsfc = av_bitstream_filter_init(fps_params);
                if (!bsfc) {
                    fprintf(stderr, "Unknown bitstream filter h264_changefps\n");
                    ffmpeg_exit(1);
                }
                
                plexBSF->next = bsfc;
            }
        }
    } else {
        fprintf(stderr, "!!! Will NOT copy the video stream\n");
    }
    
    
    return 0;
}

static char *escapeQuotes(char *str)
{
    int quoteCount = 0;
    int length = strlen(str);
    int i;
    for (i = 0; i < length; i++)
    {
        if (str[i] == '"' || str[i] == '\\' || str[i] < 0x20)
        {
            quoteCount++;
        }
    }
    if (!quoteCount)
    {
        return str;
    }
    char *result = av_malloc(length + quoteCount + 1);
    int destOffset = 0;
    for (i = 0; i < length; i++)
    {
        result[i + destOffset] = str[i];
        if (result[i + destOffset] == '"')
        {
            result[i + destOffset] = '\\';
            destOffset++;
            result[i + destOffset] = '"';
        }
        else if (result[i + destOffset] == '\\')
        {
            result[i + destOffset] = '\\';
            destOffset++;
            result[i + destOffset] = '\\';
        }
    }
    result[i + destOffset] = '\0';
    av_free(str);
    
    return result;
}

static int writeJsonToBuffer(STMTranscodeContext *context, char *outputBuffer, size_t outputBufferSize)
{
    int isDirname;
    int isFilename;
    char *artist = escapeQuotes(newArtistForFile(context->stc_input_files[0], &isDirname));
    char *title = escapeQuotes(newTitleForFile(context->stc_input_files[0], &isFilename));
    
    int jsonLength = 0;
    
    jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{");
    
    if (!isDirname)
    {
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"\"artist\":\"%s\",", artist);
    }
    if (!isFilename)
    {
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"\"title\":\"%s\",", title);
    }
    
    av_free(artist);
    av_free(title);
    
    jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"\"length\":%f,\"audio_streams\":[",
                           (double)context->stc_input_files[0]->duration /
                           (double)AV_TIME_BASE);
    
    int audio_track_count = 0;
    for (int i = 0; i < context->stc_input_files[0]->nb_streams; i++)
    {
        if (context->stc_input_files[0]->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            context->stc_input_files[0]->streams[i]->discard != AVDISCARD_ALL)
        {
            if (audio_track_count != 0)
            {
                jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",");
            }
            audio_track_count++;
            
            //
            // Look to see if it is possible to use the audio track without conversion
            //
            AVCodecContext *audioCodec = context->stc_input_files[0]->streams[i]->codec;
            char compatibility_string[6];
            strcpy(compatibility_string, "-----");
            if (audioCodec->codec_id != CODEC_ID_MP3 &&
                audioCodec->codec_id != CODEC_ID_AAC &&
                audioCodec->codec_tag != MKTAG('.', 'm', 'p', '3'))
            {
                compatibility_string[0] = 'c'; // wrong codec
            }
            else if (audioCodec->codec_id == CODEC_ID_MP3 && audioCodec->sample_rate != 48000)
            {
                compatibility_string[0] = 'M'; // MP3 must be 48kHz
            }
            else if (audioCodec->codec_id == CODEC_ID_AAC &&
                     (audioCodec->sample_rate < 8000 ||
                      audioCodec->sample_rate > 48000))
            {
                compatibility_string[0] = 'A'; // AAC must be 8-48kHz
            }
            if (audioCodec->channels < 1 || audioCodec->channels > 2)
            {
                compatibility_string[1] = 'c'; // wrong number of channels
            }
            if (audioCodec->bit_rate != 0 &&				(audioCodec->bit_rate > 320 * 1024 || audioCodec->bit_rate < 8 * 1024))
            {
                compatibility_string[2] = 'b'; // wrong codec
            }
            
            AVMetadataTag *languageTag =
            av_metadata_get(context->stc_input_files[0]->streams[i]->metadata, "language", NULL, 0);
            AVMetadataTag *titleTag =
            av_metadata_get(context->stc_input_files[0]->streams[i]->metadata, "title", NULL, 0);
            if (titleTag && titleTag->value)
            {
                jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{\"language\":\"%s\",\"title\":\"%s\"",
                                       (languageTag && languageTag->value) ? languageTag->value : "und",
                                       titleTag->value);
            }
            else
            {
                jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{\"language\":\"%s\"", (languageTag && languageTag->value) ? languageTag->value : "und");
            }
            jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",\"trans\":\"%s\"}", compatibility_string);
        }
    }
    jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"],\"subtitle_streams\":[");
    int subtitle_track_count = 0;
    for (int i = 0; i < context->stc_input_files[0]->nb_streams; i++)
    {
        AVStream *stream = context->stc_input_files[0]->streams[i];
        if (!isParseableSubtitleCodec(stream->codec))
        {
            continue;
        }
        if (subtitle_track_count != 0)
        {
            jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",");
        }
        subtitle_track_count++;
        
        AVMetadataTag *languageTag =
        av_metadata_get(stream->metadata, "language", NULL, 0);
        AVMetadataTag *titleTag =
        av_metadata_get(stream->metadata, "title", NULL, 0);
        if (titleTag && titleTag->value)
        {
            jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{\"language\":\"%s\",\"title\":\"%s\"}",
                                   (languageTag && languageTag->value) ? languageTag->value : "und",
                                   titleTag->value);
        }
        else
        {
            jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{\"language\":\"%s\"}", (languageTag && languageTag->value) ? languageTag->value : "und");
        }
    }
    
    char **subtitleFileIdentifiers = NULL;
    int numSubtitleFiles = newSubtitleIdentifiersForInputFilename(context->stc_input_files[0]->filename, &subtitleFileIdentifiers, NULL);
    for (int i = 0; i < numSubtitleFiles; i++)
    {
        if (subtitle_track_count != 0)
        {
            jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",");
        }
        subtitle_track_count++;
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"{\"file\":\"%s\"}", escapeQuotes(subtitleFileIdentifiers[i]));
        av_free(subtitleFileIdentifiers[i]);
    }
    if (numSubtitleFiles)
    {
        av_free(subtitleFileIdentifiers);
    }
    
    int videoStreamIndex = -1;
    int isDRM = 0;
    for (int i = 0; i < context->stc_input_files[0]->nb_streams; i++)
    {
        if (context->stc_input_files[0]->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            if (context->stc_input_files[0]->streams[i]->codec->codec_tag == MKTAG('d', 'r', 'm', 'i'))
            {
                isDRM = 1;
            }
            if (videoStreamIndex == -1 && context->stc_input_files[0]->streams[i]->discard != AVDISCARD_ALL)
                videoStreamIndex = i;
        }
        else if (context->stc_input_files[0]->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (context->stc_input_files[0]->streams[i]->codec->codec_tag == MKTAG('d', 'r', 'm', 's'))
            {
                isDRM = 1;
            }
        }
    }
    jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,"]");
    if (isDRM)
    {
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",\"has_drm\":true");
    }
    if (videoStreamIndex == -1)
    {
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",\"has_video\":false}");
    }
    else
    {
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",\"has_video\":true");
        
        //
        // Look to see if it is possible to use the video track without conversion
        //
        AVCodecContext *videoCodec = context->stc_input_files[0]->streams[videoStreamIndex]->codec;
        
        char compatibility_string[11];
        strcpy(compatibility_string, "----------");
        
        if (videoCodec->codec_id != CODEC_ID_H264)
        {
            compatibility_string[0] = 'c'; // wrong codec
        }
        if (videoCodec->level > 31)
        {
            compatibility_string[1] = 'l'; // H.264 level too high
        }
        if (videoCodec->profile > FF_PROFILE_H264_BASELINE)
        {
            if (videoCodec->profile > FF_PROFILE_H264_MAIN)
            {
                compatibility_string[2] = 'h'; // H.264 profile too high
            }
            else
            {
                compatibility_string[2] = 'm'; // H.264 profile is mainline (iPad/iPhone4 only)
            }
        }
        double avg_fps = context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.num /
        (double)context->stc_input_files[0]->streams[videoStreamIndex]->avg_frame_rate.den;
        if (avg_fps > 31)
        {
            compatibility_string[3] = 'h'; // frame rate too high
        }
        else if (avg_fps < 2.5)
        {
            compatibility_string[3] = 'l'; // frame rate too low
        }
        if (context->stc_input_files[0]->streams[videoStreamIndex]->vfr != 0)
        {
            compatibility_string[4] = 'v'; // VFR is not supported
        }
        if (context->stc_settings.sts_source_frame_width > 720)
        {
            if (context->stc_settings.sts_source_frame_width > 1280)
            {
                compatibility_string[5] = 'W'; // video is too wide for playback
            }
            else
            {
                compatibility_string[5] = 'w'; // video is too wide for playback pre-iPad
            }
        }
        if (context->stc_settings.sts_source_frame_height > 576)
        {
            if (context->stc_settings.sts_source_frame_height > 720)
            {
                compatibility_string[6] = 'H'; // video is too wide for playback
            }
            else
            {
                compatibility_string[6] = 'h'; // video is too wide for playback pre-iPad
            }
        }
        if (videoCodec->bit_rate > 2560 * 1024)
        {
            if (videoCodec->bit_rate > 6400 * 1024)
            {
                compatibility_string[7] = 'B'; // video is too wide for playback
            }
            else
            {
                compatibility_string[7] = 'b'; // video is too wide for playback pre-iPad
            }
        }
        jsonLength += snprintf(&outputBuffer[jsonLength], outputBufferSize - jsonLength,",\"trans\":\"%s\"}", compatibility_string);
    }
    
    return jsonLength;
}

static int applyMetadataOutputSettings(STMTranscodeContext *context, int hd)
{
    int videoStreamIndex = -1;
    for (int i = 0; i < context->stc_input_files[0]->nb_streams; i++)
    {
        if (context->stc_input_files[0]->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
            context->stc_input_files[0]->streams[i]->discard != AVDISCARD_ALL)
        {
            videoStreamIndex = i;
            break;
        }
    }
    
#define MetadataBufferSize 2048
    char metdataBuffer[MetadataBufferSize];
    int jsonLength = writeJsonToBuffer(context, metdataBuffer, MetadataBufferSize);
    fwrite(&jsonLength, sizeof(int), 1, stdout);
    fwrite(metdataBuffer, jsonLength, 1, stdout);
    fflush(stdout);
    
    if (videoStreamIndex == -1)
    {
        AVMetadata *metadata = context->stc_input_files[0]->metadata;
        AVMetadataTag *tag =
        av_metadata_get(metadata, "covr", NULL, AV_METADATA_IGNORE_SUFFIX);
        if (!tag)
        {
            tag = av_metadata_get(metadata, "PIC", NULL, AV_METADATA_IGNORE_SUFFIX);
        }
        if (!tag)
        {
            tag = av_metadata_get(metadata, "APIC", NULL, AV_METADATA_IGNORE_SUFFIX);
        }
        if (tag /*&& tag->data_size > 0*/)
        {
            fprintf(stderr, "Not Implemented!\n");
            assert(0);
            /*			inmemory_buffer = av_malloc(tag->data_size);
             inmemory_buffer_size = tag->data_size;
             memcpy(inmemory_buffer, tag->data, tag->data_size);*/
            
            register_inmemory_protocol();
            
            av_close_input_file(context->stc_input_files[0]);
            context->stc_input_files[0] = NULL;
            context->stc_nb_input_files--;
            
            memset(context, 0, sizeof(STMTranscodeContext));
            
            if (strncmp(inmemory_buffer, "\x89PNG", 4)==0)
            {
                applyInitialSettings(&context->stc_settings, 0, 0, 0, "inmemory://file.png");
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    return 1;
                }
            }
            else if (strncmp(inmemory_buffer, "BM", 2)==0)
            {
                applyInitialSettings(&context->stc_settings, 0, 0, 0, "inmemory://file.bmp");
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    return 1;
                }
            }
            else
            {
                applyInitialSettings(&context->stc_settings, 0, 0, 0, "inmemory://file.jpg");
                if (configureInputFiles(context, 0))
                {
                    fprintf(stderr, "Thumbnail parsing failed\n");
                    return 1;
                }
            }
            
            videoStreamIndex = 0;
        }
        else
        {
            fprintf(stderr, "No video and no covr tag.");
            return -1;
        }
    }
    
    STMTranscodeSettings *settings = &context->stc_settings;
    
    settings->sts_audio_stream_specifier = "nil";
    settings->sts_subtitle_stream_specifier = "nil";
    
    settings->sts_input_start_time = 0.7;
    settings->sts_output_dts_delta_threshold = 10;
    settings->sts_output_duration = INT64_MAX;
    
    settings->sts_output_file_format = "image2pipe";
    settings->sts_video_codec_name = "mjpeg";
    
    const double target_width = 108;
    const double target_height = 72;
    const double target_width_hd = 216;
    const double target_height_hd = 144;
    const double thumb_width = hd ? target_width_hd : target_width;
    const double thumb_height = hd ? target_height_hd : target_height;
    
    AVStream *stream = context->stc_input_files[0]->streams[videoStreamIndex];
    double stream_width = stream->codec->width;
    double stream_height = stream->codec->height;
    
    if (stream->codec->sample_aspect_ratio.num > 0 && stream->codec->sample_aspect_ratio.den > 0)
    {
        stream_width *= stream->codec->sample_aspect_ratio.num / (double)stream->codec->sample_aspect_ratio.den;
    }
    
    if (thumb_width / thumb_height >
        stream_width / stream_height)
    {
        settings->sts_video_frame_height = thumb_height;
        settings->sts_video_frame_width = thumb_height * stream_width / stream_height;
    }
    else
    {
        settings->sts_video_frame_width = thumb_width;
        settings->sts_video_frame_height = thumb_width * stream_height / stream_width;
    }
    
    settings->sts_video_qmin = 2;
    settings->sts_video_qmax = 31;
    settings->sts_video_qcomp = 0.5;
    settings->sts_video_qdiff = 3;
    settings->sts_video_g = 12;
    
    return 0;
}


extern char* HTTP_USER_AGENT;
extern char* HTTP_COOKIES;
#define PHOTO_DURATION_METADATA OUTPUT_FILE_PATH_PREFIX

#ifdef WINDOWS
#include "iconv.h"
#endif

int main_plex(int argc, char **argv);
int main(int argc, char **argv);
int main_plex(int argc, char **argv)
{
    //just print CPU Detection values
    if (argc > 2 && strcmp(argv[2], "cpuinfo") == 0)
    {
        PLEXshowAutoCPUInfo();
        return 0;
    }
    
    int PLEXcontinousMode = 1;
    if (argc >= TRANSCODE_OPTIONS){
        char* tcopt = argv[TRANSCODE_OPTIONS];
        int len = strlen(tcopt);
        if (len>0){
            if (tcopt[0]=='s'){
                fprintf(stderr, "--> Creating Client Socket\n");
                PLEXcontinousMode = 0;
                for (int i=0; i<len-1; i++)
                    tcopt[i] = tcopt[i+1];
                tcopt[len-1] = 0;
            } else if (tcopt[0]=='d'){
                fprintf(stderr, "--> Verbose Mode (you asked for it!!!)\n");
                PLEX_DEBUG = 1;
                verbose = 1;
                for (int i=0; i<len-1; i++)
                    tcopt[i] = tcopt[i+1];
                tcopt[len-1] = 0;
            } else {
                fprintf(stderr, "--> Continous Mode\n");
            }
        }
    }
    int result = 1;
    
    if (verbose > 0)
    {
        int arg;
        for (arg = 0; arg < argc; arg++)
        {
            fprintf(stderr, "Arg %d: %s\n", arg, argv[arg]);
            //fprintf(stderr, " %s", argv[arg]);
        }
        fflush(stderr);
    }
    if (argc > SEGMENT_DURATION)  
        PMS_Log(LOG_LEVEL_DEBUG, "Starting transcoder v306 (t=%ss, q=%s)", argv[SEGMENT_DURATION], argv[VARIANT_NAME]);
    
    executable_path = argv[0];
    subtitle_path = NULL;
    
    if (verbose != 0 && (getenv("NoAcknowledgements") != NULL &&
                         strcmp(getenv("NoAcknowledgements"), "1") == 0))
    {
        fprintf(stderr, "No acknowledgements.\n");
    }
    else
    {
#ifdef WINDOWS
#else
        long parentProcess = 0;
        pthread_t thread; 
        pthread_create (&thread, 0, WatchForParentTermination, (void *)parentProcess);
        
        signal(SIGABRT, SignalHandler);
        signal(SIGILL, SignalHandler);
        signal(SIGSEGV, SignalHandler);
        signal(SIGFPE, SignalHandler);
#endif
        
        //   fprintf(stderr, "Acknowledgements: %s\n", getenv("NoAcknowledgements"));
    }
    
#ifdef WINDOWS
    char fetchedPath[MAX_PATH+1];
    if (strlen(executable_path) == 0)
    {
        GetModuleFileName(NULL, (TCHAR *)fetchedPath, MAX_PATH+1);
        executable_path = fetchedPath;
    }
#endif
    
    if (argc > OPERATION_NAME &&
        strcmp(argv[OPERATION_NAME], "fontcache") == 0)
    {
        char *conf_path;
        char *lastSlash = strrchr(executable_path, STMPathSeparator);
        int length = (long)lastSlash - (long)executable_path;
        asprintf(&conf_path, "%.*s%c%s", length, executable_path, STMPathSeparator, STMConfFilename);
        
        FcConfig *config = FcConfigCreate();
        FcConfigParseAndLoad(config, (const FcChar8 *)conf_path, FcTrue);
        FcConfigBuildFonts(config);
        
        av_free(conf_path);
        return 0;
    }
    
    if (argc < INPUT_PATH + 1)
    {
        fprintf(stderr, "Wrong number of arguments for metadata or transcode.\n");
        return 1;
    }
    
    //do not wait in debug mode
    if (!PLEX_DEBUG && !PLEXcontinousMode){
        char segFile[1024];
        memset(segFile, 0, 1024);
        sprintf(segFile, "%s-com.socket", argv[OUTPUT_FILE_PATH_PREFIX]);
        int res = PLEXcreateSegmentSocket(segFile, atoi(argv[INITIAL_SEGMENT_INDEX]));
        if (res>0) {return res;} else {if (res<0) goto close_and_exit;}
    }
    
    memset(&globalTranscodeContext, 0, sizeof(STMTranscodeContext));
    
    avcodec_register_all();
    avfilter_register_all();
    av_register_all();
    
    PMS_Log(LOG_LEVEL_DEBUG, "Registered components, about to parse arguments.");
    if (argc > INPUT_PATH &&
        (strcmp(argv[OPERATION_NAME], "metadata") == 0 ||
         strcmp(argv[OPERATION_NAME], "metadata_hd") == 0))
    {
#ifdef WINDOWS
#if 0
        if (verbose == 0 || (getenv("NoAcknowledgements") == NULL || strcmp(getenv("NoAcknowledgements"), "1") != 0))
        {
            char *filename = getline(stdin);
            argv[INPUT_PATH] = filename;
            
            if (verbose > 0)
            {
                fprintf(stderr, "Input Path: %s\n", argv[INPUT_PATH]);
                fprintf(stderr, "Output Path: %s\n", argv[OUTPUT_FILE_PATH_PREFIX]);
            }
        }
        else
        {
            argv[INPUT_PATH] = "\\\\?\\C:\\metadata_test_file.mkv";
        }
#endif
#endif		
        applyInitialSettings(&globalTranscodeContext.stc_settings, 0, 0, 0, argv[INPUT_PATH]);
        
        PMS_Log(LOG_LEVEL_DEBUG, "Opening input file.");
        
        /* Set the input file */
        if (configureInputFiles(&globalTranscodeContext,
                                strtof(argv[PHOTO_DURATION_METADATA], NULL)))
        {
            goto close_and_exit;
        }
        
        PMS_Log(LOG_LEVEL_DEBUG, "Applying output settings.\n");
        
        int metadataResult =
        applyMetadataOutputSettings(&globalTranscodeContext,
                                    strcmp(argv[OPERATION_NAME], "metadata_hd") == 0);
        if (metadataResult != 0)
        {
            if (metadataResult == -1)
            {
                result = 0;
            }
            goto close_and_exit;
        }
        
        PMS_Log(LOG_LEVEL_DEBUG, "Configuring output.\n");
        
        /* Set the output file */
        if (configureOutputFiles(
                                 &globalTranscodeContext,
                                 "pipe:"))
        {
            goto close_and_exit;
        }
        
        /* Configure the mappings */
        if (configureMappings(&globalTranscodeContext))
        {
            goto close_and_exit;
        }
    }
    else if (argc >= PLEX_MIN_NUM_ARGUMENTS &&
             strcmp(argv[OPERATION_NAME], "transcode") == 0)
    {
#ifdef WINDOWS
#if 0
        if (verbose == 0 || (getenv("NoAcknowledgements") == NULL ||
                             strcmp(getenv("NoAcknowledgements"), "1") != 0))
        {
            char *filename = getline(stdin);
            argv[INPUT_PATH] = filename;
            
            char *outputPath = getline(stdin);
            argv[OUTPUT_FILE_PATH_PREFIX] = outputPath;
            
            if (verbose > 0)
            {
                fprintf(stderr, "Input Path: %s\n", argv[INPUT_PATH]);
                fprintf(stderr, "Output Path: %s\n", argv[OUTPUT_FILE_PATH_PREFIX]);
            }
        }
        else
        {
            argv[INPUT_PATH] = "\\\\?\\C:\\transcode_test_file.mkv";
        }
#endif
#endif
        
        // User agent.
        if (argc > USER_AGENT && strlen(argv[USER_AGENT]) > 0)
        {
            //assert(0);
            HTTP_USER_AGENT = argv[USER_AGENT];
        }
        
        // Cookie.
        if (argc > COOKIES && strlen(argv[COOKIES]) > 0)
        {
            HTTP_COOKIES = argv[COOKIES];
        }
        
        
        av_log(NULL, AV_LOG_INFO,"Opening %s, %s", argv[INPUT_PATH], argv[USER_AGENT]);
        
        //Make sure we vill the PLEXContext data structure
        PLEXsetupContext(&globalTranscodeContext, argc, argv);
        
        /* Set the initial settings */
        PMS_Log(LOG_LEVEL_DEBUG, "About to apply initial settings.\n");
        if (applyInitialSettings(
                                 &globalTranscodeContext.stc_settings,
                                 strtoll(argv[SEGMENT_DURATION], NULL, 10),
                                 strtoll(argv[INITIAL_SEGMENT_INDEX], NULL, 10),
                                 strcmp(argv[ALBUM_ART], "background") == 0,
                                 argv[INPUT_PATH]))
        {
            PMS_Log(LOG_LEVEL_ERROR, "Unable to initialize.");
            goto close_and_exit;
        }
        
        PMS_Log(LOG_LEVEL_DEBUG, "Opening the input file.");
        
        /* Set the input file */
        if (configureInputFiles(&globalTranscodeContext, 0.0f))
        {
            fprintf(stderr, "Unable to configure input files.\n");
            goto close_and_exit;
        }
        
        PMS_Log(LOG_LEVEL_DEBUG, "Applying output settings.");
        
        //PLEX (default subtitle scaling to 1.0)
        float subtitleScale = 1.0;
        if (argc>SUBTITLE_SCALE) subtitleScale = strtof(argv[SUBTITLE_SCALE], NULL); 
        
        if (applyOutputSettings(
                                &globalTranscodeContext,
                                argv[VARIANT_NAME],
                                argv[AUDIO_STREAM_SPECIFIER],
                                argv[SUBTITLE_STREAM_SPECIFIER],
                                strtof(argv[AUDIO_GAIN], NULL),
                                strcmp(argv[ALBUM_ART], "yes") == 0,
                                argv[SRT_ENCODING],
                                strcmp(argv[SRT_DIRECTION], "rtl") == 0,
                                subtitleScale,
                                argv[OUTPUT_FILE_PATH_PREFIX]))
        {
            PMS_Log(LOG_LEVEL_ERROR, "Unable to determine output settings.\n");
            goto close_and_exit;
        }
        
        if (globalTranscodeContext.plex.transcodeToFileArgv!=NULL && globalTranscodeContext.plex.transcodeToFileArgc>1){
            if (PLEX_DEBUG){
                fprintf(stderr, "Looping back to default ffmpeg:\n");
                for (int i=0; i<globalTranscodeContext.plex.transcodeToFileArgc; i++){
                    fprintf(stderr, "    %i: %s\n", i, globalTranscodeContext.plex.transcodeToFileArgv[i]);    
                }
            }
            
            //call the ffmpeg main
            main(globalTranscodeContext.plex.transcodeToFileArgc, globalTranscodeContext.plex.transcodeToFileArgv);
            
            //free the parameter array
            for (int i=0; i<globalTranscodeContext.plex.transcodeToFileArgc; i++){
                free(globalTranscodeContext.plex.transcodeToFileArgv[i]);
            }
            free(globalTranscodeContext.plex.transcodeToFileArgv);
            globalTranscodeContext.plex.transcodeToFileArgv = NULL;
            globalTranscodeContext.plex.transcodeToFileArgc = 0;
            
            //free everything we did prep for the transcode
            goto close_and_exit;
        }
        
        PMS_Log(LOG_LEVEL_DEBUG, "Configuring output.");
        
        
        /* Set the output file */
        if (configureOutputFiles(
                                 &globalTranscodeContext,
                                 argv[OUTPUT_FILE_PATH_PREFIX]))
        {
            goto close_and_exit;
        }
        
        /* Configure the mappings */
        if (configureMappings(&globalTranscodeContext))
        {
            PMS_Log(LOG_LEVEL_ERROR, "Failed to configure mapping.");
            goto close_and_exit;
        }
    }
    else
    {
        PMS_Log(LOG_LEVEL_ERROR, "Wrong number of arguments for transcode.");
        goto close_and_exit;
    }
    
    if (verbose > 0) {fprintf(stderr, "Starting av_encode.\n"); fflush(stderr);}
    
    if(transcode(
                 globalTranscodeContext.stc_output_files,
                 globalTranscodeContext.stc_nb_output_files,
                 globalTranscodeContext.stc_input_files,
                 globalTranscodeContext.stc_nb_input_files,
                 globalTranscodeContext.stc_stream_maps,
                 globalTranscodeContext.stc_nb_stream_maps))
    {
        PMS_Log(LOG_LEVEL_ERROR, "The transcoder died.");
        goto close_and_exit;
    }
    
    result = 0;
    if (verbose > 0) {fprintf(stderr, "Encode finished successfully.\n"); fflush(stderr);}
    
    goto close_and_exit;
    
close_and_exit:
    if (result)
    {
        PMS_Log(LOG_LEVEL_ERROR, "Exiting after failure.");
    }
    
    PLEXcloseSegmentSocket();
    ffmpeg_exit(0);
    return result;
}
