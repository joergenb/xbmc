#pragma once

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

#define FILETIME_TO_ULARGE_INTEGER(ularge, filetime) { ularge.u.HighPart = filetime.dwHighDateTime; ularge.u.LowPart = filetime.dwLowDateTime; }

#include "system.h"
#include "threads/Thread.h"
#include "threads/SingleLock.h"

class CDVDMessageQueue;

typedef struct stProcessPerformance
{
  ULARGE_INTEGER  timer_thread;
  ULARGE_INTEGER  timer_system;
  CThread*        hThread;
} ProcessPerformance;

class CDVDPerformanceCounter
{
public:
  CDVDPerformanceCounter();
  ~CDVDPerformanceCounter();

  bool Initialize();
  void DeInitialize();

  void EnableAudioQueue(CDVDMessageQueue* pQueue)     { CSingleLock lock(m_critSection); m_pAudioQueue = pQueue; }
  void DisableAudioQueue()                            { CSingleLock lock(m_critSection); m_pAudioQueue = NULL;  }

  void EnableVideoQueue(CDVDMessageQueue* pQueue)     { CSingleLock lock(m_critSection); m_pVideoQueue = pQueue;  }
  void DisableVideoQueue()                            { CSingleLock lock(m_critSection); m_pVideoQueue = NULL;  }

  void EnableVideoDecodePerformance(CThread *hThread) { CSingleLock lock(m_critSection); m_videoDecodePerformance.hThread = hThread;  }
  void DisableVideoDecodePerformance()                { CSingleLock lock(m_critSection); m_videoDecodePerformance.hThread = NULL;  }

  void EnableAudioDecodePerformance(CThread *hThread) { CSingleLock lock(m_critSection); m_audioDecodePerformance.hThread = hThread;  }
  void DisableAudioDecodePerformance()                { CSingleLock lock(m_critSection); m_audioDecodePerformance.hThread = NULL;  }

  void EnableMainPerformance(CThread *hThread)        { CSingleLock lock(m_critSection); m_mainPerformance.hThread = hThread;  }
  void DisableMainPerformance()                       { CSingleLock lock(m_critSection); m_mainPerformance.hThread = NULL;  }

  CDVDMessageQueue*         m_pAudioQueue;
  CDVDMessageQueue*         m_pVideoQueue;

  ProcessPerformance        m_videoDecodePerformance;
  ProcessPerformance        m_audioDecodePerformance;
  ProcessPerformance        m_mainPerformance;

private:
  CCriticalSection m_critSection;
};

extern CDVDPerformanceCounter g_dvdPerformanceCounter;

