#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mp4_dist_config.h"
#include "ipc/ipc_server.h"

static void usage(const char* prog) {
  fprintf(stderr, "Usage: %s [--config <json>] [--ipc-sock <path>]\n", prog);
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

  if (config_path) {
    mp4_dist_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (mp4_dist_config_load(config_path, &cfg) != 0) {
      fprintf(stderr, "failed to load config: %s\n", config_path);
      return 1;
    }
    printf("dvs-media: loaded config %s\n", config_path);
  }

  if (ipc_sock) {
    printf("dvs-media: ipc socket %s\n", ipc_sock);
    return dvs_ipc_server_run(ipc_sock);
  } else {
    printf("dvs-media: static mode\n");
  }

  /* Keep process alive briefly then exit for build/smoke testing. */
  sleep(1);
  return 0;
}
