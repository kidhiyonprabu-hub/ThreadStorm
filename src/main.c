/*
 * ThreadStorm - main.c
 *
 * Entry point: parse command-line arguments, configure and start the server.
 * A Ctrl-C handler calls server_stop() for graceful shutdown.
 */

#include "common.h"
#include "server.h"

/* ── Global server instance ────────────────────────────────────────────── */
static Server g_srv;

/* ── Ctrl-C / SIGINT handler ──────────────────────────────────────────── */
static BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT
            || ctrl_type == CTRL_CLOSE_EVENT) {
        printf("\n[INFO ] Ctrl-C received – stopping server...\n");
        server_stop(&g_srv);
        return TRUE;
    }
    return FALSE;
}

/* ── Usage ────────────────────────────────────────────────────────────── */
static void print_usage(const char *prog) {
    printf("\nUsage: %s [options]\n\n"
           "  -p PORT       Port to listen on         (default: %d)\n"
           "  -w WORKERS    Worker thread count        (default: %d)\n"
           "  -q CAPACITY   Request queue capacity     (default: %d)\n"
           "  -a            Enable adaptive pool       (default: off)\n"
           "  -h            Print this help\n\n"
           "The server binds to %s only.\n\n",
           prog,
           SERVER_PORT, POOL_DEFAULT_WORKERS, QUEUE_DEFAULT_CAPACITY,
           SERVER_HOST);
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║        ThreadStorm  v1.0  –  Starting up         ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* Initialise server with defaults */
    memset(&g_srv, 0, sizeof(g_srv));
    server_default_config(&g_srv.config);
    g_srv.listen_sock = INVALID_SOCKET;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            int port = atoi(argv[++i]);
            if (port > 0 && port < 65536) g_srv.config.port = port;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            int w = atoi(argv[++i]);
            g_srv.config.worker_count = TS_CLAMP(w, POOL_MIN_WORKERS, POOL_MAX_WORKERS);
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            int q = atoi(argv[++i]);
            g_srv.config.queue_capacity = TS_CLAMP(q, 1, QUEUE_MAX_CAPACITY);
        } else if (strcmp(argv[i], "-a") == 0) {
            g_srv.config.adaptive_pool = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("[INFO ] Config: port=%d  workers=%d  queue=%d  adaptive=%s\n",
           g_srv.config.port,
           g_srv.config.worker_count,
           g_srv.config.queue_capacity,
           g_srv.config.adaptive_pool ? "yes" : "no");
    printf("[INFO ] Dashboard: http://%s:%d/\n\n", SERVER_HOST, g_srv.config.port);

    /* Register Ctrl-C handler */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    /* Start the server (blocks until shutdown) */
    bool ok = server_run(&g_srv);
    return ok ? 0 : 1;
}

