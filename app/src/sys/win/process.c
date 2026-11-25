#include "util/process.h"

#include <processthreadsapi.h>

#include <assert.h>
#include <stdlib.h>

#include "util/command.h"
#include "util/log.h"
#include "util/str.h"

#define CMD_MAX_LEN 8192

// Return an inheritable duplicate of `h` (NULL if `h` is not usable)
static HANDLE
dup_inheritable(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    HANDLE dup;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0,
                         TRUE, DUPLICATE_SAME_ACCESS)) {
        return NULL;
    }
    return dup;
}

// Open the NUL device as an inheritable handle
static HANDLE
open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : h;
}

enum sc_process_result
sc_process_execute_p(const char *const argv[], HANDLE *handle, unsigned flags,
                     HANDLE *pin, HANDLE *pout, HANDLE *perr) {
    bool inherit_stdout = !pout && !(flags & SC_PROCESS_NO_STDOUT);
    bool inherit_stderr = !perr && !(flags & SC_PROCESS_NO_STDERR);

    // Add 1 per non-NULL pointer
    unsigned handle_count = !!pin + !!pout + !!perr;

    enum sc_process_result ret = SC_PROCESS_ERROR_GENERIC;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read_handle;
    HANDLE stdout_write_handle;
    HANDLE stderr_write_handle;
    if (pin) {
        if (!CreatePipe(&stdin_read_handle, pin, &sa, 0)) {
            perror("pipe");
            return SC_PROCESS_ERROR_GENERIC;
        }
        if (!SetHandleInformation(*pin, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stdin failed");
            goto error_close_stdin;
        }
    }
    if (pout) {
        if (!CreatePipe(pout, &stdout_write_handle, &sa, 0)) {
            perror("pipe");
            goto error_close_stdin;
        }
        if (!SetHandleInformation(*pout, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stdout failed");
            goto error_close_stdout;
        }
    }
    if (perr) {
        if (!CreatePipe(perr, &stderr_write_handle, &sa, 0)) {
            perror("pipe");
            goto error_close_stdout;
        }
        if (!SetHandleInformation(*perr, HANDLE_FLAG_INHERIT, 0)) {
            LOGE("SetHandleInformation stderr failed");
            goto error_close_stderr;
        }
    }

    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    HANDLE handles[3];
    // Handles created only for the child (closed after CreateProcessW())
    HANDLE extra[3];
    unsigned extra_count = 0;
    unsigned list_count = 0;

    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (inherit_stdout) {
        si.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (inherit_stderr) {
        si.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList = NULL;
    if (handle_count) {
        unsigned i = 0;
        if (pin) {
            si.StartupInfo.hStdInput = stdin_read_handle;
            handles[i++] = si.StartupInfo.hStdInput;
        }
        if (pout) {
            assert(!inherit_stdout);
            si.StartupInfo.hStdOutput = stdout_write_handle;
            handles[i++] = si.StartupInfo.hStdOutput;
        }
        if (perr) {
            assert(!inherit_stderr);
            si.StartupInfo.hStdError = stderr_write_handle;
            handles[i++] = si.StartupInfo.hStdError;
        }

        if (!pin) {
            HANDLE h = open_nul(GENERIC_READ);
            if (h) {
                si.StartupInfo.hStdInput = h;
                handles[i++] = h;
                extra[extra_count++] = h;
            }
        }
        if (!pout) {
            HANDLE h = inherit_stdout
                     ? dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE))
                     : open_nul(GENERIC_WRITE);
            if (h) {
                si.StartupInfo.hStdOutput = h;
                handles[i++] = h;
                extra[extra_count++] = h;
            }
        }
        if (!perr) {
            HANDLE h = inherit_stderr
                     ? dup_inheritable(GetStdHandle(STD_ERROR_HANDLE))
                     : open_nul(GENERIC_WRITE);
            if (h) {
                si.StartupInfo.hStdError = h;
                handles[i++] = h;
                extra[extra_count++] = h;
            }
        }
        list_count = i;

        SIZE_T size;
        // Call it once to know the required buffer size
        BOOL ok =
            InitializeProcThreadAttributeList(NULL, 1, 0, &size)
                || GetLastError() == ERROR_INSUFFICIENT_BUFFER;
        if (!ok) {
            goto error_close_stderr;
        }

        lpAttributeList = malloc(size);
        if (!lpAttributeList) {
            LOG_OOM();
            goto error_close_stderr;
        }

        ok = InitializeProcThreadAttributeList(lpAttributeList, 1, 0, &size);
        if (!ok) {
            free(lpAttributeList);
            goto error_close_stderr;
        }

        ok = UpdateProcThreadAttribute(lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles, list_count * sizeof(HANDLE),
                                       NULL, NULL);
        if (!ok) {
            goto error_free_attribute_list;
        }

        si.lpAttributeList = lpAttributeList;
    }

    assert(argv && *argv);
    char *cmd = sc_command_serialize_windows(argv);
    if (!cmd) {
        LOG_OOM();
        goto error_free_attribute_list;
    }

    wchar_t *wide = sc_str_to_wchars(cmd);
    free(cmd);
    if (!wide) {
        LOG_OOM();
        goto error_free_attribute_list;
    }

    BOOL bInheritHandles = handle_count > 0 || inherit_stdout || inherit_stderr;
    DWORD dwCreationFlags = 0;
    if (handle_count > 0) {
        dwCreationFlags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    if (!inherit_stdout && !inherit_stderr) {
        // DETACHED_PROCESS to disable stdin, stdout and stderr
        dwCreationFlags |= DETACHED_PROCESS;
    }
    BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, bInheritHandles,
                             dwCreationFlags, NULL, NULL, &si.StartupInfo, &pi);
    free(wide);
    for (unsigned k = 0; k < extra_count; ++k) {
        CloseHandle(extra[k]);
    }
    extra_count = 0;
    if (!ok) {
        int err = GetLastError();
        LOGE("CreateProcessW() error %d", err);
        if (err == ERROR_FILE_NOT_FOUND) {
            ret = SC_PROCESS_ERROR_MISSING_BINARY;
        }
        goto error_free_attribute_list;
    }

    if (lpAttributeList) {
        DeleteProcThreadAttributeList(lpAttributeList);
        free(lpAttributeList);
    }

    CloseHandle(pi.hThread);

    // These handles are used by the child process, close them for this process
    if (pin) {
        CloseHandle(stdin_read_handle);
    }
    if (pout) {
        CloseHandle(stdout_write_handle);
    }
    if (perr) {
        CloseHandle(stderr_write_handle);
    }

    *handle = pi.hProcess;

    return SC_PROCESS_SUCCESS;

error_free_attribute_list:
    for (unsigned k = 0; k < extra_count; ++k) {
        CloseHandle(extra[k]);
    }
    if (lpAttributeList) {
        DeleteProcThreadAttributeList(lpAttributeList);
        free(lpAttributeList);
    }
error_close_stderr:
    if (perr) {
        CloseHandle(*perr);
        CloseHandle(stderr_write_handle);
    }
error_close_stdout:
    if (pout) {
        CloseHandle(*pout);
        CloseHandle(stdout_write_handle);
    }
error_close_stdin:
    if (pin) {
        CloseHandle(*pin);
        CloseHandle(stdin_read_handle);
    }

    return ret;
}

bool
sc_process_terminate(HANDLE handle) {
    return TerminateProcess(handle, 1);
}

sc_exit_code
sc_process_wait(HANDLE handle, bool close) {
    DWORD code;
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0
            || !GetExitCodeProcess(handle, &code)) {
        // could not wait or retrieve the exit code
        code = SC_EXIT_CODE_NONE;
    }
    if (close) {
        CloseHandle(handle);
    }
    return code;
}

void
sc_process_close(HANDLE handle) {
    bool closed = CloseHandle(handle);
    assert(closed);
    (void) closed;
}

ssize_t
sc_pipe_read(HANDLE pipe, char *data, size_t len) {
    DWORD r;
    if (!ReadFile(pipe, data, len, &r, NULL)) {
        return -1;
    }
    return r;
}

ssize_t
sc_pipe_write(HANDLE pipe, const char *data, size_t len) {
    DWORD w;
    if (!WriteFile(pipe, data, len, &w, NULL)) {
        return -1;
    }
    return w;
}

void
sc_pipe_close(HANDLE pipe) {
    if (!CloseHandle(pipe)) {
        LOGW("Cannot close pipe");
    }
}
