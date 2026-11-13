# Reflector list registration

urfd can register itself with the reflector list, on a schedule, from the service
itself. This replaces the dashboard's calling-home code.

## Why it moved

The dashboard registered the reflector during page rendering (`index.php`), which
meant:

* a reflector whose dashboard nobody visited stopped being listed, even though
  the service was running perfectly;
* any visitor could force a push by adding `?callhome` to the URL;
* the identity hash was generated in PHP and kept in a world-writable file that
  the web server wrote and then `include`d;
* a registry outage aborted page rendering — `CallHome()` ended in
  `die("CONNECTION FAILED!")`;
* the payload was assembled from outside the process: uptime was inferred from
  the ctime of the pid file, the interlink list was re-parsed from
  `urfd.interlink`, and the version was read back out of the XML export.

Registration is a property of the service, so the service does it.

## Configuration

```ini
[Registration]
Enable   = false
Url      = http://xlxapi.rlx.lu/api.php
Interval = 300
HashFile = /usr/local/etc/urfd.registration
#Comment =
#OverrideIP =
```

| Key | Meaning |
|---|---|
| `Enable` | Off by default. Nothing changes on an existing install until you turn it on. |
| `Url` | The reflector list's API endpoint. |
| `Interval` | Seconds between registrations, 300–86400. The first push happens 30 s after startup, so a restart refreshes the listing. **Do not set this above 600** — see below. |
| `HashFile` | One line of plain text: this reflector's identity. Created on first run if absent. **Back it up.** |
| `Comment` | Up to 100 characters. Defaults to `[Names]Sponsor`. |
| `OverrideIP` | Leave blank to let the list autodetect the address. |

The callsign, country, dashboard URL, version, uptime and interlink list all come
from the running reflector. There is nothing to keep in step by hand.

## Why the interval has to stay under 600 seconds

The reflector list's own dashboard decides whether a reflector is up by looking at
nothing but the age of its last contact (`dashboard/pgs/reflectors.php`):

```php
if ($LASTCONTACT < (time() - 600)) { echo 'down'; } ELSE { echo 'up'; }
```

Ten minutes, and that is the whole test. It does not probe the reflector, so a
reflector that registers less often than that shows as **down** for most of every
cycle while being perfectly healthy. An hourly interval, for instance, reads as
down for about 83% of the time.

The default of 300 leaves a full interval of margin: one missed registration still
lands inside the window. urfd warns at startup if the interval is set above 600,
because the symptom — a red icon on a working reflector — is not obviously a
configuration problem.

For context on how gentle this still is: the dashboard's `PushDelay` ships at 10
seconds, so a browsed dashboard re-registered up to 360 times an hour. This
registers 12 times an hour, on a fixed schedule, whether anyone is looking or not.

## A new reflector

Set `Enable = true` and start urfd. If `HashFile` does not exist, urfd generates a
random 16-character hash, writes it there, and logs `Registration: wrote a new hash`.
Later starts read it back. urfd must be able to create that file; if it cannot, it
logs why and does not register. Back the file up: losing it means a new listing.

## Moving an already-listed reflector

The list identifies a reflector by its hash. If urfd generates a fresh one, your
reflector may appear as a second, unrelated entry. So, in this order:

1. **Copy the existing hash.** In the dashboard's `callinghome.php` there is a
   line like `$Hash = "aB3dE6gH9jK2mN5p";`. Put just that value — no quotes, no
   PHP — on the first line of the file named by `HashFile`:

   ```sh
   echo 'aB3dE6gH9jK2mN5p' > /usr/local/etc/urfd.registration
   chown urfd /usr/local/etc/urfd.registration   # whoever urfd runs as
   ```

2. **Stop the dashboard from calling home.** In the dashboard config:

   ```php
   $CallingHome['Active'] = false;
   ```

   Leaving both enabled registers the same reflector twice per interval.

3. **Turn this on**, and restart urfd:

   ```ini
   [Registration]
   Enable = true
   ```

Watch the log on startup. It says which hash file it used, and whether it found
an existing hash or wrote a new one:

```
Registration: using the existing hash in /usr/local/etc/urfd.registration
Registration: http://xlxapi.rlx.lu/api.php every 300s, first push in 30s
Registration: registered with http://xlxapi.rlx.lu/api.php
```

## When it goes wrong

A registry that is unreachable, slow, or answering with an HTTP error is logged
and retried at the next interval. It never stops the reflector, and — unlike the
dashboard's version — it cannot take a web page down with it:

```
WARNING: registration with http://xlxapi.rlx.lu/api.php failed (curl 7): Could not connect to server
WARNING: retrying in 300s; this reflector may drop off the list (first failure)
```

Success after a failure is logged once, so a flapping registry is visible in the
log without a line every hour when things are fine.

An unusable `HashFile` — bad path, wrong permissions, non-alphanumeric contents —
switches registration off for that run and says so, rather than registering under
a fresh identity and creating a duplicate listing.

## Compatibility

The payload is byte-for-byte what the dashboard posted: the same
`<query>CallingHome</query>`, `<reflector>` and `<interlinks>` elements, posted as
`application/x-www-form-urlencoded` with the XML in an `xml` field. The reflector
registers under its **XLX** name, which is what the list already knows it by —
the dashboard sent `"XLX" . <the three characters after "<XLX" in the XML export>`,
and urfd applies the same patch to its own callsign.

One deliberate difference: for an interlink peer listed without an IP address, one
resolved through the DHT, urfd reports the address it actually resolved and the
modules it is really using. The dashboard, parsing the interlink file positionally,
would have put the module list in the address field for those entries.
