# U30 Air native dual SIM (experimental)

This opt-in OpenWrt path provides a LuCI **Cellular > Dual SIM** page and
`mu300-sim` CLI. Both physical cards can be selected without booting Android
first. A live switch that gets mobile data saves the target as the next
cold-boot default, matching the factory system's selection behavior. Android
partitions, firmware and subscriber identities are not modified.

**One controller, with a U30 backend.** `mu300-sim` retains the shared F50
selector from merged #72; its public interface is `status [--json]`,
`active`, `configured`, `select external|internal`, and `default 1|2`.
The dedicated `mu300-sim-u30` backend handles native cold/live operations only
on positively identified U30 Air/OpenWrt. F50 keeps its boot-only selector;
Ubuntu never dispatches the U30 backend. Unknown devices retain slot-0
routing and cannot opt into slot 1. M3 remains a compatibility candidate,
not a hardware-validated target.

Both frontends read OpenWrt preferences from `network.wan.sim_slot` (0/1)
and `network.wan.sim_hot` (0/1). On the first U30 startup, legacy
`/etc/mu300-sim-slot` and `/etc/mu300-sim-hot` values are migrated only into
absent UCI options. Existing choices, including explicitly empty profile
options, are preserved. When the old selection was SIM2, the old shared
APN/PDP is copied into absent `apn_internal`/`pdptype_internal` options.
Legacy preference files are retained under `/etc/mu300/sim-migration/`
as `.migrated` backups after the commit;
they are no longer a second active configuration source. Failed commits keep
the old preference visible and leave migration backups intact.

Runtime scripts read the pinned `/run/mu300/sim-slot`, not a newly saved boot
preference. U30 native brokers keep distinct nr1/nr2/nr4/nr5 roles; F50's
selected nr1/nr2 or nr4/nr5 still use the established general/data role
names. F50's context/PCO/QoS/SPSWDATA setup is not reused as a U30 switch
sequence.

**A failed cold start never costs more than one boot.** The dual-SIM path
leaves `/etc/mu300-sim-pending` until `mu300-sim2-warmup` confirms the radio
is up. When the cold start cannot be done (no card, PIN, the modem refusing a
command, the second group's brokers never coming up) the warm-up puts the
settings back to SIM1 without live switching and records why in
`/etc/mu300-sim-fallback` (`cold-start-failed`, `brokers-missing`); if nothing
was sent to the radio yet, this boot already runs the ordinary SIM1 warm-up.
A pending marker still there at the next boot (`unconfirmed`: the warm-up
died) makes that boot a plain SIM1 boot too. The page shows the reason until
the user chooses again; `mu300-sim status --json` carries it as `fallback`.

## Select and switch

The fresh-image default remains SIM1, with live switching disabled. Enable the
hot-switch checkbox and reboot once to prepare both groups of persistent AT
brokers. Then use **Switch to SIM1/SIM2**. On an existing SIM2 cold-start boot
where all four brokers are already held open, another reboot is unnecessary.
Disabling the checkbox blocks further live switches; already-open channels stay
open until shutdown. **Use SIM1/SIM2 at next boot** saves a boot preference
without moving the current data session.

```sh
mu300-sim status --json
mu300-sim hot 1       # enable; reboot if status.available is 0
mu300-sim switch 2    # change now, and save SIM2 for subsequent boots once it has mobile data
mu300-sim default 1   # change the NEXT boot only (no radio command, no hot switching needed)
mu300-sim hot 0       # block live switching
```

The CLI and page use public card numbers 1/2; stored slots and `active` /
`configured` CLI values remain 0/1 for compatibility with #72. `status` is
text; rpcd uses `status --json`, with schema `version: 1`. JSON separates
`active`, `default`, `hot`, capabilities, operation phase/busy/result/error,
`radio_ready`, `registered`, and `wan_up`; an unknown radio/card or an
unobserved state is reported as null. Status reads snapshots and WAN status,
without opening modem channels.

SIM1 uses `network.wan.apn` / `pdptype`; SIM2 uses `apn_internal` /
`pdptype_internal`. Netifd tears down the old bearer before the runtime slot
changes, then dials the selected profile. A normal live switch is saved only
once WAN is up, has an address, and names the selected `sipa_eth0` or
`sipa_eth8` (60 seconds by default). A missing bearer, a wrong/stale bearer,
or a failed target `ifup` triggers one rollback to the previous active SIM;
the already saved next-boot choice is unchanged. The operation deadline is
300 seconds, covering RF transition, data confirmation and rollback.

**Manual `default 1|2` is independent of all of that.** It needs neither hot
mode nor target WAN service, never moves RF or restarts the current WAN, and
can intentionally save SIM1 while SIM2 stays online. Lack of registration or
data at the next boot is left to netifd; only a failed radio initialization
triggers cold-start fallback. Selecting the already active card through a
live-switch command still requires data before replacing a manually saved
boot choice. An optional offline *live* switch is not implemented.

UCI configuration, diagnostics and legacy migration backups are preserved by
`mu300-update`; retain them in manual reinstall backups. Live switching
interrupts cellular sessions, so use USB management for validation. An unknown
radio after an interrupted operation requires a Linux reboot rather than
reopening SIPC channels.

| Physical card | URC / command / data channel | CID 1 bearer |
| --- | --- | --- |
| SIM1 (0) | nr0 / nr1 / nr2 | sipa_eth0 |
| SIM2 (1) | nr3 / nr4 / nr5 | sipa_eth8 |

## Native cold start

With SIM2 selected, or hot switching enabled for either selected card, S19
starts nr1/nr2/nr4/nr5 owners and nr0/nr3 URC readers. The nr4/nr5 instances
wait (inside the procd instance, bounded at 240 s, never in `start_service`)
for `sbuf_5_006, state: 1,` in `/dev/sipc_sbuf`: a character node exists before
the CP is up and `spipe_open` returns ENODEV until then. The data brokers use
their own group's URC log for liveness. Owner respawn and channel reopening
are disabled: repeated SIPC opens can wedge the channel, and the cold start
and the switch refuse to run without all four owners rather than talk to a
reopened one (a dead owner means no switch until the next boot). The
`sbuf_5_006` seqfile text was checked on Linux 7.2.9 only.

Under the shared radio lock, the controller waits for actual CP output, sends
`AT+SMMSWAP=0` through nr1, and requires both groups to report CFUN=0. It checks
the selected card's CPIN/CCID, then sends through its data broker:

```text
AT+SPTESTMODEM=134,134
AT+SPSWDATA
AT+SFUN=4
```

The two-argument work-mode command matches the observed modemConfig=7 firmware.
`SMMSWAP=1` is not a slot selector. Radio readiness is CFUN=1 for either card,
exactly as the SIM1 path marks it: registration is netifd's to wait for and
retry (a slow registration used to fail a boot-time gate and leave the whole
boot without WAN). An optional private
`/etc/mu300/sim1-iccid` or `sim2-iccid` pins an expected card. Logs omit these
identifiers. The controller runs at most once per boot. Netifd then owns data
attachment and retries on `sipa_eth0` or `sipa_eth8`.

## Live transaction

The sequence follows the factory `VendorTelephonyManager.setProtocolStack`
selector logic. After inhibiting new dial/watch rounds and stopping WAN:

```text
target command broker: AT+SPACTCARD=<target>;+SFUN=2
                       AT+SFUN=4
                       AT+SPACTCARD=0
target data broker:    AT+SPSWDATA
old command broker:    AT+SPACTCARD=<old>;+SFUN=5
                       AT+SFUN=3
                       AT+SPACTCARD=0
```

After enabling the target, verify target CFUN=1, old CFUN=0 and target CPIN
READY. An inactive stack can report `+CME ERROR: 10` for CPIN despite an
inserted card, so CPIN is deliberately checked after enable. Only after these
gates does the controller publish the active slot, clear shared signal caches,
and ask netifd to dial. The boot default is saved once `wan` is up with an
address (`ubus call network.interface.wan status`, 60 s at most); without one
the controller takes WAN down, runs the same sequence back to the previous
card, dials it again and reports `no_data`, the saved default untouched. The
watcher reloads its slot without restarting AT owners or the `mu300-post`
service.

Kernel flock serializes policy edits and switches; the shared mobile-data
radio mutex protects stack operations. RPC checks strict argument allow-lists
and launches the switch outside its 30-second request budget. A whole-operation
300-second deadline (`MU300_SIM_DEADLINE`) covers the switch, the wait for a
bearer, one rollback and two dials. On a command failure, rollback is tried
once only if both command brokers still answer. A successful rollback
preserves the previous boot preference. Silence or failed rollback marks radio
state unknown, inhibits automated radio resets, and requires a Linux reboot.
No blind SFUN reset loops or AT owner reopening are attempted.

## Validation and limits

Validation uses one U30 Air with OpenWrt 25.12.5 and Linux 7.2.9. Both directions
have been tested through the native selector sequence and the CLI. The original
four AT owner PIDs remained unchanged, both RF states matched their selections,
and switching back to SIM2 restored external IPv4 traffic. SIM1 has no working
data service, so RF/card readiness and internet availability are reported
separately. After a CLI switch to SIM1, a full Linux reboot directly enabled SIM1 in about
32 seconds (SIM2 stayed off). The Chinese LuCI page then switched to SIM2,
persisted it, and a second full reboot directly registered SIM2 in about
36 seconds and restored WAN. These are CP cold-initialization tests after
Linux reboots; a physical power-disconnect cycle has not been separately tested.

Tests cover bidirectional switch/default persistence, no-op switches, PIN-lock
rollback, command rejection, silent CP, missing owners, disabled live switching,
concurrent requests, injection rejection, charging boots, cold-start order,
registration failures, and repeated-attempt refusal. A clean image build and
other device/firmware combinations remain unvalidated.

Charging-only boots leave both radios off. Managed dual-SIM boots refuse the
legacy SIM-unaware modem reset, suspend/resume and network-lock restart/replay
paths. Read-only SIM2 dashboard queries share nr4. Voice/USSD and portions of
the IPv6 relay helpers still assume SIM1; use IPv4 WAN until they are adapted.
A modem crash needs a Linux reboot; automatic CP reload is outside this patch.

Upstream PR #72 independently proposes an F50 internal/external SIM selector
with overlapping channel mapping. Configuration and broker interfaces need to
be reconciled before either patch is described as universal dual-SIM support.

## Current review follow-up

The branch retains the maintainer's #166 review fixes and merges main
`c47e2c9`, including #72's shared F50 foundation. The UCI/dispatch integration
has not yet been installed or hardware-validated. Older U30 radio-sequence
results establish the previous sequences, not this new integration. In
particular, the cold-start and live/data-rollback cases still require the
matching scripts on the two-SIM device; F50 regression coverage here is
simulated unless noted as the maintainer's earlier independent test.
