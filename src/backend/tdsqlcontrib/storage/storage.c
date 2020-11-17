/*-------------------------------------------------------------------------
 *
 * storaged.c
 *		bgworker for storage engine
 *
 * Copyright (c) 2019-Present, TDSQL
 *
 * IDENTIFICATION
 *		storage/storage.c
 *
 *-------------------------------------------------------------------------
 */

/* Minimum set of headers */
#include <signal.h>
#include <mcheck.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "postgres.h"

#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/lwlock.h"
#include "storage/shm_toc.h"
#include "storage/shm_mq.h"
#include "storage/threadpool.h"
#include "fmgr.h"
#include "utils/resowner.h"
#include "access/xact.h"
#include "tdb/storage_processor.h"
#include "tdb/session_processor.h"
#include "tdb/range_processor.h"
#include "tdb/rocks_engine.h"
#include "tdb/bootstraprange.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "utils/memutils.h"
#include "tdb/storage_param.h"
#include "postmaster/fts.h"
#include "postmaster/postmaster.h"
#include "storage/pmsignal.h"
#include "librw/librm.h"
#include "rstorage/rs.h"
#include "mystorage.h"
#include "tdb/timestamp_transaction/http.h"

/*
 * Our standard signal and real-time signal can be received at same time, so it
 * is possible mq_handle has not be created. If it has not be created, we should
 * wait for it.
 *
 * Note that this function and standard signal callback function should not be in
 * the same thread, or it may cause an endless loop.
 */

/*
 * TODO: Hiding bugs may exist. 32768 is just the default pid_max in Linux,
 * according to /proc/sys/kernel/pid_max.
 */
#define MAX_PID 32768

#if (HANDLE_STORAGE == 1)
List *mq_handles = NULL;
#elif (HANDLE_STORAGE == 2)
KVEngineMQHandleData mq_handles[MAX_PID + 1];
#endif
__thread KVEngineMQHandle current_handle = NULL;
#define wait_for_attach_finished(pid) while (mq_handles[pid].status == KV_MQ_NOT_ATTACHED)

MemoryContext TxnRWSetContext = NULL;

static void storage_sigterm(SIGNAL_ARGS);
static void register_signals(void);
static void storage_attach_session(AttachRequestData attach_req);
static dsm_segment *storage_attach_dsm_with_toc(dsm_handle handle, shm_toc **toc);
static dsm_segment *storage_mapping_dsm_with_toc(dsm_handle handle, shm_toc **toc);
static shm_mq_handle *storage_attach_req_mq(shm_toc *toc, dsm_segment *seg);
static shm_mq_handle *storage_attach_res_mq(shm_toc *toc, dsm_segment *seg);
static void *pre_handle_process_kv_req(void *job);
static void storage_detach(int pid);
static KVEngineInterface *create_engine(KVEngineType type);
static void check_all_req(void);
static void check_kv_req(void);
static ResponseHeader *process_kv_request(RequestHeader *req);

typedef struct {
    int pid;
    int isLeader;
    RequestHeader *req;
} PoolThreadJob;

void *packjob(int pid, const char *req, int reqlen, int isLeader) {
    PoolThreadJob *tmp = malloc(sizeof(PoolThreadJob));
    tmp->pid = pid;
    tmp->isLeader = isLeader;
    tmp->req = malloc(reqlen);
    memcpy(tmp->req, req, reqlen);
    return (void *) tmp;
}

#if (HANDLE_STORAGE == 1)
static ThreadJob check_and_receive_kv_req(KVEngineMQHandle handle);
#elif (HANDLE_STORAGE == 2)
static ThreadJob check_and_receive_kv_req(int pid);
#endif

static void BootStrapThreadLock(void);
/* Signal handling */
static volatile sig_atomic_t got_sigterm = false;

static void
freeJob(ThreadJob job) {
    range_free(job);
}


KVEngineInterface *
create_engine(KVEngineType type) {
    switch (type) {
        case KVENGINE_ROCKSDB:
            return rocks_create_normal_engine();
        case KVENGINE_TRANSACTIONDB:
            return rocks_create_optimistic_transactions_engine();
        default:
            Assert(false);
            return NULL;
    }
}

/*
 * storage_sigterm
 *
 * SIGTERM handler.
 */
void storage_sigterm(SIGNAL_ARGS) {
    int save_errno = errno;
    got_sigterm = true;
    if (MyProc)
        SetLatch(&MyProc->procLatch);
    errno = save_errno;
}

static void
BootStrapThreadLock(void) {
    am_kv_storage = true;
    enable_thread_lock = true;
    enable_req_failed_retry = false;
    memorycontext_lock = true;
    if (CurrentThreadMemoryContext == NULL)
        CurrentThreadMemoryContext = AllocSetContextCreate(TopMemoryContext,
                                                           "Storage Work Main Thread Top Context",
                                                           ALLOCSET_DEFAULT_MINSIZE,
                                                           ALLOCSET_DEFAULT_INITSIZE,
                                                           ALLOCSET_DEFAULT_MAXSIZE);
    DEFAULT_REPLICA_NUM = enable_paxos ? 3 : 1;
    if (enable_thread_lock) {
#ifndef USE_SPIN_LOCK
        pthread_mutex_init(&(StorageThreadLock._Thread_MemoryContext_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_VisionJudge_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_WBHTAB_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_LWinte_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_LWarray_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_Update_Lock), NULL);
        pthread_mutex_init(&(StorageThreadLock._Thread_TupleLockHash_Lock), NULL);
#else
        SpinLockInit(&StorageThreadLock._Thread_MemoryContext_Lock);
        SpinLockInit(&StorageThreadLock._Thread_VisionJudge_Lock);
        SpinLockInit(&StorageThreadLock._Thread_WBHTAB_Lock);
        SpinLockInit(&StorageThreadLock._Thread_LWinte_Lock);
        SpinLockInit(&StorageThreadLock._Thread_LWarray_Lock);
        SpinLockInit(&StorageThreadLock._Thread_Update_Lock);
        SpinLockInit(&StorageThreadLock._Thread_TupleLockHash_Lock);
        SpinLockInit(&StorageThreadLock._Thread_RWSET_Lock);
#endif
        StorageHaveLock.HAVE_MemoryContext_LOCK = false;
        StorageHaveLock.HAVE_VisionJudge_LOCK = false;
        StorageHaveLock.HAVE_WBHTAB_LOCK = false;
        StorageHaveLock.HAVE_LWinte_LOCK = false;
        StorageHaveLock.HAVE_LWarray_LOCK = false;
        StorageHaveLock.HAVE_Update_LOCK = false;
        StorageHaveLock.HAVE_TupleLockHash_LOCK = false;
        StorageHaveLock.HAVE_RWSET_LOCK = false;

        StorageInitTupleLockHash();
        InitTxnLock();
    }

    if (enable_dynamic_lock) {
        InitThreadLock();
    }
}

bool StorageStartRule(Datum main_arg) {
    /* we start storage on master only when -E is specified */
    return am_mirror ? false : true;
}

FILE *logfile;

/*
 * storage_main
 * Main loop processing.
 */
void storage_main(Datum main_arg) {
    char buff[20];
    snprintf(buff,20,"/tmp/output.%d", GpIdentity.dbid);
    logfile = fopen(buff, "w");

    mtrace();
    BootStrapThreadLock();

    if (MyBackendId == InvalidBackendId && GpIdentity.segindex == -1 && IsUnderMasterDispatchMode()) {
        BackgroundWorkerInitializeConnection(DB_FOR_COMMON_ACCESS, NULL);
    }
    if (MyBackendId == InvalidBackendId && GpIdentity.segindex > -1 && Gp_role == GP_ROLE_DISPATCH) {
        BackgroundWorkerInitializeConnection(DB_FOR_COMMON_ACCESS, NULL);
    }
    Assert(TopTransactionContext == NULL);
    TopTransactionContext =
            AllocSetContextCreate(TopMemoryContext,
                                  "TopTransactionContext",
                                  ALLOCSET_DEFAULT_MINSIZE,
                                  ALLOCSET_DEFAULT_INITSIZE,
                                  ALLOCSET_DEFAULT_MAXSIZE);
    TxnRWSetContext =
            AllocSetContextCreate(TopMemoryContext,
                                  "TxnRWSetContext",
                                  ALLOCSET_DEFAULT_MINSIZE,
                                  ALLOCSET_DEFAULT_INITSIZE,
                                  ALLOCSET_DEFAULT_MAXSIZE);
    CurTransactionContext = TopTransactionContext;
    CurrentResourceOwner = ResourceOwnerCreate(NULL, "KV storage");
    attachQueueControl->storage_pid = MyProcPid;
    register_signals();
    engine = create_engine(transaction_type);
    pool_init(MAX_STORAGE_THREADS);

    StorageInitWritebatchHash();
    StorageInitTransactionBatchHash();
    KeyXidCache_init();
    RtsCache_init();
    if (enable_range_distribution) {
        BootstrapInitRange();
        BootstrapInitStatistics();
    }
    if (GpIdentity.segindex >= 0) {
        initReplicadStorage(GpIdentity.segindex);
    }

    while (!got_sigterm) {
        WaitLatch(&MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT, (10L));
        ResetLatch(&MyProc->procLatch);
        check_all_req();
    }

    engine->destroy(engine);
    pool_destroy();
    proc_exit(0);
}

void register_signals() {
    /* Set up the signals before unblocking them. */
    // pqsignal(SIGTERM, storage_sigterm);
    pqsignal(SIGQUIT, storage_sigterm);
    /* We're now ready to receive signals. */
    BackgroundWorkerUnblockSignals();
}

#if (HANDLE_STORAGE == 1)
static KVEngineMQHandle
findHandleByPid(int pid) {
    ListCell *l;
    foreach (l, mq_handles) {
        KVEngineMQHandle handle = (KVEngineMQHandle) lfirst(l);
        if (handle->pid == pid)
            return handle;
    }
    return NULL;
}
#endif

void storage_attach_session(AttachRequestData attach_req) {
    shm_toc *toc = NULL;
#if (HANDLE_STORAGE == 1 || HANDLE_STORAGE == 3)
    KVEngineMQHandle handle = palloc0(sizeof(*handle));
    handle->pid = attach_req.session_pid;
    handle->node_head.type = T_MQ_HandlePlan;
#elif (HANDLE_STORAGE == 2)
    KVEngineMQHandle handle = &mq_handles[attach_req.session_pid];
#endif
    if (handle->status == KV_MQ_SUCCESS)
        return;
    Assert(handle->status == KV_MQ_NOT_ATTACHED);
    dsm_segment *seg;
    if (handle->seg && dsm_segment_handle(handle->seg) == attach_req.handle)
        seg = storage_mapping_dsm_with_toc(attach_req.handle, &toc);
    else
        seg = storage_attach_dsm_with_toc(attach_req.handle, &toc);
    if (seg) {
        handle->req_handle = storage_attach_req_mq(toc, seg);
        handle->res_handle = storage_attach_res_mq(toc, seg);
        handle->seg = seg;
        handle->isruning = false;
        handle->status = KV_MQ_SUCCESS;
#if (HANDLE_STORAGE == 1)
        mq_handles = lappend(mq_handles, handle);
#elif (HANDLE_STORAGE == 3)
        ThreadJob job = palloc0(sizeof(*job));
        job->handle = handle;
        job->pid = handle->pid;
        job->req = NULL;
        pool_add_worker(handle_process_kv_req, job);
#endif
        ResponseHeader *okres = palloc0(sizeof(*okres));
        okres->req_id = -2;
        okres->size = sizeof(*okres);
        okres->type = 0;
        shm_mq_send(handle->res_handle, okres->size, okres, false);
        pfree(okres);
    } else {
        handle->status = KV_MQ_NOT_ATTACHED;
        pfree(handle);
    }
}

dsm_segment *
storage_attach_dsm_with_toc(dsm_handle handle, shm_toc **toc) {
    dsm_segment *seg = dsm_attach(handle);
    if (seg)
        *toc = shm_toc_attach(PG_KV_ENGINE_MQ_MAGIC, dsm_segment_address(seg));
    return seg;
}

dsm_segment *
storage_mapping_dsm_with_toc(dsm_handle handle, shm_toc **toc) {
    dsm_segment *seg = dsm_find_mapping(handle);
    if (seg)
        *toc = shm_toc_attach(PG_KV_ENGINE_MQ_MAGIC, dsm_segment_address(seg));
    return seg;
}

shm_mq_handle *
storage_attach_req_mq(shm_toc *toc, dsm_segment *seg) {
    shm_mq *req_mq = shm_toc_lookup(toc, REQUEST_MQ_KEY);
    shm_mq_set_receiver(req_mq, MyProc);
    return shm_mq_attach(req_mq, seg, NULL);
}

shm_mq_handle *
storage_attach_res_mq(shm_toc *toc, dsm_segment *seg) {
    shm_mq *res_mq = shm_toc_lookup(toc, RESPONSE_MQ_KEY);
    shm_mq_set_sender(res_mq, MyProc);
    return shm_mq_attach(res_mq, seg, NULL);
}

static void
check_attach_req() {
    if (attachQueueControl->queue_len == 0)
        return;

    SpinLockAcquire(&attachQueueControl->amq_mutex);
    int length = attachQueueControl->queue_len, i;
    AttachRequestData *attach_reqs = palloc0(
            sizeof(AttachRequestData) * length);
    for (i = 0; i < length; ++i) {
        attach_reqs[i] = attachQueueControl->attach_queue[i];
    }
    attachQueueControl->queue_len = 0;
    SpinLockRelease(&attachQueueControl->amq_mutex);

    for (i = 0; i < length; ++i) {
#if (HANDLE_STORAGE != 3)
        if (attach_reqs[i].handle == 0)
            storage_detach(attach_reqs[i].session_pid);
        else
#endif
            storage_attach_session(attach_reqs[i]);
    }
}

void check_all_req() {
    check_attach_req();
#if (HANDLE_STORAGE != 3)
    check_kv_req();
#endif
}

#if (HANDLE_STORAGE == 1)
void check_kv_req() {
    ListCell *l;
    foreach (l, mq_handles) {
        KVEngineMQHandle handle = (KVEngineMQHandle) lfirst(l);
        if (handle->isruning == true)
            continue;
        ThreadJob job = check_and_receive_kv_req(handle);
        if (job != NULL) {
            handle->isruning = true;
            pool_add_worker(pre_handle_process_kv_req, job);
        }
    }
}

ThreadJob
check_and_receive_kv_req(KVEngineMQHandle handle) {
    if (handle->status == KV_MQ_NOT_ATTACHED)
        return NULL;

    Size len = 0;
    RequestHeader *req = NULL;
    shm_mq_result r = shm_mq_receive(handle->req_handle, &len, (void **) &req, true);
    if (r != SHM_MQ_SUCCESS)
        return NULL;

    ThreadJob job = palloc0(sizeof(*job));
    job->pid = handle->pid;
    job->req = palloc0(req->size);
    memcpy(job->req, req, req->size);
    return job;
}
#elif (HANDLE_STORAGE == 2)
void check_kv_req() {
    for (int i = 0; i < MAX_PID; i++) {
        ThreadJob job = check_and_receive_kv_req(i);
        if (job != NULL) {
            pool_add_worker(handle_process_kv_req, job);
        }
    }
}

ThreadJob
check_and_receive_kv_req(int pid) {
    Size len = 0;
    RequestHeader *req = NULL;
    if (mq_handles[pid].status == KV_MQ_NOT_ATTACHED)
        return NULL;
    shm_mq_result result = shm_mq_receive(mq_handles[pid].req_handle, &len, (void **) &req, true);
    if (result != SHM_MQ_SUCCESS)
        return NULL;
    ThreadJob job = palloc0(sizeof(*job));
    job->pid = pid;
    job->req = req;
    return job;
}
#endif

#if (HANDLE_STORAGE == 1)
void *
handle_process_kv_req(void *job) {
    PoolThreadJob *t = (PoolThreadJob *) job;
    int pid = t->pid;
    int isLeader = t->isLeader;
    RequestHeader *req = t->req;
    KVEngineMQHandle handle = NULL;
    ListCell *l;
#ifdef PRINT_PAXOS_MSG
    fprintf(logfile,"real %d %d %d\n", t->req->type, pid, isLeader);
    fflush(logfile);
#endif
    if (isLeader) {
        foreach (l, mq_handles) {
            handle = (KVEngineMQHandle) lfirst(l);
            if (handle->pid == pid)
                break;
        }
    }

    Assert(CurrentThreadMemoryContext != NULL);
    /*
	 * Apply for a new memorycontext, for each request to prevent operational
	 * memory leaks.
	 */
    MemoryContext ThreadWorkMemoryContext =
            AllocSetContextCreate(CurrentThreadMemoryContext,
                                  "Storage Work Thread Context",
                                  ALLOCSET_DEFAULT_MINSIZE,
                                  ALLOCSET_DEFAULT_INITSIZE,
                                  ALLOCSET_DEFAULT_MAXSIZE);
    MemoryContext oldThreadContext = CurrentThreadMemoryContext;
    CurrentThreadMemoryContext = ThreadWorkMemoryContext;

    if (enable_range_distribution) {
        KVEngineTransactionInterface * txn = engine->create_txn(engine, req->gxid, true, req->start_ts);
        RouteCheckScanDesc = init_kv_scan(true, txn);
    }

    /* It's data from shared memory. It can't be released. */
    ResponseHeader *res = process_kv_request(req);
    if (isLeader) {
        res->req_id = req->req_id;
        shm_mq_result result = SHM_MQ_SUCCESS;
#if (HANDLE_STORAGE == 1)
        if (handle->res_handle)
            result = shm_mq_send(handle->res_handle, res->size, res, false);
#elif (HANDLE_STORAGE == 2)
        if (handle->res_handle)
            result = shm_mq_send(mq_handles[pid].res_handle, res->size, res, false);
#endif
        Assert(result == SHM_MQ_SUCCESS);
        handle->isruning = false;
    }

    range_free(res);

    if (enable_range_distribution) {
        free_kv_desc(RouteCheckScanDesc);
    }

    CurrentThreadMemoryContext = oldThreadContext;
    if (ThreadWorkMemoryContext != NULL) {
        MemoryContextDelete(ThreadWorkMemoryContext);
        ThreadWorkMemoryContext = NULL;
    }
    return NULL;
}

static char * type2char(int type) 
{
    switch(type)
    {
        case ROCKSDB_PUT: return "ROCKSDB_PUT";
        case ROCKSDB_PUTRTS: return "ROCKSDB_PUTRTS";
        case ROCKSDB_GET: return "ROCKSDB_GET";
        case ROCKSDB_SCAN: return "ROCKSDB_SCAN";
        case ROCKSDB_CYCLEGET: return "ROCKSDB_CYCLEGET";
        case ROCKSDB_DELETE: return "ROCKSDB_DELETE";
        case ROCKSDB_UPDATE: return "ROCKSDB_UPDATE";
        case ROCKSDB_DELETE_DIRECT: return "ROCKSDB_DELETE_DIRECT";
        case ROCKSDB_RANGESCAN: return "ROCKSDB_RANGESCAN";
        case ROCKSDB_PREPARE: return "ROCKSDB_PREPARE";
        case ROCKSDB_COMMIT: return "ROCKSDB_COMMIT";
        case ROCKSDB_ABORT: return "ROCKSDB_ABORT";
        case ROCKSDB_CLEAR: return "ROCKSDB_CLEAR";
        case ROCKSDB_DETACH: return "ROCKSDB_DETACH";
        case STATISTIC_READ: return "STATISTIC_READ";
        case STATISTIC_WRITE: return "STATISTIC_WRITE";
        case GO_ON: return "GO_ON";
        case RANGE_SCAN: return "RANGE_SCAN";
        case FIND_MIDDLE_KEY: return "FIND_MIDDLE_KEY";
        case INIT_STATISTICS: return "INIT_STATISTICS";
        case RANGE_CREATE: return "RANGE_CREATE";
        case ADDREPLICA: return "ADDREPLICA";
        case REMOVEREPLICA: return "REMOVEREPLICA";
        case REMOVEGROUP: return "REMOVEGROUP";
        case ADDGROUPMEMBER: return "ADDGROUPMEMBER";
        case REMOVEGROUPMEMBER: return "REMOVEGROUPMEMBER";
        default: return "ERROR";
    }
}

static void type_log_print(RequestHeader *req) 
{
    switch(req->type)
    {
        case ROCKSDB_PUT: 
        {
            PutRequest *put_req = (PutRequest *)req;
            TupleKeySlice key = pick_tuple_key_from_buffer(put_req->k_v);
            fprintf(logfile, " keysize:%ld keytype:%c keyrelid:%u keyindex:%u", key.len, key.data->type, key.data->rel_id, key.data->indexOid);
            return ;
        }
        case ROCKSDB_GET: return ;
        case ROCKSDB_SCAN: return ;
        case ROCKSDB_CYCLEGET: return ;
        case ROCKSDB_DELETE: return ;
        case ROCKSDB_UPDATE: return ;
        case ROCKSDB_DELETE_DIRECT: return ;
        case ROCKSDB_RANGESCAN: return ;
        case ROCKSDB_PREPARE: return ;
        case ROCKSDB_COMMIT: return;
        case ROCKSDB_ABORT: return ;
        case ROCKSDB_CLEAR: return ;
        case ROCKSDB_DETACH: return ;
        case STATISTIC_READ: return ;
        case STATISTIC_WRITE: return ;
        case GO_ON: return ;
        case RANGE_SCAN: return ;
        case FIND_MIDDLE_KEY: return ;
        case INIT_STATISTICS: return ;
        case RANGE_CREATE: return ;
        case ADDREPLICA: return ;
        case REMOVEREPLICA: return ;
        case REMOVEGROUP: return ;
        case ADDGROUPMEMBER: return ;
        case REMOVEGROUPMEMBER: return ;
        default: return ;
    }
}

static void print_log_file(ThreadJob j)
{
    if (GpIdentity.segindex >= 0 && j->req->need_paxos && !(j->req->type == ROCKSDB_GET || j->req->type == ROCKSDB_SCAN || j->req->type == ROCKSDB_CYCLEGET || j->req->type == ROCKSDB_PREPARE || j->req->type == ROCKSDB_COMMIT || j->req->type == ROCKSDB_ABORT)) {
        fprintf(logfile, "pre type:%s session_id:%d needpaxos:%d pa", type2char(j->req->type), j->pid, j->req->need_paxos);
        type_log_print(j->req);
        fprintf(logfile, "\n");
        fflush(logfile);
    } else {
        fprintf(logfile, "pre type:%s session_id:%d needpaxos:%d np", type2char(j->req->type), j->pid, j->req->need_paxos);
        type_log_print(j->req);
        fprintf(logfile, "\n");
        fflush(logfile);
    }
}

void *
pre_handle_process_kv_req(void *job) {
    ThreadJob j = (ThreadJob) job;
#ifdef PRINT_PAXOS_MSG
    print_log_file(j);
#endif
    if (enable_paxos && GpIdentity.segindex >= 0 && j->req->need_paxos && 
        !(j->req->type == ROCKSDB_GET || j->req->type == ROCKSDB_SCAN || 
          j->req->type == ROCKSDB_CYCLEGET || j->req->type == ROCKSDB_PREPARE || 
          j->req->type == ROCKSDB_COMMIT || j->req->type == ROCKSDB_ABORT)) {
        echo(j->pid, (const char *) (j->req), j->req->size);
    } else {
        PoolThreadJob *tmp = packjob(j->pid, (const char *) j->req, j->req->size, 1);
        handle_process_kv_req(tmp);
    }
    return NULL;
}

#elif (HANDLE_STORAGE == 3)
void *
handle_process_kv_req(void *arg) {
    ThreadJob job = (ThreadJob) arg;
    int pid = job->pid;
    KVEngineMQHandle handle = job->handle;

    while (true) {
        Assert(CurrentThreadMemoryContext != NULL);
        MemoryContext ThreadWorkMemoryContext =
                AllocSetContextCreate(CurrentThreadMemoryContext,
                                      "Storage Work Thread Context",
                                      ALLOCSET_DEFAULT_MINSIZE,
                                      ALLOCSET_DEFAULT_INITSIZE,
                                      ALLOCSET_DEFAULT_MAXSIZE);
        MemoryContext oldThreadContext = CurrentThreadMemoryContext;
        CurrentThreadMemoryContext = ThreadWorkMemoryContext;

        Size len = 0;
        RequestHeader *req = NULL;
        shm_mq_result result = shm_mq_receive(handle->req_handle, &len, (void **) &req, false);

        if (enable_range_distribution)
            RouteCheckScanDesc = init_kv_scan(true);
        if (req->type == ROCKSDB_DETACH) {
            while (handle->status == KV_MQ_NOT_ATTACHED)
                ;
            Assert(handle->status != KV_MQ_NOT_ATTACHED);
            if (handle->status == KV_MQ_SUCCESS)
                dsm_detach(handle->seg);
            handle->req_handle = NULL;
            handle->res_handle = NULL;
            handle->status = KV_MQ_NOT_ATTACHED;
            range_free(handle);
            break;
        }
        ResponseHeader *res = process_kv_request((RequestHeader *) req);
        res->req_id = req->req_id;

        result = shm_mq_send(handle->res_handle, res->size, res, false);
        Assert(result == SHM_MQ_SUCCESS);
        range_free(res);
        if (enable_range_distribution)
            free_kv_desc(RouteCheckScanDesc);
        CurrentThreadMemoryContext = oldThreadContext;
        if (ThreadWorkMemoryContext != NULL) {
            MemoryContextDelete(ThreadWorkMemoryContext);
            ThreadWorkMemoryContext = NULL;
        }
    }
    freeJob(job);
    return NULL;
}
#endif

/*
 * The storage layer accepts the upper layer data and processes it.
 */
ResponseHeader *
process_kv_request(RequestHeader *req) {
    StorageUpdateTransactionState(req);
    switch (req->type) {
        case ROCKSDB_GET:
            return kvengine_process_get_req(req);
        case ROCKSDB_PUT:
            return kvengine_process_put_req(req);
        case ROCKSDB_PUTRTS:
            return kvengine_process_put_rts_req(req);
        case ROCKSDB_DELETE_DIRECT:
            return kvengine_process_delete_direct_req(req);
        case ROCKSDB_RANGESCAN:
        case ROCKSDB_SCAN:
            return kvengine_process_scan_req(req);
        case FIND_MIDDLE_KEY:
            return kvengine_process_rangescan_req(req);
        case ROCKSDB_CYCLEGET:
            return kvengine_process_multi_get_req(req);
        case ROCKSDB_DELETE:
            return kvengine_process_delete_normal_req(req);
        case ROCKSDB_UPDATE:
            return kvengine_process_update_req(req);
        case INIT_STATISTICS:
            return kvengine_process_init_statistics_req(req);
        case ROCKSDB_PREPARE:
            return kvengine_process_prepare(req);
        case ROCKSDB_COMMIT:
            return kvengine_process_commit(req);
        case ROCKSDB_ABORT:
            return kvengine_process_abort(req);
        case ROCKSDB_CLEAR:
            return kvengine_process_clear(req);
        case ROCKSDB_DETACH:
            return kvengine_process_detach(req);
        default: {
            if (!enable_req_failed_retry)
                ereport(ERROR,
                        (errmsg("Storage: request type %d error. req_id = %d",
                                req->type, req->req_id)));
            ResponseHeader *erres = palloc0(sizeof(*erres));
            erres->req_id = -1;
            erres->size = sizeof(*erres);
            erres->type = 0;
            return erres;
        }
    }
}

#if (HANDLE_STORAGE == 1)
void storage_detach(int pid) {
    KVEngineMQHandle handle = findHandleByPid(pid);
    while (handle->status == KV_MQ_NOT_ATTACHED)
        ;

    Assert(handle->status != KV_MQ_NOT_ATTACHED);
    if (handle->status == KV_MQ_SUCCESS) {
        dsm_detach(handle->seg);
    }
    //handle->seg = NULL;
    handle->req_handle = NULL;
    handle->res_handle = NULL;
    handle->status = KV_MQ_NOT_ATTACHED;
    //handle->pid = 0;
    mq_handles = list_delete(mq_handles, handle);
    range_free(handle);
}
#elif (HANDLE_STORAGE == 2)
storage_detach(int pid) {
    wait_for_attach_finished(pid);
    KVEngineMQHandle handle = &mq_handles[pid];

    Assert(handle->status != KV_MQ_NOT_ATTACHED);
    if (handle->status == KV_MQ_SUCCESS)
        dsm_detach(handle->seg);
    //handle->seg = NULL;
    handle->req_handle = NULL;
    handle->res_handle = NULL;
    handle->status = KV_MQ_NOT_ATTACHED;
}
#endif
