#pragma once

#include "s3-udc2.h"

S3Info gPatchable = {
    .host = L"<PATCH-ME-HOST>",
    .uploadKey = "<PATCH-ME-UPLOAD-KEY>",
    .downloadKey = "<PATCH-ME-DOWNLOAD-KEY>",
    .xAmzCredential = "<PATCH-ME-X-AMZ-CREDENTIAL>",
    .xAmzDate = "<PATCH-ME-X-AMZ-DATE>",
    .xAmzSignature = "<PATCH-ME-X-AMZ-SIGNATURE>",
    .xAmzPolicy = "<PATCH-ME-X-AMZ-POLICY>"
};
wchar_t gUserAgent[128] = L"<PATCH-ME-USER-AGENT>";

#define C2_STATE_KEY         ".s3-udc2.state"
#define MAX_FRAME_RETRIES    10
#define FRAME_RETRY_DELAY_MS 2000

#define USER_AGENT           gUserAgent
#define DEFAULT_S3_INFO      gPatchable
