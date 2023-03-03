/*
 * VVC video Decoder
 *
 * Copyright (C) 2012 - 2021 Pierre-Loup Cabarat
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

#include <ovdec.h>
#include <ovdefs.h>
#include <ovunits.h>
#include <ovframe.h>

#include "libavutil/attributes.h"
#include "libavutil/opt.h"

#include "bytestream.h"

#include "profiles.h"
#include "avcodec.h"
#include "vvc.h"
#include "h2645_parse.h"

struct OVDecContext{
     AVClass *c;
     OVVCDec* libovvc_dec;
     int nal_length_size;
     int is_nalff;
     int64_t log_level;
     int64_t nb_entry_th;
     int64_t nb_frame_th;
     uint8_t *last_extradata;
};

#define OFFSET(x) offsetof(struct OVDecContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption options[] = {
    { "threads_frame", "Number of threads to be used on frames", OFFSET(nb_frame_th),
        AV_OPT_TYPE_INT, {.i64 = 0}, 0, 16, PAR },
    { "threads_tile", "Number of threads to be used on tiles", OFFSET(nb_entry_th),
        AV_OPT_TYPE_INT, {.i64 = 8}, 0, 16, PAR },
    { "log_level", "Verbosity of OpenVVC decoder", OFFSET(log_level),
        AV_OPT_TYPE_INT, {.i64 = 1}, 0, 5, PAR },
    { NULL },
};

static const AVClass libovvc_decoder_class = {
    .class_name = "Open VVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int copy_rpbs_info(OVNALUnit **ovnalu_p, const uint8_t *rbsp_buffer, int raw_size, const int *skipped_bytes_pos, int skipped_bytes) {

    uint8_t *rbsp_cpy = av_malloc(raw_size + 8);
    OVNALUnit *ovnalu = av_mallocz(sizeof(OVNALUnit));
    if (!ovnalu) {
        return AVERROR(ENOMEM);
    }
    ov_nalu_init(ovnalu);

    /* TODO check allocs */
    memcpy(rbsp_cpy, rbsp_buffer, raw_size);
    rbsp_cpy[raw_size]     = 0;
    rbsp_cpy[raw_size + 1] = 0;
    rbsp_cpy[raw_size + 2] = 0;
    rbsp_cpy[raw_size + 3] = 0;
    rbsp_cpy[raw_size + 4] = 0;
    rbsp_cpy[raw_size + 5] = 0;
    rbsp_cpy[raw_size + 6] = 0;
    rbsp_cpy[raw_size + 7] = 0;

    ovnalu->rbsp_data = rbsp_cpy;
    ovnalu->rbsp_size = raw_size;

    if (skipped_bytes) {
        int *epb_cpy = av_malloc(skipped_bytes * sizeof (*ovnalu->epb_pos));
        memcpy(epb_cpy, skipped_bytes_pos, skipped_bytes * sizeof (*ovnalu->epb_pos));

        ovnalu->epb_pos = epb_cpy;
        ovnalu->nb_epb = skipped_bytes;
    }

    *ovnalu_p = ovnalu;

    return 0;
}

static int convert_avpkt(OVPictureUnit *ovpu, const H2645Packet *pkt) {
    int i;
    ovpu->nb_nalus = pkt->nb_nals;
    ovpu->nalus = av_malloc(sizeof(*ovpu->nalus) * ovpu->nb_nalus);
    if (!ovpu->nb_nalus) {
        av_log(NULL, AV_LOG_ERROR, "No NAL Unit in packet.\n");
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < ovpu->nb_nalus; ++i) {
         const H2645NAL *avnalu = &pkt->nals[i];
         OVNALUnit **ovnalu_p = &ovpu->nalus[i];
         copy_rpbs_info(ovnalu_p, avnalu->rbsp_buffer, avnalu->raw_size, avnalu->skipped_bytes_pos, avnalu->skipped_bytes);
         (*ovnalu_p)->type = avnalu->type;
    }

    return 0;
}

static int unref_ovvc_nalus(OVPictureUnit *ovpu) {
    int i;
    for (i = 0; i < ovpu->nb_nalus; ++i) {
         OVNALUnit **ovnalu_p = &ovpu->nalus[i];
         ov_nalu_unref(ovnalu_p);
    }
    return 0;
}

static void ovvc_unref_ovframe(void *opaque, uint8_t *data) {

    OVFrame **frame_p = (OVFrame **)&data;
    ovframe_unref(frame_p);
}

static void convert_ovframe(AVFrame *avframe, const OVFrame *ovframe) {
    avframe->data[0] = ovframe->data[0];
    avframe->data[1] = ovframe->data[1];
    avframe->data[2] = ovframe->data[2];

    avframe->linesize[0] = ovframe->linesize[0];
    avframe->linesize[1] = ovframe->linesize[1];
    avframe->linesize[2] = ovframe->linesize[2];

    avframe->width  = ovframe->width;
    avframe->height = ovframe->height;

    avframe->color_trc       = ovframe->frame_info.color_desc.transfer_characteristics;
    avframe->color_primaries = ovframe->frame_info.color_desc.colour_primaries;
    avframe->colorspace      = ovframe->frame_info.color_desc.matrix_coeffs;

    avframe->buf[0] = av_buffer_create(ovframe, sizeof(ovframe),
                                       ovvc_unref_ovframe, NULL, 0);

    avframe->pict_type = ovframe->frame_info.chroma_format == OV_YUV_420_P8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
}

static int ff_vvc_decode_extradata(const uint8_t *data, int size, OVVCDec *dec,
                                   int *is_nalff, int *nal_length_size,
                                   void *logctx) {
    int i, j, num_arrays, nal_len_size, b, has_ptl, num_sublayers;
    int ret = 0;
    GetByteContext gb;

    bytestream2_init(&gb, data, size);

    /* It seems the extradata is encoded as hvcC format.
     * Temporarily, we support configurationVersion==0 until 14496-15 3rd
     * is finalized. When finalized, configurationVersion will be 1 and we
     * can recognize hvcC by checking if avctx->extradata[0]==1 or not. */

    av_log(logctx, AV_LOG_WARNING, "Extra data support is experimental in openVVC.\n");

    *is_nalff = 1;

    b = bytestream2_get_byte(&gb);

    num_sublayers = (b >> 3) & 0x7;
    
    nal_len_size  = ((b >> 1) & 0x3) + 1;

    has_ptl = b & 0x1;

    if (has_ptl) {
        int num_bytes_constraint_info;
        int general_profile_idc;
        int general_tier_flag;
        int ptl_num_sub_profiles;
        int temp3, temp4;
        int temp2 = bytestream2_get_be16(&gb);
        int ols_idx  = (temp2 >> 7) & 0x1ff;
        int num_sublayers  = (temp2 >> 4) & 0x7;
        int constant_frame_rate = (temp2 >> 2) & 0x3;
        int chroma_format_idc = temp2 & 0x3;
        int bit_depth_minus8 = (bytestream2_get_byte(&gb) >> 5) & 0x7;
        av_log(logctx, AV_LOG_DEBUG,
            "bit_depth_minus8 %d chroma_format_idc %d\n", bit_depth_minus8, chroma_format_idc);
        // VvcPTLRecord(num_sublayers) native_ptl
        temp3 = bytestream2_get_byte(&gb);
        num_bytes_constraint_info = (temp3) & 0x3f;
        temp4 = bytestream2_get_byte(&gb);
        general_profile_idc = (temp4 >> 1) & 0x7f;
        general_tier_flag = (temp4) & 1;
        av_log(logctx, AV_LOG_DEBUG,
            "general_profile_idc %d, num_sublayers %d num_bytes_constraint_info %d\n", general_profile_idc, num_sublayers, num_bytes_constraint_info);
        for (i = 0; i < num_bytes_constraint_info; i++)
            // unsigned int(1) ptl_frame_only_constraint_flag;
            // unsigned int(1) ptl_multi_layer_enabled_flag;
            // unsigned int(8*num_bytes_constraint_info - 2) general_constraint_info;
            bytestream2_get_byte(&gb);
        /*for (i=num_sublayers - 2; i >= 0; i--)
            unsigned int(1) ptl_sublayer_level_present_flag[i];
        for (j=num_sublayers; j<=8 && num_sublayers > 1; j++)
            bit(1) ptl_reserved_zero_bit = 0;
        */
        bytestream2_get_byte(&gb);
        /*for (i=num_sublayers-2; i >= 0; i--)
            if (ptl_sublayer_level_present_flag[i])
                unsigned int(8) sublayer_level_idc[i]; */
        ptl_num_sub_profiles = bytestream2_get_byte(&gb); 
        
        for (j=0; j < ptl_num_sub_profiles; j++) {
            // unsigned int(32) general_sub_profile_idc[j];
            bytestream2_get_be16(&gb);
            bytestream2_get_be16(&gb);
        }

        int max_picture_width = bytestream2_get_be16(&gb); // unsigned_int(16) max_picture_width;
        int max_picture_height = bytestream2_get_be16(&gb); // unsigned_int(16) max_picture_height;
        int avg_frame_rate = bytestream2_get_be16(&gb); // unsigned int(16) avg_frame_rate; }
        av_log(logctx, AV_LOG_DEBUG,
            "max_picture_width %d, max_picture_height %d, avg_frame_rate %d\n", max_picture_width, max_picture_height, avg_frame_rate);
    }
    
    num_arrays  = bytestream2_get_byte(&gb);
    
    

    /* nal units in the hvcC always have length coded with 2 bytes,
     * so put a fake nal_length_size = 2 while parsing them */
    *nal_length_size = 2;

    /* Decode nal units from hvcC. */
    for (i = 0; i < num_arrays; i++) {
        int cnt;
        int type = bytestream2_get_byte(&gb) & 0x1f;

        if (type != VVC_OPI_NUT || type != VVC_DCI_NUT)
            cnt  = bytestream2_get_be16(&gb);
        else
            cnt = 1;

        av_log(logctx, AV_LOG_DEBUG, "nalu_type %d cnt %d\n", type, cnt);

        for (j = 0; j < cnt; j++) {
            // +2 for the nal size field

            int nalsize = bytestream2_peek_be16(&gb) + 2;
            av_log(logctx, AV_LOG_DEBUG, "nalsize %d \n", nalsize);


            OVPictureUnit ovpu= {0};

            if (bytestream2_get_bytes_left(&gb) < nalsize) {
                av_log(logctx, AV_LOG_ERROR,
                       "Invalid NAL unit size in extradata.\n");
                return AVERROR_INVALIDDATA;
            }

            /* FIMXE unrequired malloc */
            ovpu.nalus = av_mallocz(sizeof(OVNALUnit*));

            OVNALUnit **ovnalu_p = &ovpu.nalus[0];

            copy_rpbs_info(ovnalu_p, gb.buffer + 2, nalsize, NULL, 0);

            (*ovnalu_p)->type = type;

            ret = ovdec_submit_picture_unit(dec, &ovpu);

            unref_ovvc_nalus(&ovpu);
            av_free(ovpu.nalus);

            if (ret < 0) {
                av_log(logctx, AV_LOG_ERROR, "Decoding nal unit %d %d from hvcC failed\n",
                       type, i);
                return ret;
            }

            bytestream2_skip(&gb, nalsize);
        }
    }

    /* Now store right nal length size, that will be used to parse * all other nals */
    *nal_length_size = nal_len_size;

    return ret;
}

static int libovvc_decode_frame(AVCodecContext *c, void *outdata, int *outdata_size, AVPacket *avpkt) {
 

    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVVCDec *libovvc_dec = dec_ctx->libovvc_dec;
    OVFrame *ovframe = NULL;
    int *nb_pic_out = outdata_size;
    int ret;

    if (!avpkt->size) {

        ret = ovdec_drain_picture(libovvc_dec, &ovframe);

        if (ret < 0) {
            //return ret;
        }

        if (ovframe) {
            c->pix_fmt = ovframe->frame_info.chroma_format == OV_YUV_420_P8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
            c->width   = ovframe->width;
            c->height  = ovframe->height;
            c->coded_width   = ovframe->width;
            c->coded_height  = ovframe->height;

            convert_ovframe(outdata, ovframe);

            av_log(c, AV_LOG_TRACE, "Draining pic with POC: %d\n", ovframe->poc);

            *outdata_size = 1;
        }

        return 0;
    }

    OVPictureUnit ovpu;
    H2645Packet pkt = {0};

    *nb_pic_out = 0;

    if (avpkt->side_data_elems) {
        av_log(c, AV_LOG_WARNING, "Unsupported side data\n");
    }

    if (c->extradata_size && c->extradata) {
        struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
        uint8_t process_extrada = c->extradata != dec_ctx->last_extradata;

        if (process_extrada && c->extradata_size > 3 &&
            (c->extradata[0] || c->extradata[1] || c->extradata[2] > 1)) {

            ret = ff_vvc_decode_extradata(c->extradata, c->extradata_size, dec_ctx->libovvc_dec,
                                          &dec_ctx->is_nalff, &dec_ctx->nal_length_size, c);

            if (ret < 0) {
                av_log(c, AV_LOG_ERROR, "Error reading parameters sets as extradata.\n");
                return ret;
            }
            dec_ctx->last_extradata = c->extradata;
        }
    }

    ret = ff_h2645_packet_split(&pkt, avpkt->data, avpkt->size, c, dec_ctx->is_nalff,
                                dec_ctx->nal_length_size, AV_CODEC_ID_VVC, 0, 0);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Error splitting the input into NAL units.\n");
        return ret;
    }

    convert_avpkt(&ovpu, &pkt);
    ret = ovdec_submit_picture_unit(libovvc_dec, &ovpu);
    if (ret < 0) {
        av_free(ovpu.nalus);
        return AVERROR_INVALIDDATA;
    }

    ovdec_receive_picture(libovvc_dec, &ovframe);

    /* FIXME use ret instead of frame */
    if (ovframe) {
        c->pix_fmt = ovframe->frame_info.chroma_format == OV_YUV_420_P8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
        c->width   = ovframe->width;
        c->height  = ovframe->height;
        c->coded_width   = ovframe->width;
        c->coded_height  = ovframe->height;

        av_log(c, AV_LOG_TRACE, "Received pic with POC: %d\n", ovframe->poc);

        convert_ovframe(outdata, ovframe);

        *nb_pic_out = 1;
    }

    unref_ovvc_nalus(&ovpu);

    ff_h2645_packet_uninit(&pkt);

    av_free(ovpu.nalus);

    return 0;
}

static int ov_log_level;

static void set_libovvc_log_level(int level) {
    ov_log_level = level;
}

static void libovvc_log(void* ctx, int log_level, const char* fmt, va_list vl)
{
     static const uint8_t log_level_lut[6] = {AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO, AV_LOG_TRACE, AV_LOG_DEBUG, AV_LOG_VERBOSE};
     AVClass *avcl = &libovvc_decoder_class;
     if (log_level < ov_log_level) {
         av_vlog(&avcl, log_level_lut[log_level], fmt, vl);
     }
}

static int libovvc_decode_init(AVCodecContext *c) {
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVVCDec **libovvc_dec_p = (OVVCDec**) &dec_ctx->libovvc_dec;
    int ret;
    int nb_frame_th = dec_ctx->nb_frame_th;
    int nb_entry_th = dec_ctx->nb_entry_th;

    int display_output = 1;

    set_libovvc_log_level(dec_ctx->log_level);

    ovdec_set_log_callback(libovvc_log);

    ret = ovdec_init(libovvc_dec_p);
    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Could not init Open VVC decoder\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    ovdec_config_threads(*libovvc_dec_p, nb_entry_th, nb_frame_th);

    ret = ovdec_start(*libovvc_dec_p);

    if (ret < 0) {
        av_log(c, AV_LOG_ERROR, "Could not init Open VVC decoder\n");
        return AVERROR_DECODER_NOT_FOUND;
    }

    dec_ctx->is_nalff        = 0;
    dec_ctx->nal_length_size = 0;

    if (c->extradata && c->extradata_size) {
        struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;

        if (c->extradata_size > 3 && (c->extradata[0] || c->extradata[1] || c->extradata[2] > 1)) {
            dec_ctx->last_extradata = c->extradata;

            ret = ff_vvc_decode_extradata(c->extradata, c->extradata_size, dec_ctx->libovvc_dec,
                                          &dec_ctx->is_nalff, &dec_ctx->nal_length_size, c);

            if (ret < 0) {
                av_log(c, AV_LOG_ERROR, "Error reading parameters sets as extradata.\n");
                return ret;
            }
        }
    }
    return 0;
}

static int libovvc_decode_free(AVCodecContext *c) {
    
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;

    ovdec_close(dec_ctx->libovvc_dec);

    dec_ctx->libovvc_dec = NULL;
    return 0;
}

static void libovvc_decode_flush(AVCodecContext *c) {
    struct OVDecContext *dec_ctx = (struct OVDecContext *)c->priv_data;
    OVVCDec *libovvc_dec = dec_ctx->libovvc_dec;
    av_log(c, AV_LOG_ERROR, "FLUSH\n");

    OVFrame *ovframe = NULL;
    int ret;

    do {
        ret = ovdec_drain_picture(libovvc_dec, &ovframe);
        #if 0
        if (ret < 0) {
            return ret;
        }
        #endif

        if (ovframe) {
            av_log(c, AV_LOG_TRACE, "Flushing pic with POC: %d\n", ovframe->poc);
            ovframe_unref(&ovframe);
        }
    } while (ret > 0);

    libovvc_decode_free(c);
    #if 0
    if (ret < 0) {
        return;
    }
    #endif

    libovvc_decode_init(c);

    return;
}

static int libovvc_update_thread_context(AVCodecContext *dst, const AVCodecContext *src) {

    return 0;
}

AVCodec ff_libopenvvc_decoder = {
    .name                  = "ovvc",
    .long_name             = NULL_IF_CONFIG_SMALL("Open VVC(Versatile Video Coding)"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_VVC,
    .priv_data_size        = sizeof(struct OVDecContext),
    .priv_class            = &libovvc_decoder_class,
    .init                  = libovvc_decode_init,
    .close                 = libovvc_decode_free,
    .decode                = libovvc_decode_frame,
    .flush                 = libovvc_decode_flush,
    .capabilities          = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
    .wrapper_name          = "OpenVVC",
#if 0
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_EXPORTS_CROPPING,
#endif
    .profiles              = NULL_IF_CONFIG_SMALL(ff_vvc_profiles),
};