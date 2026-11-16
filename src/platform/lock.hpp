#ifndef JITVM_PLATFORM_LOCK_HPP
#define JITVM_PLATFORM_LOCK_HPP

namespace jitvm {

class Lock {
public:
	Lock();
	~Lock();
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	void lock();
	void unlock();

private:
	struct Impl;
	Impl* impl_;
};

class LockGuard {
public:
	explicit LockGuard(Lock& lock) : lock_(lock) { lock_.lock(); }
	~LockGuard() { lock_.unlock(); }
	LockGuard(const LockGuard&) = delete;
	LockGuard& operator=(const LockGuard&) = delete;

private:
	Lock& lock_;
};

Lock& code_lock();

}  // namespace jitvm

#endif
