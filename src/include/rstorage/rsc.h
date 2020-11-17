#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern void *handle_process_kv_req(void *job);

extern void *packjob(int pid, const char *req, int reqlen, int isLeader);

#ifdef __cplusplus
}
#endif
