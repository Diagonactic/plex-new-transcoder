/*
 *  stringconversions.h
 *  ffmpeg-servetome
 *
 *  Created by Matt Gallagher on 2011/01/14.
 *  Copyright 2011 Matt Gallagher. All rights reserved.
 *
 */
 
#ifndef STM_STRINGCONVERSIONS_H
#define STM_STRINGCONVERSIONS_H

#include <stdio.h>
#include <wchar.h>

wchar_t *utf8_to_new_utf16le(const char *filename);
char *utf16le_withbytelength_to_new_utf8(const wchar_t *wfilename, size_t wfilename_length);
char *utf16be_withbytelength_to_new_utf8(const wchar_t *wfilename, size_t wfilename_length);
char *utf16_withbytelength_to_utf8(const wchar_t *wfilename, size_t wfilename_length);
char *latin1_to_new_utf8(const char *wfilename, size_t wfilename_length);
char *utf16le_to_new_utf8(const wchar_t *wfilename);
const char *encodingForFile(const char *filename);
char * my_getline(FILE *fp);
size_t sizeToEvenPositionZero(const char *c);

#endif /* STM_STRINGCONVERSIONS_H */
