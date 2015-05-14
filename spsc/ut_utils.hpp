/// @file ut_utils.hpp
/// @brief Contains definition of various UT utilities
/// @copyright Licensed under the MIT License
/// @author Rostislav Ostapenko (rostislav.ostapenko@gmail.com)
/// @date 6-Apr-2014

#pragma once

#include <sstream>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "boost_test.hpp"

namespace boost
{
   /// Catches BOOST_ASSERT and represents it as Boost.Test failure
   void assertion_failed(char const * expr, char const * function, char const * file, long line)
   {
      std::stringstream msg;
      msg << "Assertion [" << expr << "] failed in function "
          << function << ", file " << file << "(" << line << ")";
      BOOST_CHECK_MESSAGE(false, msg.str());
   }
}

namespace xcon
{

namespace spsc
{

namespace queue_ut
{

   /// Simple utility class to count number of destructed objects
   struct delete_counter
   {
      static size_t& count()
      {
         static size_t delete_count = 0;
         return delete_count;
      }

      ~delete_counter()
      {
         ++count();
      }
   };

   /// Simple thread barrier class.
   /// Waits till N threads reach the barrier then releases them all.
   class barrier
   {
      std::mutex mutex_;
      std::condition_variable cv_;
      size_t count_;
      size_t wait_count_;
      const size_t init_count_;
   public:

      explicit barrier(size_t count): 
         count_(count)
         , wait_count_(0)
         , init_count_(count)
      {}

      void wait()
      {
         std::unique_lock<std::mutex> lock(mutex_);
         //std::cout << std::this_thread::get_id() << " wait, count=" << count_ << std::endl;

         // Reset count if it's already zero - starting another sync session.
         while(count_ == 0)
         {
            // If no threads are still waiting for release reset count.
            if(wait_count_ == 0)
            {
               count_ = init_count_;
               break;
            }

            // Otherwise release lock and let them awake
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
         }

         if(--count_ == 0)
         {
            cv_.notify_all();
         }
         else
         {
            //std::cout << std::this_thread::get_id() << " enter wait, count=" << count_ << std::endl;
            ++wait_count_;
            cv_.wait(lock, [this] { return count_ == 0; });
            --wait_count_;
         }
      }
   };

   /// Utility UT class, friend of xcon::spsc::queue. Has access to its internals to check
   /// invariants (security flaw by the way).
   struct invariants_checker
   {
      template <class T>
      static void check(const T& queue, barrier* wait_barrier = nullptr)
      {
         // If we are in multithreading test we need to ensure that both producer and consumer
         // are not inside any queue update function. Barrier is used for this purpose.
          if(wait_barrier)
         {
            wait_barrier->wait();
            queue.check_invariants();
            wait_barrier->wait();
         }
         else
         {
            queue.check_invariants();
         }
      }
   };

} // namespace queue_ut
} // namespace spsc
} // namespace xcon
