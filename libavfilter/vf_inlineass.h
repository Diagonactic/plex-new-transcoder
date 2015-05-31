/*
 *  vf_inlineass.h
 *  ffmpeg
 *
 *  Created by Matt Gallagher on 2010/02/03.
 *  Copyright 2010 Matt Gallagher. All rights reserved.
 *
 */

extern char *executable_path;
extern char *subtitle_path;

void vf_inlineass_append_data(AVFilterContext *link, enum CodecID codecID, char *data, int dataSize, int64_t pts, int64_t duration, AVRational time_base);
void vf_inlineass_set_aspect_ratio(AVFilterContext *context, double dar);
