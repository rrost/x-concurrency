# xcon::spsc::queue

Single Producer/Single Consumer Lock-free Shared Queue.<br>
Author Rostislav Ostapenko (rostislav.ostapenko@gmail.com).<br>
Licensed under the MIT License, 2014-2015.<br>

## Description

Queue algorithm is lock-free and for several cases wait-free.

Selected solution is based on 2 independent preallocated queues - for producer and
consumer thread respectively. Each internal queue preallocates memory size equivalent
to queue size passed on construction.

Drawback is doubled memory consumption, but the benefit is good performance and
simplier algorithm.

## Implementation Notes

Project requires C++11 compiler (VS2013 or higher, gcc 4.8 or higher) and Boost 1.55.
Please set BOOST environment variable to locate Boost library on your machine, e.g.
set BOOST=C:\Libs\boost_1_55_0

boost::circular_buffer is used for internal queues - it has push_back()/pop_front()
operations of constant time complexity.

Boost.Test (header only) is used for unit tests.

## Performance

Running x64 Release exe on on Win7 Pro x64 SP1, CPU Core i7-4770 3.40 GHz:

```
Empty queue latency (min): 0.00 microseconds (yep, QPC precision is not enough :-)
Empty queue latency (med): 0.30 microseconds
Empty queue latency (avg): 0.57 microseconds
Empty queue latency (max): 15.40 microseconds
```

```
Queue input throughput:  6404480.93 ops/sec
Queue output throughput: 6405051.45 ops/sec
```

So the total performance is ~12 million operations per second.
