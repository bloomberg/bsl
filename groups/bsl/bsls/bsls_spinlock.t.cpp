// bsls_spinlock.t.cpp
#include <bsls_spinlock.h>

#include <bsls_asserttest.h>   
#include <bsls_bsltestutil.h>  

#include <stdlib.h>  // atoi
#include <stdio.h>   // printf

using namespace BloombergLP;
using namespace std;

// ============================================================================
//                     STANDARD BSL ASSERT TEST FUNCTION
// ----------------------------------------------------------------------------

namespace {

int testStatus = 0;

void aSsErT(bool condition, const char *message, int line)
{
    if (condition) {
        printf("Error " __FILE__ "(%d): %s    (failed)\n", line, message);

        if (0 <= testStatus && testStatus <= 100) {
            ++testStatus;
        }
    }
}

}  // close unnamed namespace

// ============================================================================
//               STANDARD BSL TEST DRIVER MACRO ABBREVIATIONS
// ----------------------------------------------------------------------------

#define ASSERT       BSLS_BSLTESTUTIL_ASSERT
#define LOOP0_ASSERT BSLS_BSLTESTUTIL_LOOP0_ASSERT
#define LOOP1_ASSERT BSLS_BSLTESTUTIL_LOOP1_ASSERT
#define LOOP2_ASSERT BSLS_BSLTESTUTIL_LOOP2_ASSERT
#define LOOP3_ASSERT BSLS_BSLTESTUTIL_LOOP3_ASSERT
#define LOOP4_ASSERT BSLS_BSLTESTUTIL_LOOP4_ASSERT
#define LOOP5_ASSERT BSLS_BSLTESTUTIL_LOOP5_ASSERT
#define LOOP6_ASSERT BSLS_BSLTESTUTIL_LOOP6_ASSERT
#define ASSERTV      BSLS_BSLTESTUTIL_ASSERTV

#define Q            BSLS_BSLTESTUTIL_Q   // Quote identifier literally.
#define P            BSLS_BSLTESTUTIL_P   // Print identifier and value.
#define P_           BSLS_BSLTESTUTIL_P_  // P(X) without '\n'.
#define T_           BSLS_BSLTESTUTIL_T_  // Print a tab (w/o newline).
#define L_           BSLS_BSLTESTUTIL_L_  // current Line number

// ============================================================================
//                  NEGATIVE-TEST MACRO ABBREVIATIONS
// ----------------------------------------------------------------------------

#define ASSERT_SAFE_PASS(EXPR) BSLS_ASSERTTEST_ASSERT_SAFE_PASS(EXPR)
#define ASSERT_SAFE_FAIL(EXPR) BSLS_ASSERTTEST_ASSERT_SAFE_FAIL(EXPR)
#define ASSERT_PASS(EXPR)      BSLS_ASSERTTEST_ASSERT_PASS(EXPR)
#define ASSERT_FAIL(EXPR)      BSLS_ASSERTTEST_ASSERT_FAIL(EXPR)
#define ASSERT_OPT_PASS(EXPR)  BSLS_ASSERTTEST_ASSERT_OPT_PASS(EXPR)
#define ASSERT_OPT_FAIL(EXPR)  BSLS_ASSERTTEST_ASSERT_OPT_FAIL(EXPR)

           // ==============================================
           //                  USAGE EXAMPLE
           // ----------------------------------------------

// Suppose that we want to determine the maximum number of threads executing
// a block of code concurrently. Note that such a use case naturally calls
// for a statically initialized lock and the critical region involves
// a few integer operations; spinlock may be suitable.
//
// First, we define a type to manage the count within a scope:

class MaxConcurrencyCounter {
     // This type manages a count and high-water-mark within a scope.
     // It decrements the count in its destructor upon leaving the scope.

     // DATA
     int           *d_count_p;
     bsls::SpinLock *d_lock_p;

   public:
     // CREATORS
    MaxConcurrencyCounter(int *count, int *max, bsls::SpinLock *lock);
         // Acquire the specified 'lock' and increment the specified 'count'.
         // If the resulting value is larger than the specified 'max',
         // load it into 'max'. Release 'lock' and create a scoped guard to
         // decrement 'count' on destruction.

     ~MaxConcurrencyCounter();
         // Acquire the lock specified at construction, decrement the count
         // variable, and release the lock.
};

MaxConcurrencyCounter::MaxConcurrencyCounter(int            *count,
                                             int            *max,
                                             bsls::SpinLock *lock)
: d_count_p(count)
, d_lock_p(lock) {
    bsls::SpinLockGuard guard(lock);
    int result = ++(*count);
    if (result > *max) {
        *max = result;
    }
}

MaxConcurrencyCounter::~MaxConcurrencyCounter() {
    bsls::SpinLockGuard guard(d_lock_p);
    --(*d_count_p);
}

//=============================================================================
//                              MAIN PROGRAM
//-----------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    const int             test = argc > 1 ? atoi(argv[1]) : 0;
    const bool         verbose = argc > 2;
    const bool     veryVerbose = argc > 3;
    const bool veryVeryVerbose = argc > 4;

    printf("TEST " __FILE__ " CASE %d\n", test);

    switch (test) { case 0:
    case 1: {
        // --------------------------------------------------------------------
        // USAGE EXAMPLE
        //
        // Concern:
        //   Demonstrate the usage of this component.

        for (int i = 0; i < 2; ++i) {
            static int            threadCount = 0;
            static int            maxThreads = 0;
            static bsls::SpinLock threadLock = BSLS_SPINLOCK_UNLOCKED;

            // Next, by creating a 'MaxConcurrencyCounter' object, each thread
            // entering the block of code uses the 'SpinLock' to synchronize
            // manipulation of the static count variables:

            MaxConcurrencyCounter counter(&threadCount, &maxThreads,
                                          &threadLock);

            ASSERT(1 == threadCount);
            ASSERT(1 == maxThreads);

            // Finally, closing the block synchronizes on the 'SpinLock' again
            // to decrement the thread count. Any intervening code can run in
            // parallel.
        }
    } break;
        
      default: {
        fprintf(stderr, "WARNING: CASE `%d' NOT FOUND.\n", test);
        testStatus = -1;
      }
    }

    if (testStatus > 0) {
        fprintf(stderr, "Error, non-zero test status = %d.\n", testStatus);
    }

    return testStatus;
}

// ----------------------------------------------------------------------------
// Copyright 2015 Bloomberg Finance L.P.
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
