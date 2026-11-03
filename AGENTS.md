# Open work

Read `HANDOFF.md` before building, testing or deploying: it lists what is live, what is still open, and the rules for sharing this tree with other sessions.

# Server and deployment operations

Use the configured Discord webhook for notifications when changing the live server or deployment. Announce maintenance before changes, then report completion or failure, including rollback when applicable. `sandalctl.ps1` sends these notices automatically for its maintenance commands; do not duplicate them.

For live changes outside `sandalctl`, use `C:\SandalCore\ops\notify-discord.ps1` on the VPS (`sun-vps`), with `-Message` and `-Level info|success|warning|error`. Notification failures must not prevent recovery or maintenance.

**The players' work board.** After every deploy, as its last step, post to the progress webhook (`D:\AshesReborn\SandalCoreDeploy\secrets\discord-progress-webhook.secret`, a different webhook from the maintenance one): a **Now live** list of what players get from that deploy, in player terms, and, when it changes, a numbered **Working on next** list (when there is a lot: what is coming in the next update). Post only what changed since the last post: no repeats, no "needs a try in game" lists, no tests or internal work. Send it with curl and a short-lived config file (`{"content": ...}` from a JSON encoder), never with the URL on a command line. Never print, log or commit its value.

The webhook is a secret. Its server path is `C:\SandalCore\secrets\discord-webhook.secret`; the build PC copy is `D:\AshesReborn\SandalCoreDeploy\secrets\discord-webhook.secret`. Never put its value in tracked files, release packages, logs or chat output. See `deploy/vps/README.md` for the notification workflow.
