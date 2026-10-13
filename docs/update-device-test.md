# An update keeps everything: the test on a device

What docs/FINDINGS.md 85 measured in containers, to be run on an F50 or a U30 Air before the release that brings it
is promoted. Nothing here has run on a device yet. The device should have Ubuntu and one OpenWrt installed (the
OpenWrt checks are driven from Ubuntu: OpenWrt has no Python), adb to Linux reachable (it is on by default), and
Android reachable with `adb` and root for the paths that start there.

The device ends each part with its systems updated; `mu300-update rollback` (part 2) or the installer's `wipe` is
the way back. Take the backup of `install.sh --check` first if the device matters.

## 0. What is needed

* The release under test, built from the branch: `tools/make-release.sh vTEST` (no `--publish`) gives `release/vTEST`
  with the four images, the kernel bundles, `mu300-update`, the manifests and `SHA256SUMS`. Its audit must have
  passed: each image has `opt/mu300/bin/mu300-carry`, its manifest and the manifests of the older releases.
  The Magisk zips of it: `tools/make-magisk-zips.sh release/vTEST out vTEST`.
* On the computer, in `release/vTEST`: `python3 -m http.server 8000` (the device reaches it over USB at the
  computer's address on the device's LAN, 192.168.77.x for an F50; `PC` below).
* On the device, a directory beside the systems that no update touches:

  ```sh
  sudo mkdir -p /mnt/mu300-disk/update-test
  # from the computer: the experiment's script
  scp tests/update-experiment/experiment.py ubuntu@192.168.77.1:/tmp/ && ssh ubuntu@192.168.77.1 sudo mv /tmp/experiment.py /mnt/mu300-disk/update-test/
  ```

## 1. Plant, update on the device, compare

From Ubuntu, as root (`sudo -i`), `T=/mnt/mu300-disk/update-test`:

```sh
# what the device is now, for the record
mu300-update version | tee $T/version.before
# real packages, so that the reinstall at the first boot has something to do
apt-get install -y hello tree
# the user state: some sixty kinds of it, written down with every attribute. --live leaves the login alone
# (no password is changed, no package is faked)
python3 $T/experiment.py plant / ubuntu $T/ubuntu.json --live
python3 $T/experiment.py plant /mnt/mu300-disk/openwrt openwrt $T/openwrt.json --live --no-mount     # (or openwrt-luci)
# the other things on the partition, which an update of one system must not touch
find /mnt/mu300-disk/extra -type f -exec sha256sum {} + | sort -k2 > $T/extra.before
df -m /mnt/mu300-disk | tee $T/df.before

# the update, timed; its output has the report
time MU300_RELEASE=vTEST MU300_RELEASE_URL=http://PC:8000 mu300-update apply 2>&1 | tee $T/apply.log
```

Expected in the output: `continuing with the mu300-update of vTEST`, for each system `carrying your files over`, the
report (`==== ubuntu: vOLD -> vTEST ...`, `Everything you added to the system was carried over: N files, M MB`), the
lists of what was set aside (the edited `/usr/lib/systemd/system/ssh.service` and `/www/index.html`) and nothing
else unexpected, `boot: updated to vTEST`. Write down the seconds between `carrying your files over` and
`switching over` for each system (not measured on a device yet: the scan and the hashing of the old system).

Before the reboot:

```sh
# the copy kept for a rollback is the old system, to the last attribute: every line must say "kept"
python3 $T/experiment.py verify /mnt/mu300-disk/ubuntu.old $T/ubuntu.json | cut -f1,2,4 | grep -v '	kept	'
python3 $T/experiment.py verify /mnt/mu300-disk/openwrt.old $T/openwrt.json | cut -f1,2,4 | grep -v '	kept	'
# the new systems, before they ran
python3 $T/experiment.py verify /mnt/mu300-disk/ubuntu $T/ubuntu.json > $T/ubuntu.after.tsv
python3 $T/experiment.py verify /mnt/mu300-disk/openwrt $T/openwrt.json > $T/openwrt.after.tsv
cut -f2 $T/ubuntu.after.tsv $T/openwrt.after.tsv | sort | uniq -c       # no "lost"; "changed" only with "in the report"
reboot
```

After the reboot, in Ubuntu:

```sh
T=/mnt/mu300-disk/update-test
mu300-update version; mu300-update report | head -40          # the login showed the note once
python3 $T/experiment.py verify / $T/ubuntu.json | cut -f1,2,4 | grep -v '	kept	'
systemctl --failed; journalctl -b -p err --no-pager | tail -30
mu300-user-packages status                                     # hello and tree came back once online
id kaan; id myapp; ls -ln /var/lib/myapp                       # the accounts, and what they own
mobile-data status; wifi-client status 2>/dev/null; ip -br a   # the device still does what it is for
find /mnt/mu300-disk/extra -type f -exec sha256sum {} + | sort -k2 | diff - $T/extra.before   # (the VPN module may have been updated: that is apply's job)
```

Then start OpenWrt (`sudo mu300-os openwrt`, reboot), log in, and look: `mu300-update report`, `uci show network |
head`, `logread | grep -i -e error -e fail | tail`, the planted `/etc/crontabs/root`, `/www/mysite/index.html`,
`/etc/init.d/myservice` with its `/etc/rc.d/S99myservice`, the disabled service still disabled, the modem and
Wi-Fi up. Back to Ubuntu: `mu300-os ubuntu`, reboot.

What may differ from the container runs, and must be written into FINDINGS 85 when it does: lines under `Back,
though you had deleted them` or `Deleted again` that nobody deleted (files the system itself removes at its first
boot - none are known besides uci-defaults, which are left alone), and files under `Yours is in use where the new
release has another version` that the user never touched (files the system rewrites at run time: expected for
`/etc/config/*` on OpenWrt, to be looked at for anything else).

## 2. Rollback

```sh
sudo mu300-update rollback && sudo mu300-update rollback-boot && sudo reboot
# after the reboot: exactly the system as it was when it was updated
python3 $T/experiment.py verify / $T/ubuntu.json | cut -f2 | sort | uniq -c       # all "kept"
```

## 3. No room, and --no-rollback

```sh
# a file of the user's that does not fit twice (free space less than its size)
df -m /mnt/mu300-disk; fallocate -l 3G /srv/big.img
MU300_RELEASE=vTEST MU300_RELEASE_URL=http://PC:8000 mu300-update apply ubuntu
#   expected: "not enough room to carry your files over beside the previous system: N MB needed, M MB free",
#   "nothing was changed", the hint at --no-rollback; ls /mnt/mu300-disk shows no ubuntu.new and no ubuntu.old
MU300_RELEASE=vTEST MU300_RELEASE_URL=http://PC:8000 mu300-update apply --no-rollback ubuntu
#   expected: the update goes through in about the time of the unpacking; /mnt/mu300-disk/ubuntu.broken is the old
#   system until the next apply or clean; mu300-update rollback says "no previous version kept"
reboot      # then: ls -l /srv/big.img, the verify of part 1, and "sudo mu300-update clean" removes ubuntu.broken
```

## 4. The computer installer's update

From the state of part 2 (the old release again, planted), with Android booted (`sudo mu300-next-boot android &&
sudo reboot`, or the panel's switch) and the release under test where `install.sh` finds it
(`MU300_RELEASE_URL=http://127.0.0.1:8000 ./install.sh`): answer **update**. In the device
script's lines (`[device] ...`): `carrying everything of yours in the previous ubuntu over`, the report, `kept the
accounts and passwords` (or the line about the password that was typed). After the reboot into Linux the verify of
part 1. There is no `<os>.old` on this path.

## 5. The Magisk zip, from Android

With the old release installed and planted again (part 2, or install the old release's zip and plant), in Android:

```sh
adb push out/mu300-magisk-vTEST-ubuntu-24.04-k6.18.zip /sdcard/Download/
# first only the plan: nothing is written
adb shell 'echo MU300_DRY_RUN=1 > /sdcard/mu300-install.conf'
adb shell su -c 'magisk --install-module /sdcard/Download/mu300-magisk-vTEST-ubuntu-24.04-k6.18.zip'
#   expected in the plan: "filesystem: keep: nothing is erased", "update: ubuntu vOLD -> vTEST: everything you added
#   to it or changed in it is kept ...", "not touched: openwrt and the extras ...", "password: kept ..."
adb shell rm /sdcard/mu300-install.conf
# offline: aeroplane mode on, or the SIM out and Wi-Fi off. Then the installation itself
adb shell su -c 'magisk --install-module /sdcard/Download/mu300-magisk-vTEST-ubuntu-24.04-k6.18.zip' | tee zip.log
adb reboot
```

After the reboot into Linux: the verify of part 1 for Ubuntu (zero lost, the same lines set aside), the old
password still logs in, `mu300-update report`, and for the system the zip did not carry:

```sh
python3 $T/experiment.py verify /mnt/mu300-disk/openwrt $T/openwrt.json | cut -f2 | sort | uniq -c    # all "kept": it was not touched
cat /mnt/mu300-disk/openwrt/etc/mu300/image-version                                                  # still vOLD
sudo mu300-os openwrt && sudo reboot      # and it boots, with the zip's kernel (its modules were installed into it)
```

Also to be seen once each:

* an older zip over the updated system (`mu300-magisk-vOLD-ubuntu-24.04-k6.18.zip`): refused with `The installed
  ubuntu is vTEST, and this zip is vOLD, an older release`, nothing written;
* `MU300_UPDATE_KEEP=list` in `/data/adb/mu300-install.conf` (mode 600): the plan says `only settings, users and home
  directories are kept`, and the line is a comment afterwards;
* the installation cut off while it says `[device] installing ubuntu` (a second `adb shell su -c 'pkill -f
  android-install.sh'`), then started again: `an earlier installation was cut off; the system it was replacing is
  back in place` when it had got that far, and in every case the planted files are there at the end.

## 6. What to bring back

`$T/apply.log`, `zip.log`, the `*.after.tsv` files, `/etc/mu300/update-report` of each system, the seconds of the
carry step, `df -m` before and after, and every line of the reports that this page did not expect.
