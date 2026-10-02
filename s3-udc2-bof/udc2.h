#pragma once
#include <windows.h>

typedef int(*UDC2ProxyCall)(const char* sendBuf, int sendBufLen, char* recvBuf, int recvBufMaxLen);
typedef void(*UDC2ProxyClose)();

typedef struct _UDC2_INFO {
	DWORD version;
	UDC2ProxyCall proxyCall;
	UDC2ProxyClose proxyClose;
} UDC2_INFO, * PUDC2_INFO;