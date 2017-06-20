// balb_assertiontracker.h                                            -*-C++-*-

#ifndef INCLUDED_BALB_ASSERTIONTRACKER
#define INCLUDED_BALB_ASSERTIONTRACKER

#ifndef INCLUDED_BSLS_IDENT
#include <bsls_ident.h>
#endif

//@PURPOSE: Provide a means to track where failed assertion occur.
//
//@CLASSES:
//   balb::AssertionTracker: capture information about failed assertions
//
//@DESCRIPTION: The 'balb::AssertionTracker' component keeps track of failed
// assertions and the stack traces leading to them.  It provides means by which
// such collection can be limited so as not to overwhelm processes where
// assertions fail frequently, and ways to examine the accumulated data.
//
///Thread Safety
///-------------
// This component is thread-safe and thread-enabled: it is safe to access and
// manipulate multiple distinct instances from different threads, and it is
// safe to access and manipulate a single shared instance from different
// threads.
//
///Usage
///-----
// This section illustrates intended use of this component.
//
///Example 1: Historic Broken Assertions
///- - - - - - - - - - - - - - - - - - -
// We have a function that has been running without assertions enabled.  We
// have reason to believe that the assertions will trigger if enabled, but that
// the effects of the violations will mostly be benignly erased (which is why
// the problems have not been detected so far).  We want to enable the
// assertions and fix the places that are causing them to trigger, but we do
// not want the program to abort each time an assertion occurs because it will
// slow down the task of information gathering, and because we need to gather
// the information in production.  We can use 'bsls::AssertionTracker' for
// this purpose.
// First, we will place a local staic 'AssertionTracker' object ahead of the
// function we want to instrument, and create custom assertion macros just for
// that function.
//..
//  namespace {
//  balb::AssertionTracker theTracker;
//  #define TRACK_ASSERT(condition) do { if (!(condition)) { \{{ Remove This }}
//  theTracker.assertionDetected(#condition, __FILE__, __LINE__); } } while (0)
//  }
//..
// Then, we define the function to be traced, and use the modified assertions.
//..
//   void foo(int percentage)
//       // Receive the specified 'percentage'.  The behavior is undefined
//       // unless '0 < percentage < 100'.
//   {
//       TRACK_ASSERT(  0 < percentage);
//       TRACK_ASSERT(100 > percentage);
//   }
//..
// Next, we create some uses of the function that may trigger the assertions.
//..
//   void useFoo()
//   {
//       for (int i = 0; i < 100; ++i) {
//           foo(i);
//       }
//       for (int i = 100; i > 0; --i) {
//           foo(i);
//       }
//   }
//..
// Finally, we prepare to track the assertion failures.  We will have the
// tracker report into a string stream object so that we can examine it.  We
// configure the tracker, trigger the assertions, and verify that they have
// been correctly discovered.
//..
//   bsl::ostringstream os;
//   theTracker.setReportingCallback(
//       bdlf::BindUtil::bind(balb::AssertionTracker::reportAssertion,
//                            &os, _1, _2, _3, _4, _5, _6));
//   theTracker.setReportingFrequency(
//                                  balb::AssertionTracker::e_onEachAssertion);
//   useFoo();
//   bsl::string report = os.str();
//   assert(report.npos != report.find("0 < percentage"));
//   assert(report.npos != report.find("100 > percentage"));
//..

#ifndef INCLUDED_BALSCM_VERSION
#include <balscm_version.h>
#endif

#ifndef INCLUDED_BSLMA_ALLOCATOR
#include <bslma_allocator.h>
#endif

#ifndef INCLUDED_BSLMT_MUTEX
#include <bslmt_mutex.h>
#endif

#ifndef INCLUDED_BSLMT_THREADUTIL
#include <bslmt_threadutil.h>
#endif

#ifndef INCLUDED_BSLS_ASSERT
#include <bsls_assert.h>
#endif

#ifndef INCLUDED_BSLS_ATOMIC
#include <bsls_atomic.h>
#endif

#ifndef INCLUDED_BSLS_LOG
#include <bsls_log.h>
#endif

#ifndef INCLUDED_BSLS_LOG
#include <bsls_logseverity.h>
#endif

#ifndef INCLUDED_BSL_FUNCTIONAL
#include <bsl_functional.h>
#endif

#ifndef INCLUDED_BSL_OSTREAM
#include <bsl_ostream.h>
#endif

#ifndef INCLUDED_BSL_UTILITY
#include <bsl_utility.h>
#endif

#ifndef INCLUDED_BSL_UNORDERED_MAP
#include <bsl_unordered_map.h>
#endif

#ifndef INCLUDED_BSL_VECTOR
#include <bsl_vector.h>
#endif

namespace BloombergLP {
namespace balb {

                            // ======================
                            // class AssertionTracker
                            // ======================

class AssertionTracker {
    // This class provides the ability to keep track of multiple assertion
    // failures and report them when they occur or cumulatively, including
    // stack traces to enable determiming the paths leading to failure.
    //
    // This class can be configured with a fallback handler, a configuration
    // callback, and a reporting callback.  It has sensible defaults if none
    // are specified:
    //
    //: fallback handler
    //: - set to 'bsls::Assert::failureHandler()'
    //:
    //: configuartion callback
    //: - retains configuration with no changes.
    //:
    //: reporting callback
    //: - log via 'bsls::Log::platformDefaultMessageHandler'
    //
    // How assertions are reported is determined by five configuration
    // parameters mediated by a client-controlled configuration callback.
    // This callback is invoked each time an assertion failure is reported via
    // 'reportAssertion' in order to make dynamic control of reporting
    // possible, for example by modifying BREGs.  The parameters are
    //
    //: maxAssertions (default -1)
    //: - The maximum number of assertion occurrences this object will handle,
    //: unlimited if set to -1.  If more assertions occur, they will be
    //: reported to the fallback handler and their stack traces will not be
    //: stored.
    //:
    //: maxLocations (default -1)
    //: - The maximum number of assertion locations (i.e., pairs of file name
    //: and line number) this object will handle, unlimited if set to -1.  If
    //: assertions at more locations occur, they will be reported to the
    //: fallback handler and their stack traces will not be stored.
    //:
    //: maxStackTracesPerLocation (default -1)
    //: - The maximum number of different stack traces that will be stored per
    //: assertion location, unlimited if -1.  (A stack trace is the path of
    //: function calls leading to the assertion location.  A given assertion
    //: location may be reached through many different paths.)  If more stack
    //: traces for a location occur, the assertion will be reported to the
    //: fallback handler and the stack traces will not be stored.
    //: 
    //: reportingSeverity (default 'bsls::LogSeverity::e_FATAL')
    //: - This severity value is passed to the reporting callback.
    //:
    //: reportingFrequency (default 'e_onNewStackTrace')
    //: - This parameter controls which assertion occurrences are reported
    //: via the reporting callback.
    //: The possible values are
    //:   1 'e_onEachAssertion' - All assertion occurrences are reported.
    //:
    //:   2 'e_onNewStackTrace' - The first time a new stack trace is seen, it
    //:       is reported.  Subsequent instances of the same stack trace are
    //:       counted but not reported.
    //:
    //:   3 'e_onNewLocation'   - The first time a new location (i.e., a pair
    //:       of file and line) is seen, it is reported.  Subsequent instances
    //:       of the same location, even if they have different stack traces,
    //:       are counted (by stack trace) but not reported.
  private:
    // PRIVATE TYPES
    typedef const char                              *Text;
    typedef const char                              *File;
    typedef int                                      Line;
    typedef bsl::pair<Text, bsl::pair<File, Line> >  AssertionLocation;
    typedef void                                    *Address;
    typedef bsl::vector<Address>                     StackTrace;
    typedef bsl::unordered_map<StackTrace, int>      AssertionCounts;
    typedef bsl::unordered_map<AssertionLocation, AssertionCounts>
        TrackingData;

  public:
    // PUBLIC TYPES
    enum ConfigurationOrder {
        e_maxAssertions,
        e_maxLocations,
        e_maxStackTracesPerLocation,
        e_severity,
        e_reportingFrequency
    };
    typedef bsl::function<void(int(&)[5])> ConfigurationCallback;
    typedef bsl::function<void(int,
                               int,
                               const char *,
                               const char *,
                               int,
                               const bsl::vector<void *>&)>
        ReportingCallback;

    enum ReportingFrequency {
        e_onNewLocation,    // Report once per distinct file/line pair
        e_onNewStackTrace,  // Report once per distinct stack trace
        e_onEachAssertion   // Report every assertion occurrence
    };

  private:
    // PRIVATE DATA
    bsls::Assert::Handler   d_fallbackHandler;  // handler when limits exceeded
    bslma::Allocator       *d_allocator_p;      // allocator
    bsls::AtomicInt         d_maxAssertions;              // configured limit
    bsls::AtomicInt         d_maxLocations;               // configured limit
    bsls::AtomicInt         d_maxStackTracesPerLocation;  // configured limit
    bsls::AtomicInt         d_severity;         // configured severity
    bsls::AtomicInt         d_assertionCount;   // number of assertions seen
    TrackingData            d_trackingData;     // store of assertions seen
    bslmt::ThreadUtil::Key  d_recursionCheck;   // thread-local data key to
                                                // prevent recursive invocation
    mutable bslmt::Mutex    d_mutex;            // mutual exclusion lock
    ConfigurationCallback   d_configurationCallback;
    ReportingCallback       d_reportingCallback;
    bsls::AtomicInt         d_reportingFrequency;

    // PRIVATE CREATORS
    AssertionTracker(const AssertionTracker&);             // = delete
        // Elided copy constructor.

    // PRIVATE MANIPULATORS
    AssertionTracker& operator=(const AssertionTracker&);  // = delete
        // Elided copy assignment operator.

  public:
    // CLASS METHODS
    static void preserveConfiguration(int (&)[5]);
        // This function can be installed as a configuration callback.  It
        // leaves the configuration unchanged.

    static void reportAssertion(bsl::ostream               *out,
                                int                         severity,
                                int                         count,
                                const char                 *text,
                                const char                 *file,
                                int                         line,
                                const bsl::vector<void *>&  stack);
        // Report the specified 'count', 'text', 'file', 'line', and 'stack' to
        // the specified stream 'out' at the specified 'severity'.  If 'out' is
        // null, the assertion will be reported via
        // 'bsls::Log::platformDefaultMessageHandler'.

    // CREATORS
    explicit AssertionTracker(
        bsls::Assert::Handler  fallback       = bsls::Assert::failureHandler(),
        ConfigurationCallback  configure      = preserveConfiguration,
        bslma::Allocator      *basicAllocator = 0);
        // Create an object of this type.  Optionally specify a 'fallback' used
        // to handle assertions that exceed configured limits.  If 'fallback'
        // is not given, the currently installed failure handler is used.
        // Optionally specify 'configure' to be used to reset configuration
        // each time an assertion occurs.  If 'configure' is not specified, the
        // configuration is left unchanged.  Optionally specify a
        // 'basicAllocator' used to supply memory.  If 'basicAllocator' is 0,
        // the currently installed default allocator is used.

    // MANIPULATORS
    void assertionDetected(const char *text, const char *file, int line);
        // This function is invoked to inform this object that an assertion
        // described by the specified 'text', 'file', and 'line' has occurred.
        // This object will refresh its configuration via its configuration
        // callback.  Depending on the configuration and the number and types
        // of assertions that have already been reported previously, this
        // assertion and its stack trace may be stored, and it may be reported
        // through the reporting callback, the fallback handler, or not at all.
        // See the class description above for how configuration controls this
        // behavior.

    void setConfigurationCallback(ConfigurationCallback cb);
        // Set the configuration callback function invoked when an assertion
        // occurs to the specified 'cb'.

    void setMaxAssertions(int value);
        // Set the maximum number of assertions that this object will handle to
        // the specified 'value'.  If 'value' is negative, an unlimited number
        // of assertions can be handled.  If there is an assertion failure
        // beyond the limit, the assertion will be passed to the saved handler.

    void setMaxLocations(int value);
        // Set the maximum number of assertion locations that this object will
        // handle to the specified 'value'.  If 'value' is negative, an
        // unlimited number of locations can be handled.  If there is an
        // assertion failure beyond the limit, the assertion will be passed to
        // the saved handler.

    void setMaxStackTracesPerLocation(int value);
        // Set the maximum number of stack traces for a given location that
        // this object will handle to the specified 'value'.  If 'value' is
        // negative, an unlimited number of stack traces can be handled.  If
        // there is an assertion failure beyond the limit, the assertion will
        // be passed to the saved handler.

    void setReportingCallback(ReportingCallback cb);
        // Set the callback function invoked when an assertion occurs to the
        // specified 'cb'.

    void setReportingFrequency(ReportingFrequency frequency);
        // Set the frequency with which assertions are reported to the
        // specified 'frequency'.  If 'frequency' is 'e_onNewLocation', an
        // assertion will be reported if it is the first time that file/line
        // assertion location has been seen.  If 'frequency' is
        // 'e_onNewStackTrace', an assertion will be reported if it is the
        // first time a particular stack trace has been seen (that is, an
        // assertion at a particular file/line location may have multiple stack
        // traces because the function in which it appears is called from a
        // variety of call paths).  Finally, if 'frequency' is
        // 'e_onEachAssertion', every assertion occurrence will be reported.
        // This reporting frequency is initially 'e_onNewStackTrace'.

    void setReportingSeverity(bsls::LogSeverity::Enum severity);
        // Set the severity level at which assertions will be reported.

    // ACCESSORS
    bslma::Allocator *allocator() const;
        // Return the allocator used by this object to supply memory.

    ConfigurationCallback configurationCallback() const;
        // Return the configuration callback used to configure this object.

    int maxAssertions() const;
        // Return the maximum number of assertions that this object will handle
        // or -1 if the number is unlimited.

    int maxLocations() const;
        // Return the maximum number of locations that this object will handle
        // or -1 if the number is unlimited.

    int maxStackTracesPerLocation() const;
        // Return the maximum number of stack traces for a given location that
        // this object will handle or -1 if the number is unlimited.

    bool onNewLocation() const;
        // Return whether the callback is invoked on each new assertion
        // location.

    bool onNewStackTrace() const;
        // Return whether the callback is invoked on each new assertion stack
        // trace;

    void reportAllRecordedStackTraces() const;
        // This method invokes the callback for each saved stack trace.

    ReportingCallback reportingCallback() const;
        // Return the callback functor used to report assertions.

    ReportingFrequency reportingFrequency() const;
        // Return the frequency with which assertions are reported.

    bsls::LogSeverity::Enum reportingSeverity() const;
        // Return the seveirty with which assertions are reported.
};

}  // close package namespace
}  // close enterprise namespace

#endif

// ----------------------------------------------------------------------------
// Copyright 2017 Bloomberg Finance L.P.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------- END-OF-FILE ----------------------------------
