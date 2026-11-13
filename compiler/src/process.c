#include "vxn.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char *command_line(const char *const argv[]) {
    size_t cap = 1;
    for (size_t i = 0; argv[i]; ++i) {
        size_t n = strlen(argv[i]);
        if (n > 16000 || cap + 2 * n + 3 > 32767) return NULL;
        cap += 2 * n + 3;
    }
    char *line = malloc(cap), *q = line;
    if (!line) return NULL;
    for (size_t i = 0; argv[i]; ++i) {
        if (i) *q++ = ' ';
        *q++ = '"'; const char *s = argv[i];
        while (*s) {
            size_t slashes = 0;
            while (*s == '\\') { ++slashes; ++s; }
            size_t copies = (*s == '"' || !*s) ? slashes * 2 : slashes;
            while (copies--) *q++ = '\\';
            if (*s == '"') *q++ = '\\';
            if (*s) *q++ = *s++;
        }
        *q++ = '"';
    }
    *q = 0; return line;
}
int vxn_process_redirect_io(const char *const argv[], const char *in, const char *out, const char *err) {
    if (!argv || !argv[0]) return -1;
    char *line = command_line(argv); if (!line) return -1;
    STARTUPINFOA start; PROCESS_INFORMATION child;
    memset(&start, 0, sizeof start); memset(&child, 0, sizeof child); start.cb = sizeof start;
    SECURITY_ATTRIBUTES security = { sizeof security, NULL, TRUE };
    HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE, error = INVALID_HANDLE_VALUE;
    if (in) input = CreateFileA(in, GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out) output = CreateFileA(out, GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (err) error = CreateFileA(err, GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if ((in && input == INVALID_HANDLE_VALUE) || (out && output == INVALID_HANDLE_VALUE) || (err && error == INVALID_HANDLE_VALUE)) {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
        free(line); return -1;
    }
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdInput = in ? input : GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = out ? output : GetStdHandle(STD_OUTPUT_HANDLE);
    start.hStdError = err ? error : GetStdHandle(STD_ERROR_HANDLE);
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &start, &child);
    free(line);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
    if (!ok) return -1;
    DWORD code = 0;
    if (WaitForSingleObject(child.hProcess, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.hProcess, &code)) code = 255;
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    return code <= 255 ? (int)code : 255;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
static void redirect(const char *path, int fd) {
    if (!path) return;
    int opened = open(path, fd == STDIN_FILENO ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (opened < 0 || dup2(opened, fd) < 0) _exit(127);
    if (opened != fd) close(opened);
}
int vxn_process_redirect_io(const char *const argv[], const char *in, const char *out, const char *err) {
    if (!argv || !argv[0]) return -1;
    pid_t child = fork(); if (child < 0) return -1;
    if (!child) {
        redirect(in, STDIN_FILENO); redirect(out, STDOUT_FILENO); redirect(err, STDERR_FILENO);
        execvp(argv[0], (char *const *)argv); _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#endif
int vxn_process_redirect(const char *const argv[], const char *out, const char *err) { return vxn_process_redirect_io(argv,NULL,out,err); }
int vxn_process(const char *const argv[]) { return vxn_process_redirect(argv, NULL, NULL); }
