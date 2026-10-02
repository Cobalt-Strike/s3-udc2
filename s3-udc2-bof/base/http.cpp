/* http.cpp - WinHTTP helper functions
*
* This file implements a small, thin wrapper around the WinHTTP API to
* perform simple HTTP(S) operations. It provides:
*  - HttpRequest and HttpResponse structures to hold request/response state.
*  - httpRequest: open connection, create request, send headers/payload.
*  - httpWrite: stream additional request body data after httpRequest.
*  - httpHeader: read a specific response header value.
*  - httpReadBody / httpReadN read response body data.
*  - httpClose: close request/connect handles and clean up.
*
* Design notes and constraints:
*  - Uses WinHTTP handles (HINTERNET) and expects the caller to create a
*    session handle (via WinHttpOpen) and provide it in HttpRequest.hSession.
*  - Minimal error handling: functions typically return bool success/failure
*    or an HttpResponse with handles set to NULL on failure. On failure,
*    the caller is responsible for cleanup where appropriate.
*  - Not thread-safe: WinHTTP session and handles should be managed by the
*    caller if used across threads.
*  - The read helpers assume the caller provides sufficiently sized buffers
*    and will fail if the server advertises more data than the provided buffer.
*
* Usage example (high-level):
*  - Fill HttpRequest with session/host/port/method/path/headers/payload info.
*  - Call httpRequest(&req) to initiate the request.
*  - Optionally call httpWrite to stream remaining request body if HttpRequest
*    was called with a smaller payloadLen / totalLen.
*  - Call the read helpers to obtain the response body.
*  - Call httpHeader to obtain specific response header values.
*  - Finally call httpClose to free the WinHTTP handles.
*
*/
#include <windows.h>
#include <winhttp.h>

#include "helpers.h"

/* HttpRequest - describes an HTTP request to be performed.
 *
 * Fields:
 *  - hSession: existing WinHTTP session handle (created with WinHttpOpen).
 *  - useHttps: set to true to use HTTPS (WINHTTP_FLAG_SECURE) for the request.
 *  - method: HTTP method string (e.g. L"GET", L"POST").
 *  - host: server host name as a wide string (e.g. L"example.com").
 *  - port: TCP port (80 for HTTP, 443 for HTTPS).
 *  - path: request path (e.g. L"/api/resource").
 *  - headers: additional request headers as a wide string (or NULL).
 *  - payload: pointer to initial request body bytes (may be NULL).
 *  - payloadLen: number of bytes provided in payload.
 *  - totalLen: total number of bytes expected to be sent for the request body.
 *              If no additional data will be written, then totalLen must be
 *              equal to payloadLen
 */
typedef struct {
    HINTERNET hSession;
    bool useHttps;
    const wchar_t* method;
    const wchar_t* host;
    unsigned int port;
    const wchar_t* path;
    const wchar_t* headers;
    const char* payload;
    unsigned int payloadLen;
    unsigned int totalLen;
} HttpRequest;

/* HttpResponse - holds response state and WinHTTP handles for an
 * in-progress or completed request.
 *
 * Fields:
 *  - status: HTTP status code from the response (e.g. 200, 404).
 *  - hConnect: WinHTTP connection handle returned by WinHttpConnect.
 *  - hRequest: WinHTTP request handle returned by WinHttpOpenRequest.
 *  - toBeWritten: number of request body bytes remaining to be written
 *    (used when sending a request in multiple chunks).
 */
typedef struct {
    DWORD status;
    HINTERNET hConnect;
    HINTERNET hRequest;
    unsigned int toBeWritten;
} HttpResponse;

extern "C" {

bool httpReceive(HttpResponse* r) {
	DFR_LOCAL(WinHttp, WinHttpReceiveResponse);
	DFR_LOCAL(WinHttp, WinHttpQueryHeaders);
	DFR_LOCAL(WinHttp, WinHttpCloseHandle);
	if (!WinHttpReceiveResponse(r->hRequest, NULL)) {
		WinHttpCloseHandle(r->hRequest);
		WinHttpCloseHandle(r->hConnect);
		r->hRequest = 0;
		r->hConnect = 0;
		return false;
	}

	DWORD statusLen = sizeof(r->status);
	if (!WinHttpQueryHeaders(
		r->hRequest,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX,
		&r->status,
		&statusLen,
		WINHTTP_NO_HEADER_INDEX
	)) {
		WinHttpCloseHandle(r->hRequest);
		WinHttpCloseHandle(r->hConnect);
		r->hRequest = 0;
		r->hConnect = 0;
		return false;
	}

	return true;
}

HttpResponse httpRequest(HttpRequest* req) {
	DFR_LOCAL(WinHttp, WinHttpConnect);
	DFR_LOCAL(WinHttp, WinHttpOpenRequest);
	DFR_LOCAL(WinHttp, WinHttpSendRequest);
	DFR_LOCAL(WinHttp, WinHttpCloseHandle);

	HttpResponse r = { };

	r.hConnect = WinHttpConnect(
		req->hSession,
		req->host,
		req->port,
		0
	);
	if (!r.hConnect) {
		return r;
	}

	r.hRequest = WinHttpOpenRequest(
		r.hConnect,
		req->method,
		req->path,
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		req->useHttps ? WINHTTP_FLAG_SECURE : 0
	);
	if (!r.hRequest) {
		WinHttpCloseHandle(r.hConnect);
		r.hConnect = 0;
		return r;
	}

	if (!WinHttpSendRequest(
		r.hRequest,
		req->headers,
		-1L,
		(LPVOID)req->payload,
		req->payloadLen,
		req->totalLen,
		0
	)) {
		WinHttpCloseHandle(r.hRequest);
		WinHttpCloseHandle(r.hConnect);
		r.hRequest = 0;
		r.hConnect = 0;
		return r;
	}
	r.toBeWritten = req->totalLen - req->payloadLen;

	if (r.toBeWritten == 0) {
		httpReceive(&r);
	}

	return r;
}

bool httpWrite(HttpResponse* r, const char* buffer, unsigned int len) {
	DFR_LOCAL(WinHttp, WinHttpWriteData);
	if (len == 0) return true;
	if (!r || !r->hRequest || !buffer) return false;

	DWORD totalWritten = 0;
	while (totalWritten < len) {
		DWORD toWrite = len - totalWritten;
		DWORD bytesWritten = 0;

		BOOL ok = WinHttpWriteData(
			r->hRequest,
			buffer + totalWritten,
			toWrite,
			&bytesWritten
		);

		if (!ok || bytesWritten == 0) {
			return false;
		}

		totalWritten += bytesWritten;
	}

	r->toBeWritten -= totalWritten;

	if (r->toBeWritten == 0) {
		return httpReceive(r);
	}

	return true;
}

void httpClose(HttpResponse* r) {
	DFR_LOCAL(WinHttp, WinHttpCloseHandle);
	if (r->hRequest) {
		WinHttpCloseHandle(r->hRequest);
		r->hRequest = 0;
	}
	if (r->hConnect) {
		WinHttpCloseHandle(r->hConnect);
		r->hConnect = 0;
	}
}

bool httpReadN(HttpResponse* r, char* buffer, unsigned long len, unsigned int* bytesRead) {
	DFR_LOCAL(WinHttp, WinHttpReadData);

	char* cursor = buffer;
	size_t remaining = len;
	unsigned int totalRead = 0;

	while (remaining > 0) {
		DWORD toRead = remaining;
		DWORD dwRead = 0;

		if (!WinHttpReadData(r->hRequest, cursor, toRead, &dwRead)) {
			return false;
		}

		if (dwRead == 0) {
			break;
		}

		cursor += dwRead;
		remaining -= dwRead;
		totalRead += dwRead;
	}

	if (bytesRead) {
		*bytesRead = totalRead;
	}

	return true;
}

bool httpReadBody(HttpResponse* r, char* buffer, size_t len, unsigned int* bytesWritten) {
	DFR_LOCAL(WinHttp, WinHttpQueryDataAvailable);

	if (bytesWritten) *bytesWritten = 0;

	unsigned int totalWritten = 0;
	size_t remaining = len;
	char* cursor = buffer;

	while (remaining > 0) {
		DWORD dwAvailable = 0;
		if (!WinHttpQueryDataAvailable(r->hRequest, &dwAvailable)) {
			return false;
		}

		if (dwAvailable == 0) {
			break;
		}

		if ((size_t)dwAvailable > remaining) {
			return false;
		}

		unsigned int chunkRead = 0;
		if (!httpReadN(r, cursor, dwAvailable, &chunkRead)) {
			return false;
		}

		cursor += chunkRead;
		remaining -= chunkRead;
		totalWritten += chunkRead;

		// If we got less than advertised, the server is done.
		if (chunkRead < dwAvailable) {
			break;
		}
	}

	if (bytesWritten) {
		*bytesWritten = totalWritten;
	}

	return true;
}

bool httpHeader(HttpResponse* r, const wchar_t* header, wchar_t* buffer, size_t len) {
	DFR_LOCAL(WinHttp, WinHttpQueryHeaders);

	DWORD size = (DWORD)len;
	return WinHttpQueryHeaders(
		r->hRequest,
		WINHTTP_QUERY_CUSTOM,
		header,
		buffer,
		&size,
		NULL
	);
}

}
