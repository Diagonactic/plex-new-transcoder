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

#pragma once
#include "ffmpeg.h"
#include "libavfilter/drawutils.h"

typedef enum LogLevel
{
   LOG_LEVEL_ERROR,
   LOG_LEVEL_WARNING,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG
} LogLevel;

void PMS_Log(LogLevel level, const char* format, ...);

extern int PLEX_DEBUG;
float PLEXcpuQualityFactor(void);
void PLEXshowAutoCPUInfo(void);
void PLEXsetupContext(STMTranscodeContext *context, int argc, char **argv);

void PLEXsetupBaseTranscoderSettings(STMTranscodeContext *context,
                                     char *audio_stream_specifier,
                                     char *subtitle_stream_specifier,
                                     float audio_gain,
                                     char *srt_encoding,
                                     int audioStreamIndex,
                                     int videoStreamIndex,
                                     STMTranscodeSettings * settings,
                                     const char* output_path);

void PLEXsetupAudioTranscoderSettingsForQuality(STMTranscodeContext *context,
                                                char *audio_stream_specifier,
                                                char *stream_quality_name,
                                                float audio_gain,
                                                int audioStreamIndex,
                                                STMTranscodeSettings * settings);
void PLEXsetupVideoTranscoderSettingsForQuality(STMTranscodeContext *context,
                                                const int videoStreamIndex,
                                                char *stream_quality_name,
                                                double *subtitle_scale_factor,
                                                STMTranscodeSettings * settings);


char* PLEXbitrateString(unsigned long long bitrate);
int PLEXfileExists(char *filename);
int PLEXfileHasExtension(const char* fileName, const char* extension);
int PLEXisNumeric(const char* str);


int PLEXcanCopyAudioStream(STMTranscodeContext const * const context, const int audioStreamIndex, float audio_gain, STMTranscodeSettings * const settings);
int PLEXcanCopyVideoStream(STMTranscodeContext const * const context, const int audioStreamIndex, const int videoStreamIndex, const float avg_fps, STMTranscodeSettings const * const settings);

int PLEXcreateSegmentSocket(const char* fileName, const int initialSegment);
void PLEXwaitForSegmentAck(const int currentSegment);
void PLEXcloseSegmentSocket(void);