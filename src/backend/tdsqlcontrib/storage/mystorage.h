#pragma once

void *handle_process_kv_req(void *job);

void *packjob(int pid, const char *req, int reqlen, int isLeader);
