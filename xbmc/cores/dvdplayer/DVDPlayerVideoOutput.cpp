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
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "DVDPlayerVideoOutput.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "DVDCodecs/Video/DVDVideoPPFFmpeg.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "windowing/WindowingFactory.h"
#include "Application.h"

CDVDPlayerVideoOutput::CDVDPlayerVideoOutput(CDVDPlayerVideo *videoplayer, CDVDClock* pClock)
  : CThread("Video Output Thread"), m_controlPort("ControlPort", &m_inMsgSignal, &m_outMsgSignal) , m_dataPort("DataPort", &m_inMsgSignal, &m_outMsgSignal)
{
  m_pVideoPlayer = videoplayer;
  m_pts = 0;
  m_glWindow = 0;
  m_glContext = 0;
  m_pixmap = 0;
  m_glPixmap = 0;
  m_pClock = pClock;
  memset(&m_picture, 0, sizeof(DVDVideoPicture));
}

CDVDPlayerVideoOutput::~CDVDPlayerVideoOutput()
{
  Dispose();
}

void CDVDPlayerVideoOutput::Start()
{
  Create();
}

void CDVDPlayerVideoOutput::Reset()
{
  m_dataPort.Purge();
  m_controlPort.Purge();

  SendControlMessage(ControlProtocol::RESET,0,0,true, 1000);
}

void CDVDPlayerVideoOutput::ReleaseCodec()
{
  SendControlMessage(ControlProtocol::RELEASE_PROCESSOR,0,0,true, 1000);
}

void CDVDPlayerVideoOutput::Unconfigure()
{
  m_dataPort.Purge();
  m_controlPort.Purge();
  SendControlMessage(ControlProtocol::UNCONFIGURE,0,0,true, 1000);
}

bool CDVDPlayerVideoOutput::SendControlMessage(ControlProtocol::OutSignal signal, void *data /* = NULL */, int size /* = 0 */, bool sync /* = false */, int timeout /* = 0*/)
{
  CLog::Log(LOGNOTICE,"------- control msg: %d", signal);
  if (sync)
  {
    Message *replyMsg;
    if (m_controlPort.SendOutMessageSync(signal, &replyMsg, timeout, data, size))
    {
      replyMsg->Release();
      return true;
    }
    return false;
  }
  else
  {
    m_controlPort.SendOutMessage(signal, data, size);
  }
  return true;
}

void CDVDPlayerVideoOutput::Dispose()
{
  m_bStop = true;
  m_inMsgSignal.Set();
  m_outMsgSignal.Set();
  StopThread();
}

void CDVDPlayerVideoOutput::OnStartup()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnStartup: Output Thread created");
}

void CDVDPlayerVideoOutput::OnExit()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnExit: Output Thread terminated");
}

bool CDVDPlayerVideoOutput::SendDataMessage(DataProtocol::OutSignal signal, ToOutputMessage &msg, bool sync /*= false*/, int timeout /* = 0 */, FromOutputMessage *reply /*= NULL*/)
{
  if (m_bStop)
    return false;

  if (sync)
  {
    Message *replyMsg;
    if (m_dataPort.SendOutMessageSync(signal, &replyMsg, timeout, &msg, sizeof(msg)))
    {
      if (reply)
      {
        switch (replyMsg->signal)
        {
        case DataProtocol::CONFIGURE:
          reply->iResult = EOS_CONFIGURE;
          break;
        case DataProtocol::QUIESCED:
          reply->iResult = EOS_QUIESCED;
          break;
        case DataProtocol::OUTPICRESULT:
          reply->iResult = *(int*)replyMsg->data;
          break;
        default:
          reply->iResult = 0;
          break;
        }
      }
      replyMsg->Release();
      return true;
    }
    return false;
  }
  else
  {
    m_dataPort.SendOutMessage(signal, &msg, sizeof(msg));
  }

  return true;
}

bool CDVDPlayerVideoOutput::GetDataMessage(FromOutputMessage &msg, int timeout /* = 0 */)
{
  bool bReturn = false;
  Message *dataMessage;

  CStopWatch timer;
  if (timeout > 0)
     timer.StartZero();
  while (!m_bStop)
  {
    if (m_dataPort.ReceiveInMessage(&dataMessage))
    {
      switch (dataMessage->signal)
      {
      case DataProtocol::CONFIGURE:
        msg.iResult = EOS_CONFIGURE;
        break;
      case DataProtocol::QUIESCED:
        msg.iResult = EOS_QUIESCED;
        break;
      case DataProtocol::OUTPICRESULT:
        msg.iResult = *(int*)dataMessage->data;
        break;
      default:
        break;
      }
      dataMessage->Release();
      bReturn = true;
      break;
    }
    if (timeout > 0 && !m_inMsgSignal.WaitMSec(timeout))
    {
      //CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetMessage - timed out");
      return false;
    }

    if (timeout <= 0)
      break;
    timeout -= timer.GetElapsedMilliseconds();
  }

  return bReturn;
}

void CDVDPlayerVideoOutput::SetCodec(CDVDVideoCodec *codec)
{
  m_pVideoCodec = codec;
}

void CDVDPlayerVideoOutput::SetPts(double pts)
{
  CSingleLock lock(m_msgSection);
  m_pts = pts;
}

double CDVDPlayerVideoOutput::GetPts()
{
  CSingleLock lock(m_msgSection);
  return m_pts;
}

void CDVDPlayerVideoOutput::SendPlayerMessage(ControlProtocol::InSignal signal, void *data /* = NULL */, int size /* = 0 */)
{
  CLog::Log(LOGNOTICE,"------- send player msg: %d", signal);
  m_controlPort.SendInMessage(signal, data, size);
}

bool CDVDPlayerVideoOutput::ResyncClockToVideo(double pts, int playerSpeed, bool bFlushed /* = false */)
{
   if (pts != DVD_NOPTS_VALUE)
   {
      // milliseconds of time required for decoder thread to buffer data adequately and 
      // to allow audtio thread the chance to adjust probably less than 100ms is not a good idea in any case
      int prepTime; 
      if (playerSpeed == DVD_PLAYSPEED_PAUSE)
         prepTime = 0;
      else if (bFlushed)
         prepTime = 300;
      else
         prepTime = 150;

      double clockPrime = 0.0;
      int sleepMs = 0;
      double displayDelay; 
      double displayTickDelay; 
      double displaySignalToViewDelay;

      //TODO: calculate the prepTime based on GetDisplayDelay(), GetDisplaySignalToViewDelay(), and buffer decode prep time estimate
      // - GetDisplayDelay() will tell us how long minimum in absolute clock time it should 
      //   take to get a picture from output to visible on display
      // - buffer decode prep estimate should be say 100ms for non-flush and 300ms for flush
      // - we should set the clock to give close to exact display pts to clock match using GetDisplaySignalToViewDelay()
      //   which tells us how much clock time after target tick is required to make the pic visible

      // for prepTime of 0 we just set the clock directly and return, otherwise we need to let other clock subscribers
      // that we are controllling the clock and let then know if they can tweak it for some period within some bounds
      // - during that time we must wait around until that option has passed (leaving at least say 50ms to output)  
      if (playerSpeed != DVD_PLAYSPEED_PAUSE)
      {
         displayDelay = g_renderManager.GetDisplayDelay() * DVD_TIME_BASE;
         displaySignalToViewDelay = g_renderManager.GetDisplaySignalToViewDelay() * DVD_TIME_BASE;
         // g_renderManager.GetDisplayDelay() is total delay and we want to know the distance to clock tick
         displayTickDelay = displayDelay - displaySignalToViewDelay;
         double tickInterval = m_pClock->GetTickInterval();

         // target delay in dvd clock time by summing the decode prep time and the display delay
         double targetDelay = ((double)prepTime * DVD_TIME_BASE / 1000) + displayTickDelay;
         // round up to a clock tick multiple to make the actual adjust
         int ticks = (int)((targetDelay / tickInterval) + 1);
         // this is the adjustment in absolute clock terms that would allow to get ready for the tick
         double ticktime = (double)ticks * tickInterval; 
         // add in signal to view delay so that we will be sync'd at view time rather than swap to front buffer tick
         clockPrime = ticktime + displaySignalToViewDelay; //add in so that we will be sync'd at view time rather than swap to front buffer tick
         
         // determine a suitable sleep in millisecs after which we can check the clock after audio may have adjusted
         sleepMs = (int)(((clockPrime - displayDelay) / DVD_TIME_BASE * 1000) - 20);
         sleepMs = std::min(1000, sleepMs);
         sleepMs = std::max(5, sleepMs);
   
         //maxTickAdjustOffer is number of ticks forward other subscribers can move the clock
         //adjustOfferExpirySys is system time when the offer expires
         //controlDurSys is how long we wish to be the clock controller (clock will remove control after this elapsed sys time
         int64_t controlDurSys = 5 * CurrentHostFrequency(); //5 seconds
         int maxTickAdjustOffer = 2;
         int64_t adjustOfferExpirySys = (sleepMs - 5) * CurrentHostFrequency() / 1000; 

         m_pClock->SetVideoIsController(controlDurSys, maxTickAdjustOffer, adjustOfferExpirySys);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo doing SYNC SetVideoIsController for pts: %f with prepTime: %i, displayDelay: %f displaySignalToViewDelay: %f tickInterval: %f clockPrime: %f sleepMs: %i", pts, prepTime, displayDelay, displaySignalToViewDelay, tickInterval, clockPrime, sleepMs);
      }

CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo pre SetCurrentTickClock GetClock: %f", m_pClock->GetClock(false));
      m_pClock->SetCurrentTickClock(pts - (playerSpeed / DVD_PLAYSPEED_NORMAL) * clockPrime);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo done SetCurrentTickClock GetClock: %f", m_pClock->GetClock(false));
      

      if (playerSpeed != DVD_PLAYSPEED_PAUSE)
      {
         //TODO: perhaps do sleep in small steps to monitor the clock progress
         Sleep(sleepMs);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo done Sleep GetClock: %f", m_pClock->GetClock(false));
      }
      return true;
   }
   else
      return false;
}

enum STATES
{
  TOP = 0,                      // 0
  TOP_UNCONFIGURED,             // 1
  TOP_CONFIGURING,              // 2
  TOP_CONFIGURED,               // 3
  TOP_CONFIGURED_RECONFIGURING, // 4
  TOP_CONFIGURED_TRYPIC,        // 5
  TOP_CONFIGURED_CLOCKSYNC,     // 6
  TOP_CONFIGURED_PROCOVERLAY,   // 7
  TOP_CONFIGURED_PROCPICTURE,   // 8
};

int parentStates[] = {
    -1,
    0, //TOP_UNCONFIGURED
    0, //TOP_CONFIGURING
    0, //TOP_CONFIGURED
    3, //TOP_CONFIGURED_RECONFIGURING
    3, //TOP_CONFIGURED_TRYPIC
    3, //TOP_CONFIGURED_CLOCKSYNC
    3, //TOP_CONFIGURED_PROCOVERLAY
    3, //TOP_CONFIGURED_PROCPICTURE
};

void CDVDPlayerVideoOutput::StateMachine(int signal, Protocol *port, Message *msg)
{
  for (int state = m_state; ; state = parentStates[state])
  {
    switch (state)
    {
    case TOP: // TOP
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case ControlProtocol::RESET:
          DestroyGlxContext();
          ResetExtVariables();
          m_state = TOP_UNCONFIGURED;
          m_extTimeout = 50000;
          msg->Reply(ControlProtocol::OK);
          return;
        case ControlProtocol::UNCONFIGURE:
          DestroyGlxContext();
          ResetExtVariables();
          m_extTimeout = 50000;
          m_state = TOP_UNCONFIGURED;
          msg->Reply(ControlProtocol::OK);
          return;
        case ControlProtocol::RELEASE_PROCESSOR:
          g_renderManager.ReleaseProcessor();
          msg->Reply(ControlProtocol::OK);
          return;
        case ControlProtocol::SPEEDCHANGE:
          m_extPlayerSpeed = *(int*)msg->data;
          if (m_extPlayerSpeed == DVD_PLAYSPEED_PAUSE)
          {
            // pretend we output at this speed as likely no pic msg will be given to us at this speed
            // - this allows us to detect un-pause later
            m_extPrevOutputSpeed = DVD_PLAYSPEED_PAUSE;
            if (m_extPrevOutputPts != DVD_NOPTS_VALUE)
              m_pClock->Discontinuity(m_extPrevOutputPts + m_pVideoPlayer->GetDelay());  //get the clock re-positioned approximately
          }
          return;
        case ControlProtocol::DEALLOCPIC:
          m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
          m_extClockFlush = true;
          return;
        case ControlProtocol::EXPECTDELAY:
          double *data;
          data = (double*)msg->data;
          m_extTimeout = *data;
        default:
          break;
        }
      }
      else if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::FINISH:
          msg->Reply(DataProtocol::QUIESCED);
          return;
        default:
          break;
        }
      }
      {
        std::string portName = port == NULL ? "timer" : port->portName;
        CLog::Log(LOGWARNING, "%s - signal: %d form port: %s not handled for state: %d", __FUNCTION__, signal, portName.c_str(), m_state);
      }
      return;

    case TOP_UNCONFIGURED:
      if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::NEWPIC:
          ToOutputMessage *data;
          data = (ToOutputMessage*)msg->data;
          if (data)
          {
            m_extSpeed = data->iSpeed;
            m_extInterval = data->fInterval;
            m_extDrop = data->bDrop;
            m_extPlayerStarted = data->bPlayerStarted;
          }
          m_bGotPicture = GetPicture(m_extPts, m_extFrametime);
          if (m_bGotPicture)
            m_pVideoPlayer->CheckRenderConfig(&m_picture);
          msg->Reply(DataProtocol::CONFIGURE);
          m_state = TOP_CONFIGURING;
          m_extTimeout = 5000;
          CLog::Log(LOGNOTICE, "---------- send configure");
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURING:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case ControlProtocol::CONFIGURED:
          RefreshGlxContext();
          m_state = TOP_CONFIGURED_CLOCKSYNC;
          m_dataPort.SendOutMessage(DataProtocol::NEWPIC);
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case ControlProtocol::RESET:
          ResetExtVariables();
          m_extTimeout = 5000;
          m_state = TOP_CONFIGURED_TRYPIC;
          msg->Reply(ControlProtocol::OK);
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED_RECONFIGURING:
      if (port == &m_controlPort)
      {
        switch (signal)
        {
        case ControlProtocol::CONFIGURED:
          m_state = TOP_CONFIGURED_CLOCKSYNC;
          m_dataPort.DeferOut(false);
          m_dataPort.SendOutMessage(DataProtocol::NEWPIC);
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED_TRYPIC:
      if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::NEWPIC:
          ToOutputMessage *data;
          data = (ToOutputMessage*)msg->data;
          if (data)
          {
            m_extSpeed = data->iSpeed;
            m_extInterval = data->fInterval;
            m_extDrop = data->bDrop;
            m_extPlayerStarted = data->bPlayerStarted;
          }

          bool bNeedReconfigure;
          bNeedReconfigure = false;
          m_bGotPicture = GetPicture(m_extPts, m_extFrametime, m_extDrop);
          if (m_bGotPicture)
            bNeedReconfigure = m_pVideoPlayer->CheckRenderConfig(&m_picture);
          if (bNeedReconfigure)
          {
            msg->Reply(DataProtocol::CONFIGURE);
            m_state = TOP_CONFIGURED_RECONFIGURING;
            m_dataPort.DeferOut(true);
          }
          else if (m_bGotPicture)
          {
            m_state = TOP_CONFIGURED_CLOCKSYNC;
            m_bStateMachineSelfTrigger = true;
          }
          else
            m_extTimeout = 1000;
          return;
        default:
          break;
        }
      }
      else if (port == NULL) // timeout
      {
        switch (signal)
        {
        case ControlProtocol::TIMEOUT:
          m_dataPort.SendOutMessage(DataProtocol::NEWPIC);
          m_extTimeout = 100;
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED_CLOCKSYNC:
      if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::NEWPIC:
          m_extResync = false;
          if (m_extPlayerStarted && m_extPrevOutputSpeed == DVD_PLAYSPEED_PAUSE && m_extPlayerSpeed != DVD_PLAYSPEED_PAUSE)
          {
            ResyncClockToVideo(m_extPts + m_extVideoDelay, m_extPlayerSpeed, m_extClockFlush);
            m_extClockFlush = false;
            m_extResync = true;
          }

          m_state = TOP_CONFIGURED_PROCOVERLAY;
          m_bStateMachineSelfTrigger = true;
          return;
        case DataProtocol::PROCESSOVERLAYONLY:
          if (m_extPlayerSpeed == DVD_PLAYSPEED_PAUSE && m_extPrevOutputPts != DVD_NOPTS_VALUE)
             m_pClock->Discontinuity(m_extPrevOutputPts + m_extVideoDelay);  //get the clock re-positioned approximately
          else if (m_extPts != DVD_NOPTS_VALUE)
             m_pClock->Discontinuity(m_extPts + m_extVideoDelay - (m_extPlayerSpeed / DVD_PLAYSPEED_NORMAL) * DVD_MSEC_TO_TIME(50));  //get the clock re-positioned approximately

          m_state = TOP_CONFIGURED_PROCOVERLAY;
          m_bStateMachineSelfTrigger = true;
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED_PROCOVERLAY:
      if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::PROCESSOVERLAYONLY:
          int result;
          double clock, interval;
          clock = m_pClock->GetAbsoluteClock(true);
          interval = m_extInterval ? m_extInterval : DVD_MSEC_TO_TIME(100);
          SetPts(m_extPts);
          if (clock - m_extPrevOutputClock >= interval)
          {
            result = m_pVideoPlayer->ProcessOverlays(&m_picture, m_extPts, m_extOverlayDelay);
            if (result != -1)
            {
              g_application.NewFrame();
              if (m_extPts != DVD_NOPTS_VALUE)
                m_extPts += clock - m_extPrevOutputClock;
            }
          }
          m_state = TOP_CONFIGURED_TRYPIC;
          m_extTimeout = 500;
          return;
        case DataProtocol::NEWPIC:
          SetPts(m_extPts);
          result = m_pVideoPlayer->ProcessOverlays(&m_picture, m_extPts, m_extOverlayDelay);
          m_state = TOP_CONFIGURED_PROCPICTURE;
          m_bStateMachineSelfTrigger = true;
          return;
        default:
          break;
        }
      }
      break;

    case TOP_CONFIGURED_PROCPICTURE:
      if (port == &m_dataPort)
      {
        switch (signal)
        {
        case DataProtocol::NEWPIC:
          int result;
          result = m_pVideoPlayer->OutputPicture(&m_picture, m_extPts + m_extVideoDelay, m_extPlayerSpeed,
                                                         m_extPrevOutputSpeed, m_extOutputEarly, m_extResync);
          if (!(result & (EOS_CONFIGURE | EOS_DROPPED | EOS_ABORT)))
            g_application.NewFrame();

          if (result)
             msg->Reply(DataProtocol::OUTPICRESULT, &result, sizeof(result));

          m_extPrevOutputClock = m_pClock->GetAbsoluteClock(true);
          m_extPrevOutputSpeed = m_extPlayerSpeed;  //update our previous playspeed only when actually outputting
          // guess next frame pts
          // required for future pics with no pts value
          m_extPrevOutputPts = m_extPts;
          if (m_extPlayerSpeed != DVD_PLAYSPEED_PAUSE && m_extPts != DVD_NOPTS_VALUE)
             m_extPts = m_extPts + m_extFrametime * m_extPlayerSpeed / abs(m_extPlayerSpeed);

          m_extOutputEarly = false;
          m_state = TOP_CONFIGURED_TRYPIC;
          m_extTimeout = 100;
          return;
        default:
          break;
        }
      }
      break;

    default: // we are in no state, should not happen
      CLog::Log(LOGERROR, "%s - no valid state: %d", __FUNCTION__, m_state);
      return;
    }
  } // for loop
}

void CDVDPlayerVideoOutput::ResetExtVariables()
{
  m_extPts = DVD_NOPTS_VALUE;
  m_extPlayerSpeed = DVD_PLAYSPEED_NORMAL;
  m_extPrevOutputSpeed = DVD_PLAYSPEED_PAUSE; //assume paused before we start
  m_extPrevOutputPts = DVD_NOPTS_VALUE;
  m_extClock = m_pClock->GetAbsoluteClock(true);
  m_extPrevOutputClock = m_extClock; //init as if we just output
  m_extTimeoutStartClock = m_extClock;
  m_extVideoDelay = m_pVideoPlayer->GetDelay();
  m_extOverlayDelay = m_extVideoDelay + m_pVideoPlayer->GetSubtitleDelay();
  m_extFrametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
  m_bGotPicture = false;
  m_extResync = false;
  m_extClockFlush = true;
  m_extOutputEarly = true;
  memset(&m_picture, 0, sizeof(DVDVideoPicture));
}

void CDVDPlayerVideoOutput::Process()
{
  Message *msg;
  Protocol *port;
  bool gotMsg;

  // init extended state variables
  ResetExtVariables();

  m_state = TOP_UNCONFIGURED;
  m_extTimeout = 10000;
  m_bStateMachineSelfTrigger = false;

  while (!m_bStop)
  {
    gotMsg = m_controlPort.HasOutMessage() || m_dataPort.HasOutMessage();
    if (!m_bStateMachineSelfTrigger && (gotMsg || m_outMsgSignal.WaitMSec(m_extTimeout)))
    {
      gotMsg = false;

      // check for message from control port
      if (m_controlPort.ReceiveOutMessage(&msg))
      {
        gotMsg = true;
        port = &m_controlPort;
      }
      // check data port
      else if (m_dataPort.ReceiveOutMessage(&msg))
      {
        gotMsg = true;
        port = &m_dataPort;
      }

      if (gotMsg)
      {
        StateMachine(msg->signal, port, msg);
        if (!m_bStateMachineSelfTrigger)
        {
          msg->Release();
          msg = NULL;
        }
      }
    }
    else if (m_bStateMachineSelfTrigger)
    {
      m_bStateMachineSelfTrigger = false;
      // self trigger state machine
      StateMachine(msg->signal, port, msg);
      if (!m_bStateMachineSelfTrigger)
      {
        msg->Release();
        msg = NULL;
      }
    }
    else
    {
      msg = m_controlPort.GetMessage();
      msg->signal = ControlProtocol::TIMEOUT;
      // signal timeout to state machine
      StateMachine(msg->signal, NULL, msg);
      if (!m_bStateMachineSelfTrigger)
      {
        msg->Release();
        msg = NULL;
      }
    }
  }
  DestroyGlxContext();
}

// pts input as estimated pts for this pic and output as the corrected pts (which should also be in m_picture.pts)
// frametime is output as the frame duration 
bool CDVDPlayerVideoOutput::GetPicture(double& pts, double& frametime, bool drop /* = false*/)
{
  bool bReturn = false;

  DVDVideoPicture picture;
  CDVDVideoPPFFmpeg mPostProcess("");
  CStdString sPostProcessType;

  // try to retrieve the picture (should never fail!), unless there is a demuxer bug ofcours
  m_pVideoCodec->ClearPicture(&m_picture);
  if (m_pVideoCodec->GetPicture(&m_picture))
  {
    sPostProcessType.clear();

    if (drop)
       m_picture.iFlags |= DVP_FLAG_DROPPED;

    //TODO: store untouched pts and dts values in ring buffer of say 20 entries to allow decoder flush to make a better job of starting from correct place

//CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::GetPicture pts: %f", m_picture.pts);
    // validate picture timing,
    // if both pts invalid, use pts calculated from previous pts and iDuration
    // if still invalid use dts, else use picture.pts as passed
    //if (m_picture.dts == DVD_NOPTS_VALUE && m_picture.pts == DVD_NOPTS_VALUE)
    if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = pts;
    else if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = m_picture.dts;

    //Deinterlace if codec said format was interlaced or if we have selected we want to deinterlace
    //this video
    EDEINTERLACEMODE mDeintMode = g_settings.m_currentVideoSettings.m_DeinterlaceMode;
    EINTERLACEMETHOD mInt = g_settings.m_currentVideoSettings.m_InterlaceMethod;
    unsigned int mFilters = m_pVideoCodec->GetFilters();
    if ((mDeintMode == VS_DEINTERLACEMODE_AUTO && (picture.iFlags & DVP_FLAG_INTERLACED)) || mDeintMode == VS_DEINTERLACEMODE_FORCE)
    {
      if(!(mFilters & CDVDVideoCodec::FILTER_DEINTERLACE_ANY))
      {
        if((mInt == VS_INTERLACEMETHOD_DEINTERLACE)
        || (mInt == VS_INTERLACEMETHOD_AUTO && !g_renderManager.Supports(VS_INTERLACEMETHOD_RENDER_BOB)
                                            && !g_renderManager.Supports(VS_INTERLACEMETHOD_DXVA_ANY)))
        {
          if (!sPostProcessType.empty())
            sPostProcessType += ",";
          sPostProcessType += g_advancedSettings.m_videoPPFFmpegDeint;
        }
      }
    }

    if (g_settings.m_currentVideoSettings.m_PostProcess)
    {
      if (!sPostProcessType.empty())
        sPostProcessType += ",";
      // This is what mplayer uses for its "high-quality filter combination"
      sPostProcessType += g_advancedSettings.m_videoPPFFmpegPostProc;
    }

    if (!sPostProcessType.empty())
    {
      mPostProcess.SetType(sPostProcessType);
      if (mPostProcess.Process(&m_picture))
        mPostProcess.GetPicture(&m_picture);
    }

    /* if frame has a pts (usually originiating from demux packet), use that */
    if(m_picture.pts != DVD_NOPTS_VALUE)
    {
      //SetPts(m_picture.pts);  
      m_picture.pts = m_pVideoPlayer->GetCorrectedPicturePts(m_picture.pts, frametime);
      pts = m_picture.pts;
    }
    else
      frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();

//TODO: consider whether we should always update iDuration to frametime regardless
// - perhaps should always set it once we think we have a good calculation on frametime?
    bool bDurationCalculated = false;
    if(m_picture.iDuration == 0.0)
    {
      bDurationCalculated = true;
      m_picture.iDuration = frametime;
    }

    if (!bDurationCalculated && m_picture.iRepeatPicture)
      m_picture.iDuration *= m_picture.iRepeatPicture + 1;

    frametime = m_picture.iDuration; //make them consistent at least
    bReturn = true;
  }
  else
  {
    CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetPicture - error getting videoPicture.");
    frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
    bReturn = false;
  }

  return bReturn;
}

bool CDVDPlayerVideoOutput::RefreshGlxContext()
{
  Display*     dpy;
  GLXContext   glContext;

  dpy = g_Windowing.GetDisplay();
  glContext = g_Windowing.GetGlxContext();

  // Get our window attribs.
  XWindowAttributes wndattribs;
  XGetWindowAttributes(dpy, DefaultRootWindow(dpy), &wndattribs); // returns a status but I don't know what success is

  m_pixmap = XCreatePixmap(dpy,
                           DefaultRootWindow(dpy),
                           192,
                           108,
                           wndattribs.depth);
  if (!m_pixmap)
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideoOutput::RefreshGlxContext - Unable to create XPixmap");
    return false;
  }

  // create gl pixmap
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
  fbConfigs = glXChooseFBConfig(dpy, DefaultScreen(dpy), doubleVisAttributes, &num);
  if (fbConfigs==NULL)
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideoOutput::RefreshGlxContext - No compatible framebuffers found");
    return false;
  }
  fbConfigIndex = 0;

  m_glPixmap = glXCreatePixmap(dpy, fbConfigs[fbConfigIndex], m_pixmap, pixmapAttribs);

  if (!m_glPixmap)
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not create Pixmap");
    XFree(fbConfigs);
    return false;
  }

  XVisualInfo *visInfo;
  visInfo = glXGetVisualFromFBConfig(dpy, fbConfigs[fbConfigIndex]);
  if (!visInfo)
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not obtain X Visual Info for pixmap");
    XFree(fbConfigs);
    return false;
  }
  XFree(fbConfigs);

  m_glContext = glXCreateContext(dpy, visInfo, glContext, True);
  XFree(visInfo);

  if (!glXMakeCurrent(dpy, m_glPixmap, m_glContext))
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not make Pixmap current");
    return false;
  }

  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::RefreshGlxContext - refreshed context");
  return true;
}

bool CDVDPlayerVideoOutput::DestroyGlxContext()
{
  g_renderManager.ReleaseProcessor();

  Display *dpy = g_Windowing.GetDisplay();
  if (m_glContext)
  {
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, m_glContext);
  }
  m_glContext = 0;

  if (m_glPixmap)
    glXDestroyPixmap(dpy, m_glPixmap);
  m_glPixmap = 0;

  if (m_pixmap)
    XFreePixmap(dpy, m_pixmap);
  m_pixmap = 0;

  return true;
}
