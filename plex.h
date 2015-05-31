/*
 * ffmpeg plex extensions
 *
 * Created by Frank Bauer (8/2011)
 *
 *  */

#include "config.h"
#include <ctype.h>
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
#include "libavutil/opt.h"
#include "libavcodec/audioconvert.h"
#include "libavutil/audioconvert.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/colorspace.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavformat/os_support.h"

#include "libavformat/ffm.h" // not public API

# include "libavfilter/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/avfiltergraph.h"

#if HAVE_SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif
#include <time.h>
#include "libavformat/plex_http.h"

#include "cmdutils.h"
#include "ffmpeg.h"

#include <stdio.h>

typedef struct
{
    int file_index;
    int stream_index;
    AVFilterContext *ctx;
} InlineAssContext;

typedef struct
{
    int64_t input_start_time;           //[+]

    int64_t output_duration;            //[+]
    char* progress_url;                 //[-]
    int throttle_delay;

    int nb_inlineass_ctxs;
    InlineAssContext *inlineass_ctxs;
} PlexContext;

extern PlexContext plexContext;

void plex_init(int *argc, char ***argv);

int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg);

int plex_opt_progress_url(void *optctx, const char *opt, const char *arg);
int plex_opt_loglevel(void *o, const char *opt, const char *arg);

void plex_feedback(const AVFormatContext *ic);

void plex_prepare_setup_streams_for_input_stream(InputStream* ist);
void plex_link_subtitles_to_graph(AVFilterGraph* graph);
int plex_process_subtitles(const InputStream *ist, AVPacket *pkt);
void plex_process_subtitle_header(const InputStream *ist);
void plex_link_input_stream(const InputStream *ist);
