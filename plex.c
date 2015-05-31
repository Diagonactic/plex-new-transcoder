/*
 * ffmpeg plex extensions
 *
 * Created by Frank Bauer (8/2011)
 *
 * Streaming Code originally created by Matt Gallagher
 * Functions were renamed to make the clearly distinct
 * from built in ffmpeg functions
 *
 *  */

#include "plex.h"
#include "plexConfig.h"
#include "plexVersion.h"
#include "ffmpeg.h"
const char* PLEX_EXTENSION_VERSION = "3.003";

#include <sys/types.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/sysctl.h>
#endif
#include "strings.h"
#include "libavcodec/mpegvideo.h"
#include "libavfilter/vf_inlineass.h"
#include "libavformat/http.h"
#include "libavutil/timestamp.h"
#include "libavformat/internal.h"

#ifndef MIN
#define MIN(____xx_, ____yy_) (____xx_<____yy_?____xx_:____yy_)
#endif

PlexContext plexContext = {0};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_init(int *argc, char ***argv)
{
    //print some version info
    fprintf(stderr, "!!! Plex Transcoder v%s.%s (%s, %s, %s) !!!\n", PLEX_EXTENSION_VERSION, PLEX_BUILD_VERSION, PLEX_GIT_HASH, PLEX_BUILD_TYPE, PLEX_BUILD_ARCH);

    fprintf(stderr, "----------------------------------------------------------------------------\n");
    fprintf(stderr, "\n\n");
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_prepare_setup_streams_for_input_stream(InputStream* ist)
{
    int i;
    for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file_index == ctx->file_index) {
            ist->discard = 0;
            ist->st->discard = AVDISCARD_NONE;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_subtitles_to_graph(AVFilterGraph* graph)
{
    static int contextId = 0;
    for (int i = 0; i < graph->nb_filters && contextId < plexContext.nb_inlineass_ctxs; i++)
    {
        const AVFilterContext* filterCtx = graph->filters[i];
        if (strcmp(filterCtx->filter->name, "inlineass") == 0)
        {
            AVFilterContext *ctx = graph->filters[i];
            plexContext.inlineass_ctxs[contextId++].ctx = ctx;
            for (int j = 0; j < nb_input_streams; j++)
                if (input_streams[j]->st->codec->codec_type == AVMEDIA_TYPE_ATTACHMENT)
                    vf_inlineass_add_attachment(ctx, input_streams[j]->st);

            vf_inlineass_set_fonts(ctx);
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg)
{
    InlineAssContext *m = NULL;
    int i, file_idx;
    char *p;
    char *map = av_strdup(arg);

    file_idx = strtol(map, &p, 0);
    if (file_idx >= nb_input_files || file_idx < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid subtitle input file index: %d.\n", file_idx);
        goto finish;
    }

    for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
        if (check_stream_specifier(input_files[file_idx]->ctx, input_files[file_idx]->ctx->streams[i],
                    *p == ':' ? p + 1 : p) <= 0)
            continue;
        if (input_files[file_idx]->ctx->streams[i]->codec->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            av_log(NULL, AV_LOG_ERROR, "Stream '%s' is not a subtitle stream.\n", arg);
            continue;
        }
        GROW_ARRAY(plexContext.inlineass_ctxs, plexContext.nb_inlineass_ctxs);
        m = &plexContext.inlineass_ctxs[plexContext.nb_inlineass_ctxs - 1];

        m->file_index   = file_idx;
        m->stream_index = i;
        break;
    }

finish:
    if (!m)
        av_log(NULL, AV_LOG_ERROR, "Subtitle stream map '%s' matches no streams.\n", arg);

    av_freep(&map);
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_process_subtitle_header(const InputStream *ist)
{
    int i;
    for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file_index == ctx->file_index && ctx->ctx)
            vf_inlineass_process_header(ctx->ctx, ist->st->codec);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_process_subtitles(const InputStream *ist, AVPacket *pkt)
{
    int i;
    /* If we're burning subtitles, pass discarded subtitle packets of the
     * appropriate stream  to the subtitle renderer */
    for (i = 0; i < plexContext.nb_inlineass_ctxs; i++) {
        InlineAssContext *ctx = &plexContext.inlineass_ctxs[i];
        if (ist->st->index == ctx->stream_index &&
            ist->file_index == ctx->file_index && ctx->ctx) {
            vf_inlineass_append_data(ctx->ctx, ist->st, pkt);
            return 1;
        }
    }
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_progress_url(void *optctx, const char *opt, const char *arg)
{
    plexContext.progress_url = (char*)arg;
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int plex_opt_loglevel(void *o, const char *opt, const char *arg)
{
    opt_loglevel((void*)&av_log_set_level_plex, opt, arg);
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_feedback(const AVFormatContext *ic)
{
    if (ic->duration != AV_NOPTS_VALUE){
        double stream_duration = ic->duration / AV_TIME_BASE;
        fprintf(stdout, "Duration: %g\n", stream_duration);
    } else {
        fprintf(stdout, "Duration: -1\n");
    }
    fflush(stdout);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void plex_link_input_stream(const InputStream *ist)
{
    int i;
    if (ist->st->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        for (i = 0; i < plexContext.nb_inlineass_ctxs; i++)
            if (plexContext.inlineass_ctxs[i].ctx)
                vf_inlineass_set_storage_size(plexContext.inlineass_ctxs[i].ctx, ist->st->codec->width, ist->st->codec->height);
}
