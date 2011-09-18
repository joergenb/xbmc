#pragma once

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

#include "threads/Thread.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "utils/BitstreamStats.h"
#include "DVDPlayerVideo.h"
#include "threads/CriticalSection.h"
#include <queue>
#include "settings/VideoSettings.h"
#include <GL/glx.h>
#include "DVDPlayerVideoOutputProtocol.h"

struct ToOutputMessage
{
  ToOutputMessage()
  {
    bDrop = false;
    bPlayerStarted = false;
    iSpeed = 0;
    fInterval = 0.0;
  };
  bool bDrop; //drop flag (eg drop the picture)
  bool bPlayerStarted; //video player has reached started state
  double fInterval; //interval in dvd time (eg for overlay freqeuncy)
  int iSpeed; //video player play speed
};

struct FromOutputMessage
{
  FromOutputMessage()
  {
    iResult = 0;
  };
  int iResult;
};

class CDVDPlayerVideoOutput : public CThread
{
public:
  CDVDPlayerVideoOutput(CDVDPlayerVideo *videoplayer, CDVDClock* pClock);
  virtual ~CDVDPlayerVideoOutput();

  void Start();
  void Reset();
  void ReleaseCodec();
  void Unconfigure();
  void Dispose();
  bool SendDataMessage(DataProtocol::OutSignal signal, ToOutputMessage &msg, bool sync = false, int timeout = 0, FromOutputMessage *reply = NULL);
  bool GetDataMessage(FromOutputMessage &msg, int timeout = 0);
  void SetCodec(CDVDVideoCodec *codec);
  void SetPts(double pts);
  double GetPts();
  bool SendControlMessage(ControlProtocol::OutSignal signal, void *data = NULL, int size = 0, bool sync = false, int timeout = 0);
protected:
  void OnStartup();
  void OnExit();
  void Process();
  bool GetPicture(double& pts, double& frametime, bool drop = false);
  void SendPlayerMessage(ControlProtocol::InSignal signal, void *data = NULL, int size = 0);
  bool RefreshGlxContext();
  bool DestroyGlxContext();
  bool ResyncClockToVideo(double pts, int playerSpeed, bool bFlushed = false);
  void StateMachine(int signal, Protocol *port, Message *msg);
  void ResetExtVariables();

  double m_pts;
  CDVDVideoCodec* m_pVideoCodec;
  DVDVideoPicture m_picture;
  CEvent m_outMsgSignal, m_inMsgSignal;
  CCriticalSection m_criticalSection;
  CCriticalSection m_msgSection;
  CDVDPlayerVideo *m_pVideoPlayer;
  GLXContext m_glContext;
  GLXWindow m_glWindow;
  Pixmap    m_pixmap;
  GLXPixmap m_glPixmap;
  CDVDClock* m_pClock;
  ControlProtocol m_controlPort;
  DataProtocol m_dataPort;
  int m_state;
  bool m_bStateMachineSelfTrigger;

  // extended state variables for state machine
  bool m_bGotPicture;
  double m_extFrametime;
  double m_extPts;
  int m_extTimeout;
  int m_extPlayerSpeed;
  int m_extPrevOutputSpeed;
  double m_extPrevOutputPts;
  double m_extClock;
  double m_extPrevOutputClock;
  double m_extTimeoutStartClock;
  double m_extInterval;
  double m_extVideoDelay;
  double m_extOverlayDelay;
  int m_extSpeed;
  bool m_extOutputEarly;
  bool m_extResync;
  bool m_extClockFlush;
  bool m_extPlayerStarted;
  bool m_extDrop;
};
