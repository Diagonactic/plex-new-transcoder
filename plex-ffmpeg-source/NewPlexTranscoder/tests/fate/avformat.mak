FATE_LAVF-$(call ENCDEC,  PCM_S16BE,             AIFF)               += aiff
FATE_LAVF-$(call ENCDEC,  PCM_ALAW,              PCM_ALAW)           += alaw
FATE_LAVF-$(call ENCDEC,  APNG,                  APNG)               += apng
FATE_LAVF-$(call ENCDEC2, MSMPEG4V3,  MP2,       ASF)                += asf
FATE_LAVF-$(call ENCDEC,  PCM_S16BE_PLANAR,      AST)                += ast
FATE_LAVF-$(call ENCDEC,  PCM_S16BE,             AU)                 += au
FATE_LAVF-$(call ENCDEC2, MPEG4,      MP2,       AVI)                += avi
FATE_LAVF-$(call ENCDEC,  BMP,                   IMAGE2)             += bmp
FATE_LAVF-$(call ENCDEC,  PCM_S16BE,             CAF)                += caf
FATE_LAVF-$(call ENCDEC,  DPX,                   IMAGE2)             += dpx
FATE_LAVF-$(call ENCDEC2, DVVIDEO,    PCM_S16LE, AVI)                += dv_fmt
FATE_LAVF-$(call ENCDEC,  FITS,                  FITS)               += fits
FATE_LAVF-$(call ENCDEC,  RAWVIDEO,              FILMSTRIP)          += flm
FATE_LAVF-$(call ENCDEC,  FLV,                   FLV)                += flv_fmt
FATE_LAVF-$(call ENCDEC,  GIF,                   IMAGE2)             += gif
FATE_LAVF-$(call ENCDEC2, MPEG2VIDEO, PCM_S16LE, GXF)                += gxf
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             IRCAM)              += ircam
FATE_LAVF-$(call ENCDEC,  MJPEG,                 IMAGE2)             += jpg
FATE_LAVF-$(call ENCMUX,  TTA,                   MATROSKA_AUDIO)     += mka
FATE_LAVF-$(call ENCDEC2, MPEG4,      MP2,       MATROSKA)           += mkv
FATE_LAVF-$(call ENCDEC,  ADPCM_YAMAHA,          MMF)                += mmf
FATE_LAVF-$(call ENCDEC2, MPEG4,      PCM_ALAW,  MOV)                += mov ismv
FATE_LAVF-$(call ENCDEC2, MPEG1VIDEO, MP2,       MPEG1SYSTEM MPEGPS) += mpg
FATE_LAVF-$(call ENCDEC,  PCM_MULAW,             PCM_MULAW)          += mulaw
FATE_LAVF-$(call ENCDEC2, MPEG2VIDEO, PCM_S16LE, MXF)                += mxf
FATE_LAVF-$(call ENCDEC2, MPEG2VIDEO, PCM_S16LE, MXF_D10 MXF)        += mxf_d10
FATE_LAVF-$(call ENCDEC2, DVVIDEO,    PCM_S16LE, MXF)                += mxf_dv25
FATE_LAVF-$(call ENCDEC2, DVVIDEO,    PCM_S16LE, MXF)                += mxf_dvcpro50
FATE_LAVF-$(call ENCDEC2, DNXHD,      PCM_S16LE, MXF_OPATOM MXF)     += mxf_opatom
FATE_LAVF-$(call ENCDEC2, DNXHD,      PCM_S16LE, MXF_OPATOM MXF)     += mxf_opatom_audio
FATE_LAVF-$(call ENCDEC2, MPEG4,      MP2,       NUT)                += nut
FATE_LAVF-$(call ENCDEC,  FLAC,                  OGG)                += ogg
FATE_LAVF-$(call ENCDEC,  PAM,                   IMAGE2)             += pam
FATE_LAVF-$(call ENCDEC,  PBM,                   IMAGE2PIPE)         += pbmpipe
FATE_LAVF-$(call ENCDEC,  PCX,                   IMAGE2)             += pcx
FATE_LAVF-$(call ENCDEC,  PGM,                   IMAGE2)             += pgm
FATE_LAVF-$(call ENCDEC,  PGM,                   IMAGE2PIPE)         += pgmpipe
FATE_LAVF-$(call ENCDEC,  PNG,                   IMAGE2)             += png
FATE_LAVF-$(call ENCDEC,  PPM,                   IMAGE2)             += ppm
FATE_LAVF-$(call ENCDEC,  PPM,                   IMAGE2PIPE)         += ppmpipe
FATE_LAVF-$(call ENCMUX,  RV10 AC3_FIXED,        RM)                 += rm
FATE_LAVF-$(call ENCDEC,  PCM_U8,                RSO)                += rso
FATE_LAVF-$(call ENCDEC,  SGI,                   IMAGE2)             += sgi
FATE_LAVF-$(call ENCMUX,  MJPEG PCM_S16LE,       SMJPEG)             += smjpeg
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             SOX)                += sox
FATE_LAVF-$(call ENCDEC,  SUNRAST,               IMAGE2)             += sunrast
FATE_LAVF-$(call ENCDEC,  FLV,                   SWF)                += swf
FATE_LAVF-$(call ENCDEC,  TARGA,                 IMAGE2)             += tga
FATE_LAVF-$(call ENCDEC,  TIFF,                  IMAGE2)             += tiff
FATE_LAVF-$(call ENCDEC2, MPEG2VIDEO, MP2,       MPEGTS)             += ts
FATE_LAVF-$(call ENCDEC,  TTA,                   TTA)                += tta
FATE_LAVF-$(call ENCDEC,  PCM_U8,                VOC)                += voc
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             VOC)                += voc_s16
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             WAV)                += wav
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             WAV)                += wav_peak
FATE_LAVF-$(call ENCDEC,  PCM_S16LE,             WAV)                += wav_peak_only
FATE_LAVF-$(call ENCMUX,  PCM_S16LE,             W64)                += w64
FATE_LAVF-$(call ENCDEC,  MP2,                   WTV)                += wtv
FATE_LAVF-$(call ENCDEC,  WAVPACK,               WV)                 += wv
FATE_LAVF-$(call ENCDEC,  XBM,                   IMAGE2)             += xbm
FATE_LAVF-$(call ENCDEC,  XWD,                   IMAGE2)             += xwd
FATE_LAVF-$(CONFIG_YUV4MPEGPIPE_MUXER)                               += yuv4mpeg

FATE_LAVF += $(FATE_LAVF-yes:%=fate-lavf-%)
FATE_LAVF_PIXFMT-$(CONFIG_SCALE_FILTER) += fate-lavf-pixfmt
FATE_LAVF += $(FATE_LAVF_PIXFMT-yes)

$(FATE_LAVF): $(AREF) $(VREF)
$(FATE_LAVF): CMD = lavftest
$(FATE_LAVF): CMP =

FATE_AVCONV += $(FATE_LAVF)
fate-lavf:     $(FATE_LAVF)

FATE_LAVF_FATE-$(call ALLYES, MATROSKA_DEMUXER   OGG_MUXER)          += ogg_vp3
FATE_LAVF_FATE-$(call ALLYES, MATROSKA_DEMUXER   OGV_MUXER)          += ogg_vp8
FATE_LAVF_FATE-$(call ALLYES, MOV_DEMUXER        LATM_MUXER)         += latm
FATE_LAVF_FATE-$(call ALLYES, MP3_DEMUXER        MP3_MUXER)          += mp3
FATE_LAVF_FATE-$(call ALLYES, MOV_DEMUXER        MOV_MUXER)          += mov_qtrle_mace6
FATE_LAVF_FATE-$(call ALLYES, AVI_DEMUXER        AVI_MUXER)          += avi_cram

FATE_LAVF_FATE +=  $(FATE_LAVF_FATE-yes:%=fate-lavf-fate-%)
$(FATE_LAVF_FATE): CMD = lavffatetest

FATE_SAMPLES_FFMPEG += $(FATE_LAVF_FATE)
fate-lavf-fate:        $(FATE_LAVF_FATE)

tests/data/mp4-to-ts.m3u8: TAG = GEN
tests/data/mp4-to-ts.m3u8: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< \
        -i $(TARGET_SAMPLES)/h264/interlaced_crop.mp4 \
        -f ssegment -segment_time 1 -map 0 -flags +bitexact -codec copy \
        -segment_list $(TARGET_PATH)/$@ -y $(TARGET_PATH)/tests/data/mp4-to-ts-%03d.ts -nostdin 2>/dev/null

tests/data/adts-to-mkv.m3u8: TAG = GEN
tests/data/adts-to-mkv.m3u8: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< \
        -i $(TARGET_SAMPLES)/audiomatch/tones_afconvert_16000_mono_aac_lc.adts \
        -f segment -segment_time 1 -map 0 -flags +bitexact -codec copy -segment_format_options live=1 \
        -segment_list $(TARGET_PATH)/$@ -y $(TARGET_PATH)/tests/data/adts-to-mkv-%03d.mkv -nostdin 2>/dev/null

tests/data/adts-to-mkv-header.mkv: TAG = GEN
tests/data/adts-to-mkv-header.mkv: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< \
        -i $(TARGET_SAMPLES)/audiomatch/tones_afconvert_16000_mono_aac_lc.adts \
        -f segment -segment_time 1 -map 0 -flags +bitexact -codec copy -segment_format_options live=1 \
        -segment_header_filename $(TARGET_PATH)/tests/data/adts-to-mkv-header.mkv \
        -y $(TARGET_PATH)/tests/data/adts-to-mkv-header-%03d.mkv -nostdin 2>/dev/null

tests/data/adts-to-mkv-header-%.mkv: tests/data/adts-to-mkv-header.mkv ;

FATE_SEGMENT_PARTS += 000 001 002

tests/data/ac3-to-mp4-header.mp4: TAG = GEN
tests/data/ac3-to-mp4-header.mp4: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< \
        -i $(TARGET_SAMPLES)/ac3/monsters_inc_5.1_448_small.ac3 \
        -f segment -segment_time 1 -map 0 -flags +bitexact -codec copy \
        -segment_header_filename $(TARGET_PATH)/tests/data/ac3-to-mp4-header.mp4 \
        -segment_format_options movflags=frag_custom+dash+delay_moov \
        -y $(TARGET_PATH)/tests/data/ac3-to-mp4-header-%03d.mp4 -nostdin 2>/dev/null

tests/data/ac3-to-mp4-header-%.mp4: tests/data/ac3-to-mp4-header.mp4 ;

FATE_AC3_SEGMENT_PARTS += 000 001

tests/data/eac3-to-mp4-header.mp4: TAG = GEN
tests/data/eac3-to-mp4-header.mp4: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< \
        -i $(TARGET_SAMPLES)/eac3/csi_miami_5.1_256_spx_small.eac3 \
        -f segment -segment_time 1 -map 0 -flags +bitexact -codec copy \
        -segment_header_filename $(TARGET_PATH)/tests/data/eac3-to-mp4-header.mp4 \
        -segment_format_options movflags=frag_custom+dash+delay_moov \
        -y $(TARGET_PATH)/tests/data/eac3-to-mp4-header-%03d.mp4 -nostdin 2>/dev/null

tests/data/eac3-to-mp4-header-%.mp4: tests/data/eac3-to-mp4-header.mp4 ;

tests/data/adts-to-mkv-cated-all.mkv: TAG = GEN
tests/data/adts-to-mkv-cated-all.mkv: tests/data/adts-to-mkv-header.mkv $(FATE_SEGMENT_PARTS:%=tests/data/adts-to-mkv-header-%.mkv) | tests/data
	$(M)cat $^ >$@

tests/data/adts-to-mkv-cated-%.mkv: TAG = GEN
tests/data/adts-to-mkv-cated-%.mkv: tests/data/adts-to-mkv-header.mkv tests/data/adts-to-mkv-header-%.mkv | tests/data
	$(M)cat $^ >$@

tests/data/ac3-to-mp4-cated-all.mp4: TAG = GEN
tests/data/ac3-to-mp4-cated-all.mp4: tests/data/ac3-to-mp4-header.mp4 $(FATE_AC3_SEGMENT_PARTS:%=tests/data/ac3-to-mp4-header-%.mp4) | tests/data
	$(M)cat $^ >$@

tests/data/ac3-to-mp4-cated-%.mp4: TAG = GEN
tests/data/ac3-to-mp4-cated-%.mp4: tests/data/ac3-to-mp4-header.mp4 tests/data/ac3-to-mp4-header-%.mp4 | tests/data
	$(M)cat $^ >$@

tests/data/eac3-to-mp4-cated-all.mp4: TAG = GEN
tests/data/eac3-to-mp4-cated-all.mp4: tests/data/eac3-to-mp4-header.mp4 $(FATE_AC3_SEGMENT_PARTS:%=tests/data/eac3-to-mp4-header-%.mp4) | tests/data
	$(M)cat $^ >$@

tests/data/eac3-to-mp4-cated-%.mp4: TAG = GEN
tests/data/eac3-to-mp4-cated-%.mp4: tests/data/eac3-to-mp4-header.mp4 tests/data/eac3-to-mp4-header-%.mp4 | tests/data
	$(M)cat $^ >$@

FATE_SEGMENT += fate-segment-mp4-to-ts
fate-segment-mp4-to-ts: tests/data/mp4-to-ts.m3u8
fate-segment-mp4-to-ts: CMD = framecrc -flags +bitexact -i $(TARGET_PATH)/tests/data/mp4-to-ts.m3u8 -c copy
FATE_SEGMENT-$(call ALLYES, MOV_DEMUXER H264_MP4TOANNEXB_BSF MPEGTS_MUXER MATROSKA_DEMUXER SEGMENT_MUXER HLS_DEMUXER) += fate-segment-mp4-to-ts

FATE_SEGMENT += fate-segment-adts-to-mkv
fate-segment-adts-to-mkv: tests/data/adts-to-mkv.m3u8
fate-segment-adts-to-mkv: CMD = framecrc -flags +bitexact -i $(TARGET_PATH)/tests/data/adts-to-mkv.m3u8 -c copy
fate-segment-adts-to-mkv: REF = $(SRC_PATH)/tests/ref/fate/segment-adts-to-mkv-header-all
FATE_SEGMENT-$(call ALLYES, AAC_DEMUXER AAC_ADTSTOASC_BSF MATROSKA_MUXER MATROSKA_DEMUXER SEGMENT_MUXER HLS_DEMUXER) += fate-segment-adts-to-mkv

FATE_SEGMENT_ALLPARTS = $(FATE_SEGMENT_PARTS)
FATE_SEGMENT_ALLPARTS += all
FATE_SEGMENT_SPLIT += $(FATE_SEGMENT_ALLPARTS:%=fate-segment-adts-to-mkv-header-%)
$(foreach N,$(FATE_SEGMENT_ALLPARTS),$(eval $(N:%=fate-segment-adts-to-mkv-header-%): tests/data/adts-to-mkv-cated-$(N).mkv))
fate-segment-adts-to-mkv-header-%: CMD = framecrc -flags +bitexact -i $(TARGET_PATH)/tests/data/$(@:fate-segment-adts-to-mkv-header-%=adts-to-mkv-cated-%).mkv -c copy
FATE_SEGMENT-$(call ALLYES, AAC_DEMUXER AAC_ADTSTOASC_BSF MATROSKA_MUXER MATROSKA_DEMUXER SEGMENT_MUXER) += $(FATE_SEGMENT_SPLIT)

FATE_SEGMENT_AC3_ALLPARTS = $(FATE_AC3_SEGMENT_PARTS)
FATE_SEGMENT_AC3_ALLPARTS += all
FATE_SEGMENT_AC3_SPLIT += $(FATE_SEGMENT_AC3_ALLPARTS:%=fate-segment-ac3-to-mp4-header-%)
$(foreach N,$(FATE_SEGMENT_ALLPARTS),$(eval $(N:%=fate-segment-ac3-to-mp4-header-%): tests/data/ac3-to-mp4-cated-$(N).mp4))
fate-segment-ac3-to-mp4-header-%: CMD = framecrc -flags +bitexact -i $(TARGET_PATH)/tests/data/$(@:fate-segment-ac3-to-mp4-header-%=ac3-to-mp4-cated-%).mp4 -c copy
fate-segment-ac3-to-mp4-header-md5sum: tests/data/ac3-to-mp4-header.mp4
fate-segment-ac3-to-mp4-header-md5sum: CMD = do_md5sum tests/data/ac3-to-mp4-header.mp4
FATE_SEGMENT_AC3_SPLIT += fate-segment-ac3-to-mp4-header-md5sum
FATE_SEGMENT-$(call ALLYES, AC3_DEMUXER MP4_MUXER MOV_DEMUXER SEGMENT_MUXER) += $(FATE_SEGMENT_AC3_SPLIT)

FATE_SEGMENT_EAC3_SPLIT += $(FATE_SEGMENT_AC3_ALLPARTS:%=fate-segment-eac3-to-mp4-header-%)
$(foreach N,$(FATE_SEGMENT_ALLPARTS),$(eval $(N:%=fate-segment-eac3-to-mp4-header-%): tests/data/eac3-to-mp4-cated-$(N).mp4))
fate-segment-eac3-to-mp4-header-%: CMD = framecrc -flags +bitexact -i $(TARGET_PATH)/tests/data/$(@:fate-segment-eac3-to-mp4-header-%=eac3-to-mp4-cated-%).mp4 -c copy
fate-segment-eac3-to-mp4-header-md5sum: tests/data/eac3-to-mp4-header.mp4
fate-segment-eac3-to-mp4-header-md5sum: CMD = do_md5sum tests/data/eac3-to-mp4-header.mp4
FATE_SEGMENT_EAC3_SPLIT += fate-segment-eac3-to-mp4-header-md5sum
FATE_SEGMENT-$(call ALLYES, EAC3_DEMUXER AC3_PARSER MP4_MUXER MOV_DEMUXER SEGMENT_MUXER) += $(FATE_SEGMENT_EAC3_SPLIT)

FATE_SAMPLES_FFMPEG += $(FATE_SEGMENT-yes)

fate-segment: $(FATE_SEGMENT-yes)
