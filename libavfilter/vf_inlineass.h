/*
 *  vf_inlineass.h
 *  ffmpeg
 *
 *  Created by Matt Gallagher on 2010/02/03.
 *  Copyright 2010 Matt Gallagher. All rights reserved.
 *
 */
void vf_inlineass_process_header(AVFilterContext *link,
                                 AVCodecContext *dec_ctx);
void vf_inlineass_append_data(AVFilterContext *link, AVStream *stream,
                              AVPacket *pkt);
void vf_inlineass_set_aspect_ratio(AVFilterContext *context, double dar);
void vf_inlineass_add_attachment(AVFilterContext *context, AVStream *st);
void vf_inlineass_set_fonts(AVFilterContext *context);
void vf_inlineass_set_storage_size(AVFilterContext *context, int w, int h);

