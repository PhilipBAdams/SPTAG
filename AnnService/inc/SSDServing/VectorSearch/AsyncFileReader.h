// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "inc/Helper/DiskIO.h"
#include "inc/Core/Common.h"

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <stdint.h>
#include <tchar.h>

#include <boost/lockfree/stack.hpp>

#include <Windows.h>

namespace SPTAG {
    namespace SSDServing {
        namespace VectorSearch {

            namespace DiskUtils
            {

                struct PrioritizedDiskFileReaderResource;

                struct CallbackOverLapped : public OVERLAPPED
                {
                    PrioritizedDiskFileReaderResource* const c_registeredResource;

                    const std::function<void(bool)>* m_callback;


                    CallbackOverLapped(PrioritizedDiskFileReaderResource* p_registeredResource)
                        : c_registeredResource(p_registeredResource),
                        m_callback(nullptr)
                    {
                    }
                };


                struct PrioritizedDiskFileReaderResource
                {
                    CallbackOverLapped m_col;

                    PrioritizedDiskFileReaderResource()
                        : m_col(this)
                    {
                    }
                };
            }
            
            class HandleWrapper
            {
            public:
                HandleWrapper(HANDLE p_handle) : m_handle(p_handle) {}
                HandleWrapper(HandleWrapper&& p_right) : m_handle(std::move(p_right.m_handle)) {}
                HandleWrapper() : m_handle(INVALID_HANDLE_VALUE) {}
                ~HandleWrapper() {}

                void Reset(HANDLE p_handle) { m_handle.reset(p_handle); }
                HANDLE GetHandle() { return m_handle.get(); }
                bool IsValid() const { return m_handle.get() != INVALID_HANDLE_VALUE; };
                void Close() { m_handle.reset(INVALID_HANDLE_VALUE); }

                HandleWrapper(const HandleWrapper&) = delete;
                HandleWrapper& operator=(const HandleWrapper&) = delete;

                struct HandleDeleter
                {
                    void operator()(HANDLE p_handle) const
                    {
                        if (p_handle != INVALID_HANDLE_VALUE)
                        {
                            ::CloseHandle(p_handle);
                        }

                        p_handle = INVALID_HANDLE_VALUE;
                    }
                };

            private:
                typedef std::unique_ptr<typename std::remove_pointer<HANDLE>::type, HandleDeleter> UniqueHandle;
                UniqueHandle m_handle;
            };

            struct DiskListRequest;

            class RequestQueue
            {
            public:
                RequestQueue() {
                    m_handle.Reset(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0));
                }

                void Push(DiskListRequest* j) {
                    ::PostQueuedCompletionStatus(m_handle.GetHandle(),
                        0,
                        NULL,
                        reinterpret_cast<LPOVERLAPPED>(j));
                }

                bool Pop(DiskListRequest*& j) {
                    DWORD cBytes;
                    ULONG_PTR key;
                    OVERLAPPED* ol;
                    BOOL ret = ::GetQueuedCompletionStatus(m_handle.GetHandle(),
                        &cBytes,
                        &key,
                        &ol,
                        INFINITE);
                    if (FALSE == ret || nullptr == ol) return false;
                    j = reinterpret_cast<DiskListRequest*>(ol);
                    return true;
                }

            private:
                HandleWrapper m_handle;
            };

            class AsyncFileIO : public SPTAG::Helper::DiskPriorityIO
            {
            public:
                AsyncFileIO(SPTAG::Helper::DiskIOScenario scenario = SPTAG::Helper::DiskIOScenario::DIS_UserRead) {}

                virtual ~AsyncFileIO() { ShutDown(); }

                virtual bool Initialize(const char* filePath, int openMode,
                    std::uint64_t maxIOSize = (1 << 20),
                    std::uint32_t maxReadRetries = 2,
                    std::uint32_t maxWriteRetries = 2,
                    std::uint16_t threadPoolSize = 4)
                {
                    m_fileHandle.Reset(::CreateFileA(filePath,
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
                        NULL));

                    if (!m_fileHandle.IsValid()) return false;

                    int iocpThreads = threadPoolSize;

                    m_fileIocp.Reset(::CreateIoCompletionPort(m_fileHandle.GetHandle(), NULL, NULL, iocpThreads));
                    for (int i = 0; i < iocpThreads; ++i)
                    {
                        m_fileIocpThreads.emplace_back(std::thread(std::bind(&AsyncFileIO::ListionIOCP, this)));
                    }

                    m_diskSectorSize = static_cast<uint32_t>(GetSectorSize(filePath));
                    LOG(SPTAG::Helper::LogLevel::LL_Info, "Success open file handle: %s DiskSectorSize: %u\n", filePath, m_diskSectorSize);

                    PreAllocQueryContext();
                    return m_fileIocp.IsValid();
                }

                virtual std::uint64_t ReadBinary(std::uint64_t readSize, char* buffer, std::uint64_t offset = UINT64_MAX) 
                {
                    OVERLAPPED col;
                    memset(&col, 0, sizeof(col));
                    col.Offset = (offset & 0xffffffff);
                    col.OffsetHigh = (offset >> 32);

                    if (!::ReadFile(m_fileHandle.GetHandle(),
                        buffer,
                        static_cast<DWORD>(readSize),
                        nullptr,
                        &col) && GetLastError() != ERROR_IO_PENDING) return 0;

                    DWORD cBytes;
                    ULONG_PTR key;
                    OVERLAPPED* ol;
                    BOOL ret = ::GetQueuedCompletionStatus(this->m_fileIocp.GetHandle(),
                        &cBytes,
                        &key,
                        &ol,
                        INFINITE);
                    if (FALSE == ret || nullptr == ol) return 0;
                    return readSize;
                }

                virtual std::uint64_t WriteBinary(std::uint64_t writeSize, const char* buffer, std::uint64_t offset = UINT64_MAX)
                {
                    return 0;
                }

                virtual std::uint64_t ReadString(std::uint64_t& readSize, std::unique_ptr<char[]>& buffer, char delim = '\n', std::uint64_t offset = UINT64_MAX)
                {
                    return 0;
                }

                virtual std::uint64_t WriteString(const char* buffer, std::uint64_t offset = UINT64_MAX)
                {
                    return 0;
                }

                virtual bool ReadFileAsync(SPTAG::Helper::AsyncReadRequest& readRequest)
                {
                    ResourceType* resource = GetResource();

                    DiskUtils::CallbackOverLapped& col = resource->m_col;
                    memset(&col, 0, sizeof(col));
                    col.Offset = (readRequest.m_offset & 0xffffffff);
                    col.OffsetHigh = (readRequest.m_offset >> 32);
                    col.m_callback = &(readRequest.m_callback);

                    if (!::ReadFile(m_fileHandle.GetHandle(),
                        readRequest.m_buffer,
                        static_cast<DWORD>(readRequest.m_readSize),
                        nullptr,
                        &col) && GetLastError() != ERROR_IO_PENDING)
                    {
                        ReturnResource(resource);
                        return false;
                    }
                    return true;
                }

                virtual std::uint64_t TellP() { return 0; }

                virtual void ShutDown()
                {
                    m_fileHandle.Close();
                    m_fileIocp.Close();

                    for (auto& th : m_fileIocpThreads)
                    {
                        if (th.joinable())
                        {
                            th.join();
                        }
                    }

                    ResourceType* res = nullptr;
                    while (m_resources.pop(res))
                    {
                        if (res != nullptr)
                        {
                            delete res;
                        }
                    }
                }

            private:
                typedef DiskUtils::PrioritizedDiskFileReaderResource ResourceType;

                static uint64_t GetSectorSize(const char* p_filePath)
                {
                    DWORD dwSectorSize = 0;
                    LPTSTR lpFilePart;
                    // Get file sector size
                    DWORD dwSize = GetFullPathName(p_filePath, 0, NULL, &lpFilePart);
                    if (dwSize > 0)
                    {
                        LPTSTR lpBuffer = new TCHAR[dwSize];
                        if (lpBuffer)
                        {
                            DWORD dwResult = GetFullPathName(p_filePath, dwSize, lpBuffer, &lpFilePart);
                            if (dwResult > 0 && dwResult <= dwSize)
                            {
                                bool nameValid = false;
                                if (lpBuffer[0] == _T('\\'))
                                {
                                    if (lpBuffer[1] == _T('\\'))
                                    {
                                        DWORD i;
                                        if (dwSize > 2)
                                        {
                                            for (i = 2; lpBuffer[i] != 0 && lpBuffer[i] != _T('\\'); i++);
                                            if (lpBuffer[i] == _T('\\'))
                                            {
                                                for (i++; lpBuffer[i] != 0 && lpBuffer[i] != _T('\\'); i++);
                                                if (lpBuffer[i] == _T('\\'))
                                                {
                                                    lpBuffer[i + 1] = 0;
                                                    nameValid = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    if (((lpBuffer[0] >= _T('a') && lpBuffer[0] <= _T('z')) || (lpBuffer[0] >= _T('A') && lpBuffer[0] <= _T('Z'))) &&
                                        lpBuffer[1] == _T(':'))
                                    {
                                        if (lpBuffer[2] != 0)
                                        {
                                            lpBuffer[2] = _T('\\'); lpBuffer[3] = 0;
                                            nameValid = true;
                                        }
                                    }
                                }
                                if (nameValid)
                                {
                                    DWORD dwSPC, dwNOFC, dwTNOC;
                                    GetDiskFreeSpace(lpBuffer, &dwSPC, &dwSectorSize, &dwNOFC, &dwTNOC);
                                }
                            }
                            delete[] lpBuffer;
                        }
                    }

                    return dwSectorSize;
                }

                void ErrorExit()
                {
                    LPVOID lpMsgBuf;
                    DWORD dw = GetLastError();

                    FormatMessage(
                        FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL,
                        dw,
                        0,
                        (LPTSTR)&lpMsgBuf,
                        0, NULL);

                    // Display the error message and exit the process

                    LOG(Helper::LogLevel::LL_Error, "Failed with: %s\n", (char*)lpMsgBuf);

                    LocalFree(lpMsgBuf);
                    ExitProcess(dw);
                }

                void ListionIOCP()
                {
                    DWORD cBytes;
                    ULONG_PTR key;
                    OVERLAPPED* ol;
                    DiskUtils::CallbackOverLapped* col;

                    while (true)
                    {
                        BOOL ret = ::GetQueuedCompletionStatus(this->m_fileIocp.GetHandle(),
                            &cBytes,
                            &key,
                            &ol,
                            INFINITE);
                        if (FALSE == ret || nullptr == ol)
                        {
                            //We exit the thread
                            return;
                        }

                        col = (DiskUtils::CallbackOverLapped*)ol;
                        auto callback = col->m_callback;
                        ReturnResource(col->c_registeredResource);

                        if (nullptr != callback && (*callback))
                        {
                            (*callback)(true);
                        }
                    }
                }

                ResourceType* GetResource()
                {
                    ResourceType* ret = nullptr;
                    if (m_resources.pop(ret))
                    {
                    }
                    else
                    {
                        ret = new ResourceType();
                    }

                    return ret;
                }

                void ReturnResource(ResourceType* p_res)
                {
                    if (p_res != nullptr)
                    {
                        m_resources.push(p_res);
                    }
                }

                void PreAllocQueryContext()
                {
                    const size_t num = 64 * 64;
                    typedef ResourceType* ResourcePtr;

                    ResourcePtr* contextArray = new ResourcePtr[num];

                    for (int i = 0; i < num; ++i)
                    {
                        contextArray[i] = GetResource();
                    }

                    for (int i = 0; i < num; ++i)
                    {
                        ReturnResource(contextArray[i]);
                        contextArray[i] = nullptr;
                    }

                    delete[] contextArray;

                }

            private:
                HandleWrapper m_fileHandle;

                HandleWrapper m_fileIocp;

                std::vector<std::thread> m_fileIocpThreads;

                uint32_t m_diskSectorSize;

                boost::lockfree::stack<ResourceType*, boost::lockfree::capacity<128>> m_resources;
            };
        }
    }
}
