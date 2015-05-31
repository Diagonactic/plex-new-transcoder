/*
 * HTTP protocol for ffmpeg client
 * Copyright (c) 2010 Frank Bauer
 *
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

#ifdef __linux__
#include <arpa/inet.h>
#endif
#include <curl/curl.h>
#include "ffmpeg.h"
#include "ffmpeg_plex.h"
#include <sys/types.h>
#ifdef __WIN32__
#include <winsock2.h>
#include <Ws2tcpip.h>
#include "stringconversions.h"
#define SocketErrno (WSAGetLastError())
#define bcopy(src,dest,len) memmove(dest,src,len)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

char *executable_path;
char *subtitle_path;
int PLEX_DEBUG = 0;

enum{
    MACHINE_UNKNOWN		= 0x0000,
    MACHINE_NOTEBOOK	= 0x0100,
    MACHINE_MACBOOK		= 0x0001,
    MACHINE_MACBOOKPRO	= 0x0002,
    MACHINE_MACMINI		= 0x0103,
    MACHINE_IMAC		= 0x0104,
    MACHINE_MACPRO		= 0x0105,
};

typedef struct PLEXMachineType{
    int majorVersion;
    int minorVersion;
    int type;
} PLEXMachineType;

char* _PLEXMachineTypeNames[6] = {"Unknown", "MacBook", "MacBookPro", "MacMini", "iMac", "MacPro"};

static PLEXMachineType getHardwareModel(){
    size_t len;
    char *p;
    int mib[2];
    PLEXMachineType mt = {0, 0, MACHINE_UNKNOWN};
    
#ifdef __APPLE__
    mib[0] = CTL_HW;
    mib[1] = HW_MODEL;
    sysctl(mib, 2, NULL, &len, NULL, 0);
    p = (char*)malloc(len);
    sysctl(mib, 2, p, &len, NULL, 0);
    
    if (strstr(p, "iMac")) mt.type = MACHINE_IMAC;
    else if (strstr(p, "MacBookPro")) mt.type = MACHINE_MACBOOKPRO;
    else if (strstr(p, "MacBook")) mt.type = MACHINE_MACBOOK;
    else if (strstr(p, "Mini")) mt.type = MACHINE_MACMINI;
    else if (strstr(p, "Mac")) mt.type = MACHINE_MACPRO;
    
    int ver[2] = {0, 0};
    char f = 0;
    int base = 1;
    for (int i=strlen(p)-1; i>=0; i--){
        char c = p[i];
        if (c>='0' && c<='9'){
            ver[f] += (c-'0')*base;
            base *= 10;
        } else {
            if (f==0) {
                f=1;
                base=1;
            } else break;
        } 
    }
    mt.majorVersion = ver[1];
    mt.minorVersion = ver[0];
    free(p);
#endif
    
    return mt;
}

static uint64_t getCpuFrequency(){ //in MHz
#ifdef __APPLE__
    uint64_t freq = 0;
    size_t size = sizeof(freq);
    
    if (sysctlbyname("hw.cpufrequency", &freq, &size, NULL, 0) < 0){
        return 0xffffffffffffffff;
    }
    return freq/(1000*1000);
#else
#warning "Need to add this"
    return 2000;
#endif
}

static uint64_t getBusFrequency(){ //in MHz
#ifdef __APPLE__
    uint64_t freq = 0;
    size_t size = sizeof(freq);
    
    if (sysctlbyname("hw.busfrequency", &freq, &size, NULL, 0) < 0){
        return 0xffffffffffffffff;
    }
    return freq/(1000*1000);
#else
#warning "Need to add this"
    return 1500;
#endif
}

static uint64_t getNumberOfCores(){
#if defined(__APPLE__) || defined(__linux__)
    return sysconf(_SC_NPROCESSORS_ONLN);
#else
#warning "Need to add this"
    return 2;
#endif
}

//Fastest CPU has returns 1.0, slower return higher numbers
float PLEXcpuQualityFactor(){
    float ret = 1.0f;
    const long cores = getNumberOfCores();
    const uint64_t cpuMHz = cores * getCpuFrequency() / 1.85; //1.85: accounts for HT cores
    const uint64_t busMHz = getBusFrequency();
    const PLEXMachineType mt = getHardwareModel();
    
    const float CPU_C2D = 1.5;
    if (mt.type==MACHINE_IMAC){
        if (mt.majorVersion<11) ret *= CPU_C2D;
    } else if (mt.type==MACHINE_MACBOOKPRO){
        if (mt.majorVersion<6) ret *= CPU_C2D;
    } else if (mt.type== MACHINE_MACMINI){
        ret *= 1.4;
    } else if (mt.type== MACHINE_MACBOOK){
        ret *= 1.4;
    } 
    
    float ref = cpuMHz / 6800.0f;
    if (ref<1.0f){
        ret /= ref;
    }
    
    return MAX(1.0f, ret);
}

void PLEXshowAutoCPUInfo(){
    const uint64_t cores = getNumberOfCores();
    const uint64_t cpuMHz = getCpuFrequency();
    const uint64_t busMHz = getBusFrequency();
    const float qualityFactor = PLEXcpuQualityFactor();
    const PLEXMachineType mt = getHardwareModel();
    
    av_log(NULL, AV_LOG_INFO, "\n---- Auto Quality ----\n");
    av_log(NULL, AV_LOG_INFO, "    CPU Cores=%llu\n", cores);
    av_log(NULL, AV_LOG_INFO, "    CPU MHz=%llu\n", cpuMHz);
    av_log(NULL, AV_LOG_INFO, "    BUS MHz=%llu\n", busMHz);
    av_log(NULL, AV_LOG_INFO, "    Machine=%s (%d.%d)\n", _PLEXMachineTypeNames[mt.type&0xFF], mt.majorVersion, mt.minorVersion);
    av_log(NULL, AV_LOG_INFO, "    CPU Quality Factor=%0.2f\n", qualityFactor);
    getHardwareModel();
}

static void PLEXsplitAudioCodecCapsFromString(const char* audioCodecs, PLEXClientVideoCaps* caps){
    const int MAX_BUFFER = strlen(audioCodecs)+1;
    char namebuffer[MAX_BUFFER];
    char bitbuffer[MAX_BUFFER];
    
    unsigned int pos = 0;
    unsigned int namebufferpos = 0;
    unsigned int bitbufferpos = 0;
    unsigned char readname = 1;
    
    
    caps->audioCodec = NULL;
    caps->mp3MaxBitrate = -1;
    caps->aacMaxBitrate = -1;
    caps->ac3MaxBitrate = -1;
    caps->dtsMaxBitrate = -1;
    caps->pcmMaxBitrate = -1;
    
    while(audioCodecs[pos]!=0){
        
        unsigned char finishedPart = audioCodecs[pos+1]==0;
        
        if (readname){
            if (audioCodecs[pos]=='='){
                namebuffer[namebufferpos] = 0;
                readname = 0;
            } else if (audioCodecs[pos]==','){
                namebuffer[namebufferpos] = 0;
                finishedPart = 1;
            } else {
                namebuffer[namebufferpos++] = audioCodecs[pos];
            }
        } else {
            if (audioCodecs[pos]==','){
                bitbuffer[bitbufferpos] = 0;
                finishedPart = 1;
            } else {
                bitbuffer[bitbufferpos++] = audioCodecs[pos];
            }
        }
        
        
        
        if (finishedPart && namebufferpos>0){
            //makes ure we terminate the string
            namebuffer[namebufferpos] = 0;
            bitbuffer[bitbufferpos] = 0;
            
            if (strcmp(namebuffer, "mp3")==0){
                if (bitbufferpos>0){
                    caps->mp3MaxBitrate = atoi(bitbuffer);
                } else {
                    caps->mp3MaxBitrate = 0;
                }
                if (caps->audioCodec==NULL) caps->audioCodec="libmp3lame";
            } else if (strcmp(namebuffer, "aac")==0 ){                
                caps->aacProfile = FF_PROFILE_AAC_LOW;
                
                if (bitbufferpos>0){
                    caps->aacMaxBitrate = atoi(bitbuffer);
                } else {
                    caps->aacMaxBitrate = 0;
                }
                if (caps->audioCodec==NULL) caps->audioCodec="libvo_aacenc";
            } else if (strcmp(namebuffer, "aac_main")==0){
                caps->aacProfile = FF_PROFILE_AAC_MAIN;
                
                if (bitbufferpos>0){
                    caps->aacMaxBitrate = atoi(bitbuffer);
                } else {
                    caps->aacMaxBitrate = 0;
                }
                if (caps->audioCodec==NULL) caps->audioCodec="libvo_aacenc";
            } else if (strcmp(namebuffer, "ac3")==0){
                if (bitbufferpos>0){
                    caps->ac3MaxBitrate = atoi(bitbuffer);
                    
                    // If less than 1MBps is specified, the client is on crack.
                    if (caps->ac3MaxBitrate < 1000000)
                      caps->ac3MaxBitrate = 3000000;
                    
                } else {
                    caps->ac3MaxBitrate = 0;
                }
                if (caps->audioCodec==NULL) caps->audioCodec="ac3";
            }else if (strcmp(namebuffer, "dts")==0){
                if (bitbufferpos>0){
                    caps->dtsMaxBitrate = atoi(bitbuffer);
                } else {
                    caps->dtsMaxBitrate = 0;
                }
            }else if (strcmp(namebuffer, "pcm")==0){
                if (bitbufferpos>0){
                    caps->pcmMaxBitrate = atoi(bitbuffer);
                } else {
                    caps->pcmMaxBitrate = 0;
                }
            }
            
            bitbufferpos = 0;
            namebufferpos = 0;
            readname = 1;
        }
        
        pos++;
    }
    
    //fall back on mp3 encoding
    if (caps->audioCodec == NULL) caps->audioCodec = "libmp3lame";
}

void PLEXsetupContext(STMTranscodeContext *context, int argc, char **argv){
    if (argc >PLEX_AUTO_CPU_QUALITY)
        context->plex.autodetect_cpu_quality = (strcmp(argv[PLEX_AUTO_CPU_QUALITY], "yes") == 0);
    else
        context->plex.autodetect_cpu_quality = 0;
    
    //setup defaults
    context->plex.transcodeToFileArgc = 0;
    context->plex.transcodeToFileArgv = NULL;
    context->plex.videoCaps.profile = FF_PROFILE_H264_MAIN;
    context->plex.videoCaps.resolution = 720;
    context->plex.videoCaps.level = 31;
    
    context->plex.videoCaps.maxSegmentSizePerSecond = 3.2 * 1024 * 1024;
    context->plex.videoCaps.playsAnamorphic = 0;
    context->plex.useFpsFilter = 0;
    context->plex.progressURL = 0;
    context->plex.delay = 0;
    
    char* transcodeOptions = "nil";
    
    /*fprintf(stderr, "args %i\n", argc);
     for (int i=0; i<argc; i++){
     fprintf(stderr, "- %i:%s\n", i, argv[i]);
     }*/
    
    if (argc>TRANSCODE_OPTIONS) transcodeOptions =  argv[TRANSCODE_OPTIONS]; 
    if (strcmp("av", transcodeOptions)==0)
    {
        context->plex.videoCaps.allowAudioCopy = 1;
        context->plex.videoCaps.allowVideoCopy = 1;
    } 
    else if (strcmp("a", transcodeOptions)==0){
        context->plex.videoCaps.allowAudioCopy = 1;
        context->plex.videoCaps.allowVideoCopy = 0;
    }
    else if (strcmp("v", transcodeOptions)==0){
        context->plex.videoCaps.allowAudioCopy = 0;
        context->plex.videoCaps.allowVideoCopy = 1;
    } else {
        context->plex.videoCaps.allowAudioCopy = 0;
        context->plex.videoCaps.allowVideoCopy = 0;
    }
    
    if (argc>PLEX_VIDEO_PROFILE) {
        if (strcmp("baseline", argv[PLEX_VIDEO_PROFILE])==0)
            context->plex.videoCaps.profile = FF_PROFILE_H264_BASELINE; 
        else if (strcmp("high", argv[PLEX_VIDEO_PROFILE])==0)
            context->plex.videoCaps.profile = FF_PROFILE_H264_HIGH;
        else if (strcmp("extended", argv[PLEX_VIDEO_PROFILE])==0)
            context->plex.videoCaps.profile = FF_PROFILE_H264_EXTENDED;
    }
    
    if (argc>PLEX_VIDEO_RESOLUTION) {
        context->plex.videoCaps.resolution = atoi(argv[PLEX_VIDEO_RESOLUTION]);
    }
    
    if (argc>PLEX_VIDEO_LEVEL) {
        context->plex.videoCaps.level = atoi(argv[PLEX_VIDEO_LEVEL]);
    }
    
    if (argc>PLEX_AUDIO_COPY_BITRATE) {
        PLEXsplitAudioCodecCapsFromString(argv[PLEX_AUDIO_COPY_BITRATE], &context->plex.videoCaps);
    } else {
        PLEXsplitAudioCodecCapsFromString("mp3", &context->plex.videoCaps);
    }
    
    if (argc>PLEX_SEGSIZE_PER_SECOND) {
        context->plex.videoCaps.maxSegmentSizePerSecond = atoi(argv[PLEX_SEGSIZE_PER_SECOND]);
    }
    
    if (argc>PLEX_PLAYS_ANAMORPHIC) {
        if (strcmp("yes", argv[PLEX_PLAYS_ANAMORPHIC])==0)
            context->plex.videoCaps.playsAnamorphic = 1;
        else
            context->plex.videoCaps.playsAnamorphic = 0;
    }
    
    if (argc>PLEX_FPS_FILTER) {
        if (strcmp("yes", argv[PLEX_FPS_FILTER])==0)
            context->plex.useFpsFilter = 1;
        else
            context->plex.useFpsFilter = 0;
    }
    
    if (argv >PLEX_PROGRESS_URL)
      context->plex.progressURL = (char* )argv[PLEX_PROGRESS_URL];
    
    PMS_Log(LOG_LEVEL_DEBUG, "Video caps: ca=%i, cv=%i, ac=%s, mp3=%i, aac=%i,%i, ac3=%i, dts=%i, pcm=%i, vp=%x, vr=%i, vl=%i, mss=%i, anam=%i", context->plex.videoCaps.allowAudioCopy, context->plex.videoCaps.allowVideoCopy, context->plex.videoCaps.audioCodec, context->plex.videoCaps.mp3MaxBitrate, context->plex.videoCaps.aacMaxBitrate, context->plex.videoCaps.aacProfile, context->plex.videoCaps.ac3MaxBitrate, context->plex.videoCaps.dtsMaxBitrate, context->plex.videoCaps.pcmMaxBitrate, context->plex.videoCaps.profile, context->plex.videoCaps.resolution, context->plex.videoCaps.level, context->plex.videoCaps.maxSegmentSizePerSecond, context->plex.videoCaps.playsAnamorphic);
}

void PLEXsetupBaseTranscoderSettings(STMTranscodeContext *context,
                                     char *audio_stream_specifier,
                                     char *subtitle_stream_specifier,
                                     float audio_gain,
                                     char *srt_encoding,
                                     int audioStreamIndex,
                                     int videoStreamIndex,
                                     STMTranscodeSettings * settings,
                                     const char* output_path){
    
    PLEXClientVideoCaps const * const videoCaps = &context->plex.videoCaps;
    
    settings->sts_output_dts_delta_threshold = 30;
    settings->sts_output_duration = context->stc_input_files[0]->duration;
    settings->sts_output_file_format = "mpegts";
    
    settings->sts_audio_codec_name = audioStreamIndex == -1 ? NULL : videoCaps->audioCodec;
    settings->sts_audio_sample_rate = 48000;
    settings->sts_audio_drift_threshold = 0.025;
    settings->sts_audio_output_volume = 256.0f * audio_gain;
    settings->sts_audio_stream_specifier = audio_stream_specifier;
    settings->sts_audio_channels = 2;
    settings->sts_audio_bitrate = 128 * 1000;
    
    settings->sts_video_codec_name = videoStreamIndex == -1 ? NULL : "libx264";
    settings->sts_video_level = 30;
    settings->sts_video_profile = "baseline";
    
    settings->sts_video_frame_rate.num = settings->sts_source_video_frame_rate.num;
    settings->sts_video_frame_rate.den = settings->sts_source_video_frame_rate.den;
    
    settings->sts_video_me_range = 16;
    settings->sts_video_me_method = "hex";
    settings->sts_video_sc_threshold = 0;
    settings->sts_video_qmin = 10;
    settings->sts_video_qmax = 51;
    settings->sts_video_qdiff = 4;
    settings->sts_video_flags = "+loop";
    settings->sts_video_flags2 = "-wpred-dct8x8";
    settings->sts_video_cmp = "+chroma";
    settings->sts_video_partitions = "+parti8x8+parti4x4+partp8x8+partb8x8";
    settings->sts_video_crf = 22;
    settings->sts_video_frame_width = 640;
    settings->sts_video_frame_height = 480;
    settings->sts_video_bufsize = 2048 * 1024;
    settings->sts_video_minrate = 64 * 1024;
    settings->sts_video_maxrate = 1440 * 1024;
    settings->sts_video_qcomp = 0.6;
    settings->sts_video_subq = 4;
    settings->sts_video_frame_refs = 1;
    
    //we will do an audio only transcoding?
    if (videoStreamIndex == -1){
        settings->sts_output_file_format = context->plex.videoCaps.aacMaxBitrate<0?"mp3":"aac";
        
        
        const char* path_ext = context->plex.videoCaps.aacMaxBitrate<0?"00.mp3":"00.aac";
        char * path = malloc((strlen(path_ext) + strlen(output_path)) * sizeof(char));
        strcpy(path, output_path);
        strcpy(path + strlen(output_path), path_ext);
        
        const int argc = 5;
        char* args[] = {"plex-loop", "-i", context->stc_settings.sts_input_file_name, "-y", path};
        if (context->plex.transcodeToFileArgv!=NULL){
            free(context->plex.transcodeToFileArgv);
        }
        context->plex.transcodeToFileArgv = malloc(sizeof(char*) * argc);
        context->plex.transcodeToFileArgc = argc;
        for (int i=0; i<argc; i++){
            char* str = malloc((strlen(args[i])+1)*sizeof(char));
            strcpy(str, args[i]);
            str[strlen(args[i])] = 0;
            context->plex.transcodeToFileArgv[i] = str;
        }
    }
}

char* PLEXbitrateString(unsigned long long val){
    char* unit = "";
    
    if (val>1024ll){
        val /= 1024;
        unit = "k";
    }
    if (val>1024ll){
        val /= 1024;
        unit = "m";
    }
    
    char* ret[256];
    sprintf(ret, "%llu%s" , val, unit);
    return ret;
}

void PLEXsetupAudioTranscoderSettingsForQuality(STMTranscodeContext *context,
                                                char *audio_stream_specifier,
                                                char *stream_quality_name,
                                                float audio_gain,
                                                int audioStreamIndex,
                                                STMTranscodeSettings * settings){
    
    PLEXClientVideoCaps const * const videoCaps = &context->plex.videoCaps;
    int quality = atoi(stream_quality_name);
    
    AVFormatContext *format = context->stc_input_files[0];
    AVCodecContext *audioCodec = format->streams[audioStreamIndex]->codec;

    // Maybe we can transcode to another multi-channel format, but only do so for higher qualities.
    if (quality > 4)
    {
      if ((    (audioCodec->codec_id == CODEC_ID_DTS && videoCaps->dtsMaxBitrate<0) 
           ||  (audioCodec->codec_id == CODEC_ID_PCM_BLURAY  && videoCaps->pcmMaxBitrate<0)
           ||  (audioCodec->codec_id == CODEC_ID_PCM_DVD  && videoCaps->pcmMaxBitrate<0)
           ||  (audioCodec->codec_id == CODEC_ID_AC3  && videoCaps->ac3MaxBitrate<0)
           ||  (audioCodec->codec_id == CODEC_ID_AAC  && videoCaps->aacMaxBitrate<0)
           ) && audioCodec->channels>2)
      {
        const uint64_t suggestedBitrate = audioCodec->channels * 96*1000;
        
        if (videoCaps->ac3MaxBitrate>=0)
        {
            fprintf(stderr, "!!! Transcoding ->AC3 !!!\n");
            settings->sts_audio_codec_name = "ac3_fixed";
            
            settings->sts_audio_sample_rate = audioCodec->sample_rate;
            settings->sts_audio_channels = audioCodec->channels;
            settings->sts_audio_bitrate = videoCaps->ac3MaxBitrate==0
                                            ? suggestedBitrate
                                            : MIN(videoCaps->ac3MaxBitrate, suggestedBitrate);
        } /*else if (videoCaps->aacMaxBitrate>=0){
            fprintf(stderr, "!!! Transcoding ->AAC !!!\n");
            settings->sts_audio_codec_name = "aac";
            
            settings->sts_audio_sample_rate = audioCodec->sample_rate;
            settings->sts_audio_channels = audioCodec->channels;
            settings->sts_audio_bitrate = videoCaps->ac3MaxBitrate==0
                                            ? suggestedBitrate
                                            : MIN(videoCaps->ac3MaxBitrate, suggestedBitrate);
        }*/
      }
    } 
    
    // Downmixing aac audio to MP3 does not work, so we mix it down to aac
    if ((audioCodec->codec_id == CODEC_ID_AAC) && 
        audioCodec->channels>2)
    {
        fprintf(stderr, "!!! Turn off audio sync !!!\n");
        plex_audio_sync_method = 0;
    }
    
    // See if we're doing audio boost.
    if ((audioCodec->codec_id == CODEC_ID_AC3 || audioCodec->codec_id == CODEC_ID_DTS) && 
        audioCodec->channels>2 && 
        (strcmp(settings->sts_audio_codec_name, "libmp3lame")==0 || 
         strcmp(settings->sts_audio_codec_name, "libvo_aacenc")==0 || 
         strcmp(settings->sts_audio_codec_name, "aac")==0))
    {
        // Don't boost audio, it distorts, especially with DTS.
      
        //fprintf(stderr, "!!! Boosting Audio Volume !!!\n");
        //settings->sts_audio_output_volume *= 4; //just for testing, we should find a way to get to the dialnorm value to set up the apropriate boost
    }
    
    // Transcodeing to aac => change the audio profile
    if (strcmp(settings->sts_audio_codec_name, "libvo_aacenc")==0)
    {
        settings->sts_audio_profile = videoCaps->aacProfile; 
        settings->sts_audio_sample_rate = 44100;
    }
    else 
        settings->sts_audio_profile = -1;
}

void PLEXsetupVideoTranscoderSettingsForQuality(STMTranscodeContext *context,
                                                const int videoStreamIndex,
                                                char *stream_quality_name,
                                                double *subtitle_scale_factor,
                                                STMTranscodeSettings * settings){
    AVCodecContext *videoCodec = context->stc_input_files[0]->streams[videoStreamIndex]->codec;
    const float qualityFactor = context->plex.autodetect_cpu_quality?PLEXcpuQualityFactor():1.0f;
    const int pixcount = settings->sts_source_frame_width * settings->sts_source_frame_height;
    if (strcmp(stream_quality_name, "0") == 0)
    {
        settings->sts_audio_bitrate = 16 * 1000;
        settings->sts_audio_channels = 2;
        
        settings->sts_video_crf = 24;
        settings->sts_video_crf = 0;
        settings->sts_video_me_method = "umh";
        settings->sts_video_frame_rate.num = 3;
        settings->sts_video_frame_rate.den = 1;
        settings->sts_video_frame_width = 192;
        settings->sts_video_frame_height = 128;
        settings->sts_video_bufsize = 128 * 1024ll;
        settings->sts_video_minrate = 32 * 1024ll;
        settings->sts_video_maxrate = 64 * 1024ll;
        settings->sts_video_qcomp = 0.0;
        settings->sts_video_subq = 4;
        settings->sts_video_frame_refs = 6;
        
        *subtitle_scale_factor = 1.2;
    }
    else if (strcmp(stream_quality_name, "1") == 0)
    {
        settings->sts_audio_bitrate = 24 * 1000;
        settings->sts_audio_channels = 2;
        
        settings->sts_video_crf = 26;
        settings->sts_video_crf = 0;
        settings->sts_video_me_method = "umh";
        settings->sts_video_frame_rate.num = 12;
        settings->sts_video_frame_rate.den = 1;
        settings->sts_video_frame_width = 192;
        settings->sts_video_frame_height = 128;
        settings->sts_video_bufsize = 256 * 1024ll;
        settings->sts_video_minrate = 64 * 1024ll;
        settings->sts_video_maxrate = 96 * 1024ll;
        settings->sts_video_qcomp = 0.15;
        settings->sts_video_subq = 4;
        settings->sts_video_frame_refs = 8;
        
        *subtitle_scale_factor = 1.2;
    }
    else if (strcmp(stream_quality_name, "2") == 0)
    {
        settings->sts_audio_bitrate = 48 * 1000;
        settings->sts_audio_channels = 2;
        
        settings->sts_video_crf = 26;
        settings->sts_video_crf = 0;
        settings->sts_video_me_method = "umh";
        settings->sts_video_frame_rate.num = 15;
        settings->sts_video_frame_rate.den = 1;
        settings->sts_video_frame_width = 240;
        settings->sts_video_frame_height = 160;
        settings->sts_video_bufsize = 256 * 1024ll;
        settings->sts_video_minrate = 144 * 1024ll;
        settings->sts_video_maxrate = 208 * 1024ll;
        settings->sts_video_qcomp = 0.225;
        settings->sts_video_subq = 4;
        settings->sts_video_frame_refs = 6;
        
        *subtitle_scale_factor = 1.2;
    }
    else if (strcmp(stream_quality_name, "3") == 0)
    {
        settings->sts_audio_bitrate = 96 * 1000;
        settings->sts_audio_channels = 2;
        
        settings->sts_video_crf = 26;
        settings->sts_video_crf = 0;
        settings->sts_video_me_method = "umh";
        settings->sts_video_frame_width = 360;
        settings->sts_video_frame_height = 240;
        settings->sts_video_bufsize = 2048 * 1024ll;
        settings->sts_video_minrate = 256 * 1024ll;
        settings->sts_video_maxrate = 416 * 1024ll;
        settings->sts_video_qcomp = 0.30;
        settings->sts_video_frame_refs = 4;
        
        *subtitle_scale_factor = 1.2;
    }
    else if (strcmp(stream_quality_name, "4") == 0)
    {
        //26 -> Alice_22 -> 0.5MB, 180kbps
        //15 -> Alice_22 -> 2.1MB, 420kbps
        //10 -> Alice_22 -> 3.9MB, 720kbps
        settings->sts_video_crf = 25 * qualityFactor;
        settings->sts_video_frame_width = MIN(480, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(320, settings->sts_source_frame_height);
        settings->sts_video_bufsize = 2048 * 1024ll;
        settings->sts_video_minrate = 420 * 1024ll;
        settings->sts_video_maxrate = 720 * 1024ll;
        settings->sts_video_qcomp = 0.5;
        settings->sts_video_frame_refs = 2;
    }
    else if (strcmp(stream_quality_name, "5") == 0)
    {
        //27 -> Alice_22 -> 0.9MB, 252kbps
        //20 -> Alice_22 -> 2.6MB, 503kbps
        //11 -> Alice_22 -> 8.4MB, 1400kbps
        //10 -> Alice_22 -> 9MB, 1600kbps
        int qf = 26;
        if (pixcount <640*480){
            fprintf(stderr, "Updating QualityRate 26 -> 23\n");
            qf = 23;
        }
        settings->sts_video_crf = 26 * qualityFactor;
        settings->sts_video_frame_width = MIN(640, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(480, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 0;
        settings->sts_video_minrate = 1.0 * 1024 * 1024ll;
        settings->sts_video_maxrate = 1.5 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "6") == 0)
    {
        //27 -> Alice_22 -> 1.8MB, 380kbps
        //20 -> Alice_22 -> 5.3MB, 942kbps
        //18 -> Alice_22 -> 5.6MB, 1300kbps
        //17 -> Alice_22 -> 9MB, 1500kbps
        //16 -> Alice_22 -> 10.6MB, 1700kbps
        //15 -> Alice_22 -> 12.4MB, 2000kbps
        //14 -> Alice_22 -> 14.4MB, 2400kbps
        //13 -> Alice_22 -> 16.2MB, 2666kbps
        //12 -> Alice_22 -> 18.1MB, 3000kbps
        //10 -> Alice_22 -> 20.0MB, 3600kbps
        //08 -> Alice_22 -> 22.2MB, 3800kbps
        //06 -> Alice_22 -> 22.3MB, 3900kbps
        int qf = 26;
        if (pixcount <640*480){
            fprintf(stderr, "Updating QualityRate 26 -> 18\n");
            qf = 20;
        } else if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 26 -> 20\n");
            qf = 23;
        } settings->sts_video_crf = qf * qualityFactor;
        //settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1024, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(768, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 0;
        settings->sts_video_minrate = 1 * 1024 * 1024ll;
        settings->sts_video_maxrate = 2 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "7") == 0)
    {
        int qf = 24;
        if (pixcount <640*480){
            fprintf(stderr, "Updating QualityRate 24 -> 16\n");
            qf = 16;
        } else if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 24 -> 18\n");
            qf = 18;
        } else if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 24 -> 21\n");
            qf = 21;
        }
        settings->sts_video_crf = qf * qualityFactor;
        settings->sts_video_me_range = 14;
        settings->sts_video_frame_width = MIN(1280, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(720, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 0;
        settings->sts_video_minrate = 2 * 1280 * 1024ll;
        settings->sts_video_maxrate = 3 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "8") == 0)
    {
        int qf = 20;
        if (pixcount <640*480){
            fprintf(stderr, "Updating QualityRate 20 -> 12\n");
            qf = 12;
        } else if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 20 -> 15\n");
            qf = 15;
        } else if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 20 -> 17\n");
            qf = 17;
        }
        settings->sts_video_crf = qf * qualityFactor;
        settings->sts_video_me_range = 4;
        //settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1280, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(720, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 0;
        settings->sts_video_minrate = 2 * 1280 * 1024ll;
        settings->sts_video_maxrate = 4 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "9") == 0)
    {
        //20 -> Alice_22 -> 26.8MB, 4100kbps
        //19 -> Alice_22 -> 31.5MB, 4800kbps
        //17 -> Alice_22 -> 41.1MB, 6400kbps
        //15 -> Alice_22 -> 53.7MB, 8100kbps
        //13 -> Alice_22 -> 75.5MB, 10100kbps
        //12 -> Alice_22 -> 71.3MB, 11200kbps
        //11 -> Alice_22 -> 76.9MB, 12300kbps
        //10 -> Alice_22 -> 88.1MB, 14500kbps
        
        int qf = 22;
        if (pixcount <640*480){
            fprintf(stderr, "Updating QualityRate 22 -> 8\n");
            qf = 8;
        } else if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 22 -> 11\n");
            qf = 11;
        } else if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 22 -> 14\n");
            qf = 14;
        }else if (pixcount < 1600*900){
            fprintf(stderr, "Updating QualityRate 22 -> 18\n");
            qf = 18;
        }
        settings->sts_video_crf = qf * qualityFactor;
        //settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1920, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(1080, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 0;
        settings->sts_video_minrate = 6 * 1280 * 1024ll;
        settings->sts_video_maxrate = 8 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "10") == 0)
    {
        
        int qf = 15;
        if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 15 -> 5\n");
            qf = 5;
        } else if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 15 -> 8\n");
            qf = 8;
        }else if (pixcount < 1600*900){
            fprintf(stderr, "Updating QualityRate 15 -> 11\n");
            qf = 11;
        }
        settings->sts_video_crf = qf * qualityFactor;
        settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1920, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(1080, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 1;
        settings->sts_video_minrate = 8 * 1280 * 1024ll;
        settings->sts_video_maxrate = 10 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "11") == 0)
    {
        int qf = 12;
        if (pixcount <900*720){
            fprintf(stderr, "Updating QualityRate 12 -> 3\n");
            qf = 3;
        } else if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 12 -> 5\n");
            qf = 5;
        }else if (pixcount < 1600*900){
            fprintf(stderr, "Updating QualityRate 12 -> 8\n");
            qf = 8;
        }
        settings->sts_video_crf = qf * qualityFactor;
        settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1920, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(1080, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 2;
        settings->sts_video_minrate = 8 * 1280 * 1024ll;
        settings->sts_video_maxrate = 12 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    else if (strcmp(stream_quality_name, "12") == 0)
    {
        
        int qf = 5;
        if (pixcount <1100*720){
            fprintf(stderr, "Updating QualityRate 5 -> 2\n");
            qf = 2;
        }
        settings->sts_video_crf = qf * qualityFactor;
        settings->sts_video_crf = 0;
        settings->sts_video_frame_width = MIN(1920, settings->sts_source_frame_width);
        settings->sts_video_frame_height = MIN(1080, settings->sts_source_frame_height);
        settings->sts_video_frame_refs = 2;
        settings->sts_video_minrate = 10 * 1280 * 1024ll;
        settings->sts_video_maxrate = 20 * 1024 * 1024ll;
        settings->sts_video_subq = 1;
        settings->sts_video_me_method = "dia";
    }
    
#if 0
    // I'm not sure why we needed this, but I think with the more accurate bitrate measure, we don't any more.

    AVFormatContext *format = context->stc_input_files[0];
    const unsigned long long maxVidBR = MAX(format->bit_rate, videoCodec->bit_rate) * 2;
    if ((settings->sts_video_maxrate>maxVidBR) && maxVidBR>0l){
        fprintf(stderr, "!!! Reducing MaxVideoBitrate from %lld to %lld\n", settings->sts_video_maxrate, maxVidBR);
        settings->sts_video_maxrate = maxVidBR;
    }
#endif
    
    av_log(NULL, AV_LOG_ERROR,"Quality %s: me=%s, famerefs=%i, subq=%f, bitrate=%llu", stream_quality_name, settings->sts_video_me_method, settings->sts_video_frame_refs, settings->sts_video_subq, settings->sts_video_maxrate);
    
}


//Filesystem stuff
int PLEXfileExists(char *filename){
    FILE *file = fopen(filename, "r");
    int result = 0;
    if (file)
    {
        fclose(file);
        result = 1;
    }
    
    return result;
}

int PLEXfileHasExtension(const char* fileName, const char* extension){
    char* my_extension = strrchr(fileName, '.');
    if (my_extension){
        my_extension++;
        if (strcmp(my_extension, extension)==0){
            return 1;
        }
    }
    
    return 0;
}

int PLEXisNumeric(const char* str){
    int len = strlen(str);
    
    for (int i = 0; i < len; i++)
        if (str[i]<'0' || str[i]>'9') 
            return 0;
    
    return 1;
}


int PLEXcanCopyAudioStream(STMTranscodeContext const * const context, const int audioStreamIndex, float audio_gain, STMTranscodeSettings * const settings){
    PLEXClientVideoCaps const * const videoCaps = &context->plex.videoCaps;
    fprintf(stderr, "\n");
    
    if (audioStreamIndex == -1 || !videoCaps->allowAudioCopy) return 0;
        
    AVFormatContext *format = context->stc_input_files[0];
    AVCodecContext *audioCodec = format->streams[audioStreamIndex]->codec;
    PMS_Log(LOG_LEVEL_DEBUG, "Audio stream info:");
    PMS_Log(LOG_LEVEL_DEBUG, "    codec_id  : %x", audioCodec->codec_id);
    PMS_Log(LOG_LEVEL_DEBUG, "    channels  : %i", audioCodec->channels);
    PMS_Log(LOG_LEVEL_DEBUG, "    bitrate   : %i/%i", audioCodec->bit_rate, settings->sts_audio_bitrate);
    PMS_Log(LOG_LEVEL_DEBUG, "    samplerate: %i", audioCodec->sample_rate);
    PMS_Log(LOG_LEVEL_DEBUG, "    audiogain : %0.2f (%i)", audio_gain, settings->sts_audio_output_volume);
    PMS_Log(LOG_LEVEL_DEBUG, "    aacProfile: %i/%i", videoCaps->aacProfile, audioCodec->profile);

    if (strstr(context->stc_settings.sts_input_file_name, "rtmp://") != 0)
    {
      PMS_Log(LOG_LEVEL_DEBUG, "    ----> we're not remuxing RTMP audio for now.\n");
      return 0;
    }

    if (audio_gain < 0.95 || audio_gain > 1.05) {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> volumechange was requested\n");
        return 0;   
    } else {
        audio_gain = 1.0f;
    }
    
    if (audioCodec->codec_id == CODEC_ID_AC3){
        if (videoCaps->ac3MaxBitrate<0) 
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> ac3 is not supported\n");
            return 0;
        }
        
        if (videoCaps->ac3MaxBitrate>0 && audioCodec->bit_rate > videoCaps->ac3MaxBitrate)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> ac3 Bitrate Failed\n");
            return 0;
        }
    
        goto allow_audio_copy;
    }
    
    if (audioCodec->codec_id == CODEC_ID_DTS){
        if (videoCaps->dtsMaxBitrate<0)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> dts is not supported\n");
            return 0;
        }
        
        if (videoCaps->dtsMaxBitrate>0 && audioCodec->bit_rate > videoCaps->dtsMaxBitrate)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> dts Bitrate Failed\n");
            return 0;
        }
        
        goto allow_audio_copy;
    }
    
    if (audioCodec->codec_id == CODEC_ID_PCM_DVD || audioCodec->codec_id == CODEC_ID_PCM_BLURAY){
        if (videoCaps->pcmMaxBitrate<0)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> pcm is not supported\n");
            return 0;
        }
        
        if (videoCaps->pcmMaxBitrate>0 && audioCodec->bit_rate > videoCaps->pcmMaxBitrate)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> pcm Bitrate Failed\n");
            return 0;
        }
        
        goto allow_audio_copy;
    }
    
    if (audioCodec->channels > 2 ||	audioCodec->channels < 1)
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Channels Failed\n");
        return 0;
    }
    
    if (settings->sts_audio_bitrate < audioCodec->bit_rate * 0.75)
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Bitrate Failed\n");
        return 0;
    }
    
    if (audioCodec->codec_id == CODEC_ID_MP3){
        if (videoCaps->mp3MaxBitrate<0)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> mp3 is not supported\n");
            return 0;
        }
        
        if ((audioCodec->codec_tag != MKTAG('.', 'm', 'p', '3')) && (audioCodec->sample_rate != 48000))
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> mp3 Tag Failed\n");
            return 0;
        }
        if (videoCaps->mp3MaxBitrate>0 && audioCodec->bit_rate > videoCaps->mp3MaxBitrate)
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> mp3 Bitrate Failed\n");
            return 0;
        }
        
        goto allow_audio_copy;
    }
    
    if (audioCodec->codec_id == CODEC_ID_AAC){
        if (videoCaps->aacMaxBitrate<0) 
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> aac is not supported\n");
            return 0;
        }
        
        if ((audioCodec->codec_tag != MKTAG('.', 'm', 'p', '3')) && (audioCodec->sample_rate < 8000 || audioCodec->sample_rate > 48000)) 
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> aac Tag Failed\n");
            return 0;
        }
        
        if (videoCaps->aacMaxBitrate>0 && audioCodec->bit_rate > videoCaps->aacMaxBitrate) 
        {
            PMS_Log(LOG_LEVEL_DEBUG, "    ----> aac Bitrate Failed\n");
            return 0;
        }
        
        //we should not test this.
        /*if (videoCaps->aacProfile>=0 && (videoCaps->aacProfile<audioCodec->profile || audioCodec->profile<0)) 
        {
            fprintf(stderr, "    ----> aac profile not supported\n");
            fflush(stderr);
            return 0;
        }*/
        
        goto allow_audio_copy;
    }
    
	//fail, cause we did not find a copyable codec
	PMS_Log(LOG_LEVEL_DEBUG, "    ----> codec not supported\n");
	return 0;
    goto allow_audio_copy;
    
allow_audio_copy:    
    //make sure we turn of audio boost
    if (settings->sts_audio_output_volume!=256.0f * audio_gain){
        PMS_Log(LOG_LEVEL_DEBUG, "    TURNING OFF BOOST\n");
        settings->sts_audio_output_volume = 256.0f * audio_gain;
    }
    return 1;
}

int PLEXcanCopyVideoStream(STMTranscodeContext const * const context, const int audioStreamIndex, const int videoStreamIndex, const float avg_fps, STMTranscodeSettings const * const settings){
    PLEXClientVideoCaps const * const videoCaps = &context->plex.videoCaps;
    fprintf(stderr, "\n");
    
    if (videoStreamIndex == -1  || !videoCaps->allowVideoCopy) return 0;
    if (context->stc_settings.sts_subtitle_stream_index != -1) return 0;
    if (subtitle_path != 0) return 0;
    if (settings->sts_album_art_mode || settings->sts_still_image_mode) return 0;
    
    AVFormatContext *format = context->stc_input_files[0];
    AVCodecContext *audioCodec = format->streams[audioStreamIndex]->codec;
    AVCodecContext *videoCodec = format->streams[videoStreamIndex]->codec;
    long long int bit_rate = MAX(format->bit_rate, videoCodec->bit_rate);

    // If the codec bitrate is incorrect, compute based on file size.
    if (videoCodec->bit_rate == 0 && format->file_size > 0 && format->duration > 0)
    {
      int64_t seconds = format->duration / AV_TIME_BASE;
      int bps = (int)(format->file_size / seconds * 8);
      
      // Subtract out the audio streams.
      for (int i=0; i<format->nb_streams; i++)
      {
        AVCodecContext* codec = format->streams[i]->codec;
        if (codec && codec->codec_type == AVMEDIA_TYPE_AUDIO)
          bps -= codec->bit_rate;
      }
      
      bit_rate = bps;
    }

    const uint64_t avg_secsize = (audioCodec->bit_rate + bit_rate);
    const uint64_t avg_segsize = avg_secsize * settings->sts_output_segment_length;
    
    float src_aspect = 1.0f;
    float in_aspect = 1.0f;
    float out_aspect = 1.0f;
    if (videoCodec->sample_aspect_ratio.num > 0 && videoCodec->sample_aspect_ratio.den > 0)
        if (settings->sts_video_frame_width > 0 &&  settings->sts_video_frame_height > 0){
            src_aspect = (videoCodec->sample_aspect_ratio.num / (double)videoCodec->sample_aspect_ratio.den);
            const float stream_width = videoCodec->width * src_aspect;
            const float stream_height = videoCodec->height;
            
            in_aspect = stream_height / stream_width;
            out_aspect = (settings->sts_video_frame_height) / (float)settings->sts_video_frame_width ;
        }
    
    const float timebase = videoCodec->time_base.den==0?0.0f:((float)videoCodec->time_base.num /  (float)videoCodec->time_base.den);
    const float minTimeBase = 0.00001;
    const int maxTimeBaseDen = 1000000;
    
    PMS_Log(LOG_LEVEL_DEBUG, "Video stream info:\n");
    PMS_Log(LOG_LEVEL_DEBUG, "    codec_id: %x/%x \n", videoCodec->codec_id, CODEC_ID_H264);
    PMS_Log(LOG_LEVEL_DEBUG, "    level   : %i/%i \n", videoCodec->level, videoCaps->level);
    PMS_Log(LOG_LEVEL_DEBUG, "    avg_fps : %0.0f/%0.0f \n", avg_fps, settings->sts_video_frame_rate.num/(float)settings->sts_video_frame_rate.den);
    PMS_Log(LOG_LEVEL_DEBUG, "    profile : %x/%x \n", videoCodec->profile, videoCaps->profile);
    PMS_Log(LOG_LEVEL_DEBUG, "    vfr     : %i \n", context->stc_input_files[0]->streams[videoStreamIndex]->vfr);
    PMS_Log(LOG_LEVEL_DEBUG, "    fps     : %0.2f/%0.2f \n", avg_fps, settings->sts_video_frame_rate.num/(float)settings->sts_video_frame_rate.den);
    PMS_Log(LOG_LEVEL_DEBUG, "    bitrate : %0.2f/%0.2f mbps\n", (bit_rate)/(1000.0f*1000), settings->sts_video_maxrate/(1000.0f*1000));
    PMS_Log(LOG_LEVEL_DEBUG, "    height  : %i/%i \n", videoCodec->height, settings->sts_video_frame_height);
    PMS_Log(LOG_LEVEL_DEBUG, "    aspect  : %0.2f(%0.2f)/%0.2f \n", in_aspect, src_aspect, out_aspect);
    PMS_Log(LOG_LEVEL_DEBUG, "    segSize : %0.2fmbits/seg, %0.2f/%0.2fMBps\n", avg_segsize/(1024.0f*1024.0f), avg_secsize/(1024.0f*1024.0f), videoCaps->maxSegmentSizePerSecond*8.0/(1024.0f*1024.0f));
    PMS_Log(LOG_LEVEL_DEBUG, "    timebase: %0.5f(%i:%i)/%0.5f(%i:%i) \n", timebase,   videoCodec->time_base.num, videoCodec->time_base.den, minTimeBase, 0, maxTimeBaseDen);
    
    // N.B. segment size comes down in bytes/sec, bitrate is in bits/sec.
    if (avg_secsize>videoCaps->maxSegmentSizePerSecond*8.0 && videoCaps->maxSegmentSizePerSecond>0) 
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> segSize Failed\n");
        return 0;
    }
    if (videoCodec->codec_id != CODEC_ID_H264) 
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> not H.264\n");
        return 0;
    }
    if (videoCodec->level < 0 ||  videoCodec->level > videoCaps->level)
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Level Failed\n");
        return 0;
    }
    if ((videoCodec->profile & 0xff)> videoCaps->profile)
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Profile Failed\n");
        return 0;
    }
    
    if (avg_fps > 35 || avg_fps < 3 ) 
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Framerate out of bounds [3..35]fps\n");
        return 0;
    }
    
    //if (fabs(in_aspect-out_aspect)>0.04) return 0;
    if (fabs(src_aspect - 1.0f)>0.05 && !videoCaps->playsAnamorphic) 
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Source Aspect Failed\n");
        return 0;
    }
    
    if (settings->sts_video_frame_rate.num/(float)settings->sts_video_frame_rate.den  < avg_fps *0.9 )
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Framerate Failed\n");
        return 0;
    }
    
    if (settings->sts_video_frame_height < videoCodec->height *0.8 )
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Height Failed\n");
        return 0;
    }
    
    if (settings->sts_video_maxrate < bit_rate * 0.9 && bit_rate>0) 
    {
        PMS_Log(LOG_LEVEL_DEBUG, "    ----> Bitrate Failed\n");
        return 0;
    }
    
    /*
     //originally added for a bugged File with a strand timeBase encoding
     //removed as it was flagging working files as beeing wrong
    if (timebase < minTimeBase) 
    {
        fprintf(stderr, "    ----> Impossible TimeBase\n");
        fflush(stderr);
        return 0;
    }
    
    if ( videoCodec->time_base.den > maxTimeBaseDen) 
    {
        fprintf(stderr, "    ----> Insane TimeBase Denominator\n");
        fflush(stderr);
        return 0;
    }*/
    
    
    return 1;
}

static int segmentSocket;
static int remoteSegmentSocket;
static int idleOnSegmentIndex;
//return 0=ok, 1=failed to create socket, -1=failed to bind socket, -2=failed to listen
int PLEXcreateSegmentSocket(const char* inFileName, const int initialSegment){
    segmentSocket = 0;
    remoteSegmentSocket = 0;
    idleOnSegmentIndex = initialSegment + 5;
    
#if !USE_SEGMENT_SOCKET
    return 0;
#endif
    
    struct sockaddr_in name;
    size_t size;
    
#if __WIN32__
	int iResult;
	WSADATA wsaData;	
	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed: %d\n", iResult);
		return 1;
	}
	
	/* Confirm that the WinSock DLL supports 2.2.*/
	/* Note that if the DLL supports versions later    */
	/* than 2.2 in addition to 2.2, it will still return */
	/* 2.2 in wVersion since that is the version we      */
	/* requested.                                        */
 
	if ( LOBYTE( wsaData.wVersion ) != 2 ||
        HIBYTE( wsaData.wVersion ) != 2 ) {
		/* Tell the user that we could not find a usable */
		/* WinSock DLL.                                  */
		WSACleanup( );
		return 1; 
	}
#endif
	
    segmentSocket = socket (AF_INET, SOCK_STREAM, 0);
    if (segmentSocket < 0)
    {
        fprintf(stderr, "Unable to create socket for communication!");
        segmentSocket = 0;
        return 1;
    }
    
    int ok = 1;
    int subport = 0;
    char* ip = "127.0.0.1";
    do {
        ok = 1;
        memset(&name, 0, sizeof(name));
        name.sin_family = AF_INET;
        name.sin_port=htons(32399 - subport);
		name.sin_addr.s_addr = inet_addr(ip);
		//inet_pton(AF_INET, ip, &name.sin_addr);
		
		
        size = sizeof(struct sockaddr_in);
        //ip = inet_ntoa(name.sin_addr);
        if (bind (segmentSocket, (struct sockaddr *) &name, size) < 0)
        {
            fprintf(stderr, "Unable to bind socket on %s:%i\n", ip, name.sin_port);
            fflush(stderr);
            ok = 0;
            subport++;
        }
        
        
    } while (ok==0);
    
    if (listen(segmentSocket, 1) == -1) {
        fprintf(stderr, "Unable to listen on socket %s:%i\n", ip, name.sin_port);
        fflush(stderr);
        return -2;
    }
    
    fprintf(stdout, "SegmentSocketPort: %i\n", ntohs(name.sin_port));
    fflush(stdout);
    
    return 0;
}

void PLEXcloseSegmentSocket(void){
    if (remoteSegmentSocket!=0){
        fprintf(stdout, "Closing RemoteSegmentSocket\n");
        fflush(stdout);
        close(remoteSegmentSocket);
        remoteSegmentSocket = 0;
    }
    
    if (segmentSocket!=0){
        fprintf(stdout, "Closing SegmentSocket\n");
        fflush(stdout);
        close(segmentSocket);
        segmentSocket = 0;
    }
	
#if __WIN32__	
    fprintf(stdout, "Shuting Down!");
	WSACleanup( );
#endif
}

void PLEXwaitForSegmentAck(const int currentSegment){
    if (segmentSocket!=0){
        if (currentSegment>=idleOnSegmentIndex){
            fprintf(stderr, "--> Ideling on segment %i\n", currentSegment);
            
            /*char c = ' ';
             do {
             c = fgetc (segmentSocket);
             if (c==EOF){
             usleep(500000);
             fpos_t p;
             fgetpos(segmentSocket, &p);
             freopen(NULL, "r", segmentSocket);
             fseek(segmentSocket, p, SEEK_SET);
             fprintf(stderr, "Did sleep\n");
             } else if (c == '\n') {
             fprintf(stderr, "Line End\n");
             } else {
             fprintf(stderr, "did read %c\n", c);
             }
             } while (c!='\n');*/
            
            if (remoteSegmentSocket==0){
                int t;
                struct sockaddr_in remote;
                fprintf(stderr, "Waiting for a connection...\n");
                t = sizeof(remote);
                if ((remoteSegmentSocket = accept(segmentSocket, (struct sockaddr *)&remote, &t)) == -1) {
                    fprintf(stderr, "Could not accept socket connection\n");
                    fflush(stderr); 
                    return;
                }
                
                fprintf(stderr, "Connected.\n");
                fflush(stderr);
            }
            
            char str[1];
            char val[100];
            val[99]=0;
            int valpos = 0;
            int errcount = 0;
            do{
                int n = recv(remoteSegmentSocket, str, 1, 0);
                if (n <= 0) {
                    errcount ++;
                    if (n < 0) {
                        fprintf(stderr, "Error while receiving data\n");
                        fflush(stderr);
                    } 
                } else {
                    errcount = 0;
                    val[valpos++] = str[0]=='\n'?0:str[0];
                    //fprintf(stderr, "Received %c\n", str[0]);
                    fflush(stderr);
                }
                
                if (errcount>100){
                    close(remoteSegmentSocket);
                    remoteSegmentSocket = 0;
                    fprintf(stderr, "The socket seems to have a problem\n");
                    fflush(stderr);
                    return;
                }
            } while ( str[0]!='\n' || valpos==99 );
            
            idleOnSegmentIndex = atoi(val);
            fprintf(stderr, "Received new max. Segment %s=>%i\n", val, idleOnSegmentIndex);
            PLEXwaitForSegmentAck(currentSegment);
        }
    }
}
