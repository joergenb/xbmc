/*
 *      Copyright (C) 2005-2008 Team XBMC
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
#include "RenderManager.h"
#include "threads/CriticalSection.h"
#include "video/VideoReferenceClock.h"
#include "utils/MathUtils.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"

#include "Application.h"
#include "settings/Settings.h"
#include "settings/GUISettings.h"
#include "settings/AdvancedSettings.h"

#ifdef _LINUX
#include "PlatformInclude.h"
#endif

#if defined(HAS_GL)
  #include "LinuxRendererGL.h"
#elif HAS_GLES == 2
  #include "LinuxRendererGLES.h"
#elif defined(HAS_DX)
  #include "WinRenderer.h"
#elif defined(HAS_SDL)
  #include "LinuxRenderer.h"
#endif

#include "RenderCapture.h"
#include "../dvdplayer/DVDCodecs/Video/VDPAU.h"

/* to use the same as player */
#include "../dvdplayer/DVDClock.h"
#include "../dvdplayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "../dvdplayer/DVDCodecs/DVDCodecUtils.h"
#include "../dvdplayer/IDVDPlayerVideoOutput.h"

#define MAXPRESENTDELAY 0.500

/* at any point we want an exclusive lock on rendermanager */
/* we must make sure we don't have a graphiccontext lock */
/* these two functions allow us to step out from that lock */
/* and reaquire it after having the exclusive lock */

template<class T>
class CRetakeLock
{
public:
  CRetakeLock(CSharedSection &section, bool immidiate = true, CCriticalSection &owned = g_graphicsContext)
    : m_owned(owned)
  {
    m_count = m_owned.exit();
    m_lock  = new T(section);
    if(immidiate)
    {
      m_owned.restore(m_count);
      m_count = 0;
    }
  }
  ~CRetakeLock()
  {
    delete m_lock;
    m_owned.restore(m_count);
  }
  void Leave() { m_lock->Leave(); }
  void Enter() { m_lock->Enter(); }

private:
  T*                m_lock;
  CCriticalSection &m_owned;
  DWORD             m_count;
};

CXBMCRenderManager::CXBMCRenderManager()
{
  m_pRenderer = NULL;
  m_bPauseDrawing = false;
  m_bIsStarted = false;

  m_presentfield = FS_NONE;
  m_presenttime = 0;
  m_presentstep = PRESENT_IDLE;
  m_rendermethod = 0;
  m_presentsource = 0;
  m_presentmethod = PRESENT_METHOD_SINGLE;
  m_bReconfigured = false;
  m_hasCaptures = false;
  m_videoOutput = NULL;
  m_flushing = false;
}

CXBMCRenderManager::~CXBMCRenderManager()
{
  delete m_pRenderer;
  m_pRenderer = NULL;
}

double CXBMCRenderManager::GetPresentTime()
{
  return CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
}

static double wrap(double x, double minimum, double maximum)
{
  if(x >= minimum
  && x <= maximum)
    return x;
  x = fmod(x - minimum, maximum - minimum) + minimum;
  if(x < minimum)
    x += maximum - minimum;
  if(x > maximum)
    x -= maximum - minimum;
  return x;
}

void CXBMCRenderManager::WaitPresentTime(double presenttime, bool reset_corr /* = false */)
{
  // The presenttime should reflect as best as possible the clock time when the 
  // frame about to flipped to front buffer will then display
  // For this we assume it will take one more vblank beyond the one we choose to wait for plus 
  // the delay from signal to visible of the physical display device 

  // In the case of smooth video mode (ie vblank based reference clock) we perform additional
  // tracking correction:
  //  To avoid any possible indeterminate behaviour around which clock tick value 
  //  will best match in the wait function when presenttime is very close to tick time
  //  we begin by setting our wait target time back half a tick from actual desired (so 
  //  that the desired tick will take it distinctly past), and then we try to
  //  track the delta between desired and tick value so that we can maintain this safe uniform 
  //  position possibly drifting but if so gradually and smoothly until such time as we may 
  //  then need to target a closer tick (resulting in a short or long display of a frame and if
  //  short likely subsequently a frame drop from player)

  if (presenttime < 0.0)
    return;  //bad data in just return and let stream play at refresh rate pace

  double signal_to_view_delay = GetDisplaySignalToViewDelay();
  double frametime;
  int fps = g_VideoReferenceClock.GetRefreshRate(&frametime);
  if(fps <= 0)
  {
    /* smooth video not enabled */
    // estimate half a frametime to get to front buffer on average
    CDVDClock::WaitAbsoluteClock((presenttime - (0.5 * frametime) - signal_to_view_delay) * DVD_TIME_BASE);
    return;
  }

  bool resetbuff = false;
  // when asked to reset our wait correction position state by player do so (due to known video sync position adjustment)
  if (reset_corr)
  {
    m_presentcorr = 0.0;
    resetbuff = true;
  }
  double presentcorr = m_presentcorr;

  // fraction of frame ahead of targetwaitclock that the abs dvd clock should ideally be after our wait has completed
  double targetpos = 0.5;
  // target wait should be earlier by 1 frametime to get to frontbuffer + targetpos as well as signal to view delay
  // and finally corrected by our wait clock tracking correction factor (m_presentcorr)
  double targetwaitclock = presenttime - ((1 + targetpos) * frametime) - signal_to_view_delay;
//CLog::Log(LOGDEBUG, "ASB: CXBMCRenderManager::WaitPresentTime targetwaitclock: %f presenttime: %f", targetwaitclock, presenttime);
  targetwaitclock += presentcorr * frametime;  //adjust by our accumulated correction

  // we now wait and wish our clock tick result to be targetpos out from target wait
  double clock = CDVDClock::WaitAbsoluteClock(targetwaitclock * DVD_TIME_BASE) / DVD_TIME_BASE;

  // error is number(fraction) of frames out we are from where we were trying to correct to
  double error = (clock - targetwaitclock) / frametime - targetpos;
  // abserror is number(fraction) of frames out we are from where we shuuld be without correction factor
  double abserror = error + presentcorr;
  m_presenterr = error;

  // we should be careful not too overshoot if we are getting close to a wrap boundary to avoid
  // swinging back and forward unnecessarily:

  // if wrapped abserror value changes by more 10% of frame from last then reset presentcorr and avgs
  // we should wrap error combine with m_presentcorr and if abs value greater 90% targetpos but 
  //   less than 105% move presentcorr by 1% avg error
  // else move by 5% - reset avg
  // if presentcorr approx 0 and error-wrapped approx abs(0.5) then steer by 10% in 
  //   that direction to get it quickly away from beat spot
  // if presentcorr + error-wrapped > 0.55 (we are required to make too large a forward correction) 
  //   then reset presentcorr and avgs
  // if presentcorr + error-wrapped < -0.55 targetpos then wrap presentcorr to 0.5 targetpos 
  //   to avoid any issues

  error = wrap(error, 0.0 - targetpos, 1.0 - targetpos);
  abserror = wrap(abserror, 0.0 - targetpos, 1.0 - targetpos);
  if ( (m_prevwaitabserror != DVD_NOPTS_VALUE && (fabs(abserror - m_prevwaitabserror) > 0.1)) || 
       (presentcorr > 0.55) || (fabs(presentcorr + error) > 0.6) )
  {
     // unstable fluctuations in position (implying no point in trying to target), 
     // or targetting too far forward a correction,
     // or simply too large the current error target...then reset presentcorr and error buffer
     // note: allowing the small forward correction overshoot helps to avoid hanging around 
     // the 0.5 beat zone for long enough to reduce the chances of targetting forward then
     // backward with high frequency.
     presentcorr = 0.0;
     m_prevwaitabserror = DVD_NOPTS_VALUE; //avoid next iteration using the prevabserror value
     resetbuff = true;
  }
  else if (presentcorr < -0.55)
  {
     // targetting to far backward, force it to next vblank at next iteration by wrapping
     // presentcorr straight to 0.5 and reset error buffer
     // - this avoids travelling through the troublesome beat zone
     presentcorr = 0.5;
     m_prevwaitabserror = DVD_NOPTS_VALUE; //avoid next iteration using the prevabserror value
     resetbuff = true;
  }
  else if (fabs(presentcorr) < 0.05 && fabs(error) > 0.45)
  {
     // strongly target in error direction to move clear of beat zone quickly initially
     presentcorr += 0.1 * error;
  }
  else if (fabs(presentcorr + error) > 0.45)
  {
     // we have drifted close to the beat zone so now we try to drift carefully so that 
     // we only wrap when surely required this is done with 1% adjustments of recent 
     // error average save error in the buffer
     m_errorindex = (m_errorindex + 1) % ERRORBUFFSIZE;
     m_errorbuff[m_errorindex] = error;

     //get the average error from the buffer
     double avgerror = 0.0;
     for (int i = 0; i < ERRORBUFFSIZE; i++)
           avgerror += m_errorbuff[i];
     avgerror /= ERRORBUFFSIZE;

     presentcorr = presentcorr + avgerror * 0.01;
  }
  else
  {
     // adjust by 5% of error directly
     resetbuff = true; //adjustment is large enough to warrant clearing averages
     presentcorr = presentcorr + (error * 0.05);
  }
  if (resetbuff)
  {
     memset(m_errorbuff, 0, sizeof(m_errorbuff));
  }
  // store new value back in global for access by codec info
  m_presentcorr = presentcorr;
}

CStdString CXBMCRenderManager::GetVSyncState()
{
  CStdString state;
  state.Format("sh:%i lo:%i sync:%+3d%% err:%2d%%"
              , m_shortdisplaycount
              , m_longdisplaycount
              ,     MathUtils::round_int(m_presentcorr * 100)
              , abs(MathUtils::round_int(m_presenterr  * 100)));
  return state;
}

bool CXBMCRenderManager::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags, unsigned int format)
{
  /* make sure any queued frame was fully presented */
  if (IsConfigured() && !WaitDrained(500))
      CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for previous frame");

  CRetakeLock<CExclusiveLock> lock(m_sharedSection, false);
  if(!m_pRenderer)
  {
    CLog::Log(LOGERROR, "%s called without a valid Renderer object", __FUNCTION__);
    return false;
  }

  bool result = m_pRenderer->Configure(width, height, d_width, d_height, fps, flags, format);
  if(result)
  {
    if( flags & CONF_FLAGS_FULLSCREEN )
    {
      lock.Leave();
      g_application.getApplicationMessenger().SwitchToFullscreen();
      lock.Enter();
    }
    m_pRenderer->Update(false);
    m_bIsStarted = true;
    m_bReconfigured = true;
    m_presentstep = PRESENT_IDLE;
    m_presentevent.Set();
  }

  return result;
}

bool CXBMCRenderManager::IsConfigured()
{
  if (!m_pRenderer)
    return false;
  return m_pRenderer->IsConfigured();
}

void CXBMCRenderManager::Update(bool bPauseDrawing)
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_bPauseDrawing = bPauseDrawing;
  if (m_pRenderer)
  {
    m_pRenderer->Update(bPauseDrawing);
  }

  m_presentevent.Set();
}

void CXBMCRenderManager::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (!m_pRenderer)
      return;

    if (m_presentstep == PRESENT_IDLE)
      CheckNextBuffer();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.FlipRender();
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
      m_requestOverlayFlip = false;
    }
    else if (m_requestOverlayFlip)
    {
      m_overlays.FlipRender();
      m_requestOverlayFlip = false;
    }
  }

  if (g_advancedSettings.m_videoDisableBackgroundDeinterlace)
  {
    CSharedLock lock(m_sharedSection);
    PresentSingle(clear, flags, alpha);
  }
  else
    Render(clear, flags, alpha);

  m_overlays.Render();

  UpdatePostRenderClock();
  UpdatePreFlipClock();

  m_presentevent.Set();
}

unsigned int CXBMCRenderManager::PreInit()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_presentcorr = 0.0;
  m_presenterr  = 0.0;
  m_errorindex  = 0;
  memset(m_errorbuff, 0, sizeof(m_errorbuff));
  m_prevwaitabserror = 0.0;

  m_videoPicId = 0;
  m_renderinfo.framedur = 0.0;
  m_renderinfo.frameId = -1;
  m_renderinfo.framepts = DVD_NOPTS_VALUE;
  m_renderinfo.frameplayspeed = 0;
  for (int i = 0; i <= NUM_DISPLAYINFOBUF; i++)
  {
     m_displayinfo[i].framedur = 0.0;
     m_displayinfo[i].framepts = DVD_NOPTS_VALUE;
     m_displayinfo[i].frameId = -1;
     m_displayinfo[i].frameplayspeed = 0;
     m_displayinfo[i].frameclock = DVD_NOPTS_VALUE;
     m_displayinfo[i].refreshdur = 0.0;
  }
  m_refdisplayinfo.framepts = DVD_NOPTS_VALUE;
  m_refdisplayinfo.frameclock = DVD_NOPTS_VALUE;
  m_refdisplayinfo.frameplayspeed = 0;

  m_flipasync = true; 
  m_preflipclock = DVD_NOPTS_VALUE;
  m_postflipclock = DVD_NOPTS_VALUE;
  m_postrenderclock = DVD_NOPTS_VALUE;
  m_shortdisplaycount = 0;
  m_longdisplaycount = 0;

  m_bIsStarted = false;
  m_bPauseDrawing = false;
  if (!m_pRenderer)
  {
#if defined(HAS_GL)
    m_pRenderer = new CLinuxRendererGL();
#elif HAS_GLES == 2
    m_pRenderer = new CLinuxRendererGLES();
#elif defined(HAS_DX)
    m_pRenderer = new CWinRenderer();
#elif defined(HAS_SDL)
    m_pRenderer = new CLinuxRenderer();
#endif
  }

  return m_pRenderer->PreInit();
}

void CXBMCRenderManager::UnInit()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_bIsStarted = false;

  m_overlays.Flush();

  // free renderer resources.
  // TODO: we may also want to release the renderer here.
  if (m_pRenderer)
    m_pRenderer->UnInit();
}

bool CXBMCRenderManager::Flush()
{
  if (!m_pRenderer)
    return true;

  if (g_application.IsCurrentThread())
  {
    CLog::Log(LOGDEBUG, "%s - flushing renderer", __FUNCTION__);

    m_flushing = true;
    { CSingleLock lock (m_videoOutCritSect);
      if (m_videoOutput)
        m_videoOutput->Flush();
    }
    CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    m_pRenderer->Flush();
    m_flushEvent.Set();
    m_flushing = false;
  }
  else
  {
    ThreadMessage msg = {TMSG_RENDERER_FLUSH};
    m_flushEvent.Reset();
    g_application.getApplicationMessenger().SendMessage(msg, false);
    if (!m_flushEvent.WaitMSec(1000))
    {
      CLog::Log(LOGERROR, "%s - timed out waiting for renderer to flush", __FUNCTION__);
      return false;
    }
    else
      return true;
  }
  return true;
}

void CXBMCRenderManager::SetupScreenshot()
{
  CSharedLock lock(m_sharedSection);
  if (m_pRenderer)
    m_pRenderer->SetupScreenshot();
}

CRenderCapture* CXBMCRenderManager::AllocRenderCapture()
{
  return new CRenderCapture;
}

void CXBMCRenderManager::ReleaseRenderCapture(CRenderCapture* capture)
{
  CSingleLock lock(m_captCritSect);

  RemoveCapture(capture);

  //because a CRenderCapture might have some gl things allocated, it can only be deleted from app thread
  if (g_application.IsCurrentThread())
  {
    delete capture;
  }
  else
  {
    capture->SetState(CAPTURESTATE_NEEDSDELETE);
    m_captures.push_back(capture);
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

void CXBMCRenderManager::Capture(CRenderCapture* capture, unsigned int width, unsigned int height, int flags)
{
  CSingleLock lock(m_captCritSect);

  RemoveCapture(capture);

  capture->SetState(CAPTURESTATE_NEEDSRENDER);
  capture->SetUserState(CAPTURESTATE_WORKING);
  capture->SetWidth(width);
  capture->SetHeight(height);
  capture->SetFlags(flags);
  capture->GetEvent().Reset();

  if (g_application.IsCurrentThread())
  {
    if (flags & CAPTUREFLAG_IMMEDIATELY)
    {
      //render capture and read out immediately
      RenderCapture(capture);
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();
    }

    if ((flags & CAPTUREFLAG_CONTINUOUS) || !(flags & CAPTUREFLAG_IMMEDIATELY))
    {
      //schedule this capture for a render and readout
      m_captures.push_back(capture);
    }
  }
  else
  {
    //schedule this capture for a render and readout
    m_captures.push_back(capture);
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

void CXBMCRenderManager::ManageCaptures()
{
  //no captures, return here so we don't do an unnecessary lock
  if (!m_hasCaptures)
    return;

  CSingleLock lock(m_captCritSect);

  std::list<CRenderCapture*>::iterator it = m_captures.begin();
  while (it != m_captures.end())
  {
    CRenderCapture* capture = *it;

    if (capture->GetState() == CAPTURESTATE_NEEDSDELETE)
    {
      delete capture;
      it = m_captures.erase(it);
      continue;
    }

    if (capture->GetState() == CAPTURESTATE_NEEDSRENDER)
      RenderCapture(capture);
    else if (capture->GetState() == CAPTURESTATE_NEEDSREADOUT)
      capture->ReadOut();

    if (capture->GetState() == CAPTURESTATE_DONE || capture->GetState() == CAPTURESTATE_FAILED)
    {
      //tell the thread that the capture is done or has failed
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();

      if (capture->GetFlags() & CAPTUREFLAG_CONTINUOUS)
      {
        capture->SetState(CAPTURESTATE_NEEDSRENDER);

        //if rendering this capture continuously, and readout is async, render a new capture immediately
        if (capture->IsAsync() && !(capture->GetFlags() & CAPTUREFLAG_IMMEDIATELY))
          RenderCapture(capture);

        it++;
      }
      else
      {
        it = m_captures.erase(it);
      }
    }
    else
    {
      it++;
    }
  }

  if (m_captures.empty())
    m_hasCaptures = false;
}

void CXBMCRenderManager::RenderCapture(CRenderCapture* capture)
{
  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer || !m_pRenderer->RenderCapture(capture))
    capture->SetState(CAPTURESTATE_FAILED);
}

void CXBMCRenderManager::RemoveCapture(CRenderCapture* capture)
{
  //remove this CRenderCapture from the list
  std::list<CRenderCapture*>::iterator it;
  while ((it = find(m_captures.begin(), m_captures.end(), capture)) != m_captures.end())
    m_captures.erase(it);
}

void CXBMCRenderManager::FlipPage(volatile bool& bStop, double timestamp /* = 0LL*/, int source /*= -1*/, EFIELDSYNC sync /*= FS_NONE*/)
{
  if(timestamp - GetPresentTime() > MAXPRESENTDELAY)
    timestamp =  GetPresentTime() + MAXPRESENTDELAY;

  /* can't flip, untill timestamp */
  if(!g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(timestamp);

  /* make sure any queued frame was fully presented */
  double timeout = m_presenttime + 1.0;
  while(m_presentstep != PRESENT_IDLE && !bStop)
  {
    if(!m_presentevent.WaitMSec(100) && GetPresentTime() > timeout && !bStop)
    {
      CLog::Log(LOGWARNING, "CRenderManager::FlipPage - timeout waiting for previous frame");
      return;
    }
  };

  if(bStop)
    return;

  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if(!m_pRenderer) return;

    m_presenttime  = timestamp;
    m_presentfield = sync;
    m_presentstep  = PRESENT_FLIP;
    m_presentsource = source;
    EDEINTERLACEMODE deinterlacemode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
    EINTERLACEMETHOD interlacemethod = g_settings.m_currentVideoSettings.m_InterlaceMethod;
    if (interlacemethod == VS_INTERLACEMETHOD_AUTO)
      interlacemethod = m_pRenderer->AutoInterlaceMethod();

    bool invert = false;

    if (deinterlacemode == VS_DEINTERLACEMODE_OFF)
      m_presentmethod = PRESENT_METHOD_SINGLE;
    else
    {
      if (deinterlacemode == VS_DEINTERLACEMODE_AUTO && m_presentfield == FS_NONE)
        m_presentmethod = PRESENT_METHOD_SINGLE;
      else
      {
        if      (interlacemethod == VS_INTERLACEMETHOD_RENDER_BLEND)            m_presentmethod = PRESENT_METHOD_BLEND;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_WEAVE)            m_presentmethod = PRESENT_METHOD_WEAVE;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED) { m_presentmethod = PRESENT_METHOD_WEAVE ; invert = true; }
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_BOB)              m_presentmethod = PRESENT_METHOD_BOB;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_BOB_INVERTED)   { m_presentmethod = PRESENT_METHOD_BOB; invert = true; }
        else if (interlacemethod == VS_INTERLACEMETHOD_DXVA_BOB)                m_presentmethod = PRESENT_METHOD_BOB;
        else if (interlacemethod == VS_INTERLACEMETHOD_DXVA_BEST)               m_presentmethod = PRESENT_METHOD_BOB;
        else                                                                    m_presentmethod = PRESENT_METHOD_SINGLE;

        /* default to odd field if we want to deinterlace and don't know better */
        if (deinterlacemode == VS_DEINTERLACEMODE_FORCE && m_presentfield == FS_NONE)
          m_presentfield = FS_TOP;

        /* invert present field */
        if(invert)
        {
          if( m_presentfield == FS_BOT )
            m_presentfield = FS_TOP;
          else
            m_presentfield = FS_BOT;
        }
      }
    }

  }

  g_application.NewFrame();
  /* wait untill render thread have flipped buffers */
  timeout = m_presenttime + 1.0;
  while(m_presentstep == PRESENT_FLIP && !bStop)
  {
    if(!m_presentevent.WaitMSec(100) && GetPresentTime() > timeout && !bStop)
    {
      CLog::Log(LOGWARNING, "CRenderManager::FlipPage - timeout waiting for flip to complete");
      return;
    }
  }
}

float CXBMCRenderManager::GetMaximumFPS()
{
  float fps;

  if (g_guiSettings.GetInt("videoscreen.vsync") != VSYNC_DISABLED)
  {
    fps = (float)g_VideoReferenceClock.GetRefreshRate();
    if (fps <= 0) fps = g_graphicsContext.GetFPS();
  }
  else
    fps = 1000.0f;

  return fps;
}

void CXBMCRenderManager::Render(bool clear, DWORD flags, DWORD alpha)
{
  CSharedLock lock(m_sharedSection);

  if( m_presentmethod == PRESENT_METHOD_BOB )
    PresentBob(clear, flags, alpha);
  else if( m_presentmethod == PRESENT_METHOD_WEAVE )
    PresentWeave(clear, flags, alpha);
  else if( m_presentmethod == PRESENT_METHOD_BLEND )
    PresentBlend(clear, flags, alpha);
  else
    PresentSingle(clear, flags, alpha);

  m_overlays.Render();
}

void CXBMCRenderManager::Present(int &frameCount)
{
  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (!m_pRenderer)
      return;

    if (m_presentstep == PRESENT_IDLE)
      CheckNextBuffer();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.FlipRender();
      m_requestOverlayFlip = false;
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
    }
    // continue flipping overlays for still frames
    else if (m_requestOverlayFlip)
    {
      m_overlays.FlipRender();
      m_requestOverlayFlip = false;
    }
  }

  Render(true, 0, 255);

  UpdatePostRenderClock();

  /* wait for this present to be valid */
  if(g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(m_presenttime);
  UpdatePreFlipClock();

  frameCount = m_pRenderer->FramesInBuffers();
  if (m_presentstep == PRESENT_FRAME2)
    frameCount++;

  m_presentevent.Set();
}

/* simple present method */
void CXBMCRenderManager::PresentSingle(bool clear, DWORD flags, DWORD alpha)
{
  CSingleLock lock(g_graphicsContext);

  m_pRenderer->RenderUpdate(clear, flags, alpha);
  m_presentstep = PRESENT_IDLE;
}

/* new simpler method of handling interlaced material, *
 * we just render the two fields right after eachother */
void CXBMCRenderManager::PresentBob(bool clear, DWORD flags, DWORD alpha)
{
  CSingleLock lock(g_graphicsContext);

  if(m_presentstep == PRESENT_FRAME)
  {
    if( m_presentfield == FS_BOT)
      m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD0, alpha);
    else
      m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD0, alpha);
    m_presentstep = PRESENT_FRAME2;
    g_application.NewFrame();
  }
  else
  {
    if( m_presentfield == FS_TOP)
      m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD1, alpha);
    else
      m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD1, alpha);
    m_presentstep = PRESENT_IDLE;
  }
}

void CXBMCRenderManager::PresentBlend(bool clear, DWORD flags, DWORD alpha)
{
  CSingleLock lock(g_graphicsContext);

  if( m_presentfield == FS_BOT )
  {
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_NOOSD, alpha);
    m_pRenderer->RenderUpdate(false, flags | RENDER_FLAG_TOP, alpha / 2);
  }
  else
  {
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_NOOSD, alpha);
    m_pRenderer->RenderUpdate(false, flags | RENDER_FLAG_BOT, alpha / 2);
  }
  m_presentstep = PRESENT_IDLE;
}

/* renders the two fields as one, but doing fieldbased *
 * scaling then reinterlaceing resulting image         */
void CXBMCRenderManager::PresentWeave(bool clear, DWORD flags, DWORD alpha)
{
  CSingleLock lock(g_graphicsContext);

  m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOTH, alpha);
  m_presentstep = PRESENT_IDLE;
}

void CXBMCRenderManager::Recover()
{
//#if defined(HAS_GL) && !defined(TARGET_DARWIN)
//  CLog::Log(LOGERROR, "CXBMCRenderManager::Recover");
//  //glFlush(); // attempt to have gpu done with pixmap and vdpau
//  glFinish();
//#endif
//  UnInit();
}

void CXBMCRenderManager::UpdateResolution()
{
  if (m_bReconfigured)
  {
    CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (g_graphicsContext.IsFullScreenVideo() && g_graphicsContext.IsFullScreenRoot())
    {
      RESOLUTION res = GetResolution();
      g_graphicsContext.SetVideoResolution(res);
    }
    m_bReconfigured = false;
  }
}


int CXBMCRenderManager::AddVideoPicture(DVDVideoPicture& pic, double pts, double presenttime, int playspeed, bool vclockresync /* = false */)
{
  if (m_flushing)
    return -2;

  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer)
    return -1;

  if(m_pRenderer->AddVideoPicture(&pic))
    return 1;

  YV12Image image;

  int source = m_pRenderer->FlipFreeBuffer();
  if (source < 0)
  {
    CLog::Log(LOGERROR, "CXBMCRenderManager::AddVideoPicture - error getting next buffer");
    return -1;
  }

  int index = m_pRenderer->GetImage(&image, source);

  if(index < 0)
  {
    return index;
  }

  double framedur = pic.iDuration;
  // set fieldsync if picture is interlaced
  EFIELDSYNC mDisplayField = FS_NONE;
  if( pic.iFlags & DVP_FLAG_INTERLACED )
  {
    if( pic.iFlags & DVP_FLAG_TOP_FIELD_FIRST )
      mDisplayField = FS_TOP;
    else
      mDisplayField = FS_BOT;
  }

  if(pic.format == DVDVideoPicture::FMT_YUV420P)
  {
    CDVDCodecUtils::CopyPicture(&image, &pic);
  }
  else if(pic.format == DVDVideoPicture::FMT_NV12)
  {
    CDVDCodecUtils::CopyNV12Picture(&image, &pic);
  }
  else if(pic.format == DVDVideoPicture::FMT_YUY2
       || pic.format == DVDVideoPicture::FMT_UYVY)
  {
    CDVDCodecUtils::CopyYUV422PackedPicture(&image, &pic);
  }
  else if(pic.format == DVDVideoPicture::FMT_DXVA)
  {
    CDVDCodecUtils::CopyDXVA2Picture(&image, &pic);
  }
#ifdef HAVE_LIBVDPAU
  else if(pic.format == DVDVideoPicture::FMT_VDPAU || pic.format == DVDVideoPicture::FMT_VDPAU_420)
  {
    if (pic.vdpau)
    {
      m_pRenderer->AddProcessor(pic.vdpau);
      pic.vdpau->Present(index);
    }
  }
#endif
#ifdef HAVE_LIBOPENMAX
  else if(pic.format == DVDVideoPicture::FMT_OMXEGL)
    m_pRenderer->AddProcessor(pic.openMax, &pic);
#endif
#ifdef HAVE_VIDEOTOOLBOXDECODER
  else if(pic.format == DVDVideoPicture::FMT_CVBREF)
    m_pRenderer->AddProcessor(pic.vtb, &pic);
#endif
#ifdef HAVE_LIBVA
  else if(pic.format == DVDVideoPicture::FMT_VAAPI)
    m_pRenderer->AddProcessor(*pic.vaapi);
#endif

  // set pts and sync etc
  *image.pPresenttime = presenttime;
  *image.pPts = pts;
  *image.pId = m_videoPicId++;
  *image.pPlaySpeed = playspeed;
  *image.pFrameDur = framedur;
  *image.pVClockResync = vclockresync;
  *image.pSync = mDisplayField;

  // upload texture
  m_pRenderer->Upload(index);
  m_pRenderer->ReleaseImage(index, false);

  return index;
}

int CXBMCRenderManager::WaitForBuffer(volatile bool& bStop)
{
  if (m_flushing)
    return -2;

  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer)
    return -1;

  //wait up to a second as this is our slowest allowed output rate
  double timeout = GetPresentTime() + 1.0;
  while(!m_pRenderer->HasFreeBuffer() && !bStop)
  {
    lock.Leave();
    m_flipEvent.WaitMSec(5);
    if(GetPresentTime() > timeout && !bStop)
    {
      CLog::Log(LOGWARNING, "CRenderManager::WaitForBuffer - timeout waiting for buffer");
      m_pRenderer->LogBuffers();
      return -1;
    }
    lock.Enter();
  }
  return 1;
}

void CXBMCRenderManager::UpdateDisplayInfo()
{
  // first update our pts tracking:
  // so assuming for now the following state:  
  //    async flipping (eg no gLfinish())
  //    && using vsync 
  //    && video reference clock 
  //    && not too high refresh rate eg vb duration > ~8ms
  // then
  //    if postflipclock - preflipclock >= 6ms && postflipclock > vb_after_preflipclock 
  //         assume displayclock == vb_after_postflipclock
  //    if postflipclock < vb_after_preflipclock && vb_after_preflipclock - postrenderclock > min(0.5 * vb_duration, 8ms)
  //         assume displayclock == vb_after_postflipclock
  //    else estimate_displayclock only as say vb_after_postflipclock 
  //         (to be used for estimating display pts and only if we have not updated displayclock recently etc)
  // TODO: async mode - will need to adjust the functions down to PresentRenderImpl() to pass this through 
  // TODO: ignore for now for simplicity non-fullscreen consideration as accuracy probably not so important...
  //       but what about non full screen root (windowed mode)?
  
  if (m_preflipclock == DVD_NOPTS_VALUE || m_postflipclock == DVD_NOPTS_VALUE || m_postrenderclock == DVD_NOPTS_VALUE)
    return;

  bool bFrameChange = false;
  if (m_renderinfo.frameId != m_displayinfo[0].frameId)
  {
     bFrameChange = true;
     int framesDropped = m_renderinfo.frameId - m_displayinfo[0].frameId - 1;
     if (framesDropped > 0)
        CLog::Log(LOGERROR, "CRenderManager::UpdateDisplayInfo detected frames dropped by RenderManager: %i", framesDropped);
  }
  //else
  //  return;
  double clocktickafterpostflipclock; 
  double signal_to_view_delay;
  if (bFrameChange)
  {
     // get the vblank clock tick time following m_postflipclock value
     clocktickafterpostflipclock = CDVDClock::GetNextAbsoluteClockTick(m_postflipclock * DVD_TIME_BASE) / DVD_TIME_BASE;
     signal_to_view_delay = GetDisplaySignalToViewDelay(); 
  }

  double displayrefreshdur;
  int fps = g_VideoReferenceClock.GetRefreshRate(&displayrefreshdur); //fps == 0 or less assume no vblank based reference clock

  CExclusiveLock lock(m_sharedDisplayInfoSection); //now we can update the info variables.
      

  if (bFrameChange)
  {
     m_flipasync = true; //TODO: get application to tell render manager if we are async or not
     for (int i = NUM_DISPLAYINFOBUF - 1; i > 0; i--)
     {
         m_displayinfo[i].frameclock = m_displayinfo[i-1].frameclock;
         m_displayinfo[i].frameplayspeed = m_displayinfo[i-1].frameplayspeed;
         m_displayinfo[i].framedur = m_displayinfo[i-1].framedur;
         m_displayinfo[i].refreshdur = m_displayinfo[i-1].refreshdur;
         m_displayinfo[i].framepts = m_displayinfo[i-1].framepts;
         m_displayinfo[i].frameId = m_displayinfo[i-1].frameId;
     }
     m_displayinfo[0].framepts = m_renderinfo.framepts;
     m_displayinfo[0].frameId = m_renderinfo.frameId;
     m_displayinfo[0].frameplayspeed = m_renderinfo.frameplayspeed;
     m_displayinfo[0].framedur = m_renderinfo.framedur;  
     m_displayinfo[0].refreshdur = displayrefreshdur;

     // now we need to establish values for m_displayinfo[0].frameclock, and if 
     // possible m_refdisplayinfo.frameclock, m_refdisplayinfo.frameplayspeed, m_refdisplayinfo.frameplaypts 
     if (fps > 0 && displayrefreshdur > 0.001) //vblank based smooth video and plausible refresh duration
     {
        if (m_flipasync) 
        {
           // 6ms overrun across a vblank to signify it must have waited for a previous flip to complete
           double flipclockoverrundur = 0.006;
           // min of {8ms, half tick duration} safe allowance for outstanding render opengl 
           // commands to complete before a vblank
           double renderallowance = std::min(0.008, 0.5 * displayrefreshdur);

           // the vblank clock tick time following m_preflipclock value 
           double clocktickafterpreflipclock = clocktickafterpostflipclock;
           while (clocktickafterpreflipclock > m_preflipclock)
                  clocktickafterpreflipclock -= displayrefreshdur;  //reduce to just before
           clocktickafterpreflipclock = clocktickafterpreflipclock + displayrefreshdur;  //finally take it forward one tick
           // standard estimate is that all is well and frame will be on front buffer at vblank after flip
           m_displayinfo[0].frameclock = clocktickafterpostflipclock + signal_to_view_delay;
 
           if ( (m_postflipclock > clocktickafterpreflipclock && 
                clocktickafterpostflipclock - clocktickafterpreflipclock > flipclockoverrundur) ||
                   (m_postflipclock < clocktickafterpreflipclock && 
                    clocktickafterpreflipclock - m_postrenderclock > renderallowance) )
           {
              // the clock estimate is considered reference quality
              m_refdisplayinfo.frameclock = m_displayinfo[0].frameclock;
              m_refdisplayinfo.framepts = m_displayinfo[0].framepts;
              m_refdisplayinfo.frameplayspeed = m_displayinfo[0].frameplayspeed;
           }
        }
        else if (!m_flipasync)
        {
           // just assume the vblank before postflip clock is displayclock
           m_refdisplayinfo.frameclock = clocktickafterpostflipclock - displayrefreshdur + signal_to_view_delay;
           m_refdisplayinfo.framepts = m_displayinfo[0].framepts;
           m_refdisplayinfo.frameplayspeed = m_displayinfo[0].frameplayspeed;
           m_displayinfo[0].frameclock = m_refdisplayinfo.frameclock;          
        }
     }
     else
     {
        // just estimate this from postflipclock + half display duration + signal to view delay
        m_displayinfo[0].frameclock = m_postflipclock + (displayrefreshdur / 2) ;
     }
  } //end bFrameChange

  // if we didn't change speed or duration, and at normal speed and using smooth video then 
  // estimate if we got long/short display frames for codec info
  int frameplayspeed1 = m_displayinfo[1].frameplayspeed;
  int frameplayspeed2 = m_displayinfo[2].frameplayspeed;
  int frameplayspeed3 = m_displayinfo[3].frameplayspeed;
  double displayframepts1 = m_displayinfo[1].framepts;
  double displayframepts2 = m_displayinfo[2].framepts;
  double displayframepts3 = m_displayinfo[3].framepts;
  //double displayframedur1 = m_displayinfo[1].framedur;
  //double displayframedur2 = m_displayinfo[2].framedur;
  //double displayframedur3 = m_displayinfo[3].framedur;
  double displayframedur1 = (displayframepts1 - displayframepts2) / DVD_TIME_BASE;
  double displayframedur2 = (displayframepts2 - displayframepts3) / DVD_TIME_BASE;
  double displayframeclock1 = m_displayinfo[1].frameclock;
  double displayframeclock2 = m_displayinfo[2].frameclock;
  double displayframeclock3 = m_displayinfo[3].frameclock;
  if (frameplayspeed1 == frameplayspeed2 && 
      frameplayspeed1 == DVD_PLAYSPEED_NORMAL && 
      displayframeclock1 != DVD_NOPTS_VALUE && displayframeclock2 != DVD_NOPTS_VALUE &&
      displayrefreshdur != 0.0 && fps > 0.0 &&
      displayframepts1 != DVD_NOPTS_VALUE && displayframepts2 != DVD_NOPTS_VALUE)
//      displayframedur1 == displayframedur2)
  {  
     //the elasped clock time estimate between previous pts changes
     double displayclockelapsed1 = displayframeclock1 - displayframeclock2;
     double displayclockelapsed2 = displayframeclock1 - displayframeclock3;
     if (displayclockelapsed1 > displayframedur1 + 0.5 * displayrefreshdur)
        m_longdisplaycount++;
     else if (displayclockelapsed1 < displayframedur1 - 0.5 * displayrefreshdur && 
              displayclockelapsed1 > 0.9 * displayrefreshdur)
        m_shortdisplaycount++;
     else if (displayframeclock3 != DVD_NOPTS_VALUE && displayframepts3 != DVD_NOPTS_VALUE &&
              frameplayspeed1 == frameplayspeed3)
     {
        // we can look to tweak our estimates if we notice a shorter than refreshdur elapse 
        // when total elapse over 2 looks normal (we can assume we previously counted as a long
        // incorrectly)
        if (displayclockelapsed2 > (displayframedur1 + displayframedur2) * 0.95 && 
              displayclockelapsed2 < (displayframedur1 + displayframedur2) * 1.05 &&
              displayclockelapsed2 - displayclockelapsed1 > displayframedur2 + 0.5 * displayrefreshdur &&
              displayclockelapsed1 < displayrefreshdur * 0.9 && m_longdisplaycount > 0)
           m_longdisplaycount--;
     }
  }
}

void CXBMCRenderManager::NotifyDisplayFlip()
{
  if (!m_pRenderer)
    return;

  UpdatePostFlipClock();
  UpdateDisplayInfo();

  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_pRenderer->NotifyDisplayFlip();
  m_overlays.NotifyDisplayFlip();
  m_flipEvent.Set();
}

void CXBMCRenderManager::UpdatePostRenderClock()
{
  m_postrenderclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE; 
} 

void CXBMCRenderManager::UpdatePreFlipClock()
{
  m_preflipclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
} 
 
void CXBMCRenderManager::UpdatePostFlipClock()
{
  m_postflipclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
}  

double CXBMCRenderManager::GetDisplaySignalToViewDelay()
{
  // the delay between when the display signal begins transmitting to when it 
  // is considered viewed/perceived.
  // TODO: function to get from advanced settings reflecting the physical display 
  //       device and video signal method and allowing multiples of vblank duration
  //       eg possibly half vb is good estimate for analog display, but with digital 1 vb + internal processing delay)

  // for now just go with a single refresh duration
  double displayrefreshdur = m_displayinfo[0].refreshdur;
  if (displayrefreshdur == 0.0)
    g_VideoReferenceClock.GetRefreshRate(&displayrefreshdur); //fps == 0 or less assume no vblank based reference clock
  return displayrefreshdur;
}

double CXBMCRenderManager::GetDisplayDelay()
{
  // estimate in clock time of the longest it will take to take an output frame, render it, 
  // then deliver it to be visible on display
  // This function allows the player to prepare relative clock-to-pts delta at initial resync

  // - assume 50ms for render + signal to view delay
  return 0.05 + GetDisplaySignalToViewDelay();
}

double CXBMCRenderManager::GetCurrentDisplayPts(int& playspeed, double& callclock)
{
  // return pts extrapolated in microseconds 
  double samplepts;
  double sampleclock;
  double clock = GetPresentTime();
  callclock = clock * DVD_TIME_BASE; //let caller know the absolute clock value we based extrapolation on 
  CSharedLock lock(m_sharedDisplayInfoSection);
  playspeed = m_displayinfo[0].frameplayspeed;
  if (m_displayinfo[0].framepts == DVD_NOPTS_VALUE)
     return DVD_NOPTS_VALUE; //tell caller we don't know

  //use estimates if available if no reference value OR playspeed has changed OR reference value is more than 2 seconds old
  if (m_refdisplayinfo.frameclock == DVD_NOPTS_VALUE || 
      playspeed != m_refdisplayinfo.frameplayspeed || clock - m_refdisplayinfo.frameclock > 2.0)
  {        
     samplepts = m_displayinfo[0].framepts;
     sampleclock = m_displayinfo[0].frameclock;
  }
  else
  {
     samplepts = m_refdisplayinfo.framepts;
     sampleclock = m_refdisplayinfo.frameclock;
  }

  if ( (playspeed == DVD_PLAYSPEED_PAUSE) ||
       (clock >= m_displayinfo[0].frameclock + m_displayinfo[0].refreshdur) ) 
  {
     return m_displayinfo[0].framepts;
  }
  else 
  {
     double interpolatedpts = samplepts + ((double)(playspeed / DVD_PLAYSPEED_NORMAL) * (clock - sampleclock) * DVD_TIME_BASE);
     return interpolatedpts;
  }
}

// despite the name this function will also prepare state for render flip too
void CXBMCRenderManager::CheckNextBuffer()
{
  if(!m_pRenderer) return;

  YV12Image image;
  int source = m_pRenderer->GetNextRenderBufferIndex();
  if (source == -1)
    return;

  int index = m_pRenderer->GetImage(&image, source, true);
  if(index < 0)
    return;

  double presenttime = *image.pPresenttime/DVD_TIME_BASE;
  double clocktime = GetPresentTime();
  if(presenttime - clocktime > MAXPRESENTDELAY)
    presenttime = clocktime + MAXPRESENTDELAY;

  //TODO: we should not request flip too early and not flip too late...improve to give some of the display dislay logic and only present if within some acceptable range
  if(g_graphicsContext.IsFullScreenVideo()
      || presenttime <= clocktime)
  {
    m_renderinfo.framepts = *image.pPts; //from image
    m_renderinfo.frameId = *image.pId; //from image
    m_renderinfo.frameplayspeed = *image.pPlaySpeed; //from image
    m_renderinfo.framedur = *image.pFrameDur/DVD_TIME_BASE; //from image
    m_vclockresync = *image.pVClockResync;

    m_presenttime  = presenttime;
    m_presentfield = *image.pSync;
    m_presentstep  = PRESENT_FLIP;
    m_presentsource = index;
    EDEINTERLACEMODE deinterlacemode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
    EINTERLACEMETHOD interlacemethod = g_settings.m_currentVideoSettings.m_InterlaceMethod;
    if (interlacemethod == VS_INTERLACEMETHOD_AUTO)
      interlacemethod = m_pRenderer->AutoInterlaceMethod();

    bool invert = false;

    if (deinterlacemode == VS_DEINTERLACEMODE_OFF)
      m_presentmethod = PRESENT_METHOD_SINGLE;
    else
    {
      if (deinterlacemode == VS_DEINTERLACEMODE_AUTO && m_presentfield == FS_NONE)
        m_presentmethod = PRESENT_METHOD_SINGLE;
      else
      {
        if      (interlacemethod == VS_INTERLACEMETHOD_RENDER_BLEND)            m_presentmethod = PRESENT_METHOD_BLEND;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_WEAVE)            m_presentmethod = PRESENT_METHOD_WEAVE;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED) { m_presentmethod = PRESENT_METHOD_WEAVE ; invert = true; }
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_BOB)              m_presentmethod = PRESENT_METHOD_BOB;
        else if (interlacemethod == VS_INTERLACEMETHOD_RENDER_BOB_INVERTED)   { m_presentmethod = PRESENT_METHOD_BOB; invert = true; }
        else if (interlacemethod == VS_INTERLACEMETHOD_DXVA_BOB)                m_presentmethod = PRESENT_METHOD_BOB;
        else if (interlacemethod == VS_INTERLACEMETHOD_DXVA_BEST)               m_presentmethod = PRESENT_METHOD_BOB;
        else                                                                    m_presentmethod = PRESENT_METHOD_SINGLE;

        /* default to odd field if we want to deinterlace and don't know better */
        if (deinterlacemode == VS_DEINTERLACEMODE_FORCE && m_presentfield == FS_NONE)
          m_presentfield = FS_TOP;

        /* invert present field */
        if(invert)
        {
          if( m_presentfield == FS_BOT )
            m_presentfield = FS_TOP;
          else
            m_presentfield = FS_BOT;
        }
      }
    }
  }
  m_pRenderer->ReleaseImage(index, false);
}

void CXBMCRenderManager::ReleaseProcessor()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);
  if (!m_pRenderer)
    return;

  m_pRenderer->ReleaseProcessor();
}

bool CXBMCRenderManager::WaitDrained(int timeout /* = 100 */)
{
  // timeout is in ms
  if (!IsConfigured())
    return true;

  double endtime = GetPresentTime() + ((double)timeout / 1000);
  while(m_presentstep != PRESENT_IDLE || m_pRenderer->GetNextRenderBufferIndex() != -1)
  {
    if (timeout > 50)
       g_application.NewFrame(); //just in case application was not told about the render frames
    m_presentevent.WaitMSec(timeout);
    double currtime = GetPresentTime();
    if (currtime >= endtime)
    {
      break;
    }
    timeout = (int)((double)(endtime - currtime) * 1000) + 1;
  }
  // return true (drained) if all buffers rendered and have reached idle step
  return (m_presentstep == PRESENT_IDLE && m_pRenderer->GetNextRenderBufferIndex() == -1);
}

void CXBMCRenderManager::RegisterVideoOutput(IDVDPlayerVideoOutput *videoOutput)
{
  CSingleLock lock(m_videoOutCritSect);
  m_videoOutput = videoOutput;
}
