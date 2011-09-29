#pragma once
/*
 *      Copyright (C) 2005-2010 Team XBMC
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

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#endif
#include "DynamicDll.h"
#include "DllAvCodec.h"

extern "C" {
#if (defined USE_EXTERNAL_FFMPEG)
  #if (defined HAVE_LIBAVFORMAT_AVDEVICE_H)
    #include <libavdevice/avdevice.h>
  #else
    #include <ffmpeg/avdevice.h>
  #endif
#else
  #include "libavdevice/avdevice.h"
#endif
}

#include "threads/SingleLock.h"

class DllAvDeviceInterface
{
public:
  virtual ~DllAvDeviceInterface() {}
  virtual void avdevice_register_all_dont_call(void)=0;
};

#if (defined USE_EXTERNAL_FFMPEG)

// Use direct mapping
class DllAvDevice : public DllDynamic, DllAvDeviceInterface
{
public:
  virtual ~DllAvDevice() {}
  virtual void avdevice_register_all()
  { 
    CSingleLock lock(DllAvCodec::m_critSection);
    return ::avdevice_register_all();
  } 
  virtual void avdevice_register_all_dont_call() { *(int* )0x0 = 0; }

  // DLL faking.
  virtual bool ResolveExports() { return true; }
  virtual bool Load() {
    CLog::Log(LOGDEBUG, "DllAvDevice: Using libavformat system library");
    return true;
  }
  virtual void Unload() {}
};

#else

class DllAvDevice : public DllDynamic, DllAvDeviceInterface
{
  DECLARE_DLL_WRAPPER(DllAvDevice, DLL_PATH_LIBAVDEVICE)

  LOAD_SYMBOLS()

  DEFINE_METHOD0(void, avdevice_register_all_dont_call)
  BEGIN_METHOD_RESOLVE()
    RESOLVE_METHOD_RENAME(avdevice_register_all, avdevice_register_all_dont_call)
  END_METHOD_RESOLVE()

public:
  void avdevice_register_all()
  {
    CSingleLock lock(DllAvCodec::m_critSection);
    avdevice_register_all_dont_call();
  }

  virtual bool Load()
  {
    return DllDynamic::Load();
  }
};

#endif
