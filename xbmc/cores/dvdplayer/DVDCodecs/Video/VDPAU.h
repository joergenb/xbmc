#pragma once
/*
 *      Copyright (C) 2005-2009 Team XBMC
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

#include "DllAvUtil.h"
#include "DVDVideoCodec.h"
#include "DVDVideoCodecFFmpeg.h"
#include "libavcodec/vdpau.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define GL_GLEXT_PROTOTYPES
#define GLX_GLXEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <queue>
#include "threads/CriticalSection.h"
#include "threads/SharedSection.h"
#include "settings/VideoSettings.h"
#include "guilib/DispResource.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "utils/ActorProtocol.h"

using namespace Actor;


#define FULLHD_WIDTH                       1920
#define MAX_PIC_Q_LENGTH                   20 //for non-interop_yuv this controls the max length of the decoded pic to render completion Q

namespace VDPAU
{

//-----------------------------------------------------------------------------
// vdpau functions
//-----------------------------------------------------------------------------

struct VDPAU_procs
{
  VdpGetProcAddress *                   vdp_get_proc_address;
  VdpDeviceDestroy *                    vdp_device_destroy;

  VdpVideoSurfaceCreate *               vdp_video_surface_create;
  VdpVideoSurfaceDestroy *              vdp_video_surface_destroy;
  VdpVideoSurfacePutBitsYCbCr *         vdp_video_surface_put_bits_y_cb_cr;
  VdpVideoSurfaceGetBitsYCbCr *         vdp_video_surface_get_bits_y_cb_cr;

  VdpOutputSurfacePutBitsYCbCr *        vdp_output_surface_put_bits_y_cb_cr;
  VdpOutputSurfacePutBitsNative *       vdp_output_surface_put_bits_native;
  VdpOutputSurfaceCreate *              vdp_output_surface_create;
  VdpOutputSurfaceDestroy *             vdp_output_surface_destroy;
  VdpOutputSurfaceGetBitsNative *       vdp_output_surface_get_bits_native;
  VdpOutputSurfaceRenderOutputSurface * vdp_output_surface_render_output_surface;
  VdpOutputSurfacePutBitsIndexed *      vdp_output_surface_put_bits_indexed;

  VdpVideoMixerCreate *                 vdp_video_mixer_create;
  VdpVideoMixerSetFeatureEnables *      vdp_video_mixer_set_feature_enables;
  VdpVideoMixerQueryParameterSupport *  vdp_video_mixer_query_parameter_support;
  VdpVideoMixerQueryFeatureSupport *    vdp_video_mixer_query_feature_support;
  VdpVideoMixerDestroy *                vdp_video_mixer_destroy;
  VdpVideoMixerRender *                 vdp_video_mixer_render;
  VdpVideoMixerSetAttributeValues *     vdp_video_mixer_set_attribute_values;

  VdpGenerateCSCMatrix *                vdp_generate_csc_matrix;

  VdpPresentationQueueTargetDestroy *         vdp_presentation_queue_target_destroy;
  VdpPresentationQueueCreate *                vdp_presentation_queue_create;
  VdpPresentationQueueDestroy *               vdp_presentation_queue_destroy;
  VdpPresentationQueueDisplay *               vdp_presentation_queue_display;
  VdpPresentationQueueBlockUntilSurfaceIdle * vdp_presentation_queue_block_until_surface_idle;
  VdpPresentationQueueTargetCreateX11 *       vdp_presentation_queue_target_create_x11;
  VdpPresentationQueueQuerySurfaceStatus *    vdp_presentation_queue_query_surface_status;
  VdpPresentationQueueGetTime *               vdp_presentation_queue_get_time;

  VdpGetErrorString *                         vdp_get_error_string;

  VdpDecoderCreate *             vdp_decoder_create;
  VdpDecoderDestroy *            vdp_decoder_destroy;
  VdpDecoderRender *             vdp_decoder_render;
  VdpDecoderQueryCapabilities *  vdp_decoder_query_caps;

  VdpPreemptionCallbackRegister * vdp_preemption_callback_register;

};

//-----------------------------------------------------------------------------
// VDPAU data structs
//-----------------------------------------------------------------------------

class CDecoder;

#define NUM_RENDER_PICS 8

class CVdpauBufferStats
{
public:
  uint16_t decodedPics;
  uint16_t processedPics;
  uint16_t renderPics;
  uint64_t latency;

  void IncDecoded() { CSingleLock l(m_sec); decodedPics++;}
  void DecDecoded() { CSingleLock l(m_sec); decodedPics--;}
  void IncProcessed() { CSingleLock l(m_sec); processedPics++;}
  void DecProcessed() { CSingleLock l(m_sec); processedPics--;}
  void IncRender() { CSingleLock l(m_sec); renderPics++;}
  void DecRender() { CSingleLock l(m_sec); renderPics--;}
  void Reset() { CSingleLock l(m_sec); decodedPics=0; processedPics=0;renderPics=0;latency=0;}
  void Get(uint16_t &decoded, uint16_t &processed, uint16_t &render) {CSingleLock l(m_sec); decoded = decodedPics, processed=processedPics, render=renderPics;}
  void SetLatency(uint64_t time) { CSingleLock l(m_sec); latency = time; }
  uint64_t GetLatency() { CSingleLock l(m_sec); return latency; }
private:
  CCriticalSection m_sec;
};

struct CVdpauConfig
{
  int surfaceWidth;
  int surfaceHeight;
  int vidWidth;
  int vidHeight;
  int outWidth;
  int outHeight;
  VDPAU_procs vdpProcs;
  VdpDevice vdpDevice;
  VdpDecoder vdpDecoder;
  VdpChromaType vdpChromaType;
  CVdpauBufferStats *stats;
  CDecoder *vdpau;
  int featureCount;
  int upscale;
  VdpVideoMixerFeature vdpFeatures[10];
  std::vector<vdpau_render_state*> *videoSurfaces;
  CCriticalSection *videoSurfaceSec;
  bool usePixmaps;
  int numRenderBuffers;
  uint32_t maxReferences;
  bool useInteropYuv;
};

struct CVdpauDecodedPicture
{
  DVDVideoPicture DVDPic;
  vdpau_render_state *render;
};

struct CVdpauProcessedPicture
{
  DVDVideoPicture DVDPic;
  vdpau_render_state *render;
  VdpOutputSurface outputSurface;
  uint8_t numDecodedPics;
};

class CVdpauRenderPicture
{
  friend class CDecoder;
  friend class COutput;
public:
  DVDVideoPicture DVDPic;
  GLuint texture[4];
  uint32_t sourceIdx;
  bool valid;
  CDecoder *vdpau;
  CVdpauRenderPicture* Acquire();
  long Release();
private:
  void ReturnUnused();
  int refCount;
  CCriticalSection *renderPicSection;
};

//-----------------------------------------------------------------------------
// Mixer
//-----------------------------------------------------------------------------

class CMixerControlProtocol : public Protocol
{
public:
  CMixerControlProtocol(std::string name, CEvent* inEvent, CEvent *outEvent) : Protocol(name, inEvent, outEvent) {};
  enum OutSignal
  {
    INIT = 0,
    FLUSH,
    TIMEOUT,
  };
  enum InSignal
  {
    ACC,
    ERROR,
  };
};

class CMixerDataProtocol : public Protocol
{
public:
  CMixerDataProtocol(std::string name, CEvent* inEvent, CEvent *outEvent) : Protocol(name, inEvent, outEvent) {};
  enum OutSignal
  {
    FRAME,
    BUFFER,
  };
  enum InSignal
  {
    PICTURE,
  };
};

class CMixer : private CThread
{
public:
  CMixer(CEvent *inMsgEvent);
  virtual ~CMixer();
  void Start();
  void Dispose();
  CMixerControlProtocol m_controlPort;
  CMixerDataProtocol m_dataPort;
protected:
  void OnStartup();
  void OnExit();
  void Process();
  void StateMachine(int signal, Protocol *port, Message *msg);
  void Init();
  void Uninit();
  void Flush();
  void CreateVdpauMixer();
  void ProcessPicture();
  void InitCycle();
  void FiniCycle();
  void CheckFeatures();
  void SetPostProcFeatures(bool postProcEnabled);
  void PostProcOff();
  void InitCSCMatrix(int Width);
  bool GenerateStudioCSCMatrix(VdpColorStandard colorStandard, VdpCSCMatrix &studioCSCMatrix);
  void SetColor();
  void SetNoiseReduction();
  void SetSharpness();
  void SetDeintSkipChroma();
  void SetDeinterlacing();
  void SetHWUpscaling();
  void DisableHQScaling();
  EINTERLACEMETHOD GetDeinterlacingMethod(bool log = false);
  bool CheckStatus(VdpStatus vdp_st, int line);
  CEvent m_outMsgEvent;
  CEvent *m_inMsgEvent;
  int m_state;
  bool m_bStateMachineSelfTrigger;

  // extended state variables for state machine
  int m_extTimeout;
  bool m_vdpError;
  CVdpauConfig m_config;
  VdpVideoMixer m_videoMixer;
  VdpProcamp m_Procamp;
  VdpCSCMatrix  m_CSCMatrix;
  bool m_PostProc;
  float m_Brightness;
  float m_Contrast;
  float m_NoiseReduction;
  float m_Sharpness;
  int m_DeintMode;
  int m_Deint;
  int m_Upscale;
  uint32_t *m_BlackBar;
  VdpVideoMixerPictureStructure m_mixerfield;
  int m_mixerstep;
  int m_mixersteps;
  CVdpauProcessedPicture m_processPicture;
  std::queue<VdpOutputSurface> m_outputSurfaces;
  std::queue<CVdpauDecodedPicture> m_decodedPics;
  std::deque<CVdpauDecodedPicture> m_mixerInput;
};

//-----------------------------------------------------------------------------
// Output
//-----------------------------------------------------------------------------

struct VdpauBufferPool
{
  struct Pixmaps
  {
    unsigned short id;
    bool used;
    DVDVideoPicture DVDPic;
    GLuint texture;
    Pixmap pixmap;
    GLXPixmap  glPixmap;
    VdpPresentationQueueTarget vdp_flip_target;
    VdpPresentationQueue vdp_flip_queue;
    VdpOutputSurface surface;
  };
  struct GLVideoSurface
  {
    GLuint texture[4];
#ifdef GL_NV_vdpau_interop
    GLvdpauSurfaceNV glVdpauSurface;
#endif
    vdpau_render_state *sourceVuv;
    VdpOutputSurface sourceRgb;
  };
  unsigned short numOutputSurfaces;
  std::vector<Pixmaps> pixmaps;
  std::vector<VdpOutputSurface> outputSurfaces;
  std::deque<Pixmaps*> notVisiblePixmaps;
  std::vector<CVdpauRenderPicture> allRenderPics;
  std::map<VdpVideoSurface, GLVideoSurface> glVideoSurfaceMap;
  std::map<VdpOutputSurface, GLVideoSurface> glOutputSurfaceMap;
  std::queue<CVdpauProcessedPicture> processedPics;
  std::deque<CVdpauRenderPicture*> usedRenderPics;
  std::deque<CVdpauRenderPicture*> freeRenderPics;
  CCriticalSection renderPicSec;
};

class COutputControlProtocol : public Protocol
{
public:
  COutputControlProtocol(std::string name, CEvent* inEvent, CEvent *outEvent) : Protocol(name, inEvent, outEvent) {};
  enum OutSignal
  {
    INIT,
    FLUSH,
    PRECLEANUP,
    TIMEOUT,
  };
  enum InSignal
  {
    ACC,
    ERROR,
    STATS,
  };
};


class COutputDataProtocol : public Protocol
{
public:
  COutputDataProtocol(std::string name, CEvent* inEvent, CEvent *outEvent) : Protocol(name, inEvent, outEvent) {};
  enum OutSignal
  {
    NEWFRAME = 0,
    RETURNPIC,
  };
  enum InSignal
  {
    PICTURE,
  };
};


class COutput : private CThread
{
public:
  COutput(CEvent *inMsgEvent);
  virtual ~COutput();
  void Start();
  void Dispose();
  COutputControlProtocol m_controlPort;
  COutputDataProtocol m_dataPort;
protected:
  void OnStartup();
  void OnExit();
  void Process();
  void StateMachine(int signal, Protocol *port, Message *msg);
  bool HasWork();
  CVdpauRenderPicture *ProcessMixerPicture();
  void ProcessReturnPicture(CVdpauRenderPicture *pic);
  int FindFreePixmap();
  bool Init();
  bool Uninit();
  void Flush();
  bool CreateGlxContext();
  bool DestroyGlxContext();
  bool EnsureBufferPool();
  void ReleaseBufferPool();
  void InitMixer();
  bool GLInit();
  void GLMapSurfaces();
  void GLUnmapSurfaces();
  void GLBindPixmaps();
  void GLUnbindPixmaps();
  bool MakePixmap(VdpauBufferPool::Pixmaps &pixmap);
  bool MakePixmapGL(VdpauBufferPool::Pixmaps &pixmap);
  bool CheckStatus(VdpStatus vdp_st, int line);
  CEvent m_outMsgEvent;
  CEvent *m_inMsgEvent;
  int m_state;
  bool m_bStateMachineSelfTrigger;

  // extended state variables for state machine
  int m_extTimeout;
  bool m_vdpError;
  CVdpauConfig m_config;
  VdpauBufferPool m_bufferPool;
  CMixer m_mixer;
  Display *m_Display;
  Window m_Window;
  GLXContext m_glContext;
  GLXWindow m_glWindow;
  Pixmap    m_pixmap;
  GLXPixmap m_glPixmap;

  // gl functions
  PFNGLXBINDTEXIMAGEEXTPROC    glXBindTexImageEXT;
  PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT;
#ifdef GL_NV_vdpau_interop
  PFNGLVDPAUINITNVPROC glVDPAUInitNV;
  PFNGLVDPAUFININVPROC glVDPAUFiniNV;
  PFNGLVDPAUREGISTEROUTPUTSURFACENVPROC glVDPAURegisterOutputSurfaceNV;
  PFNGLVDPAUREGISTERVIDEOSURFACENVPROC glVDPAURegisterVideoSurfaceNV;
  PFNGLVDPAUISSURFACENVPROC glVDPAUIsSurfaceNV;
  PFNGLVDPAUUNREGISTERSURFACENVPROC glVDPAUUnregisterSurfaceNV;
  PFNGLVDPAUSURFACEACCESSNVPROC glVDPAUSurfaceAccessNV;
  PFNGLVDPAUMAPSURFACESNVPROC glVDPAUMapSurfacesNV;
  PFNGLVDPAUUNMAPSURFACESNVPROC glVDPAUUnmapSurfacesNV;
  PFNGLVDPAUGETSURFACEIVNVPROC glVDPAUGetSurfaceivNV;
#endif
};

//-----------------------------------------------------------------------------
// VDPAU decoder
//-----------------------------------------------------------------------------

class CDecoder
 : public CDVDVideoCodecFFmpeg::IHardwareDecoder
 , public IDispResource
{
   friend class CVdpauRenderPicture;

public:

  struct PictureAge
  {
    int b_age;
    int ip_age[2];
  };

  struct Desc
  {
    const char *name;
    uint32_t id;
    uint32_t aux; /* optional extra parameter... */
  };

  CDecoder();
  virtual ~CDecoder();

  virtual bool Open      (AVCodecContext* avctx, const enum PixelFormat, unsigned int surfaces = 0);
  virtual int  Decode    (AVCodecContext* avctx, AVFrame* frame) {return Decode(avctx, frame, false, false);};
  virtual int  Decode    (AVCodecContext* avctx, AVFrame* frame, bool bSoftDrain = false, bool bHardDrain = false);
  virtual bool GetPicture(AVCodecContext* avctx, AVFrame* frame, DVDVideoPicture* picture);
  virtual void Reset();
  virtual void Close();
  virtual long Release();
  virtual bool AllowFrameDropping();
  virtual void SetDropState(bool bDrop);

  virtual int  Check(AVCodecContext* avctx);
  virtual const std::string Name() { return "vdpau"; }

  bool Supports(VdpVideoMixerFeature feature);
  bool Supports(EINTERLACEMETHOD method);
  EINTERLACEMETHOD AutoInterlaceMethod();
  static bool IsVDPAUFormat(PixelFormat fmt);

  static void FFReleaseBuffer(AVCodecContext *avctx, AVFrame *pic);
  static void FFDrawSlice(struct AVCodecContext *s,
                          const AVFrame *src, int offset[4],
                          int y, int type, int height);
  static int FFGetBuffer(AVCodecContext *avctx, AVFrame *pic);

  virtual void OnLostDevice();
  virtual void OnResetDevice();

protected:
  void SetWidthHeight(int width, int height);
  bool ConfigVDPAU(AVCodecContext *avctx, int ref_frames);
  void SpewHardwareAvailable();
  bool CheckStatus(VdpStatus vdp_st, int line);
  bool IsSurfaceValid(vdpau_render_state *render);
  void InitVDPAUProcs();
  void FiniVDPAUProcs();
  void FiniVDPAUOutput();
  void ReturnRenderPicture(CVdpauRenderPicture *renderPic);

  static void ReadFormatOf( PixelFormat fmt
                          , VdpDecoderProfile &decoder_profile
                          , VdpChromaType     &chroma_type);

  VdpStatus (*dl_vdp_device_create_x11)(Display* display, int screen, VdpDevice* device, VdpGetProcAddress **get_proc_address);
  VdpStatus (*dl_vdp_get_proc_address)(VdpDevice device, VdpFuncId function_id, void** function_pointer);
  VdpStatus (*dl_vdp_preemption_callback_register)(VdpDevice device, VdpPreemptionCallback callback, void* context);

  // OnLostDevice triggers transition from all states to LOST
  // internal errors trigger transition from OPEN to RESET
  // OnResetDevice triggers transition from LOST to RESET
  enum EDisplayState
  { VDPAU_OPEN
  , VDPAU_RESET
  , VDPAU_LOST
  , VDPAU_ERROR
  } m_DisplayState;
  CCriticalSection m_DecoderSection;
  CEvent         m_DisplayEvent;

  static void*  dl_handle;
  DllAvUtil     m_dllAvUtil;
  Display*      m_Display;
  ThreadIdentifier m_decoderThread;
  bool          m_vdpauConfigured;
  CVdpauConfig  m_vdpauConfig;
  std::vector<vdpau_render_state*> m_videoSurfaces;
  CCriticalSection m_videoSurfaceSec;
  PictureAge    m_picAge;

  COutput       m_vdpauOutput;
  CVdpauBufferStats m_bufferStats;
  CEvent        m_inMsgEvent;
  CVdpauRenderPicture *m_presentPicture;

  int m_dropCount;
  bool m_dropState;
};

}

