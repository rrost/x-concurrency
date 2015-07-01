/// @file spsc/queue.hpp
/// @brief Contains definition of single producer/single consumer thread-safe
///        lock-free fixed-size shared queue.
/// @copyright Licensed under the MIT License
/// @author Rostislav Ostapenko (rostislav.ostapenko@gmail.com)
/// @date 6-Apr-2014

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

#include <boost/assert.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/version.hpp>

// Unfortunately on Windows std::high_resolution_clock is not accurate enough, so using another
// bicyle implementation from StackOverflow.
#include "high_res_clock.hpp"

// Check Boost version. We need at least 1.55 to run properly.
#if BOOST_VERSION < 105500
#error This code has been developed and tested with Boost 1.55. It's not guaranteed to compile and run with earlier Boost versions.
#endif

namespace xcon
{

namespace spsc
{

// Fwd declaration of unit tests utility struct.
namespace queue_ut
{
   struct invariants_checker;
}

/// Thread-safe (single producer/single consumer) lock-free fixed-size queue.
///
/// @note The queue owns the elements that it contains.
///
///       Implementation idea:
///       Queue algorithm is lock-free and for several cases wait-free.
///       Selected solution is based on 2 independent preallocated queues - for producer and
///       consumer thread respectively. Each internal queue preallocates memory size equivalent
///       to queue size passed on construction.
///       Drawback is doubled memory consumption, but the benefit is good performance and
///       simplier algorithm. boost::circular_buffer is used for internal queues - it has
///       push_back()/pop_front() operations of constant time complexity.
///
///       Ideas to improve: parametrize with Allocator to be able to use custom allocators
///       with memory pools etc.
template <typename T> class queue
{
public:

   /// @enum YieldPolicy
   /// Describes how to yield CPU time in wait loop.
   enum yield_policy
   {
       spinning, ///< Don't yield CPU time in wait loop, just spin.
       yielding, ///< Yield CPU time in wait loop.
       sleeping  ///< Use sleep() in wait loop.
   };

   /// Constructor.
   ///
   /// @param[in] size Queue size.
   /// @throws std::length_error if provided size is too big or zero
   ///         std::bad_alloc if underlying container unable to allocate memory
   ///         block of requested size.
   explicit queue(size_t size, yield_policy policy = spinning)
      : yield_cpu_time_(
           policy == spinning ? yield_function([](){}) :
           (policy == yielding ? yield_function([](){ std::this_thread::yield(); }) :
              yield_function([](){ std::this_thread::sleep_for(std::chrono::milliseconds(1)); })))
      , max_size_(size)
      , count_(0)
      , queue1_(align_capacity_to_cache_line(size))
      , queue2_(align_capacity_to_cache_line(size))
      , producer_queue_(&queue1_)
      , consumer_queue_(&queue2_)
      , in_enqueue_(false)
      , in_dequeue_(false)
   {
      // Ensure queue storages are aligned to cache line size.
      BOOST_ASSERT(queue1_.capacity() * sizeof(raw_ptr_type) % cache_line_size == 0);
      BOOST_ASSERT(queue2_.capacity() * sizeof(raw_ptr_type) % cache_line_size == 0);

      if(size == 0)
      {
         throw std::length_error("Zero-sized queue is not allowed");
      }

      if(queue1_.capacity() < size)
      {
         // Oops, got overflow with alignment.
         throw std::length_error("Queue size is too big");
      }
   }

   /// Destructor.
   ///
   /// @note Destructor is NOT thread-safe, it's responsibility of caller to ensure no other
   ///       threads are accessing this object.
   ~queue()
   {
      destroy_objects(queue1_);
      destroy_objects(queue2_);
   }

   /// Prohibit copying.
   queue(const queue<T>&) = delete;
   queue<T>& operator=(const queue<T>&) = delete;

   /// Returns the number of elements contained in the Queue.
   size_t count() const
   {
      return count_;
   }

   /// Puts the item into the queue.
   ///
   /// @param[in] item Source item to put. Cannot be NULL.
   /// @note If the queue is full then this method blocks until there is the room
   ///       for the item again.
   ///       Item is owned by queue in case of success.
   ///       In case of failure item is not owned (not placed to queue) and it's responsibility
   ///       of caller to manage it.
   /// @throws std::invalid_argument if item is NULL.
   ///         std::runtime_error if reentrance is detected (more than one thread
   ///         called enqueue() for same object the same time).
   void enqueue(T* item)
   {
      enqueue_impl(item, wait_eternally);
   }

   /// Puts the item into the queue.
   ///
   /// @param[in] item Source item to put. Cannot be NULL.
   /// @param[in] milliseconds_timeout Number of milliseconds to wait.
   ///
   /// @returns 'true' if the operation was completed successfully, 'false'
   ///         if the operation timed out.
   ///
   /// @note If the queue is full then this method blocks until there is the room
   ///       for the item again or the operation timed out.
   ///       Item is owned by queue in case of success.
   ///       In case of failure item is not owned (not placed to queue) and it's responsibility
   ///       of caller to manage it.
   /// @throws std::invalid_argument if item is NULL.
   ///         std::runtime_error if reentrance is detected (more than one thread
   ///         called enqueue() for same object the same time).
   bool enqueue(T* item, int milliseconds_timeout)
   {
      return enqueue_impl(item, milliseconds_timeout);
   }

   /// Removes and returns the item at the beginning of the Queue.
   ///
   /// @note if the queue is empty then this method blocks until there is an item again.
   ///       Item is owned by caller in case of success.
   /// @throws std::runtime_error if reentrance is detected (more than one thread
   ///         called dequeue() for same object the same time).
   T* dequeue()
   {
      return dequeue_impl(wait_eternally);
   }

   /// Removes and returns the item at the beginning of the Queue.
   ///
   /// @param[in] milliseconds_timeout Number of milliseconds to wait.
   ///
   /// @returns The item at the betting of the Queue or NULL if the operation timed out.
   ///
   /// @note if the queue is empty then this method blocks until there is an item again
   ///       or the operation timed out.
   ///       Item is owned by caller in case of success.
   /// @throws std::runtime_error if reentrance is detected (more than one thread
   ///         called dequeue() for same object the same time).
   T* dequeue(int milliseconds_timeout)
   {
      return dequeue_impl(milliseconds_timeout);
   }

private:

   /// Internal types
   using value_type         = T;
   using raw_ptr_type       = T*;
   using smart_ptr_type     = std::unique_ptr<value_type>;
   using container_type     = boost::circular_buffer<raw_ptr_type>;
   using container_ptr_type = container_type*;
   using container_ref_type = container_type&;
   using reentrance_flag    = std::atomic<bool>;
   using yield_function     = void(*)();

   /// Implements enqueue() operation.
   ///
   /// @see enqueue()
   ///
   /// @note Special value (wait_eternally) can be passed as timeout to wait
   /// without timeout (eternally).
   bool enqueue_impl(T* item, int timeout_ms)
   {
      if(item == nullptr)
      {
         throw std::invalid_argument("NULL items not allowed");
      }

      reentrance_guard re_guard(in_enqueue_);
      smart_ptr_type item_guard(item);

      const size_t count = count_;
      if(count == max_size_)
      {
         // Queue is full, wait for dequeue or exit on timeout
         if(!wait_for_count_change(max_size_, timeout_ms))
         {
            // Release item, we don't want to loose it. It's responsibility of caller to manage
            // it in case of timeout
            item_guard.release();
            return false;
         }
      }
      else if(count == 0)
      {
         // It's safe to write directly into consumer queue because we know
         // that consumer thread is waiting for count change or even not entered dequeue()
         consumer_queue_->push_back(item_guard.get());
         item_guard.release();
         ++count_;
         return true;
      }

      // Protect producer queue to ensure consumer will not swap queues while
      // we are updating producer queue.
      // Get the producer queue and swap it with null.
      container_ref_type queue = *producer_queue_.exchange(nullptr);

      // Update producer queue.
      queue.push_back(item_guard.get());
      item_guard.release();

      // Restore shared producer queue ptr and increase item counter.
      producer_queue_ = &queue;
      ++count_;
      return true;
   }

   /// Implements dequeue() operation.
   ///
   /// @see dequeue()
   ///
   /// @note Special value (wait_eternally) can be passed as timeout to wait
   /// without timeout (eternally).
   T* dequeue_impl(int timeout_ms)
   {
      reentrance_guard re_guard(in_dequeue_);
      if(count_ == 0)
      {
         // Queue is empty, wait for enqueue or exit on timeout.
         if(!wait_for_count_change(0, timeout_ms))
         {
            return nullptr;
         }
      }

      // Consumer queue isn't empty - no need to sync, just dequeue and return.
      if(!consumer_queue_->empty())
      {
         return dequeue_from_consumer_queue().release();
      }

      // Consumer queue is empty but there are items in producer queue (because count_ is > 0).
      // Let's swap consumer and producer queue in lock-free style
      const container_ptr_type init_producer_queue = get_opposite_queue(consumer_queue_);
      container_ptr_type producer_queue = init_producer_queue;
      while(!producer_queue_.compare_exchange_strong(producer_queue, consumer_queue_))
      {
         // producer_queue can be overriden by nullptr if producer is updating now,
         // restore expected value.
         producer_queue = init_producer_queue;
      }

      // Now we swapped queues successfully, continue with dequeing from new consumer queue.
      consumer_queue_ = init_producer_queue;
      return dequeue_from_consumer_queue().release();
   }

   /// Gets opposite queue - consumer one for producer one and vice-versa.
   ///
   /// @param[in] queue Source queue.
   ///
   /// @returns Queue opposite to source.
   container_ptr_type get_opposite_queue(container_ptr_type const queue)
   {
      BOOST_ASSERT(queue == &queue1_ || queue == &queue2_);
      return queue == &queue1_ ? &queue2_ : &queue1_;
   }

   static const int wait_eternally = -1; ///< Magic timeout value meaning no timeout shall expire.

   /// Waits until queue item count is changed.
   ///
   /// @param[in] cur_count Current item count.
   /// @param[in] timeout_ms Timeout to wait in milliseconds, wait_eternally to wait eternally.
   ///
   /// @returns 'true' if count has been changed, 'false' if not and timeout expired.
   bool wait_for_count_change(size_t cur_count, int timeout_ms) const
   {
      BOOST_ASSERT(timeout_ms > 0 || timeout_ms == wait_eternally);

      using namespace std::chrono;
      const auto start_time = std_ex::high_resolution_clock::now();
      while(count_ == cur_count)
      {
         yield_cpu_time_();
         if(timeout_ms != wait_eternally)
         {
            const auto end_time = std_ex::high_resolution_clock::now();
            const milliseconds timeout = duration_cast<milliseconds>(end_time - start_time);
            if(timeout.count() >= timeout_ms)
            {
               return count_ != cur_count; // Last chance to not fail
            }
         }
      }
      return true;
   }

   /// Dequeue one item from consumer queue. Queue must be non-empty.
   ///
   /// @returns Smart pointer owning dequeued item.
   smart_ptr_type dequeue_from_consumer_queue()
   {
      BOOST_ASSERT(!consumer_queue_->empty());
      smart_ptr_type item_guard(consumer_queue_->front());
      consumer_queue_->pop_front();
      --count_;
      return item_guard;
   }

   /// Destroys items remained in queue.
   ///
   /// @param[in,out] queue Queue object to destroy items from.
   void destroy_objects(container_ref_type queue)
   {
      for(auto p : queue)
      {
         // If we need to use memory pool with queued items, we can use standard Allocator idiom
         // (http://en.cppreference.com/w/cpp/memory/allocator) - parametrize queue
         // template with custom memory pool Allocator trait type and
         // use Allocator::destroy() / Allocator::deallocate() instead of raw delete.
         delete p;
      }
   }

   /// Simple RAII reentrance guard.
   struct reentrance_guard
   {
      /// Constructs reentrance guard and sets protection flag.
      ///
      /// @throws std::runtime_error if reentrance detected.
      reentrance_guard(reentrance_flag& reentranceFlag): reentrance_flag_(reentranceFlag)
      {
         if(reentrance_flag_)
         {
            throw std::runtime_error("Reentrance not allowed. Only one thread at time "
               "can call enqueue() or dequeue()");
         }
         reentrance_flag_ = true;
      }

      /// Destructs reentrance guard and resets protection flag.
      ~reentrance_guard()
      {
         reentrance_flag_ = false;
      }

      reentrance_guard(const reentrance_guard&) = delete;
      reentrance_guard& operator=(const reentrance_guard&) = delete;

      reentrance_flag& reentrance_flag_; ///< Protection flag reference.
   };

   /// Friend utility class giving UTs access to invariants checking.
   friend struct queue_ut::invariants_checker;

   /// Checks queue invariants, asserts if something is going wrong. Used in UTs only.
   void check_invariants() const
   {
      BOOST_ASSERT(!in_dequeue_);
      BOOST_ASSERT(!in_enqueue_);
      BOOST_ASSERT(count_ <= max_size_);
      BOOST_ASSERT(count_ == (queue1_.size() + queue2_.size()));
      BOOST_ASSERT((consumer_queue_ == &queue1_ && producer_queue_ == &queue2_) ||
         (consumer_queue_ == &queue2_ && producer_queue_ == &queue1_));
   }

   /// Cache line size & corresponding padding type (to ensure padded data members
   /// will not be false-shared).
   static const size_t cache_line_size = 64;
   typedef char cache_line_padding[cache_line_size];

   /// Aligns capacity of queues to cache line size (to ensure it will not be false-shared
   /// with another queue).
   ///
   /// @param size[in] Initial capacity (internal queue size in items).
   ///
   /// @returns Aligned capacity.
   static size_t align_capacity_to_cache_line(size_t size)
   {
      size *= sizeof(raw_ptr_type);
      return (size + cache_line_size - size % cache_line_size) / sizeof(raw_ptr_type);
   }

   cache_line_padding pad1_;
   const yield_function yield_cpu_time_; ///< Function to yield CPU time.
   const size_t max_size_;               ///< Maximal size of queue.

   cache_line_padding pad2_;
   std::atomic_size_t count_; ///< Current size of queue. Shared between threads.

   cache_line_padding pad3_;
   container_type queue1_; ///< First internal queue.

   cache_line_padding pad4_;
   container_type queue2_; ///< Second internal queue.

   cache_line_padding pad5_;
   std::atomic<container_ptr_type> producer_queue_; ///< Producer queue pointer. Shared between threads.

   cache_line_padding pad6_;
   container_ptr_type consumer_queue_; ///< Consumer queue pointer.

   cache_line_padding pad7_;
   reentrance_flag in_enqueue_; ///< Enqueue reentrance protection flag.

   cache_line_padding pad8_;
   reentrance_flag in_dequeue_; ///< Dequeue reentrance protection flag.

}; // class queue

} // namespace spsc

} // namespace xcon
