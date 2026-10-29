# Invitation links and one-time codes

Network owners can issue `lanlink://join/NETWORK_ID/SECRET` links and equivalent `LL1.NETWORK_ID.SECRET` clipboard codes. The secret contains 160 random bits. The relay stores only its SHA-256 digest, expiry, network binding, and remaining use count. New links default to one use. Lifetime is configurable from 60 seconds to 24 hours, and uses from 1 to 100. Only normal membership can be granted; a link cannot confer ownership, account enrollment or administrative roles.

`lanlink-ui-cli invite-link NETWORK_ID` creates a one-use, 15-minute invitation. `lanlink-ui-cli join LINK_OR_CODE` redeems it over the existing authenticated relay connection. The connected relay must be the issuer: links cannot supply new server URLs, trust keys, authentication tokens or shell commands. `lanlink-ui-cli revoke-links NETWORK_ID` invalidates all remaining links. Issue a fresh code to replace a canceled or expired one. Already approved members remain until explicitly removed.

Expiry and decrements are checked transactionally. Redeeming a code creates a durable device-specific invitation before consuming the use, so a process interruption before the membership reply can be retried through ordinary join without spending another use. Already joined devices do not consume uses. Credential rate limits also apply to invitation issue and redemption. At most 64 active links per network are retained. Schema 6 migrates in a transaction.

This build supports copying/pasting links. OS URL-handler registration and a hosted invitation landing page are not required for redemption. Keep bearer codes private and do not place them in public issue reports or logs.
