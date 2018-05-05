/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#include "libavformat/os_support.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/thread.h"
#include "ac3_parser_internal.h"
#include "avcodec.h"
#include "internal.h"
#include "mlp_parser.h"

#ifdef _WIN32
#define PATHSEP "\\"
#else
#define PATHSEP "/"
#endif

static atomic_int g_process_sequence_nr;

// Upper bound on number of packets to buffer before starting decoding, or
// frames to buffer before starting encoding.
#define NUM_MAX_BUFFER 20

// Number of files which should be put on disk at the same time (reduces
// latency issues).
#define NUM_MAX_FILES 4

typedef struct eae_frame {
    int64_t pts;
    int frame_size; // decoding only
    AVBufferRef *buffer;
} eae_frame;

// each of these is an "in flight" result we expect from EAE
// (they are buffered for efficiency)
typedef struct eae_file {
    int discard_frames;
    char *path_in;
    char *path_out;
    struct eae_frame frames[NUM_MAX_BUFFER]; // all with .buffer==NULL
} eae_file;

typedef struct EAEContext {
    AVClass *class;

    char *root;
    // The path the the target folder, including a unique prefix.
    // (A sequence number and the file extension have still to be added.)
    char *path_prefix;
    int sequence_nr;
    const char *ext_in, *ext_out;
    char *path_in, *path_in_tmp, *path_out;

    // Number of frames/packets to buffer at least before starting encoding/decoding.
    int num_min_buffered;
    // Number of frames/packets to repeat at start of each encode/decode run.
    int num_min_priming;

    struct eae_frame input[NUM_MAX_BUFFER];
    int num_buffered;
    int num_priming;

    // Output data to be returned to the user. Valid if buffer is set.
    AVBufferRef *output_buffer;
    struct eae_frame output_frames[NUM_MAX_BUFFER]; // all with .buffer==NULL
    int output_discard_frames;

    // Decoding only.
    AVPacket prebuffered_packet;
    int prebuffered_au_size;
    int num_prebuffered_samples;
    int frame_size;
    int64_t prev_pts;
    int prev_pts_samples;

    // Pending output data for encoding and decoding.
    // The oldest entry is 0 (i.e. will be returned to the user next).
    struct eae_file files[NUM_MAX_FILES];
    int num_files;

    int eof;

    char *opt_eae_root;
    char *opt_eae_prefix;
    int opt_eae_batch_frames;
    int opt_max_files;

    // State related to I/O thread.
    pthread_t io_thread;
    int io_thread_valid;
    pthread_mutex_t io_lock;
    pthread_cond_t io_cond;

    // Protected by io_lock
    int io_active; // whether the IO thread is writing data
    int io_terminate; // if set, exit the thread

    // Accessed by the main thread if io_active==0
    // Accessed by the IO thread if io_active!=0
    char *io_tmp_file;
    char *io_out_file;
    AVBufferRef *io_data;
    size_t io_data_size;
    int io_status; // error code when done
} EAEContext;

// Create a new file (like fopen(path, "wb")), but:
// - flag it such that it's possibly not flushed to disk (Windows only)
// - add O_EXCL
// Also supports UTF-8 filenames.
static FILE *eae_fopen_temp(const char *path)
{
    int fd;
    int access = O_CREAT|O_WRONLY|O_TRUNC|O_EXCL;
#ifdef O_BINARY
    access |= O_BINARY;
#endif
#ifdef _O_SHORT_LIVED
    access |= _O_SHORT_LIVED;
#endif
    fd = avpriv_open(path, access, 0666);
    if (fd == -1)
        return NULL;
    return fdopen(fd, "wb");
}

static int write_on_io_thread(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    FILE *f;

    f = eae_fopen_temp(s->io_tmp_file);
    if (!f)
        return AVERROR_EXTERNAL;

    fwrite(s->io_data->data, s->io_data_size, 1, f);

    if (fclose(f)) {
        unlink(s->io_tmp_file);
        return AVERROR(EIO);
    }

    // We assume that this rename is atomic.
    if (rename(s->io_tmp_file, s->io_out_file)) {
        unlink(s->io_tmp_file);
        return AVERROR(EIO);
    }

    return 0;
}

static void *io_thread(void *p)
{
    AVCodecContext *avctx = p;
    EAEContext *s = avctx->priv_data;

    pthread_mutex_lock(&s->io_lock);
    while (1) {
        while (!s->io_active && !s->io_terminate)
            pthread_cond_wait(&s->io_cond, &s->io_lock);

        if (s->io_terminate) {
            pthread_mutex_unlock(&s->io_lock);
            return NULL;
        }

        pthread_mutex_unlock(&s->io_lock);
        s->io_status = write_on_io_thread(avctx);
        pthread_mutex_lock(&s->io_lock);
        s->io_active = 0;
        pthread_cond_broadcast(&s->io_cond);
    }
}

static void eae_wait_on_io_thread(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    pthread_mutex_lock(&s->io_lock);
    while (s->io_active)
        pthread_cond_wait(&s->io_cond, &s->io_lock);
    pthread_mutex_unlock(&s->io_lock);
}

static int eae_common_init(AVCodecContext *avctx, const char *subfolder)
{
    EAEContext *s = avctx->priv_data;
    int sequence;
    long long pid;
    char *tmp;
    FILE *tmpf;
    int success = 0;

    pthread_mutex_init(&s->io_lock, NULL);
    pthread_cond_init(&s->io_cond, NULL);
    if (pthread_create(&s->io_thread, NULL, io_thread, avctx))
        return AVERROR(ENOMEM);
    s->io_thread_valid = 1;

    if (s->opt_eae_root && s->opt_eae_root[0])
        s->root = av_strdup(s->opt_eae_root);
    else
        s->root = ff_getenv("EAE_ROOT");
    if (!s->root || !s->root[0]) {
        av_log(avctx, AV_LOG_ERROR, "No EAE watchfolder set!\n");
        return AVERROR_EXTERNAL;
    }

    if (!s->opt_eae_prefix)
        return AVERROR(EINVAL);

    // Try to make up an ID that's unique on the system. We could use a PRNG,
    // but this seems simpler.
    sequence = atomic_fetch_add(&g_process_sequence_nr, 1);
    pid = getpid();

    s->path_prefix = av_asprintf("%s" PATHSEP "%s" PATHSEP "%s%lld-%d", s->root, subfolder, s->opt_eae_prefix, pid, sequence);
    if (!s->path_prefix)
        return AVERROR(ENOMEM);

    tmp = av_asprintf("%s-test.tmp", s->path_prefix);
    if (!tmp)
        return AVERROR(ENOMEM);
    tmpf = eae_fopen_temp(tmp);
    if (tmpf) {
        success = 1;
        fclose(tmpf);
        unlink(tmp);
    }

    if (!success) {
        av_log(avctx, AV_LOG_ERROR, "EAE watchfolder is not writable: %s\n", tmp);
        av_free(tmp);
        return AVERROR_EXTERNAL;
    }

    av_free(tmp);

    s->num_min_buffered = FFMAX(s->opt_eae_batch_frames, s->num_min_priming + 1);
    s->num_min_buffered = FFMIN(s->num_min_buffered, NUM_MAX_BUFFER);

    av_assert0(s->num_min_buffered <= NUM_MAX_BUFFER);
    av_assert0(s->num_min_priming < s->num_min_buffered);

    s->prev_pts = AV_NOPTS_VALUE;
    s->prev_pts_samples = 0;

    return 0;
}

static int eae_decode_init(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    int def_batch_frames = 0;
    const char *folder = "Convert to WAV (to 8ch or less)";
    if (avctx->request_channel_layout == AV_CH_LAYOUT_5POINT1_BACK ||
        avctx->request_channel_layout == AV_CH_LAYOUT_5POINT1) {
        //folder = "Convert to WAV (to 6ch or less)";
    } else if (avctx->request_channel_layout == AV_CH_LAYOUT_STEREO) {
        folder = "Convert to WAV (to 2ch or less)";
    }

    def_batch_frames = 20;

    switch (avctx->codec_id) {
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_MLP:
        s->ext_in = "mlp";
        s->num_min_priming = 0;
        def_batch_frames = 4;
        break;
    case AV_CODEC_ID_AC3:
        s->ext_in = "ac3";
        s->num_min_priming = 1;
        break;
    case AV_CODEC_ID_EAC3:
        s->ext_in = "ec3";
        s->num_min_priming = 1;
        break;
    }

    if (!s->opt_eae_batch_frames)
        s->opt_eae_batch_frames = def_batch_frames;

    s->ext_out = "wav";

    return eae_common_init(avctx, folder);
}

static int eae_encode_init(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    const char *folder = NULL;

    switch (avctx->codec_id) {
    case AV_CODEC_ID_AC3:
        if (avctx->bit_rate >= 640 * 1000)
            folder = "Convert to Dolby Digital (High Quality - 640 kbps)";
        else
            folder = "Convert to Dolby Digital (Low Quality - 384 kbps)";
        s->ext_out = "ac3";
        break;
    case AV_CODEC_ID_EAC3:
        if (avctx->bit_rate >= 1024 * 1000 || avctx->channels > 6)
            folder = "Convert to Dolby Digital Plus (Max Quality - 1024 kbps)";
        else
            folder = "Convert to Dolby Digital Plus (High Quality - 384 kbps)";
        s->ext_out = "ec3";
        break;
    }

    if (!s->opt_eae_batch_frames)
        s->opt_eae_batch_frames = 20;

    s->ext_in = "wav";

    s->num_min_priming = 2;

    avctx->frame_size = 6 * 256; // AC3 + EAC3

    return eae_common_init(avctx, folder);
}

static int eae_next_path(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    av_freep(&s->path_in);
    av_freep(&s->path_out);
    av_freep(&s->path_in_tmp);

    s->path_in  = av_asprintf("%s-%d.%s", s->path_prefix, s->sequence_nr, s->ext_in);
    s->path_out = av_asprintf("%s-%d.%s", s->path_prefix, s->sequence_nr, s->ext_out);
    s->path_in_tmp = av_asprintf("%s.tmp", s->path_in);

    s->sequence_nr++;

    return (s->path_in && s->path_in_tmp && s->path_out) ? 0 : AVERROR(ENOMEM);
}

// Poll until the file comes into existence and then open it. It would be nicer
// to use path notification mechanism (like inotify()), but for now it's not
// worth the trouble to implement this for the 3 or 4 OSes this has to work on.
static FILE *eae_wait_and_open(void *log, const char *path)
{
    struct timeval start;
    gettimeofday(&start, NULL);

    while (1) {
        struct timeval now;
        FILE *f = av_fopen_utf8(path, "rb");
        if (f)
            return f;
        // (we should probably error out if the error is anything but ENOENT,
        // but it's also likely the error code got clobbered.)

#ifdef _WIN32
        Sleep(0);
#else
        sched_yield();
#endif

        // Timeout rather than potentially freezing forever.
        gettimeofday(&now, NULL);
        if (now.tv_sec > start.tv_sec + 2)
            break;
    }

    av_log(log, AV_LOG_ERROR, "EAE timeout! EAE not running, or wrong folder? Could not read '%s'\n", path);
    return NULL;
}

static void eae_flush(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    int i;

    eae_wait_on_io_thread(avctx);

    for (i = 0; i < s->num_files; i++) {
        FILE *f;
        eae_file *e = &s->files[i];

        // unfortunately we have wait here, because we want to make sure the
        // output file is really deleted
        f = eae_wait_and_open(avctx, e->path_out);
        if (f)
            fclose(f);

        unlink(e->path_in);
        av_freep(&e->path_in);

        unlink(e->path_out);
        av_freep(&e->path_out);
    }
    s->num_files = 0;

    for (i = 0; i < s->num_buffered; i++)
        av_buffer_unref(&s->input[i].buffer);
    s->num_buffered = 0;
    s->num_priming = 0;

    av_buffer_unref(&s->output_buffer);

    s->eof = 0;

    av_packet_unref(&s->prebuffered_packet);
    s->prebuffered_au_size = 0;
    s->num_prebuffered_samples = 0;
    s->frame_size = 0;
    s->prev_pts = AV_NOPTS_VALUE;
    s->prev_pts_samples = 0;
}

static const uint8_t guid_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
static const uint8_t guid_KSDATAFORMAT_SUBTYPE_PCM[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

// fuck Windows
typedef struct XFILE {
    EAEContext *s;
    int had_error;
} XFILE;

static void writeBytes(const void *p, size_t c, size_t num, XFILE *f)
{
    EAEContext *s = f->s;

    if (f->had_error)
        return;

    if (!s->io_data || s->io_data_size + c > s->io_data->size) {
        if (av_buffer_realloc(&s->io_data, (s->io_data_size + c + 1) * 2) < 0) {
            f->had_error = 1;
            return;
        }
    }

    memcpy(s->io_data->data + s->io_data_size, p, c);
    s->io_data_size += c;
}

static void writeByte(XFILE *file, uint8_t b)
{
  writeBytes(&b, 1, 1, file);
}

static void writeWord(XFILE *file, uint16_t w)
{
  writeByte(file, w & 0xFF);
  writeByte(file, w >> 8);
}

static void writeDWord(XFILE *file, uint32_t dw)
{
  writeWord(file, dw & 0xFFFF);
  writeWord(file, dw >> 16);
}

static uint32_t fourCCToInt(const char *fcc)
{
  uint32_t r = 0;
  for (int n = 0; n < 4 && fcc[n]; n++)
    r |= (unsigned)fcc[n] << (8 * n);
  return r;
}

static int eae_write_wav_header(AVCodecContext *avctx, XFILE *f, int sample_count)
{
    int sample_size = av_get_bytes_per_sample(avctx->sample_fmt);
    int data_size = sample_count * sample_size * avctx->channels;

    // RIFF header
    writeDWord(f, fourCCToInt("RIFF"));
    writeDWord(f, 4 + 4 + 18 + 22 + 4 + 4 + data_size);
    writeDWord(f, fourCCToInt("WAVE"));

    // "fmt " chunk
    writeDWord(f, fourCCToInt("fmt "));
    writeDWord(f, 18 + 22);

    // WAVEFORMATEX
    writeWord(f, 0xFFFE);  // wFormatTag
    writeWord(f, avctx->channels); // nChannels
    writeDWord(f, avctx->sample_rate); // nSamplesPerSec
    writeDWord(f, sample_size * avctx->channels * avctx->sample_rate); // nAvgBytesPerSec
    writeWord(f, sample_size * avctx->channels); // nBlockAlign
    writeWord(f, sample_size * 8); // wBitsPerSample
    writeWord(f, 22); // cbSize

    // WAVE_FORMAT_EXTENSIBLE
    writeWord(f, 4); // wValidBitsPerSample (in this case)
    writeDWord(f, avctx->channel_layout); // dwChannelMask
    if (avctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
        writeBytes(guid_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16, 1, f);
    } else {
        writeBytes(guid_KSDATAFORMAT_SUBTYPE_PCM, 16, 1, f);
    }

    // "data" chunk
    writeDWord(f, fourCCToInt("data"));
    writeDWord(f, data_size);

    return 0;
}

// Dump the raw data, let the EAE process it, and add the result to s->files.
static int eae_write_input_files(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    int i;
    int t;
    int err;
    eae_file *e;
    XFILE f = {.s = s};

    if (s->num_priming == s->num_buffered)
        return 0;

    if ((err = eae_next_path(avctx)) < 0)
        return err;

    // The previous write must be done (no overlap supported).
    eae_wait_on_io_thread(avctx);

    // If the previous write had an error, return it.
    if (s->io_status < 0)
        return s->io_status;

    s->io_status = 0;
    s->io_data_size = 0;
    av_freep(&s->io_tmp_file);
    av_freep(&s->io_out_file);

    s->io_tmp_file = av_strdup(s->path_in_tmp);
    s->io_out_file = av_strdup(s->path_in);
    if (!s->io_tmp_file || !s->io_out_file)
        return AVERROR(ENOMEM);

    if (av_codec_is_decoder(avctx->codec)) {
        if (s->frame_size < 1) {
            av_log(avctx, AV_LOG_ERROR, "invalid frame size\n");
            return AVERROR_UNKNOWN;
        }
    } else {
        int sample_count = 0;

        for (i = 0; i < s->num_buffered; i++) {
            int bps = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;
            int nb_samples = s->input[i].buffer->size / bps;
            sample_count += nb_samples;
            if (i != s->num_buffered - 1 || !s->eof) {
                // (s->eof might not be set if the last frame start encoding before EOF itself is sent)
                if (nb_samples != avctx->frame_size)
                    av_log(avctx, AV_LOG_WARNING, "wrong frame size %d (ignore if last frame)\n", nb_samples);
            }
        }

        if ((err = eae_write_wav_header(avctx, &f, sample_count)) < 0)
            return err;
    }

    for (i = 0; i < s->num_buffered; i++) {
        AVBufferRef *frame = s->input[i].buffer;
        writeBytes(frame->data, frame->size, 1, &f);
    }

    if (f.had_error)
        return AVERROR(ENOMEM);

    pthread_mutex_lock(&s->io_lock);
    s->io_active = 1;
    pthread_cond_broadcast(&s->io_cond);
    pthread_mutex_unlock(&s->io_lock);

    av_assert0(s->num_priming < s->num_buffered);
    av_assert0(s->num_buffered >= 1);

    av_assert0(s->num_files < s->opt_max_files);

    e = &s->files[s->num_files++];

    e->path_in = s->path_in;
    s->path_in = NULL;
    e->path_out = s->path_out;
    s->path_out = NULL;

    e->discard_frames = s->num_priming;

    for (i = 0; i < s->num_buffered; i++) {
        e->frames[i] = s->input[i];
        e->frames[i].buffer = NULL;
    }

    // Drop all queued packets/frames, except the ones needed for the next "priming".
    t = FFMIN(s->num_buffered, s->num_min_priming);
    for (i = 0; i < s->num_buffered - t; i++)
        av_buffer_unref(&s->input[i].buffer);
    for (i = 0; i < t; i++)
        s->input[i] = s->input[s->num_buffered - t + i];
    s->num_buffered = t;
    s->num_priming = t;

    return 0;
}

// len==-1 means read until EOF
static int eae_read_file(AVBufferRef **out_buf, FILE *f, int len)
{
    if (len < 0) {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        len = ftell(f) - cur;
        fseek(f, cur, SEEK_SET);
    }

    *out_buf = av_buffer_alloc(len);
    if (!*out_buf)
        return AVERROR(ENOMEM);

    if (fread((*out_buf)->data, len, 1, f) != 1) {
        av_buffer_unref(out_buf);
        return AVERROR(EIO);
    }

    return 0;
}

static uint8_t readByte(FILE *file)
{
  uint8_t r = 0;
  fread(&r, 1, 1, file);
  return r;
}

static uint16_t readWord(FILE *file)
{
  uint8_t f = readByte(file);
  return f | ((unsigned)readByte(file) << 8);
}

static uint32_t readDWord(FILE *file)
{
  uint16_t f = readWord(file);
  return f | ((unsigned)readWord(file) << 16);
}

// Parse as wav file and read the contents as s->output_buffer.
static int eae_read_wav(AVCodecContext *avctx, FILE *f, eae_file *e)
{
    EAEContext *s = avctx->priv_data;
    uint32_t bps, isfloat, pcmsize;

    // While the input is a normal .wav file, we strictly parse only to the
    // extend we absolutely need, i.e. we have strict requirements on what the
    // EAE program outputs.
    // (And no, we can't use libavformat's code here.)

    if (readDWord(f) != fourCCToInt("RIFF"))
        return AVERROR_UNKNOWN;
    readDWord(f); // skip filesize
    readDWord(f); // skip WAVE
    if (readDWord(f) != fourCCToInt("fmt "))
        return AVERROR_UNKNOWN;
    if (readDWord(f) != 18 + 22) // fmt chunk size
        return AVERROR_UNKNOWN;
    readWord(f); // skip wFormatTag
    avctx->channels = readWord(f); // nChannels
    avctx->sample_rate = readDWord(f); // nSamplesPerSec
    readDWord(f); // skip nAvgBytesPerSec
    readWord(f); // skip nBlockAlign
    bps = readWord(f); // wBitsPerSample
    readWord(f); // skip cbSize
    avctx->bits_per_raw_sample = readWord(f); // wValidBitsPerSample (in this case)
    avctx->channel_layout = readDWord(f); // dwChannelMask
    // PCM or FLOAT GUID. Distinguish by the first byte, ignore rest.
    isfloat = readByte(f) == 0x03;
    fseek(f, 15, SEEK_CUR);
    if (readDWord(f) != fourCCToInt("data"))
        return AVERROR_UNKNOWN;
    pcmsize = readDWord(f); // chunk size

    if (bps == 16) {
        avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    } else if (bps == 32 && isfloat) {
        avctx->sample_fmt = AV_SAMPLE_FMT_FLT;
    } else if (bps == 32 && !isfloat) {
        avctx->sample_fmt = AV_SAMPLE_FMT_S32;
    } else {
        return AVERROR_UNKNOWN;
    }

    if (avctx->channels < 1 || avctx->channels > 8)
        return AVERROR_UNKNOWN;
    if (pcmsize > INT_MAX)
        return AVERROR_UNKNOWN;

    return eae_read_file(&s->output_buffer, f, (int)pcmsize);
}

static int eae_add_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    EAEContext *s = avctx->priv_data;
    eae_frame *frame;

    av_assert0(s->num_buffered < NUM_MAX_BUFFER);
    av_assert0(avpkt->buf);

    frame = &s->input[s->num_buffered];

    frame->buffer = av_buffer_ref(avpkt->buf);
    if (!frame->buffer)
        return AVERROR(ENOMEM);

    // AVPacket.data must point into the buffer ref, so this is ok.
    frame->buffer->data = avpkt->data;
    frame->buffer->size = avpkt->size;

    frame->pts = avpkt->pts;
    frame->frame_size = s->frame_size;

    s->num_buffered++;

    return 0;
}

// Append packet data to the prebuffer.
static int eae_merge_prebuffer_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    EAEContext *s = avctx->priv_data;
    int err;

    if (s->prebuffered_packet.size) {
        int start_offset = s->prebuffered_packet.size;
        if ((err = av_grow_packet(&s->prebuffered_packet, avpkt->size)) < 0)
            return err;
        av_assert0(av_buffer_is_writable(s->prebuffered_packet.buf));
        memcpy(s->prebuffered_packet.data + start_offset, avpkt->data, avpkt->size);
    } else {
        if ((err = av_packet_ref(&s->prebuffered_packet, avpkt)) < 0)
            return err;
    }

    return 0;
}

// Add the current prebuffer as packet, and discard the prebuffer.
static int eae_flush_prebuffer_packets(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;
    int err;

    s->frame_size = s->num_prebuffered_samples;
    s->num_prebuffered_samples = 0;
    err = eae_add_packet(avctx, &s->prebuffered_packet);
    av_packet_unref(&s->prebuffered_packet);

    return err;
}

// Return the frame size, or 0 if no major sync info available.
static int eae_parse_truehd_frame_size(AVCodecContext *avctx, const AVPacket *pkt)
{
    uint8_t *buf = pkt->data;
    int buf_size = pkt->size;
    GetBitContext gb;
    int length;
    int ret;

    if (pkt->size < 2)
        return AVERROR_INVALIDDATA;

    length = (AV_RB16(buf) & 0xfff) * 2;

    if (length < 4 || length > buf_size)
        return AVERROR_INVALIDDATA;

    init_get_bits(&gb, (buf + 4), (length - 4) * 8);

    if (show_bits_long(&gb, 31) == (0xf8726fba >> 1)) {
        MLPHeaderInfo mh;
        if ((ret = ff_mlp_read_major_sync(avctx, &mh, &gb)) < 0)
            return ret;
        return mh.access_unit_size;
    }

    // normally the parser will prevent this
    if (length != pkt->size)
        av_log(avctx, AV_LOG_WARNING, "unaligned packet?\n");

    return 0;
}

static int eae_parse_ac3_frame_header(uint8_t *data, int size,
                                      AC3HeaderInfo *header, int *eac3_sync)
{
    int ret;
    GetBitContext gb;

    if ((ret = init_get_bits8(&gb, data, size)) < 0)
        return ret;

    if ((ret = ff_ac3_parse_header(&gb, header)) < 0)
        return ret;

    if (eac3_sync) {
        int i;

        *eac3_sync = 1;

        skip_bits(&gb, 5); // skip bitstream id

        /* volume control params */
        for (i = 0; i < (header->channel_mode ? 1 : 2); i++) {
            skip_bits(&gb, 5);
            if (get_bits1(&gb))
                skip_bits(&gb, 8);
        }


        /* dependent stream channel map */
        if (header->frame_type == EAC3_FRAME_TYPE_DEPENDENT) {
            if (get_bits1(&gb))
                skip_bits(&gb, 16); // skip custom channel map
        }

        /* mixing metadata */
        if (get_bits1(&gb)) {
            /* center and surround mix levels */
            if (header->channel_mode > AC3_CHMODE_STEREO) {
                skip_bits(&gb, 2);
                if (header->channel_mode & 1)
                    skip_bits(&gb, 6);
                if (header->channel_mode & 4)
                    skip_bits(&gb, 6);
            }

            /* lfe mix level */
            if (header->lfe_on && get_bits1(&gb))
                skip_bits(&gb, 5);

            /* info for mixing with other streams and substreams */
            if (header->frame_type == EAC3_FRAME_TYPE_INDEPENDENT) {
                for (i = 0; i < (header->channel_mode ? 1 : 2); i++) {
                    if (get_bits1(&gb))
                        skip_bits(&gb, 6);  // skip program scale factor
                }
                if (get_bits1(&gb))
                    skip_bits(&gb, 6);  // skip external program scale factor
                /* skip mixing parameter data */
                switch(get_bits(&gb, 2)) {
                    case 1: skip_bits(&gb, 5);  break;
                    case 2: skip_bits(&gb, 12); break;
                    case 3: {
                        int mix_data_size = (get_bits(&gb, 5) + 2) << 3;
                        skip_bits_long(&gb, mix_data_size);
                        break;
                    }
                }
                /* skip pan information for mono or dual mono source */
                if (header->channel_mode < AC3_CHMODE_STEREO) {
                    for (i = 0; i < (header->channel_mode ? 1 : 2); i++) {
                        if (get_bits1(&gb)) {
                            /* note: this is not in the ATSC A/52B specification
                            reference: ETSI TS 102 366 V1.1.1
                                        section: E.1.3.1.25 */
                            skip_bits(&gb, 8);  // skip pan mean direction index
                            skip_bits(&gb, 6);  // skip reserved paninfo bits
                        }
                    }
                }
                /* skip mixing configuration information */
                if (get_bits1(&gb)) {
                    int blk;
                    for (blk = 0; blk < header->num_blocks; blk++) {
                        if (header->num_blocks == 1 || get_bits1(&gb))
                            skip_bits(&gb, 5);
                    }
                }
            }
        }

        /* informational metadata */
        if (get_bits1(&gb)) {
            skip_bits(&gb, 3);
            skip_bits(&gb, 2); // skip copyright bit and original bitstream bit
            if (header->channel_mode == AC3_CHMODE_STEREO)
                skip_bits(&gb, 4);
            if (header->channel_mode >= AC3_CHMODE_2F2R)
                skip_bits(&gb, 2);
            for (i = 0; i < (header->channel_mode ? 1 : 2); i++) {
                if (get_bits1(&gb))
                    skip_bits(&gb, 8); // skip mix level, room type, and A/D converter type
            }
            if (header->sr_code != 3)
                skip_bits1(&gb); // skip source sample rate code
        }

        // converter synchronization flag
        // yes, this is the only bit we actually want
        if (header->frame_type == EAC3_FRAME_TYPE_INDEPENDENT && header->num_blocks != 6)
            *eac3_sync = get_bits1(&gb);
    }

    return 0;
}

// Return size of the (E)AC3 frame (including dependent streams).
// Returns <0 on error, never returns 0.
static int eae_get_ac3_packet_size(uint8_t *data, int size)
{
    int frame_size;
    AC3HeaderInfo header;
    int err = eae_parse_ac3_frame_header(data, size, &header, NULL);
    if (err < 0)
        return err;
    if (header.frame_size < 1)
        return AVERROR_INVALIDDATA;

    if (header.frame_type == EAC3_FRAME_TYPE_DEPENDENT)
        return AVERROR_INVALIDDATA;

    frame_size = header.frame_size;

    while (frame_size < size) {
        // We might need to grab dependent substream packets too. Assume there's at
        // most 1 dependent substream, which should be always true for our uses.
        err = eae_parse_ac3_frame_header(data + frame_size, size - frame_size,
                                         &header, NULL);
        if (err < 0)
            return err;

        if (header.frame_type != EAC3_FRAME_TYPE_DEPENDENT)
            break;

        frame_size += header.frame_size;
    }

    return frame_size;
}

// Mangle input-packets in some way, and merge or queue them to the actual input
// buffer if applicable.
// Currently, this will merge TrueHD packets, so that each new packet starts
// with a major sync header.
static int eae_prequeue_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    EAEContext *s = avctx->priv_data;
    int frame_size;
    int err;

    if (!avpkt) {
        if (!s->prebuffered_packet.size)
            return 0;
        return eae_flush_prebuffer_packets(avctx);
    }

    if (avctx->codec_id == AV_CODEC_ID_EAC3) {
        // For EAC3, we must be sure EAE receives groups of 6 blocks. If the
        // number of blocks per packet is less than 6 (the maximum), we must
        // merge them to avoid cutting audio at the wrong boundaries. EAE will
        // simply discard packets until the next sync point.
        // The sync point could be retrieved exactly by parsing the EAC3
        // payload, but unfortunately the sync bit is placed inconveniently
        // behind lots of other data which is very annoying to skip.
        // In addition, this code retrieves the real frame_size.
        AC3HeaderInfo header;
        int sync;
        if ((err = eae_parse_ac3_frame_header(avpkt->data, avpkt->size, &header, &sync)) < 0)
            return err;

        if (s->num_prebuffered_samples == 0) {
            // Discard packets until we find a sync bit.
            if (!sync)
                return 0;

            // A .ts sample exists that has dependent frames, followed by plain
            // old AC3 frame. EAE will not produce any output if the input
            // consists only of dependent frames. Passing through all frames
            // produces artifacts, and apparently decodes _some_ of the
            // dependent frames.
            if (header.frame_type == EAC3_FRAME_TYPE_DEPENDENT)
                return 0;
        }

        if ((err = eae_merge_prebuffer_packet(avctx, avpkt)) < 0)
            return err;
        s->num_prebuffered_samples += header.num_blocks * 256;

        if (s->num_prebuffered_samples >= 6 * 256)
            return eae_flush_prebuffer_packets(avctx);

        return 0;
    }

    if (avctx->codec_id == AV_CODEC_ID_AC3) {
        // AC3 can start decoding on any packet.
        s->frame_size = 256 * 6;
        return eae_add_packet(avctx, avpkt);
    }

    if ((frame_size = eae_parse_truehd_frame_size(avctx, avpkt)) < 0)
        return frame_size;

    if (s->prebuffered_packet.size) {
        // We already have data, and this packet starts a new major sync, so
        // flush out the old one.
        if (frame_size) {
            if ((err = eae_flush_prebuffer_packets(avctx)) < 0)
                return err;
        }
    } else {
        // Starting a new "complete" packet. We need major sync info.
        if (!frame_size) {
            av_log(avctx, AV_LOG_ERROR, "does not start with major sync!\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if ((err = eae_merge_prebuffer_packet(avctx, avpkt)) < 0)
        return err;

    if (frame_size)
        s->prebuffered_au_size = frame_size;
    if (frame_size && frame_size != s->prebuffered_au_size)
        av_log(avctx, AV_LOG_ERROR, "AU size mismatch: %d != %d\n",
               frame_size, s->prebuffered_au_size);
    s->num_prebuffered_samples += s->prebuffered_au_size;

    // Avoid pathological corner cases OOM'ing us.
    if (s->num_prebuffered_samples > 1000000) {
        av_log(avctx, AV_LOG_ERROR, "no major sync found, dropping\n");
        av_packet_unref(&s->prebuffered_packet);
        s->num_prebuffered_samples = 0;
    }

    return 0;
}

static int eae_need_input(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    if (s->output_buffer || s->num_files == s->opt_max_files)
        return 0;

    return s->num_buffered < s->num_min_buffered;
}

static int eae_process_input(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    if (s->num_buffered >= s->num_min_buffered || s->eof) {
        int err = eae_write_input_files(avctx);
        if (err < 0)
            eae_flush(avctx);
        return err;
    }

    return 0;
}

static int eae_decode_send_packet(AVCodecContext *avctx, const AVPacket *avpkt)
{
    EAEContext *s = avctx->priv_data;
    int err;

    if (!eae_need_input(avctx))
        return AVERROR(EAGAIN);

    if ((err = eae_prequeue_packet(avctx, avpkt)) < 0)
        return err;

    if (!avpkt)
        s->eof = 1;

    return eae_process_input(avctx);
}

static int eae_encode_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    EAEContext *s = avctx->priv_data;

    if (!eae_need_input(avctx))
        return AVERROR(EAGAIN);

    if (frame) {
        eae_frame *e = &s->input[s->num_buffered];

        av_assert0(!frame->buf[1]);

        e->buffer = av_buffer_ref(frame->buf[0]);
        if (!e->buffer)
            return AVERROR(ENOMEM);

        // AVFrame.data[0] must point into the buffer ref, so this is ok.
        e->buffer->data = frame->data[0];
        e->buffer->size = frame->nb_samples * av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;

        e->pts = frame->pts;

        s->num_buffered++;
    } else {
        s->eof = 1;
    }

    return eae_process_input(avctx);
}

static int eae_read_output(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    if (!s->output_buffer) {
        struct eae_file *e;
        FILE *f = NULL;
        int err = 0;

        if (!(s->num_files >= s->opt_max_files || (s->eof && s->num_files)))
            return s->eof ? AVERROR_EOF : AVERROR(EAGAIN);

        e = &s->files[0];

        f = eae_wait_and_open(avctx, e->path_out);
        if (!f) {
            err = AVERROR(EIO);
            goto done;
        }

        av_assert0(sizeof(s->output_frames) == sizeof(e->frames));
        memcpy(s->output_frames, e->frames, sizeof(s->output_frames));
        s->output_discard_frames = e->discard_frames;

        if (av_codec_is_decoder(avctx->codec)) {
            err = eae_read_wav(avctx, f, e);
        } else {
            err = eae_read_file(&s->output_buffer, f, -1);
        }

    done:
        unlink(e->path_in);
        av_freep(&e->path_in);

        if (f)
            fclose(f);

        unlink(e->path_out);
        av_freep(&e->path_out);

        for (int n = 0; n < s->num_files - 1; n++)
            s->files[n] = s->files[n + 1];
        s->num_files--;

        if (err < 0) {
            av_log(avctx, AV_LOG_ERROR, "error reading output\n");
            return err;
        }
    }

    if (s->output_buffer)
        return 0;

    return s->eof ? AVERROR_EOF : AVERROR(EAGAIN);
}

// Remove the first output PTS, and shift the ones above it into its place.
static void eae_remove_output_frame(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    memmove(&s->output_frames[0], &s->output_frames[1],
            sizeof(s->output_frames) - sizeof(s->output_frames[0]));
}

static int eae_decode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    EAEContext *s = avctx->priv_data;
    int samples;
    int blocksize;
    int err = eae_read_output(avctx);
    if (err < 0)
        return err;

    av_assert0(s->output_buffer);

    if ((err = ff_decode_frame_props(avctx, frame)) < 0)
        return err;

    blocksize = av_get_bytes_per_sample(avctx->sample_fmt) * avctx->channels;
    samples = s->output_buffer->size / blocksize;
    if (samples * blocksize != s->output_buffer->size) {
        av_log(avctx, AV_LOG_ERROR, "EAE output with odd size\n");
        av_buffer_unref(&s->output_buffer);
        return AVERROR_UNKNOWN;
    }

    // Remove the bogus data produced by "priming" packets.
    while (s->output_discard_frames && samples > s->output_frames[0].frame_size) {
        int frame_size = s->output_frames[0].frame_size * blocksize;
        s->output_buffer->data += frame_size;
        s->output_buffer->size -= frame_size;
        s->output_discard_frames--;
        eae_remove_output_frame(avctx);
    }

    samples = FFMIN(samples, s->output_frames[0].frame_size);

    if (samples != s->output_frames[0].frame_size && !s->eof)
        av_log(avctx, AV_LOG_WARNING, "EAE output unaligned (got %d, expected %d)\n",
               samples, s->output_frames[0].frame_size);

    frame->buf[0] = av_buffer_ref(s->output_buffer);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->buf[0]->size = samples * blocksize;

    frame->data[0] = frame->buf[0]->data;
    frame->linesize[0] = frame->buf[0]->size;
    frame->pts = frame->pkt_dts = s->output_frames[0].pts;
    frame->nb_samples = samples;

    if (frame->pts == AV_NOPTS_VALUE) {
        if (avctx->pkt_timebase.num && avctx->pkt_timebase.den) {
            frame->pts = av_add_stable(avctx->pkt_timebase,
                                       s->prev_pts,
                                       (AVRational){1, avctx->sample_rate},
                                       s->prev_pts_samples);
        }
    } else {
        s->prev_pts = frame->pts;
        s->prev_pts_samples = 0;
    }
    s->prev_pts_samples += frame->nb_samples;

    s->output_buffer->data += frame->buf[0]->size;
    s->output_buffer->size -= frame->buf[0]->size;
    eae_remove_output_frame(avctx);

    av_log(avctx, AV_LOG_TRACE, "decoded frame samples=%d pts=%lld\n",
           frame->nb_samples, (long long)frame->pts);

    if (s->output_buffer->size == 0)
        av_buffer_unref(&s->output_buffer);

    return 0;
}

static int eae_encode_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    EAEContext *s = avctx->priv_data;
    AVBufferRef *buffer;
    int frame_size;
    uint8_t *frame_data;
    int64_t frame_pts;
    int err = eae_read_output(avctx);
    if (err < 0)
        return err;

    av_assert0(s->output_buffer);

    buffer = s->output_buffer;

    while (1) {
        frame_pts = s->output_frames[0].pts;
        frame_data = buffer->data;
        frame_size = err = eae_get_ac3_packet_size(buffer->data, buffer->size);
        if (err < 0) {
            av_log(avctx, AV_LOG_ERROR, "got broken frame from EAE\n");
            goto error;
        }
        av_assert0(frame_size > 0);

        if (frame_size > buffer->size) {
            av_log(avctx, AV_LOG_ERROR, "frame cut short: %d\n", buffer->size);
            err = AVERROR(EINVAL);
            goto error;
        }

        buffer->data += frame_size;
        buffer->size -= frame_size;

        eae_remove_output_frame(avctx);

        if (s->output_discard_frames == 0)
            break;

        s->output_discard_frames--;
    }

    avpkt->buf = av_buffer_ref(buffer);
    if (!avpkt->buf) {
        err = AVERROR(ENOMEM);
        goto error;
    }

    avpkt->buf->data = frame_data;
    avpkt->buf->size = frame_size;
    avpkt->data = frame_data;
    avpkt->size = frame_size;

    avpkt->pts = avpkt->dts = frame_pts;
    avpkt->flags |= AV_PKT_FLAG_KEY;

    if (buffer->size == 0)
        av_buffer_unref(&s->output_buffer);

    return 0;

error:
    av_buffer_unref(&s->output_buffer);
    return err;
}

static int eae_close(AVCodecContext *avctx)
{
    EAEContext *s = avctx->priv_data;

    eae_flush(avctx);

    if (s->io_thread_valid) {
        pthread_mutex_lock(&s->io_lock);
        s->io_terminate = 1;
        pthread_cond_broadcast(&s->io_cond);
        pthread_mutex_unlock(&s->io_lock);

        pthread_join(s->io_thread, NULL);

        pthread_mutex_destroy(&s->io_lock);
        pthread_cond_destroy(&s->io_cond);
    }

    av_freep(&s->io_tmp_file);
    av_freep(&s->io_out_file);
    av_buffer_unref(&s->io_data);

    av_freep(&s->path_in);
    av_freep(&s->path_out);
    av_freep(&s->path_in_tmp);
    av_freep(&s->path_prefix);
    av_freep(&s->root);

    return 0;
}

#define OPTFLAGS AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
{ "eae_root", "EAE root path", offsetof(EAEContext, opt_eae_root), AV_OPT_TYPE_STRING, {.str = "" }, 0, 0, OPTFLAGS },
{ "eae_prefix", "EAE file prefix", offsetof(EAEContext, opt_eae_prefix), AV_OPT_TYPE_STRING, {.str = "frame-" }, 0, 0, OPTFLAGS },
{ "eae_batch_frames", "EAE number of frames for each pass", offsetof(EAEContext, opt_eae_batch_frames), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1000, OPTFLAGS },
{ "eae_max_files", "EAE number of files on disk", offsetof(EAEContext, opt_max_files), AV_OPT_TYPE_INT, {.i64 = 2}, 1, NUM_MAX_FILES, OPTFLAGS },
{ NULL },
};

static const AVClass eae_ac3_decoder_class = {
    "EAE AC3 decoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_ac3_eae_decoder = {
    .name           = "ac3_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE AC3"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AC3,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_decode_init,
    .close          = eae_close,
    .send_packet    = eae_decode_send_packet,
    .receive_frame  = eae_decode_receive_frame,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class     = &eae_ac3_decoder_class,
};

static const AVClass eae_eac3_decoder_class = {
    "EAE EAC3 decoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_eac3_eae_decoder = {
    .name           = "eac3_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE EAC3"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_EAC3,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_decode_init,
    .close          = eae_close,
    .send_packet    = eae_decode_send_packet,
    .receive_frame  = eae_decode_receive_frame,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class     = &eae_eac3_decoder_class,
};

static const AVClass eae_truehd_decoder_class = {
    "EAE TrueHD decoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_truehd_eae_decoder = {
    .name           = "truehd_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE TrueHD"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_TRUEHD,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_decode_init,
    .close          = eae_close,
    .send_packet    = eae_decode_send_packet,
    .receive_frame  = eae_decode_receive_frame,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class     = &eae_truehd_decoder_class,
};

static const AVClass eae_mlp_decoder_class = {
    "EAE MLP decoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_mlp_eae_decoder = {
    .name           = "mlp_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE MLP"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_MLP,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_decode_init,
    .close          = eae_close,
    .send_packet    = eae_decode_send_packet,
    .receive_frame  = eae_decode_receive_frame,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class     = &eae_mlp_decoder_class,
};

static const AVClass eae_ac3_encoder_class = {
    "EAE AC3 encoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_ac3_eae_encoder = {
    .name           = "ac3_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE AC3"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AC3,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_encode_init,
    .close          = eae_close,
    .send_frame     = eae_encode_send_frame,
    .receive_packet = eae_encode_receive_packet,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_FLT,
                                                      AV_SAMPLE_FMT_S16,
                                                      AV_SAMPLE_FMT_NONE },
    .supported_samplerates = (const int[]) { 48000, 0 },
    .channel_layouts = (const uint64_t[]) { AV_CH_LAYOUT_STEREO,
                                            AV_CH_LAYOUT_5POINT1, 0 },
    .priv_class     = &eae_ac3_encoder_class,
};

static const AVClass eae_eac3_encoder_class = {
    "EAE EAC3 encoder",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_eac3_eae_encoder = {
    .name           = "eac3_eae",
    .long_name      = NULL_IF_CONFIG_SMALL("EAE EAC3"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_EAC3,
    .priv_data_size = sizeof(EAEContext),
    .init           = eae_encode_init,
    .close          = eae_close,
    .send_frame     = eae_encode_send_frame,
    .receive_packet = eae_encode_receive_packet,
    .flush          = eae_flush,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP,
    .sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_FLT,
                                                      AV_SAMPLE_FMT_S16,
                                                      AV_SAMPLE_FMT_NONE },
    .supported_samplerates = (const int[]) { 48000, 0 },
    .channel_layouts = (const uint64_t[]) { AV_CH_LAYOUT_5POINT1,
                                            AV_CH_LAYOUT_7POINT1, 0 },
    .priv_class     = &eae_eac3_encoder_class,
};
