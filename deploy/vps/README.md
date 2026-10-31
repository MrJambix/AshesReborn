# Reborn Launcher server: a modified SandalCore on a VPS shared with another application

Players know this as Reborn Launcher. It is a modified SandalCore deployment (branch `vps-deploy`), not the original project and not endorsed by Cyb. Internal names on the VPS, such as `C:\SandalCore` and the `SandalCore-*` task, service and firewall rules, keep the old name because players never see them. Developed by Cyb. Thanks to Hatemost and CoA (Creation of Ashes). The changes from upstream are this folder, `/bigobj` in `server/worldd/CMakeLists.txt`, a TLS and rate-limited public login listener in the auth service (`services/internal/authhttp/limit.go`, `services/cmd/auth`, `services/internal/config`), `auth_server_tls` in the launcher, and the Reborn Launcher name and credit line in the launcher and tether.

It runs beside another game server on a 2 vCPU, 4 GB Windows Server, so it is built to give way: its programs run at below-normal priority, a watchdog stops it if the machine runs out of memory, and at most 10 players can be on at once (`$MaxPlayers` in `ops\supervisor.ps1`). The programs that face the internet run as LOCAL SERVICE, not as an administrator.

## Layout on the VPS (`C:\SandalCore`)

| Path | What it is |
|---|---|
| `dist\` | programs and game data; logs in `dist\services\logs` |
| `ops\` | these scripts; `site.json` holds the public IP and the owner's IP |
| `secrets\`, `certs\`, `backups\` | database password, service token, TLS key, database dumps; SYSTEM and Administrators, plus read for LOCAL SERVICE on `secrets\` and `certs\` |
| `pgsql\`, `pgdata\` | PostgreSQL 17 program and data |

NATS is not installed: the docs mention it, but no code uses it.

## Ports

| Port | Program | Reachable from |
|---|---|---|
| TCP 8443 | gateway, TLS | everyone, or the owner after `close` |
| TCP 8444 | auth, sign-in over TLS, rate limited | everyone, or the owner after `close` |
| UDP 7777 | worldd | everyone, or the owner after `close` |
| TCP 8081 | auth, plain HTTP, not rate limited, for the health check | loopback |
| TCP 5432, 50051-50058 | PostgreSQL, internal services, admin API | loopback |

Do not open 8081 to the internet: it is clear text and has no rate limit. Anyone can register on 8444, up to 5 accounts an hour from one address. World traffic on UDP 7777 is not encrypted.

## Commands

`powershell -File C:\SandalCore\ops\sandalctl.ps1 <command>`

| Command | What it does |
|---|---|
| `start`, `stop`, `restart` | clean start and stop of the stack; PostgreSQL stays up |
| `status` | programs, health, memory, watchdog state |
| `logs [name]` | follow a log (default `worldd`; also `auth`, `character`, `citynode`, `quests`, `mesh`, `gateway`, `supervisor`) |
| `diag` | status plus ports, firewall, certificate, backups, disk and log errors |
| `backup` | database dump to `backups\`; keeps 14; also runs daily at 04:30 |
| `restore <file>` | dumps first, stops, restores, starts |
| `update <package.tar>` | dumps first, swaps the programs, goes back by itself if they do not come up |
| `open` | lets everyone reach the game ports |
| `close` | locks the game ports to the owner's IP, for an emergency or maintenance |
| `allow-ip <ip>` | locks the game ports to one IP |

GM tools are in `dist\services` (`worldctl`, `nodectl`, `questctl`); give them `--token-file C:\SandalCore\secrets\service.token`.

## Updating

On the build PC: pull, run `build_all.bat`, then `deploy\vps\package.ps1 -PublicIp <ip>`, copy the package over with scp, and run `sandalctl update <package.tar>`.

## Players

On the build PC, after committing: `deploy\vps\package-client.ps1 -ServerIp <ip>` makes `RebornLauncher.zip`. It holds the launcher, tether, CA certificate, settings and source, and no game files or keys. Players unzip it and run `Start Reborn Launcher.bat`; `README.txt` in the pack is their guide.

Rebuild and redistribute the pack when the launcher, certificate or address changes. The AGPL requires offering players the source of what this server runs, so keep the pack's commit in step with the server.

## If something is wrong

- **Stack stopped, status shows the watchdog:** the machine ran short of memory (over 13 GB committed, or under 60 MB available for a minute). Check the other application first, then `sandalctl start`.
- **Under attack, or the other application is suffering:** `sandalctl close`, then `sandalctl stop` if needed.
- **Your home IP changed:** update `owner_ip` in `ops\site.json`. The SSH firewall rule belongs to the machine and needs its own change.
- **Certificates:** the server certificate lasts 825 days and its CA key no longer exists; to renew, delete `vps-certs` on the build PC, package, copy the new files to `C:\SandalCore\certs`, rebuild the client pack and send it to everyone.
