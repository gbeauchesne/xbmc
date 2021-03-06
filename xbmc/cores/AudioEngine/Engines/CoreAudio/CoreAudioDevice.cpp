/*
 *      Copyright (C) 2011-2012 Team XBMC
 *      http://xbmc.org
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

#include "CoreAudioDevice.h"
#include "CoreAudioAEHAL.h"
#include "CoreAudioChannelLayout.h"
#include "CoreAudioHardware.h"
#include "utils/log.h"

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// CCoreAudioDevice
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CCoreAudioDevice::CCoreAudioDevice()  :
  m_Started             (false    ),
  m_pSource             (NULL     ),
  m_DeviceId            (0        ),
  m_MixerRestore        (-1       ),
  m_IoProc              (NULL     ),
  m_ObjectListenerProc  (NULL     ),
  m_SampleRateRestore   (0.0f     ),
  m_HogPid              (-1       ),
  m_frameSize           (0        ),
  m_OutputBufferIndex   (0        ),
  m_BufferSizeRestore   (0        )
{
}

CCoreAudioDevice::CCoreAudioDevice(AudioDeviceID deviceId) :
  m_Started             (false    ),
  m_pSource             (NULL     ),
  m_DeviceId            (deviceId ),
  m_MixerRestore        (-1       ),
  m_IoProc              (NULL     ),
  m_ObjectListenerProc  (NULL     ),
  m_SampleRateRestore   (0.0f     ),
  m_HogPid              (-1       ),
  m_frameSize           (0        ),
  m_OutputBufferIndex   (0        ),
  m_BufferSizeRestore   (0        )
{
}

CCoreAudioDevice::~CCoreAudioDevice()
{
  Close();
}

bool CCoreAudioDevice::Open(AudioDeviceID deviceId)
{
  m_DeviceId = deviceId;
  m_BufferSizeRestore = GetBufferSize();
  CLog::Log(LOGDEBUG, "CCoreAudioDevice::Open: Opened device 0x%04x", (uint)m_DeviceId);
  return true;
}

void CCoreAudioDevice::Close()
{
  if (!m_DeviceId)
    return;

  // Stop the device if it was started
  Stop();

  // Unregister the IOProc if we have one
  if (m_IoProc)
    SetInputSource(NULL, 0, 0);

  SetHogStatus(false);
  CCoreAudioHardware::SetAutoHogMode(false);

  if (m_MixerRestore > -1) // We changed the mixer status
    SetMixingSupport((m_MixerRestore ? true : false));
  m_MixerRestore = -1;

  if (m_SampleRateRestore != 0.0f)
  {
    CLog::Log(LOGDEBUG,  "CCoreAudioDevice::Close: Restoring original nominal samplerate.");
    SetNominalSampleRate(m_SampleRateRestore);
  }

  if (m_BufferSizeRestore && m_BufferSizeRestore != GetBufferSize())
  {
    SetBufferSize(m_BufferSizeRestore);
    m_BufferSizeRestore = 0;
  }

  CLog::Log(LOGDEBUG, "CCoreAudioDevice::Close: Closed device 0x%04x", (uint)m_DeviceId);
  m_IoProc = NULL;
  m_pSource = NULL;
  m_DeviceId = 0;
  m_ObjectListenerProc = NULL;
}

void CCoreAudioDevice::Start()
{
  if (!m_DeviceId || m_Started)
    return;

  OSStatus ret = AudioDeviceStart(m_DeviceId, m_IoProc);
  if (ret)
    CLog::Log(LOGERROR, "CCoreAudioDevice::Start: "
      "Unable to start device. Error = %s", GetError(ret).c_str());
  else
    m_Started = true;
}

void CCoreAudioDevice::Stop()
{
  if (!m_DeviceId || !m_Started)
    return;

  OSStatus ret = AudioDeviceStop(m_DeviceId, m_IoProc);
  if (ret)
    CLog::Log(LOGERROR, "CCoreAudioDevice::Stop: "
      "Unable to stop device. Error = %s", GetError(ret).c_str());
  m_Started = false;
}

void CCoreAudioDevice::RemoveObjectListenerProc(AudioObjectPropertyListenerProc callback, void* pClientData)
{
  if (!m_DeviceId)
    return;

  AudioObjectPropertyAddress audioProperty;
  audioProperty.mSelector = kAudioObjectPropertySelectorWildcard;
  audioProperty.mScope = kAudioObjectPropertyScopeWildcard;
  audioProperty.mElement = kAudioObjectPropertyElementWildcard;

  OSStatus ret = AudioObjectRemovePropertyListener(m_DeviceId, &audioProperty, callback, pClientData);
  if (ret)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::RemoveObjectListenerProc: "
      "Unable to set ObjectListener callback. Error = %s", GetError(ret).c_str());
  }
  m_ObjectListenerProc = NULL;
}

bool CCoreAudioDevice::SetObjectListenerProc(AudioObjectPropertyListenerProc callback, void* pClientData)
{
  // Allow only one ObjectListener at a time
  if (!m_DeviceId || m_ObjectListenerProc)
    return false;

  AudioObjectPropertyAddress audioProperty;
  audioProperty.mSelector = kAudioObjectPropertySelectorWildcard;
  audioProperty.mScope = kAudioObjectPropertyScopeWildcard;
  audioProperty.mElement = kAudioObjectPropertyElementWildcard;

  OSStatus ret = AudioObjectAddPropertyListener(m_DeviceId, &audioProperty, callback, pClientData);

  if (ret)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetObjectListenerProc: "
      "Unable to remove ObjectListener callback. Error = %s", GetError(ret).c_str());
    return false;
  }

  m_ObjectListenerProc = callback;
  return true;
}

bool CCoreAudioDevice::SetInputSource(ICoreAudioSource* pSource, unsigned int frameSize, unsigned int outputBufferIndex)
{
  m_pSource   = pSource;
  m_frameSize = frameSize;
  m_OutputBufferIndex = outputBufferIndex;

  if (pSource)
    return AddIOProc();
  else
    return RemoveIOProc();
}

bool CCoreAudioDevice::AddIOProc()
{
  // Allow only one IOProc at a time
  if (!m_DeviceId || m_IoProc)
    return false;

  OSStatus ret = AudioDeviceCreateIOProcID(m_DeviceId, DirectRenderCallback, this, &m_IoProc);
  if (ret)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::AddIOProc: "
      "Unable to add IOProc. Error = %s", GetError(ret).c_str());
    m_IoProc = NULL;
    return false;
  }

  Start();

  CLog::Log(LOGDEBUG, "CCoreAudioDevice::AddIOProc: "
    "IOProc %p set for device 0x%04x", m_IoProc, (uint)m_DeviceId);
  return true;
}

bool CCoreAudioDevice::RemoveIOProc()
{
  if (!m_DeviceId || !m_IoProc)
    return false;

  Stop();

  OSStatus ret = AudioDeviceDestroyIOProcID(m_DeviceId, m_IoProc);
  if (ret)
    CLog::Log(LOGERROR, "CCoreAudioDevice::RemoveIOProc: "
      "Unable to remove IOProc. Error = %s", GetError(ret).c_str());
  else
    CLog::Log(LOGDEBUG, "CCoreAudioDevice::RemoveIOProc: "
      "IOProc %p removed for device 0x%04x", m_IoProc, (uint)m_DeviceId);
  m_IoProc = NULL; // Clear the reference no matter what

  m_pSource = NULL;

  Sleep(100);

  return true;
}

std::string CCoreAudioDevice::GetName()
{
  if (!m_DeviceId)
    return NULL;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyDeviceName;

  UInt32 propertySize;
  OSStatus ret = AudioObjectGetPropertyDataSize(m_DeviceId, &propertyAddress, 0, NULL, &propertySize); 
  if (ret != noErr)
    return NULL;

  std::string name = "";
  char *buff = new char[propertySize + 1];
  buff[propertySize] = 0x00;
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, buff); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::GetName: "
      "Unable to get device name - id: 0x%04x. Error = %s", (uint)m_DeviceId, GetError(ret).c_str());
  }
  else
  {
    name = buff;
  }
  delete buff;


  return name;
}

UInt32 CCoreAudioDevice::GetTotalOutputChannels()
{
  UInt32 channels = 0;

  if (!m_DeviceId)
    return channels;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;

  UInt32 size = 0;
  OSStatus ret = AudioObjectGetPropertyDataSize(m_DeviceId, &propertyAddress, 0, NULL, &size); 
  if (ret != noErr)
    return channels;

  AudioBufferList* pList = (AudioBufferList*)malloc(size);
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &size, pList); 
  if (ret == noErr)
  {
    for(UInt32 buffer = 0; buffer < pList->mNumberBuffers; ++buffer)
      channels += pList->mBuffers[buffer].mNumberChannels;
  }
  else
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::GetTotalOutputChannels: "
      "Unable to get total device output channels - id: 0x%04x. Error = %s",
      (uint)m_DeviceId, GetError(ret).c_str());
  }

  CLog::Log(LOGDEBUG, "CCoreAudioDevice::GetTotalOutputChannels: "
    "Found %u channels in %u buffers", (uint)channels, (uint)pList->mNumberBuffers);
  free(pList);

  return channels;
}

bool CCoreAudioDevice::GetStreams(AudioStreamIdList* pList)
{
  if (!pList || !m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyStreams;

  UInt32  propertySize = 0;
  OSStatus ret = AudioObjectGetPropertyDataSize(m_DeviceId, &propertyAddress, 0, NULL, &propertySize); 
  if (ret != noErr)
    return false;

  UInt32 streamCount = propertySize / sizeof(AudioStreamID);
  AudioStreamID* pStreamList = new AudioStreamID[streamCount];
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, pStreamList); 
  if (ret == noErr)
  {
    for (UInt32 stream = 0; stream < streamCount; stream++)
      pList->push_back(pStreamList[stream]);
  }
  delete[] pStreamList;

  return ret == noErr;
}


bool CCoreAudioDevice::IsRunning()
{
  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyDeviceIsRunning;

  UInt32 isRunning = 0;
  UInt32 propertySize = sizeof(isRunning);
  OSStatus ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, &isRunning); 
  if (ret != noErr)
    return false;

  return isRunning != 0;
}

bool CCoreAudioDevice::SetHogStatus(bool hog)
{
  // According to Jeff Moore (Core Audio, Apple), Setting kAudioDevicePropertyHogMode
  // is a toggle and the only way to tell if you do get hog mode is to compare
  // the returned pid against getpid, if the match, you have hog mode, if not you don't.
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyHogMode;

  if (hog)
  {
    // Not already set
    if (m_HogPid == -1)
    {
      CLog::Log(LOGDEBUG, "CCoreAudioDevice::SetHogStatus: "
        "Setting 'hog' status on device 0x%04x", (unsigned int)m_DeviceId);
      OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, sizeof(m_HogPid), &m_HogPid); 
      if (ret || m_HogPid != getpid())
      {
        CLog::Log(LOGERROR, "CCoreAudioDevice::SetHogStatus: "
          "Unable to set 'hog' status. Error = %s", GetError(ret).c_str());
        return false;
      }
      CLog::Log(LOGDEBUG, "CCoreAudioDevice::SetHogStatus: "
                "Successfully set 'hog' status on device 0x%04x", (unsigned int)m_DeviceId);
    }
  }
  else
  {
    // Currently Set
    if (m_HogPid > -1)
    {
      CLog::Log(LOGDEBUG, "CCoreAudioDevice::SetHogStatus: "
                "Releasing 'hog' status on device 0x%04x", (unsigned int)m_DeviceId);
      pid_t hogPid = -1;
      OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, sizeof(hogPid), &hogPid); 
      if (ret || hogPid == getpid())
      {
        CLog::Log(LOGERROR, "CCoreAudioDevice::SetHogStatus: "
          "Unable to release 'hog' status. Error = %s", GetError(ret).c_str());
        return false;
      }
      // Reset internal state
      m_HogPid = hogPid;
    }
  }
  return true;
}

pid_t CCoreAudioDevice::GetHogStatus()
{
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyHogMode;

  pid_t hogPid = -1;
  UInt32 size = sizeof(hogPid);
  AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &size, &hogPid); 

  return hogPid;
}

bool CCoreAudioDevice::SetMixingSupport(UInt32 mix)
{
  if (!m_DeviceId)
    return false;

  if (!GetMixingSupport())
    return false;

  int restore = -1;
  if (m_MixerRestore == -1)
  {
    // This is our first change to this setting. Store the original setting for restore
    restore = (GetMixingSupport() ? 1 : 0);
  }

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertySupportsMixing;

  UInt32 mixEnable = mix ? 1 : 0;
  CLog::Log(LOGDEBUG, "CCoreAudioDevice::SetMixingSupport: "
            "%sabling mixing for device 0x%04x", mix ? "En" : "Dis", (unsigned int)m_DeviceId);
  OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, sizeof(mixEnable), &mixEnable); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetMixingSupport: "
      "Unable to set MixingSupport to %s. Error = %s", mix ? "'On'" : "'Off'", GetError(ret).c_str());
    return false;
  }
  if (m_MixerRestore == -1)
    m_MixerRestore = restore;
  return true;
}

bool CCoreAudioDevice::GetMixingSupport()
{
  if (!m_DeviceId)
    return false;

  UInt32    size;
  UInt32    mix = 0;
  Boolean   writable = false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertySupportsMixing;

  OSStatus ret = AudioObjectIsPropertySettable(m_DeviceId, &propertyAddress, &writable);
  if (ret)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SupportsMixing: "
      "Unable to get propertyinfo mixing support. Error = %s", GetError(ret).c_str());
    writable = false;
  }

  if (writable)
  {
    size = sizeof(mix);
    ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &size, &mix); 
    if (ret != noErr)
      mix = 0;
  }

  CLog::Log(LOGERROR, "CCoreAudioDevice::SupportsMixing: "
    "Device mixing support : %s.", mix ? "'Yes'" : "'No'");

  return (mix > 0);
}

bool CCoreAudioDevice::SetCurrentVolume(Float32 vol)
{
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kHALOutputParam_Volume;

  OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, sizeof(Float32), &vol); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetCurrentVolume: "
      "Unable to set AudioUnit volume. Error = %s", GetError(ret).c_str());
    return false;
  }
  return true;
}

bool CCoreAudioDevice::GetPreferredChannelLayout(CCoreAudioChannelLayout& layout)
{
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyPreferredChannelLayout;

  UInt32 propertySize = 0;
  OSStatus ret = AudioObjectGetPropertyDataSize(m_DeviceId, &propertyAddress, 0, NULL, &propertySize); 
  if (ret)
    return false;

  void* pBuf = malloc(propertySize);
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, pBuf); 
  if (ret != noErr)
    CLog::Log(LOGERROR, "CCoreAudioDevice::GetPreferredChannelLayout: "
      "Unable to retrieve preferred channel layout. Error = %s", GetError(ret).c_str());
  else
  {
    // Copy the result into the caller's instance
    layout.CopyLayout(*((AudioChannelLayout*)pBuf));
  }
  free(pBuf);
  return (ret == noErr);
}

bool CCoreAudioDevice::GetDataSources(CoreAudioDataSourceList* pList)
{
  if (!pList || !m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyDataSources;

  UInt32 propertySize = 0;
  OSStatus ret = AudioObjectGetPropertyDataSize(m_DeviceId, &propertyAddress, 0, NULL, &propertySize); 
  if (ret != noErr)
    return false;

  UInt32  sources = propertySize / sizeof(UInt32);
  UInt32* pSources = new UInt32[sources];
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, pSources); 
  if (ret == noErr)
  {
    for (UInt32 i = 0; i < sources; i++)
      pList->push_back(pSources[i]);
  }
  delete[] pSources;
  return (!ret);
}

Float64 CCoreAudioDevice::GetNominalSampleRate()
{
  if (!m_DeviceId)
    return 0.0f;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyNominalSampleRate;

  Float64 sampleRate = 0.0f;
  UInt32  propertySize = sizeof(Float64);
  OSStatus ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, &sampleRate); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::GetNominalSampleRate: "
      "Unable to retrieve current device sample rate. Error = %s", GetError(ret).c_str());
    return 0.0f;
  }
  return sampleRate;
}

bool CCoreAudioDevice::SetNominalSampleRate(Float64 sampleRate)
{
  if (!m_DeviceId || sampleRate == 0.0f)
    return false;

  Float64 currentRate = GetNominalSampleRate();
  if (currentRate == sampleRate)
    return true; //No need to change

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyNominalSampleRate;

  OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, sizeof(Float64), &sampleRate); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetNominalSampleRate: "
      "Unable to set current device sample rate to %0.0f. Error = %s",
      (float)sampleRate, GetError(ret).c_str());
    return false;
  }
  CLog::Log(LOGDEBUG,  "CCoreAudioDevice::SetNominalSampleRate: "
    "Changed device sample rate from %0.0f to %0.0f.",
    (float)currentRate, (float)sampleRate);
  if (m_SampleRateRestore == 0.0f)
    m_SampleRateRestore = currentRate;

  return true;
}

UInt32 CCoreAudioDevice::GetNumLatencyFrames()
{
  UInt32 num_latency_frames = 0;
  if (!m_DeviceId)
    return 0;

  // number of frames of latency in the AudioDevice
  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyLatency;

  UInt32 i_param = 0;
  UInt32 i_param_size = sizeof(uint32_t);
  OSStatus ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &i_param_size, &i_param); 
  if (ret == noErr)
    num_latency_frames += i_param;

  // number of frames in the IO buffers
  propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &i_param_size, &i_param); 
  if (ret == noErr)
    num_latency_frames += i_param;

  // number for frames in ahead the current hardware position that is safe to do IO
  propertyAddress.mSelector = kAudioDevicePropertySafetyOffset;
  ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &i_param_size, &i_param); 
  if (ret == noErr)
    num_latency_frames += i_param;

  return (num_latency_frames);
}

UInt32 CCoreAudioDevice::GetBufferSize()
{
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;

  UInt32 size = 0;
  UInt32 propertySize = sizeof(size);
  OSStatus ret = AudioObjectGetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, &propertySize, &size); 
  if (ret != noErr)
    CLog::Log(LOGERROR, "CCoreAudioDevice::GetBufferSize: "
      "Unable to retrieve buffer size. Error = %s", GetError(ret).c_str());
  return size;
}

bool CCoreAudioDevice::SetBufferSize(UInt32 size)
{
  if (!m_DeviceId)
    return false;

  AudioObjectPropertyAddress  propertyAddress;
  propertyAddress.mScope    = kAudioDevicePropertyScopeOutput;
  propertyAddress.mElement  = 0;
  propertyAddress.mSelector = kAudioDevicePropertyBufferFrameSize;

  UInt32 propertySize = sizeof(size);
  OSStatus ret = AudioObjectSetPropertyData(m_DeviceId, &propertyAddress, 0, NULL, propertySize, &size); 
  if (ret != noErr)
  {
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetBufferSize: "
      "Unable to set buffer size. Error = %s", GetError(ret).c_str());
  }

  if (GetBufferSize() != size)
    CLog::Log(LOGERROR, "CCoreAudioDevice::SetBufferSize: Buffer size change not applied.");
  else
    CLog::Log(LOGDEBUG, "CCoreAudioDevice::SetBufferSize: Set buffer size to %d", (int)size);

  return (ret == noErr);
}

OSStatus CCoreAudioDevice::DirectRenderCallback(AudioDeviceID inDevice,
  const AudioTimeStamp  *inNow,
  const AudioBufferList *inInputData,
  const AudioTimeStamp  *inInputTime,
  AudioBufferList       *outOutputData,
  const AudioTimeStamp  *inOutputTime,
  void                  *inClientData)
{
  OSStatus ret = noErr;
  CCoreAudioDevice *audioDevice = (CCoreAudioDevice*)inClientData;

  if (audioDevice->m_pSource && audioDevice->m_frameSize)
  {
    UInt32 frames = outOutputData->mBuffers[audioDevice->m_OutputBufferIndex].mDataByteSize / audioDevice->m_frameSize;
    ret = audioDevice->m_pSource->Render(NULL, inInputTime, 0, frames, outOutputData);
  }
  else
  {
    outOutputData->mBuffers[audioDevice->m_OutputBufferIndex].mDataByteSize = 0;
  }

  return ret;
}
