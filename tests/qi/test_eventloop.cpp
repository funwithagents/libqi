#include <condition_variable>
#include <mutex>
#include <gtest/gtest.h>
#include <qi/eventloop.hpp>
#include <src/eventloop_p.hpp>
#include <ka/macro.hpp>
#include "test_future.hpp"

namespace
{
  // Overloading the eventloop with tasks that sleep makes its growth depend on
  // how long sleeps actually last, which varies a lot between machines (e.g.
  // `sleep_for(10ms)` can last 20-50ms on CI virtual machines). Instead, post
  // tasks that block until released, so that the eventloop stays overloaded
  // as long as needed, whatever the machine.
  void postBlockingTask(qi::EventLoopAsio& ev, const std::atomic<bool>& released)
  {
    ev.asyncCall(qi::Duration{0}, [&released]() {
      while (!released.load())
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    });
  }

  // Deadlines of the loops waiting for the eventloop to grow or shrink, so that
  // the tests fail instead of hanging.
  const auto growthTimeout = std::chrono::seconds{30};
  const auto shrinkTimeout = std::chrono::seconds{60};
}

int ping(int v)
{
  if (v>= 0)
    return v;
  else
    throw std::runtime_error("Invalid argument ");
}

static const auto gEventLoopName = "TestEventLoop";

TEST(EventLoop, EventLoopCanPostWithDuration)
{
  std::mutex m;
  std::condition_variable cv;

  auto cb = [&]
  {
    std::unique_lock<std::mutex> l{m};
    cv.notify_one();
  };

  qi::EventLoop loop{ gEventLoopName, 1 };
  {
    std::unique_lock<std::mutex> l{m};
KA_WARNING_PUSH()
KA_WARNING_DISABLE(4996, deprecated-declarations) // ignore use of deprecated overloads.
    loop.post(cb, qi::MilliSeconds{1});
KA_WARNING_POP()
    ASSERT_EQ(std::cv_status::no_timeout, cv.wait_for(l, std::chrono::milliseconds{100}));
  }
}

TEST(EventLoop, EventLoopCanAsyncDelay)
{
  qi::EventLoop loop{ gEventLoopName, 1 };
  loop.asyncDelay([] {}, qi::MilliSeconds{ 1 }).value(100);
}

TEST(EventLoop, asyncNoop)
{
  qi::async([]{}).value(100);
}

TEST(EventLoop, async)
{
  static const qi::MilliSeconds SMALL_CALL_DELAY{ 20 };
  static const qi::MilliSeconds BIG_CALL_DELAY{ 200 };
  static const int VALID_VALUE = 42;
  static const auto makeValidValue = [] { return 42; };
  static const auto makeError = [] { throw std::runtime_error("Voluntary Fail"); };

  qi::EventLoop& el = *qi::getEventLoop();
  {
    auto f = el.asyncDelay(makeValidValue, BIG_CALL_DELAY);
    EXPECT_FALSE(f.isFinished());
    f.wait();
    EXPECT_FALSE(f.hasError());
    EXPECT_EQ(f.value(), VALID_VALUE);
  }

  {
    auto f = el.asyncDelay(makeValidValue, BIG_CALL_DELAY);
    EXPECT_FALSE(f.isFinished());
    EXPECT_NO_THROW(f.cancel());
    EXPECT_EQ(f.wait(), qi::FutureState_Canceled);
  }

  {
    auto f = el.asyncDelay(makeError, BIG_CALL_DELAY);
    EXPECT_FALSE(f.isFinished());
    f.wait();
    EXPECT_TRUE(f.hasError());
  }

  // We cannot guarantee the minimum delay that an async call will take, but we can guarantee that it will
  // be systematically after the specified delay.

  {
    const auto beginTime = qi::SteadyClock::now();
    auto callTime = beginTime;
    auto f = el.asyncDelay([&]{ callTime = qi::SteadyClock::now(); }, SMALL_CALL_DELAY);
    f.wait(); // This test will timeout if it's not called in a reasonable time
    EXPECT_TRUE(f.isFinished());
    const auto timeUntilCall = callTime - beginTime;
    EXPECT_TRUE(timeUntilCall >= SMALL_CALL_DELAY);
  }

  {
    const auto beginTime = qi::SteadyClock::now();
    auto callTime = beginTime;
    auto f = el.asyncAt([&] { callTime = qi::SteadyClock::now(); }, qi::SteadyClock::now() + SMALL_CALL_DELAY);
    f.wait(); // This test will timeout if it's not called in a reasonable time
    EXPECT_TRUE(f.isFinished());
    const auto timeUntilCall = callTime - beginTime;
    EXPECT_TRUE(timeUntilCall >= SMALL_CALL_DELAY);
  }

  qi::async([]{}).value();
}

TEST(EventLoop, asyncFast)
{
  qi::EventLoop* el = qi::getEventLoop();
  for (int i = 0; i < 10; ++i)
  {
    qi::Future<int> f = el->async(get42);
    f.wait();
  }
}

// Algorithm:
//  1) Set the eventloop maximum number of tries after max thread count
//      has been reached.
//  2) Register a callback that will be run when this maximum number of
//      timeouts has been reached, and that will raise a flag.
//  3) Spam the eventloop until the flag is raised, and record the distinct
//      successive thread counts.
//  4) Check that the thread count has raised up to the max.
TEST(EventLoopAsio, CannotGoAboveMaximumThreadCount)
{
  using namespace qi;
  const int minThreadCount = 1;
  const int maxThreadCount = 8;
  const int threadCount = 4;
  const bool spawnOnOverload = true;

  // 1) Set max tries and create eventloop
  // It's ugly to set the value via an environment variable, but the current API
  // doesn't allow to do it another way.
  const std::string oldMaxTimeouts = os::getenv("QI_EVENTLOOP_MAX_TIMEOUTS");
  os::setenv("QI_EVENTLOOP_MAX_TIMEOUTS", "1");
  auto _ = ka::scoped([&]() {
    os::setenv("QI_EVENTLOOP_MAX_TIMEOUTS", oldMaxTimeouts.c_str());
  });
  // Declared before the eventloop, as its tasks use it until it is destroyed.
  std::atomic<bool> released{false};
  EventLoopAsio ev{threadCount, minThreadCount, maxThreadCount, "youp", spawnOnOverload};
  // Release the tasks before the eventloop destruction waits for them.
  auto releaseTasks = ka::scoped([&]() { released.store(true); });

  // 2) Register callback
  std::atomic<bool> emergencyCalled{false};
  ev._emergencyCallback = [&]() {
    emergencyCalled.store(true);
  };

  // 3) Spam eventloop and record distinct thread counts
  // We're going to keep track of the thread counts.
  std::vector<int> threadCounts{threadCount};

  auto pushIfDistinctThreadCount = [&threadCounts](const EventLoopAsio& ev) {
    const auto n = ev.workerCount();
    if (n != threadCounts.back())
    {
      threadCounts.push_back(n);
    }
  };

  // Spam the eventloop until the thread count goes up to the maximum.
  const auto deadline = std::chrono::steady_clock::now() + growthTimeout;
  while (!emergencyCalled.load())
  {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline)
      << "the eventloop did not reach its maximum size, current size: " << ev.workerCount();
    postBlockingTask(ev, released);
    pushIfDistinctThreadCount(ev);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  // 4) Check that the thread counts have gone up to the max
  auto b = threadCounts.begin();
  auto e = threadCounts.end();

  // The range must not be empty.
  ASSERT_NE(b, e);

  // The thread counts must have gone up.
  ASSERT_TRUE(std::is_sorted(b, e));

  // We must have reached the maximum thread count.
  ASSERT_EQ(maxThreadCount, *(e-1));
}

// We check that the eventloop gets bigger when overloaded, then shrinks when
// idle.
TEST(EventLoopAsio, DynamicShrinking)
{
  using namespace qi;
  const int minThreadCount = 2;
  const int maxThreadCount = 8;
  const int threadCount = 4;
  const bool spawnOnOverload = true;
  // Declared before the eventloop, as its tasks use it until it is destroyed.
  std::atomic<bool> released{false};
  EventLoopAsio ev{threadCount, minThreadCount, maxThreadCount, "youp", spawnOnOverload};
  // Release the tasks before the eventloop destruction waits for them.
  auto releaseTasks = ka::scoped([&]() { released.store(true); });

  // We're going to keep track of the thread counts.
  std::vector<int> threadCounts{threadCount};

  // Spam the eventloop until the thread count goes up to the maximum.
  const auto growthDeadline = std::chrono::steady_clock::now() + growthTimeout;
  while (ev.workerCount() != maxThreadCount)
  {
    ASSERT_LT(std::chrono::steady_clock::now(), growthDeadline)
      << "the eventloop did not reach its maximum size, current size: " << ev.workerCount();
    postBlockingTask(ev, released);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  // Release the tasks, so that the eventloop becomes idle and can shrink.
  released.store(true);

  auto pushIfDistinctThreadCount = [&threadCounts](EventLoopAsio const& ev) {
    const auto n = ev.workerCount();
    if (n != threadCounts.back())
    {
      threadCounts.push_back(n);
    }
  };

  // Wait until the thread count goes back down to the minimum, recording the
  // distinct thread counts.
  const auto shrinkDeadline = std::chrono::steady_clock::now() + shrinkTimeout;
  while (ev.workerCount() != minThreadCount)
  {
    ASSERT_LT(std::chrono::steady_clock::now(), shrinkDeadline)
      << "the eventloop did not shrink to its minimum size, current size: " << ev.workerCount();
    pushIfDistinctThreadCount(ev);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  pushIfDistinctThreadCount(ev);

  // Check that the recorded distinct thread counts strictly increase, then
  // strictly decrease.
  auto b = threadCounts.begin();
  auto e = threadCounts.end();

  // [b, i) is the increasing sub-range.
  auto i = std::is_sorted_until(b, e);

  // i must be in (b, e-1), i.e. sub-ranges must not be empty.
  ASSERT_LT(b, i);
  ASSERT_LT(i, e);

  // We must have peaked at the maximum thread count.
  ASSERT_EQ(maxThreadCount, *(i-1));

  // [i, e) must decrease.
  ASSERT_TRUE(std::is_sorted(i, e, std::greater<int>{}));

  // We must have gone down to the minimum thread count.
  ASSERT_EQ(minThreadCount, *(e-1));
}

TEST(EventLoop, posInBetween)
{
  using qi::detail::posInBetween;

  // percent
  ASSERT_EQ(  0, posInBetween(   0,       0,     100));
  ASSERT_EQ( 10, posInBetween(   0+3,    10+3,   100+3));
  ASSERT_EQ( 50, posInBetween(   0,      50,     100));
  ASSERT_EQ( 90, posInBetween(   0+35,   90+35,  100+35));
  ASSERT_EQ(100, posInBetween(   0,     100,     100));
  ASSERT_EQ(100, posInBetween(   0-100, 100-100, 100-100));

  // range of a single value
  ASSERT_EQ(100, posInBetween( 0, 0, 0));
  ASSERT_EQ(100, posInBetween(10,10,10));
  ASSERT_EQ(100, posInBetween(-3,-3,-3));

  // small ranges
  ASSERT_EQ(  0, posInBetween(0,   0,   2));
  ASSERT_EQ( 50, posInBetween(0,   1,   2));
  ASSERT_EQ(100, posInBetween(0,   2,   2));
  ASSERT_EQ(  0, posInBetween(0+5, 0+5, 2+5));
  ASSERT_EQ( 50, posInBetween(0-7, 1-7, 2-7));
  ASSERT_EQ(100, posInBetween(0+5, 2+5, 2+5));

  ASSERT_EQ(  0, posInBetween(0,   0,   3));
  ASSERT_EQ( 33, posInBetween(0,   1,   3));
  ASSERT_EQ( 66, posInBetween(0+7, 2+7, 3+7));
  ASSERT_EQ(100, posInBetween(0,   3,   3));

  ASSERT_EQ(  0, posInBetween( 0,    0,    4));
  ASSERT_EQ( 25, posInBetween( 0,    1,    4));
  ASSERT_EQ( 50, posInBetween( 0,    2,    4));
  ASSERT_EQ( 75, posInBetween( 0,    3,    4));
  ASSERT_EQ(100, posInBetween( 0+12, 4+12, 4+12));

  ASSERT_EQ( 20, posInBetween(0,   1,   5));
  ASSERT_EQ( 80, posInBetween(0,   4,   5));
  ASSERT_EQ( 20, posInBetween(0-6, 1-6, 5-6));
  ASSERT_EQ( 80, posInBetween(0+6, 4+6, 5+6));
}
