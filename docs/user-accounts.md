# Optional accounts and device enrollment

Accounts are local to a relay and optional. Existing authentication tokens and device identities remain the login boundary; no official account is required. Enrollment groups signed device identities without replacing the relay token or granting network membership.

`lanlink-ui-cli account create "Panda"` creates an account on the connected relay. `account issue` generates a 160-bit one-use enrollment code valid for five minutes and replaces the previous code. On another already authenticated device, run `account enroll "Desktop"` and enter the code on standard input. `account status` lists up to 64 enrolled devices. Only the founding device can issue enrollment or run `account revoke DEVICE_ID`.

Revocation persists, removes the device's memberships, advances network key epochs and sends peer revocations. The founding device and devices owning networks cannot be revoked; leave/delete their owned networks first. Revoked identities cannot authenticate again. Use a new identity to re-enroll a replacement device. Enrollment never copies a device private key or a network membership. Lost founding identities currently require relay administrator recovery from a backup.

The SQLite schema migrates from version 3 to 4 in a transaction. Back up the database before upgrades; an older binary cannot open the migrated database. Codes are stored only as SHA-256 digests and are never included in application logs. Capability negotiation prevents extension requests from being sent to legacy relays.
