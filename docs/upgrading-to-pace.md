# Upgrading from fastcached to Pace

Release 0.6.0 renames the project. **fastcached** is now **Pace**, and every
executable, service, account, directory, environment variable and CMake option
carries the new name. It is a clean break: Pace reads none of the old names, and
it refuses to install while fastcached is still installed. Plan the switch as an
uninstall followed by an install.

## What was renamed

| | fastcached (0.5.x) | Pace (0.6.0) |
|---|---|---|
| Cache daemon | `fastcached` | `pace-kv` |
| Compile node | `fastcache-compile-node` | `pace-node` |
| Compiler launcher | `fastcache-cc` | `pace-cc` |
| Operator client | `fastcache-cli` | `pacectl` |
| Windows services | `FastCached`, `FastCacheCompileNode` | `pace-kv`, `pace-node` |
| systemd units | `fastcached.service`, `fastcached-user.service`, `fastcache-compile-node.service`, `.socket` | `pace-kv.service`, `pace-kv-user.service`, `pace-node.service`, `.socket`, and the new `pace-node-user.service` |
| Linux accounts | `fastcached`, `fastcache-node` | `pace-kv`, `pace-node` |
| launchd labels | `software.lastrada.fastcached`, `software.lastrada.fastcachecompilenode` | `software.lastrada.pace-kv`, `software.lastrada.pace-node` |
| Environment variables | `FASTCACHE_*` (`FASTCACHE_ADDR`, `FASTCACHE_SCHEDULER`, ...) | `PACE_*` (`PACE_ADDR`, `PACE_SCHEDULER`, ...) |
| CMake options | `FASTCACHED_*` | `PACE_*` |
| Metrics | `fastcached_*`, `fastcache_*` | `pace_*` |
| Windows installer properties | `FASTCACHE_FLEET_ID`, `FASTCACHE_FLEET_SEED`, `FASTCACHE_FIREWALL_ALLOW`, `FASTCACHED_START_SERVICE`, `FASTCACHE_DISCOVERY_REPLY_PORT` | `PACE_FLEET_ID`, `PACE_FLEET_SEED`, `PACE_FIREWALL_ALLOW`, `PACE_START_SERVICE`, `PACE_DISCOVERY_REPLY_PORT` |
| DNS SRV record a node asks for | `_fastcache._tcp.<DNS suffix>` | `_pace._tcp.<DNS suffix>` |
| Documentation site | `lastrada-software.github.io/fastcached/` | `lastrada-software.github.io/pace/` |

The old documentation address does **not** redirect; update bookmarks. GitHub
redirects the old repository URL to `LASTRADA-Software/pace`.

### Where the files are

| Platform | fastcached (0.5.x) | Pace (0.6.0) |
|---|---|---|
| Windows, install | `C:\Program Files\fastcached` | `C:\Program Files\Pace` |
| Windows, daemon config | `%ProgramData%\fastcached\fastcached.yaml` | `%ProgramData%\pace\pace-kv.yaml` |
| Windows, node config | `%ProgramData%\fastcache-compile-node\fastcache-compile-node.yaml` | `%ProgramData%\pace\pace-node.yaml` |
| Windows, per-user config | `%APPDATA%\fastcached\fastcached.yaml` | `%APPDATA%\pace\pace-kv.yaml` |
| Windows, node state and identity | `%ProgramData%\fastcache-node` | `%ProgramData%\pace-node` |
| Linux, configs | `/etc/fastcached/fastcached.yaml`, `/etc/fastcached/fastcache-compile-node.yaml` | `/etc/pace/pace-kv.yaml`, `/etc/pace/pace-node.yaml` |
| Linux, state | `/var/lib/fastcached`, `/var/lib/fastcache-node` | `/var/lib/pace-kv`, `/var/lib/pace-node` |
| macOS, install | `/opt/fastcached` | `/opt/pace` |
| macOS, configs | `/opt/fastcached/etc/fastcached.yaml`, `/opt/fastcached/etc/fastcache-compile-node.yaml` | `/opt/pace/etc/pace-kv.yaml`, `/opt/pace/etc/pace-node.yaml` |
| macOS, node state | `/Library/Application Support/fastcache-node` | `/Library/Application Support/pace-node` |
| Launcher state and log | `%LOCALAPPDATA%\fastcache-cc`, `~/.cache/fastcache-cc` | `%LOCALAPPDATA%\pace-cc`, `~/.cache/pace-cc` |

The layout itself is unchanged: an administrator-only machine config, and a
private state directory per service account.

## Step by step

### 1. Uninstall fastcached

Uninstalling removes fastcached's services through its own, tested path. What
happens to your configuration files depends on the platform, so read this before
you run it:

- **Windows:** Settings > Apps > fastcached > Uninstall (or
  `msiexec /x fastcached-0.5.0-Windows-AMD64.msi`). The files under
  `%ProgramData%\fastcached` and `%ProgramData%\fastcache-compile-node` stay.
- **Debian / Ubuntu:** `sudo apt remove fastcached`. The files under
  `/etc/fastcached` stay (`remove`, not `purge`).
- **RHEL / Fedora:** `sudo dnf remove fastcached`. A file you edited stays as
  `/etc/fastcached/<name>.yaml.rpmsave`; one you never edited is removed, and
  had nothing to copy.
- **macOS:** the uninstaller deletes `/opt/fastcached` **including your
  configuration in `/opt/fastcached/etc`**, so copy it aside first:

  ```sh
  mkdir -p ~/fastcached-config && cp /opt/fastcached/etc/*.yaml ~/fastcached-config/
  sudo /opt/fastcached/bin/fastcached-uninstall
  ```

The Windows installer's remembered properties (`HKLM\SOFTWARE\fastcached\Installer`)
are forgotten by this uninstall and are not read by Pace either: pass what you
need again, under the `PACE_*` property names from the table above, when you
install in step 2. `PACE_START_SERVICE` keeps its old meaning under its new name: it
decides whether `pace-kv` is started, and only `pace-kv`; the node has no such
property and is always started when it is installed.

### 2. Install Pace

Install the 0.6.0 package for your platform (`pace-0.6.0-...`). If fastcached
is still installed, the installer refuses and names the step above:

- the MSI stops with "fastcached is installed. Uninstall it (Settings > Apps >
  fastcached), then install Pace.";
- `apt` and `dnf` report that `pace` conflicts with `fastcached`;
- the macOS package names `/opt/fastcached/bin/fastcached-uninstall`.

On Windows, `msiexec` accepts a property it does not know without a word, so an
install command or script still passing `FASTCACHE_FLEET_ID=...` or any other old
property installs Pace **without** that setting. Rename every one, the colleague
installer's included (step 6).

A compile node that runs as you rather than as a service account is new in
Pace on both POSIX platforms:

- **macOS:** `pace-node --install-service --service-scope=user` registers the
  LaunchAgent `software.lastrada.pace-node` for the invoking account (the
  package's LaunchAgent choice is the cache daemon's).
- **Linux:** `systemctl --user enable --now pace-node` starts the packaged
  user unit.

Either keeps its identity in `~/.local/state/pace-node`, the directory the node
chooses by itself. Run the node or `pace-kv` as you, not both: each serves on 6674.

### 3. Copy your settings

Copy what you changed from the old file into the new one (on macOS, from the
copy you made in step 1): `fastcached.yaml` into `pace-kv.yaml`, and
`fastcache-compile-node.yaml` into `pace-node.yaml`. Every key keeps its name,
but two kinds of value do not carry over:

- **`fleet_id:`** names the OLD fleet, which no Pace node belongs to. Leave it
  out; step 5 says what to pin instead.
- **A path** (`cluster_dir:`, `storage_path:`, a key file) still names the old
  directory. Change it to the new one from the table above.

### 4. Rename the environment variables

Every `FASTCACHE_*` variable is now `PACE_*`: `FASTCACHE_ADDR` is `PACE_ADDR`,
`FASTCACHE_SCHEDULER` is `PACE_SCHEDULER`, and so on. Nothing reads the old
spelling any more. So that a build does not stop distributing in silence,
`pace-cc` and `pace-node` print one line on stderr while any `FASTCACHE_*`
variable is still set, naming each one.

A CMake build tree configured before the rename holds `FASTCACHED_*` cache
entries. Its next configure fails, naming them: delete the build tree and
configure again with the `PACE_*` options.

### 5. Fleet members: re-enroll

A node's identity is the key in its state directory, and Pace's directory is
`pace-node` rather than `fastcache-node`, so every node mints a fresh identity
and the fleet is formed again, as a new fleet with a new fleet id:

1. **Upgrade the leader first**, with no fleet pin (no `PACE_FLEET_ID`, no
   `fleet_id:`). It founds the new fleet; `pacectl node` on it prints the new
   `fleet-id`.
2. **Then every other member**, pinned to that new value (`PACE_FLEET_ID=<new
   fleet-id>` on Windows, `fleet_id:` elsewhere), with the seed it used before
   if it needs one. A node that found the leader through a DNS SRV record asks
   for `_pace._tcp.<DNS suffix>` now, so publish that record beside (or instead
   of) the `_fastcache._tcp` one.
3. **Approve each joiner on the leader**, as for any new machine, and promote
   the ones that were voters again.

There is nothing to forget: the new fleet never recorded the old members.

Moving the old state directory to keep a node's identity is **not** supported.
`pace-node --install-service` hands the directory to the new service account but
not the files already in it, and nothing on any platform was tested to do more.

### 6. Lastrada builds

- `FASTCACHE_DEFAULT_VERSION` becomes `PACE_DEFAULT_VERSION` = `0.6.0`, with the
  new asset names (`pace-0.6.0-...`).
- Every `FASTCACHE_*` in `cmake/cache.cmake` and the CI definitions becomes
  `PACE_*`, including `FASTCACHE_MINT_PENALTY_VERSIONS`.
- The colleague distribution moves to a NEW directory, and the old one is left
  as it is. `X:\LASTRADA\Tools\fastcache\` keeps fastcached 0.1.1 to 0.5.0
  unchanged, so a build still pinned to it keeps working during the switch.
  Pace, from 0.6.0, lives in `X:\LASTRADA\Tools\pace\<version>\`: `pace-cc.exe`,
  the MSI, the zip and the deb, `install-pace.cmd` and `README.txt`. Every property
  `install-pace.cmd` passes to the MSI is renamed to `PACE_*` (step 2).
- Lastrada's `cmake/cache.cmake` points at
  `x:\LASTRADA\Tools\pace\<version>\pace-cc.exe`, together with its version pin.
  That change is Lastrada's merge request, not this repository's.

### 7. Bookmarks

The documentation moved from `https://lastrada-software.github.io/fastcached/`
to `https://lastrada-software.github.io/pace/`, and the old address does not
redirect.
