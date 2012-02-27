/*
 * H.264 HW decode acceleration through XVBA
 *
 * Copyright (C) 2005-2011 Team XBMC
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

#include "xvba.h"
#include "xvba_internal.h"
#include "h264.h"
#include <assert.h>

/** @file
 *  This file implements the glue code between FFmpeg's and XvBA API's
 *  structures for H.264 decoding.
 */


/** Initialize and start decoding a frame with XVBA. */
static int start_frame(AVCodecContext          *avctx,
                       av_unused const uint8_t *buffer,
                       av_unused uint32_t       size)
{
  H264Context * const h = avctx->priv_data;
  MpegEncContext * const s = &h->s;
  struct xvba_context *hwaccel_context;
  XVBAPictureDescriptor *pic_descriptor;
  int i;

  hwaccel_context = (struct xvba_context *)avctx->hwaccel_context;
  assert(hwaccel_context);
  pic_descriptor = hwaccel_context->picture_descriptor_buffer->bufferXVBA;

  for (i = 0; i < 2; ++i) {
      int foc = s->current_picture_ptr->field_poc[i];
      if (foc == INT_MAX)
          foc = 0;
      pic_descriptor->avc_curr_field_order_cnt_list[i] = foc;
  }

  pic_descriptor->avc_frame_num = h->frame_num;

  hwaccel_context->num_slices = 0;
  hwaccel_context->data_buffer->data_size_in_buffer = 0;

  return 0;
}

/** End a hardware decoding based frame. */
static int end_frame(AVCodecContext *avctx)
{
  H264Context * const h = avctx->priv_data;
  MpegEncContext * const s = &h->s;
  struct xvba_context *hwaccel_context;
  XVBAPictureDescriptor *pic_descriptor;
  XVBAQuantMatrixAvc *iq_matrix;

  hwaccel_context = (struct xvba_context *)avctx->hwaccel_context;
  assert(hwaccel_context);

  pic_descriptor = hwaccel_context->picture_descriptor_buffer->bufferXVBA;
  iq_matrix = hwaccel_context->iq_matrix_buffer->bufferXVBA;

  av_dlog(avctx, "end_frame()\n");

  /* Fill in Picture Parameters*/
  pic_descriptor->profile                                     = ff_xvba_translate_profile(avctx->profile);
  pic_descriptor->level                                       = avctx->level;
  pic_descriptor->width_in_mb                                 = s->mb_width;
  pic_descriptor->height_in_mb                                = s->mb_height;
  pic_descriptor->picture_structure                           = s->picture_structure;
  pic_descriptor->chroma_format                               = s->chroma_format ? s->chroma_format : 1;
  pic_descriptor->avc_intra_flag                              = (h->slice_type == FF_I_TYPE) ? 1 : 0;
  pic_descriptor->avc_reference                               = (s->current_picture_ptr->reference & 3) ? 1 : 0;

  pic_descriptor->avc_bit_depth_luma_minus8                   = h->sps.bit_depth_luma - 8;
  pic_descriptor->avc_bit_depth_chroma_minus8                 = h->sps.bit_depth_chroma - 8;
  pic_descriptor->avc_log2_max_frame_num_minus4               = h->sps.log2_max_frame_num -4;
  pic_descriptor->avc_pic_order_cnt_type                      = h->sps.poc_type;
  pic_descriptor->avc_log2_max_pic_order_cnt_lsb_minus4       = h->sps.log2_max_poc_lsb - 4;
  pic_descriptor->avc_num_ref_frames                          = h->sps.ref_frame_count;
  pic_descriptor->avc_reserved_8bit                           = 0;

  pic_descriptor->avc_num_slice_groups_minus1                 = h->pps.slice_group_count - 1;
  pic_descriptor->avc_num_ref_idx_l0_active_minus1            = h->pps.ref_count[0] - 1;
  pic_descriptor->avc_num_ref_idx_l1_active_minus1            = h->pps.ref_count[1] - 1;

  pic_descriptor->avc_pic_init_qp_minus26                     = h->pps.init_qp - 26;
  pic_descriptor->avc_pic_init_qs_minus26                     = h->pps.init_qs - 26;
  pic_descriptor->avc_chroma_qp_index_offset                  = h->pps.chroma_qp_index_offset[0];
  pic_descriptor->avc_second_chroma_qp_index_offset           = h->pps.chroma_qp_index_offset[1];
  pic_descriptor->avc_slice_group_change_rate_minus1          = 0; // not implemented in ffmpeg
  pic_descriptor->avc_reserved_16bit                          = 0; // must be 0
  memset(pic_descriptor->avc_field_order_cnt_list,0,sizeof(pic_descriptor->avc_field_order_cnt_list)); // must be 0
  memset(pic_descriptor->avc_slice_group_map,0,sizeof(pic_descriptor->avc_slice_group_map)); // must be 0

  // sps
  pic_descriptor->sps_info.avc.delta_pic_always_zero_flag     = h->sps.delta_pic_order_always_zero_flag;
  pic_descriptor->sps_info.avc.direct_8x8_inference_flag      = h->sps.direct_8x8_inference_flag;
  pic_descriptor->sps_info.avc.frame_mbs_only_flag            = h->sps.frame_mbs_only_flag;
  pic_descriptor->sps_info.avc.gaps_in_frame_num_value_allowed_flag = h->sps.gaps_in_frame_num_allowed_flag;
  pic_descriptor->sps_info.avc.mb_adaptive_frame_field_flag   = h->sps.mb_aff;
  pic_descriptor->sps_info.avc.residual_colour_transform_flag = h->sps.residual_color_transform_flag;
  pic_descriptor->sps_info.avc.xvba_avc_sps_reserved          = 0;

  // pps
  pic_descriptor->pps_info.avc.entropy_coding_mode_flag       = h->pps.cabac;
  pic_descriptor->pps_info.avc.pic_order_present_flag         = h->pps.pic_order_present;
  pic_descriptor->pps_info.avc.weighted_pred_flag             = h->pps.weighted_pred;
  pic_descriptor->pps_info.avc.weighted_bipred_idc            = h->pps.weighted_bipred_idc;
  pic_descriptor->pps_info.avc.deblocking_filter_control_present_flag = h->pps.deblocking_filter_parameters_present;
  pic_descriptor->pps_info.avc.constrained_intra_pred_flag    = h->pps.constrained_intra_pred;
  pic_descriptor->pps_info.avc.redundant_pic_cnt_present_flag = h->pps.redundant_pic_cnt_present;
  pic_descriptor->pps_info.avc.transform_8x8_mode_flag        = h->pps.transform_8x8_mode;
  pic_descriptor->pps_info.avc.xvba_avc_pps_reserved          = 0; // must be 0

  memcpy(iq_matrix->bScalingLists4x4, h->pps.scaling_matrix4, sizeof(iq_matrix->bScalingLists4x4));
  memcpy(iq_matrix->bScalingLists8x8, h->pps.scaling_matrix8, sizeof(iq_matrix->bScalingLists8x8));

  ff_draw_horiz_band(s, 0, s->avctx->height);

  return 0;
}

/** Decode the given H.264 slice with VA API. */
static int decode_slice(AVCodecContext *avctx,
                        const uint8_t  *buffer,
                        uint32_t        size)
{
  struct xvba_context *hwaccel_context;

  hwaccel_context = (struct xvba_context *)avctx->hwaccel_context;
  assert(hwaccel_context);

  ff_xvba_add_slice_data(hwaccel_context, buffer, size);
  return 0;
}

AVHWAccel ff_h264_xvba_hwaccel = {
    .name           = "h264_xvba",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = CODEC_ID_H264,
    .pix_fmt        = PIX_FMT_XVBA_VLD,
    .capabilities   = 0,
    .start_frame    = start_frame,
    .end_frame      = end_frame,
    .decode_slice   = decode_slice,
    .priv_data_size = 0,
};
