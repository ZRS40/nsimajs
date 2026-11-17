# nsimaj wire protocol

TCP port **7422** (default). Framing: `uint32` big-endian length + UTF-8 JSON body.

## Agent → server

- `hello` — `{type,secret,id,hostname,ip,distro,family,agent_version,pending_updates}`
- `heartbeat` — `{type,id,distro,pending_updates}`
- `job_event` — `{type,job_id,stage,exit_code?,error?}`
- `focus_data` — `{type,data}` (`data` is base64 PTY output)
- `focus_end` — `{type}`

## Server → agent

- `ok` / `error`
- `job_start` — `{type,job_id,kind,payload?}` (`kind`: `update` | `exec`)
- `job_cancel` — `{type,job_id}`
- `focus_start` — `{type,job_id}`
- `focus_stdin` — `{type,data}` (base64)
- `focus_resize` — `{type,rows,cols}`
- `focus_detach` — `{type}`

Stages: `queued`, `preparing`, `updating_indexes`, `upgrading`, `running`, `done`, `failed`, `cancelled`.
