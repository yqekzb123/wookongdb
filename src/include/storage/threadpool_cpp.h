#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern int pool_add_worker(void *(*process)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif