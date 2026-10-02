#include <iostream>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <utility>
#include <cstring>
#include <map>
#include <tuple>
#include <cassert>
#include <windows.h>

extern "C" {
#ifdef _DEBUG
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#endif
#include "../beacon.h"
}

#include "mock.h"

#ifndef min
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#endif

namespace bof {
    namespace utils {
        template <typename T>
        T swapEndianness(T value) {
            char *ptr = reinterpret_cast<char *>(&value);
            std::reverse(ptr, ptr + sizeof(T));
            return value;
        }

        template <typename T>
        std::vector<char> toBytes(T input) {
            char *ptr = reinterpret_cast<char *>(&input);
            return std::vector<char>(ptr, ptr + sizeof(T));
        }

        const char* typeToStr(int callbackType) {
            switch (callbackType) {
                case CALLBACK_OUTPUT: return "CALLBACK_OUTPUT";
                case CALLBACK_OUTPUT_OEM: return "CALLBACK_OUTPUT_OEM";
                case CALLBACK_ERROR: return "CALLBACK_ERROR";
                case CALLBACK_OUTPUT_UTF8: return "CALLBACK_OUTPUT_UTF8";
                default: return "CALLBACK_UNKOWN";
            }
        }
    }

    namespace mock {
        static BEACON_INFO beaconInfo = { 0 };

        char *BofData::get() {
            return size() > 0 ? reinterpret_cast<char *>(&data[0]) : nullptr;
        }

        int BofData::size() {
            return data.size();
        }

        void BofData::addData(const char *buf, std::size_t len) {
            std::vector<char> bytes;
            bytes.assign(buf, buf + len);
            insert(static_cast<int>(len));
            append(bytes);
        }

        void BofData::append(const std::vector<char> &data) {
            this->data.insert(std::end(this->data), std::begin(data), std::end(data));
        }

        void BofData::insert(int v) {
            append(bof::utils::toBytes(bof::utils::swapEndianness(v)));
        }

        void BofData::insert(short v) {
            append(bof::utils::toBytes(bof::utils::swapEndianness(v)));
        }

        void BofData::insert(unsigned int v) {
            insert(static_cast<int>(v));
        }

        void BofData::insert(unsigned short v) {
            insert(static_cast<short>(v));
        }

        void BofData::insert(const char *v) {
            addData(v, std::strlen(v) + 1);
        }

        void BofData::insert(const wchar_t *v) {
            addData((const char *)v, (std::wcslen(v) + 1) * sizeof(wchar_t));
        }

        void BofData::insert(const std::vector<char>& data) {
            pack<int32_t>(data.size());
            append(data);
        }

        void setBeaconInfo(BEACON_INFO& info) {
            std::memcpy(&bof::mock::beaconInfo, &info, sizeof(BEACON_INFO));
        }
    }

    namespace output {
        std::vector<OutputEntry> outputs;

        void addEntry(int type, const char* data, int len) {
            OutputEntry output = {
                type,
                std::string(data, data + len)
            };
            outputs.push_back(output);
        }

        const std::vector<OutputEntry>& getOutputs() {
            return outputs;
        }

        void reset() {
            outputs.clear();
        }

        void PrintTo(const OutputEntry& o, std::ostream* os) {
            *os << "{ callbackType: " << bof::utils::typeToStr(o.callbackType) << ", output: " << o.output << " }";
        }
    }

    namespace valuestore {
        std::map<std::string, void*> values;

        void reset() {
            values.clear();
        }
    }

    namespace bud {
        char custom[BEACON_USER_DATA_CUSTOM_SIZE] = { 0 };

        void reset() {
            std::memset(custom, 0, BEACON_USER_DATA_CUSTOM_SIZE);
        }

        void set(const char* data) {
            if (data) {
                std::memcpy(custom, data, BEACON_USER_DATA_CUSTOM_SIZE);
            }
        }
    }
}

extern "C"
{
    // Print API
    void BeaconPrintf(int type, const char *fmt, ...) {
        printf("[Output Callback: %s (0x%X)]\n", bof::utils::typeToStr(type), type);
        va_list args;
        va_start(args, fmt);
        int size = vsnprintf(nullptr, 0, fmt, args);
        if (size >= 0) {
            char* buffer = new char[size + 1];
            vsnprintf(buffer, size + 1, fmt, args);
            bof::output::addEntry(type, buffer, size);
            delete[] buffer;
        }
        vprintf(fmt, args);
        printf("\n");
        va_end(args);
    }

    void BeaconOutput(int type, const char *data, int len) {
        bof::output::addEntry(type, data, len);
        printf("[Output Callback: %s (0x%X)]\n%.*s", bof::utils::typeToStr(type), type, len, data);
    }

    // Parser API
    void BeaconDataParse(datap *parser, char *buffer, int size) {
        parser->buffer = buffer;
        parser->original = buffer;
        parser->size = size;
        parser->length = size;
    }

    char* BeaconDataPtr(datap* parser, int size) {
        if (parser->length < size) {
            return NULL;
        }

        char* data = parser->buffer;
        parser->buffer += size;
        parser->length -= size;

        return data;
    }

    int BeaconDataInt(datap *parser) {
        int value = *(int *)(parser->buffer);
        parser->buffer += sizeof(int);
        parser->length -= sizeof(int);
        return bof::utils::swapEndianness(value);
    }

    short BeaconDataShort(datap *parser) {
        short value = *(short *)(parser->buffer);
        parser->buffer += sizeof(short);
        parser->length -= sizeof(short);
        return bof::utils::swapEndianness(value);
    }

    int BeaconDataLength(datap *parser) {
        return parser->length;
    }

    char *BeaconDataExtract(datap *parser, int *size) {
        int size_im = BeaconDataInt(parser);
        char *buff = parser->buffer;
        parser->buffer += size_im;
        if (size)
        {
            *size = size_im;
        }
        return buff;
    }

    // Format API
    void BeaconFormatAlloc(formatp *format, int maxsz) {
        format->original = new char[maxsz];
        format->buffer = format->original;
        format->length = 0;
        format->size = maxsz;
    }

    void BeaconFormatReset(formatp *format) {
        format->buffer = format->original;
        format->length = 0;
        memset(format->original, 0, format->size);
    }

    void BeaconFormatFree(formatp *format) {
        memset(format->original, 0, format->size);
        delete[] format->original;
        format->original = nullptr;
        format->buffer = nullptr;
        format->size = 0;
        format->length = 0;
    }

    void BeaconFormatAppend(formatp *format, const char *text, int len) {
        memcpy(format->buffer, text, len);
        format->buffer += len;
        format->length += len;
    }

    void BeaconFormatPrintf(formatp *format, const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int len = vsprintf_s(format->buffer, format->size - format->length, fmt, args);
        format->buffer += len;
        format->length += len;
        va_end(args);
    }

    char *BeaconFormatToString(formatp *format, int *size) {
        if (size)
        {
            *size = format->length;
        }
        return format->original;
    }

    void BeaconFormatInt(formatp *format, int value) {
        value = bof::utils::swapEndianness(value);
        BeaconFormatAppend(format, (char *)&value, 4);
    }

    // Internal API
    BOOL BeaconUseToken(HANDLE token) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return TRUE;
    }

    void BeaconRevertToken() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    BOOL BeaconIsAdmin() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return FALSE;
    }

    void BeaconGetSpawnTo(BOOL x86, char *buffer, int length) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    void BeaconInjectProcess(HANDLE hProc, int pid, char *payload,
                             int p_len, int p_offset, char *arg,
                             int a_len)
    {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    void BeaconInjectTemporaryProcess(PROCESS_INFORMATION *pInfo,
                                      char *payload, int p_len,
                                      int p_offset, char *arg,
                                      int a_len)
    {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }


    BOOL BeaconSpawnTemporaryProcess(BOOL x86, BOOL ignoreToken, STARTUPINFO* si, PROCESS_INFORMATION* pInfo) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return false;
    }

    void BeaconCleanupProcess(PROCESS_INFORMATION *pInfo) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    BOOL toWideChar(char *src, wchar_t *dst, int max) {
        std::string str = src;
        std::wstring wstr(str.begin(), str.end());

        size_t bytes = min(wstr.length() * sizeof(wchar_t), static_cast<size_t>(max));
        std::memcpy(dst, wstr.c_str(), bytes);
        return TRUE;
    }

    BOOL BeaconInformation(PBEACON_INFO info) {
        std::memcpy(info, &bof::mock::beaconInfo, sizeof(BEACON_INFO));
        return TRUE;
    }

    BOOL BeaconAddValue(const char* key, void* ptr) {
        auto item = bof::valuestore::values.find(std::string(key));
        if (ptr && item == bof::valuestore::values.end()) {
            bof::valuestore::values[std::string(key)] = ptr;
            return TRUE;
        }
        return FALSE;
    }

    void* BeaconGetValue(const char* key) {
        auto item = bof::valuestore::values.find(std::string(key));
        if (item != bof::valuestore::values.end()) {
            return item->second;
        }
        return NULL;
    }

    BOOL BeaconRemoveValue(const char* key) {
        auto item = bof::valuestore::values.find(std::string(key));
        if (item != bof::valuestore::values.end()) {
            bof::valuestore::values.erase(item);
            return TRUE;
        }
        return FALSE;
    }

    PDATA_STORE_OBJECT BeaconDataStoreGetItem(size_t index) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return NULL;
    }

    void BeaconDataStoreProtectItem(size_t index) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    void BeaconDataStoreUnprotectItem(size_t index) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    size_t BeaconDataStoreMaxEntries() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return 0;
    }

    char* BeaconGetCustomUserData() {
        return bof::bud::custom;
    }

    BOOL BeaconGetSyscallInformation(PBEACON_SYSCALLS info, SIZE_T infoSize, BOOL resolveIfNotInitialized) {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
        return FALSE;
    }

    // Beacon System Call API
    LPVOID BeaconVirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
        return VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    }

    LPVOID BeaconVirtualAllocEx(HANDLE processHandle, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
        return VirtualAllocEx(processHandle, lpAddress, dwSize, flAllocationType, flProtect);
    }

    BOOL BeaconVirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
        return VirtualProtect(lpAddress, dwSize, flNewProtect, lpflOldProtect);
    }

    BOOL BeaconVirtualProtectEx(HANDLE processHandle, LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
        return VirtualProtectEx(processHandle, lpAddress, dwSize, flNewProtect, lpflOldProtect);
    }

    BOOL BeaconVirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
        return VirtualFree(lpAddress, dwSize, dwFreeType);
    }

    BOOL BeaconGetThreadContext(HANDLE threadHandle, PCONTEXT threadContext) {
        return GetThreadContext(threadHandle, threadContext);
    }

    BOOL BeaconSetThreadContext(HANDLE threadHandle, PCONTEXT threadContext) {
        return SetThreadContext(threadHandle, threadContext);
    }

    DWORD BeaconResumeThread(HANDLE threadHandle) {
        return ResumeThread(threadHandle);
    }

    HANDLE BeaconOpenProcess(DWORD desiredAccess, BOOL inheritHandle, DWORD processId) {
        return OpenProcess(desiredAccess, inheritHandle, processId);
    }

    HANDLE BeaconOpenThread(DWORD desiredAccess, BOOL inheritHandle, DWORD threadId) {
        return OpenThread(desiredAccess, inheritHandle, threadId);
    }

    BOOL BeaconCloseHandle(HANDLE object) {
        return CloseHandle(object);
    }

    BOOL BeaconUnmapViewOfFile(LPCVOID baseAddress) {
        return UnmapViewOfFile(baseAddress);
    }

    SIZE_T BeaconVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length) {
        return VirtualQuery(address, buffer, length);
    }

    BOOL BeaconDuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
        return DuplicateHandle(hSourceProcessHandle, hSourceHandle, hTargetProcessHandle, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
    }

    BOOL BeaconReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead) {
        return ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
    }

    BOOL BeaconWriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten) {
        return WriteProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesWritten);
    }

    VOID BeaconDisableBeaconGate() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    VOID BeaconEnableBeaconGate() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    VOID BeaconDisableBeaconGateMasking() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }

    VOID BeaconEnableBeaconGateMasking() {
        std::cerr << "Not implemented: " << __FUNCTION__ << std::endl;
    }
}
