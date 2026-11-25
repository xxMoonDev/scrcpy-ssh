# scrcpy-ssh: changes from scrcpy v5.0.1

This repository is a modified version of [scrcpy](https://github.com/Genymobile/scrcpy)
(Copyright (C) 2018 Genymobile, Copyright (C) 2018-2026 Romain Vimont, Apache License 2.0).
As the Apache License 2.0 requires, the files changed for **scrcpy-ssh** are listed here.
The changes are Copyright 2026 <YOUR NAME>, under the same Apache License 2.0.

## New files
| File | Purpose |
|---|---|
| `app/src/ssh/ssh.h`, `app/src/ssh/ssh.c` | `--ssh` target parsing, running `ssh` commands, push/install over SSH, connection check |
| `app/src/ssh/ssh_tunnel.h`, `app/src/ssh/ssh_tunnel.c` | `adb forward` replacement: local listener + one `ssh … ScrcpySshRelay` per connection |
| `ssh-relay/ScrcpySshRelay.java`, `ssh-relay/scrcpy-ssh-relay.jar`, `ssh-relay/build_relay.sh` | Device-side relay between the SSH channel and the server's abstract socket |
| `doc/ssh.md`, `SSH_FORK_CHANGES.md` | Documentation |

## Modified files
| File | Change |
|---|---|
| `app/meson.build` | Builds the `ssh/` sources |
| `app/src/cli.c` | `--ssh [user@]host[:port]` option and its incompatibility checks |
| `app/src/options.h`, `app/src/options.c` | The `ssh` option field |
| `app/src/scrcpy.c` | Passes the SSH target to the server |
| `app/src/server.h`, `app/src/server.c` | Push, start and tunnel the server over SSH when `--ssh` is used |
| `app/src/file_pusher.c` | File drop / APK install over SSH |
| `app/src/util/process.h`, `app/src/util/process.c` | `sc_pipe_write` and helpers to feed a child process's stdin (used to stream files into `ssh`) |
| `app/src/sys/unix/process.c`, `app/src/sys/win/process.c` | `sc_pipe_write`; on Windows also: inherit all piped handles and pass a complete std handle list (needed by Windows OpenSSH) |
| `README.md` | Fork notice at the top |
