/*
**
** Copyright (C) 2012 Aldebaran Robotics
*/

// Test that calls happen in the correct event loop

#include <cstdint>

#include <boost/thread.hpp>

#include <gtest/gtest.h>
#include <qi/anyobject.hpp>
#include <qi/type/dynamicobjectbuilder.hpp>
#include <qi/type/objecttypebuilder.hpp>
#include <qi/application.hpp>


// ARGH! boost::thread::id is an opaque class, not an int, so we have
// to hack like hell to pass it in qimessaging calls: We pass along
// pointers to TID in an big-enough int type.

using TID = boost::thread::id;
using RawTID = std::uintptr_t;

bool sameThread(const RawTID& tid)
{
  boost::thread::id* id = reinterpret_cast<boost::thread::id*>(tid);
  bool res = *id == boost::this_thread::get_id();
  return res;
}

void fire_samethread(qi::AnyObject obj, void* tid)
{
  if (!tid)
    tid = new TID(boost::this_thread::get_id());
  obj.post("fire", reinterpret_cast<RawTID>(tid));
}

// Fire sameThread in given event loop
void fireSameThreadIn(qi::AnyObject obj, qi::EventLoop* el, void* tid)
{
  el->post(boost::bind(fire_samethread, obj, tid));
}

qi::AnyObject makeDynamicObj()
{
  qi::DynamicObjectBuilder ob;
  ob.setThreadingModel(qi::ObjectThreadingModel_MultiThread);
  ob.advertiseMethod("sameThread", &sameThread);
  ob.advertiseSignal<RawTID>("fire");
  qi::AnyObject res = ob.object();
  return res;
}

// Lets calls check that they run concurrently: each call waits, for a bounded
// duration, until the expected number of calls are in progress at the same
// time. Unlike timing the calls, this does not depend on the load of the
// machine.
class Rendezvous
{
public:
  void reset(int expected)
  {
    boost::unique_lock<boost::mutex> lock(_mutex);
    _expected = expected;
    _arrived = 0;
  }

  // Returns true if all the expected calls were in progress at the same time.
  bool meet()
  {
    boost::unique_lock<boost::mutex> lock(_mutex);
    ++_arrived;
    _arrivedChanged.notify_all();
    return _arrivedChanged.wait_for(lock, boost::chrono::seconds{10},
                                    [&] { return _arrived >= _expected; });
  }

private:
  boost::mutex _mutex;
  boost::condition_variable _arrivedChanged;
  int _expected = 0;
  int _arrived = 0;
};

Rendezvous& rendezvous()
{
  static Rendezvous r;
  return r;
}

bool meetRendezvous()
{
  return rendezvous().meet();
}

qi::AnyObject makeDynamicObjWithThreadModel(qi::ObjectThreadingModel model)
{
  static const auto sleep = [](qi::MilliSeconds dura){
    boost::this_thread::sleep_for(dura);
  };

  qi::DynamicObjectBuilder ob;
  ob.advertiseMethod("sameThread", &sameThread);
  ob.advertiseMethod("delayms", sleep);
  ob.advertiseMethod("delaymsThreadSafe", sleep, "", qi::MetaCallType_Queued);
  ob.advertiseMethod("rendezvousThreadSafe", &meetRendezvous, "", qi::MetaCallType_Queued);
  ob.advertiseMethod("delaymsFast", sleep, "", qi::MetaCallType_Direct);
  ob.advertiseSignal<RawTID>("fire");
  ob.setThreadingModel(model);
  qi::AnyObject res = ob.object();
  return res;
}

class EventObject {
public:
  bool sameThread(const RawTID& tid)
  {
    return ::sameThread(tid);
  }

  void delayms(qi::MilliSeconds delay)
  {
    boost::this_thread::sleep_for(delay);
  }
  bool rendezvous()
  {
    return meetRendezvous();
  }

  qi::Signal<RawTID> fire;

};

qi::AnyObject makeStaticObjWithThreadModel(EventObject &obj, qi::ObjectThreadingModel model)
{
  qi::ObjectTypeBuilder<EventObject>  ob;
  ob.advertiseMethod("sameThread", &EventObject::sameThread);
  ob.advertiseMethod("delayms", &EventObject::delayms);
  ob.advertiseMethod("delaymsThreadSafe", &EventObject::delayms, qi::MetaCallType_Queued);
  ob.advertiseMethod("rendezvousThreadSafe", &EventObject::rendezvous, qi::MetaCallType_Queued);
  ob.advertiseMethod("delaymsFast", &EventObject::delayms, qi::MetaCallType_Direct);
  ob.advertiseSignal("fire", &EventObject::fire);
  ob.setThreadingModel(model);
  qi::AnyObject res = ob.object(&obj, &qi::AnyObject::deleteGenericObjectOnly);
  return res;
}

// Calls the thread-safe method twice, and checks that the calls ran
// concurrently.
void expectConcurrentThreadSafeCalls(qi::AnyObject& obj)
{
  rendezvous().reset(2);
  qi::Future<bool> f1 = obj.async<bool>("rendezvousThreadSafe");
  qi::Future<bool> f2 = obj.async<bool>("rendezvousThreadSafe");
  EXPECT_TRUE(f1.value()) << "the calls did not run concurrently";
  EXPECT_TRUE(f2.value()) << "the calls did not run concurrently";
}

TEST(TestEventLoop, Basic)
{
  auto mainId = boost::this_thread::get_id();
  qi::AnyObject o1 = makeDynamicObj();
  // Call is synchronous, no reason not to
  ASSERT_TRUE(o1.call<bool>("sameThread", reinterpret_cast<RawTID>(&mainId)));
  // FIXME more!
}


TEST(TestThreadModel, notThreadSafe)
{
  qi::AnyObject o1 = makeDynamicObjWithThreadModel(qi::ObjectThreadingModel_SingleThread);
  qi::int64_t start = qi::os::ustime();
  qi::Future<void> f1 = o1.async<void>("delayms", qi::MilliSeconds{ 150 });
  o1.async<void>("delayms", qi::MilliSeconds{ 150 }).wait();
  f1.wait();
  // we expect >300ms result, take 10% marging to take into acount
  // timer granularity and sleep duration inprecision.
  ASSERT_GT(qi::os::ustime() - start, 270000);
}

TEST(TestThreadModel, ThreadSafe)
{
  qi::AnyObject o1 = makeDynamicObjWithThreadModel(qi::ObjectThreadingModel_MultiThread);
  expectConcurrentThreadSafeCalls(o1);
}

TEST(TestThreadModel, MethodModel)
{
  qi::AnyObject o1 = makeDynamicObjWithThreadModel(qi::ObjectThreadingModel_SingleThread);
  qi::int64_t start = qi::os::ustime();
  qi::Future<void> f1 = o1.async<void>("delaymsThreadSafe", qi::MilliSeconds{ 150 });
  ASSERT_LT(qi::os::ustime() - start, 100000);
  f1.wait();
  start = qi::os::ustime();
  // fast method->synchronous call
  f1 = o1.async<void>("delaymsFast", qi::MilliSeconds{ 150 });
  ASSERT_GT(qi::os::ustime() - start, 100000);
  ASSERT_TRUE(f1.isFinished());
  // Thread-safe method: parallel call
  expectConcurrentThreadSafeCalls(o1);
}

TEST(TestThreadModelStatic, notThreadSafeObjectStatic)
{
  EventObject e;
  qi::AnyObject o1 = makeStaticObjWithThreadModel(e, qi::ObjectThreadingModel_SingleThread);
  qi::int64_t start = qi::os::ustime();
  qi::Future<void> f1 = o1.async<void>("delayms", qi::MilliSeconds{ 150 });
  o1.async<void>("delayms", qi::MilliSeconds{ 150 }).wait();
  f1.wait();
  // we expect >300ms result, take 10% marging to take into acount
  // timer granularity and sleep duration inprecision.
  ASSERT_GT(qi::os::ustime() - start, 270000);
}

TEST(TestThreadModelStatic, ThreadSafe)
{
  EventObject e;
  qi::AnyObject o1 = makeStaticObjWithThreadModel(e, qi::ObjectThreadingModel_MultiThread);
  expectConcurrentThreadSafeCalls(o1);
}

TEST(TestThreadModelStatic, MethodModel)
{
  EventObject e;
  qi::AnyObject o1 = makeStaticObjWithThreadModel(e, qi::ObjectThreadingModel_SingleThread);
  qi::int64_t start = qi::os::ustime();
  qi::Future<void> f1 = o1.async<void>("delaymsThreadSafe", qi::MilliSeconds{ 150 });
  ASSERT_LT(qi::os::ustime() - start, 100000);
  f1.wait();
  start = qi::os::ustime();
  // fast method->synchronous call
  f1 = o1.async<void>("delaymsFast", qi::MilliSeconds{ 150 });
  ASSERT_GT(qi::os::ustime() - start, 100000);
  ASSERT_TRUE(f1.isFinished());
  // Thread-safe method: parallel call
  expectConcurrentThreadSafeCalls(o1);
}
