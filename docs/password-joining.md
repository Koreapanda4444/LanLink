# Password-protected networks

An owner can set or remove a network password without creating an account. The verifier uses a unique 32-byte salt and OpenSSL scrypt (N=32768, r=8, p=1, 32-byte output). Passwords contain 8–128 bytes, travel only inside the authenticated encrypted control connection, and are never stored or logged in plaintext. Existing TLS certificate and relay-token checks remain required.

`lanlink-ui-cli password NETWORK_ID` reads a new password from standard input with console echo disabled; an empty password removes protection. Append `approve` to require owner approval after successful password verification. `lanlink-ui-cli join-password NETWORK_ID` reads the password and joins or submits a pending request. Explicit owner invitations remain valid. Legacy clients cannot bypass a protected network by submitting an ordinary join request.

Credential checks are bounded per device/network (5/minute), per device (10/minute), per network (20/minute), and server-wide (40/minute), including successful attempts. All KDF work is serialized with access decisions. At most 4096 throttle entries are retained and old entries expire. Unknown IDs and wrong passwords return the same denial and perform the same KDF. These in-memory limits reset after a relay restart; existing transport request limits also apply.

The database migrates transactionally to schema 5. Password changes do not evict already approved members; explicitly remove them if their access must end. Manual approval and key/peer notifications follow the existing membership path.
