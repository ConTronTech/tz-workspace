# tz-workspace

## For most people: what is this?

Short version: it's a tiny app that changes your computer's clock to
another city's time with one click, and keeps notes and copy-paste
snippets attached to each place you save.

If your job ever involves working with people in different time zones
(calling an office in another state, supporting customers overseas,
scheduling meetings with remote teammates) and you keep having to do
mental math like "wait, is it 3 PM there or 4 PM?" - this tool makes
that a non-issue.

### What can I actually do with it?

- **Save a place**, for example "Memphis Office", and tie it to that
  city's time zone. Next time you're helping the Memphis office, click
  that place once and your whole computer (clock, calendar, meeting
  times in email) is now on Memphis time. Click "Go Home" to switch
  back.
- **See two clocks at once** at the top of the window - your current
  time and your Home time - with a little orange tag showing the
  difference (like "+1h") whenever they don't match. No more almost
  working an extra hour because you forgot you were on a different
  zone.
- **Type notes for each place** (the office address, who the manager
  is, the direct line) and they automatically come back when you pick
  that place again.
- **Save copy-paste snippets** per place - phone numbers, email
  templates, commands, anything you repeat. Click Copy, paste
  anywhere.
- **Search any city or zone in the world** - including typos. Type
  "memfis tenisee" and it'll still find Memphis, Tennessee.

### How do I start?

1. Open the app.
2. On the left, click **My Places** > **+ Add place**, type a
   nickname like "Office" or "Chicago Team", and pick the time zone
   from the list.
3. Click your saved place. That's it - the computer is now on that
   time zone, and the right side of the window shows notes and
   snippets for it.
4. Click **Go Home** (top right) anytime to switch back to your own
   time zone.

### Why bother with this over a website or phone app?

- It changes the *actual system clock*, not just shows you a number.
  Your calendar, your email timestamps, your meeting reminders - all
  of it lines up with the zone you switched to.
- It's offline. No sign-in, no account, no ads, no "would you like to
  subscribe" popup. Runs local on your machine.
- Notes and snippets live with each place, so you don't end up
  juggling sticky notes or a separate app for "stuff about the
  Memphis office."

---

## For developers

A timezone-aware workspace for Linux that combines system timezone switching
with per-location notes and snippets. Built with GTK4 / gtkmm-4.0 and backed
by SQLite.

Developed for a workflow that requires frequent timezone swaps (e.g. jumping
between time zones to support different regional offices). The application
surfaces the current system timezone alongside a saved Home timezone so that
the offset between the two is always visible, and provides one-click switching
to any saved place, pinned zone, or searched IANA zone.

## Features

- One-click system timezone switching via `timedatectl`, with polkit prompt.
- Dual clock: current system zone and saved Home zone, side by side, with a
  visible offset delta when they differ.
- Named places ("locations") that store a nickname, an IANA zone, free-form
  notes, and associated snippets.
- Pinned timezones for frequent destinations.
- Full IANA zone directory with phonetic fuzzy search. Common misspellings
  resolve to the intended zone (for example, `menfis tenisee` matches
  `America/Chicago` via the "Memphis, Tennessee" alias).
- US state and major-city aliases so non-technical users can pick zones by
  place name rather than IANA path.
- Per-location reusable snippets with category filtering.
- Responsive layout: content compresses via ellipsize, and font and padding
  scale with the window height to keep the UI readable from small tiles up
  to full-screen.
- Persistent window size and paned split positions across sessions.
- Bundled SVG application icon and an auto-installed freedesktop
  `.desktop` file so the window is treated as a first-class application by
  the compositor (snap-to-tile, Alt-Tab identity, per-workspace memory).

## Requirements

- Linux with `systemd` (the app uses `timedatectl` for timezone changes).
- GTK 4 and gtkmm 4.0 development packages.
- SQLite 3 development package.
- A C++17 compiler and GNU Make.
- `polkit` for the timezone-change authorization prompt.

On Debian / Ubuntu / Mint:

```
sudo apt install build-essential libgtkmm-4.0-dev libsqlite3-dev
```

## Build

```
make
```

The default target produces a stripped, PIE-linked binary named
`tz-workspace` in the project root. The build uses stack protection and
`_FORTIFY_SOURCE=2` by default. Header-dependency tracking (`-MMD -MP`) is
enabled so header edits trigger correct recompilation.

## Run

```
./tz-workspace
```

On first launch the application will:

1. Create its data directory at `$XDG_DATA_HOME/tz-workspace` (defaulting to
   `~/.local/share/tz-workspace`), chmod 0700.
2. Open a SQLite database at `<data_dir>/data.db`.
3. Import any existing config from the legacy `time-zone-changer` directory
   at `~/.config/time-zone-changer/` (favorites, home zone, named locations).
   The import is guarded by a setting so it runs only once.
4. Write its icon to `<data_dir>/icons/hicolor/scalable/apps/tz-workspace.svg`
   and register that directory with the current GTK icon theme.
5. Install a `.desktop` entry at
   `$XDG_DATA_HOME/applications/local.tzworkspace.app.desktop` so the window
   manager recognizes the application.

## Data and configuration

All state lives in a single SQLite database at
`$XDG_DATA_HOME/tz-workspace/data.db`. Tables:

- `settings` - key/value string settings (home zone, clock preferences,
  window size, paned positions, legacy-import marker).
- `locations` - named places: id, name, IANA zone, free-form notes.
- `favorites` - pinned IANA zones.
- `snippets` - reusable text snippets, optionally scoped to a location
  and/or a category.
- `snippet_locations` - join table linking snippets to locations.
- `categories` - snippet category names.

The database is the only persistent state. Deleting the directory resets
the application.

## Security notes

The application is intended for local, single-user use but takes several
baseline precautions:

- The `timedatectl` invocation uses an argv vector (no shell interpolation),
  and the target zone is validated against the IANA zone list and a strict
  charset before being passed.
- Location, snippet, and category names are bounded in length and rejected
  if they contain control characters.
- The data directory is chmod 0700 on creation. Relative or empty
  `HOME` / `XDG_DATA_HOME` paths are rejected at startup to avoid any
  operation landing outside the user's home.
- SQL is executed exclusively through parameterised `sqlite3_stmt`
  prepared statements.

## Roadmap

- Windows port (planned; the GTK4 + SQLite stack is portable, the only
  Linux-specific piece is the `timedatectl` invocation and the freedesktop
  integration helpers).

## License

Not yet specified. Add a `LICENSE` file before redistribution.
