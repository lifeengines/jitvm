#include "platform/lock.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace jitvm {

struct Lock::Impl {
#if defined(_WIN32)
	CRITICAL_SECTION cs;
#else
	pthread_mutex_t mu;
#endif
};

Lock::Lock() : impl_(new Impl) {
#if defined(_WIN32)
	InitializeCriticalSection(&impl_->cs);
#else
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&impl_->mu, &attr);
	pthread_mutexattr_destroy(&attr);
#endif
}

Lock::~Lock() {
#if defined(_WIN32)
	DeleteCriticalSection(&impl_->cs);
#else
	pthread_mutex_destroy(&impl_->mu);
#endif
	delete impl_;
}

void Lock::lock() {
#if defined(_WIN32)
	EnterCriticalSection(&impl_->cs);
#else
	pthread_mutex_lock(&impl_->mu);
#endif
}

void Lock::unlock() {
#if defined(_WIN32)
	LeaveCriticalSection(&impl_->cs);
#else
	pthread_mutex_unlock(&impl_->mu);
#endif
}

Lock& code_lock() {
	static Lock lock;
	return lock;
}

}  // namespace jitvm
