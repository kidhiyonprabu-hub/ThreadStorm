/*
 * ThreadStorm - common.c
 *
 * Shared utilities: logging, timestamps.
 */

#include "common.h"
#include <stdarg.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

/* ── Monotonic timer ──────────────────────────────────────────────────── */
static LARGE_INTEGER g_freq   = {0};
static LARGE_INTEGER g_origin = {0};
static bool          g_timer_init = false;

long long ts_now_ms(void) {
    if (!g_timer_init) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_origin);
        g_timer_init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (long long)((now.QuadPart - g_origin.QuadPart) * 1000LL
                       / g_freq.QuadPart);
}

/* ── Log file handle ──────────────────────────────────────────────────── */
static FILE *g_log_fp = NULL;
static CRITICAL_SECTION g_log_lock;
static bool g_log_lock_init = false;

static void ensure_log_open(void) {
    if (!g_log_fp) {
        /* Create logs/ directory in case it doesn't exist yet */
        CreateDirectoryA("logs", NULL);
        g_log_fp = fopen(LOG_FILE, "a");
        if (!g_log_lock_init) {
            InitializeCriticalSection(&g_log_lock);
            g_log_lock_init = true;
        }
    }
}

void ts_log(const char *level, const char *fmt, ...) {
    ensure_log_open();

    /* Format timestamp */
    time_t t = time(NULL);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_buf);

    /* Format message */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_log_lock_init) EnterCriticalSection(&g_log_lock);

    /* Write to console */
    printf("[%s] [%s] %s\n", time_str, level, msg);

    /* Write to log file */
    if (g_log_fp) {
        fprintf(g_log_fp, "[%s] [%s] %s\n", time_str, level, msg);
        fflush(g_log_fp);
    }

    if (g_log_lock_init) LeaveCriticalSection(&g_log_lock);
}

/* ── System resource metrics (CO3: Memory & CPU Telemetry) ───────────── */
void ts_get_memory_usage(size_t *working_set_kb, size_t *peak_working_set_kb, size_t *private_kb) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    ZeroMemory(&pmc, sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        if (working_set_kb)      *working_set_kb      = (size_t)(pmc.WorkingSetSize / 1024);
        if (peak_working_set_kb) *peak_working_set_kb = (size_t)(pmc.PeakWorkingSetSize / 1024);
        if (private_kb)          *private_kb          = (size_t)(pmc.PrivateUsage / 1024);
    } else {
        if (working_set_kb)      *working_set_kb      = 0;
        if (peak_working_set_kb) *peak_working_set_kb = 0;
        if (private_kb)          *private_kb          = 0;
    }
}

static ULONGLONG g_last_proc_time = 0;
static ULONGLONG g_last_sys_time  = 0;
static int       g_num_processors = 0;

double ts_get_cpu_usage(void) {
    if (g_num_processors == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_num_processors = (int)si.dwNumberOfProcessors;
        if (g_num_processors < 1) g_num_processors = 1;
    }

    FILETIME ftCreation, ftExit, ftKernel, ftUser;
    FILETIME ftSysIdle, ftSysKernel, ftSysUser;

    if (!GetProcessTimes(GetCurrentProcess(), &ftCreation, &ftExit, &ftKernel, &ftUser)) {
        return 0.0;
    }
    if (!GetSystemTimes(&ftSysIdle, &ftSysKernel, &ftSysUser)) {
        return 0.0;
    }

    ULARGE_INTEGER pk, pu;
    pk.LowPart = ftKernel.dwLowDateTime; pk.HighPart = ftKernel.dwHighDateTime;
    pu.LowPart = ftUser.dwLowDateTime;   pu.HighPart = ftUser.dwHighDateTime;
    ULONGLONG proc_time = pk.QuadPart + pu.QuadPart;

    ULARGE_INTEGER sk, su;
    sk.LowPart = ftSysKernel.dwLowDateTime; sk.HighPart = ftSysKernel.dwHighDateTime;
    su.LowPart = ftSysUser.dwLowDateTime;   su.HighPart = ftSysUser.dwHighDateTime;
    ULONGLONG sys_time = sk.QuadPart + su.QuadPart;

    if (g_last_proc_time == 0 || g_last_sys_time == 0 || sys_time <= g_last_sys_time) {
        g_last_proc_time = proc_time;
        g_last_sys_time  = sys_time;
        return 0.0;
    }

    ULONGLONG proc_delta = proc_time - g_last_proc_time;
    ULONGLONG sys_delta  = sys_time - g_last_sys_time;
    g_last_proc_time = proc_time;
    g_last_sys_time  = sys_time;

    if (sys_delta == 0) return 0.0;
    double cpu_pct = ((double)proc_delta / (double)sys_delta) * 100.0;
    if (cpu_pct < 0.0) cpu_pct = 0.0;
    if (cpu_pct > 100.0) cpu_pct = 100.0;
    return cpu_pct;
}


