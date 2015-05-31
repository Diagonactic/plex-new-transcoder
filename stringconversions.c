/*
 *  stringconversions.c
 *  ffmpeg-servetome
 *
 *  Created by Matt Gallagher on 2011/01/14.
 *  Copyright 2011 Matt Gallagher. All rights reserved.
 *
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include "stringconversions.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <wchar.h>

//#include "UniversalDetector.h"
#include "iconv.h"
#include "libavutil/mem.h"

#ifdef __linux__
char *strdup(const char *s);
#endif

size_t sizeToEvenPositionZero(const char *c)
{
	size_t result = 0;
	while (c[0] != 0)
	{
		result+=2;
		c+=2;
	}
	return result;
}

wchar_t *utf8_to_new_utf16le(const char *filename)
{
#ifdef _WIN32

  int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, NULL, 0);
  if (len)
  {
    LPWSTR utf16;
    len ++;

    utf16 = (LPWSTR)av_malloc(len * sizeof(WCHAR));
    ZeroMemory(utf16, len * sizeof(WCHAR));

    len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, utf16, len);
    if (len)
      return utf16;
    free(utf16);
  }
  return NULL;

#else

	iconv_t conv = iconv_open("UTF-16LE", "UTF-8");
	
	size_t filename_length = strlen(filename);

	size_t wfilename_length = filename_length * 2;
	size_t wfilename_length_left = wfilename_length;
	char *wfilename = av_malloc(wfilename_length + 2);
	char *wfilename_tracking = wfilename;
	iconv(conv, (char **)&filename, &filename_length, &wfilename_tracking, &wfilename_length_left);
	wfilename_length -= wfilename_length_left;
	wfilename[wfilename_length] = '\0';
	wfilename[wfilename_length+1] = '\0';
	
	iconv_close(conv);
	
	return (wchar_t *)wfilename;

#endif
}

char *utf16le_withbytelength_to_new_utf8(const wchar_t *wfilename, size_t wfilename_length)
{
#ifdef _WIN32

  int len = WideCharToMultiByte(CP_UTF8, 0, wfilename, wfilename_length/2, NULL, 0, NULL, NULL);
  if (len)
  {
    LPSTR utf8;
    len ++;

    utf8 = (LPSTR)av_malloc(len);
    ZeroMemory(utf8, len);

    len = WideCharToMultiByte(CP_UTF8, 0, wfilename, wfilename_length/2, utf8, len, NULL, NULL);
    if (len)
      return utf8;
    free(utf8);
  }
  return NULL;

#else

	iconv_t conv = iconv_open("UTF-8", "UTF-16LE");
	
	size_t filename_length = wfilename_length * 2;
	size_t filename_length_left = filename_length;
	char *filename = av_malloc(filename_length + 1);
	char *filename_tracking = filename;
	iconv(conv, (char **)&wfilename, &wfilename_length, &filename_tracking, &filename_length_left);
	filename_length -= filename_length_left;
	filename[filename_length] = '\0';
	
	iconv_close(conv);
	
	return filename;

#endif
}

// These seem to be unused
#ifndef _WIN32
char *utf16be_withbytelength_to_new_utf8(const wchar_t *wfilename, size_t wfilename_length)
{
	iconv_t conv = iconv_open("UTF-8", "UTF-16BE");
	
	size_t filename_length = wfilename_length * 2;
	size_t filename_length_left = filename_length;
	char *filename = av_malloc(filename_length + 1);
	char *filename_tracking = filename;
	iconv(conv, (char **)&wfilename, &wfilename_length, &filename_tracking, &filename_length_left);
	filename_length -= filename_length_left;
	filename[filename_length] = '\0';
	
	iconv_close(conv);
	
	return filename;
}

char *utf16_withbytelength_to_utf8(const wchar_t *wfilename, size_t wfilename_length)
{
	iconv_t conv = iconv_open("UTF-8", "UTF-16");
	
	size_t filename_length = wfilename_length * 2;
	size_t filename_length_left = filename_length;
	char *filename = av_malloc(filename_length + 1);
	char *filename_tracking = filename;
	iconv(conv, (char **)&wfilename, &wfilename_length, &filename_tracking, &filename_length_left);
	filename_length -= filename_length_left;
	filename[filename_length] = '\0';
	
	iconv_close(conv);
	
	return filename;
}

char *latin1_to_new_utf8(const char *wfilename, size_t wfilename_length)
{
	iconv_t conv = iconv_open("UTF-8", "ISO-8859-1");
	
	size_t filename_length = wfilename_length * 2;
	size_t filename_length_left = filename_length;
	char *filename = av_malloc(filename_length + 1);
	char *filename_tracking = filename;
	iconv(conv, (char **)&wfilename, &wfilename_length, &filename_tracking, &filename_length_left);
	filename_length -= filename_length_left;
	filename[filename_length] = '\0';
	
	iconv_close(conv);
	
	return filename;
}
#endif

char *utf16le_to_new_utf8(const wchar_t *wfilename)
{
	size_t wfilename_length = sizeToEvenPositionZero((const char *)wfilename);
	
	return utf16le_withbytelength_to_new_utf8(wfilename, wfilename_length);
}

const char *encodingForFile(const char *filename)
{
	return strdup("unknown");
#if 0
#ifdef WINDOWS
	wchar_t *wother = utf8_to_new_utf16le(filename);
	FILE *file = _wfopen(wother, L"r");
	av_free(wother);
#else
	FILE *file = fopen(filename, "r");
#endif
	
	fseek(file, 0, SEEK_END);
	size_t file_length = ftell(file);
	fseek(file, 0, SEEK_SET);
	char *buf = av_malloc(file_length);
	file_length = fread(buf, 1, file_length, file);
	if (file_length == 0)
	{
		av_free(buf);
		return "UTF-8";
	}
	
	const char *result = mimeCharsetForData(buf, file_length, NULL);
	av_free(buf);
	
	if (0 /*STM_VERBOSITY > 0*/)
	{
	    av_log(NULL, AV_LOG_WARNING, "File: %s Detected Encoding: %s\n", filename, result);
	}
	
	return result;
#endif
}

#if 0
char * getline(FILE *fp) {
    char * line = av_malloc(100), * linep = line;
    size_t lenmax = 100, len = lenmax;
    int c;

    if(line == NULL)
        return NULL;

    for(;;) {
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

        if(--len == 1) {
			char * linen = av_realloc(linep, lenmax *= 2);

            if(linen == NULL) {
                av_free(linep);
                return NULL;
            }
            line = linen + (line - linep);
            linep = linen;
            len = lenmax - (size_t)(line - linep) - 1;
        }

        *line++ = c;
    }
    *line++ = '\0';
    *line = '\0';
    return linep;
}
#endif
