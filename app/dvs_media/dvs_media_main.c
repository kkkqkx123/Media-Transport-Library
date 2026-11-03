#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>

#include "file_playout_config.h"
#include "ipc/ipc_server.h"
#include "engine/capability.h"
#include "engine/session_mgr.h"
#include "engine/engine_ctx.h"

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int s) {
    (void)s;
    g_stop = 1;
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s --config <json> [--ipc-sock <path>]\n"
            "  --config   static JSON config (network/DRM/MTL + initial layout)\n"
            "  --ipc-sock run controlled by IPC; layout applied via configure/commit\n",
            prog);
}

int main(int argc, char** argv) {
    const char* config_path = NULL;
    const char* ipc_sock = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--ipc-sock") == 0 && i + 1 < argc) {
            ipc_sock = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Probe node capability once (DRM/MPP/MTL; best-effort, never aborts). */
    dvs_capability cap;
    dvs_capability_probe(&cap);
    char* cap_json = dvs_capability_to_json(&cap);
    printf("dvs-media: capability %s\n", cap_json ? cap_json : "{}");
    free(cap_json);
    dvs_capability_free(&cap);

    file_playout_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config_path) {
        if (file_playout_config_load(config_path, &cfg) != 0) {
            fprintf(stderr, "failed to load config: %s\n", config_path);
            return 1;
        }
        printf("dvs-media: loaded config %s\n", config_path);
    }

    /* Build the engine from the (possibly empty) base config. */
    dvs_engine* eng = dvs_session_mgr_create(&cfg);
    if (!eng) {
        fprintf(stderr, "dvs-media: engine create failed\n");
        return 1;
    }

    if (ipc_sock) {
        /* Controlled mode: start idle (apply initial layout if present), then
         * serve IPC. Layout changes arrive via configure/commit. */
        if (cfg.pip_stream_count > 0) {
            if (dvs_engine_start(eng) != 0)
                fprintf(stderr, "dvs-media: initial layout start failed\n");
        } else {
            dvs_engine_init_mtl(eng);
        }
        dvs_ipc_attach_engine(eng);
        printf("dvs-media: ipc socket %s\n", ipc_sock);
        int rc = dvs_ipc_server_run(ipc_sock);
        dvs_engine_stop(eng);
        dvs_session_mgr_destroy(eng);
        return rc;
    }

    /* Static mode: apply the config layout and run until timeout/stop. */
    if (cfg.pip_stream_count <= 0) {
        fprintf(stderr, "dvs-media: static mode needs pip_streams in config\n");
        dvs_session_mgr_destroy(eng);
        return 1;
    }
    if (dvs_engine_start(eng) != 0) {
        fprintf(stderr, "dvs-media: media start failed\n");
        dvs_session_mgr_destroy(eng);
        return 1;
    }
    printf("dvs-media: static mode running (Ctrl-C to stop)\n");
    while (!g_stop && atomic_load(&eng->running)) sleep(1);
    dvs_engine_stop(eng);
    dvs_session_mgr_destroy(eng);
    printf("dvs-media: stopped\n");
    return 0;
}
