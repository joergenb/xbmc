/*
 *      Copyright (C) 2005-2011 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "NalParser.h"
#include <memory.h>

class cBitstream
{
private:
  uint8_t *m_data;
  int m_offset;
  int m_len;

public:
  cBitstream(uint8_t *data, int bits);

  void setBitstream(uint8_t *data, int bits);
  void skipBits(int num);
  unsigned int readBits(int num);
  unsigned int showBits(int num);
  unsigned int readBits1() { return readBits(1); }
  unsigned int readGolombUE();
  signed int readGolombSE();
  unsigned int remainingBits();
  void putBits(int val, int num);
  int length() { return m_len; }
};

cBitstream::cBitstream(uint8_t *data, int bits)
{
  m_data = data;
  m_offset = 0;
  m_len = bits;
}

void cBitstream::setBitstream(uint8_t *data, int bits)
{
  m_data = data;
  m_offset = 0;
  m_len = bits;
}

void cBitstream::skipBits(int num)
{
  m_offset += num;
}

unsigned int cBitstream::readBits(int num)
{
  int r = 0;

  while(num > 0)
  {
    if(m_offset >= m_len)
      return 0;

    num--;

    if(m_data[m_offset / 8] & (1 << (7 - (m_offset & 7))))
      r |= 1 << num;

    m_offset++;
  }
  return r;
}

unsigned int cBitstream::showBits(int num)
{
  int r = 0;
  int offs = m_offset;

  while(num > 0)
  {
    if(offs >= m_len)
      return 0;

    num--;

    if(m_data[offs / 8] & (1 << (7 - (offs & 7))))
      r |= 1 << num;

    offs++;
  }
  return r;
}

unsigned int cBitstream::readGolombUE()
{
  int lzb = -1;

  for(int b = 0; !b; lzb++)
    b = readBits1();

  return (1 << lzb) - 1 + readBits(lzb);
}

signed int cBitstream::readGolombSE()
{
  int v, neg;
  v = readGolombUE();
  if(v == 0)
    return 0;

  neg = v & 1;
  v = (v + 1) >> 1;
  return neg ? -v : v;
}


unsigned int cBitstream::remainingBits()
{
  return m_len - m_offset;
}


void cBitstream::putBits(int val, int num)
{
  while(num > 0) {
    if(m_offset >= m_len)
      return;

    num--;

    if(val & (1 << num))
      m_data[m_offset / 8] |= 1 << (7 - (m_offset & 7));
    else
      m_data[m_offset / 8] &= ~(1 << (7 - (m_offset & 7)));

    m_offset++;
  }
}

static const int h264_lev2cpbsize[][2] =
{
  {10, 175},
  {11, 500},
  {12, 1000},
  {13, 2000},
  {20, 2000},
  {21, 4000},
  {22, 4000},
  {30, 10000},
  {31, 14000},
  {32, 20000},
  {40, 25000},
  {41, 62500},
  {42, 62500},
  {50, 135000},
  {51, 240000},
  {-1, -1},
};

bool CParserH264::FindSPS(uint8_t *buf, uint8_t **sps, int &len)
{
  int i;
  uint32_t state = -1;

  for(i=0; i<=len; i++)
  {
    if((state&0xFFFFFF1F) == 0x107)
    {
      *sps = buf + i;
      len -= i;
      return true;
    }
    if (i<len)
    {
      state = (state<<8);
      state = state | buf[i];
    }
  }
  return false;
}

bool CParserH264::ParseSPS(uint8_t *buf, int len)
{
  cBitstream bs(buf, len*8);
  unsigned int tmp, frame_mbs_only;
  int cbpsize = -1;

  int profile_idc = bs.readBits(8);
  /* constraint_set0_flag = bs.readBits1(); */
  /* constraint_set1_flag = bs.readBits1(); */
  /* constraint_set2_flag = bs.readBits1(); */
  /* constraint_set3_flag = bs.readBits1(); */
  /* reserved = bs.readBits(4); */
  bs.skipBits(8);
  int level_idc = bs.readBits(8);
  unsigned int seq_parameter_set_id = bs.readGolombUE();

  unsigned int i = 0;
  while (h264_lev2cpbsize[i][0] != -1)
  {
    if (h264_lev2cpbsize[i][0] >= level_idc)
    {
      cbpsize = h264_lev2cpbsize[i][1];
      break;
    }
    i++;
  }
  if (cbpsize < 0)
    return false;

//  m_streamData.sps[seq_parameter_set_id].cbpsize = cbpsize * 125; /* Convert from kbit to bytes */

  if (profile_idc >= 100) /* high profile */
  {
    if(bs.readGolombUE() == 3) /* chroma_format_idc */
      bs.skipBits(1); /* residual_colour_transform_flag */
    bs.readGolombUE(); /* bit_depth_luma - 8 */
    bs.readGolombUE(); /* bit_depth_chroma - 8 */
    bs.skipBits(1); /* transform_bypass */
    if (bs.readBits1()) /* seq_scaling_matrix_present */
    {
      for (int i = 0; i < 8; i++)
      {
        if (bs.readBits1()) /* seq_scaling_list_present */
        {
          int last = 8, next = 8, size = (i<6) ? 16 : 64;
          for (int j = 0; j < size; j++)
          {
            if (next)
              next = (last + bs.readGolombSE()) & 0xff;
            last = next ?: last;
          }
        }
      }
    }
  }

  bs.readGolombUE(); /* log2_max_frame_num - 4 */
  int pic_order_cnt_type = bs.readGolombUE();
  if (pic_order_cnt_type == 0)
    bs.readGolombUE(); /* log2_max_poc_lsb - 4 */
  else if (pic_order_cnt_type == 1)
  {
    bs.skipBits(1); /* delta_pic_order_always_zero */
    bs.readGolombSE(); /* offset_for_non_ref_pic */
    bs.readGolombSE(); /* offset_for_top_to_bottom_field */
    tmp = bs.readGolombUE(); /* num_ref_frames_in_pic_order_cnt_cycle */
    for (unsigned int i = 0; i < tmp; i++)
      bs.readGolombSE(); /* offset_for_ref_frame[i] */
  }
  else if(pic_order_cnt_type != 2)
  {
    /* Illegal poc */
    return false;
  }

  bs.readGolombUE(); /* ref_frames */
  bs.skipBits(1); /* gaps_in_frame_num_allowed */
  m_width = bs.readGolombUE() + 1;
  m_height = bs.readGolombUE() + 1;
  frame_mbs_only = bs.readBits1();

  m_width *= 16;
  m_height *= 16 * (2-frame_mbs_only);

  if (!frame_mbs_only)
  {
    if (bs.readBits1()) /* mb_adaptive_frame_field_flag */
      ;
  }
  bs.skipBits(1); /* direct_8x8_inference_flag */
  crop_left = crop_right = crop_top = crop_bottom = 0;
  if (bs.readBits1()) /* frame_cropping_flag */
  {
    crop_left = bs.readGolombUE();
    crop_right = bs.readGolombUE();
    crop_top = bs.readGolombUE();
    crop_bottom = bs.readGolombUE();

    crop_left *= 2;
    crop_right *= 2;

    if (frame_mbs_only)
    {
      crop_top *= 2;
      crop_bottom *= 2;
    }
    else
    {
      crop_top *= 4;
      crop_bottom *= 4;
    }
  }

  /* VUI parameters */
  mpeg_rational_t PixelAspect;
  PixelAspect.num = 0;
  if (bs.readBits1()) /* vui_parameters_present flag */
  {
    if (bs.readBits1()) /* aspect_ratio_info_present */
    {
      uint32_t aspect_ratio_idc = bs.readBits(8);

      if (aspect_ratio_idc == 255 /* Extended_SAR */)
      {
        PixelAspect.num = bs.readBits(16); /* sar_width */
        PixelAspect.den = bs.readBits(16); /* sar_height */
      }
      else
      {
        static const mpeg_rational_t aspect_ratios[] =
        { /* page 213: */
          /* 0: unknown */
          {0, 1},
          /* 1...16: */
          { 1, 1}, {12, 11}, {10, 11}, {16, 11}, { 40, 33}, {24, 11}, {20, 11}, {32, 11},
          {80, 33}, {18, 11}, {15, 11}, {64, 33}, {160, 99}, { 4, 3}, { 3, 2}, { 2, 1}
        };

        if (aspect_ratio_idc < sizeof(aspect_ratios)/sizeof(aspect_ratios[0]))
        {
          memcpy(&PixelAspect, &aspect_ratios[aspect_ratio_idc], sizeof(mpeg_rational_t));
        }
        else
        {
//          DEBUGLOG("H.264 SPS: aspect_ratio_idc out of range !");
        }
      }
    }
  }

  return true;
}

