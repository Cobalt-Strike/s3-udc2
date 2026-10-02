#pragma once

// The key used to support configration hot-swap
#define C2_STATE_KEY         ".s3-udc2.state"

// The used user-agent for HTTP requests
#define USER_AGENT           L"s3-udc2"

// The re-try configuration for HTTP requests
#define MAX_FRAME_RETRIES    10
#define FRAME_RETRY_DELAY_MS 2000

// The hostname of the S3 bucket
#define S3_HOST          L"<my-bucket-name>.s3.amazonaws.com"

// The key prefix for Beacon -> TS frames. The frames are uploaded into <prefix>/<session id>
#define S3_UPLOAD_KEY    "c2s"

// The key prefix for TS -> Beacon frames. The frames are downloaded from https://<s3 host>/<prefix>/<session id>
#define S3_DOWNLOAD_KEY  "s2c"

// The authentication info for the presigned POST upload url
#define X_AMZ_CREDENTIAL "<key-id>/<date>/<region>/s3/aws4_request"
#define X_AMZ_DATE       "<timestamp>"
#define X_AMZ_SIGNATURE  "<signature>"
#define X_AMZ_POLICY     "<base64 encoded policy>"
