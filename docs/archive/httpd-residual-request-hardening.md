# httpd: request-path hardening WP4 left for later

**Severity:** low to medium (none of these is memory corruption; each is
denial of service, a policy gap or a misleading surface).
**Filed:** 2026-09-30, by security-audit-plan WP4 (M-4).
**Tag:** security-

**RESOLVED 2026-10-03: item 9 is done too, so every item is closed.**

- **9** -- `(httpd-set-bind-addr! addr)` names the interface: a numeric IPv4
  or IPv6 address (`"192.168.1.5"`, `"::1"`, `"::"`), winning over
  `httpd-set-bind-any!`; `""` clears it; an address that does not parse
  returns `false` and leaves the setting alone.  Both constructors now open
  their listener through one helper, `httpd-listen-socket`, which picks the
  socket family from the address -- with no address set it binds exactly as
  before -- and `httpd-port` / `httpd-async-port` read either family's port.
  `TUR_BIND_LOOPBACK` still forces loopback, of the address's own family.
  Pinned by `tests/fixtures/httpd-bind-addr` (the parse, IPv6 literals
  included, and a live round trip on a named IPv4 address).  The IPv6 BIND
  was not exercised: the container this was written in has no IPv6 stack
  (`EAFNOSUPPORT`), where a server on `"::1"` comes back NULL from both
  constructors, cleanly -- checked by hand.

**Narrowed a third time 2026-10-03: item 3 is fixed; only 9 remained** (an
enhancement: IPv6 and a bind address beyond loopback / every interface).

- **3** -- the default chosen is **512** for both servers.  `httpd-new-async`
  is `(httpd-new-async-with-limit port handler 512)`; the blocking pool's
  pending queue (accepted connections waiting for a worker, each an open
  descriptor) stops at 512 too, answering `503` and lingering the close as
  the async cap does.  512 sits under the common 1024 open-file soft limit,
  so a flood is refused before it exhausts the process's descriptors.  `0`
  still means unlimited, asked for by name:
  `httpd-new-async-with-limit` as before, and the new
  `httpd-new-pool-with-limit port workers handler max-pending`.  Pinned by
  `tests/fixtures/httpd-pool-pending-limit` (one worker, cap 1: the third
  connection is refused while the second waits; both defaults read back).

**Narrowed again 2026-10-03: item 4 is fixed; 3 and 9 remained** (3 needed a
default chosen, 9 is an enhancement).

- **4** -- `mw-rate-limit`'s table is keyed by the IP string (first 47
  bytes, the whole of any IPv6 text form; the hash still covers a longer
  key), so `10.0.107.237` and `10.2.219.40`, which share an FNV-1a hash, no
  longer share a counter.  It is 8-way set-associative (256 sets, 2048
  entries, 128 KiB): a new IP takes an empty entry of its set, else one whose
  window has ended, else the oldest window in the set.  The default chosen is
  **evict, never fail open and never fail closed**: a flood of distinct IPs
  restarts the windows of the IPs it evicts, but a new IP is always tracked,
  so it cannot leave every later IP unlimited (4096 distinct IPs used to);
  and each check touches at most 8 entries, so the flood buys no
  per-request scan.  Pinned by the `rl` cases in
  `tests/fixtures/httpd-request-hardening` -- against the old table the
  colliding IP was refused and the IP after the flood was never limited.

**Narrowed 2026-10-03: item 10 is fixed too; 3, 4 and 9 remained** (3 and 4
need a default chosen, 9 is an enhancement).

- **10** -- `httpd-req-multipart-parse` refuses a non-`multipart/` media type;
  reads `boundary` as a parameter of it, matched by name in any case (so a
  quoted `charset="boundary=YY"` cannot supply it), and caps it at RFC 2046's
  70; reads `name` and `filename` as parameters of `Content-Disposition` (so
  `filename="a.bin"; name="up"` names the part `up`, not `a.bin`); matches a
  part header by its whole field name; and requires the CRLF after each
  delimiter.  Worse than the filing said: every search was `strstr`, so a NUL
  byte in an uploaded file ended the scan -- the part was never terminated and
  EVERY part was lost.  All searches are now bounded by the body's length
  (`httpd_mp_find`, beside `httpd_hdr_param` in the file-scope include block).
  Against the old parser, five of the six new `mp` cases in
  `tests/fixtures/httpd-request-hardening` came out wrong.

**Narrowed 2026-10-01: items 1, 2, 5, 6, 7 and 8 are fixed; 3, 4, 9 and 10
remained** (each needs a default chosen, or is an enhancement).

- **1** -- both header read loops resume the terminator search 3 bytes (1 for
  `\n\n`) before where the previous one stopped, instead of rescanning the
  whole buffer after every `recv`.
- **2** -- `httpd-async-fiber-body`'s response writes park 5 s per wait, as the
  reads do; a write that times out or fails closes the connection.
  (`httpd-await-writable`, the user-facing primitive, keeps its no-timeout
  contract.)
- **5** -- a method or version of 15+ bytes is refused 400, a path of 1023+
  bytes 414 (`URI Too Long`, new in `httpd-status-text`), where the field used
  to be left `""` and the request served.
- **6** -- `Connection` is read as a comma-separated token list, each token
  matched whole: `closed` is not `close`, `Upgrade, close` closes, and
  `keep-alives` is not `keep-alive`.
- **7** -- the `mw-basic-auth` example compares both fields every time and
  combines the results with `*`.
- **8** -- `mw-log` writes every byte of the method and path outside printable
  ASCII (and the backslash) as `\xHH`, so a request cannot forge a log line.

Pinned by new cases in `tests/fixtures/httpd-request-hardening` (5, 6, 8).
The items below keep their original numbering.

WP4 closed M-4's memory-safety and request-smuggling items in
`stdlib/httpd.tur`. It also rewrote the static handler's containment and
fixed the `httpd-set-cookie!` stack overrun (see the plan, section 2e). The
research pass (a full read of all 4300 lines) turned up the items below, which
were left alone because each one needs a design decision or is out of scope
for the parser work. Line numbers are against the WP4 branch.

## Denial of service

1. **Header scan is quadratic.** Both read loops call
   `strstr(buf, "\r\n\r\n")` over the whole buffer after every `recv`, so a
   peer trickling one byte at a time up to the 256 KiB header cap costs about
   n^2 / 2 byte compares. Fix: resume the search from `total - 3`.
2. **Async writes park forever.** The response write loops in
   `httpd-async-fiber-body` call `tur_local_park_fd(group, fd, 2, -1)`. A
   client that stops reading holds its fiber and its in-flight slot
   indefinitely. The reads got a 5 s bound in WP4; the writes want the same.
3. **No in-flight cap on `httpd-new-async`.** Only
   `httpd-new-async-with-limit` bounds it. The blocking pool's fd queue also
   grows without limit.
4. **Rate limiter fails open.** `mw-rate-limit` matches IPs by 32-bit FNV hash
   alone, so colliding IPs share a bucket. Its 1024 slots are never evicted, and
   once the table fills every new IP is allowed.

## Policy and surface

5. **Oversized request-line fields are silently dropped.** A method of 15 or
   more bytes, or a path of 1023 or more, leaves the field `""`, where it
   should be a 400 or 414.
6. **The `Connection` header is prefix-matched.** `closed` counts as `close`.
7. **Basic-auth example leaks username validity.** `cstr-eq-const-time` is
   documented as leaking length. The doc example short-circuits on the
   username before comparing the password, which leaks whether a username is
   valid. The example should compare both unconditionally and combine the
   results.
8. **`mw-log` writes raw request bytes.** It prints the method and path to
   stdout, so control characters pass into logs.
9. **IPv4 only.** There is no API to choose a bind address beyond loopback
   versus every interface (`httpd-set-bind-any!`).
10. **Multipart parsing is loose.** It does not check that the Content-Type
    is multipart. `boundary=` is found case-sensitively anywhere in the
    header. `name="` also matches inside `filename="`, and part-header lines
    are scanned with `strstr` to the end of the body.

## Repro

Each item reads directly off the named function in `stdlib/httpd.tur`. None
needs a crafted input to see.

## Fix directions

Items 1, 2, 5 and 6 are local fixes of a few lines. For items 3 and 4, pick a
default before changing anything, because they change behaviour under load.
Item 7 is a docs fix. Items 8 to 10 are enhancements. Each fix should extend
`tests/fixtures/httpd-request-hardening` (socket-free) or the
`tests/fuzz/fuzz_httpd_head` seeds.
