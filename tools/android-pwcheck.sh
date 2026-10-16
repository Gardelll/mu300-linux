#!/system/bin/sh
# The password of a system's own account (Ubuntu's ubuntu, OpenWrt's root) on the Linux filesystem, looked at on the
# device: for "keep the current password" of an update (install.sh, install.ps1 and the Magisk zip). It prints one
# word, and never a password or a hash:
#   android-pwcheck.sh -s ROOT USER   SET, EMPTY (no password: root logs in without one), LOCKED (! or *), NOUSER
#   android-pwcheck.sh ROOT USER      is the first line of standard input (an empty line: the empty password) USER's
#                                     password? MATCH, NOMATCH, LOCKED, NOUSER or UNVERIFIABLE (nothing here computes
#                                     that hash's method). Exit status 0 only with MATCH.
# ROOT is one system's directory on the mounted Linux filesystem (.../ubuntu, .../openwrt). The hash is computed with
# the hash's own method and salt by the system's own tools, run in it with chroot (Android runs on the same
# architecture): perl's crypt() on Ubuntu (libxcrypt: yescrypt, which Ubuntu's passwd writes, SHA-crypt, MD5-crypt,
# bcrypt), openssl passwd on OpenWrt (MD5-, SHA-256- and SHA-512-crypt: what its passwd and LuCI write).
# MU300_PWCHECK_BB=<a busybox with mkpasswd> (the Magisk zip's) does MD5- and SHA-crypt first, without chroot.
# MU300_CHROOT: the chroot to use (tests).
set -u
CHROOT=${MU300_CHROOT:-chroot}
STATE=0
if [ "${1:-}" = -s ]; then STATE=1; shift; fi
[ $# = 2 ] || { echo "usage: android-pwcheck.sh [-s] ROOT USER" >&2; exit 2; }
ROOT=$1 U=$2
case $U in ''|*[!a-z0-9_-]*) echo NOUSER; exit 1 ;; esac
[ -f "$ROOT/etc/shadow" ] && grep -q "^$U:" "$ROOT/etc/shadow" 2>/dev/null || { echo NOUSER; exit 1; }
h=$(sed -n "s/^$U:\([^:]*\):.*/\1/p" "$ROOT/etc/shadow" | head -n1)
if [ $STATE = 1 ]; then
    case $h in '') echo EMPTY ;; '!'*|'*'*) echo LOCKED ;; *) echo SET ;; esac
    exit 0
fi
IFS= read -r pw || [ -n "${pw:-}" ] || pw=
case $h in
    '') if [ -z "$pw" ]; then echo MATCH; exit 0; fi; echo NOMATCH; exit 1 ;;
    '!'*|'*'*) echo LOCKED; exit 1 ;;
esac
# no password typed, and the account has one: passwd and LuCI never write a hash of the empty password (no password
# is an empty field, above)
if [ -z "$pw" ]; then h=; echo NOMATCH; exit 1; fi
got=
case $h in
    '$1$'*|'$5$'*|'$6$'*)
        case $h in '$1$'*) m=md5 n=1 ;; '$5$'*) m=sha256 n=5 ;; *) m=sha512 n=6 ;; esac
        s=${h#\$?\$}; s=${s%%\$*}
        case $s in
            rounds=*|'') ;;     # neither busybox's mkpasswd nor openssl passwd take rounds: perl below
            *)
                if [ -n "${MU300_PWCHECK_BB:-}" ] && [ -n "$pw" ]; then
                    got=$(printf '%s\n' "$pw" | "$MU300_PWCHECK_BB" mkpasswd -m $m -S "$s" -P 0 2>/dev/null) || got=
                fi
                if [ -z "$got" ] && [ -x "$ROOT/usr/bin/openssl" ]; then
                    got=$(printf '%s\n' "$pw" | env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "$CHROOT" "$ROOT" /usr/bin/openssl passwd -$n -salt "$s" -stdin 2>/dev/null) || got=
                fi ;;
        esac ;;
esac
if [ -z "$got" ] && [ -x "$ROOT/usr/bin/perl" ]; then
    # the hash goes in the environment, the password on standard input: neither is on a command line
    got=$(printf '%s\n' "$pw" | env -i PATH=/usr/bin:/bin MU300_H="$h" "$CHROOT" "$ROOT" /usr/bin/perl -e \
        'my $p = <STDIN>; $p = "" unless defined $p; chomp $p; my $c = crypt($p, $ENV{MU300_H}); print $c if defined $c' 2>/dev/null) || got=
fi
pw=
if [ -z "$got" ]; then h=; echo UNVERIFIABLE; exit 1; fi
if [ "$got" = "$h" ]; then h=; echo MATCH; exit 0; fi
h=; echo NOMATCH; exit 1
