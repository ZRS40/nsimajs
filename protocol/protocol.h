#ifndef NSIMAJ_PROTOCOL_H
#define NSIMAJ_PROTOCOL_H

#ifndef NSIMAJ_VERSION
#define NSIMAJ_VERSION "0.1.0"
#endif

#define NSIMAJ_CONTROL_PORT 7422
#define NSIMAJ_HTTP_PORT    7421

#define NSIMAJ_MAX_FRAME    (256 * 1024)
#define NSIMAJ_MAX_AGENTS   128
#define NSIMAJ_ID_LEN       128
#define NSIMAJ_HOST_LEN     128
#define NSIMAJ_IP_LEN       64
#define NSIMAJ_DISTRO_LEN   64
#define NSIMAJ_STAGE_LEN    64
#define NSIMAJ_SECRET_LEN   128
#define NSIMAJ_PATH_LEN     512
#define NSIMAJ_CMD_LEN      4096

#define NSIMAJ_HEARTBEAT_SEC 2
#define NSIMAJ_OFFLINE_SEC   6

/* Message type strings */
#define MSG_HELLO        "hello"
#define MSG_HEARTBEAT    "heartbeat"
#define MSG_JOB_START    "job_start"
#define MSG_JOB_CANCEL   "job_cancel"
#define MSG_JOB_EVENT    "job_event"
#define MSG_FOCUS_START  "focus_start"
#define MSG_FOCUS_DATA   "focus_data"
#define MSG_FOCUS_STDIN  "focus_stdin"
#define MSG_FOCUS_RESIZE "focus_resize"
#define MSG_FOCUS_DETACH "focus_detach"
#define MSG_FOCUS_END    "focus_end"
#define MSG_ERROR        "error"
#define MSG_OK           "ok"

#define JOB_KIND_UPDATE "update"
#define JOB_KIND_EXEC   "exec"

#define STAGE_QUEUED            "queued"
#define STAGE_PREPARING         "preparing"
#define STAGE_UPDATING_INDEXES  "updating_indexes"
#define STAGE_UPGRADING         "upgrading"
#define STAGE_RUNNING           "running"
#define STAGE_DONE              "done"
#define STAGE_FAILED            "failed"
#define STAGE_CANCELLED         "cancelled"

#endif
