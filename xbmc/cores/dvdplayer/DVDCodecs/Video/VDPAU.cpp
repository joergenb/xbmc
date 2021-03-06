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

#include "system.h"
#ifdef HAVE_LIBVDPAU
#include <dlfcn.h>
#include "windowing/WindowingFactory.h"
#include "VDPAU.h"
#include "guilib/TextureManager.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "DVDVideoCodecFFmpeg.h"
#include "DVDClock.h"
#include "settings/Settings.h"
#include "settings/GUISettings.h"
#include "settings/AdvancedSettings.h"
#include "Application.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "DVDCodecs/DVDCodecUtils.h"
#include "cores/VideoRenderers/RenderFlags.h"

#define ARSIZE(x) (sizeof(x) / sizeof((x)[0]))

CVDPAU::Desc decoder_profiles[] = {
{"MPEG1",        VDP_DECODER_PROFILE_MPEG1},
{"MPEG2_SIMPLE", VDP_DECODER_PROFILE_MPEG2_SIMPLE},
{"MPEG2_MAIN",   VDP_DECODER_PROFILE_MPEG2_MAIN},
{"H264_BASELINE",VDP_DECODER_PROFILE_H264_BASELINE},
{"H264_MAIN",    VDP_DECODER_PROFILE_H264_MAIN},
{"H264_HIGH",    VDP_DECODER_PROFILE_H264_HIGH},
{"VC1_SIMPLE",   VDP_DECODER_PROFILE_VC1_SIMPLE},
{"VC1_MAIN",     VDP_DECODER_PROFILE_VC1_MAIN},
{"VC1_ADVANCED", VDP_DECODER_PROFILE_VC1_ADVANCED},
#ifdef VDP_DECODER_PROFILE_MPEG4_PART2_ASP
{"MPEG4_PART2_ASP", VDP_DECODER_PROFILE_MPEG4_PART2_ASP},
#endif
};
const size_t decoder_profile_count = sizeof(decoder_profiles)/sizeof(CVDPAU::Desc);

//static float studioCSC[3][4] =
//{
//    { 1.0f,        0.0f, 1.57480000f,-0.78740000f},
//    { 1.0f,-0.18737736f,-0.46813736f, 0.32775736f},
//    { 1.0f, 1.85556000f,        0.0f,-0.92780000f}
//};
static float studioCSCKCoeffs601[3] = {0.299, 0.587, 0.114};  //BT601 {Kr, Kg, Kb}
static float studioCSCKCoeffs709[3] = {0.2126, 0.7152, 0.0722};  //BT709 {Kr, Kg, Kb}

static struct SInterlaceMapping
{
  const EINTERLACEMETHOD     method;
  const VdpVideoMixerFeature feature;
} g_interlace_mapping[] = 
{ {VS_INTERLACEMETHOD_VDPAU_TEMPORAL             , VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL}
, {VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF        , VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL}
, {VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL     , VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL}
, {VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF, VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL}
, {VS_INTERLACEMETHOD_VDPAU_INVERSE_TELECINE     , VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE}
, {VS_INTERLACEMETHOD_NONE                       , (VdpVideoMixerFeature)-1}
};

//since libvdpau 0.4, vdp_device_create_x11() installs a callback on the Display*,
//if we unload libvdpau with dlclose(), we segfault on XCloseDisplay,
//so we just keep a static handle to libvdpau around
void* CVDPAU::dl_handle;

CVDPAU::CVDPAU() : CThread("CVDPAU")
{
  glXBindTexImageEXT = NULL;
  glXReleaseTexImageEXT = NULL;
#ifdef GL_NV_vdpau_interop
  glVDPAUInitNV = NULL;
  glVDPAUFiniNV = NULL;
  glVDPAURegisterOutputSurfaceNV = NULL;
  glVDPAURegisterVideoSurfaceNV = NULL;
  glVDPAUIsSurfaceNV = NULL;
  glVDPAUUnregisterSurfaceNV = NULL;
  glVDPAUSurfaceAccessNV = NULL;
  glVDPAUMapSurfacesNV = NULL;
  glVDPAUUnmapSurfacesNV = NULL;
  glVDPAUGetSurfaceivNV = NULL;
#endif

  vdp_device = VDP_INVALID_HANDLE;

  picAge.b_age    = picAge.ip_age[0] = picAge.ip_age[1] = 256*256*256*64;
  vdpauConfigured = false;
  m_DisplayState = VDPAU_OPEN;
  m_mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME;
  m_mixerstep  = 0;
  m_outPicsNum = std::min(NUM_OUTPUT_SURFACES, NUM_OUTPUT_PICS);

  for (int i=0;i<NUM_OUTPUT_PICS;i++)
  {
    m_allOutPic[i].pixmap = 0;
    m_allOutPic[i].glPixmap = 0;
  }

  if (!glXBindTexImageEXT)
    glXBindTexImageEXT    = (PFNGLXBINDTEXIMAGEEXTPROC)glXGetProcAddress((GLubyte *) "glXBindTexImageEXT");
  if (!glXReleaseTexImageEXT)
    glXReleaseTexImageEXT = (PFNGLXRELEASETEXIMAGEEXTPROC)glXGetProcAddress((GLubyte *) "glXReleaseTexImageEXT");

  m_mixerInputSize = 0;
  m_preBindPixmapsDone = false;
  m_GlInteropStatus = OUTPUT_NONE;
  m_renderThread = NULL;

#ifdef GL_NV_vdpau_interop
  if (glewIsSupported("GL_NV_vdpau_interop"))
  {
    if (!glVDPAUInitNV)
      glVDPAUInitNV    = (PFNGLVDPAUINITNVPROC)glXGetProcAddress((GLubyte *) "glVDPAUInitNV");
    if (!glVDPAUFiniNV)
      glVDPAUFiniNV = (PFNGLVDPAUFININVPROC)glXGetProcAddress((GLubyte *) "glVDPAUFiniNV");
    if (!glVDPAURegisterOutputSurfaceNV)
      glVDPAURegisterOutputSurfaceNV    = (PFNGLVDPAUREGISTEROUTPUTSURFACENVPROC)glXGetProcAddress((GLubyte *) "glVDPAURegisterOutputSurfaceNV");
    if (!glVDPAURegisterVideoSurfaceNV)
      glVDPAURegisterVideoSurfaceNV    = (PFNGLVDPAUREGISTERVIDEOSURFACENVPROC)glXGetProcAddress((GLubyte *) "glVDPAURegisterVideoSurfaceNV");
    if (!glVDPAUIsSurfaceNV)
      glVDPAUIsSurfaceNV    = (PFNGLVDPAUISSURFACENVPROC)glXGetProcAddress((GLubyte *) "glVDPAUIsSurfaceNV");
    if (!glVDPAUUnregisterSurfaceNV)
      glVDPAUUnregisterSurfaceNV = (PFNGLVDPAUUNREGISTERSURFACENVPROC)glXGetProcAddress((GLubyte *) "glVDPAUUnregisterSurfaceNV");
    if (!glVDPAUSurfaceAccessNV)
      glVDPAUSurfaceAccessNV    = (PFNGLVDPAUSURFACEACCESSNVPROC)glXGetProcAddress((GLubyte *) "glVDPAUSurfaceAccessNV");
    if (!glVDPAUMapSurfacesNV)
      glVDPAUMapSurfacesNV = (PFNGLVDPAUMAPSURFACESNVPROC)glXGetProcAddress((GLubyte *) "glVDPAUMapSurfacesNV");
    if (!glVDPAUUnmapSurfacesNV)
      glVDPAUUnmapSurfacesNV = (PFNGLVDPAUUNMAPSURFACESNVPROC)glXGetProcAddress((GLubyte *) "glVDPAUUnmapSurfacesNV");
    if (!glVDPAUGetSurfaceivNV)
      glVDPAUGetSurfaceivNV = (PFNGLVDPAUGETSURFACEIVNVPROC)glXGetProcAddress((GLubyte *) "glVDPAUGetSurfaceivNV");

    CLog::Log(LOGNOTICE, "CVDPAU::CVDPAU GL interop supported");
  }
  else
#endif
  {
    g_guiSettings.SetBool("videoplayer.usevdpauinteroprgb",false);
    g_guiSettings.SetBool("videoplayer.usevdpauinteropyuv",false);
  }

  totalAvailableOutputSurfaces = 0;
//  presentSurface = VDP_INVALID_HANDLE;
  vid_width = vid_height = OutWidth = OutHeight = 0;
  memset(&outRectVid, 0, sizeof(VdpRect));

  tmpBrightness  = 0;
  tmpContrast    = 0;
  tmpDeintMode   = 0;
  tmpDeint       = 0;
  max_references = 0;

  for (int i = 0; i < NUM_OUTPUT_SURFACES; i++)
    outputSurfaces[i] = VDP_INVALID_HANDLE;

  videoMixer = VDP_INVALID_HANDLE;
  m_BlackBar = NULL;

  for (int i = 0; i < NUM_OUTPUT_PICS; i++)
  {
    m_allOutPic[i].vdp_flip_target = VDP_INVALID_HANDLE;
    m_allOutPic[i].vdp_flip_queue = VDP_INVALID_HANDLE;
  }

  upScale = g_advancedSettings.m_videoVDPAUScaling;
}

bool CVDPAU::Open(AVCodecContext* avctx, const enum PixelFormat, unsigned int surfaces)
{
  if(avctx->coded_width  == 0
  || avctx->coded_height == 0)
  {
    CLog::Log(LOGWARNING,"(VDPAU) no width/height available, can't init");
    return false;
  }
  m_numRenderBuffers = surfaces;
  m_flipBuffer = new OutputPicture*[m_numRenderBuffers];
  for (int i = 0; i < m_numRenderBuffers; ++i)
    m_flipBuffer[i] = 0;

  if (!dl_handle)
  {
    dl_handle  = dlopen("libvdpau.so.1", RTLD_LAZY);
    if (!dl_handle)
    {
      const char* error = dlerror();
      if (!error)
        error = "dlerror() returned NULL";

      CLog::Log(LOGNOTICE,"(VDPAU) Unable to get handle to libvdpau: %s", error);
      //g_application.m_guiDialogKaiToast.QueueNotification(CGUIDialogKaiToast::Error, "VDPAU", error, 10000);

      return false;
    }
  }

  if (!m_dllAvUtil.Load())
    return false;

  InitVDPAUProcs();

  if (vdp_device != VDP_INVALID_HANDLE)
  {
    SpewHardwareAvailable();

    VdpDecoderProfile profile = 0;
    if(avctx->codec_id == CODEC_ID_H264)
      profile = VDP_DECODER_PROFILE_H264_HIGH;
#ifdef VDP_DECODER_PROFILE_MPEG4_PART2_ASP
    else if(avctx->codec_id == CODEC_ID_MPEG4)
      profile = VDP_DECODER_PROFILE_MPEG4_PART2_ASP;
#endif
    if(profile)
    {
      if (!CDVDCodecUtils::IsVP3CompatibleWidth(avctx->coded_width))
        CLog::Log(LOGWARNING,"(VDPAU) width %i might not be supported because of hardware bug", avctx->width);
   
      /* attempt to create a decoder with this width/height, some sizes are not supported by hw */
      VdpStatus vdp_st;
      vdp_st = vdp_decoder_create(vdp_device, profile, avctx->coded_width, avctx->coded_height, 5, &decoder);

      if(vdp_st != VDP_STATUS_OK)
      {
        CLog::Log(LOGERROR, " (VDPAU) Error: %s(%d) checking for decoder support\n", vdp_get_error_string(vdp_st), vdp_st);
        FiniVDPAUProcs();
        return false;
      }

      vdp_decoder_destroy(decoder);
      CheckStatus(vdp_st, __LINE__);
    }

    InitCSCMatrix(avctx->coded_height);

    m_vdpauOutputMethod = OUTPUT_NONE;
    glInteropFinish = false;
//    m_bPixmapBound = false;

    /* finally setup ffmpeg */
    avctx->get_buffer      = CVDPAU::FFGetBuffer;
    avctx->release_buffer  = CVDPAU::FFReleaseBuffer;
    avctx->draw_horiz_band = CVDPAU::FFDrawSlice;
    avctx->slice_flags=SLICE_FLAG_CODED_ORDER|SLICE_FLAG_ALLOW_FIELD;

    g_Windowing.Register(this);
    return true;
  }
  return false;
}

CVDPAU::~CVDPAU()
{
  GLFinish();
  Close();
}

void CVDPAU::Close()
{
  CLog::Log(LOGNOTICE, " (VDPAU) %s", __FUNCTION__);

  FiniVDPAUOutput();
  FiniVDPAUProcs();

  while (!m_videoSurfaces.empty())
  {
    vdpau_render_state *render = m_videoSurfaces.back();
    m_videoSurfaces.pop_back();
    if (render->bitstream_buffers_allocated)
      m_dllAvUtil.av_freep(&render->bitstream_buffers);
    render->bitstream_buffers_allocated = 0;
    free(render);
  }

  g_Windowing.Unregister(this);
  m_dllAvUtil.Unload();
}

bool CVDPAU::MakePixmapGL(int index)
{
  int num=0;
  int fbConfigIndex = 0;

  int doubleVisAttributes[] = {
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DEPTH_SIZE, 8,
    GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
    GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
    GLX_DOUBLEBUFFER, False,
    GLX_Y_INVERTED_EXT, True,
    GLX_X_RENDERABLE, True,
    None
  };

  int pixmapAttribs[] = {
    GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
    GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
    None
  };

  GLXFBConfig *fbConfigs;
  fbConfigs = glXChooseFBConfig(m_Display, DefaultScreen(m_Display), doubleVisAttributes, &num);
  if (fbConfigs==NULL)
  {
    CLog::Log(LOGERROR, "GLX Error: MakePixmap: No compatible framebuffers found");
    return false;
  }
  CLog::Log(LOGDEBUG, "Found %d fbconfigs.", num);
  fbConfigIndex = 0;
  CLog::Log(LOGDEBUG, "Using fbconfig index %d.", fbConfigIndex);

  m_allOutPic[index].glPixmap = glXCreatePixmap(m_Display, fbConfigs[fbConfigIndex], m_allOutPic[index].pixmap, pixmapAttribs);

  if (!m_allOutPic[index].glPixmap)
  {
    CLog::Log(LOGINFO, "GLX Error: Could not create Pixmap");
    XFree(fbConfigs);
    return false;
  }
  XFree(fbConfigs);

  return true;
}

void CVDPAU::SetWidthHeight(int width, int height)
{
  int vdpauMaxHeight = g_advancedSettings.m_videoVDPAUmaxHeight;
  if (vdpauMaxHeight > 0 && height > vdpauMaxHeight)
  {
    width = MathUtils::round_int((double)width * vdpauMaxHeight / height);
    height = vdpauMaxHeight;
  }

  //pick the smallest dimensions, so we downscale with vdpau and upscale with opengl when appropriate
  //this requires the least amount of gpu memory bandwidth
  if (g_graphicsContext.GetWidth() < width || g_graphicsContext.GetHeight() < height || upScale >= 0)
  {
    //scale width to desktop size if the aspect ratio is the same or bigger than the desktop
    if ((double)height * g_graphicsContext.GetWidth() / width <= (double)g_graphicsContext.GetHeight())
    {
      OutWidth = g_graphicsContext.GetWidth();
      OutHeight = MathUtils::round_int((double)height * g_graphicsContext.GetWidth() / width);
    }
    else //scale height to the desktop size if the aspect ratio is smaller than the desktop
    {
      OutHeight = g_graphicsContext.GetHeight();
      OutWidth = MathUtils::round_int((double)width * g_graphicsContext.GetHeight() / height);
    }
  }
  else
  { //let opengl scale
    OutWidth = width;
    OutHeight = height;
  }
  CLog::Log(LOGDEBUG, "CVDPAU::SetWidthHeight Setting OutWidth: %i OutHeight: %i vdpauMaxHeight: %i", OutWidth, OutHeight, vdpauMaxHeight);
}

bool CVDPAU::MakePixmap(int index, int width, int height)
{
  CLog::Log(LOGNOTICE,"Creating %ix%i pixmap", OutWidth, OutHeight);

    // Get our window attribs.
  XWindowAttributes wndattribs;
  XGetWindowAttributes(m_Display, DefaultRootWindow(m_Display), &wndattribs); // returns a status but I don't know what success is

  m_allOutPic[index].pixmap = XCreatePixmap(m_Display,
                           DefaultRootWindow(m_Display),
                           OutWidth,
                           OutHeight,
                           wndattribs.depth);
  if (!m_allOutPic[index].pixmap)
  {
    CLog::Log(LOGERROR, "GLX Error: MakePixmap: Unable to create XPixmap");
    return false;
  }

  XGCValues values = {};
  GC xgc;
  values.foreground = BlackPixel (m_Display, DefaultScreen (m_Display));
  xgc = XCreateGC(m_Display, m_allOutPic[index].pixmap, GCForeground, &values);
  XFillRectangle(m_Display, m_allOutPic[index].pixmap, xgc, 0, 0, OutWidth, OutHeight);
  XFreeGC(m_Display, xgc);

  if(!MakePixmapGL(index))
    return false;

  return true;
}

int CVDPAU::PreBindAllPixmaps()
{
  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return false;
  }

  if (m_vdpauOutputMethod != OUTPUT_PIXMAP || m_preBindPixmapsDone)
  {
      return 0;
  }

  //TODO: need to get a gl expert to look at correct way to texture target
  GLenum textureTarget = GL_TEXTURE_2D;  //assume 2D for now
  //if (glGet(GL_TEXTURE_RECTANGLE_ARB))
  //   textureTarget = GL_TEXTURE_RECTANGLE_ARB;

  glEnable(textureTarget);
  OutputPicture *outPic;
  for (int i = 0; i < NUM_OUTPUT_PICS; i++)
  {
      outPic = &m_allOutPic[i];
      // set texture
      if (!glIsTexture(outPic->texture[0]))
         glGenTextures(1, outPic->texture);

      //bind texture
      glBindTexture(textureTarget, outPic->texture[0]);

      // bind pixmap
      GLXPixmap glPixmap = outPic->glPixmap;
      bool bound = outPic->bound;
      outPic->bound = true;
      if (bound)
         glXReleaseTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT);
      glXBindTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT, NULL);

      glBindTexture(textureTarget, 0);
  }
  //glDisable(textureTarget); 
  m_preBindPixmapsDone = true;
  return 1;
}

int CVDPAU::SetTexture(int plane, int field, int flipBufferIdx)
{
  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return false;
  }

  m_glTexture = 0;

  CSingleLock fLock(m_flipSec);

  if (m_vdpauOutputMethod != OUTPUT_PIXMAP)
  {
    m_glTexture = GLGetSurfaceTexture(plane, field, flipBufferIdx);
    if (m_glTexture)
      return 1;
    else
      return -1;
  }
  else
  {
    PreBindAllPixmaps();
    if (m_flipBuffer[flipBufferIdx])
    {
      if (!glIsTexture(m_flipBuffer[flipBufferIdx]->texture[0]))
        glGenTextures(1, m_flipBuffer[flipBufferIdx]->texture);
      m_glTexture = m_flipBuffer[flipBufferIdx]->texture[0];
      return 0;
    }
    else
      return -1;
  }
}

GLuint CVDPAU::GetTexture()
{
  return m_glTexture;
}

void CVDPAU::BindPixmap(int flipBufferIdx)
{
  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return;
  }

  CSingleLock fLock(m_flipSec);

  if (m_vdpauOutputMethod != OUTPUT_PIXMAP)
    return;

  if (m_flipBuffer[flipBufferIdx])
  {
    GLXPixmap glPixmap = m_flipBuffer[flipBufferIdx]->glPixmap;
    bool bound = m_flipBuffer[flipBufferIdx]->bound;
    
    m_flipBuffer[flipBufferIdx]->bound = true;
    lock.Leave();
    if (bound)
      glXReleaseTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT);
    glXBindTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT, NULL);
  }
  else CLog::Log(LOGERROR,"(VDPAU) BindPixmap called without valid pixmap");
}

void CVDPAU::ReleasePixmap(int flipBufferIdx)
{
  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return;
  }

  CSingleLock fLock(m_flipSec);

  if (m_vdpauOutputMethod != OUTPUT_PIXMAP)
    return;

  if (m_flipBuffer[flipBufferIdx])
  {
    GLXPixmap glPixmap = m_flipBuffer[flipBufferIdx]->glPixmap;
    lock.Leave();
    if (glPixmap)
      glXReleaseTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT);
    //lock.Enter();
  }
  else CLog::Log(LOGERROR,"(VDPAU) ReleasePixmap called without valid pixmap");
}

void CVDPAU::OnLostDevice()
{
  CLog::Log(LOGNOTICE,"CVDPAU::OnLostDevice event");

  CExclusiveLock lock(m_DecoderSection);
  FiniVDPAUOutput();
  FiniVDPAUProcs();

  m_DisplayState = VDPAU_LOST;
  m_DisplayEvent.Reset();
}

void CVDPAU::OnResetDevice()
{
  CLog::Log(LOGNOTICE,"CVDPAU::OnResetDevice event");

  CExclusiveLock lock(m_DisplaySection);
  if (m_DisplayState == VDPAU_LOST)
  {
    m_DisplayState = VDPAU_RESET;
    m_DisplayEvent.Set();
  }
}

int CVDPAU::Check(AVCodecContext* avctx)
{
  EDisplayState state;

  { CSharedLock lock(m_DisplaySection);
    state = m_DisplayState;
  }

  if (state == VDPAU_LOST)
  {
    CLog::Log(LOGNOTICE,"CVDPAU::Check waiting for display reset event");
    if (!m_DisplayEvent.WaitMSec(2000))
    {
      CLog::Log(LOGERROR, "CVDPAU::Check - device didn't reset in reasonable time");
      return VC_ERROR;
    }
    { CSharedLock lock(m_DisplaySection);
      state = m_DisplayState;
    }
  }
  if (state == VDPAU_RESET || state == VDPAU_ERROR)
  {
    glInteropFinish = true;

    CSingleLock gLock(g_graphicsContext);
    CExclusiveLock lock(m_DecoderSection);

    FiniVDPAUOutput();
    FiniVDPAUProcs();

    InitVDPAUProcs();

    if (state == VDPAU_RESET)
      return VC_FLUSHED;
    else
      return VC_ERROR;
  }
  return 0;
}

bool CVDPAU::IsVDPAUFormat(PixelFormat format)
{
  if ((format >= PIX_FMT_VDPAU_H264) && (format <= PIX_FMT_VDPAU_VC1)) return true;
#if (defined PIX_FMT_VDPAU_MPEG4_IN_AVUTIL)
  if (format == PIX_FMT_VDPAU_MPEG4) return true;
#endif
  else return false;
}

void CVDPAU::SetPostProcFeatures(bool postProcEnabled /* = true */)
{
  if (tmpPostProc != postProcEnabled)
  {
    if (postProcEnabled)
    {
      SetNoiseReduction();
      SetSharpness();
      SetDeinterlacing();
       SetHWUpscaling();
    }
    else
      PostProcOff();
    tmpPostProc = postProcEnabled;
  }
}

void CVDPAU::CheckFeatures()
{
  if (m_vdpauOutputMethod != OUTPUT_GL_INTEROP_YUV)
  {
    if (videoMixer == VDP_INVALID_HANDLE)
    {
      CLog::Log(LOGNOTICE, " (VDPAU) Creating the video mixer");
      // Creation of VideoMixer.
      VdpVideoMixerParameter parameters[] = {
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE};

      void const * parameter_values[] = {
        &surface_width,
        &surface_height,
        &vdp_chroma_type};

      tmpBrightness = 0;
      tmpContrast = 0;
      tmpNoiseReduction = 0;
      tmpSharpness = 0;
      tmpDeint = 0;
      tmpUpScale = -1;
      tmpPostProc = true;

      VdpStatus vdp_st = VDP_STATUS_ERROR;
      vdp_st = vdp_video_mixer_create(vdp_device,
                                    m_feature_count,
                                    m_features,
                                    ARSIZE(parameters),
                                    parameters,
                                    parameter_values,
                                    &videoMixer);
      CheckStatus(vdp_st, __LINE__);
    }

    if (tmpUpScale != upScale)
    {
      SetHWUpscaling();
      tmpUpScale = upScale;
    }
    if (tmpBrightness != g_settings.m_currentVideoSettings.m_Brightness ||
      tmpContrast   != g_settings.m_currentVideoSettings.m_Contrast)
    {
      SetColor();
      tmpBrightness = g_settings.m_currentVideoSettings.m_Brightness;
      tmpContrast = g_settings.m_currentVideoSettings.m_Contrast;
    }
    if (tmpNoiseReduction != g_settings.m_currentVideoSettings.m_NoiseReduction)
    {
      tmpNoiseReduction = g_settings.m_currentVideoSettings.m_NoiseReduction;
      SetNoiseReduction();
    }
    if (tmpSharpness != g_settings.m_currentVideoSettings.m_Sharpness)
    {
      tmpSharpness = g_settings.m_currentVideoSettings.m_Sharpness;
      SetSharpness();
    }
    if (tmpDeintMode != g_settings.m_currentVideoSettings.m_DeinterlaceMode ||
        tmpDeint     != g_settings.m_currentVideoSettings.m_InterlaceMethod)
    {
      tmpDeintMode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
      tmpDeint     = g_settings.m_currentVideoSettings.m_InterlaceMethod;
      SetDeinterlacing();
    }
  }
}

bool CVDPAU::Supports(VdpVideoMixerFeature feature)
{
  for(int i = 0; i < m_feature_count; i++)
  {
    if(m_features[i] == feature)
      return true;
  }
  return false;
}

bool CVDPAU::Supports(EINTERLACEMETHOD method)
{
  if(method == VS_INTERLACEMETHOD_VDPAU_BOB
  || method == VS_INTERLACEMETHOD_AUTO)
    return true;

  if (g_guiSettings.GetBool("videoplayer.usevdpauinteropyuv"))
  {
    if (method == VS_INTERLACEMETHOD_RENDER_BOB)
      return true;
  }

  for(SInterlaceMapping* p = g_interlace_mapping; p->method != VS_INTERLACEMETHOD_NONE; p++)
  {
    if(p->method == method)
      return Supports(p->feature);
  }
  return false;
}

EINTERLACEMETHOD CVDPAU::AutoInterlaceMethod()
{
  return VS_INTERLACEMETHOD_VDPAU_TEMPORAL;
}

bool CVDPAU::GenerateStudioCSCMatrix(VdpColorStandard colorStandard, VdpCSCMatrix &studioCSCMatrix)
{
   // instead use studioCSCKCoeffs601[3], studioCSCKCoeffs709[3] to generate float[3][4] matrix (float studioCSC[3][4])
   // m00 = mRY = red: luma factor (contrast factor) (1.0)
   // m10 = mGY = green: luma factor (contrast factor) (1.0)
   // m20 = mBY = blue: luma factor (contrast factor) (1.0)
   //
   // m01 = mRB = red: blue color diff coeff (0.0)
   // m11 = mGB = green: blue color diff coeff (-2Kb(1-Kb)/(Kg))
   // m21 = mBB = blue: blue color diff coeff ((1-Kb)/0.5) 
   //
   // m02 = mRR = red: red color diff coeff ((1-Kr)/0.5)
   // m12 = mGR = green: red color diff coeff (-2Kr(1-Kr)/(Kg))
   // m22 = mBR = blue: red color diff coeff (0.0)
   //
   // m03 = mRC = red: colour zero offset (brightness factor) (-(1-Kr)/0.5 * (128/255))
   // m13 = mGC = green: colour zero offset (brightness factor) ((256/255) * (Kb(1-Kb) + Kr(1-Kr)) / Kg)
   // m23 = mBC = blue: colour zero offset (brightness factor) (-(1-Kb)/0.5 * (128/255))

   // columns
   int Y = 0;
   int Cb = 1;
   int Cr = 2;
   int C = 3; 
   // rows
   int R = 0;
   int G = 1;
   int B = 2;
   // colour standard coefficients for red, geen, blue
   double Kr, Kg, Kb;
   // colour diff zero position (use standard 8-bit coding precision)
   double CDZ = 128; //256*0.5
   // range excursion (use standard 8-bit coding precision)
   double EXC = 255; //256-1
   
   if (colorStandard == VDP_COLOR_STANDARD_ITUR_BT_601)
   {
      Kr = studioCSCKCoeffs601[0];
      Kg = studioCSCKCoeffs601[1];
      Kb = studioCSCKCoeffs601[2];
   }
   else // assume VDP_COLOR_STANDARD_ITUR_BT_709
   {
      Kr = studioCSCKCoeffs709[0];
      Kg = studioCSCKCoeffs709[1];
      Kb = studioCSCKCoeffs709[2];
   }
   // we keep luma unscaled to retain the levels present in source so that 16-235 luma is converted to RGB 16-235
   studioCSCMatrix[R][Y] = 1.0;
   studioCSCMatrix[G][Y] = 1.0;
   studioCSCMatrix[B][Y] = 1.0;
   
   studioCSCMatrix[R][Cb] = 0.0;
   studioCSCMatrix[G][Cb] = (double)-2 * Kb * (1 - Kb) / Kg;
   studioCSCMatrix[B][Cb] = (double)(1 - Kb) / 0.5;

   studioCSCMatrix[R][Cr] = (double)(1 - Kr) / 0.5;
   studioCSCMatrix[G][Cr] = (double)-2 * Kr * (1 - Kr) / Kg;
   studioCSCMatrix[B][Cr] = 0.0;

   studioCSCMatrix[R][C] = (double)-1 * studioCSCMatrix[R][Cr] * CDZ/EXC;
   studioCSCMatrix[G][C] = (double)-1 * (studioCSCMatrix[G][Cb] + studioCSCMatrix[G][Cr]) * CDZ/EXC;
   studioCSCMatrix[B][C] = (double)-1 * studioCSCMatrix[B][Cb] * CDZ/EXC;

   return true; 
}

void CVDPAU::SetColor()
{
  VdpStatus vdp_st;

  if (tmpBrightness != g_settings.m_currentVideoSettings.m_Brightness)
    m_Procamp.brightness = (float)((g_settings.m_currentVideoSettings.m_Brightness)-50) / 100;
  if (tmpContrast != g_settings.m_currentVideoSettings.m_Contrast)
    m_Procamp.contrast = (float)((g_settings.m_currentVideoSettings.m_Contrast)+50) / 100;

  VdpColorStandard colorStandard;
//  if(vid_height >= 600 || vid_width > 1024)
  if(vid_width > 1000)
    colorStandard = VDP_COLOR_STANDARD_ITUR_BT_709;
    //vdp_st = vdp_generate_csc_matrix(&m_Procamp, VDP_COLOR_STANDARD_ITUR_BT_709, &m_CSCMatrix);
  else
    colorStandard = VDP_COLOR_STANDARD_ITUR_BT_601;
    //vdp_st = vdp_generate_csc_matrix(&m_Procamp, VDP_COLOR_STANDARD_ITUR_BT_601, &m_CSCMatrix);

  VdpVideoMixerAttribute attributes[] = { VDP_VIDEO_MIXER_ATTRIBUTE_CSC_MATRIX };
  if (g_guiSettings.GetBool("videoplayer.vdpaustudiolevel"))
  {
    float studioCSC[3][4];
    GenerateStudioCSCMatrix(colorStandard, studioCSC);
    void const * pm_CSCMatix[] = { &studioCSC };
    vdp_st = vdp_video_mixer_set_attribute_values(videoMixer, ARSIZE(attributes), attributes, pm_CSCMatix);
  }
  else
  {
    vdp_st = vdp_generate_csc_matrix(&m_Procamp, colorStandard, &m_CSCMatrix);
    void const * pm_CSCMatix[] = { &m_CSCMatrix };
    vdp_st = vdp_video_mixer_set_attribute_values(videoMixer, ARSIZE(attributes), attributes, pm_CSCMatix);
  }
  CheckStatus(vdp_st, __LINE__);
}

void CVDPAU::SetNoiseReduction()
{
  if(!Supports(VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION))
    return;

  VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION };
  VdpVideoMixerAttribute attributes[] = { VDP_VIDEO_MIXER_ATTRIBUTE_NOISE_REDUCTION_LEVEL };
  VdpStatus vdp_st;

  if (!g_settings.m_currentVideoSettings.m_NoiseReduction)
  {
    VdpBool enabled[]= {0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
    return;
  }
  VdpBool enabled[]={1};
  vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
  CheckStatus(vdp_st, __LINE__);
  void* nr[] = { &g_settings.m_currentVideoSettings.m_NoiseReduction };
  CLog::Log(LOGNOTICE,"Setting Noise Reduction to %f",g_settings.m_currentVideoSettings.m_NoiseReduction);
  vdp_st = vdp_video_mixer_set_attribute_values(videoMixer, ARSIZE(attributes), attributes, nr);
  CheckStatus(vdp_st, __LINE__);
}

void CVDPAU::SetSharpness()
{
  if(!Supports(VDP_VIDEO_MIXER_FEATURE_SHARPNESS))
    return;
  
  VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_SHARPNESS };
  VdpVideoMixerAttribute attributes[] = { VDP_VIDEO_MIXER_ATTRIBUTE_SHARPNESS_LEVEL };
  VdpStatus vdp_st;

  if (!g_settings.m_currentVideoSettings.m_Sharpness)
  {
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
    return;
  }
  VdpBool enabled[]={1};
  vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
  CheckStatus(vdp_st, __LINE__);
  void* sh[] = { &g_settings.m_currentVideoSettings.m_Sharpness };
  CLog::Log(LOGNOTICE,"Setting Sharpness to %f",g_settings.m_currentVideoSettings.m_Sharpness);
  vdp_st = vdp_video_mixer_set_attribute_values(videoMixer, ARSIZE(attributes), attributes, sh);
  CheckStatus(vdp_st, __LINE__);
}

void CVDPAU::SetDeintSkipChroma()
{
  VdpVideoMixerAttribute attribute[] = { VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE};
  VdpStatus vdp_st;

  uint8_t val;
  if (g_advancedSettings.m_videoVDPAUdeintSkipChromaHD && vid_height >= 720)
    val = 1;
  else
    val = 0;

  void const *values[]={&val};
  vdp_st = vdp_video_mixer_set_attribute_values(videoMixer, ARSIZE(attribute), attribute, values);

  CheckStatus(vdp_st, __LINE__);
}

void CVDPAU::SetHWUpscaling()
{
#ifdef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
  //if(!Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1) || upScale <= 0)
    //return;

  VdpStatus vdp_st;
  VdpBool enabled[]={1};
  switch (upScale)
  {
    case 9:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L9))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L9 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 8:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L8))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L8 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 7:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L7))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L7 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 6:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L6))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L6 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 5:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L5))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L5 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 4:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L4))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L4 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 3:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L3))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L3 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 2:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L2))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L2 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    case 1:
       if (Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1))
       {
          VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 };
          vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
          break;
       }
    default:
       DisableHQScaling();
       return;
  }
  CheckStatus(vdp_st, __LINE__);
#endif
}

EINTERLACEMETHOD CVDPAU::GetDeinterlacingMethod(bool log /* = false */)
{
  EINTERLACEMETHOD method = g_settings.m_currentVideoSettings.m_InterlaceMethod;
  if (method == VS_INTERLACEMETHOD_AUTO)
  {
    int deint = -1;
    if (vid_height >= 720)
      deint = g_advancedSettings.m_videoVDPAUdeintHD;
    else
      deint = g_advancedSettings.m_videoVDPAUdeintSD;

    if (deint != -1)
    {
      if (Supports(EINTERLACEMETHOD(deint)))
      {
        method = EINTERLACEMETHOD(deint);
        if (log)
          CLog::Log(LOGNOTICE, "CVDPAU::GetDeinterlacingMethod: set de-interlacing to %d",  deint);
      }
      else
      {
        if (log)
          CLog::Log(LOGWARNING, "CVDPAU::GetDeinterlacingMethod: method for de-interlacing (advanced settings) not supported");
      }
    }
  }
  return method;
}

void CVDPAU::SetDeinterlacing()
{
  VdpStatus vdp_st;

  if (videoMixer == VDP_INVALID_HANDLE)
    return;

  EDEINTERLACEMODE   mode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
  EINTERLACEMETHOD method = GetDeinterlacingMethod(true);

  VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL,
                                     VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL,
                                     VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE };

  if (mode == VS_DEINTERLACEMODE_OFF)
  {
    VdpBool enabled[]={0,0,0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
  }
  else
  {
    if (method == VS_INTERLACEMETHOD_AUTO)
    {
      VdpBool enabled[]={1,1,1};
      vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    }
    else if (method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL
         ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF)
    {
      VdpBool enabled[]={1,0,0};
      vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    }
    else if (method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL
         ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF)
    {
      VdpBool enabled[]={1,1,0};
      vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    }
    else if (method == VS_INTERLACEMETHOD_VDPAU_INVERSE_TELECINE)
    {
      VdpBool enabled[]={1,0,1};
      vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    }
    else
    {
      VdpBool enabled[]={0,0,0};
      vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    }
  }
  CheckStatus(vdp_st, __LINE__);

  SetDeintSkipChroma();
}

void CVDPAU::DisableHQScaling()
{
  VdpStatus vdp_st;

  if (videoMixer == VDP_INVALID_HANDLE)
    return;

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L2))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L2 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L3))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L3 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L4))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L4 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L5))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L5 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L6))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L6 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L7))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L7 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L8))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L8 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L9))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L9 };
    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }
}

void CVDPAU::PostProcOff()
{
  VdpStatus vdp_st;

  if (videoMixer == VDP_INVALID_HANDLE)
    return;

  VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL,
                                     VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL,
                                     VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE};

  VdpBool enabled[]={0,0,0};
  vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
  CheckStatus(vdp_st, __LINE__);

  if(Supports(VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION};

    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  if(Supports(VDP_VIDEO_MIXER_FEATURE_SHARPNESS))
  {
    VdpVideoMixerFeature feature[] = { VDP_VIDEO_MIXER_FEATURE_SHARPNESS};

    VdpBool enabled[]={0};
    vdp_st = vdp_video_mixer_set_feature_enables(videoMixer, ARSIZE(feature), feature, enabled);
    CheckStatus(vdp_st, __LINE__);
  }

  DisableHQScaling();
}


void CVDPAU::InitVDPAUProcs()
{
  CSingleLock glock(g_graphicsContext);

  char* error;

  (void)dlerror();
  dl_vdp_device_create_x11 = (VdpStatus (*)(Display*, int, VdpDevice*, VdpStatus (**)(VdpDevice, VdpFuncId, void**)))dlsym(dl_handle, (const char*)"vdp_device_create_x11");
  error = dlerror();
  if (error)
  {
    CLog::Log(LOGERROR,"(VDPAU) - %s in %s",error,__FUNCTION__);
    vdp_device = VDP_INVALID_HANDLE;

    //g_application.m_guiDialogKaiToast.QueueNotification(CGUIDialogKaiToast::Error, "VDPAU", error, 10000);

    return;
  }

  if (dl_vdp_device_create_x11)
  {
    CSingleLock lock(g_graphicsContext);
    m_Display = g_Windowing.GetDisplay();
  }

  int mScreen = DefaultScreen(m_Display);
  VdpStatus vdp_st;

  // Create Device
  // tested on 64bit Ubuntu 11.10 and it deadlocked without this
  XLockDisplay(m_Display);
  vdp_st = dl_vdp_device_create_x11(m_Display, //x_display,
                                 mScreen, //x_screen,
                                 &vdp_device,
                                 &vdp_get_proc_address);
  XUnlockDisplay(m_Display);

  CLog::Log(LOGNOTICE,"vdp_device = 0x%08x vdp_st = 0x%08x",vdp_device,vdp_st);
  if (vdp_st != VDP_STATUS_OK)
  {
    CLog::Log(LOGERROR,"(VDPAU) unable to init VDPAU - vdp_st = 0x%x.  Falling back.",vdp_st);
    vdp_device = VDP_INVALID_HANDLE;
    return;
  }

#define VDP_PROC(id, proc) \
  do { \
    vdp_st = vdp_get_proc_address(vdp_device, id, (void**)&proc); \
    CheckStatus(vdp_st, __LINE__); \
  } while(0);

  VDP_PROC(VDP_FUNC_ID_GET_ERROR_STRING                    , vdp_get_error_string);
  VDP_PROC(VDP_FUNC_ID_DEVICE_DESTROY                      , vdp_device_destroy);
  VDP_PROC(VDP_FUNC_ID_GENERATE_CSC_MATRIX                 , vdp_generate_csc_matrix);
  VDP_PROC(VDP_FUNC_ID_VIDEO_SURFACE_CREATE                , vdp_video_surface_create);
  VDP_PROC(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY               , vdp_video_surface_destroy);
  VDP_PROC(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR      , vdp_video_surface_put_bits_y_cb_cr);
  VDP_PROC(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR      , vdp_video_surface_get_bits_y_cb_cr);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_Y_CB_CR     , vdp_output_surface_put_bits_y_cb_cr);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE      , vdp_output_surface_put_bits_native);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE               , vdp_output_surface_create);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY              , vdp_output_surface_destroy);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE      , vdp_output_surface_get_bits_native);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE, vdp_output_surface_render_output_surface);
  VDP_PROC(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_INDEXED     , vdp_output_surface_put_bits_indexed);  
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_CREATE                  , vdp_video_mixer_create);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_SET_FEATURE_ENABLES     , vdp_video_mixer_set_feature_enables);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_DESTROY                 , vdp_video_mixer_destroy);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_RENDER                  , vdp_video_mixer_render);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES    , vdp_video_mixer_set_attribute_values);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_QUERY_PARAMETER_SUPPORT , vdp_video_mixer_query_parameter_support);
  VDP_PROC(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT   , vdp_video_mixer_query_feature_support);
  VDP_PROC(VDP_FUNC_ID_DECODER_CREATE                      , vdp_decoder_create);
  VDP_PROC(VDP_FUNC_ID_DECODER_DESTROY                     , vdp_decoder_destroy);
  VDP_PROC(VDP_FUNC_ID_DECODER_RENDER                      , vdp_decoder_render);
  VDP_PROC(VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES          , vdp_decoder_query_caps);
  VDP_PROC(VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER        , vdp_preemption_callback_register);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY          , vdp_presentation_queue_target_destroy);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE                  , vdp_presentation_queue_create);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY                 , vdp_presentation_queue_destroy);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY                 , vdp_presentation_queue_display);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE, vdp_presentation_queue_block_until_surface_idle);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11       , vdp_presentation_queue_target_create_x11);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS    , vdp_presentation_queue_query_surface_status);
  VDP_PROC(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME                , vdp_presentation_queue_get_time);
  
#undef VDP_PROC

  // set all vdpau resources to invalid
  videoMixer = VDP_INVALID_HANDLE;
  totalAvailableOutputSurfaces = 0;
  presentSurface = VDP_INVALID_HANDLE;
  for (int i = 0; i < NUM_OUTPUT_SURFACES; i++)
    outputSurfaces[i] = VDP_INVALID_HANDLE;

  m_vdpauOutputMethod = OUTPUT_NONE;

  CExclusiveLock lock(m_DisplaySection);
  m_DisplayState = VDPAU_OPEN;
  vdpauConfigured = false;
}

void CVDPAU::FiniVDPAUProcs()
{
  if (vdp_device == VDP_INVALID_HANDLE) return;

  VdpStatus vdp_st;
  vdp_st = vdp_device_destroy(vdp_device);
  CheckStatus(vdp_st, __LINE__);
  vdp_device = VDP_INVALID_HANDLE;
  vdpauConfigured = false;
}

void CVDPAU::InitCSCMatrix(int Width)
{
  VdpStatus vdp_st;
  m_Procamp.struct_version = VDP_PROCAMP_VERSION;
  m_Procamp.brightness     = 0.0;
  m_Procamp.contrast       = 1.0;
  m_Procamp.saturation     = 1.0;
  m_Procamp.hue            = 0;
  vdp_st = vdp_generate_csc_matrix(&m_Procamp,
                                   (Width < 1000)? VDP_COLOR_STANDARD_ITUR_BT_601 : VDP_COLOR_STANDARD_ITUR_BT_709,
                                   &m_CSCMatrix);
  CheckStatus(vdp_st, __LINE__);
}

void CVDPAU::FiniVDPAUOutput()
{
  FiniOutputMethod();

  if (vdp_device == VDP_INVALID_HANDLE || !vdpauConfigured) return;

  CLog::Log(LOGNOTICE, " (VDPAU) %s", __FUNCTION__);

  VdpStatus vdp_st;

  vdp_st = vdp_decoder_destroy(decoder);
  if (CheckStatus(vdp_st, __LINE__))
    return;
  decoder = VDP_INVALID_HANDLE;

  CSingleLock lock(m_videoSurfaceSec);
  CLog::Log(LOGDEBUG, "CVDPAU::FiniVDPAUOutput destroying %i video surfaces", m_videoSurfaces.size());
  
  for(unsigned int i = 0; i < m_videoSurfaces.size(); ++i)
  {
    vdpau_render_state *render = m_videoSurfaces[i];
    if (render->surface != VDP_INVALID_HANDLE)
    {
      vdp_st = vdp_video_surface_destroy(render->surface);
      render->surface = VDP_INVALID_HANDLE;
    }
    if (CheckStatus(vdp_st, __LINE__))
      return;
  }
}

void CVDPAU::ReadFormatOf( PixelFormat fmt
                         , VdpDecoderProfile &vdp_decoder_profile
                         , VdpChromaType     &vdp_chroma_type)
{
  switch (fmt)
  {
    case PIX_FMT_VDPAU_MPEG1:
      vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG1;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
      break;
    case PIX_FMT_VDPAU_MPEG2:
      vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG2_MAIN;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
      break;
    case PIX_FMT_VDPAU_H264:
      vdp_decoder_profile = VDP_DECODER_PROFILE_H264_HIGH;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
      break;
    case PIX_FMT_VDPAU_WMV3:
      vdp_decoder_profile = VDP_DECODER_PROFILE_VC1_MAIN;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
      break;
    case PIX_FMT_VDPAU_VC1:
      vdp_decoder_profile = VDP_DECODER_PROFILE_VC1_ADVANCED;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
      break;
#if (defined PIX_FMT_VDPAU_MPEG4_IN_AVUTIL) && \
    (defined VDP_DECODER_PROFILE_MPEG4_PART2_ASP)
    case PIX_FMT_VDPAU_MPEG4:
      vdp_decoder_profile = VDP_DECODER_PROFILE_MPEG4_PART2_ASP;
      vdp_chroma_type     = VDP_CHROMA_TYPE_420;
#endif
    default:
      vdp_decoder_profile = 0;
      vdp_chroma_type     = 0;
  }
}


bool CVDPAU::ConfigVDPAU(AVCodecContext* avctx, int ref_frames)
{
CLog::Log(LOGDEBUG, "ASB: ConfigVDPAU()");
  FiniVDPAUOutput();

  VdpStatus vdp_st;
  VdpDecoderProfile vdp_decoder_profile;

  vid_width = avctx->width;
  vid_height = avctx->height;
  surface_width = avctx->coded_width;
  surface_height = avctx->coded_height;

  SetWidthHeight(avctx->width,avctx->height);

  CLog::Log(LOGNOTICE, " (VDPAU) screenWidth:%i vidWidth:%i surfaceWidth:%i",OutWidth,vid_width,surface_width);
  CLog::Log(LOGNOTICE, " (VDPAU) screenHeight:%i vidHeight:%i surfaceHeight:%i",OutHeight,vid_height,surface_height);

  ReadFormatOf(avctx->pix_fmt, vdp_decoder_profile, vdp_chroma_type);

  if(avctx->pix_fmt == PIX_FMT_VDPAU_H264)
  {
     max_references = ref_frames;
     if (max_references > 16) max_references = 16;
     if (max_references < 5)  max_references = 5;
  }
  else
    max_references = 2;

  vdp_st = vdp_decoder_create(vdp_device,
                              vdp_decoder_profile,
                              surface_width,
                              surface_height,
                              max_references,
                              &decoder);
  if (CheckStatus(vdp_st, __LINE__))
    return false;

  m_vdpauOutputMethod = OUTPUT_NONE;

  vdpauConfigured = true;
  m_binterlacedFrame = false;
  return true;
}

bool CVDPAU::ConfigOutputMethod(AVCodecContext *avctx, AVFrame *pFrame)
{
  VdpStatus vdp_st;

  if (!pFrame)
    return true;

  if (m_binterlacedFrame != pFrame->interlaced_frame)
  {
    m_binterlacedFrame = pFrame->interlaced_frame;
    CLog::Log(LOGNOTICE, "CVDPAU::ConfigOutputMethod: interlaced flag changed to %d", m_binterlacedFrame);
    tmpDeint = 0;
  }

  // check if one of the vdpau interlacing methods are chosen
  m_bVdpauDeinterlacing = false;
  EDEINTERLACEMODE   mode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;

  // VS_INTERLACEMETHOD_FORCE_VDPAU_NONE tells us force mixer use when interop_yuv is available, without enabling vdpau de-interlacing
  // VS_INTERLACEMETHOD_VDPAU_BOB is the basic vdpau mixer de-interlacer
  // the HALF modes are only consider half the fields in the mixer
  // the AUTO mode enables de-interlacing if stream appears to be interlaced and enables temporal + temporal/spatial + ivtc-detection
  // TODO: AUTO mode should offer ability to switch off IVTC detection 
  // TODO: VS_INTERLACEMETHOD_VDPAU_INVERSE_TELECINE has a bad name so at least change in GUI string to show that it is temporal + ivtc detection
  if (mode == VS_DEINTERLACEMODE_FORCE ||
     (mode == VS_DEINTERLACEMODE_AUTO && m_binterlacedFrame))
  {
    EINTERLACEMETHOD method = GetDeinterlacingMethod();
    if((method == VS_INTERLACEMETHOD_AUTO && m_binterlacedFrame)
     ||  method == VS_INTERLACEMETHOD_VDPAU_BOB
     ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL
     ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF
     ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL
     ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF
     ||  method == VS_INTERLACEMETHOD_VDPAU_INVERSE_TELECINE)
    {
      m_bVdpauDeinterlacing = true;
    }
  }

  if (!m_bVdpauDeinterlacing &&
        g_guiSettings.GetBool("videoplayer.usevdpauinteropyuv"))
  {
    if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_YUV)
      return true;

    FiniOutputMethod();
    CLog::Log(LOGNOTICE, " (VDPAU) Configure YUV output");

    OutWidth = surface_width;
    OutHeight = surface_height;

    m_outPicsNum = std::min(NUM_OUTPUT_SURFACES, NUM_OUTPUT_PICS);
    for (int i = 0; i < m_outPicsNum; i++)
    {
      m_allOutPic[i].render = NULL;
      m_freeOutPic.push_back(&m_allOutPic[i]);
    }

    m_vdpauOutputMethod = OUTPUT_GL_INTEROP_YUV;
  }
  // RGB config for pixmap and gl interop with vdpau deinterlacing
  else
  {
    if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_RGB
        || m_vdpauOutputMethod == OUTPUT_PIXMAP)
      return true;

    CSingleLock glock(g_graphicsContext);
    FiniOutputMethod();

    CLog::Log(LOGNOTICE, " (VDPAU) Configure RGB output");

    // create mixer thread
    Create();

    SetWidthHeight(avctx->width,avctx->height);
    totalAvailableOutputSurfaces = 0;

    int tmpMaxOutputSurfaces = NUM_OUTPUT_SURFACES;
    if (!g_guiSettings.GetBool("videoplayer.usevdpauinteroprgb"))
      tmpMaxOutputSurfaces = std::min(std::max(g_advancedSettings.m_videoVDPAUnumOutSurfacesPixmap, 1), NUM_OUTPUT_SURFACES);

    // Creation of outputSurfaces
    for (int i = 0; i < tmpMaxOutputSurfaces; i++)
    {
      vdp_st = vdp_output_surface_create(vdp_device,
                                       VDP_RGBA_FORMAT_B8G8R8A8,
                                       OutWidth,
                                       OutHeight,
                                       &outputSurfaces[i]);
      if (CheckStatus(vdp_st, __LINE__))
        return false;

      totalAvailableOutputSurfaces++;
    }
    CLog::Log(LOGNOTICE, " (VDPAU) Total Output Surfaces Available: %i of a max (tmp: %i const: %i)",
                       totalAvailableOutputSurfaces,
                       tmpMaxOutputSurfaces,
                       NUM_OUTPUT_SURFACES);

    // create 3 pitches of black lines needed for clipping top
    // and bottom lines when de-interlacing
    m_BlackBar = new uint32_t[3*OutWidth];
    memset(m_BlackBar, 0, 3*OutWidth*sizeof(uint32_t));

    if (g_guiSettings.GetBool("videoplayer.usevdpauinteroprgb"))
       m_outPicsNum = std::min(NUM_OUTPUT_SURFACES, NUM_OUTPUT_PICS);
    else
       m_outPicsNum = NUM_OUTPUT_PICS;
        
    for (int i = 0; i < m_outPicsNum; i++)
    {
      if (g_guiSettings.GetBool("videoplayer.usevdpauinteroprgb"))
        m_allOutPic[i].outputSurface = outputSurfaces[i];

      m_allOutPic[i].render = NULL;
      m_freeOutPic.push_back(&m_allOutPic[i]);
    }

    m_mixerCmd = 0;

    if (g_guiSettings.GetBool("videoplayer.usevdpauinteroprgb"))
      m_vdpauOutputMethod = OUTPUT_GL_INTEROP_RGB;
    else
    {
      for (int i = 0; i < m_outPicsNum; i++)
      {
        MakePixmap(i, OutWidth, OutHeight);
        vdp_st = vdp_presentation_queue_target_create_x11(vdp_device,
                                                m_allOutPic[i].pixmap, //x_window,
                                                &m_allOutPic[i].vdp_flip_target);
        if (CheckStatus(vdp_st, __LINE__))
          return false;

        vdp_st = vdp_presentation_queue_create(vdp_device,
                                               m_allOutPic[i].vdp_flip_target,
                                               &m_allOutPic[i].vdp_flip_queue);
        if (CheckStatus(vdp_st, __LINE__))
          return false;

        m_allOutPic[i].outputSurface = VDP_INVALID_HANDLE;
        m_allOutPic[i].bound = false;
      }
      m_vdpauOutputMethod = OUTPUT_PIXMAP;
    }
  } // RGB

  return true;
}

bool CVDPAU::FiniOutputMethod()
{
  VdpStatus vdp_st;

  // make sure no output surfaces are in use
  Reset();

  // stop mixer thread
  StopThread();

  CSingleLock glock(g_graphicsContext);
  // destroy pixmap stuff
  for (int i = 0; i < m_outPicsNum; i++)
  {
    if (m_allOutPic[i].vdp_flip_queue != VDP_INVALID_HANDLE)
    {
      vdp_st = vdp_presentation_queue_destroy(m_allOutPic[i].vdp_flip_queue);
      CheckStatus(vdp_st, __LINE__);
      m_allOutPic[i].vdp_flip_queue = VDP_INVALID_HANDLE;
    }
    if (m_allOutPic[i].vdp_flip_target != VDP_INVALID_HANDLE)
    {
      vdp_st = vdp_presentation_queue_target_destroy(m_allOutPic[i].vdp_flip_target);
      CheckStatus(vdp_st, __LINE__);
      m_allOutPic[i].vdp_flip_target = VDP_INVALID_HANDLE;
    }
    if (m_allOutPic[i].glPixmap)
    {
      CLog::Log(LOGDEBUG, "GLX: Destroying glPixmap");
      glXDestroyPixmap(m_Display, m_allOutPic[i].glPixmap);
      m_allOutPic[i].glPixmap = 0;
    }
    if (m_allOutPic[i].pixmap)
    {
      CLog::Log(LOGDEBUG, "GLX: Destroying XPixmap");
      XFreePixmap(m_Display, m_allOutPic[i].pixmap);
      m_allOutPic[i].pixmap = 0;
    }
    if (m_vdpauOutputMethod != OUTPUT_NONE && vdpauConfigured)
      ClearPicUsedForRender(&m_allOutPic[i]); //just to be sure
  }

  for (int i = 0; i < totalAvailableOutputSurfaces; i++)
  {
    if (outputSurfaces[i] == VDP_INVALID_HANDLE)
      continue;
    vdp_st = vdp_output_surface_destroy(outputSurfaces[i]);
    CheckStatus(vdp_st, __LINE__);
    outputSurfaces[i] = VDP_INVALID_HANDLE;
  }
  totalAvailableOutputSurfaces = 0;

  if (videoMixer != VDP_INVALID_HANDLE)
  {
    vdp_st = vdp_video_mixer_destroy(videoMixer);
    CheckStatus(vdp_st, __LINE__);
    videoMixer = VDP_INVALID_HANDLE;
  }

  if (m_BlackBar)
  {
    delete [] m_BlackBar;
    m_BlackBar = NULL;
  }

  m_preBindPixmapsDone = false;

  { CSingleLock lock(m_mixerSec);
    while (!m_mixerMessages.empty())
    {
      MixerMessage &tmp = m_mixerMessages.front();
      ClearMsgUsedForRender(tmp);
      m_mixerMessages.pop();
    }
    while (!m_mixerInput.empty())
    {
      MixerMessage &tmp = m_mixerInput.front();
      ClearMsgUsedForRender(tmp);
      m_mixerInput.pop_front();
    }
  }
  { CSingleLock lock(m_outPicSec);
    while (!m_freeOutPic.empty())
      m_freeOutPic.pop_front();
    while (!m_usedOutPic.empty())
      m_usedOutPic.pop_front();
    while (!m_presentOutPic.empty())
      m_presentOutPic.pop_front();
    while (!m_mixerOutPic.empty())
      m_mixerOutPic.pop_front();
    m_presentPicture = 0;
  }

  for (int i = 0; i < m_numRenderBuffers; ++i)
    m_flipBuffer[i] = 0;

  // force cleanup of opengl interop
  glInteropFinish = true;
}

void CVDPAU::SpewHardwareAvailable()  //Copyright (c) 2008 Wladimir J. van der Laan  -- VDPInfo
{
  VdpStatus rv;
  CLog::Log(LOGNOTICE,"VDPAU Decoder capabilities:");
  CLog::Log(LOGNOTICE,"name          level macbs width height");
  CLog::Log(LOGNOTICE,"------------------------------------");
  for(unsigned int x=0; x<decoder_profile_count; ++x)
  {
    VdpBool is_supported = false;
    uint32_t max_level, max_macroblocks, max_width, max_height;
    rv = vdp_decoder_query_caps(vdp_device, decoder_profiles[x].id,
                                &is_supported, &max_level, &max_macroblocks, &max_width, &max_height);
    if(rv == VDP_STATUS_OK && is_supported)
    {
      CLog::Log(LOGNOTICE,"%-16s %2i %5i %5i %5i\n", decoder_profiles[x].name,
                max_level, max_macroblocks, max_width, max_height);
    }
  }
  CLog::Log(LOGNOTICE,"------------------------------------");
  m_feature_count = 0;
#define CHECK_SUPPORT(feature)  \
  do { \
    VdpBool supported; \
    if(vdp_video_mixer_query_feature_support(vdp_device, feature, &supported) == VDP_STATUS_OK && supported) { \
      CLog::Log(LOGNOTICE, "Mixer feature: "#feature);  \
      m_features[m_feature_count++] = feature; \
    } \
  } while(false)

  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_SHARPNESS);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE);
#ifdef VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L1);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L2);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L3);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L4);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L5);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L6);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L7);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L8);
  CHECK_SUPPORT(VDP_VIDEO_MIXER_FEATURE_HIGH_QUALITY_SCALING_L9);
#endif
#undef CHECK_SUPPORT

}

bool CVDPAU::IsSurfaceValid(vdpau_render_state *render)
{
  // find render state in queue
  bool found(false);
  unsigned int i;
  for(i = 0; i < m_videoSurfaces.size(); ++i)
  {
    if(m_videoSurfaces[i] == render)
    {
      found = true;
      break;
    }
  }
  if (!found)
  {
    CLog::Log(LOGERROR,"%s - video surface not found", __FUNCTION__);
    return false;
  }
  if (m_videoSurfaces[i]->surface == VDP_INVALID_HANDLE)
  {
    m_videoSurfaces[i]->state = 0;
    return false;
  }

  return true;
}

int CVDPAU::FFGetBuffer(AVCodecContext *avctx, AVFrame *pic)
{
  //CLog::Log(LOGNOTICE,"%s",__FUNCTION__);
  CDVDVideoCodecFFmpeg* ctx        = (CDVDVideoCodecFFmpeg*)avctx->opaque;
  CVDPAU*               vdp        = (CVDPAU*)ctx->GetHardware();
  struct pictureAge*    pA         = &vdp->picAge;

  // while we are waiting to recover we can't do anything
  CSharedLock lock(vdp->m_DecoderSection);

  { CSharedLock dLock(vdp->m_DisplaySection);
    if(vdp->m_DisplayState != VDPAU_OPEN)
    {
      CLog::Log(LOGWARNING, "CVDPAU::FFGetBuffer - returning due to awaiting recovery");
      return -1;
    }
  }

  vdpau_render_state * render = NULL;

  // find unused surface
  { CSingleLock lock(vdp->m_videoSurfaceSec);
    for(unsigned int i = 0; i < vdp->m_videoSurfaces.size(); i++)
    {
      if(!(vdp->m_videoSurfaces[i]->state & (FF_VDPAU_STATE_USED_FOR_REFERENCE | FF_VDPAU_STATE_USED_FOR_RENDER)))
      {
        render = vdp->m_videoSurfaces[i];
        render->state = 0;
        break;
      }
    }
  }

  VdpStatus vdp_st = VDP_STATUS_ERROR;
  if (render == NULL)
  {
    // create a new surface
    VdpDecoderProfile profile;
    ReadFormatOf(avctx->pix_fmt, profile, vdp->vdp_chroma_type);
    render = (vdpau_render_state*)calloc(sizeof(vdpau_render_state), 1);
    if (render == NULL)
    {
      CLog::Log(LOGWARNING, "CVDPAU::FFGetBuffer - calloc failed");
      return -1;
    }
    CSingleLock lock(vdp->m_videoSurfaceSec);
    render->surface = VDP_INVALID_HANDLE;
    vdp->m_videoSurfaces.push_back(render);
  }

  if (render->surface == VDP_INVALID_HANDLE)
  {
    vdp_st = vdp->vdp_video_surface_create(vdp->vdp_device,
                                         vdp->vdp_chroma_type,
                                         avctx->coded_width,
                                         avctx->coded_height,
                                         &render->surface);
    vdp->CheckStatus(vdp_st, __LINE__);
    if (vdp_st != VDP_STATUS_OK)
    {
      free(render);
      CLog::Log(LOGERROR, "CVDPAU::FFGetBuffer - No Video surface available could be created");
      return -1;
    }
  }

  if (render == NULL)
    return -1;

  pic->data[1] =  pic->data[2] = NULL;
  pic->data[0]= (uint8_t*)render;

  pic->linesize[0] = pic->linesize[1] =  pic->linesize[2] = 0;

  if(pic->reference)
  {
    pic->age = pA->ip_age[0];
    pA->ip_age[0]= pA->ip_age[1]+1;
    pA->ip_age[1]= 1;
    pA->b_age++;
  }
  else
  {
    pic->age = pA->b_age;
    pA->ip_age[0]++;
    pA->ip_age[1]++;
    pA->b_age = 1;
  }
  pic->type= FF_BUFFER_TYPE_USER;

  render->state |= FF_VDPAU_STATE_USED_FOR_REFERENCE;
  pic->reordered_opaque= avctx->reordered_opaque;
  return 0;
}

void CVDPAU::FFReleaseBuffer(AVCodecContext *avctx, AVFrame *pic)
{
  //CLog::Log(LOGNOTICE,"%s",__FUNCTION__);
  CDVDVideoCodecFFmpeg* ctx        = (CDVDVideoCodecFFmpeg*)avctx->opaque;
  CVDPAU*               vdp        = (CVDPAU*)ctx->GetHardware();

  vdpau_render_state  * render;
  unsigned int i;

  CSharedLock lock(vdp->m_DecoderSection);

  render=(vdpau_render_state*)pic->data[0];
  if(!render)
  {
    CLog::Log(LOGERROR, "CVDPAU::FFReleaseBuffer - invalid context handle provided");
    return;
  }

  CSingleLock vLock(vdp->m_videoSurfaceSec);
  render->state &= ~FF_VDPAU_STATE_USED_FOR_REFERENCE;
  for(i=0; i<4; i++)
    pic->data[i]= NULL;

  // find render state in queue
  if (!vdp->IsSurfaceValid(render))
  {
    CLog::Log(LOGDEBUG, "CVDPAU::FFReleaseBuffer - ignoring invalid buffer");
    return;
  }

  render->state &= ~FF_VDPAU_STATE_USED_FOR_REFERENCE;
}


void CVDPAU::FFDrawSlice(struct AVCodecContext *s,
                                           const AVFrame *src, int offset[4],
                                           int y, int type, int height)
{
  CDVDVideoCodecFFmpeg* ctx = (CDVDVideoCodecFFmpeg*)s->opaque;
  CVDPAU*               vdp = (CVDPAU*)ctx->GetHardware();

  // while we are waiting to recover we can't do anything
  CSharedLock lock(vdp->m_DecoderSection);

  { CSharedLock dLock(vdp->m_DisplaySection);
    if(vdp->m_DisplayState != VDPAU_OPEN)
      return;
  }


  if(src->linesize[0] || src->linesize[1] || src->linesize[2]
  || offset[0] || offset[1] || offset[2])
  {
    CLog::Log(LOGERROR, "CVDPAU::FFDrawSlice - invalid linesizes or offsets provided");
    return;
  }

  VdpStatus vdp_st;
  vdpau_render_state * render;

  render = (vdpau_render_state*)src->data[0];
  if(!render)
  {
    CLog::Log(LOGERROR, "CVDPAU::FFDrawSlice - invalid context handle provided");
    return;
  }

  // ffmpeg vc-1 decoder does not flush, make sure the data buffer is still valid
  if (!vdp->IsSurfaceValid(render))
  {
    CLog::Log(LOGWARNING, "CVDPAU::FFDrawSlice - ignoring invalid buffer");
    return;
  }

  uint32_t max_refs = 0;
  if(s->pix_fmt == PIX_FMT_VDPAU_H264)
    max_refs = render->info.h264.num_ref_frames;

  if(vdp->decoder == VDP_INVALID_HANDLE
  || vdp->vdpauConfigured == false
  || vdp->max_references < max_refs)
  {
    if(!vdp->ConfigVDPAU(s, max_refs))
      return;
  }

  vdp_st = vdp->vdp_decoder_render(vdp->decoder,
                                   render->surface,
                                   (VdpPictureInfo const *)&(render->info),
                                   render->bitstream_buffers_used,
                                   render->bitstream_buffers);
  vdp->CheckStatus(vdp_st, __LINE__);
}

bool CVDPAU::QueueIsFull(bool wait /* = false */)
{
  if (m_vdpauOutputMethod == OUTPUT_NONE || !vdpauConfigured)
    return false;

  m_queueSignal.Reset();
  int msgs, iFreePics, usedPics;
  int reported = 0;

  if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_YUV)
  {
    CSingleLock locku(m_outPicSec);
    if (!m_freeOutPic.empty())
      return false; // buffers are not full
  }
  else
  {
    // always ensure we have at least 1 free pic (ie assume full if only 1 freeOutPic) so that mixer cannot be starved
    CSingleLock lockm(m_mixerSec);
    msgs = m_mixerMessages.size();
    lockm.Leave();
    CSingleLock locku(m_outPicSec);
    iFreePics = m_freeOutPic.size();
    int iNotFreePics = m_outPicsNum - iFreePics;
    usedPics = m_usedOutPic.size();
    for (int i=0; i<2 && i<usedPics; i++)
    {
      if (m_usedOutPic[i]->reported)
        reported++;
    }
    locku.Leave();

    // assume for simplicity 2 output pics can come from one frame message if de-interlacing via mixer
    // and that mixer-input can have 2 outstanding msgs inside it
    int msgsFactor = m_bVdpauDeinterlacing ? 2 : 1;

    int estimatedPicQLength = iNotFreePics + (msgsFactor * (msgs + 2));
    if (!(estimatedPicQLength >= MAX_PIC_Q_LENGTH || iFreePics < 2))
      return false; // buffers are not full
  }

//CLog::Log(LOGDEBUG,"ASB: CVDPAU::QueueIsFull m_queueSignal.WaitMSec(100)");
  // if we get an event assume there is work to do
  if (wait && reported >= 2 && m_queueSignal.WaitMSec(20))
  {
//CLog::Log(LOGNOTICE,"------------ free: %d, msgs: %d, used: %d, reported: %d"
//       , iFreePics, msgs, usedPics, reported);
    return false;
  }

  return true;
}

void CVDPAU::ClearPicUsedForRender(OutputPicture *pic)
{
  CSingleLock lock(m_videoSurfaceSec);
  if (pic && pic->render)
  {
    if (pic->render->state)
      pic->render->state &= ~FF_VDPAU_STATE_USED_FOR_RENDER;
    pic->render = NULL;
  }
}

void CVDPAU::ClearMsgUsedForRender(MixerMessage &msg)
{
  CSingleLock lock(m_videoSurfaceSec);
  if (msg.render)
    msg.render->state &= ~FF_VDPAU_STATE_USED_FOR_RENDER;
}

int CVDPAU::Decode(AVCodecContext *avctx, AVFrame *pFrame, bool bSoftDrain, bool bHardDrain)
{
  //CLog::Log(LOGNOTICE,"%s",__FUNCTION__);
  VdpStatus vdp_st;
  VdpTime time;
  int retval;

  int result = Check(avctx);
  if (result)
    return result;

  CSharedLock lock(m_DecoderSection);

  if (!vdpauConfigured)
    return VC_ERROR;

  // configure vdpau output
  if (!ConfigOutputMethod(avctx, pFrame))
    return VC_FLUSHED;

  CheckFeatures();

  if (( (int)outRectVid.x1 != OutWidth ) ||
      ( (int)outRectVid.y1 != OutHeight ))
  {
    outRectVid.x0 = 0;
    outRectVid.y0 = 0;
    outRectVid.x1 = OutWidth;
    outRectVid.y1 = OutHeight;
  }

  if (m_dropState && m_vdpauOutputMethod != OUTPUT_GL_INTEROP_YUV)
  {
    CSingleLock lock(m_mixerSec);
    m_mixerCmd |= MIXER_CMD_HURRY;
    m_dropCount++;
  }
  else
    m_dropCount = 0;

  // target usedPics level where we stop holding off from returning VC_PICTURE (to encourage buffering)
  // TODO: ensure m_outPicsNum is at least 4 for mixer or 3 for YUV (2 for presentOut and 1 for usedOut and 1 for mixer usedOut)
  int targetUsed = 0;
  if (m_vdpauOutputMethod == OUTPUT_PIXMAP) //target around 5 if sensible
     //targetUsed = std::min(std::min(m_outPicsNum - NUM_RENDERBUF_PICS - 1, totalAvailableOutputSurfaces), 5);
     targetUsed = std::min(std::min(m_outPicsNum - 1, totalAvailableOutputSurfaces), 5);
  else if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_RGB) //target around 4 if sensible
     //targetUsed = std::min(m_outPicsNum - NUM_RENDERBUF_PICS - 1, 3);
     targetUsed = std::min(m_outPicsNum - 1, 3);
  else //OUTPUT_GL_INTEROP_YUV, target around 3 if sensible
     //targetUsed = std::min(m_outPicsNum - NUM_RENDERBUF_PICS - 1, 3);
     targetUsed = std::min(m_outPicsNum, 3);
  // tweak target by if presentOutPics < 3 or >=4
  { CSingleLock lock(m_outPicSec);
    if (m_presentOutPic.size() < 3)
      --targetUsed; //to try to prevent render getting starved
    else if (m_presentOutPic.size() >= 4)
      --targetUsed; //to throttle back target a little when we have so many presented
  }
  targetUsed = std::max(targetUsed, 1);

  OutputPicture *firstNotReportedPic;
  int usedPics;

  if(pFrame)
  { // we have a new frame from decoder

    vdpau_render_state * render = (vdpau_render_state*)pFrame->data[2];
    if(!render) // old style ffmpeg gave data on plane 0
      render = (vdpau_render_state*)pFrame->data[0];
    if(!render)
    {
      CLog::Log(LOGERROR, "CVDPAU::Decode: no valid frame");
      return VC_ERROR;
    }

    // ffmpeg vc-1 decoder does not flush, make sure the data buffer is still valid
    if (!IsSurfaceValid(render))
    {
      CLog::Log(LOGWARNING, "CVDPAU::Decode - ignoring invalid buffer");
      return VC_BUFFER;
    }

    CSingleLock lock(m_videoSurfaceSec);
    render->state |= FF_VDPAU_STATE_USED_FOR_RENDER;
    lock.Leave();

    if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_YUV)
    {
      CSingleLock lock(m_outPicSec);
      if (m_freeOutPic.empty())
      {
        return VC_ERROR;
      }
      OutputPicture *outPic = m_freeOutPic.front();
      m_freeOutPic.pop_front();
      memset(&outPic->DVDPic, 0, sizeof(DVDVideoPicture));
      ((CDVDVideoCodecFFmpeg*)avctx->opaque)->GetPictureCommon(&outPic->DVDPic);
      outPic->render = render;
      outPic->reported = false;

      outPic->DVDPic.format = DVDVideoPicture::FMT_VDPAU_420;
      outPic->DVDPic.iWidth = OutWidth;
      outPic->DVDPic.iHeight = OutHeight;
      outPic->DVDPic.vdpau = this;
      // tell renderer not to do de-interlacing when using OUTPUT_GL_INTEROP_YUV
      // and no post processing is desired
      if (outPic->DVDPic.iFlags & DVP_FLAG_NOPOSTPROC)
         outPic->DVDPic.iFlags &= ~(DVP_FLAG_TOP_FIELD_FIRST |
                           DVP_FLAG_REPEAT_TOP_FIELD |
                           DVP_FLAG_INTERLACED);

      m_usedOutPic.push_back(outPic);
      lock.Leave();
    }
    else //add a mixer msg
    {
       MixerMessage msg;
       msg.render = render;
       memset(&msg.DVDPic, 0, sizeof(DVDVideoPicture));
       ((CDVDVideoCodecFFmpeg*)avctx->opaque)->GetPictureCommon(&msg.DVDPic);
       msg.outRectVid = outRectVid;

       { CSingleLock lock(m_mixerSec);
         m_mixerMessages.push(msg);
       }
       m_msgSignal.Set();
    }
  }
  // frame adding code completed!

  // now we decide whether to return with a picture ready reported or not, and whether to report wanting more data
  int iter = 0;
  int64_t starttime = CurrentHostCounter();

  if (m_vdpauOutputMethod == OUTPUT_GL_INTEROP_YUV) 
  {
    int retval = 0;
    while (++iter < 2000) //just a failsafe only
    {
      CSingleLock lock(m_outPicSec);
      usedPics = m_usedOutPic.size();

      firstNotReportedPic = 0;
      for (int i = 0; i < 2 && i < usedPics; ++i)
      {
        if (!m_usedOutPic[i]->reported)
        {
          firstNotReportedPic = m_usedOutPic[i];
          break;
        }
      }
      lock.Leave();

      if ((!bSoftDrain) && usedPics < targetUsed && (!QueueIsFull()))
        return VC_BUFFER;

      if (firstNotReportedPic) //we have not reported about this usedPic yet
      {
        firstNotReportedPic->reported = true;
        retval |= VC_PICTURE;
        break;
      }
      //loop again to check to see if GetPicture has been called if asked to drain
      //if (usedPics > 1 && bSoftDrain && iter < 200)
      if (usedPics > 1 && bSoftDrain && 
          CurrentHostCounter() - starttime < (int64_t)50 * CurrentHostFrequency() / 1000 ) //50ms
      {
        usleep(100);
        continue;
      }
      break;
    }

    if (!QueueIsFull())
      retval |= VC_BUFFER;
    return retval;
  }
  // else MIXER based modes from here on

  int msgs;
  retval = 0;
  int msgsFactor = m_bVdpauDeinterlacing ? 2 : 1; //how many usedPics can come out of 1 mixer input msg
  bool dropped = false;
  bool prevNotEmpty = true;
  int noOfReportedPics = 0;
//CLog::Log(LOGDEBUG,"ASB: CVDPAU::Decode bSoftDrain: %i targetUsed: %i m_usedOutPic.size(): %i m_presentOutPic.size(): %i m_mixerMessages.size(): %i", (int)bSoftDrain, targetUsed, m_usedOutPic.size(), m_presentOutPic.size(), m_mixerMessages.size());
  while (++iter < 2000)
  {
    { CSingleLock lock(m_outPicSec);
      usedPics = m_usedOutPic.size();
    }

    // check if we have a picture to return
    if (usedPics > 0)
    {
      CSingleLock lock(m_outPicSec);

      firstNotReportedPic = 0;
      int firstNotReportedPicIndex;
      for (int i = 0; i < 2 && i < usedPics; ++i)
      {
        if (!m_usedOutPic[i]->reported)
        {
          firstNotReportedPic = m_usedOutPic[i];
          firstNotReportedPicIndex = i;
          break;
        }
        else
          noOfReportedPics++;
      }

      if ( firstNotReportedPic &&
           firstNotReportedPic->DVDPic.iFlags & DVP_FLAG_DROPPED)
      {
        //drop this next time around else caller can't report on drops accurately
        if (dropped)
{
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop with already dropped at iter: %i", iter);
          break;
}
        // when using pixmap we can only drop pic with index 0
        if (m_vdpauOutputMethod == OUTPUT_PIXMAP &&
            firstNotReportedPicIndex != 0)
{
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop with not first pic drop at iter: %i", iter);
          break;
}
CLog::Log(LOGNOTICE, "---------------------- vdpau drop");
        m_freeOutPic.push_back(firstNotReportedPic);
        m_usedOutPic.erase(m_usedOutPic.begin()+firstNotReportedPicIndex);
        lock.Leave();
        retval =  VC_DROPPED | VC_PRESENTDROP;
        dropped = true;
        continue;
      }

      lock.Leave();

      // if not asked to drain and not enough data queued then request more data - don't tell caller there is a picture
      // in order to promote pre-buffering.
      if ((!bSoftDrain) && usedPics < targetUsed && (!QueueIsFull()))
      {
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop with no-soft-drain and not teached targetUsed at iter: %i", iter);
        retval |= VC_BUFFER;
        break;
      }

      if (firstNotReportedPic) //we have not reported about this usedPic yet
      {
        if (m_vdpauOutputMethod == OUTPUT_PIXMAP)
        {
          VdpPresentationQueueStatus status;
          VdpTime time;
          VdpStatus vdp_st;
          VdpOutputSurface surface = firstNotReportedPic->outputSurface;

          vdp_st = vdp_presentation_queue_query_surface_status(
                     firstNotReportedPic->vdp_flip_queue,
                     surface, &status, &time);
          CheckStatus(vdp_st, __LINE__);
          if (status == VDP_PRESENTATION_QUEUE_STATUS_VISIBLE && vdp_st == VDP_STATUS_OK)
          {
            // this output surface should now be fully copied to the pixmap presentation
            // but currently we don't allow re-use for other outPics until GetPicture removes this one from usedPic Q

            firstNotReportedPic->reported = true;
            retval |= VC_PICTURE;
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop with VC_PICTURE at iter: %i", iter);
            break;
          }
        }
        else
        {
          firstNotReportedPic->reported = true;
          retval |= VC_PICTURE;
          break;
        }
      }
    }

    // if we are here we have adequate usedPics buffers filled but no picture to give or we have no usedPics at all
    // - we need to catch mixer problems resulting in not delivering any usedPics
    // - and we need to sleep and loop back round to check again 
    //   when draining loop for a relatively long time so that we aim to give a pic at the expense of blocking,
    //   when not draining loop for a relatively short while to avoid our input flow stalling

    // wait if we have enough msgs but don't have an output from mixer
    // if everything is running smooth, we won't wait here

    usedPics -= noOfReportedPics;

    { CSingleLock lock(m_mixerSec);
      msgs = m_mixerMessages.size();
    }

    // if we do not HardDrain msgs should be greater 0 in order to keep
    // mixer going
    if (!bHardDrain && msgs < 1)
{
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop with msgs==0  at iter: %i", iter);
      break;
}

    if (usedPics == 0 && msgs * msgsFactor >= MAX_PIC_Q_LENGTH - 1 && m_picSignal.WaitMSec(200))
    {
      // we got a signal to check msgs & usedPics again
      continue;
    }
    //only don't enter here if previous wait timed out
    if (!(usedPics == 0 && msgs * msgsFactor >= MAX_PIC_Q_LENGTH - 1))
    {
      if (bSoftDrain && !bHardDrain)
      {
        if (usedPics == 0 && m_picSignal.WaitMSec(100))
        {
          continue;
        }
        //else if (iter < 200)
        else if (CurrentHostCounter() - starttime < (int64_t)50 * CurrentHostFrequency() / 1000 ) //50ms
        {
          usleep(100);
          continue;
        }
      }
      else if (bHardDrain)
      {
        //if (usedPics == 0 && msgs == 0 && prevNotEmpty && iter > 100)
        if (usedPics == 0 && msgs == 0 && prevNotEmpty &&
            CurrentHostCounter() - starttime > (int64_t)30 * CurrentHostFrequency() / 1000 ) //30ms
        {
          CSingleLock lock(m_mixerSec);
          m_mixerCmd |= MIXER_CMD_DRAIN;
          lock.Leave();
          m_msgSignal.Set();
          iter = 0; //reset
          prevNotEmpty = false;
        }
        //if (iter < 1000)
        if (CurrentHostCounter() - starttime < (int64_t)200 * CurrentHostFrequency() / 1000 ) //200ms
        {
          if (!(usedPics == 0 && msgs == 0))
            prevNotEmpty = true;
          usleep(100);
          continue;
        }
      }
      //else if (iter < 100)
      //else if (iter < 50)
      else if (CurrentHostCounter() - starttime < (int64_t)10 * CurrentHostFrequency() / 1000 ) //10ms
      {
          // loop back around to check for a picture after a short sleep but break out after a
          // while to protect against starving input to the decoder
          usleep(100);
          continue;
      }
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: break loop at iter: %i", iter);
      break;
    }

    // we still appear to have no usedPics and a lots msgs and the picSignal wait timed out 
    // so we assume mixer thread is having difficulties
    CLog::Log(LOGERROR, "CVDPAU::Decode: timed out waiting for picture, messages: %d, pics %d",
                 msgs, usedPics);
    retval |= VC_ERROR;
    break;
  }// while

  if (!QueueIsFull() || msgs < 1)
     retval |= VC_BUFFER;
//CLog::Log(LOGDEBUG, "ASB: CVDPAU::Decode: return %i iter: %i, now: %"PRId64", starttime: %"PRId64"", retval, iter, CurrentHostCounter(), starttime);
  return retval;
}

bool CVDPAU::DiscardPresentPicture()
{
  // discard most recently added present picture
  return DiscardPicture();
}

bool CVDPAU::GetPicture(AVCodecContext* avctx, AVFrame* frame, DVDVideoPicture* picture)
{
  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return false;
  }

  CSingleLock olock(m_outPicSec);
  if (m_usedOutPic.size() > 0 && m_usedOutPic.front()->reported)
  {
    OutputPicture *pic = m_usedOutPic.front();

    if (m_presentPicture)
    {
      lock.Leave();
      DiscardPicture();
      lock.Enter();
    }
    m_presentPicture = pic;
    m_usedOutPic.pop_front();
    m_presentOutPic.push_back(pic);
    *picture = pic->DVDPic;
  }
  else
  {
    CLog::Log(LOGERROR,"CVDPAU::GetPicture: no picture");
    return false;
  }
  return true;
}

void CVDPAU::Reset()
{
  CSingleLock lockM(m_mixerSec);
  while (!m_mixerMessages.empty())
  {
    MixerMessage &tmp = m_mixerMessages.front();
    ClearMsgUsedForRender(tmp);
    m_mixerMessages.pop();
  }
  m_mixerCmd |= MIXER_CMD_FLUSH;
  lockM.Leave();

  m_msgSignal.Set(); //tell mixer thread to look for our flush cmd
  m_flushSignal.WaitMSec(200); //wait upto 200ms for mixer to flush 
  lockM.Enter();
  if (m_mixerInputSize != 0)
    CLog::Log(LOGERROR, "CVDPAU::Reset - mixer message queue failed to empty after mixer flush request (%i)", m_mixerInputSize);
  lockM.Leave();

  CSingleLock lockO(m_outPicSec);
  if (!m_mixerOutPic.empty())
    CLog::Log(LOGERROR, "CVDPAU::Reset - m_mixerOutPic queue failed to empty after mixer flush request");
  while (!m_usedOutPic.empty())
  {
    OutputPicture *pic = m_usedOutPic.front();

    // make sure there are no queued output surfaces
    int iter = 0;
    while (m_vdpauOutputMethod == OUTPUT_PIXMAP &&
        pic->outputSurface != VDP_INVALID_HANDLE)
    {
      VdpPresentationQueueStatus status;
      VdpTime time;
      VdpStatus vdp_st;

      vdp_st = vdp_presentation_queue_query_surface_status(
                  pic->vdp_flip_queue,
                  pic->outputSurface, &status, &time);
      CheckStatus(vdp_st, __LINE__);
      if(status != VDP_PRESENTATION_QUEUE_STATUS_QUEUED &&
          vdp_st == VDP_STATUS_OK)
      {
        pic->outputSurface = VDP_INVALID_HANDLE;
        break;
      }
      if (++iter < 200)
        Sleep(1);
      else
      {
        CLog::Log(LOGERROR, "CVDPAU::Reset - timed out waiting for output surface");
        pic->outputSurface = VDP_INVALID_HANDLE;
        break;
      }
    }
    ClearPicUsedForRender(pic);
    m_usedOutPic.pop_front();
    m_freeOutPic.push_back(pic);
  }
  // can't really clear down m_presentOutPic as they have probably been handed to renderer 
  // so lets wait a little and report a warning if not cleared by renderer after that time
   
//  int i = 0;
//  while (!m_presentOutPic.empty())
//  {
//    if (i > 10)
//    {
//      CLog::Log(LOGWARNING, "CVDPAU::Reset timed out waiting for renderer to clear down presentOutPic queue m_presentOutPic.size(): %i m_presentOutPic.front(): %u", m_presentOutPic.size(), (unsigned int)m_presentOutPic.front());
//      break;
//    }
//    lockO.Leave();
//    m_picSignal.WaitMSec(20);
//    lockO.Enter();
//    i++;
//  }
//  lockO.Leave();
}

bool CVDPAU::AllowFrameDropping()
{
  if (m_bVdpauDeinterlacing && m_dropCount < 5)
    return false;
  else
    return true;
}

void CVDPAU::SetDropState(bool bDrop)
{
  m_dropState = bDrop;
}


bool CVDPAU::DiscardPicture(int flipBufferIdx /* = -1 */)
{
  OutputPicture *pic = NULL;
  if (flipBufferIdx >= 0)
  {
    // we unpresent the presentPic associated with flipBufferIdx - expect to be front!
    CSingleLock lock(m_flipSec);
    pic = m_flipBuffer[flipBufferIdx];
  }
  else
  {
    // we discard the back presentPic (most recently added)
    CSingleLock lock(m_outPicSec);
    pic = m_presentOutPic.back();
  }

  if (pic)
  {
    ClearPicUsedForRender(pic);
    CSingleLock lock(m_outPicSec);
    if (flipBufferIdx >= 0)
    {
      int presentPicIndex = -1;
      int count = 0;
      for (int i = 0; i < m_presentOutPic.size(); ++i)
      {
        if (m_presentOutPic[i] == pic)
        {
          presentPicIndex = i;
          count++;
          m_presentOutPic.erase(m_presentOutPic.begin() + presentPicIndex);
//          break;
        }
      }
      lock.Leave();
      if (count > 1) //this should not happen ever
        CLog::Log(LOGWARNING, "CVDPAU::DiscardPicture Unexecpectedly discarded %i pics from presentOutPic queue", count);
      else if (count == 0) //this should not happen ever
        CLog::Log(LOGERROR, "CVDPAU::DiscardPicture Failed to locate flip buffer in presentOutPic queue");
      { CSingleLock lock(m_flipSec);
        m_flipBuffer[flipBufferIdx] = NULL;
      }
    }
    else
      m_presentOutPic.pop_back();

    m_freeOutPic.push_back(pic);
    m_queueSignal.Set();
    return true;
  }
  return false;
}

void CVDPAU::Present(int flipBufferIdx)
{
//  CLog::Log(LOGNOTICE,"%s",__FUNCTION__);

  CSharedLock lock(m_DecoderSection);

  { CSharedLock dLock(m_DisplaySection);
    if (m_DisplayState != VDPAU_OPEN)
      return;
  }

  // point flipbuffer at most recently added present picture
  CSingleLock oLock(m_outPicSec);
  if (m_presentOutPic.size() > 0)
  {
    OutputPicture *pic = m_presentOutPic.back();
    lock.Leave();
    if (!pic)
      CLog::Log(LOGNOTICE,"--------------------- no pic");

    { CSingleLock lock(m_flipSec);
      if (m_flipBuffer[flipBufferIdx] != NULL)
      {
        lock.Leave();
        DiscardPicture(flipBufferIdx);
        lock.Enter();
//        CLog::Log(LOGWARNING, "CVDPAU::Present flip buffer being overwritten unexcpectedly, idx: %i", flipBufferIdx);
      }
      m_flipBuffer[flipBufferIdx] = pic;
      m_presentPicture = NULL;
    }
  }
  else
    CLog::Log(LOGWARNING, "CVDPAU::Present Failed to move flip buffer as there is no present picture");
}

bool CVDPAU::CheckStatus(VdpStatus vdp_st, int line)
{
  if (vdp_st != VDP_STATUS_OK)
  {
    CLog::Log(LOGERROR, " (VDPAU) Error: %s(%d) at %s:%d\n", vdp_get_error_string(vdp_st), vdp_st, __FILE__, line);

    CExclusiveLock lock(m_DisplaySection);

    if(m_DisplayState == VDPAU_OPEN)
    {
      if (vdp_st == VDP_STATUS_DISPLAY_PREEMPTED)
        m_DisplayState = VDPAU_LOST;
      else
        m_DisplayState = VDPAU_ERROR;
    }

    return true;
  }
  return false;
}

void CVDPAU::FlushMixer()
{
  // assume called from mixer thread so no need for mixer lock
  { //CSingleLock lock(m_videoSurfaceSec);
    while (!m_mixerInput.empty())
    {
      MixerMessage &tmp = m_mixerInput.front();
      ClearMsgUsedForRender(tmp);
      //tmp.render->state &= ~FF_VDPAU_STATE_USED_FOR_RENDER;
      m_mixerInput.pop_front();
    }
  }
  CSingleLock mixerLock(m_mixerSec);
  m_mixerInputSize = m_mixerInput.size();
}

void CVDPAU::Process()
{
  VdpStatus vdp_st;
  VdpTime time;
  unsigned int cmd;
  bool gotMsg;
  int outputSurfaceNum = 0;

  CSingleLock mixerLock(m_mixerSec);
  mixerLock.Leave();

  while (!m_bStop)
  {
    // wait for message
    gotMsg = false;
    MixerMessage msg;
    cmd = 0;
    m_flushSignal.Reset();

    mixerLock.Enter();
    if (m_mixerCmd & MIXER_CMD_FLUSH)
    {
      cmd = m_mixerCmd;
      m_mixerCmd = 0;
      mixerLock.Leave();
      // flush mixer input queue
      FlushMixer();
      m_flushSignal.Set();
      
      continue;
    }
    else if (!m_mixerMessages.empty())
    {
      msg = m_mixerMessages.front();
      m_mixerMessages.pop();
      m_mixerInput.push_front(msg);
      cmd = m_mixerCmd;
      m_mixerCmd = 0;
      gotMsg = true;
      m_queueSignal.Set(); //to inform interested party that m_mixerMessages has changed
    }
    m_mixerInputSize = m_mixerInput.size();
    mixerLock.Leave();

    // wait for next picture
    if (!gotMsg)
    {
       if (!m_msgSignal.WaitMSec(20))
          ; //CLog::Log(LOGNOTICE, "CVDPAU::Process ------------- wait");
       // check for mixer command update
       mixerLock.Enter();
       cmd |= m_mixerCmd;
       mixerLock.Leave();
       // allow to drain by one if full input, by inserting a null msg
       if ((cmd & MIXER_CMD_DRAIN && m_mixerInput.size() > 2) && !(cmd & MIXER_CMD_FLUSH))
       {
          msg.render = NULL;
          mixerLock.Enter();
          m_mixerInput.push_front(msg);
          m_mixerInputSize = m_mixerInput.size();
          mixerLock.Leave();
          CLog::Log(LOGNOTICE,"%s insert null message for draining", __FUNCTION__);
       }
       else
       {
         continue;
       }
    }

    // need at least 2 frames in the queue
    if (m_mixerInput.size() < 2)
    {
      continue;
    }

    int mixersteps;
    VdpVideoMixerPictureStructure mixerfield;

    EDEINTERLACEMODE   mode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
    EINTERLACEMETHOD method;
    if (mode == VS_DEINTERLACEMODE_FORCE ||
       (mode == VS_DEINTERLACEMODE_AUTO && m_binterlacedFrame))
    {
      method = GetDeinterlacingMethod();
      if((method == VS_INTERLACEMETHOD_AUTO && m_binterlacedFrame)
        ||  method == VS_INTERLACEMETHOD_VDPAU_BOB
        ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL
        ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF
        ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL
        ||  method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF
        ||  method == VS_INTERLACEMETHOD_VDPAU_INVERSE_TELECINE )
      {
        if(method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF
          || method == VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF)
          mixersteps = 1;
        else
          mixersteps = 2;

        if(m_mixerInput[1].DVDPic.iFlags & DVP_FLAG_TOP_FIELD_FIRST)
          mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;
        else
          mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;
      }
    }
    else
    {
      // either progressive or weave de-interlacing (as presented by the decode)
      mixersteps = 1;
      mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME;
    }

    // mixer stage
    for (int mixerstep = 0; mixerstep < mixersteps; mixerstep++)
    {
      if (mixerstep == 1)
      {
        if(mixerfield == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD)
          mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_BOTTOM_FIELD;
        else
          mixerfield = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD;
      }

      VdpVideoSurface past_surfaces[4] = { VDP_INVALID_HANDLE, VDP_INVALID_HANDLE, VDP_INVALID_HANDLE, VDP_INVALID_HANDLE };
      VdpVideoSurface futu_surfaces[2] = { VDP_INVALID_HANDLE, VDP_INVALID_HANDLE };
      uint32_t pastCount = 4;
      uint32_t futuCount = 2;

      VdpRect sourceRect;
      sourceRect.x0 = 0;
      sourceRect.y0 = 0;
      sourceRect.x1 = vid_width;
      sourceRect.y1 = vid_height;

      // handle our null future msg (no surface) created by drain request
      VdpVideoSurface mixerInput0;
      if (m_mixerInput[0].render == NULL)
         mixerInput0 = VDP_INVALID_HANDLE;
      else
         mixerInput0 = m_mixerInput[0].render->surface;

      if(mixerfield == VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME)
      {
        // use only 2 past 1 future for progressive/weave (only used for postproc anyway eg noise reduction)
        if (m_mixerInput.size() > 3)
          past_surfaces[1] = m_mixerInput[3].render->surface;
        if (m_mixerInput.size() > 2)
          past_surfaces[0] = m_mixerInput[2].render->surface;
        futu_surfaces[0] = mixerInput0;
        pastCount = 2;
        futuCount = 1;
      }
      else
      {
        if(mixerstep == 0)
        { // first field
          if (m_mixerInput.size() > 3)
          {
            past_surfaces[3] = m_mixerInput[3].render->surface;
            past_surfaces[2] = m_mixerInput[3].render->surface;
          }
          if (m_mixerInput.size() > 2)
          {
            past_surfaces[1] = m_mixerInput[2].render->surface;
            past_surfaces[0] = m_mixerInput[2].render->surface;
          }
          futu_surfaces[0] = m_mixerInput[1].render->surface;
          futu_surfaces[1] = mixerInput0;
        }
        else
        { // second field
          if (m_mixerInput.size() > 3)
          {
	    past_surfaces[3] = m_mixerInput[3].render->surface;
          }
          if (m_mixerInput.size() > 2)
          {
	    past_surfaces[2] = m_mixerInput[2].render->surface;
            past_surfaces[1] = m_mixerInput[2].render->surface;
          }
          past_surfaces[0] = m_mixerInput[1].render->surface;
          futu_surfaces[0] = mixerInput0;
          futu_surfaces[1] = mixerInput0;
        }
      }

      // get free pic from queue
      OutputPicture *outPic = NULL;
      while (!outPic && !m_bStop)
      {
        { CSingleLock outPicLock(m_outPicSec);
          // make sure not to overwrite an output surface
          if (!(m_vdpauOutputMethod == OUTPUT_PIXMAP && m_usedOutPic.size() >= totalAvailableOutputSurfaces) &&
              !m_freeOutPic.empty())
          {
            outPic = m_freeOutPic.front();
            m_freeOutPic.pop_front();
            outPic->render = NULL;
            outPic->reported = false;
            m_mixerOutPic.push_back(outPic);
            break;
          }
        }

        Sleep(1);
        // if we are waiting for an output surface to become available
        // but player has stopped consuming we will get stuck here
        // - we will allow escape via Flush check so that Reset is able to 
        //   to get us to flush at least
        mixerLock.Enter();
        cmd = m_mixerCmd;
        mixerLock.Leave();
        if (cmd & MIXER_CMD_FLUSH)
           break; //to get flush processed
      }
      if (!outPic)
        break;

      // set pts / dts for interlaced pic
      outPic->DVDPic = m_mixerInput[1].DVDPic;
      if (mixerfield != VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME
          && method != VS_INTERLACEMETHOD_VDPAU_TEMPORAL_HALF
          && method != VS_INTERLACEMETHOD_VDPAU_TEMPORAL_SPATIAL_HALF)
      {
        //TODO: keep an eye on this iRepeatPicture stuff as in theory does not have to be just 1 repeat
        //      and also the pts interpolation might be better done by player after it has looked for pts patterns
        outPic->DVDPic.iRepeatPicture = -0.5;
        if (mixerstep == 1 &&
            m_mixerInput[1].DVDPic.pts != DVD_NOPTS_VALUE && 
            m_mixerInput[0].DVDPic.pts != DVD_NOPTS_VALUE)
        {
          double pts = m_mixerInput[1].DVDPic.pts +
              (m_mixerInput[0].DVDPic.pts - m_mixerInput[1].DVDPic.pts)/2;
          outPic->DVDPic.dts = pts; 
          outPic->DVDPic.pts = pts;
        }
      }

      outPic->DVDPic.format = DVDVideoPicture::FMT_VDPAU;
      outPic->DVDPic.iWidth = OutWidth;
      outPic->DVDPic.iHeight = OutHeight;
      outPic->DVDPic.vdpau = this;
      // when using mixer no de-interlacing should be done by renderer. VDPAU doesn't expose fields in this case
      outPic->DVDPic.iFlags &= ~(DVP_FLAG_TOP_FIELD_FIRST |
                           DVP_FLAG_REPEAT_TOP_FIELD |
                           DVP_FLAG_INTERLACED);
      if (outPic->DVDPic.iFlags & DVP_FLAG_NOPOSTPROC)
          SetPostProcFeatures(false);
      else
          SetPostProcFeatures();

      // skip mixer step if requested
      if (cmd & MIXER_CMD_HURRY ||
          (outPic->DVDPic.iFlags & DVP_FLAG_DROPPED))
      {
        outPic->DVDPic.iFlags |= DVP_FLAG_DROPPED;
        if (m_vdpauOutputMethod == OUTPUT_PIXMAP)
          outPic->outputSurface = VDP_INVALID_HANDLE;
      }
      else
      {
        if (m_vdpauOutputMethod == OUTPUT_PIXMAP)
        {
          outPic->outputSurface = outputSurfaces[outputSurfaceNum];
          outputSurfaceNum = (outputSurfaceNum +1) % totalAvailableOutputSurfaces;
        }

        // start vdpau video mixer
        vdp_st = vdp_video_mixer_render(videoMixer,
                                VDP_INVALID_HANDLE,
                                0,
                                mixerfield,
                                pastCount,
                                past_surfaces,
                                m_mixerInput[1].render->surface,
                                futuCount,
                                futu_surfaces,
                                &sourceRect,
                                outPic->outputSurface,
                                &(m_mixerInput[1].outRectVid),
                                &(m_mixerInput[1].outRectVid),
                                0,
                                NULL);
        CheckStatus(vdp_st, __LINE__);

        if (mixerfield != VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME)
        {
          // in order to clip top and bottom lines when de-interlacing
          // we black those lines as a work around for not working
          // background colour using the mixer
          // pixel perfect is preferred over overscanning or zooming

          VdpRect clipRect = m_mixerInput[1].outRectVid;
          clipRect.y1 = clipRect.y0 + 2;
          uint32_t *data[] = {m_BlackBar};
          uint32_t pitches[] = {m_mixerInput[1].outRectVid.x1};
          vdp_st = vdp_output_surface_put_bits_native(outPic->outputSurface,
                                            (void**)data,
                                            pitches,
                                            &clipRect);
          CheckStatus(vdp_st, __LINE__);

          clipRect = m_mixerInput[1].outRectVid;
          clipRect.y0 = clipRect.y1 - 2;
          vdp_st = vdp_output_surface_put_bits_native(outPic->outputSurface,
                                            (void**)data,
                                            pitches,
                                            &clipRect);
          CheckStatus(vdp_st, __LINE__);
        }

        if (m_vdpauOutputMethod == OUTPUT_PIXMAP)
        {
          vdp_st = vdp_presentation_queue_display(outPic->vdp_flip_queue,
                                                outPic->outputSurface,
                                                0,
                                                0,
                                                0);
          CheckStatus(vdp_st, __LINE__);
        }
      }

      // put mixer outpic in used out queue
      { CSingleLock outPicLock(m_outPicSec);
        m_mixerOutPic.pop_front();
        m_usedOutPic.push_back(outPic);
      }

      //tell other threads there is (or may be for pixmap) a picture ready
      m_picSignal.Set();

      // mixing could have taken a while, check for new command
      mixerLock.Enter();
      cmd = m_mixerCmd;
      mixerLock.Leave();

      if (cmd & MIXER_CMD_FLUSH)
        break;

    }// for (mixer stage)
    // if we added a null msg for drain purpoes then we we should throw out all mixer input except current (index=1)
    if (m_mixerInput[0].render == NULL)
    {
       m_mixerInput.pop_front();
       while (m_mixerInput.size() > 1)
          m_mixerInput.pop_back();
       mixerLock.Enter();
       m_mixerInputSize = m_mixerInput.size();
       mixerLock.Leave();
    }
    while (m_mixerInput.size() > 3)
    {
      MixerMessage &tmp = m_mixerInput.back();
      ClearMsgUsedForRender(tmp);
      m_mixerInput.pop_back();
      mixerLock.Enter();
      m_mixerInputSize = m_mixerInput.size();
      mixerLock.Leave();
    }
  }//while not stop
}

void CVDPAU::OnStartup()
{
  CLog::Log(LOGNOTICE, "CVDPAU::OnStartup: Mixer Thread created");
}

void CVDPAU::OnExit()
{
  CLog::Log(LOGNOTICE, "CVDPAU::OnExit: Mixer Thread terminated");
}

void CVDPAU::GLFinish()
{
  //TODO: look into this more to see if something is wrong here that could be causing graphics resources to not completely clear

#ifdef GL_NV_vdpau_interop
  GLFiniInterop();
#endif
  for (int i=0; i < m_outPicsNum; i++)
  {
    GLXPixmap glPixmap = m_allOutPic[i].glPixmap;
    if (glPixmap && m_allOutPic[i].bound)
    {
      glXReleaseTexImageEXT(m_Display, glPixmap, GLX_FRONT_LEFT_EXT);
      m_allOutPic[i].bound = false;
    }
    if (glIsTexture(m_allOutPic[i].texture[0]))
    {
      glDeleteTextures(1, m_allOutPic[i].texture);
    }
  }
  CLog::Log(LOGNOTICE, "CVDPAU::GLFinish: cleared down gl resources");
}

#ifdef GL_NV_vdpau_interop
void CVDPAU::GLInitInterop()
{
  m_renderThread = CThread::GetCurrentThreadId();

  while (glGetError() != GL_NO_ERROR) ;
  glVDPAUInitNV((GLvoid*)vdp_device, (GLvoid*)vdp_get_proc_address);
  if (glGetError() != GL_NO_ERROR)
  {
    CLog::Log(LOGERROR, "CVDPAU::GLInitInterop glVDPAUInitNV failed");
  }

  glInteropFinish = false;
  CLog::Log(LOGNOTICE, "CVDPAU::GlInitInterop: gl interop initialized");
}

void CVDPAU::GLFiniInterop()
{
  if (m_GlInteropStatus == OUTPUT_NONE)
  {
    glInteropFinish = false;
    return;
  }

  glVDPAUFiniNV();

  for (int i=0; i < m_outPicsNum; i++)
  {
    if (glIsTexture(m_allOutPic[i].texture[0]))
    {
      glVDPAUUnregisterSurfaceNV(m_allOutPic[i].glVdpauSurface);
      glDeleteTextures(1, m_allOutPic[i].texture);
    }
  }
  std::map<VdpVideoSurface, GLVideoSurface>::iterator it;
  for (it = m_videoSurfaceMap.begin(); it != m_videoSurfaceMap.end(); ++it)
  {
    glVDPAUUnregisterSurfaceNV(it->second.glVdpauSurface);
    glDeleteTextures(4, it->second.texture);
  }
  m_videoSurfaceMap.clear();

  m_GlInteropStatus = OUTPUT_NONE;
  glInteropFinish = false;
  CLog::Log(LOGNOTICE, "CVDPAU::GlFiniInterop: gl interop finished");
}

bool CVDPAU::GLMapSurface(OutputPicture *outPic)
{
  bool bReturn = true;
  if (outPic->DVDPic.format == DVDVideoPicture::FMT_VDPAU)
  {
    if (m_GlInteropStatus != OUTPUT_GL_INTEROP_RGB)
    {
      GLInitInterop();
      bReturn = GLRegisterOutputSurfaces();
      m_GlInteropStatus = OUTPUT_GL_INTEROP_RGB;
    }
//    glVDPAUMapSurfacesNV(1, &outPic->glVdpauSurface);
  }
  else if (outPic->DVDPic.format == DVDVideoPicture::FMT_VDPAU_420)
  {
    if (m_GlInteropStatus != OUTPUT_GL_INTEROP_YUV)
    {
      GLInitInterop();
      m_GlInteropStatus = OUTPUT_GL_INTEROP_YUV;
    }
    bReturn = GLRegisterVideoSurfaces(outPic);
    if (bReturn)
    {
      GLVideoSurface surface = m_videoSurfaceMap[outPic->render->surface];
      for (int i = 0; i < 4; i++)
        outPic->texture[i] = surface.texture[i];
    }
  }
  return bReturn;
}

bool CVDPAU::GLUnmapSurface(OutputPicture *outPic)
{
  if (outPic->DVDPic.format == DVDVideoPicture::FMT_VDPAU)
  {
    glVDPAUUnmapSurfacesNV(1, &outPic->glVdpauSurface);
  }
  else if (outPic->DVDPic.format == DVDVideoPicture::FMT_VDPAU_420)
  {
    GLVideoSurface surface = m_videoSurfaceMap[outPic->render->surface];
    glVDPAUUnmapSurfacesNV(1, &surface.glVdpauSurface);
  }
  return true;
}

bool CVDPAU::GLRegisterOutputSurfaces()
{
  for (int i=0; i<m_outPicsNum;i++)
  {
    glGenTextures(1, m_allOutPic[i].texture);
    m_allOutPic[i].glVdpauSurface = glVDPAURegisterOutputSurfaceNV((GLvoid*)m_allOutPic[i].outputSurface,
                                               GL_TEXTURE_2D, 1, m_allOutPic[i].texture);
    if (glGetError() != GL_NO_ERROR)
    {
      CLog::Log(LOGERROR, "CVDPAU::GLRegisterOutputSurfaces error register output surface");
      return false;
    }
    glVDPAUSurfaceAccessNV(m_allOutPic[i].glVdpauSurface, GL_READ_ONLY);
    if (glGetError() != GL_NO_ERROR)
    {
      CLog::Log(LOGERROR, "CVDPAU::GLRegisterOutputSurfaces error setting access");
      return false;
    }
    glVDPAUMapSurfacesNV(1, &m_allOutPic[i].glVdpauSurface);
    if (glGetError() != GL_NO_ERROR)
    {
      CLog::Log(LOGERROR, "CVDPAU::GLRegisterOutputSurfaces error mapping surface");
      return false;
    }
  }
  return true;
}

bool CVDPAU::GLRegisterVideoSurfaces(OutputPicture *outPic)
{
  CSingleLock lock(m_videoSurfaceSec);
  bool bError = false;
  if (m_videoSurfaces.size() != m_videoSurfaceMap.size())
  {
    for (int i = 0; i < m_videoSurfaces.size(); i++)
    {
      if (m_videoSurfaceMap.find(m_videoSurfaces[i]->surface) == m_videoSurfaceMap.end())
      {
        GLVideoSurface glVideoSurface;
        while (glGetError() != GL_NO_ERROR) ;
        glGenTextures(4, glVideoSurface.texture);
        if (glGetError() != GL_NO_ERROR)
        {
           CLog::Log(LOGERROR, "CVDPAU::GLRegisterVideoSurfaces error creating texture");
           bError = true;
        }
        glVideoSurface.glVdpauSurface = glVDPAURegisterVideoSurfaceNV((GLvoid*)(m_videoSurfaces[i]->surface),
                                                  GL_TEXTURE_2D, 4, glVideoSurface.texture);

        if (glGetError() != GL_NO_ERROR)
        {
          CLog::Log(LOGERROR, "CVDPAU::GLRegisterVideoSurfaces error register video surface");
          bError = true;
        }
        glVDPAUSurfaceAccessNV(glVideoSurface.glVdpauSurface, GL_READ_ONLY);
        if (glGetError() != GL_NO_ERROR)
        {
          CLog::Log(LOGERROR, "CVDPAU::GLRegisterVideoSurfaces error setting access");
          bError = true;
        }
        glVDPAUMapSurfacesNV(1, &glVideoSurface.glVdpauSurface);
        if (glGetError() != GL_NO_ERROR)
        {
          CLog::Log(LOGERROR, "CVDPAU::GLRegisterVideoSurfaces error mapping surface");
          bError = true;
        }
        m_videoSurfaceMap[m_videoSurfaces[i]->surface] = glVideoSurface;
        if (bError)
          return false;
        CLog::Log(LOGNOTICE, "CVDPAU::GLRegisterVideoSurfaces registered surface");
      }
    }
  }
  return true;
}

#endif

GLuint CVDPAU::GLGetSurfaceTexture(int plane, int field, int flipBufferIdx)
{
  GLuint glReturn = 0;

#ifdef GL_NV_vdpau_interop

  //check if current output method is valid
  if (m_GlInteropStatus != m_vdpauOutputMethod)
  {
    glInteropFinish = true;
  }

  // check for request to finish interop
  if (glInteropFinish)
  {
     GLFiniInterop();
  }

  // register and map surface
  if (m_flipBuffer[flipBufferIdx])
  {
    if (!GLMapSurface(m_flipBuffer[flipBufferIdx]))
    {
      glInteropFinish = true;
      return 0;
    }
    if (plane == 0 && (field == 0))
    {
      glReturn = m_flipBuffer[flipBufferIdx]->texture[0];
    }
    else if (plane == 0 && (field == 1))
    {
      glReturn = m_flipBuffer[flipBufferIdx]->texture[0];
    }
    else if (plane == 1 && (field == 1))
    {
      glReturn = m_flipBuffer[flipBufferIdx]->texture[2];
    }
    else if (plane == 0 && (field == 2))
    {
      glReturn = m_flipBuffer[flipBufferIdx]->texture[1];
    }
    else if (plane == 1 && (field == 2))
    {
      glReturn = m_flipBuffer[flipBufferIdx]->texture[3];
    }
  }
  else
    CLog::Log(LOGWARNING, "CVDPAU::GLGetSurfaceTexture - no picture, index %d", flipBufferIdx);

#endif

  return glReturn;
}

//long CVDPAU::Release()
//{
//#ifdef GL_NV_vdpau_interop
//  if (m_renderThread == CThread::GetCurrentThreadId())
//  {
////    InterlockedIncrement(&m_references);
////    long count = InterlockedDecrement(&m_references);
////    if (count < 2)
////    {
////      CLog::Log(LOGNOTICE, "CVDPAU::Release");
////      GLFiniInterop();
////    }
//  }
//#endif
//  return CDVDVideoCodecFFmpeg::IHardwareDecoder::Release();
//}
#endif
