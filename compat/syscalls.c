#include <stddef.h>
#include <stdint.h>

/*
 * Microsoft x64 ABI attributes are supported by GCC and Clang on x86-64.
 * They ensure RCX, RDX, R8, and R9 carry the first four arguments.
 */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define WIN64_ABI __attribute__((ms_abi))
#else
#error "This Win32 compatibility shim requires GCC/Clang x86-64 ms_abi support"
#endif

typedef void *win_handle_t;
typedef uint32_t win_dword_t;
typedef int32_t win_bool_t;

/* Win32-style standard-handle identifiers. */
#define WIN_STD_INPUT_HANDLE  ((uint32_t)-10)
#define WIN_STD_OUTPUT_HANDLE ((uint32_t)-11)
#define WIN_STD_ERROR_HANDLE  ((uint32_t)-12)

#define WIN_ERROR_INVALID_HANDLE 6U
#define WIN_ERROR_INVALID_PARAMETER 87U
#define WIN_ERROR_NOT_SUPPORTED 50U
#define WIN_ERROR_SUCCESS 0U

#define WIN_DUMMY_INPUT_HANDLE  ((uintptr_t)0x1001U)
#define WIN_CONSOLE_OUT_HANDLE  ((uintptr_t)0x1002U)
#define WIN_CONSOLE_ERR_HANDLE  ((uintptr_t)0x1003U)

/*
 * Kernel-provided integration hooks.
 *
 * vesa_console_write must synchronously consume exactly `length` bytes and
 * return the number of bytes written, or a negative value on failure.
 * kernel_process_exit must terminate the current user process and not return.
 * kernel_set_last_error stores a per-process Win32-compatible last-error value.
 */
extern int64_t vesa_console_write(const char *bytes, size_t length,
                                  uint32_t stream);
extern void kernel_process_exit(uint32_t exit_code);
extern void kernel_set_last_error(uint32_t error_code);

static void set_error(uint32_t error_code)
{
    kernel_set_last_error(error_code);
}

static int is_console_handle(win_handle_t handle, uint32_t *stream)
{
    uintptr_t value = (uintptr_t)handle;

    if (value == WIN_CONSOLE_OUT_HANDLE) {
        if (stream != NULL) {
            *stream = 0;
        }
        return 1;
    }

    if (value == WIN_CONSOLE_ERR_HANDLE) {
        if (stream != NULL) {
            *stream = 1;
        }
        return 1;
    }

    return 0;
}

/*
 * HANDLE WINAPI GetStdHandle(DWORD nStdHandle)
 *
 * This returns process-local pseudo-handles, not host pointers or device
 * addresses. The PE import resolver must bind the imported name to this
 * function address.
 */
WIN64_ABI win_handle_t GetStdHandle(win_dword_t standard_handle)
{
    switch (standard_handle) {
    case WIN_STD_INPUT_HANDLE:
        set_error(WIN_ERROR_SUCCESS);
        return (win_handle_t)WIN_DUMMY_INPUT_HANDLE;

    case WIN_STD_OUTPUT_HANDLE:
        set_error(WIN_ERROR_SUCCESS);
        return (win_handle_t)WIN_CONSOLE_OUT_HANDLE;

    case WIN_STD_ERROR_HANDLE:
        set_error(WIN_ERROR_SUCCESS);
        return (win_handle_t)WIN_CONSOLE_ERR_HANDLE;

    default:
        set_error(WIN_ERROR_INVALID_PARAMETER);
        return (win_handle_t)(intptr_t)-1;
    }
}

/*
 * BOOL WINAPI WriteFile(
 *     HANDLE hFile,
 *     LPCVOID lpBuffer,
 *     DWORD nNumberOfBytesToWrite,
 *     LPDWORD lpNumberOfBytesWritten,
 *     LPOVERLAPPED lpOverlapped);
 *
 * Overlapped/asynchronous writes are deliberately unsupported. The shim
 * validates NULL pointers and only accepts the two console output handles.
 */
WIN64_ABI win_bool_t WriteFile(win_handle_t file_handle,
                               const void *buffer,
                               win_dword_t bytes_to_write,
                               win_dword_t *bytes_written,
                               void *overlapped)
{
    uint32_t stream;

    if (bytes_written != NULL) {
        *bytes_written = 0;
    }

    if (overlapped != NULL) {
        set_error(WIN_ERROR_NOT_SUPPORTED);
        return 0;
    }

    if (bytes_to_write != 0 && buffer == NULL) {
        set_error(WIN_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!is_console_handle(file_handle, &stream)) {
        set_error(WIN_ERROR_INVALID_HANDLE);
        return 0;
    }

    if (bytes_to_write == 0) {
        set_error(WIN_ERROR_SUCCESS);
        return 1;
    }

    int64_t result = vesa_console_write((const char *)buffer,
                                        (size_t)bytes_to_write,
                                        stream);

    if (result < 0 || (uint64_t)result > bytes_to_write) {
        set_error(WIN_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (bytes_written != NULL) {
        *bytes_written = (win_dword_t)result;
    }

    if ((win_dword_t)result != bytes_to_write) {
        set_error(WIN_ERROR_INVALID_PARAMETER);
        return 0;
    }

    set_error(WIN_ERROR_SUCCESS);
    return 1;
}

/*
 * VOID WINAPI ExitProcess(UINT uExitCode)
 *
 * This must not return. If the kernel exit hook unexpectedly returns, halt
 * rather than falling through into unknown user code.
 */
WIN64_ABI void ExitProcess(win_dword_t exit_code)
{
    kernel_process_exit(exit_code);

    for (;;) {
        __asm__ volatile("cli; hlt" ::: "memory");
    }
}
