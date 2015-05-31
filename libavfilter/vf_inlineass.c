/*
 * SSA/ASS subtitles rendering filter, using libssa.
 * Based on vf_drawbox.c from libavfilter and vf_ass.c from mplayer.
 *
 * Copyright (c) 2006 Evgeniy Stepanov <eugeni.stepa...@gmail.com>
 * Copyright (c) 2008 Affine Systems, Inc (Michael Sullivan, Bobby Impollonia)
 * Copyright (c) 2009 Alexey Lebedeff <bina...@binarin.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

/*
 * Usage: '-vfilters ass=filename:somefile.ass|margin:50|encoding:utf-8'
 * Only 'filename' param is mandatory.
 */
#ifdef __linux__
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE // required for asprintf
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/stat.h>
#endif

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <libass/ass.h>
#include <fribidi/fribidi.h>
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavutil/avstring.h"
#include "vf_inlineass.h"
//#include "defaults.h"

#include "iconv.h"

#ifdef WINDOWS
#include <shlobj.h>
#include <asprintf.h>
#endif

typedef struct
{
  ASS_Library *ass_library;
  ASS_Renderer *ass_renderer;
  ASS_Track *ass_track;

  int margin;
  char *filename;
  char *encoding;
  double font_scale;

  int64_t pts_offset;
  int64_t event_number;
  int frame_width, frame_height;
  int vsub,hsub;   //< chroma subsampling
  int flip_srt;
  char *srt_encoding;

} AssContext;

extern char *executable_path;
extern char *subtitle_path;

static int parse_args(AVFilterContext *ctx, AssContext *context, const char* args);
static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
  //fprintf(stderr, "----> init\n");
  AssContext *context= ctx->priv;

  /* defaults */
  context->margin = 10;
  context->encoding = "utf-8";
  context->font_scale = 1.0;
  context->filename = subtitle_path;
  
  if ( parse_args(ctx, context, args) )
    return 1;

  return 0;
}

void set_common_formats(AVFilterContext *ctx, AVFilterFormats *fmts,
	enum AVMediaType type, int offin, int offout);
static int query_formats(AVFilterContext *ctx)
{
	enum PixelFormat list[11] = {PIX_FMT_YUV444P,  PIX_FMT_YUV422P,  PIX_FMT_YUV420P,
			       PIX_FMT_YUV411P,  PIX_FMT_YUV410P,
			       PIX_FMT_YUVJ444P, PIX_FMT_YUVJ422P, PIX_FMT_YUVJ420P,
			       PIX_FMT_YUV440P,  PIX_FMT_YUVJ440P,
				   PIX_FMT_NONE};
  set_common_formats(ctx, avfilter_make_format_list(list), AVMEDIA_TYPE_VIDEO, offsetof(AVFilterLink, in_formats), offsetof(AVFilterLink, out_formats));
  return 0;
}

void vf_inlineass_set_aspect_ratio(AVFilterContext *context, double dar)
{
    //fprintf(stderr, "----> vf_inlineass_set_aspect_ratio %f\n", dar);
	AssContext *assContext = (AssContext *)context->priv;
	ASS_Renderer *assRenderer = assContext->ass_renderer;
	
	ass_set_aspect_ratio(assRenderer, 1.0 / dar, 1.0);
}

char *parse_srt_tags(char *input, size_t *length);
char *parse_srt_tags(char *input, size_t *length)
{
    //fprintf(stderr, "---> reading %c, %d\n", input[0], *length);
	size_t input_length = *length;
	size_t output_length = input_length + 1;
	char *output = NULL;
	size_t j = 0;
	int bState = 0;
	int iState = 0;

	for (size_t i = 0; i < input_length; i++)
	{
		if (input[i] == '<')
		{
			if (i < input_length - 1 && (input[i+1] == 'b' || input[i+1] == 'i'))
			{
				if (i < input_length - 2 && input[i+2] == '>')
				{
					output_length += 2;
					if (!output)
					{
						output = av_malloc(output_length);
						memcpy(output, input, i);
						j = i;
					}
					else
					{
						output = av_realloc(output, output_length);
					}
					output[j++] = '{';
					output[j++] = '\\';
					output[j++] = input[i+1];
					output[j++] = (input[i+1] == 'b' ? bState : iState) ? '0' : '1';
					output[j++] = '}';

					if (input[i+1] == 'b')
					{
						bState = !bState;
					}
					else
					{
						iState = !iState;
					}
					
					i+=2;
					
					continue;
				}
				else if (i < input_length - 3 && input[i+2] == '/' && input[i+3] == '>')
				{
					output_length += 1;
					if (!output)
					{
						output = av_malloc(output_length);
						memcpy(output, input, i);
						j = i;
					}
					else
					{
						output = av_realloc(output, output_length);
					}
					output[j++] = '{';
					output[j++] = '\\';
					output[j++] = input[i+1];
					output[j++] = (input[i+1] == 'b' ? bState : iState) ? '0' : '1';
					output[j++] = '}';

					if (input[i+1] == 'b')
					{
						bState = !bState;
					}
					else
					{
						iState = !iState;
					}

					i+=3;
					continue;
				}
			}
			else if (i < input_length - 3 && input[i+1] == '/' && (input[i+2] == 'b' || input[i+2] == 'i') && input[i+3] == '>')
			{
				output_length += 1;
				if (!output)
				{
					output = av_malloc(output_length);
					memcpy(output, input, i);
					j = i;
				}
				else
				{
					output = av_realloc(output, output_length);
				}
				output[j++] = '{';
				output[j++] = '\\';
				output[j++] = input[i+2];
				output[j++] = (input[i+1] == 'b' ? bState : iState) ? '0' : '1';
				output[j++] = '}';

				if (input[i+1] == 'b')
				{
					bState = !bState;
				}
				else
				{
					iState = !iState;
				}

				i+=3;

				continue;
			}
		}

		if (output)
		{
			output[j++] = input[i];
		}
	}
	
	*length = output_length - 1;
	
	if (output)
	{
		av_free(input);
		output[output_length - 1] = '\0';
		return output;
	}
	
	return input;
}

void vf_inlineass_append_data(AVFilterContext *context, enum CodecID codecID,
	char *data, int dataSize, int64_t pts, int64_t duration, AVRational time_base)
{
    //fprintf(stderr, "----> appending %i bytes at %lld for %lld", dataSize, pts, duration);
	AssContext *assContext = (AssContext *)context->priv;
	ASS_Track *track = assContext->ass_track;
	double current_time = pts * time_base.num / (double)time_base.den;
	double packetDuration = duration * time_base.num / (double)time_base.den;

	if (codecID == CODEC_ID_SSA)
	{
		if (!track->event_format)
		{
//			if (pts == -1)
//			{
//				data += 3;
//			}

			av_log(0, AV_LOG_ERROR, "Subtitle text at t=%f:\n%s", current_time, data);
			/*int reprocess = */ass_process_codec_private(track, data, dataSize);
			#if 0
			if (reprocess)
			{
				ass_process_data(track, data, dataSize);
			}
			#endif
		}
		else
		{
			av_log(0, AV_LOG_ERROR, "Subtitle text at t=%f:\n%s", current_time, data);
			ass_process_data(track, data, dataSize);
		}
	}
	else if (codecID == 0)
	{
		//
		// Audio track information display
		//
		if (!track->event_format)
		{
			char *eventLine =
			    "[Events]\n"
				"Format: EventIndex, Layer, Ignored, Start, Duration, Style, "
				"Name, MarginL, MarginR, MarginV, Effect, Text\n";
			ass_process_data(track, eventLine, strlen(eventLine));
		}
		
		//
		// Show for the first and last 15 seconds of the track
		//
		const int duration = 15;

		char *terminatedData = av_malloc(dataSize + 1);
		av_strlcpy(terminatedData, data, dataSize);
		terminatedData[dataSize] = '\0';
		
		char *ssa;
		int wrote = asprintf(&ssa, "%lld,0,0:00:00.00,0:00:%02d.00f,TrackDisplay,NoName,0000,0000,0000,,{\\fad(1000,1000)}%s",
			assContext->event_number, duration, terminatedData);
		av_log(0, AV_LOG_ERROR, "Subtitle text at t=0:\n%s", ssa);
		ass_process_chunk(track, ssa, wrote, 0, duration * 1000.0);
		
		av_free(ssa);
		
		assContext->event_number++;
		
		const float endTime = pts / (double)AV_TIME_BASE - duration;
		
		if (endTime > duration * 3)
		{
			int hours = endTime / 3600.0;
			int minutes = (endTime - (hours * 3600.0)) / 60.0;
			float seconds = endTime - (hours * 3600.0) - (minutes * 60.0);

			// The "Ignored" parameter should not actually be present.
			wrote = asprintf(&ssa, "%lld,0,%d:%02d:%05.02f,0:00:%02d.00,TrackDisplay,NoName,0000,0000,0000,,{\\fad(1000,1000)}%s",
				assContext->event_number, hours, minutes, seconds, duration, terminatedData);
			av_log(0, AV_LOG_ERROR, "Subtitle text at t=%f:\n%s", endTime, ssa);
			ass_process_chunk(track, ssa, wrote, endTime * 1000.0, duration * 1000.0);
			
			av_free(ssa);
			av_free(terminatedData);

			assContext->event_number++;
		}
	}
	else
	{
		// Ignore headers
		if (pts == -1)
		{
			return;
		}
		
		if (!track->event_format)
		{
			char *eventLine =
			    "[Events]\n"
				"Format: EventIndex, Layer, Ignored, Start, Duration, Style, "
				"Name, MarginL, MarginR, MarginV, Effect, Text\n";
			ass_process_data(track, eventLine, strlen(eventLine));
		}

#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

		const float charactersPerSecond = 25.0;
		float displayDuration;
		
		if (packetDuration)
		{
			displayDuration = packetDuration;
		}
		else
		{
			displayDuration = MAX(1.75, MIN(3.5, dataSize / charactersPerSecond));
		}
		int hours = current_time / 3600.0;
		int minutes = (current_time - (hours * 3600.0)) / 60.0;
		float seconds = current_time - (hours * 3600.0) - (minutes * 60.0);
		
		int offset = 0;
		if (codecID == CODEC_ID_MOV_TEXT)
		{
			dataSize = (data[offset++] << 8);
			dataSize += data[offset++];
		}
		if (dataSize == 0)
		{
			return;
		}
		
		char subtitleBuffer[512];
		
		// The "Ignored" parameter should not actually be present.
		int wrote = snprintf(subtitleBuffer, 512, "%lld,0,%d:%d:%05.02f,0:00:%05.02f,Default,NoName,0000,0000,0000,,",
			assContext->event_number, hours, minutes, seconds, displayDuration);
		
		char *subtitle = av_malloc(dataSize);
		memcpy(subtitle, &data[offset], dataSize);
		size_t subtitle_size = dataSize;
		subtitle = parse_srt_tags(subtitle, &subtitle_size);

		if (subtitle_size > 512 - wrote)
		{
			subtitle_size = 512 - wrote;
		}
		
		av_strlcpy(&subtitleBuffer[wrote], subtitle, subtitle_size);
		if (subtitle_size + wrote < 512)
		{
			subtitleBuffer[subtitle_size + wrote] = '\0';
		}
		av_log(0, AV_LOG_ERROR, "Subtitle text at t=%f:\n%s", current_time, subtitleBuffer);
		ass_process_chunk(track, subtitleBuffer, wrote + subtitle_size, current_time * 1000.0, displayDuration * 1000);
		
		assContext->event_number++;
	}
}

ASS_Track* ass_default_track(ASS_Library* library, int width, int height);
ASS_Track* ass_default_track(ASS_Library* library, int width, int height) {
    
    //fprintf(stderr, "----> ass_default_track %ix%i\n", width, height);
	ASS_Track* track = ass_new_track(library);

	track->track_type = TRACK_TYPE_ASS;
	track->Timer = 100.;
	track->PlayResX = 640;
	track->PlayResY = 480;
	track->WrapStyle = 0;

	ASS_Style *style;
	int sid;
//	double fs;
	uint32_t c1, c2;

	//
	// Default style
	//
	sid = ass_alloc_style(track);
	style = track->styles + sid;
	style->Name = av_strdup("Default");
	style->FontName = "Arial";
	style->treat_fontname_as_pattern = 1;

//	float text_font_scale_factor = 3.5;
//	int subtitle_autoscale = 3;
//	fs = track->PlayResY * text_font_scale_factor / 100.;
//	// approximate autoscale coefficients
//	if (subtitle_autoscale == 2)
//		fs *= 1.3;
//	else if (subtitle_autoscale == 3)
//		fs *= 1.4;
	style->FontSize = 26;

	char *ass_color = NULL;
	char *ass_border_color = NULL;
	if (ass_color) c1 = strtoll(ass_color, NULL, 16);
	else c1 = 0xFFFFFF00;
	if (ass_border_color) c2 = strtoll(ass_border_color, NULL, 16);
	else c2 = 0x00000000;

	style->PrimaryColour = c1;
	style->SecondaryColour = c1;
	style->OutlineColour = c2;
	style->BackColour = 0x00000000;
	style->BorderStyle = 1;
	style->Alignment = 2;
	style->Outline = 2;
	style->MarginL = 5;
	style->MarginR = 5;
	style->MarginV = 20;
	style->ScaleX = 1.;
	style->ScaleY = 1.;

	//
	// TrackDisplay style
	//
	sid = ass_alloc_style(track);
	style = track->styles + sid;
	style->Name = av_strdup("TrackDisplay");
	style->FontName = "Arial";
	style->treat_fontname_as_pattern = 1;

//	text_font_scale_factor = 3.0;
//	subtitle_autoscale = 3;
//	fs = track->PlayResY * text_font_scale_factor / 100.;
//	// approximate autoscale coefficients
//	if (subtitle_autoscale == 2)
//		fs *= 1.3;
//	else if (subtitle_autoscale == 3)
//		fs *= 1.4;
	style->FontSize = 22;

	ass_color = NULL;
	ass_border_color = NULL;
	if (ass_color) c1 = strtoll(ass_color, NULL, 16);
	else c1 = 0xFFFFFF00;
	if (ass_border_color) c2 = strtoll(ass_border_color, NULL, 16);
	else c2 = 0x00000000;

	style->PrimaryColour = c1;
	style->SecondaryColour = c1;
	style->OutlineColour = c2;
	style->BackColour = 0x00000000;
	style->BorderStyle = 1;
	style->Alignment = 1;
	style->Outline = 2;
	style->MarginL = 10;
	style->MarginR = 10;
	style->MarginV = 20;
	style->ScaleX = 1.;
	style->ScaleY = 1.;

	ass_process_force_style(track);
	return track;
}

char * my_getline(FILE *fp);
char * my_getline(FILE *fp) {
    char * line = av_malloc(100), * linep = line;
    size_t lenmax = 100, len = lenmax;
    int c;

    if(line == NULL)
        return NULL;

    for(int index=0;;index++) {
        c = fgetc(fp);
        if(c == EOF)
            break;

		if (c == '\r')
		{
			continue;
		}
		
		if (c == '\n')
		{
			break;
		}

    // Does it look like BOM? Skip.
    if (index == 0 && c == 0xEF)
      continue;
    if (index == 1 && c == 0xBB)
      continue;
    if (index == 2 && c == 0xBF)
      continue;

        if(--len == 0) {
            char * linen = av_realloc(linep, lenmax *= 2);
            len = lenmax;

            if(linen == NULL) {
                av_free(linep);
                return NULL;
            }
            line = linen + (line - linep);
            linep = linen;
        }

        *line++ = c;
    }
    *line = '\0';
    return linep;
}

char *getlinewithopts(FILE *fp, iconv_t conv, int flip);
char *getlinewithopts(FILE *fp, iconv_t conv, int flip)
{
	char *line_in = my_getline(fp);
	size_t dst_length;
	if (conv)
	{
		size_t src_length = strlen(line_in);
		size_t in_length = strlen(line_in);
		size_t orig_length = src_length;
		dst_length = src_length;
		char *result = av_malloc(dst_length + 1);
		const char *in_tracking = line_in;
		char *out_tracking = result;

		long conv_length = -1;
		int attempts = 0;
		while (conv_length < 0 && attempts < 4)
		{
			conv_length = iconv(conv, &in_tracking, &in_length, &out_tracking, &dst_length);
			
			if (conv_length < 0 && errno == E2BIG)
			{
				orig_length <<= 1;
				dst_length = orig_length;
				in_length = src_length;

				result = av_realloc(result, dst_length + 1);
				in_tracking = line_in;
				out_tracking = result;
				attempts++;
			}
			else if (conv_length < 0)
			{
				av_free(result);

				return line_in;
			}
		}
		
		av_free(line_in);
		dst_length = orig_length - dst_length;
		result[dst_length] = '\0';
		
		line_in = result;
	}
	else
	{
		dst_length = strlen(line_in);
	}
	
	if (flip)
	{
#define LINE_LEN 512
		FriBidiChar logical[LINE_LEN+1], visual[LINE_LEN+1]; // Hopefully these two won't smash the stack
		char *op;
		FriBidiParType type = FRIBIDI_PAR_RTL;
		int char_set_num = fribidi_parse_charset ("UTF-8");
		size_t len = fribidi_charset_to_unicode (char_set_num, line_in, dst_length, logical);
		fribidi_boolean log2vis = fribidi_log2vis(logical, len, &type, visual, NULL, NULL, NULL);
		if(log2vis)
		{
			len = fribidi_remove_bidi_marks (visual, len, NULL, NULL, NULL);
			op = av_malloc(MAX(2*dst_length,2*len) + 1);
		}
		if (!op)
		{
			return line_in;
		}

	   size_t op_len = fribidi_unicode_to_charset(char_set_num, visual, len, op);
	   op[op_len] = '\0';
		return op;
	}

	return line_in;
}

#define BOM_CHECK_2(___bomp1___, ___bomp2___, ___bomname___)   \
if ((bom[0]==___bomp1___ && bom[1]==___bomp2___ ) ||  \
    (bom[0]==___bomp2___ && bom[1]==___bomp1___)){   \
    fprintf(stderr, "Found %s BOM in subtitle. Ignoring\n", ___bomname___);                 \
    lineoffset = 2;                                                         \
}

#define BOM_CHECK_3(___bomp1___, ___bomp2___, ___bomp3___, ___bomname___)   \
if ((bom[0]==___bomp1___ && bom[1]==___bomp2___ && bom[2]==___bomp3___) ||  \
    (bom[0]==___bomp3___ && bom[1]==___bomp2___ && bom[2]==___bomp1___)){   \
    fprintf(stderr, "Found %s BOM in subtitle. Ignoring\n", ___bomname___);                 \
    lineoffset = 3;                                                         \
}

#define BOM_CHECK_4(___bomp1___, ___bomp2___, ___bomp3___, ___bomp4___, ___bomname___)   \
if ((bom[0]==___bomp1___ && bom[1]==___bomp2___ && bom[2]==___bomp3___ && bom[3]==___bomp4___) ||  \
    (bom[0]==___bomp4___ && bom[1]==___bomp3___ && bom[2]==___bomp2___ && bom[3]==___bomp1___)){   \
    fprintf(stderr, "Found %s BOM in subtitle. Ignoring\n", ___bomname___);                 \
    lineoffset = 4;                                                         \
}

static ASS_Track *import_srt_file(AssContext *context)
{
    //fprintf(stderr, "----> loading %s\n", context->filename);
	FILE *file = fopen(context->filename, "r");
	if (!file)
	{
		return NULL;
	}
	//fprintf(stderr, "----> loaded %s\n", context->filename);
	iconv_t conv = NULL;
	if (context->srt_encoding &&
		strcmp(context->srt_encoding, "UTF-8") != 0)
	{
		conv = iconv_open("UTF-8", context->srt_encoding);
	}
	
	ASS_Track *track = ass_default_track(context->ass_library, context->frame_width, context->frame_height);
	char *eventLine =
		"[Events]\n"
		"Format: EventIndex, Layer, Ignored, Start, End, Style, "
		"Name, MarginL, MarginR, MarginV, Effect, Text\n";
	ass_process_data(track, eventLine, strlen(eventLine));
	
    //handle Byteorder-Mark: http://de.wikipedia.org/wiki/Byte_Order_Mark
	char *line = getlinewithopts(file, conv, 0);
    const uint8_t *bom = (uint8_t*)line;
    const int lineln = strlen(line);
    //fprintf(stderr, "----> linelen=%i, 0x%x-0x%x-0x%x-0x%x\n", lineln, bom[0], bom[1], bom[2], bom[3]);
    uint16_t lineoffset = 0;
    if (lineln>=2){
        BOM_CHECK_2(0xFE, 0xFF, "UTF-16");
    }
    if (lineln>=3){
        BOM_CHECK_3(0xEF, 0xBB, 0xBF, "UTF-8");
        BOM_CHECK_3(0xF7, 0x64, 0x4C, "UTF-1");
        BOM_CHECK_3(0x0E, 0xFE, 0xFF, "SCSU");
    }
    if (lineln>=4){
        BOM_CHECK_4(0x00, 0x00, 0xFE, 0xFF, "UTF-32");
        BOM_CHECK_3(0x2B, 0x2F, 0x76, "UTF-7");
        BOM_CHECK_4(0xDD, 0x73, 0x66, 0x73, "UTF-EBCDIC");
        BOM_CHECK_3(0xFB, 0xEE, 0x28, "BOCU-1");
        BOM_CHECK_4(0x84, 0x31, 0x95, 0x33, "GB 18030");
    }
    char* realline = line + lineoffset;
    
	while (realline != NULL && strlen(realline) != 0)
	{
		char *endp = NULL;
		int subtitle_index = strtol(realline, &endp, 10);
		
		if (endp != realline + strlen(realline))
		{
			av_free(line);
			line = getlinewithopts(file, conv, 0);
            realline = line;
			continue;
		}
		
		av_free(line);
		line = getlinewithopts(file, conv, 0);
        realline = line;
		
		int startHour, startMinute, startSecond, startMilli,
			endHour, endMinute, endSecond, endMilli;
		if (sscanf(realline, "%02d:%02d:%02d,%03d --> %02d:%02d:%02d,%03d",
			&startHour, &startMinute, &startSecond, &startMilli,
			&endHour, &endMinute, &endSecond, &endMilli) != 8)
		{
			av_free(line);
			line = getlinewithopts(file, conv, 0);
            realline = line;
			continue;
		}
		
		av_free(line);
		char *subtitle = getlinewithopts(file, conv, context->flip_srt);
		//fprintf(stderr, "----> subtitle: >>%s<<\n", subtitle);
		size_t subtitleLength = strlen(subtitle);
		if (subtitleLength == 0)
		{
            //fprintf(stderr, "----> nothing to show here\n");
            while (subtitleLength==0){
                av_free(subtitle);
                subtitle = getlinewithopts(file, conv, 0); // load the next index.
                subtitleLength = strlen(realline);
                //fprintf(stderr, "----> moving one one line\n");
            }
            line = subtitle;
            realline = line;
			continue;
		}
		
		subtitle = av_realloc(subtitle, subtitleLength + 2);
		subtitle[subtitleLength] = '\n';
		subtitle[subtitleLength + 1] = '\0';
		
		int lineLength;
		while ((lineLength = strlen(line = getlinewithopts(file, conv, context->flip_srt))) != 0)
		{
            realline = line;
            //fprintf(stderr, "----> line: >>%s<<\n", realline);
			subtitle = av_realloc(subtitle, subtitleLength + 1 + lineLength + 2);
			memcpy(&subtitle[subtitleLength + 1], realline, lineLength + 1);
			av_free(line);
			
			subtitleLength += lineLength + 1;
			subtitle[subtitleLength] = '\n';
			subtitle[subtitleLength + 1] = '\0';
		}
		av_free(line);
		
		size_t subtitleLengthIncNewline = subtitleLength + 1;
        //fprintf(stderr, "----> subtitle2: >>%s<<\n", subtitle);
		subtitle = parse_srt_tags(subtitle, &subtitleLengthIncNewline);
		subtitleLength = subtitleLengthIncNewline - 1;
        //fprintf(stderr, "----> subtitle3: >>%s<<\n", subtitle);
		
		char subtitleBuffer[512];
		
		// The "Ignored" parameter should not actually be present.
		int wrote = snprintf(subtitleBuffer, 512, "%d,0,%1d:%02d:%02d.%02d,%1d:%02d:%02d.%02d,Default,NoName,0000,0000,0000,,",
			subtitle_index, startHour, startMinute, startSecond, startMilli / 10, endHour, endMinute, endSecond, endMilli / 10);
		if (subtitleLength + 1 > 512 - wrote)
		{
			subtitleLength = 512 - wrote - 1;
		}
		av_strlcpy(&subtitleBuffer[wrote], subtitle, subtitleLength + 1);
        //fprintf(stderr, "----> subtitle4: >>%s<<\n", &subtitleBuffer[wrote]);
		ass_process_chunk(track, subtitleBuffer, wrote + subtitleLength + 1,
			startHour * 3600000 + startMinute * 60000 + startSecond * 1000 + startMilli,
			(endHour - startHour) * 3600000 + (endMinute - startMinute) * 60000 + (endSecond - startSecond) * 1000 + (endMilli - startMilli));
		
		line = getlinewithopts(file, conv, 0);
        realline = line;
	}
	av_free(line);
	
	return track;
}

static int config_input(AVFilterLink *link)
{
  //fprintf(stderr, "----> config_input\n");
  AssContext *context = link->dst->priv;

  context->frame_width = link->w;
  context->frame_height = link->h;

  context->ass_library = ass_library_init();

  if ( !context->ass_library ) {
    av_log(0, AV_LOG_ERROR, "ass_library_init() failed!\n");
    return 1;
  }

  ass_set_fonts_dir(context->ass_library, NULL);
  ass_set_extract_fonts(context->ass_library, 1);
  ass_set_style_overrides(context->ass_library, NULL);

  context->ass_renderer = ass_renderer_init(context->ass_library);
  if ( ! context->ass_renderer ) {
    av_log(0, AV_LOG_ERROR, "ass_renderer_init() failed!\n");
    return 1;
  }

  ass_set_frame_size(context->ass_renderer, link->w, link->h);
  ass_set_margins(context->ass_renderer, context->margin, context->margin, context->margin, context->margin);
  ass_set_use_margins(context->ass_renderer, 1);
  ass_set_font_scale(context->ass_renderer, context->font_scale);
  
	  char *conf_path;

#ifdef WINDOWS

  // Windows.
  char *lastSlash = strrchr(executable_path, '\\');
  int length = (long)lastSlash - (long)executable_path;
  asprintf(&conf_path, "%.*s\\%s", length, executable_path, "fonts-windows.conf");

  // Default font.
  char* font_path;
  asprintf(&font_path, "%.*s\\Resources\\%s", length, executable_path, "DejaVuUniversal.ttf");
  ass_set_fonts(context->ass_renderer, font_path, "DejaVu Sans", 0, NULL, 0);

#elif defined(__linux__)

  // Linux.
  char *lastSlash = strrchr(executable_path, '/');
  int length = (long)lastSlash - (long)executable_path;

  // Configuration.
  asprintf(&conf_path, "%.*s/%s", length, executable_path, "fonts.conf");
	  
  // Default font.
  char* font_path;
  asprintf(&font_path, "%.*s/%s", length, executable_path, "DejaVuUniversal.ttf");
  ass_set_fonts(context->ass_renderer, font_path, "DejaVu Sans", 0, NULL, 0);

  av_free(font_path);

#else

  // Mac.
  char *lastSlash = strrchr(executable_path, '/');
  int length = (long)lastSlash - (long)executable_path;
  asprintf(&conf_path, "%.*s/%s", length, executable_path, "fonts.conf");
  
  struct stat st;
  if (stat("/Library/Fonts/Arial Unicode.ttf", &st) == 0)
      ass_set_fonts(context->ass_renderer, "/Library/Fonts/Arial Unicode.ttf", "Arial", 0, NULL, 0);
  else if (stat("/Library/Fonts/Arial.ttf", &st) == 0)
      ass_set_fonts(context->ass_renderer, "/Library/Fonts/Arial.ttf", "Arial", 0, NULL, 0);

#endif

	av_free(conf_path);

  if (context->filename)
  {
  	int filenameLength = strlen(context->filename);
  	if (strncmp(&context->filename[filenameLength - 3], "srt", 3) == 0)
    {
        context->ass_track = import_srt_file(context);
    }
    else
    {
        context->ass_track = ass_read_file(context->ass_library, context->filename, context->encoding);
    }
  }
  else
  {
      context->ass_track = ass_default_track(context->ass_library, context->frame_width, context->frame_height);
  }
  if ( !context->ass_track ) {
    av_log(0, AV_LOG_ERROR, "Failed to read subtitle file with ass_read_file()!\n");
    return 1;
  }

  avcodec_get_chroma_sub_sample(link->format,
				&context->hsub, &context->vsub);

  return 0;
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
  //fprintf(stderr, "----> start_frame\n");
  avfilter_start_frame(link->dst->outputs[0], picref);
}

#define _r(c)  ((c)>>24)
#define _g(c)  (((c)>>16)&0xFF)
#define _b(c)  (((c)>>8)&0xFF)
#define _a(c)  ((c)&0xFF)
#define rgba2y(c)  ( (( 263*_r(c)  + 516*_g(c) + 100*_b(c)) >> 10) + 16  )                                                                     
#define rgba2u(c)  ( ((-152*_r(c) - 298*_g(c) + 450*_b(c)) >> 10) + 128 )                                                                      
#define rgba2v(c)  ( (( 450*_r(c) - 376*_g(c) -  73*_b(c)) >> 10) + 128 )                                                                      

static void draw_ass_image(AVFilterBufferRef *pic, ASS_Image *img, AssContext *context)
{
  //fprintf(stderr, "----> draw_ass_image\n");
  unsigned char *row[4];
  unsigned char c_y = rgba2y(img->color);
  unsigned char c_u = rgba2u(img->color);
  unsigned char c_v = rgba2v(img->color);
  unsigned char opacity = 255 - _a(img->color);
  unsigned char *src;
  int i, j;

  unsigned char *bitmap = img->bitmap;
  int bitmap_w = img->w;
  int bitmap_h = img->h;
  int dst_x = img->dst_x;
  int dst_y = img->dst_y;

  int channel;
  int x,y;

  src = bitmap;

  for (i = 0; i < bitmap_h; ++i) {
    y = dst_y + i;
    if ( y >= pic->video->h )
      break;

    row[0] = pic->data[0] + y * pic->linesize[0];

    for (channel = 1; channel < 3; channel++)
      row[channel] = pic->data[channel] +
	pic->linesize[channel] * (y>> context->vsub);

    for (j = 0; j < bitmap_w; ++j) {
      unsigned k = ((unsigned)src[j]) * opacity >> 8;

      x = dst_x + j;
      if ( y >= pic->video->w )
	break;

      row[0][x] = (k*c_y + (255-k)*row[0][x]) >> 8;
      row[1][x >> context->hsub] = (k*c_u + (255-k)*row[1][x >> context->hsub]) >> 8;
      row[2][x >> context->hsub] = (k*c_v + (255-k)*row[2][x >> context->hsub]) >> 8;
    }

    src += img->stride;
  } 
}

static void end_frame(AVFilterLink *link)
{
  AssContext *context = link->dst->priv;
  AVFilterLink* output = link->dst->outputs[0];
  AVFilterBufferRef *pic = link->cur_buf;
  //fprintf(stderr, "ass img at: %f\n",  (pic->pts + context->pts_offset) * 1000.0 / AV_TIME_BASE);
  //fprintf(stderr, "ass renderer 0x%x, track 0x%x\n", context->ass_renderer, context->ass_track);
  ASS_Image* img = ass_render_frame(context->ass_renderer,
				      context->ass_track,
				      (pic->pts + context->pts_offset) * 1000.0 / AV_TIME_BASE,
				      NULL);

  while ( img ) {
    //fprintf(stderr, "    draw\n");
    draw_ass_image(pic, img, context);
    img = img->next;
  }

  avfilter_draw_slice(output, 0, pic->video->h, 1);
  avfilter_end_frame(output);
}

static int parse_args(AVFilterContext *ctx, AssContext *context, const char* args)
{
  char *arg_copy = av_strdup(args);
  char *strtok_arg = arg_copy;
  char *param;

  while ( param = strtok(strtok_arg, "|") ) {
    char *tmp = param;
    char *param_name;
    char *param_value;

    strtok_arg = NULL;

    while ( *tmp && *tmp != ':' ) {
      tmp++;
    }

    if ( param == tmp || ! *tmp ) {
      av_log(ctx, AV_LOG_ERROR, "Error while parsing arguments - must be like 'param1:value1|param2:value2'\n");
      return 1;
    }

    param_name = av_malloc(tmp - param + 1);
    memset(param_name, 0, tmp - param + 1);
    av_strlcpy(param_name, param, tmp-param +1);

    tmp++;

    if ( ! *tmp ) {
      av_log(ctx, AV_LOG_ERROR, "Error while parsing arguments - parameter value cannot be empty\n");
      return 1;
    }

    param_value = av_strdup(tmp);
    if ( !strcmp("filename", param_name ) ) {
      context->filename = av_strdup(param_value);
    } else if ( !strcmp("pts_offset", param_name ) ) {
      context->pts_offset = atoll(param_value);
    } else if ( !strcmp("font_scale", param_name ) ) {
      context->font_scale = atof(param_value);
    } else if ( !strcmp("encoding", param_name ) ) {
      context->srt_encoding = av_strdup(param_value);
    } else if ( !strcmp("flip_srt", param_name ) ) {
        context->flip_srt = atol(param_value);
    } else if ( !strcmp("fps", param_name ) ) {
        //nothing to do...
    } else {
      av_log(ctx, AV_LOG_WARNING, "Warning while parsing arguments - unsupported parameter '%s'\n", param_name);
      //return 1;
    }
    av_free(param_name);
    av_free(param_value);
  }

  return 0;
}

AVFilter avfilter_vf_inlineass=
  {
    .name      = "inlineass",
    .priv_size = sizeof(AssContext),
    .init      = init,

    .query_formats   = query_formats,
    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame,
                                    .end_frame       = end_frame,
                                    .config_props    = config_input,
                                    .min_perms       = AV_PERM_WRITE | AV_PERM_READ,
                                    .rej_perms       = AV_PERM_REUSE | AV_PERM_REUSE2},
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO, },
                                  { .name = NULL}},
  };
