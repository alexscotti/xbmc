/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "VideoReferenceClock.h"

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/VideoSync.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

// Bounds for a refreshrate the clock is willing to divide by. Nothing a
// display presents is outside this, and a value outside it is a report taken
// mid-mode-set, not a rate.
namespace
{
// A vblank timestamp further than this from CurrentHostCounter() is not a
// late vblank, it is a different clock source.
constexpr int64_t MAX_VBLANK_TIME_SKEW_S = 60;
constexpr double MIN_SANE_REFRESHRATE = 10.0;
constexpr double MAX_SANE_REFRESHRATE = 1000.0;
constexpr double FALLBACK_REFRESHRATE = 60.0;
} // namespace

CVideoReferenceClock::CVideoReferenceClock() : CThread("RefClock")
{
  m_SystemFrequency = CurrentHostFrequency();
  m_ClockSpeed = 1.0;
  m_TotalMissedVblanks = 0;
  m_UseVblank = false;

  m_CurrTime = 0;
  m_LastIntTime = 0;
  m_CurrTimeFract = 0.0;
  m_RefreshRate = FALLBACK_REFRESHRATE; // never 0: every consumer divides by it
  m_MissedVblanks = 0;
  m_VblankTime = 0;
  m_vsyncStopEvent.Reset();

  Start();
}

CVideoReferenceClock::~CVideoReferenceClock()
{
  m_bStop = true;
  m_vsyncStopEvent.Set();
  StopThread();
}

void CVideoReferenceClock::Start()
{
  if (IsRunning())
    return;

  // Gate the vblank reference clock on the CoreELEC hardware-vsync toggle
  // (default on). Users who hit a regression with the AML hardware vsync clock
  // can fall back to legacy system-clock timing without rebuilding.
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (settings && !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK))
    return;

  Create();
}

void CVideoReferenceClock::UpdateClock(int NrVBlanks, uint64_t time)
{
  std::unique_lock lock(m_CritSection);

  // The vblank timestamp must be in the SAME epoch as CurrentHostCounter():
  // it becomes m_VblankTime, which GetTime() interpolates from and which the
  // teardown turns into the offset carried across every restart.
  //
  // Measured on a 3D display reset (2026-10-17): a timestamp arrived
  // ~14.19e15 ticks out - 164 days at this box's 1 GHz counter - while the
  // clock's own base was a sane 18.8 minutes of uptime. The offset carried
  // that epoch gap forward permanently, every later restart faithfully
  // re-applied it, and the DVD clock the audio compares against read 3941
  // hours. That is the -1.419e10 "large audio sync error" and the player
  // clock pinned at INT32_MAX with playback stuck, on six 3D titles.
  //
  // A timestamp this far from the host counter is not a late vblank, it is a
  // different clock source. Take the host counter instead; a slightly
  // mistimed vblank costs one frame of bookkeeping, the bad epoch costs the
  // session.
  const int64_t now = CurrentHostCounter();
  const int64_t supplied = static_cast<int64_t>(time);
  const int64_t away = std::abs(supplied - now);
  if (away > m_SystemFrequency * MAX_VBLANK_TIME_SKEW_S)
  {
    CLog::Log(LOGWARNING,
              "CVideoReferenceClock: vblank timestamp {} is {} ticks from the host counter {} "
              "- out of epoch, using the host counter",
              supplied, away, now);
    m_VblankTime = now;
  }
  else
  {
    m_VblankTime = time;
  }
  UpdateClockInternal(NrVBlanks, true);
}

void CVideoReferenceClock::Process()
{
  bool SetupSuccess = false;
  int64_t Now;
  int setupFailures = 0;

  while(!m_bStop)
  {
    m_pVideoSync = CServiceBroker::GetWinSystem()->GetVideoSync(this);

    if (m_pVideoSync)
    {
      SetupSuccess = m_pVideoSync->Setup();
      UpdateRefreshrate();
    }

    std::unique_lock SingleLock(m_CritSection);
    Now = CurrentHostCounter();
    // Re-anchor WITH the carried offset so the clock is continuous with what
    // GetTime returned while the vblank clock was down. A bare `= Now` here
    // jumps the master clock by whatever offset the vblank timeline had
    // accumulated; the players see that jump as a sync error and "correct" it
    // in whole-video-frame steps, physically walking video away from
    // passthrough audio on every videosync restart (BD playitem/mode churn).
    m_CurrTime = Now + m_TimeOffset;
    m_LastIntTime = m_CurrTime;
    m_CurrTimeFract = 0.0;
    m_ClockSpeed = 1.0;
    m_TotalMissedVblanks = 0;
    m_MissedVblanks = 0;

    if (SetupSuccess)
    {
      m_UseVblank = true;          //tell other threads we're using vblank as clock
      m_VblankTime = Now;          //initialize the timestamp of the last vblank
      SingleLock.unlock();

      // we might got signalled while we did not wait
      if (!m_vsyncStopEvent.Signaled())
      {
        //run the clock
        m_pVideoSync->Run(m_vsyncStopEvent);
        m_vsyncStopEvent.Reset();
      }
    }
    else
    {
      SingleLock.unlock();
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setup failed, falling back to CurrentHostCounter()");
    }

    SingleLock.lock();
    // Carry the vblank timeline's offset into the systemclock fallback so
    // GetTime stays continuous through the teardown (and through the next
    // setup, which re-applies it).
    const int64_t prevOffset = m_TimeOffset;
    m_TimeOffset = std::max(m_CurrTime, m_LastIntTime) - CurrentHostCounter();
    // Second line of defence: never carry an offset that is itself out of
    // epoch. Without this one corrupt timestamp poisons every subsequent
    // restart, because each one re-anchors m_CurrTime = Now + m_TimeOffset.
    if (std::abs(m_TimeOffset) > m_SystemFrequency * MAX_VBLANK_TIME_SKEW_S)
    {
      CLog::Log(LOGWARNING,
                "CVideoReferenceClock: refusing to carry offset {} (out of epoch) - dropping it",
                m_TimeOffset);
      m_TimeOffset = 0;
    }
    m_UseVblank = false;                       //we're back to using the systemclock
    // The offset is carried across every vblank-clock restart, and a 3D
    // display reset restarts it dozens of times in a session (39 stops in
    // one run, measured). If each restart hands on a slightly larger offset
    // the clock walks away, which is the remaining candidate for the 3.94 h
    // clock behind the -1.419e10 audio sync error. Log what is carried so
    // the term is named rather than guessed at.
    // LOGWARNING, not LOGDEBUG: debug logging is off on this box by policy
    // (forced debug logging is the confirmed cause of 250-1400 ms
    // presentation stalls), so a LOGDEBUG diagnostic would never print.
    CLog::Log(LOGWARNING,
              "CVideoReferenceClock: vblank clock down - carrying offset {} (was {}, delta {}), "
              "CurrTime {}",
              m_TimeOffset, prevOffset, m_TimeOffset - prevOffset, m_CurrTime);
    SingleLock.unlock();

    //clean up the vblank clock
    if (m_pVideoSync)
    {
      m_pVideoSync->Cleanup();
      m_pVideoSync.reset();
    }

    // A failed Setup used to permanently exit this thread - unreachable when
    // Setup could never fail, but the DRM vblank Setup CAN fail transiently
    // (a measured-rate restart or display reset landing mid-HDMI-mode-set),
    // and one transient failure then silently cost the vblank clock for the
    // whole session (review finding A8). Retry with backoff; give up only
    // after persistent failure.
    if (!SetupSuccess)
    {
      if (++setupFailures > 10)
      {
        CLog::Log(LOGERROR,
                  "CVideoReferenceClock: vblank clock setup failed {} times - "
                  "staying on the system clock for this session",
                  setupFailures);
        break;
      }
      CLog::Log(LOGWARNING,
                "CVideoReferenceClock: vblank clock setup failed ({}/10), retrying in 1s",
                setupFailures);
      if (m_vsyncStopEvent.Wait(std::chrono::milliseconds(1000)))
      {
        m_vsyncStopEvent.Reset();
        break;
      }
    }
    else
      setupFailures = 0;
  }
}

//this is called from the vblank run function and from CVideoReferenceClock::Wait in case of a late update
void CVideoReferenceClock::UpdateClockInternal(int NrVBlanks, bool CheckMissed)
{
  if (CheckMissed) //set to true from the vblank run function, set to false from Wait and GetTime
  {
    if (NrVBlanks < m_MissedVblanks) //if this is true the vblank detection in the run function is wrong
      CLog::Log(
          LOGDEBUG,
          "CVideoReferenceClock: detected {} vblanks, missed {}, refreshrate might have changed",
          NrVBlanks, m_MissedVblanks);

    NrVBlanks -= m_MissedVblanks; //subtract the vblanks we missed
    m_MissedVblanks = 0;
  }
  else
  {
    m_MissedVblanks += NrVBlanks;      //tell the vblank clock how many vblanks it missed
    m_TotalMissedVblanks += NrVBlanks; //for the codec information screen
    // double divide so fractional rates (23.976/29.97/59.94) don't round up to integer
    m_VblankTime += static_cast<int64_t>(
        static_cast<double>(m_SystemFrequency) / m_RefreshRate * NrVBlanks); //set the vblank time forward
  }

  if (NrVBlanks > 0) //update the clock with the adjusted frequency if we have any vblanks
  {
    double increment = UpdateInterval() * NrVBlanks;
    double integer   = floor(increment);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5); //make sure it gets correctly converted to int

    //accumulate what we lost due to rounding in m_CurrTimeFract, then add the integer part of that to m_CurrTime
    m_CurrTimeFract += increment - integer;
    integer          = floor(m_CurrTimeFract);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5);
    m_CurrTimeFract -= integer;
  }
}

double CVideoReferenceClock::UpdateInterval() const
{
  return m_ClockSpeed / m_RefreshRate * static_cast<double>(m_SystemFrequency);
}

//called from dvdclock to get the time
int64_t CVideoReferenceClock::GetTime(bool interpolated /* = true*/)
{
  std::unique_lock SingleLock(m_CritSection);

  //when using vblank, get the time from that, otherwise use the systemclock
  if (m_UseVblank)
  {
    int64_t  NextVblank;
    int64_t  Now;

    Now = CurrentHostCounter();        //get current system time
    NextVblank = TimeOfNextVblank();   //get time when the next vblank should happen

    // Bounded. Each pass credits one missed vblank, so the count needed is
    // (gap / vblank period); after a display reset the gap can be seconds,
    // which is thousands of legitimate passes. The bound only has to stop the
    // pathological case - a vblank period so small the loop can never catch
    // up - and 4 seconds' worth at 1 kHz is far above any real catch-up.
    int guard = 4000;
    while (Now >= NextVblank && guard-- > 0)
    {
      UpdateClockInternal(1, false); //update clock when next vblank should have happened already
      NextVblank = TimeOfNextVblank(); //get time when the next vblank should happen
    }
    if (guard <= 0)
    {
      // The clock cannot be trusted after this: re-anchor on the system clock
      // rather than hand the players an extrapolated value. Seen as a player
      // clock pinned at INT32_MAX with playback stuck (2026-10-17).
      CLog::Log(LOGWARNING,
                "CVideoReferenceClock: vblank catch-up did not converge at {:.3f} hertz - "
                "re-anchoring on the system clock",
                m_RefreshRate);
      m_TimeOffset = std::max(m_CurrTime, m_LastIntTime) - CurrentHostCounter();
      m_UseVblank = false;
      m_vsyncStopEvent.Set();
      return CurrentHostCounter() + m_TimeOffset;
    }

    if (interpolated)
    {
      //interpolate from the last time the clock was updated
      double elapsed = static_cast<double>(Now - m_VblankTime) * m_ClockSpeed;
      //don't interpolate more than 2 vblank periods
      elapsed = std::min(elapsed, UpdateInterval() * 2.0);

      //make sure the clock doesn't go backwards
      int64_t intTime = m_CurrTime + static_cast<int64_t>(elapsed);
      if (intTime > m_LastIntTime)
        m_LastIntTime = intTime;

      return m_LastIntTime;
    }
    else
    {
      return m_CurrTime;
    }
  }
  else
  {
    // Systemclock fallback carries the last vblank timeline's offset so the
    // returned time is continuous across vblank clock teardown/re-setup.
    int64_t time = CurrentHostCounter() + m_TimeOffset;
    if (time > m_LastIntTime)
      m_LastIntTime = time;
    return m_LastIntTime;
  }
}

void CVideoReferenceClock::SetSpeed(double Speed)
{
  std::unique_lock SingleLock(m_CritSection);
  //VideoPlayer can change the speed to fit the rereshrate
  if (m_UseVblank)
  {
    if (Speed != m_ClockSpeed)
    {
      m_ClockSpeed = Speed;
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Clock speed {:0.2f} %", m_ClockSpeed * 100.0);
    }
  }
}

double CVideoReferenceClock::GetSpeed()
{
  std::unique_lock SingleLock(m_CritSection);

  //VideoPlayer needs to know the speed for the resampler
  if (m_UseVblank)
    return m_ClockSpeed;
  else
    return 1.0;
}

void CVideoReferenceClock::UpdateRefreshrate()
{
  std::unique_lock SingleLock(m_CritSection);

  // EVERY consumer of m_RefreshRate divides by it - UpdateInterval() and
  // TimeOfNextVblank() both do - and this runs on every Setup, which means
  // every vblank-clock restart. A display reset on a 3D title restarts it
  // while the HDMI mode is still being renegotiated, and GetFps() reads the
  // gfx context, which has no meaningful rate at that moment. A zero or
  // absurd value there does not merely mistime a frame: it makes the clock's
  // own increments enormous, GetTime()'s catch-up loop advances m_CurrTime by
  // garbage, and the DVD clock the audio compares against explodes - measured
  // as "ActiveAE - large audio sync error: -1.4e10" followed by a player
  // clock pinned at INT32_MAX and playback stuck, on three separate 3D titles
  // (Wonder Woman/3D Atmos, Titanic/3D Part 1, Jurassic Park/3D, 2026-10-16/17).
  //
  // So a rate outside what any display can present is rejected and the last
  // good one kept. Keeping a stale rate costs at most slightly mistimed
  // vblank bookkeeping until the next Setup; taking the bad one costs the
  // session.
  const double reported = static_cast<double>(m_pVideoSync->GetFps());
  if (reported >= MIN_SANE_REFRESHRATE && reported <= MAX_SANE_REFRESHRATE)
  {
    m_RefreshRate = reported;
  }
  else
  {
    // Keeping the previous value is only safe if it is itself sane - the
    // constructor's starting value is not a measured rate.
    if (m_RefreshRate < MIN_SANE_REFRESHRATE || m_RefreshRate > MAX_SANE_REFRESHRATE)
      m_RefreshRate = FALLBACK_REFRESHRATE;

    CLog::Log(LOGWARNING,
              "CVideoReferenceClock: ignoring refreshrate {:.3f} hertz (outside {:.0f}-{:.0f}), "
              "using {:.3f}",
              reported, MIN_SANE_REFRESHRATE, MAX_SANE_REFRESHRATE, m_RefreshRate);
  }
  m_ClockSpeed = 1.0;

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: {:.3f} hertz", m_RefreshRate);
}

//VideoPlayer needs to know the refreshrate for matching the fps of the video playing to it
double CVideoReferenceClock::GetRefreshRate(double* interval /*= NULL*/)
{
  std::unique_lock SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    if (interval)
      *interval = m_ClockSpeed / m_RefreshRate;

    return m_RefreshRate;
  }
  else
    return -1;
}

#define MAXVBLANKDELAY 13LL
//guess when the next vblank should happen,
//based on the refreshrate and when the previous one happened
//increase that by 30% to allow for errors
int64_t CVideoReferenceClock::TimeOfNextVblank() const
{
  // Divide by the real refresh rate, not a rounded one: at 23.976 the integer
  // divide guesses a 24Hz period and expires the delay early. UpdateClockInternal()
  // and UpdateInterval() already advance the clock on the fractional rate.
  return m_VblankTime + static_cast<int64_t>(static_cast<double>(m_SystemFrequency) /
                                             m_RefreshRate * MAXVBLANKDELAY / 10.0);
}

//for the codec information screen
bool CVideoReferenceClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const
{
  std::unique_lock SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    MissedVblanks = m_TotalMissedVblanks;
    ClockSpeed = m_ClockSpeed;
    RefreshRate = m_RefreshRate;
    return true;
  }
  return false;
}
