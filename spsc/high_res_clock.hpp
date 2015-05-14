/// @file high_res_clock.hpp
/// @brief Contains definition of high_resolution_clock that uses precise
///        QueryPerformanceCounter() Win32 API.
///        Shamelessly stolen from StackOverflow:
///        http://stackoverflow.com/questions/16299029/resolution-of-stdchronohigh-resolution-clock-doesnt-correspond-to-measureme
/// @author Dave
/// @date 6-Apr-2014

#pragma once

#include <chrono>

#ifdef _WIN32
#include <windows.h>

namespace std_ex
{
   struct high_resolution_clock
   {
      typedef long long                                      rep;
      typedef std::nano                                      period;
      typedef std::chrono::duration<rep, period>             duration;
      typedef std::chrono::time_point<high_resolution_clock> time_point;
      static const bool is_steady = true;

      static long long getFrequency()
      {
         static const long long s_frequency = []() -> long long
         {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return frequency.QuadPart;
         }();
         return s_frequency;
      }

      static time_point now()
      {
         LARGE_INTEGER count;
         QueryPerformanceCounter(&count);
         return time_point(
             duration(count.QuadPart * static_cast<rep>(period::den) / getFrequency()));
      }
   };
}

#else

namespace std_ex
{
    using high_resolution_clock = std::chrono::high_resolution_clock;
}

#endif
