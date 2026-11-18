#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cerrno>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#if defined(__SWITCH2__)
#include "platform/switch/nx2_platform.h"
#endif

extern "C"
{
	long sysconf(int name)
	{
		if (name == _SC_PAGESIZE)
			return 0x1000;
#ifdef _SC_NPROCESSORS_ONLN
		if (name == _SC_NPROCESSORS_ONLN)
#if defined(__SWITCH2__)
			return nx2CoreCount();
#else
			return 4;
#endif
#endif
#ifdef _SC_LEVEL1_ICACHE_SIZE
		if (name == _SC_LEVEL1_ICACHE_SIZE)
			return 48 * 1024;
#endif
#ifdef _SC_LEVEL1_DCACHE_SIZE
		if (name == _SC_LEVEL1_DCACHE_SIZE)
			return 32 * 1024;
#endif
#ifdef _SC_LEVEL2_CACHE_SIZE
		if (name == _SC_LEVEL2_CACHE_SIZE)
			return 2 * 1024 * 1024;
#endif
#ifdef _SC_LEVEL3_CACHE_SIZE
		if (name == _SC_LEVEL3_CACHE_SIZE)
			return 0;
#endif
		errno = EINVAL;
		return -1;
	}
	int pipe(int fildes[2])
	{
		if (!fildes)
		{
			errno = EFAULT;
			return -1;
		}

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fildes) == 0)
			return 0;

		int listener = socket(AF_INET, SOCK_STREAM, 0);
		if (listener < 0)
			return -1;

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = 0;

		int writer = -1;
		int reader = -1;
		if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
			listen(listener, 1) == 0)
		{
			socklen_t length = sizeof(address);
			if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0)
			{
				writer = socket(AF_INET, SOCK_STREAM, 0);
				if (writer >= 0 && connect(writer, reinterpret_cast<sockaddr*>(&address), length) == 0)
					reader = accept(listener, nullptr, nullptr);
			}
		}

		const int saved_errno = errno;
		close(listener);
		if (reader < 0 || writer < 0)
		{
			if (reader >= 0)
				close(reader);
			if (writer >= 0)
				close(writer);
			errno = saved_errno;
			return -1;
		}

		fildes[0] = reader;
		fildes[1] = writer;
		return 0;
	}
	int pause(void)
	{
		errno = ENOSYS;
		return -1;
	}
	int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset)
	{
		(void)how;
		(void)set;
		if (oldset)
			sigemptyset(oldset);
		return 0;
	}
#if !defined(__SWITCH2__) // libounce's allocator (mimalloc) provides posix_memalign
	int posix_memalign(void** memptr, size_t alignment, size_t size)
	{
		if (!memptr || alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0)
			return EINVAL;
		void* p = memalign(alignment, size);
		if (!p && size != 0)
			return ENOMEM;
		*memptr = p;
		return 0;
	}
#endif

	unsigned int if_nametoindex(const char* /*ifname*/)
	{
		return 0;
	}
	char* if_indextoname(unsigned int /*ifindex*/, char* ifname)
	{
		if (ifname)
			ifname[0] = '\0';
		return nullptr;
	}

	time_t timegm(struct tm* t)
	{
		if (!t)
		{
			errno = EINVAL;
			return (time_t)-1;
		}

		int64_t year = (int64_t)t->tm_year + 1900;
		int64_t month = t->tm_mon;
		if (month < 0)
		{
			const int64_t years = (-month + 11) / 12;
			year -= years;
			month += years * 12;
		}
		year += month / 12;
		month %= 12;
		const unsigned civilMonth = (unsigned)month + 1;
		year -= civilMonth <= 2;
		const int64_t era = (year >= 0 ? year : year - 399) / 400;
		const unsigned yearOfEra = (unsigned)(year - era * 400);
		const unsigned adjustedMonth = civilMonth > 2 ? civilMonth - 3 : civilMonth + 9;
		const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5;
		const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
		const int64_t days = era * 146097 + dayOfEra - 719468 + (int64_t)t->tm_mday - 1;
		return (time_t)(days * 86400 + (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec);
	}

} // extern "C"
