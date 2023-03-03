/*
 * H.266 decoding using the VVdeC library
 *
 * Copyright (c) 2018-2022, Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
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

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "avcodec.h"
#include "internal.h"

#include "libavutil/avutil.h"
#include "libavutil/pixdesc.h"
#include "libavutil/opt.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"

#include <vvdec/vvdec.h>

#define VVDEC_LOG_ERROR( ...) \
    { \
        av_log(avctx, AV_LOG_ERROR, __VA_ARGS__); \
        return AVERROR(EINVAL); \
    }

#define VVDEC_LOG_WARNING( ...) \
    { \
        av_log(avctx, AV_LOG_WARNING, __VA_ARGS__); \
    }

#define VVDEC_LOG_INFO( ...) \
    { \
        av_log(avctx, AV_LOG_INFO, __VA_ARGS__); \
    }

#define VVDEC_LOG_VERBOSE( ...) \
    { \
        av_log(avctx, AV_LOG_VERBOSE, __VA_ARGS__); \
    }

#define VVDEC_LOG_DBG( ...) \
    { \
        av_log(avctx, AV_LOG_DEBUG, __VA_ARGS__); \
    }

typedef struct VVdeCOptions {
    int upscaling_mode;
} VVdeCOptions;

typedef struct VVdeCContext {
   AVClass         *av_class;
   VVdeCOptions     options;      // decoding options
   vvdecDecoder*    vvdecDec;
   vvdecParams      vvdecParams;
   bool             bFlush;
}VVdeCContext;


static av_cold void ff_vvdec_log_callback(void *avctx, int level, const char *fmt, va_list args )
{
  vfprintf( level == 1 ? stderr : stdout, fmt, args );
}


static av_cold void ff_vvdec_printParameterInfo( AVCodecContext *avctx, vvdecParams* params )
{
  // print some encoder info
  VVDEC_LOG_DBG( "Version info: vvdec %s\n",vvdec_get_version() );
  VVDEC_LOG_DBG( "threads: %d\n",params->threads );
}

static av_cold int ff_vvdec_set_pix_fmt(AVCodecContext *avctx, vvdecFrame* frame )
{
    if( NULL != frame->picAttributes && NULL != frame->picAttributes->vui &&
        frame->picAttributes->vui->colourDescriptionPresentFlag )
    {
      avctx->color_trc       = frame->picAttributes->vui->transferCharacteristics;
      avctx->color_primaries = frame->picAttributes->vui->colourPrimaries;
      avctx->colorspace      = frame->picAttributes->vui->matrixCoefficients;
    }
    else
    {
      avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
      avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
      avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
    }

    if( NULL != frame->picAttributes && NULL != frame->picAttributes->vui &&
        frame->picAttributes->vui->videoSignalTypePresentFlag)
    {
      avctx->color_range =
          frame->picAttributes->vui->videoFullRangeFlag ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }
    else
    {
      avctx->color_range = AVCOL_RANGE_MPEG;
    }

    switch ( frame->colorFormat )
    {
    case VVDEC_CF_YUV420_PLANAR:
        if (frame->bitDepth == 8)
        {
            avctx->pix_fmt = frame->numPlanes == 1 ?
                             AV_PIX_FMT_GRAY8 : AV_PIX_FMT_YUV420P;
            avctx->profile = FF_PROFILE_VVC_MAIN_10;
            return 0;
        }
        else if (frame->bitDepth == 10)
        {
            avctx->pix_fmt = frame->numPlanes == 1 ?
                             AV_PIX_FMT_GRAY10 : AV_PIX_FMT_YUV420P10;
            avctx->profile = FF_PROFILE_VVC_MAIN_10;
            return 0;
        }
//        else if (frame->bitDepth == 12)
//        {
//            avctx->pix_fmt = frame->numPlanes == 1 ?
//                             AV_PIX_FMT_GRAY12 : AV_PIX_FMT_YUV420P12;
//            avctx->profile = FF_PROFILE_VVC_MAIN_10;
//            return 0;
//        }
        else
        {
            return AVERROR_INVALIDDATA;
        }
//    case AOM_IMG_FMT_I422:
//        if (frame->bitDepth == 8) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else if (frame->bitDepth == 10) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P10;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else if (frame->bitDepth == 12) {
//            avctx->pix_fmt = AV_PIX_FMT_YUV422P12;
//            avctx->profile = FF_PROFILE_AV1_PROFESSIONAL;
//            return 0;
//        } else {
//            return AVERROR_INVALIDDATA;
//        }
    default:
        return AVERROR_INVALIDDATA;
    }
}

/*
 * implementation of the interface functions
 */
static av_cold int ff_vvdec_decode_init(AVCodecContext *avctx)
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  VVDEC_LOG_DBG("ff_vvdec_decode_init() threads %d\n", avctx->thread_count );

  vvdec_params_default( &s->vvdecParams );
  s->vvdecParams.logLevel = VVDEC_DETAILS;

  if     ( av_log_get_level() >= AV_LOG_DEBUG )   s->vvdecParams.logLevel = VVDEC_DETAILS;
  else if( av_log_get_level() >= AV_LOG_VERBOSE ) s->vvdecParams.logLevel = VVDEC_INFO;     // VVDEC_INFO will output per picture info
  else if( av_log_get_level() >= AV_LOG_INFO )    s->vvdecParams.logLevel = VVDEC_WARNING;  // AV_LOG_INFO is ffmpeg default
  else s->vvdecParams.logLevel = VVDEC_SILENT;

  // set desired decoding options

  // threading
  if( avctx->thread_count > 0 )
  {
    s->vvdecParams.threads = avctx->thread_count;  // number of worker threads (should not exceed the number of physical cpu's)
  }
  else
  {
    s->vvdecParams.threads = -1; // get max cpus
  }

  ff_vvdec_printParameterInfo( avctx, &s->vvdecParams );
  s->vvdecDec = vvdec_decoder_open( &s->vvdecParams );
  if( !s->vvdecDec )
  {
    av_log(avctx, AV_LOG_ERROR, "cannot init vvc decoder\n" );
    return -1;
  }

  vvdec_set_logging_callback( s->vvdecDec, ff_vvdec_log_callback );
  s->bFlush = false;

  return 0;
}

static av_cold int ff_vvdec_decode_close(AVCodecContext *avctx)
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  if( 0 != vvdec_decoder_close(s->vvdecDec) )
  {
    av_log(avctx, AV_LOG_ERROR, "cannot close vvdec\n" );
    return -1;
  }

  s->bFlush = false;

  return 0;
}


static av_cold int ff_vvdec_decode_frame( AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt )
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  AVFrame *pcAVFrame      = (AVFrame*)data;

  int iRet = 0;
  vvdecFrame *frame = NULL;

  if ( pcAVFrame )
  {
    if( !avpkt->size && !s->bFlush )
    {
      s->bFlush = true;
    }

    if( s->bFlush )
    {
      iRet = vvdec_flush( s->vvdecDec, &frame );
    }
    else
    {
      vvdecAccessUnit accessUnit;
      vvdec_accessUnit_default( &accessUnit );
      accessUnit.payload         = avpkt->data;
      accessUnit.payloadSize     = avpkt->size;
      accessUnit.payloadUsedSize = avpkt->size;

      accessUnit.cts = avpkt->pts; accessUnit.ctsValid = true;
      accessUnit.dts = avpkt->pts; accessUnit.dtsValid = true;

      iRet = vvdec_decode( s->vvdecDec, &accessUnit, &frame );
    }

    if( iRet < 0 )
    {
      if( iRet == VVDEC_TRY_AGAIN )
      {
        VVDEC_LOG_DBG( "vvdec::decode - more input data needed\n" );
      }
      else if( iRet == VVDEC_EOF )
      {
        s->bFlush = true;
        VVDEC_LOG_DBG( "vvdec::decode - eof reached\n" );
      }
      else
      {
        VVDEC_LOG_ERROR( "error in vvdec::decode - ret:%d - %s\n", iRet, vvdec_get_last_error(s->vvdecDec) );
        return -1;
      }
    }
    else
    {
      if( NULL != frame )
      {
        const uint8_t * src_data[4]      = { frame->planes[0].ptr, frame->planes[1].ptr, frame->planes[2].ptr, NULL };
        const int       src_linesizes[4] = { (int)frame->planes[0].stride, (int)frame->planes[1].stride, (int)frame->planes[2].stride, 0 };

        #if 1
        if( frame->picAttributes )
        {
          const static char acST[3] = { 'I', 'P', 'B' };
          char c = acST[frame->picAttributes->sliceType];
          if( !frame->picAttributes->isRefPic ) c += 32;

          VVDEC_LOG_DBG( "vvdec_decode_frame SEQ %" PRId64 " TId: %d  %c-SLICE flush %d\n", frame->sequenceNumber,
              frame->picAttributes->temporalLayer, c, s->bFlush );
        }
        else
        {
          VVDEC_LOG_DBG( "vvdec_decode_frame SEQ %" PRId64 "\n", frame->sequenceNumber );
        }
        #endif

        if (( iRet = ff_vvdec_set_pix_fmt(avctx, frame)) < 0)
        {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d) / bit_depth (%d)\n",
                frame->colorFormat, frame->bitDepth);
            return iRet;
        }

        if( avctx->pix_fmt != AV_PIX_FMT_YUV420P && avctx->pix_fmt != AV_PIX_FMT_YUV420P10LE )
        {
          av_log(avctx, AV_LOG_ERROR, "Unsupported output colorspace (%d) / bit_depth (%d)\n",
              frame->colorFormat, frame->bitDepth );
          return iRet;
        }

        if ((int)frame->width != avctx->width || (int)frame->height != avctx->height)
        {
            av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                   avctx->width, avctx->height, frame->width, frame->height);

            iRet = ff_set_dimensions(avctx, frame->width, frame->height);
            if (iRet < 0)
                return iRet;
        }

        // The decoder doesn't support decoding into a user provided buffer yet, so do a copy
        if (ff_get_buffer(avctx, pcAVFrame, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Could not allocate the video frame data\n");
            return AVERROR(ENOMEM);
        }

        av_image_copy(pcAVFrame->data, pcAVFrame->linesize, src_data,
                      src_linesizes, avctx->pix_fmt, frame->width, frame->height );
        
        pcAVFrame->pts     = frame->ctsValid ? frame->cts : AV_NOPTS_VALUE;
        pcAVFrame->pkt_dts = AV_NOPTS_VALUE;

        if( 0 != vvdec_frame_unref( s->vvdecDec, frame ) )
        {
          av_log(avctx, AV_LOG_ERROR, "cannot free picture memory\n");
        }

        *got_frame = 1;
      }
    }
  }

  return avpkt->size;
}

static av_cold void ff_vvdec_decode_flush(AVCodecContext *avctx)
{
  VVdeCContext *s = (VVdeCContext*)avctx->priv_data;

  if( 0 != vvdec_decoder_close(s->vvdecDec) )
  {
    av_log(avctx, AV_LOG_ERROR, "cannot close vvdec during flush\n" );
  }

  s->vvdecDec = vvdec_decoder_open( &s->vvdecParams );
  if( !s->vvdecDec )
  {
    av_log(avctx, AV_LOG_ERROR, "cannot reinit vvdec during flush\n" );
  }

  vvdec_set_logging_callback( s->vvdecDec, ff_vvdec_log_callback );

  s->bFlush = false;
}

static const enum AVPixelFormat pix_fmts_vvc[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV420P10LE,
    AV_PIX_FMT_NONE
};

#define OFFSET(x) offsetof(VVdeCContext, x)
#define VVDEC_FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption libvvdec_options[] = {
    {"upscaling", "RPR upscaling mode", OFFSET(options.upscaling_mode), AV_OPT_TYPE_INT, {.i64 = 0}, -1, 1, VVDEC_FLAGS, "upscaling_mode"},
        {"auto",     "Selected by the Decoder", 0, AV_OPT_TYPE_CONST, {.i64 = -1 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
        {"off",   "Disable", 0, AV_OPT_TYPE_CONST, {.i64 =  0 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
        {"on", "on", 0, AV_OPT_TYPE_CONST, {.i64 =  1 }, INT_MIN, INT_MAX, VVDEC_FLAGS, "upscaling_mode"},
    {NULL}
};

static const AVClass libvvdec_class = {
    "libvvdec-vvc decoder",
    av_default_item_name,
    libvvdec_options,
    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libvvdec_decoder = {
  .name            = "libvvdec",
  .long_name       = "H.266 / VVC Decoder VVdeC",
  .type            = AVMEDIA_TYPE_VIDEO,
  .id              = AV_CODEC_ID_VVC,
  .priv_data_size  = sizeof(VVdeCContext),
  .init            = ff_vvdec_decode_init,
  .decode          = ff_vvdec_decode_frame,
  .close           = ff_vvdec_decode_close,
  .flush           = ff_vvdec_decode_flush,
  .capabilities    = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
  .bsfs            = "vvc_mp4toannexb",
  .caps_internal   = FF_CODEC_CAP_AUTO_THREADS,
  .pix_fmts        = pix_fmts_vvc,
  .priv_class      = &libvvdec_class,
  .wrapper_name    = "libvvdec",
};
