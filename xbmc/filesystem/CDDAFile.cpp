/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CDDAFile.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "storage/MediaManager.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>

#include <sys/stat.h>

using namespace MEDIA_DETECT;
using namespace XFILE;

CFileCDDA::CFileCDDA(void) : m_cdio(CLibcdio::GetInstance())
{
  m_pCdIo = NULL;
  m_iSectorCount = 52;
}

CFileCDDA::~CFileCDDA(void)
{
  Close();
}

bool CFileCDDA::Open(const CURL& url)
{
  std::string strURL = url.GetWithoutFilename();

  if (!CServiceBroker::GetMediaManager().IsDiscInDrive(strURL) || !IsValidFile(url))
    return false;

  // Open the dvd drive
#ifdef TARGET_POSIX
  m_pCdIo = m_cdio->cdio_open(CServiceBroker::GetMediaManager().TranslateDevicePath(strURL).c_str(),
                              DRIVER_UNKNOWN);
#elif defined(TARGET_WINDOWS)
  m_pCdIo = m_cdio->cdio_open_win32(
      CServiceBroker::GetMediaManager().TranslateDevicePath(strURL, true).c_str());
#endif
  if (!m_pCdIo)
  {
    CLog::Log(LOGERROR, "file cdda: Opening the dvd drive failed");
    return false;
  }

  int iTrack = GetTrackNum(url);

  m_lsnStart = m_cdio->cdio_get_track_lsn(m_pCdIo, iTrack);
  m_lsnEnd = m_cdio->cdio_get_track_last_lsn(m_pCdIo, iTrack);
  m_lsnCurrent = m_lsnStart;

  if (m_lsnStart == CDIO_INVALID_LSN || m_lsnEnd == CDIO_INVALID_LSN)
  {
    m_cdio->cdio_destroy(m_pCdIo);
    m_pCdIo = NULL;
    return false;
  }

  return true;
}

bool CFileCDDA::Exists(const CURL& url)
{
  if (!IsValidFile(url))
    return false;

  int iTrack = GetTrackNum(url);

  if (!Open(url))
    return false;

  int iLastTrack = m_cdio->cdio_get_last_track_num(m_pCdIo);
  if (iLastTrack == CDIO_INVALID_TRACK)
    return false;

  return (iTrack > 0 && iTrack <= iLastTrack);
}

int CFileCDDA::Stat(const CURL& url, struct __stat64* buffer)
{
  if (Open(url) && buffer)
  {
    *buffer = {};
    buffer->st_size = GetLength();
    buffer->st_mode = _S_IFREG;
    Close();
    return 0;
  }
  errno = ENOENT;
  return -1;
}

ssize_t CFileCDDA::Read(void* lpBuf, size_t uiBufSize)
{
  if (!m_pCdIo || !CServiceBroker::GetMediaManager().IsDiscInDrive())
    return -1;

  if (uiBufSize > SSIZE_MAX)
    uiBufSize = SSIZE_MAX;

  // limit number of sectors that fits in buffer by m_iSectorCount
  int iSectorCount = std::min((int)uiBufSize / CDIO_CD_FRAMESIZE_RAW, m_iSectorCount);

  if (iSectorCount <= 0)
    return -1;

  // Are there enough sectors left to read
  if (m_lsnCurrent + iSectorCount > m_lsnEnd)
    iSectorCount = m_lsnEnd - m_lsnCurrent;

  // The loop tries to solve read error problem by lowering number of sectors to read (iSectorCount).
  // When problem is solved the proper number of sectors is stored in m_iSectorCount
  int big_iSectorCount = iSectorCount;
  while (iSectorCount > 0)
  {
    int iret = m_cdio->cdio_read_audio_sectors(m_pCdIo, lpBuf, m_lsnCurrent, iSectorCount);

    if (iret == DRIVER_OP_SUCCESS)
    {
      // If lower iSectorCount solved the problem limit it's value
      if (iSectorCount < big_iSectorCount)
      {
        m_iSectorCount = iSectorCount;
      }
      break;
    }

    // iSectorCount is low so it cannot solve read problem
    if (iSectorCount <= 10)
    {
      CLog::Log(LOGERROR,
                "file cdda: Reading {} sectors of audio data starting at lsn {} failed with error "
                "code {}",
                iSectorCount, m_lsnCurrent, iret);
      return -1;
    }

    iSectorCount = 10;
  }
  m_lsnCurrent += iSectorCount;

  return iSectorCount*CDIO_CD_FRAMESIZE_RAW;
}

int64_t CFileCDDA::Seek(int64_t iFilePosition, int iWhence /*=SEEK_SET*/)
{
  if (!m_pCdIo)
    return -1;

  lsn_t lsnPosition = (int)iFilePosition / CDIO_CD_FRAMESIZE_RAW;

  switch (iWhence)
  {
  case SEEK_SET:
    // cur = pos
    m_lsnCurrent = m_lsnStart + lsnPosition;
    break;
  case SEEK_CUR:
    // cur += pos
    m_lsnCurrent += lsnPosition;
    break;
  case SEEK_END:
    // end += pos
    m_lsnCurrent = m_lsnEnd + lsnPosition;
    break;
  default:
    return -1;
  }

  return ((int64_t)(m_lsnCurrent -m_lsnStart)*CDIO_CD_FRAMESIZE_RAW);
}

void CFileCDDA::Close()
{
  if (m_pCdIo)
  {
    m_cdio->cdio_destroy(m_pCdIo);
    m_pCdIo = NULL;
  }
}

int64_t CFileCDDA::GetPosition()
{
  if (!m_pCdIo)
    return 0;

  return ((int64_t)(m_lsnCurrent -m_lsnStart)*CDIO_CD_FRAMESIZE_RAW);
}

int64_t CFileCDDA::GetLength()
{
  if (!m_pCdIo)
    return 0;

  return ((int64_t)(m_lsnEnd -m_lsnStart)*CDIO_CD_FRAMESIZE_RAW);
}

bool CFileCDDA::IsValidFile(const CURL& url)
{
  // Only .cdda files are supported
  return URIUtils::HasExtension(url.Get(), ".cdda");
}

int CFileCDDA::GetTrackNum(const CURL& url)
{
  std::string strFileName = url.Get();

  // get track number from "cdda://local/01.cdda"
  return atoi(strFileName.substr(13, strFileName.size() - 13 - 5).c_str());
}

#define SECTOR_COUNT 52 // max. sectors that can be read at once
int CFileCDDA::GetChunkSize()
{
  return SECTOR_COUNT*CDIO_CD_FRAMESIZE_RAW;
}
