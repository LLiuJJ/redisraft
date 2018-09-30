#ifndef _REDISRAFT_H
#define _REDISRAFT_H

#include <stdint.h>
#include <stdbool.h>
#include <bsd/sys/queue.h>
#include <stdio.h>
#include <unistd.h>

#define REDISMODULE_EXPERIMENTAL_API
#include "uv.h"
#include "hiredis/hiredis.h"
#include "hiredis/async.h"
#include "redismodule.h"
#include "raft.h"

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_RAFT_DATATYPE_NAME     "redisraft"
#define REDIS_RAFT_DATATYPE_ENCVER   1

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_WARNING   "warning"
#define REDIS_NOTICE    "notice"
#define REDIS_VERBOSE   "verbose"

/* -------------------- Logging macros -------------------- */

/*
 * We use our own logging mechanism because most log output is generated by
 * the Raft thread which cannot use Redis logging.
 *
 * TODO Migrate to RedisModule_Log when it's capable of logging using a
 * Thread Safe context.
 */

extern int redis_raft_loglevel;
extern FILE *redis_raft_logfile;

void raft_module_log(const char *fmt, ...);

#define LOGLEVEL_ERROR           0
#define LOGLEVEL_INFO            1
#define LOGLEVEL_VERBOSE         2
#define LOGLEVEL_DEBUG           3

#define LOG(level, fmt, ...) \
    do { if (redis_raft_loglevel >= level) \
            raft_module_log(fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(fmt, ...) LOG(LOGLEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LOG(LOGLEVEL_VERBOSE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG(LOGLEVEL_DEBUG, fmt, ##__VA_ARGS__)

#define PANIC(fmt, ...) \
    do {  LOG_ERROR("\n\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n" \
                    "REDIS RAFT PANIC\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" \
                    fmt, ##__VA_ARGS__); exit(1); } while (0)

#define TRACE(fmt, ...) LOG(LOGLEVEL_DEBUG, "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

#define NODE_LOG(level, node, fmt, ...) \
    LOG(level, "node:%d: " fmt, (node)->id, ##__VA_ARGS__)

#define NODE_LOG_ERROR(node, fmt, ...) NODE_LOG(LOGLEVEL_ERROR, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_INFO(node, fmt, ...) NODE_LOG(LOGLEVEL_INFO, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_VERBOSE(node, fmt, ...) NODE_LOG(LOGLEVEL_VERBOSE, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_DEBUG(node, fmt, ...) NODE_LOG(LOGLEVEL_DEBUG, node, fmt, ##__VA_ARGS__)

/* Forward declarations */
struct RaftReq;
struct RedisRaftConfig;
struct Node;

/* Node address specifier. */
typedef struct node_addr {
    uint16_t port;
    char host[256];             /* Hostname or IP address */
} NodeAddr;

typedef struct NodeAddrListElement {
    NodeAddr addr;
    struct NodeAddrListElement *next;
} NodeAddrListElement;

typedef enum RedisRaftState {
    REDIS_RAFT_UP,
    REDIS_RAFT_LOADING,
    REDIS_RAFT_JOINING
} RedisRaftState;

typedef struct SnapshotCfgEntry {
    raft_node_id_t  id;
    int             active;
    int             voting;
    NodeAddr        addr;
    struct SnapshotCfgEntry *next;
} SnapshotCfgEntry;

#define RAFT_DBID_LEN   32

typedef struct RaftSnapshotInfo {
    bool loaded;
    char dbid[RAFT_DBID_LEN+1];
    raft_term_t last_applied_term;
    raft_index_t last_applied_idx;
    SnapshotCfgEntry *cfg;
} RaftSnapshotInfo;

typedef struct {
    void *raft;                 /* Raft library context */
    RedisModuleCtx *ctx;        /* Redis module thread-safe context; only used to push commands
                                   we get from the leader. */
    RedisRaftState state;       /* Raft module state */
    uv_thread_t thread;         /* Raft I/O thread */
    uv_loop_t *loop;            /* Raft I/O loop */
    uv_async_t rqueue_sig;      /* A signal we have something on rqueue */
    uv_timer_t raft_periodic_timer;     /* Invoke Raft periodic func */
    uv_timer_t node_reconnect_timer;    /* Handle connection issues */
    uv_mutex_t rqueue_mutex;    /* Mutex protecting rqueue access */
    STAILQ_HEAD(rqueue, RaftReq) rqueue;     /* Requests queue (from Redis) */
    struct RaftLog *log;
    struct RedisRaftConfig *config;
    NodeAddrListElement *join_addr;
    NodeAddrListElement *join_addr_iter;
    struct Node *join_node;
    bool snapshot_in_progress;
    bool loading_snapshot;
    raft_index_t snapshot_rewrite_last_idx;
    struct RaftReq *compact_req;
    bool callbacks_set;
    int snapshot_child_fd;
    /* Tracking of applied entries */
    RaftSnapshotInfo snapshot_info;
} RedisRaftCtx;

#define REDIS_RAFT_DEFAULT_RAFTLOG  "redisraft.db"

#define REDIS_RAFT_DEFAULT_INTERVAL                 100
#define REDIS_RAFT_DEFAULT_REQUEST_TIMEOUT          250
#define REDIS_RAFT_DEFAULT_ELECTION_TIMEOUT         500
#define REDIS_RAFT_DEFAULT_RECONNECT_INTERVAL       100
#define REDIS_RAFT_DEFAULT_MAX_LOG_ENTRIES          10000

typedef struct RedisRaftConfig {
    raft_node_id_t id;          /* Local node Id */
    NodeAddr addr;              /* Address of local node, if specified */
    NodeAddrListElement *join;
    char *rdb_filename;         /* Original Redis dbfilename */
    char *raftlog;              /* Raft log file name, derived from dbfilename */
    char *snapshot_filename;    /* Name used when creating a snapshot */
    bool persist;               /* Should log be persisted */
    /* Tuning */
    int raft_interval;
    int request_timeout;
    int election_timeout;
    int reconnect_interval;
    int max_log_entries;
    /* Flags */
    bool init;
    /* Debug options */
    int compact_delay;
} RedisRaftConfig;

typedef void (*NodeConnectCallbackFunc)(const redisAsyncContext *, int);

typedef enum NodeState {
    NODE_DISCONNECTED,
    NODE_RESOLVING,
    NODE_CONNECTING,
    NODE_CONNECTED,
    NODE_CONNECT_ERROR
} NodeState;

typedef enum NodeFlags {
    NODE_TERMINATING    = 1 << 0
} NodeFlags;

#define NODE_STATE_IDLE(x) \
    ((x) == NODE_DISCONNECTED || \
     (x) == NODE_CONNECT_ERROR)

typedef struct Node {
    raft_node_id_t id;
    NodeState state;
    NodeFlags flags;
    NodeAddr addr;
    redisAsyncContext *rc;
    uv_getaddrinfo_t uv_resolver;
    RedisRaftCtx *rr;
    NodeConnectCallbackFunc connect_callback;
    bool load_snapshot_in_progress;
    bool unlinked;
    raft_index_t load_snapshot_idx;
    time_t load_snapshot_last_time;
    LIST_ENTRY(Node) entries;
} Node;

typedef void (*RaftReqHandler)(RedisRaftCtx *, struct RaftReq *);

typedef enum RedisRaftResult {
    RR_OK       = 0,
    RR_ERROR
} RedisRaftResult;

enum RaftReqType {
    RR_CFGCHANGE_ADDNODE = 1,
    RR_CFGCHANGE_REMOVENODE,
    RR_APPENDENTRIES,
    RR_REQUESTVOTE,
    RR_REDISCOMMAND,
    RR_INFO,
    RR_LOADSNAPSHOT,
    RR_COMPACT
};

typedef struct {
    raft_node_id_t id;
    NodeAddr addr;
} RaftCfgChange;

typedef struct {
    int argc;
    RedisModuleString **argv;
} RaftRedisCommand;

typedef struct RaftReq {
    int type;
    STAILQ_ENTRY(RaftReq) entries;
    RedisModuleBlockedClient *client;
    RedisModuleCtx *ctx;
    union {
        RaftCfgChange cfgchange;
        struct {
            raft_node_id_t src_node_id;
            msg_appendentries_t msg;
        } appendentries;
        struct {
            raft_node_id_t src_node_id;
            msg_requestvote_t msg;
        } requestvote;
        struct {
            RaftRedisCommand cmd;
            msg_entry_response_t response;
        } redis;
        struct {
            raft_term_t term;
            raft_index_t idx;
            RedisModuleString *snapshot;
        } loadsnapshot;
    } r;
} RaftReq;

typedef struct RaftLog {
    uint32_t            version;
    char                dbid[RAFT_DBID_LEN+1];
    unsigned long int   num_entries;
    raft_term_t         snapshot_last_term;
    raft_index_t        snapshot_last_idx;
    raft_node_id_t      vote;
    raft_term_t         term;
    raft_index_t        index;
    FILE                *file;
} RaftLog;

#define RAFTLOG_VERSION     1

#define SNAPSHOT_RESULT_MAGIC    0x70616e73  /* "snap" */
typedef struct SnapshotResult {
    int magic;
    int success;
    long long int num_entries;
    char rdb_filename[256];
    char log_filename[256];
    char err[256];
} SnapshotResult;


/* node.c */
void NodeFree(Node *node);
void NodeUnlink(Node *node);
Node *NodeInit(int id, const NodeAddr *addr);
bool NodeConnect(Node *node, RedisRaftCtx *rr, NodeConnectCallbackFunc connect_callback);
bool NodeAddrParse(const char *node_addr, size_t node_addr_len, NodeAddr *result);
void NodeAddrListAddElement(NodeAddrListElement *head, NodeAddr *addr);
void HandleNodeStates(RedisRaftCtx *rr);

/* raft.c */
const char *getStateStr(RedisRaftCtx *rr);
void RaftRedisCommandSerialize(raft_entry_data_t *target, RaftRedisCommand *source);
bool RaftRedisCommandDeserialize(RedisModuleCtx *ctx, RaftRedisCommand *target, raft_entry_data_t *source);
void RaftRedisCommandFree(RedisModuleCtx *ctx, RaftRedisCommand *r);
int RedisRaftInit(RedisModuleCtx *ctx, RedisRaftCtx *rr, RedisRaftConfig *config);
int RedisRaftStart(RedisModuleCtx *ctx, RedisRaftCtx *rr);

void RaftReqFree(RaftReq *req);
RaftReq *RaftReqInit(RedisModuleCtx *ctx, enum RaftReqType type);
void RaftReqSubmit(RedisRaftCtx *rr, RaftReq *req);
void RaftReqHandleQueue(uv_async_t *handle);

/* util.c */
int RedisModuleStringToInt(RedisModuleString *str, int *value);
char *catsnprintf(char *strbuf, size_t *strbuf_len, const char *fmt, ...);
int stringmatchlen(const char *pattern, int patternLen, const char *string, int stringLen, int nocase);
int stringmatch(const char *pattern, const char *string, int nocase);
int RedisInfoIterate(const char **info_ptr, size_t *info_len, const char **key, size_t *keylen, const char **value, size_t *valuelen);
char *RedisInfoGetParam(RedisRaftCtx *rr, const char *section, const char *param);

/* log.c */
typedef enum LogEntryAction {
    LA_APPEND,
    LA_REMOVE_HEAD,
    LA_REMOVE_TAIL
} LogEntryAction;

RaftLog *RaftLogCreate(const char *filename, const char *dbid, raft_term_t term, raft_index_t index);
RaftLog *RaftLogOpen(const char *filename);
void RaftLogClose(RaftLog *log);
bool RaftLogAppend(RaftLog *log, raft_entry_t *entry);
bool RaftLogSetVote(RaftLog *log, raft_node_id_t vote);
bool RaftLogSetTerm(RaftLog *log, raft_term_t term, raft_node_id_t vote);
int RaftLogLoadEntries(RaftLog *log, int (*callback)(void *, LogEntryAction action, raft_entry_t *), void *callback_arg);
bool RaftLogRemoveHead(RaftLog *log);
bool RaftLogRemoveTail(RaftLog *log);
bool RaftLogWriteEntry(RaftLog *log, raft_entry_t *entry);
bool RaftLogSync(RaftLog *log);

/* config.c */
int ConfigInit(RedisModuleCtx *ctx, RedisRaftConfig *config);
int ConfigParseArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, RedisRaftConfig *target);
int ConfigValidate(RedisModuleCtx *ctx, RedisRaftConfig *config);
int handleConfigSet(RedisModuleCtx *ctx, RedisRaftConfig *config, RedisModuleString **argv, int argc);
int handleConfigGet(RedisModuleCtx *ctx, RedisRaftConfig *config, RedisModuleString **argv, int argc);
int ConfigSetupFilenames(RedisRaftCtx *rr);
int ValidateRedisConfig(RedisRaftCtx *rr, RedisModuleCtx *ctx);

/* snapshot.c */
extern RedisModuleTypeMethods RedisRaftTypeMethods;
extern RedisModuleType *RedisRaftType;
void initializeSnapshotInfo(RedisRaftCtx *rr);
void handleLoadSnapshot(RedisRaftCtx *rr, RaftReq *req);
void checkLoadSnapshotProgress(RedisRaftCtx *rr);
RedisRaftResult initiateSnapshot(RedisRaftCtx *rr);
RedisRaftResult finalizeSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void cancelSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void handleCompact(RedisRaftCtx *rr, RaftReq *req);
int pollSnapshotStatus(RedisRaftCtx *rr, SnapshotResult *sr);
void configRaftFromSnapshotInfo(RedisRaftCtx *rr);
int raftSendSnapshot(raft_server_t *raft, void *user_data, raft_node_t *raft_node);

#endif  /* _REDISRAFT_H */
