# SSH transport (scrcpy-ssh)

**scrcpy-ssh** is a fork of [scrcpy](https://github.com/Genymobile/scrcpy) v5.0.1 that can reach the device
over **SSH instead of adb**. Mirroring and control work over any network the SSH server is reachable on:
Wi-Fi, a VPN, or a USB port forward. ADB doesn't need to be enabled on the phone.

```
scrcpy --ssh root@192.168.1.50:2022
scrcpy --ssh phone-alias                # a Host from ~/.ssh/config
```

Everything else (video, audio, control, clipboard, file drop, recording, all options) is plain scrcpy.

## Requirements
- **On the phone:** an SSH server whose sessions can run `app_process` and write `/data/local/tmp`. In
  practice that's a root or ADB-shell-user session:
  - [AndroOpenHeart](https://github.com/<your-github-user>/AndroOpenHeart): root through `su`, or the shell user
    through Shizuku.
  - Any other server that runs commands as root (Magisk SSH modules, etc.).
  - A user-level server such as Termux `sshd` won't work. It can't start the scrcpy server.
- **Non-interactive login.** Several `ssh` processes are started per session, so they must log in without
  prompts: use a key or `ssh-agent`, and accept the host key once with a normal `ssh` first.
- **On the PC:** an OpenSSH client (`ssh`). On Windows the built-in `C:\Windows\System32\OpenSSH\ssh.exe` works.

## Options and environment
| | |
|---|---|
| `--ssh [user@]host[:port]` | Connect over SSH. Incompatible with `-s`, `-d`, `-e`, `--tcpip`, `--tunnel-host` and `--tunnel-port` |
| `SSH` (environment) | The `ssh` executable to use (default: `ssh` on PATH) |
| `SCRCPY_SSH_ARGS` (environment) | Extra ssh arguments, **one per line**, e.g. `-i`, `C:\keys\id_ed25519`, `-o`, `UserKnownHostsFile=C:\…\known_hosts`. AndroUnderTheHood uses this to pass its saved key and host keys |

scrcpy-ssh always adds `-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -T` (no pty, so
the pipes are binary-safe). IPv6 literals aren't supported in the short `host:port` form; use a `Host` alias in
`~/.ssh/config`.

## How it works
Standard scrcpy uses adb for three things: pushing `scrcpy-server`, starting it with `app_process`, and
`adb forward` to reach the server's abstract Unix socket. scrcpy-ssh replaces each with SSH:

| adb | scrcpy-ssh |
|---|---|
| `adb push scrcpy-server /data/local/tmp/` | `ssh … 'cat > /data/local/tmp/scrcpy-server.jar'` (stdin = the file) |
| `adb shell CLASSPATH=… app_process / com.genymobile.scrcpy.Server …` | `ssh … 'CLASSPATH=… app_process / …'` |
| `adb forward tcp:PORT localabstract:scrcpy_XXXX` | an **SSH tunnel** (below) |
| `adb install` / `adb push` (file drop) | `ssh … 'cat > …'` and `pm install` |

**The tunnel** (`app/src/ssh/ssh_tunnel.c`): the client listens on `localhost:PORT`. For every connection
scrcpy opens (video, audio, control), it runs:

```
ssh <dest> 'CLASSPATH=/data/local/tmp/scrcpy-ssh-relay.jar app_process / ScrcpySshRelay scrcpy_XXXX'
```

`ScrcpySshRelay` (in `ssh-relay/`, pushed next to the server) connects to the server's abstract socket. It
writes one `K` byte once connected, then copies bytes both ways between the socket and the SSH channel.
Connections are relayed one at a time, so the server accepts video, audio and control in the same order the
client opened them. If the socket isn't listening yet, the relay exits with status 1 and the client retries.

### Windows process launching
SSH clients need valid standard handles. The fork fixes two details in `app/src/sys/win/process.c`:
- All piped handles are inherited.
- The handle list in `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` is always complete.

Without this, Windows' OpenSSH exits with code 255 when stdout/stderr aren't valid. Git for Windows' `ssh`
tolerated it, so test with `C:\Windows\System32\OpenSSH\ssh.exe`.

## Building
Build exactly like upstream scrcpy (see [BUILD.md](BUILD.md)). On Linux, the Windows release can be
cross-compiled with the release scripts:
```
./release/build_windows.sh 64      # → release/work/build-win64
```
The relay helper is plain Java run with `app_process`. A prebuilt `ssh-relay/scrcpy-ssh-relay.jar` is in the
repository. Rebuild it with:
```
ANDROID_HOME=/path/to/Sdk ./ssh-relay/build_relay.sh
```
Put `scrcpy-ssh-relay.jar` next to `scrcpy-server` (and `scrcpy.exe`) in the release folder.

## Files changed from scrcpy v5.0.1
See [SSH_FORK_CHANGES.md](../SSH_FORK_CHANGES.md).

## Credits
scrcpy is by **Romain Vimont** and **Genymobile**, licensed under Apache-2.0. All the mirroring, encoding,
decoding and control in this fork is theirs; scrcpy-ssh only adds the SSH transport.
