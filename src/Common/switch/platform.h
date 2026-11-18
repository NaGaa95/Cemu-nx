#pragma once

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <shared_mutex>
#include <mutex>

#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <net/if.h>

#ifndef ESHUTDOWN
#define ESHUTDOWN 110
#endif
#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x00000002
#endif

extern "C" {
struct ipv6_mreq
{
	struct in6_addr ipv6mr_multiaddr;
	unsigned int ipv6mr_interface;
};
unsigned int if_nametoindex(const char* ifname);
char* if_indextoname(unsigned int ifindex, char* ifname);
time_t timegm(struct tm* tm);
}

#ifndef bswap_16
#define bswap_16(x) __builtin_bswap16(x)
#endif
#ifndef bswap_32
#define bswap_32(x) __builtin_bswap32(x)
#endif
#ifndef bswap_64
#define bswap_64(x) __builtin_bswap64(x)
#endif

class SlimRWLock
{
public:
	void LockRead() { m_sm.lock_shared(); }
	void UnlockRead() { m_sm.unlock_shared(); }
	void LockWrite() { m_sm.lock(); }
	bool TryLockWrite() { return m_sm.try_lock(); }
	void UnlockWrite() { m_sm.unlock(); }

private:
	std::shared_mutex m_sm;
};

inline uint32_t GetExceptionError()
{
	return errno;
}

uint32_t GetTickCount();

template<size_t N>
void strcpy_s(char (&dst)[N], const char* src)
{
	if (!src)
	{
		dst[0] = '\0';
		return;
	}
	size_t i = 0;
	while (i + 1 < N && src[i] != '\0')
	{
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

template<size_t N>
void strcat_s(char (&dst)[N], const char* src)
{
	if (!src)
		return;
	size_t dstLength = 0;
	while (dstLength + 1 < N && dst[dstLength] != '\0')
		dstLength++;
	size_t srcIndex = 0;
	while (dstLength + 1 < N && src[srcIndex] != '\0')
	{
		dst[dstLength++] = src[srcIndex++];
	}
	dst[dstLength] = '\0';
}
