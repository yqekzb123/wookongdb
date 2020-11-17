#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern int initReplicadStorage(int segidx);
extern void *echo(int pid, const char *message, int messageLen);

#ifdef __cplusplus
}
#endif
