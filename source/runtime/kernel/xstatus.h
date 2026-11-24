#pragma once

// Xbox 360 kernel status codes and constants (X_ prefix so they never clash with windows.h).

constexpr uint32_t X_STATUS_SUCCESS                  = 0x00000000;
constexpr uint32_t X_STATUS_WAIT_0                   = 0x00000000;
constexpr uint32_t X_STATUS_ABANDONED_WAIT_0         = 0x00000080;
constexpr uint32_t X_STATUS_USER_APC                 = 0x000000C0;
constexpr uint32_t X_STATUS_ALERTED                  = 0x00000101;
constexpr uint32_t X_STATUS_TIMEOUT                  = 0x00000102;
constexpr uint32_t X_STATUS_PENDING                  = 0x00000103;
constexpr uint32_t X_STATUS_OBJECT_NAME_EXISTS       = 0x40000000;
constexpr uint32_t X_STATUS_BUFFER_OVERFLOW          = 0x80000005;
constexpr uint32_t X_STATUS_NO_MORE_FILES            = 0x80000006;
constexpr uint32_t X_STATUS_UNSUCCESSFUL             = 0xC0000001;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED          = 0xC0000002;
constexpr uint32_t X_STATUS_INFO_LENGTH_MISMATCH     = 0xC0000004;
constexpr uint32_t X_STATUS_ACCESS_VIOLATION         = 0xC0000005;
constexpr uint32_t X_STATUS_INVALID_HANDLE           = 0xC0000008;
constexpr uint32_t X_STATUS_INVALID_PARAMETER        = 0xC000000D;
constexpr uint32_t X_STATUS_NO_SUCH_FILE             = 0xC000000F;
constexpr uint32_t X_STATUS_END_OF_FILE              = 0xC0000011;
constexpr uint32_t X_STATUS_NO_MEMORY                = 0xC0000017;
constexpr uint32_t X_STATUS_ALREADY_COMMITTED        = 0xC0000021;
constexpr uint32_t X_STATUS_ACCESS_DENIED            = 0xC0000022;
constexpr uint32_t X_STATUS_BUFFER_TOO_SMALL         = 0xC0000023;
constexpr uint32_t X_STATUS_OBJECT_TYPE_MISMATCH     = 0xC0000024;
constexpr uint32_t X_STATUS_OBJECT_NAME_INVALID      = 0xC0000033;
constexpr uint32_t X_STATUS_OBJECT_NAME_NOT_FOUND    = 0xC0000034;
constexpr uint32_t X_STATUS_OBJECT_NAME_COLLISION    = 0xC0000035;
constexpr uint32_t X_STATUS_OBJECT_PATH_NOT_FOUND    = 0xC000003A;
constexpr uint32_t X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t X_STATUS_MUTANT_NOT_OWNED         = 0xC0000046;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED     = 0xC00000A0;
constexpr uint32_t X_STATUS_FILE_IS_A_DIRECTORY      = 0xC00000BA;
constexpr uint32_t X_STATUS_NOT_SUPPORTED            = 0xC00000BB;
constexpr uint32_t X_STATUS_DEVICE_NOT_CONNECTED     = 0xC000009D;
constexpr uint32_t X_STATUS_NOT_A_DIRECTORY          = 0xC0000103;
constexpr uint32_t X_STATUS_INVALID_PARAMETER_2      = 0xC00000F0;

// Win32 error codes returned by XAM
constexpr uint32_t X_ERROR_SUCCESS               = 0x00000000;
constexpr uint32_t X_ERROR_ACCESS_DENIED         = 0x00000005;
constexpr uint32_t X_ERROR_INVALID_PARAMETER     = 0x00000057;
constexpr uint32_t X_ERROR_IO_PENDING            = 0x000003E5;
constexpr uint32_t X_ERROR_CANCELLED             = 0x000004C7;
constexpr uint32_t X_ERROR_NO_SUCH_USER          = 0x00000525;
constexpr uint32_t X_ERROR_FUNCTION_FAILED       = 0x0000065B;
constexpr uint32_t X_ERROR_NOT_FOUND             = 0x00000490;
constexpr uint32_t X_ERROR_NO_MORE_FILES         = 0x00000012;
constexpr uint32_t X_ERROR_EMPTY                 = 0x000010D2;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED  = 0x0000048F;
constexpr uint32_t X_ERROR_PATH_NOT_FOUND        = 0x00000003;

constexpr uint32_t X_INFINITE = 0xFFFFFFFF;

// Memory
constexpr uint32_t X_MEM_COMMIT      = 0x00001000;
constexpr uint32_t X_MEM_RESERVE     = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT    = 0x00004000;
constexpr uint32_t X_MEM_RELEASE     = 0x00008000;
constexpr uint32_t X_MEM_FREE        = 0x00010000;
constexpr uint32_t X_MEM_PRIVATE     = 0x00020000;
constexpr uint32_t X_MEM_RESET       = 0x00080000;
constexpr uint32_t X_MEM_TOP_DOWN    = 0x00100000;
constexpr uint32_t X_MEM_NOZERO      = 0x00800000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_16MB_PAGES  = 0x80000000;

constexpr uint32_t X_PAGE_NOACCESS  = 0x01;
constexpr uint32_t X_PAGE_READONLY  = 0x02;
constexpr uint32_t X_PAGE_READWRITE = 0x04;
