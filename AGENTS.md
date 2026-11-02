# Open work

Read `HANDOFF.md` before building, testing or deploying: it lists what is live, what is still open, and the rules for sharing this tree with other sessions.

# Server and deployment operations

Use the configured Discord webhook for notifications when changing the live server or deployment. Announce maintenance before changes, then report completion or failure, including rollback when applicable. `sandalctl.ps1` sends these notices automatically for its maintenance commands; do not duplicate them.

For live changes outside `sandalctl`, use `C:\SandalCore\ops\notify-discord.ps1` on the VPS (`sun-vps`), with `-Message` and `-Level info|success|warning|error`. Notification failures must not prevent recovery or maintenance.

The webhook is a secret. Its server path is `C:\SandalCore\secrets\discord-webhook.secret`; the build PC copy is `C:\Users\mrjam\SandalCoreDeploy\secrets\discord-webhook.secret`. Never put its value in tracked files, release packages, logs or chat output. See `deploy/vps/README.md` for the notification workflow.
