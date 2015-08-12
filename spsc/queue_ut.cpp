/// @file spsc/queue_ut.cpp
/// @brief Contains queue unit tests
/// @copyright Licensed under the MIT License
/// @author Rostislav Ostapenko (rostislav.ostapenko@gmail.com)
/// @date 6-Apr-2014

#define NOMINMAX

#include <algorithm>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <thread>
#include <type_traits>

#include <boost/predef.h>

#define BOOST_ENABLE_ASSERT_HANDLER
#define BOOST_TEST_MODULE xcon_spsc_queue_test
#include "boost_test.hpp"

#include "ut_utils.hpp"
#include "high_res_clock.hpp"
#include "queue.hpp"

namespace xcon
{

namespace spsc
{

namespace queue_ut
{

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // Basic Functionality Test

   /// Tests shared queue basic functionality, no threads envolved
   BOOST_AUTO_TEST_CASE(test_queue_basics)
   {
      typedef std::unique_ptr<int> smart_ptr;

      // Check ctor throwing
      BOOST_CHECK_THROW(queue<int>(0u), std::length_error);

      const size_t max_size = std::numeric_limits<size_t>::max();
      BOOST_CHECK_THROW((queue<int>(max_size)), std::length_error);

#if BOOST_ARCH_X86_64
      const size_t max_size_bad_alloc = 10000000000u;
#else
      const size_t max_size_bad_alloc = 1000000000u;
#endif

      BOOST_CHECK_THROW((queue<int>(max_size_bad_alloc)), std::bad_alloc);

      // Check enqueue NULL throwing
      BOOST_CHECK_THROW(queue<int>(1).enqueue(nullptr), std::invalid_argument);
      BOOST_CHECK_THROW(queue<int>(1).enqueue(nullptr, 1), std::invalid_argument);

      // Basic enqueue/dequeue
      {
         queue<int> q(1);
         BOOST_CHECK_EQUAL(q.count(), 0u);
         invariants_checker::check(q);

         auto p1 = new int(42);
         q.enqueue(p1);
         BOOST_CHECK_EQUAL(q.count(), 1u);
         invariants_checker::check(q);

         smart_ptr p2(q.dequeue());
         BOOST_CHECK_EQUAL(q.count(), 0u);
         BOOST_CHECK_EQUAL(p1, p2.get());
         invariants_checker::check(q);
      }

      // Enqueue/dequeue sequence
      {
         const int vals[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
         const auto count = std::extent<decltype(vals)>::value;

         queue<int> q(count);

         for(auto i : vals)
         {
            q.enqueue(new int(i));
            invariants_checker::check(q);
         }
         BOOST_CHECK_EQUAL(q.count(), count);
         invariants_checker::check(q);

         for(auto i = 0; i < count; ++i)
         {
            smart_ptr p(q.dequeue());
            BOOST_CHECK_EQUAL(*p, vals[i]);
            invariants_checker::check(q);
         }
         BOOST_CHECK_EQUAL(q.count(), 0u);
         invariants_checker::check(q);
      }

      // Queue cleanup on destruction
      const auto delete_count = 10u;
      {
         queue<delete_counter> q(delete_count);
         for(auto i = 0; i < delete_count; ++i)
         {
            q.enqueue(new delete_counter());
         }
         BOOST_CHECK_EQUAL(q.count(), delete_count);
         invariants_checker::check(q);
         delete_counter::count() = 0;
      }
      BOOST_CHECK_EQUAL(delete_counter::count(), delete_count);
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // High Load Test

   const auto high_load_item_count = 100500;

   /// Producer for checking high load work
   void high_load_producer(queue<int>& queue, std::atomic<bool>& producer_waits,
      barrier& wait_barrier)
   {
      for(auto i = 0; i < high_load_item_count; ++i)
      {
         queue.enqueue(new int(i));
         // Make a "spontaneous" check inside a loop
         if(i != 0 && i % 7531 == 0)
         {
            producer_waits = true;
            invariants_checker::check(queue, &wait_barrier);
         }
      }
   }

   // Consumer for checking high load work
   void high_load_consumer(queue<int>& queue, std::atomic<bool>& producer_waits,
      barrier& wait_barrier)
   {
      for(auto i = 0; i < high_load_item_count; ++i)
      {
         std::unique_ptr<int> p(queue.dequeue());
         BOOST_CHECK_EQUAL(*p, i);
         if(producer_waits)
         {
            producer_waits = false;
            invariants_checker::check(queue, &wait_barrier);
         }
      }
   }

   /// Tests shared queue under high load with producer and consumer threads
   BOOST_AUTO_TEST_CASE(test_queue_high_load)
   {
      const auto queue_size = 100u;
      queue<int> q(queue_size);

      std::atomic<bool> producer_waits;
      barrier wait_barrier(2);

      // Start producer first
      std::thread producer(high_load_producer, std::ref(q), std::ref(producer_waits),
          std::ref(wait_barrier));

      // Give him a chance to fill the queue
      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      BOOST_CHECK_EQUAL(q.count(), queue_size);

      // Start consumer
      std::thread consumer(high_load_consumer, std::ref(q), std::ref(producer_waits),
          std::ref(wait_barrier));

      // Wait while both producer and consumer are finished
      producer.join();
      consumer.join();

      BOOST_CHECK_EQUAL(q.count(), 0u);
      invariants_checker::check(q);
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // Low Latency Empty Queue Test

   using clock = std_ex::high_resolution_clock;
   using time_point = std_ex::high_resolution_clock::time_point;

   /// Producer for checking low latency empty queue
   void low_latency_producer(queue<int>& queue, time_point& time_point)
   {
      BOOST_CHECK_EQUAL(queue.count(), 0u);
      auto p = new int(42);
      time_point = clock::now();
      queue.enqueue(p);
   }

   /// Consumer for checking low latency empty queue
   void low_latency_consumer(queue<int>& queue, time_point& end_point, time_point& start_point)
   {
      BOOST_CHECK_EQUAL(queue.count(), 0u);
      start_point = clock::now();
      const auto p = queue.dequeue();
      end_point = clock::now();
      delete p;
   }

   /// Runs Empty Queue Low Latency test once
   double run_queue_low_latency_test()
   {
      queue<int> q(1);

      // Start consumer first on empty queue, it will block and wait for producer.
      time_point consumer_start_point;
      time_point end_point;
      std::thread consumer(low_latency_consumer, std::ref(q), std::ref(end_point),
          std::ref(consumer_start_point));

      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // Start producer then
      time_point producer_start_point;
      std::thread producer(low_latency_producer, std::ref(q), std::ref(producer_start_point));

      // Wait while both producer and consumer are finished.
      consumer.join();
      producer.join();

      BOOST_CHECK(consumer_start_point < producer_start_point);
      BOOST_CHECK_EQUAL(q.count(), 0u);
      invariants_checker::check(q);

      const std::chrono::duration<double, std::micro> period(end_point - producer_start_point);
      return period.count();
   }

   /// Tests shared queue empty queue low latency metric with producer and consumer threads
   /// running them several times. Outputs to stdout min. max, median and average
   /// latency time in microsecs.
   BOOST_AUTO_TEST_CASE(test_queue_low_latency)
   {
      const auto maxRuns = 1000;
      std::vector<double> latency;
      latency.reserve(maxRuns);

      for(auto i = 0; i < maxRuns; ++i)
      {
         latency.push_back(run_queue_low_latency_test());
      }

      if(latency.empty())
      {
          BOOST_FAIL("No latencies recorded");
      }

      std::sort(latency.begin(), latency.end());

      std::cout << std::fixed << std::setprecision(2) << std::endl;
      std::cout << "Empty queue latency (min): " << latency.front() << " microseconds" << std::endl;

      const auto median = latency.size() / 2;
      const bool oddSize = latency.size() % 2 != 0;
      std::cout << "Empty queue latency (med): "
         << (oddSize ? latency[median] : (latency[median] + latency[median - 1]) / 2)
         << " microseconds" << std::endl;

      if(latency.size() > 2)
      {
         const auto avg = std::accumulate(latency.begin() + 1, latency.end() - 1, 0.0) / (latency.size() - 2);
         std::cout << "Empty queue latency (avg): " << avg << " microseconds" << std::endl;
      }

      std::cout << "Empty queue latency (max): " << latency.back() << " microseconds" << std::endl;
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // Blocking Enqueue/Dequeue Timeout Expiration Test

   /// Tests shared queue timeout expiration
   BOOST_AUTO_TEST_CASE(test_queue_timeout_expiration)
   {
      using namespace std::chrono;

      const auto max_timeout = 1000;
      queue<int> q(1);

      // Test dequeueing with timeout
      for(auto i = 1; i <= max_timeout; i *= 10)
      {
         const auto start_point = clock::now();
         const auto p = q.dequeue(i);
         const auto end_point = clock::now();
         const auto timeout = duration_cast<milliseconds>(end_point - start_point);

         // Sometimes actual timeout can be a bit greater, ~1ms
         BOOST_CHECK(timeout.count() >= i);
         BOOST_CHECK(timeout.count() < i + 1);
         BOOST_CHECK(p == nullptr);
         invariants_checker::check(q);
      }

      BOOST_CHECK(q.enqueue(new int(66), 100));
      std::unique_ptr<int> p(new int(42));

      // Test enqueueing with timeout
      for(auto i = 1; i <= max_timeout; i *= 10)
      {
         const auto start_point = clock::now();
         const auto res = q.enqueue(p.get(), i);
         const auto end_point = clock::now();
         const auto timeout = duration_cast<milliseconds>(end_point - start_point);

         // Sometimes actual timeout can be a bit greater, ~1ms
         BOOST_CHECK(timeout.count() >= i);
         BOOST_CHECK(timeout.count() <= i + 1);
         BOOST_CHECK(*p == 42);
         BOOST_CHECK(!res);
         invariants_checker::check(q);
      }
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // Reentrance Throwing Test

   /// Tests shared queue throwing on reentrance
   BOOST_AUTO_TEST_CASE(test_queue_reentrance)
   {
      queue<int> q(1);

      // Test dequeue reentrance
      // Start consumer 1 on empty queue, it would block
      std::thread consumer1([&](){ delete q.dequeue(); });

      // Ensure consumer 1 is running and blocking
      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // Start consumer 2 on empty queue, it would throw
      std::thread consumer2([&](){ BOOST_CHECK_THROW(q.dequeue(), std::runtime_error); });

      // Ensure consumer 2 is running and throwing
      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // Release consumer 1 thread
      q.enqueue(new int(42));

      consumer1.join();
      consumer2.join();

      BOOST_CHECK_EQUAL(q.count(), 0u);
      invariants_checker::check(q);

      // Test enqueue reentrance

      q.enqueue(new int(66));

      // Start producer 1 on full queue, it would block
      std::thread producer1([&](){ q.enqueue(new int(42)); });

      // Ensure producer 1 is running and blocking
      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // Start producer 2 on full queue, it would throw
      std::thread producer2(
         [&]()
         {
            std::unique_ptr<int> p(new int(42));
            BOOST_CHECK_THROW(q.enqueue(p.get()), std::runtime_error);
         });

      // Ensure producer 2 is running and throwing
      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

      // Release producer 1 thread
      delete q.dequeue();
      producer1.join();
      producer2.join();

      BOOST_CHECK_EQUAL(q.count(), 1u);
      invariants_checker::check(q);
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
   // Throughput (ops/sec) Measuring Test

   static int dummy_obj = 42;

   // Dummy struct that will not envolve dynamic memory allocation.
   struct dummy_type
   {
      int dummy;

      void* operator new(size_t)
      {
         return &dummy_obj;
      }

      void operator delete(void*)
      {
      }
   };

   /// Producer for checking throughput
   void throughput_producer(queue<dummy_type>& queue, std::atomic<bool>& stop,
      size_t& opCount, time_point& start, time_point& end)
   {
      start = clock::now();
      for(;;)
      {
         queue.enqueue(new dummy_type());
         ++opCount;

         if(opCount % 1000 == 0 && stop)
         {
            break;
         }
      }
      end = clock::now();
   }

   // Consumer for checking throughput
   void throughput_consumer(queue<dummy_type>& queue, std::atomic<bool>& stop,
      size_t& opCount, time_point& start, time_point& end)
   {
      start = clock::now();
      for(;;)
      {
         const auto p = queue.dequeue();
         p;
         ++opCount;

         if(opCount % 1000 == 0 && stop)
         {
            break;
         }
      }
      end = clock::now();
   }

   /// Tests shared queue throughput
   BOOST_AUTO_TEST_CASE(test_queue_throughput)
   {
      using namespace std::chrono;
      queue<dummy_type> q(1000);

      std::atomic<bool> stop;
      size_t enq_count = 0;
      size_t deq_count = 0;
      time_point producer_start;
      time_point producer_end;
      time_point consumer_start;
      time_point consumer_end;

      // Start producer
      std::thread producer(throughput_producer, std::ref(q), std::ref(stop), std::ref(enq_count),
         std::ref(producer_start), std::ref(producer_end));

      // Start consumer
      std::thread consumer(throughput_consumer, std::ref(q), std::ref(stop), std::ref(deq_count),
         std::ref(consumer_start), std::ref(consumer_end));

      // TODO: delays aren't reliable, used for simplicity only.
      // Replace with correct synchronization!
      std::this_thread::sleep_for(std::chrono::seconds(1));
      stop = true;

      // Wait while both producer and consumer are finished.
      producer.join();
      consumer.join();

      const std::chrono::duration<double, std::milli> prod_time(producer_end - producer_start);
      const std::chrono::duration<double, std::milli> cons_time(consumer_end - consumer_start);

      const auto in_throughput = double(enq_count) / prod_time.count() * 1000.0;
      const auto out_throughput = double(deq_count) / cons_time.count() * 1000.0;

      invariants_checker::check(q);

      std::cout << std::fixed << std::setprecision(2) << std::endl;
      std::cout << "Queue input throughput:  " << in_throughput << " ops/sec" << std::endl;
      std::cout << "Queue output throughput: " << out_throughput << " ops/sec" << std::endl;
   }

} // namespace queue_ut
} // namespace spsc
} // namespace xcon
