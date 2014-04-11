// btemt_tcptimereventmanager.cpp                                     -*-C++-*-
#include <btemt_tcptimereventmanager.h>

#include <bdes_ident.h>
BDES_IDENT_RCSID(btemt_tcptimereventmanager_cpp,"$Id$ $CSID$")

#include <bteso_defaulteventmanager.h>
#include <bteso_defaulteventmanager_devpoll.h>
#include <bteso_defaulteventmanager_epoll.h>
#include <bteso_defaulteventmanager_poll.h>
#include <bteso_defaulteventmanager_select.h>
#include <bteso_eventmanager.h>
#include <bteso_platform.h>
#include <bteso_socketimputil.h>

#include <bcemt_lockguard.h>
#include <bcemt_readlockguard.h>
#include <bcemt_writelockguard.h>

#include <bdet_timeinterval.h>
#include <bdetu_systemtime.h>

#include <bdef_function.h>
#include <bdef_bind.h>
#include <bdef_memfn.h>

#include <bdema_bufferedsequentialallocator.h>

#include <bslalg_typetraits.h>
#include <bslalg_typetraitusesbslmaallocator.h>
#include <bslma_autorawdeleter.h>
#include <bslma_default.h>
#include <bsls_assert.h>
#include <bsls_types.h>

#include <bsl_cstdio.h>                // printf
#include <bsl_ostream.h>

#include <bsl_c_errno.h>

#if defined(BSLS_PLATFORM_OS_UNIX)
#include <bsl_c_signal.h>              // sigfillset
#endif

using namespace bsl;  // automatically added by script

namespace BloombergLP {

enum {
    MAX_NUM_RETRIES = 3
};
                // ===============================================
                // class btemt_TcpTimerEventManager_ControlChannel
                // ===============================================

    // IMPLEMENTATION NOTES: This class manages a client-server pair of
    // connected sockets.  It is used to communicate requests to the dispatcher
    // thread while it is executing the dispatcher loop.  This communication is
    // done via sockets because this ensures that the loop is unblocked if it
    // is blocking in the socket polling step (when no timers are enqueued).

                   // ========================================
                   // class btemt_TcpTimerEventManager_Request
                   // ========================================

class btemt_TcpTimerEventManager_Request {
    // This class represents a request to the dispatcher thread.
    // It contains all the parameters associated with the request and,
    // depending on the request type, can be used to report results.  It is
    // simple by design, and does not use dynamic memory.  It is used by the
    // timer event manager to synchronize with the dispatcher thread, and is
    // enqueued onto a ControlChannel (connect pair of sockets) in order to
    // trigger the polling mechanism in the dispatcher thread loop, even if no
    // other event is scheduled.

  public:
    // TRAITS
    BSLALG_DECLARE_NESTED_TRAITS(btemt_TcpTimerEventManager_Request,
                                 bslalg::TypeTraitUsesBslmaAllocator);

    enum OpCode {
        NO_OP,                         // no operation
        TERMINATE,                     // exit signal
        DEREGISTER_ALL_SOCKET_EVENTS,  // invoke 'deregisterAllSocketEvents'
        DEREGISTER_ALL_TIMERS,         // invoke 'deregisterAllTimers'
        DEREGISTER_SOCKET_EVENT,       // invoke 'deregisterSocketEvent'
        DEREGISTER_SOCKET,             // invoke 'deregisterSocket'
        DEREGISTER_TIMER,              // invoke 'deregisterTimer'
        REGISTER_SOCKET_EVENT,         // invoke 'registerSocketEvent'
        EXECUTE,                       // invoke 'execute'
        REGISTER_TIMER,                // invoke 'registerTimer'
        RESCHEDULE_TIMER,              // invoke 'rescheduleTimer'
        IS_REGISTERED,                 // invoke 'isRegistered'.
        NUM_SOCKET_EVENTS              // invoke 'numSocketEvents'.
    };

  private:
    // DATA
    OpCode                        d_opCode;       // request type
    bcemt_Mutex                  *d_mutex_p;      // result notification
    bcemt_Condition              *d_condition_p;  //

    // The following two fields are used in socket related requests.
    bteso_SocketHandle::Handle    d_handle;       // socket handle associated
                                                  // with this request (in)
    bteso_EventType::Type         d_eventType;    // event code (in)

    // The following two fields are used in timer related requests.
    bdet_TimeInterval             d_timeout;      // timeout interval (in)
    void                         *d_timerId;      // timer ID (in/out)

    // The following field is used in both socket and timer related
    // registration requests.
    bteso_EventManager::Callback  d_callback;     // callback to be registered
                                                  // (in)

    // The following object is used for responses.
    int                           d_result;       // (out)

  public:
    // CREATORS
    btemt_TcpTimerEventManager_Request(
                      const bteso_SocketHandle::Handle&    handle,
                      bteso_EventType::Type                event,
                      const bteso_EventManager::Callback&  callback,
                      bslma::Allocator                    *basicAllocator = 0);
        // Create a 'REGISTER_SOCKET_EVENT' request containing the specified
        // socket 'handle', the specified 'event' and the specified
        // 'callback'.  Optionally specify a 'basicAllocator' used to supply
        // memory.  If 'basicAllocator' is 0, the currently installed default
        // allocator is used.

    btemt_TcpTimerEventManager_Request(
                      const bdet_TimeInterval&             timeout,
                      const bteso_EventManager::Callback&  callback,
                      bslma::Allocator                    *basicAllocator = 0);
        // Create a 'REGISTER_TIMER' request containing the specified 'timeout'
        // and the specified 'callback'.  Optionally specify a
        // 'basicAllocator' used to supply memory.  If 'basicAllocator' is 0,
        // the currently installed default allocator is used.

    btemt_TcpTimerEventManager_Request(
                                 const void               *timerId,
                                 const bdet_TimeInterval&  timeout,
                                 bslma::Allocator         *basicAllocator = 0);
        // Create a 'RESCHEDULE_TIMER' request containing the specified
        // 'timerId' and the specified 'timeOut'.  Optionally specify a
        // 'basicAllocator' used to supply memory.  If 'basicAllocator' is 0,
        // the currently installed default allocator is used.

    btemt_TcpTimerEventManager_Request(void             *timerId,
                                       bslma::Allocator *basicAllocator = 0);
        // Create a 'DEREGISTER_TIMER' request containing the specified
        // 'timerId'.  Optionally specify a 'basicAllocator' used to supply
        // memory.  If 'basicAllocator' is 0, the currently installed default
        // allocator is used.

    btemt_TcpTimerEventManager_Request(
                        const bteso_SocketHandle::Handle&  handle,
                        bslma::Allocator                  *basicAllocator = 0);
        // Create a 'DEREGISTER_SOCKET' request containing the specified
        // 'handle'.  Optionally specify a 'basicAllocator' used to supply
        // memory.  If 'basicAllocator' is 0, the currently installed default
        // allocator is used.

    btemt_TcpTimerEventManager_Request(
                               bteso_SocketHandle::Handle  handle,
                               bteso_EventType::Type       event,
                               bslma::Allocator           *basicAllocator = 0);
        // Create a 'DEREGISTER_SOCKET_EVENT' request containing the specified
        // 'handle' and the specified 'event'.  Optionally specify a
        // 'basicAllocator' used to supply memory.  If 'basicAllocator' is 0,
        // the currently installed default allocator is used.

    btemt_TcpTimerEventManager_Request(
                               bteso_SocketHandle::Handle  handle,
                               bteso_EventType::Type       event,
                               bcemt_Condition            *condition,
                               bcemt_Mutex                *mutex,
                               bslma::Allocator           *basicAllocator = 0);
        // Create an 'IS_REGISTERED' request containing the specified 'handle'
        // and the specified 'event'; the specified 'condition' is signaled
        // when the request is processed and the specified 'mutex' is used to
        // synchronize access to the result (i.e., 'result').  Optionally
        // specify a 'basicAllocator' used to supply memory.  If
        // 'basicAllocator' is 0, the currently installed default allocator is
        // used.

    btemt_TcpTimerEventManager_Request(
                        const bteso_SocketHandle::Handle&  handle,
                        bcemt_Condition                   *condition,
                        bcemt_Mutex                       *mutex,
                        bslma::Allocator                  *basicAllocator = 0);
        // Create a 'NUM_SOCKET_EVENTS' request containing the specified
        // 'handle' the specified 'condition' is signaled when the request is
        // processed and the specified 'mutex' is used to synchronize access to
        // the result (i.e., 'result').  Optionally specify a 'basicAllocator'
        // used to supply memory.  If 'basicAllocator' is 0, the currently
        // installed default allocator is used.

    btemt_TcpTimerEventManager_Request(OpCode            code,
                                       bslma::Allocator *basicAllocator = 0);
        // Create a request having the specified 'code'.  The behavior is
        // undefined unless 'code' is 'DEREGISTER_ALL_SOCKET_EVENTS',
        // 'DEREGISTER_ALL_TIMERS' or 'NO_OP'.  Optionally specify a
        // 'basicAllocator' used to supply memory.  If 'basicAllocator' is 0,
        // the currently installed default allocator is used.

    btemt_TcpTimerEventManager_Request(OpCode            code,
                                       bcemt_Condition  *condition,
                                       bcemt_Mutex      *mutex,
                                       bslma::Allocator *basicAllocator = 0);
        // Create a request with the specified 'code'; the request will be
        // processed according to the value of 'code'.  The behavior is
        // undefined if 'code' has a value that requires some fields other
        // than 'condition' or 'mutex' to be defined.  The treatment of the
        // specified 'condition' and 'mutex' depends on the value of 'opCode'.
        // Optionally specify a 'basicAllocator' used to supply memory.  If
        // 'basicAllocator' is 0, the currently installed default allocator is
        // used.

    btemt_TcpTimerEventManager_Request(
                      const bteso_EventManager::Callback&  callback,
                      bslma::Allocator                    *basicAllocator = 0);
        // Create an 'EXECUTE' request for the specified 'functor.  Optionally
        // specify a 'basicAllocator' used to supply memory.  If
        // 'basicAllocator' is 0, the currently installed default allocator is
        // used.

    ~btemt_TcpTimerEventManager_Request();
        // Destroy this request

    // MANIPULATORS
    void setTimerId(void *value);
        // Set the timer id contained in this request to the specified
        // 'value'.  The behavior is undefined unless 'opCode' is
        // 'REGISTER_TIMER'.

    void setResult(int value);
        // Set the result contained in this request to the specified
        // 'value'.  The behavior is undefined unless 'opCode' is
        // 'IS_REGISTERED'.

    void signal();
        // Signal the completion of the processing of this request.
        // The behavior is undefined unless 'opCode' refers to one of the
        // blocking request types.

    void waitForResult();
        // Suspend the calling thread until this request is processed
        // and the result is available.  The behavior is undefined unless
        // 'opCode' for this request refers to one of the blocking request
        // types.

    // ACCESSORS
    const bteso_EventManager::Callback& callback() const;
        // Return the callback contained in this request.  The behavior is
        // undefined unless 'opCode' is either 'REGISTER_SOCKET_EVENT' or
        // 'REGISTER_TIMER';

    bteso_EventType::Type event() const;
        // Return the socket event type contained in this request.  The
        // behavior is undefined unless 'opCode' is one of the following:
        // 'REGISTER_SOCKET_EVENT', 'DEREGISTER_SOCKET_EVENT'.

    OpCode opCode() const;
        // Return the op code for this request;

    const bteso_SocketHandle::Handle& socketHandle() const;
        // Return the socked handle contained in this request.  The behavior
        // is undefined unless 'opCode' is one of the following:
        // 'REGISTER_SOCKET_EVENT', 'DEREGISTER_SOCKET_EVENT',
        // 'DEREGISTER_SOCKET', 'NUM_SOCKET_EVENTS'.

    int result() const;
        // Return the result contained in this request.

    const bdet_TimeInterval& timeout() const;
        // Return the timeout value contained with this request.  The
        // behavior is undefined unless 'opCode' is 'REGISTER_TIMER' or
        // 'RESCHEDULE_TIMER'.

    const void *timerId() const;
        // Return the timer 'id' contained in this object.  The behavior is
        // undefined unless 'opCode' is 'REGISTER_TIMER', 'RESCHEDULE_TIMER'
        // or 'DEREGISTER_TIMER'.
};

                   // ----------------------------------------
                   // class btemt_TcpTimerEventManager_Request
                   // ----------------------------------------

// CREATORS
inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                           const bteso_SocketHandle::Handle&    handle,
                           bteso_EventType::Type                event,
                           const bteso_EventManager::Callback&  callback,
                           bslma::Allocator                    *basicAllocator)
: d_opCode(REGISTER_SOCKET_EVENT)
, d_mutex_p(0)
, d_condition_p(0)
, d_handle(handle)
, d_eventType(event)
, d_timerId((void *) 0)
, d_callback(callback, basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                           const bdet_TimeInterval&             timeout,
                           const bteso_EventManager::Callback&  callback,
                           bslma::Allocator                    *basicAllocator)
: d_opCode(REGISTER_TIMER)
, d_mutex_p(0)
, d_condition_p(0)
, d_timeout(timeout)
, d_timerId((void *) 0)
, d_callback(callback, basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                      const void               *timerId,
                                      const bdet_TimeInterval&  timeout,
                                      bslma::Allocator         *basicAllocator)
: d_opCode(RESCHEDULE_TIMER)
, d_mutex_p(0)
, d_condition_p(0)
, d_timeout(timeout)
, d_timerId(const_cast<void *>(timerId))
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                              void             *timerId,
                                              bslma::Allocator *basicAllocator)
: d_opCode(DEREGISTER_TIMER)
, d_mutex_p(0)
, d_condition_p(0)
, d_timerId(timerId)
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                             const bteso_SocketHandle::Handle&  handle,
                             bslma::Allocator                  *basicAllocator)
: d_opCode(DEREGISTER_SOCKET)
, d_mutex_p(0)
, d_condition_p(0)
, d_handle(handle)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                             const bteso_SocketHandle::Handle&  handle,
                             bcemt_Condition                   *condition,
                             bcemt_Mutex                       *mutex,
                             bslma::Allocator                  *basicAllocator)
: d_opCode(NUM_SOCKET_EVENTS)
, d_mutex_p(mutex)
, d_condition_p(condition)
, d_handle(handle)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                    bteso_SocketHandle::Handle  handle,
                                    bteso_EventType::Type       event,
                                    bslma::Allocator           *basicAllocator)
: d_opCode(DEREGISTER_SOCKET_EVENT)
, d_mutex_p(0)
, d_condition_p(0)
, d_handle(handle)
, d_eventType(event)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                    bteso_SocketHandle::Handle  handle,
                                    bteso_EventType::Type       event,
                                    bcemt_Condition            *condition,
                                    bcemt_Mutex                *mutex,
                                    bslma::Allocator           *basicAllocator)
: d_opCode(IS_REGISTERED)
, d_mutex_p(mutex)
, d_condition_p(condition)
, d_handle(handle)
, d_eventType(event)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                              OpCode            code,
                                              bslma::Allocator *basicAllocator)
: d_opCode(code)
, d_mutex_p(0)
, d_condition_p(0)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
    BSLS_ASSERT(NO_OP == code
                || TERMINATE == code
                || DEREGISTER_ALL_SOCKET_EVENTS == code
                || DEREGISTER_ALL_TIMERS == code);
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                                              OpCode            code,
                                              bcemt_Condition  *condition,
                                              bcemt_Mutex      *mutex,
                                              bslma::Allocator *basicAllocator)
: d_opCode(code)
, d_mutex_p(mutex)
, d_condition_p(condition)
, d_timerId((void *) 0)
, d_callback(basicAllocator)
, d_result(-1)
{
    BSLS_ASSERT(NO_OP == code || TERMINATE == code);
}

inline
btemt_TcpTimerEventManager_Request::btemt_TcpTimerEventManager_Request(
                           const bteso_EventManager::Callback&  callback,
                           bslma::Allocator                    *basicAllocator)
: d_opCode(EXECUTE)
, d_mutex_p(0)
, d_condition_p(0)
, d_timerId((void *) 0)
, d_callback(callback, basicAllocator)
, d_result(-1)
{
}

inline
btemt_TcpTimerEventManager_Request::~btemt_TcpTimerEventManager_Request()
{
}

// MANIPULATORS
inline
void btemt_TcpTimerEventManager_Request::setTimerId(void *value)
{
    bcemt_LockGuard<bcemt_Mutex> lock(d_mutex_p);
    d_timerId = value;
}

inline
void btemt_TcpTimerEventManager_Request::setResult(int value)
{
    bcemt_LockGuard<bcemt_Mutex> lock(d_mutex_p);
    d_result = value;
}

inline
void btemt_TcpTimerEventManager_Request::signal()
{
    BSLS_ASSERT(d_condition_p);
    d_condition_p->signal();
}

inline
void btemt_TcpTimerEventManager_Request::waitForResult()
{
    switch (d_opCode) {
      case NO_OP:
      case IS_REGISTERED:
      case NUM_SOCKET_EVENTS: {
        while (-1 == d_result) {
            d_condition_p->wait(d_mutex_p);
        }
      } break;
      case REGISTER_TIMER: {
        while (!d_timerId) {
            d_condition_p->wait(d_mutex_p);
        }
      } break;
      default: {
        BSLS_ASSERT("MUST BE UNREACHABLE BY DESIGN." && 0);
      } break;
    }
}

// ACCESSORS
inline
const bteso_EventManager::Callback&
btemt_TcpTimerEventManager_Request::callback() const
{
    BSLS_ASSERT(d_opCode == REGISTER_SOCKET_EVENT
                || d_opCode == REGISTER_TIMER
                || d_opCode == EXECUTE);
    return d_callback;
}

inline
bteso_EventType::Type btemt_TcpTimerEventManager_Request::event() const
{
    BSLS_ASSERT(REGISTER_SOCKET_EVENT == d_opCode
                || DEREGISTER_SOCKET_EVENT == d_opCode
                || IS_REGISTERED == d_opCode);
    return d_eventType;
}

inline
btemt_TcpTimerEventManager_Request::OpCode
btemt_TcpTimerEventManager_Request::opCode() const
{
    return d_opCode;
}

inline
const bteso_SocketHandle::Handle&
btemt_TcpTimerEventManager_Request::socketHandle() const
{
    BSLS_ASSERT(REGISTER_SOCKET_EVENT == d_opCode
                || DEREGISTER_SOCKET_EVENT == d_opCode
                || DEREGISTER_SOCKET == d_opCode
                || IS_REGISTERED == d_opCode
                || NUM_SOCKET_EVENTS == d_opCode);
    return d_handle;
}

inline
int btemt_TcpTimerEventManager_Request::result() const
{
    return d_result;
}

inline
const bdet_TimeInterval& btemt_TcpTimerEventManager_Request::timeout() const
{
    return d_timeout;
}

inline
const void *btemt_TcpTimerEventManager_Request::timerId() const
{
    BSLS_ASSERT(EXECUTE == d_opCode
                || REGISTER_TIMER == d_opCode
                || RESCHEDULE_TIMER == d_opCode
                || DEREGISTER_TIMER == d_opCode);
    return d_timerId;
}

inline
const char *toAscii(btemt_TcpTimerEventManager_Request::OpCode value)
{
#define CASE(X) case(X): return #X;

    switch (value) {
      CASE(btemt_TcpTimerEventManager_Request::NO_OP)
      CASE(btemt_TcpTimerEventManager_Request::EXECUTE)
      CASE(btemt_TcpTimerEventManager_Request::TERMINATE)
      CASE(btemt_TcpTimerEventManager_Request::DEREGISTER_ALL_SOCKET_EVENTS)
      CASE(btemt_TcpTimerEventManager_Request::DEREGISTER_ALL_TIMERS)
      CASE(btemt_TcpTimerEventManager_Request::DEREGISTER_SOCKET_EVENT)
      CASE(btemt_TcpTimerEventManager_Request::DEREGISTER_SOCKET)
      CASE(btemt_TcpTimerEventManager_Request::DEREGISTER_TIMER)
      CASE(btemt_TcpTimerEventManager_Request::REGISTER_SOCKET_EVENT)
      CASE(btemt_TcpTimerEventManager_Request::REGISTER_TIMER)
      CASE(btemt_TcpTimerEventManager_Request::IS_REGISTERED)
      CASE(btemt_TcpTimerEventManager_Request::NUM_SOCKET_EVENTS)
      default: return "(* UNKNOWN *)";
    }

#undef CASE
}

// FREE OPERATORS
bsl::ostream& operator<<(bsl::ostream& stream,
                         btemt_TcpTimerEventManager_Request::OpCode rhs)
{
    return stream << toAscii(rhs);
}

              // -----------------------------------------------
              // class btemt_TcpTimerEventManager_ControlChannel
              // -----------------------------------------------
int btemt_TcpTimerEventManager_ControlChannel::initialize()
{
#ifdef BTESO_PLATFORM_BSD_SOCKETS
    // Use UNIX domain sockets, if possible, rather than a standard socket
    // pair, to avoid using ephemeral ports for the control channel.  AIX and
    // Sun platforms have a more restrictive number of epheremal ports, and
    // several production machines have come close to that limit ({DRQS
    // 28135201<GO>}).  Note that the posix standard 'AF_LOCAL', is not
    // supported by a number of platforms -- use the legacy identifier,
    // 'AF_UNIX', instead.

    int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, d_fds);
#else
    int rc = bteso_SocketImpUtil::socketPair<bteso_IPv4Address>(
                                     d_fds,
                                     bteso_SocketImpUtil::BTESO_SOCKET_STREAM);

#endif
    if (rc) {
#ifdef BTESO_PLATFORM_WIN_SOCKETS
        d_fds[0] = d_fds[1] = INVALID_SOCKET;
#else
        d_fds[0] = d_fds[1] = -1;
#endif
        bsl::printf("%s(%d): Failed to create control channel"
                    " (errno = %d, rc = %d).\n",
                    __FILE__, __LINE__, errno, rc);
        return rc;                                                    // RETURN
    }

    bteso_IoUtil::setBlockingMode(d_fds[1],
                                  bteso_IoUtil::BTESO_NONBLOCKING,
                                  0);
    bteso_SocketOptUtil::setOption(d_fds[0],
                                   bteso_SocketOptUtil::BTESO_TCPLEVEL,
                                   bteso_SocketOptUtil::BTESO_TCPNODELAY,
                                   1);
    return 0;
}

// CREATORS
#ifdef BTESO_PLATFORM_BSD_SOCKETS
btemt_TcpTimerEventManager_ControlChannel::
                                    btemt_TcpTimerEventManager_ControlChannel()
: d_byte(0x53)
, d_numServerReads(0)
, d_numServerBytesRead(0)
{
    initialize();
}
#else
btemt_TcpTimerEventManager_ControlChannel::
                                     btemt_TcpTimerEventManager_ControlChannel(
                                bdef_Function<void (*)()> managerReinitFunctor)
: d_byte(0x53)
, d_numServerReads(0)
, d_numServerBytesRead(0)
, d_numReinitsAttempted(0)
, d_managerReinitFunctor(managerReinitFunctor)
{
    initialize();
}
#endif

// MANIPULATORS
int btemt_TcpTimerEventManager_ControlChannel::clientWrite(bool forceWrite)
{
    if (1 == ++d_numPendingRequests || forceWrite) {
        int errorNumber = 0;
        int rc;
        do {
            rc = bteso_SocketImpUtil::write(clientFd(),
                                            &d_byte,
                                            sizeof(char),
                                            &errorNumber);

            if (rc < 0) {
                --d_numPendingRequests;
                return rc;                                            // RETURN
            }
        } while (bteso_SocketHandle::BTESO_ERROR_INTERRUPTED == rc);
        if (rc >= 0) {
            return rc;
        }
        bsl::printf("%s(%d): Failed to communicate request to control channel"
                    " (errno = %d, errorNumber = %d, rc = %d).\n",
                    __FILE__, __LINE__, errno, errorNumber, rc);
        BSLS_ASSERT(errorNumber > 0);
        return -errorNumber;
    }
    return 0;
}

int btemt_TcpTimerEventManager_ControlChannel::serverRead()
{
    int rc = d_numPendingRequests.swap(0);
    char byte;

    int numBytes = bteso_SocketImpUtil::read(&byte, serverFd(), 1);

    ++d_numServerReads;
    if (numBytes > 0) {
        d_numServerBytesRead += numBytes;
    }
#ifdef BTESO_PLATFORM_WIN_SOCKETS
    else {
        // Control channel was killed.  Reinitialize the control  channel.

        d_managerReinitFunctor();
    }
#endif

    return rc;
}

#ifdef BTESO_PLATFORM_WIN_SOCKETS
int btemt_TcpTimerEventManager_ControlChannel::recreateSocketPair()
{
    BSLS_ASSERT_OPT(++d_numReinitsAttempted <= MAX_NUM_RETRIES);

    bcemt_WriteLockGuard<bcemt_RWMutex> guard(&d_socketPairLock);

    bteso_SocketHandle::Handle clientFd = d_fds[0];
    bteso_SocketHandle::Handle serverFd = d_fds[1];

    bteso_SocketImpUtil::close(serverFd);
    bteso_SocketImpUtil::close(clientFd);

    return initialize();
}
#endif

                         // --------------------------------
                         // class btemt_TcpTimerEventManager
                         // --------------------------------

// PRIVATE METHODS
void btemt_TcpTimerEventManager::initialize()
{
    BSLS_ASSERT(d_allocator_p);

    bteso_TimeMetrics *metrics = d_collectMetrics ? &d_metrics : 0;

    // Initialize the (managed) event manager.
#ifdef BSLS_PLATFORM_OS_LINUX
    if (bteso_DefaultEventManager<>::isSupported()) {
        d_manager_p = new (*d_allocator_p)
                           bteso_DefaultEventManager<>(metrics, d_allocator_p);
    }
    else {
        d_manager_p = new (*d_allocator_p)
                           bteso_DefaultEventManager<bteso_Platform::POLL>(
                                                       metrics, d_allocator_p);
    }
#else
    d_manager_p = new (*d_allocator_p)
                           bteso_DefaultEventManager<>(metrics, d_allocator_p);
#endif

    d_isManagedFlag = 1;

    // Initialize the functor containing the dispatch thread's entry point
    // method.
    d_dispatchThreadEntryPoint
        = bdef_Function<void (*)()>(
                bdef_MemFnUtil::memFn(
                      &btemt_TcpTimerEventManager::dispatchThreadEntryPoint
                    , this)
              , d_allocator_p);

    // Create the queue of executed timers.
    d_executeQueue_p = new (*d_allocator_p)
                        bsl::vector<bdef_Function<void (*)()> >(d_allocator_p);
    d_executeQueue_p->reserve(4);
}

int btemt_TcpTimerEventManager::initiateRead(bool executeInSameThread)
{
    if (executeInSameThread) {
        // Wait for the dispatcher thread to start and process
        // the request.

        bcemt_Mutex mutex;
        bcemt_Condition condition;

        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                     btemt_TcpTimerEventManager_Request::NO_OP,
                                     &condition,
                                     &mutex,
                                     d_allocator_p);
        BSLS_ASSERT(-1 == req->result());
        bcemt_LockGuard<bcemt_Mutex> lock(&mutex);

        d_requestQueue.pushBack(req);
        int ret = d_controlChannel_p->clientWrite(true);
        if (0 > ret) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
            return ret;                                               // RETURN
        }
        req->waitForResult();
        d_requestPool.deleteObjectRaw(req);
    }
    else {
        bcemt_ThreadUtil::Handle handle;
        bcemt_ThreadAttributes   attributes;
        attributes.setDetachedState(
                                bcemt_ThreadAttributes::BCEMT_CREATE_DETACHED);

        bdef_Function<void (*)()> writeFunctor = bdef_Function<void (*)()>(
                bdef_BindUtil::bindA(d_allocator_p,
                                     &btemt_TcpTimerEventManager::initiateRead,
                                     this,
                                     true));
        const int rc = bcemt_ThreadUtil::create(&handle,
                                                attributes,
                                                writeFunctor);
        BSLS_ASSERT_OPT(0 == rc);
    }
    return 0;
}

void btemt_TcpTimerEventManager::controlCb()
    // At least one request is pending on the queue.  Process as many
    // as there are.
{
    int numRequests = d_controlChannel_p->serverRead();

    BSLS_ASSERT(0 <= numRequests);
    BSLS_ASSERT((bcemt_LockGuard<bcemt_Mutex>(&d_requestQueue.mutex()),
                    numRequests <= d_requestQueue.queue().length()));

    for (int i = 0; i < numRequests; ++i) {
        btemt_TcpTimerEventManager_Request *req = d_requestQueue.popFront();

        switch (req->opCode()) {
            case btemt_TcpTimerEventManager_Request::TERMINATE: {
                BSLS_ASSERT(-1 == req->result());
                req->setResult(0);
                d_terminateThread = 1;
                req->signal();
            } break;
            case btemt_TcpTimerEventManager_Request::NO_OP: {
                BSLS_ASSERT(-1 == req->result());
                req->setResult(0);
                req->signal();
            } break;
            case btemt_TcpTimerEventManager_Request::REGISTER_SOCKET_EVENT: {
                d_manager_p->registerSocketEvent(req->socketHandle(),
                                                 req->event(),
                                                 req->callback());
                d_numTotalSocketEvents = d_manager_p->numEvents()-1;
                d_requestPool.deleteObjectRaw(req);
            } break;
            case btemt_TcpTimerEventManager_Request::EXECUTE:   // FALL THROUGH
            case btemt_TcpTimerEventManager_Request::REGISTER_TIMER: {
                BSLS_ASSERT(0 == req->timerId());
                d_requestPool.deleteObjectRaw(req);
            } break;
            case btemt_TcpTimerEventManager_Request::RESCHEDULE_TIMER: {
                BSLS_ASSERT(0 != req->timerId());
                d_requestPool.deleteObjectRaw(req);
            } break;
            case btemt_TcpTimerEventManager_Request::DEREGISTER_TIMER: {
                BSLS_ASSERT(0 != req->timerId());
                d_timerQueue.remove((int)(bsls::Types::IntPtr)req->timerId());
            } break;
            case btemt_TcpTimerEventManager_Request::DEREGISTER_ALL_TIMERS: {
                d_timerQueue.removeAll();
            } break;
            case btemt_TcpTimerEventManager_Request::
                                                DEREGISTER_ALL_SOCKET_EVENTS: {
                d_manager_p->deregisterAll();
            } break;
            case btemt_TcpTimerEventManager_Request::DEREGISTER_SOCKET_EVENT: {
                d_manager_p->deregisterSocketEvent(req->socketHandle(),
                                                   req->event());
                d_numTotalSocketEvents = d_manager_p->numEvents()-1;
                d_requestPool.deleteObjectRaw(req);
            } break;
            case btemt_TcpTimerEventManager_Request::DEREGISTER_SOCKET: {
                d_manager_p->deregisterSocket(req->socketHandle());
                d_numTotalSocketEvents = d_manager_p->numEvents()-1;
                d_requestPool.deleteObjectRaw(req);
            } break;
            case btemt_TcpTimerEventManager_Request::NUM_SOCKET_EVENTS: {
                int result = d_manager_p->numSocketEvents(req->socketHandle());
                req->setResult(result);
                req->signal();
            } break;
            case btemt_TcpTimerEventManager_Request::IS_REGISTERED: {
                req->setResult(
                    d_manager_p->isRegistered(req->socketHandle(),
                                              req->event()));
                req->signal();
            } break;
            default: {
                BSLS_ASSERT("MUST BE UNREACHABLE BY DESIGN" && 0);
            }
        }
    }
}

void btemt_TcpTimerEventManager::dispatchThreadEntryPoint()
{
    if (d_collectMetrics) {
        d_metrics.switchTo(bteso_TimeMetrics::BTESO_CPU_BOUND);
    }

    bsl::vector<bdef_Function<void (*)()> > *requestsPtr
        = new (*d_allocator_p)
                        bsl::vector<bdef_Function<void (*)()> >(d_allocator_p);
    requestsPtr->reserve(4);
    bslma::AutoRawDeleter< bsl::vector<bdef_Function<void (*)()> >
                        , bslma::Allocator>
                                    autoDelete(&requestsPtr, d_allocator_p, 1);

    // Set the state to BTEMT_ENABLED before dispatching any events
    // (DRQS 15212134).  Note that the thread calling 'enable' should be
    // blocked (awaiting a response on the control channel) and holding a write
    // lock to 'd_stateLock'.

    d_state = BTEMT_ENABLED;

    while (1) {
        // Dispatch socket events, from shorter to longer timeout.
        if (d_executeQueue_p->size()) {
            bdet_TimeInterval zeroTimeout;
            d_manager_p->dispatch(zeroTimeout, 0); // non-blocking
        } else if (d_timerQueue.length()) {
            bdet_TimeInterval timeout;
            d_timerQueue.minTime(&timeout);
            int rc = d_manager_p->dispatch(timeout, 0);  // blocking w/ timeout

#ifdef BTESO_PLATFORM_WIN_SOCKETS
            if (rc <= 0) {
                // Check if the control channel is still connected or not

                bteso_IPv4Address address;
                rc = bteso_SocketImpUtil::getPeerAddress(
                                               &address,
                                               d_controlChannel_p->serverFd());
                if (0 != rc) {
                    rc = reinitializeControlChannel();
                    BSLS_ASSERT_OPT(0 == rc);
                }
            }
#else
            (void) rc;  // quash warning
#endif
        }
        else {
            int rc = d_manager_p->dispatch(0);  // blocking indefinitely

#ifdef BTESO_PLATFORM_WIN_SOCKETS
            if (rc <= 0) {
                // Check if the control channel is still connected or not

                bteso_IPv4Address address;
                rc = bteso_SocketImpUtil::getPeerAddress(
                                               &address,
                                               d_controlChannel_p->serverFd());
                if (0 != rc) {
                    rc = reinitializeControlChannel();
                    BSLS_ASSERT_OPT(0 == rc);
                }
            }
#else
            (void) rc;  // quash warning
#endif
        }

        // Process executed callbacks (without timeouts), in respective order.
        {
            // A lock is necessary here, and not just an atomic pointer, since
            // we must guarantee that the pop_back in execute() must hold the
            // same functor that was enqueued, which would no longer be true if
            // swap below could occur between the push_back(functor) and
            // pop_back(functor).

            bcemt_LockGuard<bcemt_Mutex> lockGuard(&d_executeQueueLock);

            BSLS_ASSERT(0 == requestsPtr->size());
            bsl::swap(d_executeQueue_p, requestsPtr);
        }

        int numCallbacks = requestsPtr->size();
        for (int i = 0; i < numCallbacks; ++i) {
            (*requestsPtr)[i]();
        }
        requestsPtr->clear();

        // Process expired timers in increasing time order.
        if (d_timerQueue.length()) {
            const int NUM_TIMERS = 32;
            const int SIZE = NUM_TIMERS *
                sizeof(bcec_TimeQueueItem<bdef_Function<void (*)()> >);

            char BUFFER[SIZE];
            bdema_BufferedSequentialAllocator bufferAllocator(BUFFER, SIZE);

            bsl::vector<bcec_TimeQueueItem<bdef_Function<void (*)()> > >
                                                    requests(&bufferAllocator);
            d_timerQueue.popLE(bdetu_SystemTime::now(), &requests);
            int numTimers = requests.size();
            for (int i = 0; i < numTimers; ++i) {
                requests[i].data()();
            }
        }

        // If a signal to quit has been issued leave immediately,
        // but only after processing expired callbacks (above).
        // This guarantees that memory associated with open channels
        // is deallocated.
        {
            if (d_terminateThread.relaxedLoad()) {  // it is volatile
                if (d_collectMetrics) {
                    d_metrics.switchTo(bteso_TimeMetrics::BTESO_IO_BOUND);
                }
                BSLS_ASSERT(0 == d_requestQueue.queue().length());
                BSLS_ASSERT(BTEMT_ENABLED == d_state);
                return;
            }
        }
    }
    BSLS_ASSERT("MUST BE UNREACHABLE BY DESIGN." && 0);
}

#ifdef BTESO_PLATFORM_WIN_SOCKETS
int btemt_TcpTimerEventManager::reinitializeControlChannel()
{
    d_manager_p->deregisterSocket(d_controlChannel_p->serverFd());

    int rc = d_controlChannel_p->recreateSocketPair();
    BSLS_ASSERT_OPT(0 == rc);

    // Register the server fd of 'd_controlChannel_p' for READs.
    bteso_EventManager::Callback cb(
                  bdef_MemFnUtil::memFn(&btemt_TcpTimerEventManager::controlCb,
                                        this),
                  d_allocator_p);

    rc = d_manager_p->registerSocketEvent(d_controlChannel_p->serverFd(),
                                          bteso_EventType::BTESO_READ,
                                          cb);
    if (rc) {
        printf("%s(%d): Failed to register controlChannel for READ events"
               " in btemt_TcpTimerEventManager constructor\n",
               __FILE__, __LINE__);
        BSLS_ASSERT("Failed to register controlChannel for READ events" &&
                    0);
        return rc;
    }

    return initiateRead(false);
}
#endif

// CREATORS
btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                         bslma::Allocator *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(true)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                        bool               collectTimeMetrics,
                                        bslma::Allocator  *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(collectTimeMetrics)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                        bool               collectTimeMetrics,
                                        bool               poolTimerMemory,
                                        bslma::Allocator  *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(poolTimerMemory, threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(collectTimeMetrics)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                        Hint,
                                        bslma::Allocator  *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(true)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                        Hint,
                                        bool               collectTimeMetrics,
                                        bslma::Allocator  *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(collectTimeMetrics)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                        Hint,
                                        bool               collectTimeMetrics,
                                        bool               poolTimerMemory,
                                        bslma::Allocator  *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_timerQueue(poolTimerMemory, threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(collectTimeMetrics)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    initialize();
}

btemt_TcpTimerEventManager::btemt_TcpTimerEventManager(
                                       bteso_EventManager *rawEventManager,
                                       bslma::Allocator   *threadSafeAllocator)
: d_requestPool(sizeof(btemt_TcpTimerEventManager_Request),
                                                           threadSafeAllocator)
, d_requestQueue(threadSafeAllocator)
, d_dispatcher(bcemt_ThreadUtil::invalidHandle())
, d_state(BTEMT_DISABLED)
, d_terminateThread(0)
, d_manager_p(rawEventManager)
, d_isManagedFlag(0)
, d_timerQueue(threadSafeAllocator)
, d_metrics(bteso_TimeMetrics::BTESO_MIN_NUM_CATEGORIES,
            bteso_TimeMetrics::BTESO_IO_BOUND,
            threadSafeAllocator)
, d_collectMetrics(false)
, d_numTotalSocketEvents(0)
, d_allocator_p(bslma::Default::allocator(threadSafeAllocator))
{
    BSLS_ASSERT(rawEventManager);

    // Initialize the functor containing the dispatch thread's entry point
    // method.
    d_dispatchThreadEntryPoint
        = bdef_Function<void (*)()>(
                bdef_MemFnUtil::memFn( &btemt_TcpTimerEventManager
                                                     ::dispatchThreadEntryPoint
                                     , this)
              , d_allocator_p);

    // Create the queue of executed timers.
    d_executeQueue_p
        = new (*d_allocator_p)
                       bsl::vector<bdef_Function<void (*)()> >(d_allocator_p);
}

btemt_TcpTimerEventManager::~btemt_TcpTimerEventManager()
{
    disable();
    bsl::vector<bdef_Function<void (*)()> > *executeQueue = d_executeQueue_p;
    d_allocator_p->deleteObjectRaw(executeQueue);
    if (d_isManagedFlag) {
        d_allocator_p->deleteObjectRaw(d_manager_p);
    }
}

// MANIPULATORS
int btemt_TcpTimerEventManager::disable()
{
    if (d_state == BTEMT_DISABLED) {
        return 0;
    }

    if(bcemt_ThreadUtil::isEqual(bcemt_ThreadUtil::self(),
                                 d_dispatcher))
    {
        return 1;
    }

    bcemt_WriteLockGuard<bcemt_RWMutex> guard(&d_stateLock);
    {
        // Synchronized section.
        if (d_state == BTEMT_DISABLED) {
            return 0;
        }

        // Send dispatcher thread request to exit and wait until it
        // terminates, via 'join'.
        bcemt_Mutex mutex;
        bcemt_Condition condition;
        bcemt_ThreadUtil::Handle dispatcherHandle = d_dispatcher;

        btemt_TcpTimerEventManager_Request *req =
           new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                 btemt_TcpTimerEventManager_Request::TERMINATE,
                                 &condition,
                                 &mutex,
                                 d_allocator_p);
        d_requestQueue.pushBack(req);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
            return -1;
        }

        // Note that for this function, the wait for result is subsumed
        // by joining with the thread.
        int rc = bcemt_ThreadUtil::join(dispatcherHandle);

        BSLS_ASSERT(0 == rc);
        d_requestPool.deleteObjectRaw(req);
        d_state = BTEMT_DISABLED;

        // Release the control channel object.
        BSLS_ASSERT(0 != d_controlChannel_p);
        d_manager_p->deregisterSocket(d_controlChannel_p->serverFd());
        d_controlChannel_p.clear();
    }
    return 0;
}

int btemt_TcpTimerEventManager::enable(const bcemt_Attribute& attr)
{
    if(bcemt_ThreadUtil::isEqual(bcemt_ThreadUtil::self(), d_dispatcher)) {
        return 0;
    }

    if (BTEMT_ENABLED == d_state) {
        return 0;
    }

    bcemt_WriteLockGuard<bcemt_RWMutex> guard(&d_stateLock);
    {
        // Synchronized section.
        if (BTEMT_ENABLED == d_state) {
            return 0;
        }

        BSLS_ASSERT(0 == d_controlChannel_p);

        // Create control channel object.
#ifdef BTESO_PLATFORM_WIN_SOCKETS
        d_controlChannel_p.load(
              new (*d_allocator_p) btemt_TcpTimerEventManager_ControlChannel(
                  bdef_MemFnUtil::memFn(
                     &btemt_TcpTimerEventManager::reinitializeControlChannel,
                     this)),
              d_allocator_p);

        if (INVALID_SOCKET == d_controlChannel_p->serverFd()) {
#else
        d_controlChannel_p.load(
              new (*d_allocator_p) btemt_TcpTimerEventManager_ControlChannel(),
              d_allocator_p);

        if (-1 == d_controlChannel_p->serverFd()) {
#endif
            // Sockets were not successfully created.

            return -1;
        }


        // Register the server fd of 'd_controlChannel_p' for READs.
        bteso_EventManager::Callback cb(
                  bdef_MemFnUtil::memFn(&btemt_TcpTimerEventManager::controlCb,
                                        this),
                  d_allocator_p);

        int rc = d_manager_p->registerSocketEvent(
                                                d_controlChannel_p->serverFd(),
                                                bteso_EventType::BTESO_READ,
                                                cb);
        if (rc) {
            printf("%s(%d): Failed to register controlChannel for READ events"
                    " in btemt_TcpTimerEventManager constructor\n",
                    __FILE__, __LINE__);
            BSLS_ASSERT("Failed to register controlChannel for READ events" &&
                        0);
            return rc;
        }

#if defined(BSLS_PLATFORM_OS_UNIX)
        sigset_t newset, oldset;
        sigfillset(&newset);
        static const int synchronousSignals[] = {
            SIGBUS,
            SIGFPE,
            SIGILL,
            SIGSEGV,
            SIGSYS,
            SIGABRT,
            SIGTRAP,
        #if !defined(BSLS_PLATFORM_OS_CYGWIN) || defined(SIGIOT)
            SIGIOT
        #endif
         };
        static const int SIZE = sizeof synchronousSignals
                                                  / sizeof *synchronousSignals;
        for (int i = 0; i < SIZE; ++i) {
            sigdelset(&newset, synchronousSignals[i]);
        }

        pthread_sigmask(SIG_BLOCK, &newset, &oldset);
#endif

        d_terminateThread = 0;
        rc = bcemt_ThreadUtil::create((bcemt_ThreadUtil::Handle*)&d_dispatcher,
                                      attr,
                                      d_dispatchThreadEntryPoint);

#if defined(BSLS_PLATFORM_OS_UNIX)
        // Restore the mask.
        pthread_sigmask(SIG_SETMASK, &oldset, &newset);
#endif
        if (rc) {
            return rc;
        }
        return initiateRead(true);
    }
    return 0;
}

int btemt_TcpTimerEventManager::registerSocketEvent(
        const bteso_SocketHandle::Handle&           handle,
        bteso_EventType::Type                       event,
        const bteso_TimerEventManager::Callback&    callback)
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher))
    {
        int rc = d_manager_p->registerSocketEvent(handle, event, callback);
        d_numTotalSocketEvents = d_manager_p->numEvents()-1;
        return rc;
    }

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                handle,
                                                                event,
                                                                callback,
                                                                d_allocator_p);
        d_requestQueue.pushBack(req);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
            return -1;
        }
      } break;
      case BTEMT_DISABLED: {
         // Processing thread is disabled -- upgrade to write lock
         // and process request in this thread.

         int rc = d_manager_p->registerSocketEvent(handle, event, callback);
         d_numTotalSocketEvents = d_manager_p->numEvents();
         return rc;
      }
    }

    return 0;
}

void *btemt_TcpTimerEventManager::registerTimer(
        const bdet_TimeInterval&                 timeout,
        const bteso_TimerEventManager::Callback& callback)
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        void *id = (void*)d_timerQueue.add(timeout, callback);
        return id;
    }

    void *result = (void *)0;
    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);
    {
        switch (d_state) {
          case BTEMT_ENABLED: {
            // As performance optimization, we may do the following:
            // Time queue is thread-safe.  Therefore, we don't have to
            // synchronize the following operation.

            int isNewTop = 0;
            int newLength;
            int handle =
                d_timerQueue.add(timeout, callback, &isNewTop, &newLength);

            if (!isNewTop) {
                result = (void*)handle;
                BSLS_ASSERT(result);
            }
            else {
                // Signal dispatcher for the new minimum, if needed.

                btemt_TcpTimerEventManager_Request *req =
                    new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                timeout,
                                                                callback,
                                                                d_allocator_p);
                d_requestQueue.pushBack(req);
                if (0 > d_controlChannel_p->clientWrite()) {
                    d_requestQueue.popBack();
                    d_requestPool.deleteObjectRaw(req);
                    d_timerQueue.remove(handle);
                    result = (void*)0;
                    BSLS_ASSERT("Failed to register timer" && result);
                }
                else {
                    result = (void*)handle;
                    BSLS_ASSERT(result);
                }
            }
          } break;
          case BTEMT_DISABLED: {
            // Processing thread is disabled -- register directly
            // since the timer queue is thread-safe.

            int newTop, newLength = 0;
            result = (void*)d_timerQueue.add(timeout, callback,
                                             &newTop, &newLength);
            BSLS_ASSERT(result);
          } break;
        }
    }
    return result;
}

int btemt_TcpTimerEventManager::rescheduleTimer(
                                             const void               *id,
                                             const bdet_TimeInterval&  timeout)
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        return d_timerQueue.update((int)(bsls::Types::IntPtr)id, timeout);
                                                                      // RETURN
    }

    int rc;
    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);
    {
        switch (d_state) {
          case BTEMT_ENABLED: {
            // As performance optimization, we may do the following:
            // Time queue is thread-safe.  Therefore, we don't have to
            // synchronize the following operation.

            int isNewTop = 0;
            rc           = d_timerQueue.update((int)(bsls::Types::IntPtr)id,
                                               timeout,
                                               &isNewTop);
            if (!rc && isNewTop) {
                // Signal dispatcher for the new minimum, if needed.

                btemt_TcpTimerEventManager_Request *req =
                    new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                id,
                                                                timeout,
                                                                d_allocator_p);

                d_requestQueue.pushBack(req);
                if (0 > d_controlChannel_p->clientWrite()) {
                    d_requestQueue.popBack();
                    d_requestPool.deleteObjectRaw(req);
                    d_timerQueue.remove((int)(bsls::Types::IntPtr)id);
                    BSLS_ASSERT("Failed to reschedule timer" && 0);
                }
            }
          } break;
          case BTEMT_DISABLED: {
            // Processing thread is disabled -- register directly
            // since the timer queue is thread-safe.

            rc = d_timerQueue.update((int)(bsls::Types::IntPtr)id, timeout);
          } break;
        }
    }
    return rc;
}

void btemt_TcpTimerEventManager::deregisterSocketEvent(
        const bteso_SocketHandle::Handle& handle,
        bteso_EventType::Type             event)
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        d_manager_p->deregisterSocketEvent(handle, event);
        d_numTotalSocketEvents = d_manager_p->numEvents()-1;
        return;
    }

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                handle,
                                                                event,
                                                                d_allocator_p);
        d_requestQueue.pushBack(req);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- upgrade to write lock
        // and process request in this thread.

        d_manager_p->deregisterSocketEvent(handle, event);

        // When disabled, the control channel object is destroyed, no need
        // to minus one from 'numEvents()'.

        d_numTotalSocketEvents = d_manager_p->numEvents();
        return;
      }
    }
}

void btemt_TcpTimerEventManager::execute(const bdef_Function<void (*)()>&
                                                                       functor)
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        bcemt_LockGuard<bcemt_Mutex> guard(&d_executeQueueLock);
        this->d_executeQueue_p->push_back(functor);
        return;
    }

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);
    switch (d_state) {
      case BTEMT_ENABLED: {
        bcemt_LockGuard<bcemt_Mutex> guard(&d_executeQueueLock);

        // Processing thread is enabled -- enqueue the request.
        this->d_executeQueue_p->push_back(functor);

        // Signal dispatcher for the new executed events, only if not in
        // dispatcher already *and* if has not been signaled already.
        if (this->d_executeQueue_p->size() == 1 &&
            !bcemt_ThreadUtil::isEqual(
                                     bcemt_ThreadUtil::self(), d_dispatcher)) {

            btemt_TcpTimerEventManager_Request *req =
                new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                functor,
                                                                d_allocator_p);

            d_requestQueue.pushBack(req);
            if (0 > d_controlChannel_p->clientWrite()) {
                d_requestQueue.popBack();
                d_requestPool.deleteObjectRaw(req);
                this->d_executeQueue_p->pop_back();

                // guaranteed to be 'functor'

                BSLS_ASSERT("Failed to execute functor" && 0);
            }
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- Simply enqueue the request.
        // There is no need for 'd_executeQueueLock' here since all other
        // threads are already guarded by the 'd_stateLock'.

        this->d_executeQueue_p->push_back(functor);
      }
    }

}

void btemt_TcpTimerEventManager::clearExecuteQueue()
{
    bcemt_LockGuard<bcemt_Mutex> guard(&d_executeQueueLock);
    d_executeQueue_p->clear();
}

void btemt_TcpTimerEventManager::deregisterSocket(
        const bteso_SocketHandle::Handle& handle)
{
    if (bcemt_ThreadUtil::isEqual(bcemt_ThreadUtil::self(), d_dispatcher)) {
        d_manager_p->deregisterSocket(handle);
        d_numTotalSocketEvents = d_manager_p->numEvents()-1;
        return;
    }

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                handle,
                                                                d_allocator_p);
        d_requestQueue.pushBack(req);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- upgrade to write lock
        // and process request in this thread.

        d_manager_p->deregisterSocket(handle);

        // When disabled, the control channel object is destroyed, no need
        // to minus one from 'numEvents()'.

        d_numTotalSocketEvents = d_manager_p->numEvents();
      }
    }
}

void btemt_TcpTimerEventManager::deregisterAllSocketEvents()
{
    if (bcemt_ThreadUtil::isEqual(bcemt_ThreadUtil::self(), d_dispatcher)) {
        d_manager_p->deregisterAll();
        d_numTotalSocketEvents = 0;
        return;
    }

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        btemt_TcpTimerEventManager_Request *req =
           new (d_requestPool) btemt_TcpTimerEventManager_Request(
             btemt_TcpTimerEventManager_Request::DEREGISTER_ALL_SOCKET_EVENTS,
             d_allocator_p);
        d_requestQueue.pushBack(req);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- upgrade to write lock
        // and process request in this thread.

        d_manager_p->deregisterAll();
        d_numTotalSocketEvents = 0;
      }
    }
}

void btemt_TcpTimerEventManager::deregisterTimer(const void *id)
{
    // We can just remove it.  If its at the top, dispatcher will
    // pick a new top on the next iteration.

    d_timerQueue.remove((int)(bsls::Types::IntPtr)id);
}

void btemt_TcpTimerEventManager::deregisterAllTimers()
{
    d_timerQueue.removeAll();
}

void btemt_TcpTimerEventManager::deregisterAll()
{
    deregisterAllTimers();
    deregisterAllSocketEvents();
}

// ACCESSORS
int btemt_TcpTimerEventManager::isRegistered(
        const bteso_SocketHandle::Handle& handle,
        bteso_EventType::Type             event) const
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        return d_manager_p->isRegistered(handle, event);
    }

    int result;

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        bcemt_Mutex     mutex;
        bcemt_Condition condition;
        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                handle,
                                                                event,
                                                                &condition,
                                                                &mutex,
                                                                d_allocator_p);
        d_requestQueue.pushBack(req);
        bcemt_LockGuard<bcemt_Mutex> lock(&mutex);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
            result = -1;
        }
        else {
            req->waitForResult();
            result = req->result();
            d_requestPool.deleteObjectRaw(req);
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- upgrade to write lock
        // and process request in this thread.

        result = d_manager_p->isRegistered(handle, event);
      }
    }

    return result;
}

int btemt_TcpTimerEventManager::numEvents() const
{
    return numTimers() + numTotalSocketEvents();
}

int btemt_TcpTimerEventManager::numTimers() const
{
    return d_timerQueue.length();
}

int btemt_TcpTimerEventManager::numSocketEvents(
        const bteso_SocketHandle::Handle& handle) const
{
    if (bcemt_ThreadUtil::isEqual(
                    bcemt_ThreadUtil::self(), d_dispatcher)) {
        return d_manager_p->numSocketEvents(handle);
    }
    int result;

    bcemt_ReadLockGuard<bcemt_RWMutex> guard(&d_stateLock);

    if (BTEMT_DISABLED == d_state) {
        d_stateLock.unlock();
        d_stateLock.lockWrite();
    }

    switch (d_state) {
      case BTEMT_ENABLED: {
        // Processing thread is enabled -- enqueue the request.

        bcemt_Mutex mutex;
        bcemt_Condition condition;
        btemt_TcpTimerEventManager_Request *req =
            new (d_requestPool) btemt_TcpTimerEventManager_Request(
                                                                handle,
                                                                &condition,
                                                                &mutex,
                                                                d_allocator_p);
        d_requestQueue.pushBack(req);
        bcemt_LockGuard<bcemt_Mutex> lock(&mutex);
        if (0 > d_controlChannel_p->clientWrite()) {
            d_requestQueue.popBack();
            d_requestPool.deleteObjectRaw(req);
            result = -1;
        }
        else {
            req->waitForResult();
            result = req->result();
            d_requestPool.deleteObjectRaw(req);
        }
      } break;
      case BTEMT_DISABLED: {
        // Processing thread is disabled -- upgrade to write lock
        // and process request in this thread.

        result = d_manager_p->numSocketEvents(handle);
      }
    }

    return result;
}

int btemt_TcpTimerEventManager::numTotalSocketEvents() const
{
    return d_numTotalSocketEvents;
}

int btemt_TcpTimerEventManager::isEnabled() const
{
    return d_state == BTEMT_ENABLED; // d_state is volatile

/*
    bcemt_LockGuard<bcemt_Mutex> lock(&d_cs);
    return d_dispatcher != bcemt_ThreadUtil::invalidHandle();
*/

}

}  // close namespace BloombergLP

// ---------------------------------------------------------------------------
// NOTICE:
//      Copyright (C) Bloomberg L.P., 2007
//      All Rights Reserved.
//      Property of Bloomberg L.P. (BLP)
//      This software is made available solely pursuant to the
//      terms of a BLP license agreement which governs its use.
// ----------------------------- END-OF-FILE ---------------------------------
