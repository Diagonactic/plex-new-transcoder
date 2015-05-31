/*
 *  vf_inlinepgs.c
 *  ffmpeg
 *
 *  Created by Frank Bauer on 2010/11/29.
 *  Copyright 2010 Plex Inc. All rights reserved.
 *
 *  Tutorials:  http://www.inb.uni-luebeck.de/~boehme/using_libavcodec.html
 *              http://wiki.multimedia.cx/index.php?title=FFmpeg_filter_HOWTO
 *              http://dranger.com/ffmpeg/tutorial05.html
 *              http://www.ffmpeg.org/libavfilter.html#SEC34
 */

#include "vf_inlinepgs.h"

#define _DARWIN_C_SOURCE // required for asprintf

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "avfilter.h"



#define _r(c)  ((c)>>24)
#define _g(c)  (((c)>>16)&0xFF)
#define _b(c)  (((c)>>8)&0xFF)
#define _a(c)  ((c)&0xFF)
#define rgba2y(c)  ( (( 263*_r(c)  + 516*_g(c) + 100*_b(c)) >> 10) + 16  )                                                                     
#define rgba2u(c)  ( ((-152*_r(c) - 298*_g(c) + 450*_b(c)) >> 10) + 128 )                                                                      
#define rgba2v(c)  ( (( 450*_r(c) - 376*_g(c) -  73*_b(c)) >> 10) + 128 )  

void vf_inlinepgs_set_subtitle_stream(AVFormatContext *fctx, AVFilterContext *ctx, AVStream *stream){
    InlinePGSContext *pgsctx = ctx->priv;
    if (pgsctx->subtitleStream!=NULL) return;
    
    pgsctx->subtitleStream = stream;
    
    // Find the decoder for the video stream
    pgsctx->subtitleCodec = avcodec_find_decoder(pgsctx->subtitleStream->codec->codec_id);
    if(pgsctx->subtitleCodec==NULL){
        fprintf(stderr, "!!! Unable to find Codec !!!\n");
        return;
    }
    
    // Open codec
    if(avcodec_open(pgsctx->subtitleStream->codec, pgsctx->subtitleCodec)<0)
    {
        fprintf(stderr, "!!! Unable to open Codec !!!\n");
        return; // Could not open codec
    }
    
    pgsctx->subtitleFrame = avcodec_alloc_frame();
    pgsctx->formatContext = fctx;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    InlinePGSContext *pgsctx= ctx->priv;
    
    pgsctx->subtitleStream = NULL;
    pgsctx->subtitleCodec = NULL;
    
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    InlinePGSContext *pgsctx= ctx->priv;
    
    if (pgsctx->subtitleStream!=NULL){
        // Free the YUV frame
        av_free(pgsctx->subtitleFrame);
        
        avcodec_close(pgsctx->subtitleStream->codec);
        pgsctx->subtitleStream = NULL;
    }
}

static int config_props(AVFilterLink *link)
{
    InlinePGSContext *pgsctx = link->dst->priv;
    
    
    return 0;
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    fprintf(stderr, "Start Frame\n");
    InlinePGSContext *pgsctx = link->dst->priv;
    AVCodecContext *pCodecCtx = pgsctx->subtitleStream->codec;
    AVFormatContext *pFormatCtx = pgsctx->formatContext;
    AVFilterBufferRef *pic = link->cur_buf;
    pgsctx->current_pts = pic->pts;
    
    avfilter_default_start_frame(link, picref);
    
    AVFrame *pFrame;
    AVFrame *pFrameRGB;
    
    // Allocate video frame
    pFrame = avcodec_alloc_frame();
    
    // Allocate an AVFrame structure
    pFrameRGB=avcodec_alloc_frame();
    if(pFrameRGB==NULL)
        return;
    
    uint8_t *buffer;
    int numBytes;
    // Determine required buffer size and allocate buffer
    numBytes=avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width,
                                pCodecCtx->height);
    buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
    
    // Assign appropriate parts of buffer to image planes in pFrameRGB
    // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
    // of AVPicture
    avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24, pCodecCtx->width,
                   pCodecCtx->height);
    
    /*int frameFinished;
    AVPacket packet;
    
    int i;
    i=0;
    while(av_read_frame(pFormatCtx, &packet)>=0) {
        // Is this a packet from the video stream?
        if(packet.stream_index==videoStream) {
            // Decode video frame
            avcodec_decode_video(pCodecCtx, pFrame, &frameFinished,
                                 packet.data, packet.size);
            
            // Did we get a video frame?
            if(frameFinished) {
                // Convert the image from its native format to RGB
                img_convert((AVPicture *)pFrameRGB, PIX_FMT_RGB24, 
                            (AVPicture*)pFrame, pCodecCtx->pix_fmt, 
                            pCodecCtx->width, pCodecCtx->height);
                
                // Save the frame to disk
                if(++i<=5)
                    SaveFrame(pFrameRGB, pCodecCtx->width, 
                              pCodecCtx->height, i);
            }
        }
        
        // Free the packet that was allocated by av_read_frame
        av_free_packet(&packet);
    }*/
}

static void draw_slice(AVFilterLink *link, int y, int h)
{
    fprintf(stderr, "Draw Slice\n");
    InlinePGSContext *pgsctx = link->dst->priv;
    AVFilterBufferRef *in  = link->cur_buf;
    AVFilterBufferRef *out = link->dst->outputs[0]->out_buf;
    
    
    
    //8 planes(channels are available in AVFilterBuffer.data
    for (int plane=0; plane<8; plane++){
        
        //something is stored in the buffer?
        if (in->linesize[plane]>0){
            
            //if the width is scaled, we assume the height is scaled as well
            const float scale = (float)out->linesize[plane] / out->linesize[0];
            const int maxRow = h*scale;
            
            uint8_t* inrow  = in->data[plane] + y * in->linesize[plane];
            uint8_t* outrow = out->data[plane] + y * out->linesize[plane];
            const unsigned int testColor = 0xFF00FF00;
            for (int i = 0; i < maxRow; i++) {
                for (int j = 0; j < in->linesize[plane]; j++){
                    if (plane==0) outrow[j] = rgba2y(testColor);
                     else if (plane==1) outrow[j] = rgba2u(testColor);
                     else if (plane==2) outrow[j] = rgba2v(testColor);
                    outrow[j] = inrow[j];
                }
                
                inrow += in->linesize[plane];
                outrow += out->linesize[plane];
            }
        } else {
            break;
        }
    }
    //avfilter_draw_slice(link->dst->outputs[0], y, h, 1);
}

static void end_frame(AVFilterLink *link)
{
    fprintf(stderr, "End Frame\n");
    avfilter_default_end_frame(link);
}

AVFilter avfilter_vf_inlinepgs =
{
    .name      = "inlinepgs",
    .description = NULL_IF_CONFIG_SMALL("Overlays a PGS subtitle stream onto the video."),
    
    .priv_size = sizeof(InlinePGSContext),
    .init      = init,
    .uninit    = uninit,
    .inputs    = (AVFilterPad[]) {
        { .name            = "default",
            .type            = AVMEDIA_TYPE_VIDEO,
            .draw_slice      = draw_slice,
            .start_frame     = start_frame,
            .end_frame       = end_frame,
            .config_props    = config_props,
            .min_perms       = AV_PERM_READ, 
        },
        { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
        .type            = AVMEDIA_TYPE_VIDEO, },
        { .name = NULL}},
};