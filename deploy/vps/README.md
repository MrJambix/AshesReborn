# SandalCore on a VPS shared with another application

This is a modified SandalCore deployment (branch `vps-deploy`), not the original project and not endorsed by Cyb. Developed by Cyb. Thanks to Hatemost and CoA (Creation of Ashes). The changes from upstream are in this folder and one build setting (`/bigobj`) in `server/worldd/CMakeLists.txt`.

It runs beside another game server on a 2 vCPU, 4 GB Windows Server, so it is built to give way: its programs run at below-normal priority, and a watchdog stops it if the machine runs out of memory.

## Layout on the VPS (`C:\SandalCore`)

| Path | What it is |
|---|---|
| `dist\` | programs and game data; logs in `dist\services\logs` |
| `ops\` | these scripts; `site.json` holds the public IP and the allowed IP |
| `secrets\`, `certs\`, `backups\` | database password, service token, TLS key, database dumps; SYSTEM and Administrators only |
| `pgsql\`, `pgdata\` | PostgreSQL 17 program and data |

NATS is not installed: the docs mention it, but no code uses it.

## Ports

| Port | Program | Reachable from |
|---|---|---|
| TCP 8443 | gateway, TLS | the allowed IP only |
| UDP 7777 | worldd | the allowed IP only |
| TCP 8081 | auth, sign-in | loopback; players use an SSH tunnel |
| TCP 5432, 50051-50058 | PostgreSQL, internal services, admin API | loopback |

The sign-in stays on loopback because the launcher sends the password over plain HTTP and the auth API lets anyone register. Do not open 8081 to the internet. World traffic on UDP 7777 is not encrypted.

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
| `allow-ip <ip>` | moves the game ports to a new home IP |

GM tools are in `dist\services` (`worldctl`, `nodectl`, `questctl`); give them `--token-file C:\SandalCore\secrets\service.token`.

## Updating

On the build PC: pull, run `build_all.bat`, then `deploy\vps\package.ps1 -PublicIp <ip>`, copy the package over with scp, and run `sandalctl update <package.tar>`.

## Playing

On the gaming PC, with the game closed: `powershell -ExecutionPolicy Bypass -File deploy\vps\play.ps1`.

## If something is wrong

- **Stack stopped, status shows the watchdog:** the machine ran short of memory (over 13 GB committed, or under 60 MB available for a minute). Check the other application first, then `sandalctl start`.
- **The game cannot connect after your home IP changed:** run `sandalctl allow-ip <new ip>`. The SSH firewall rule belongs to the machine and needs its own change.
- **Certificates:** the server certificate lasts 825 days and its CA key no longer exists; to renew, delete `vps-certs` on the build PC, package, copy the new files to `C:\SandalCore\certs` and run `play.ps1` again.
