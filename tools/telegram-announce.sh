#!/bin/sh
# Announce a release on the project's Telegram channel (.github/workflows/telegram.yml and magisk.yml).
#   tools/telegram-announce.sh release TAG [published|released]   a release published (a prerelease too) or promoted
#   tools/telegram-announce.sh zips TAG                            its Magisk zips attached
# Needs TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID (repository secrets), gh and jq. The release's own notes are the
# static asset table, so the message is short: what the release is, the "New in" list of the wiki Home at that tag
# (the first lines, without the links), and where to read on. Nothing from the environment but the two secrets
# goes out, and the token never reaches a log: curl takes the URL from a file.
set -eu
MODE=${1:-}; TAG=${2:-}; ACTION=${3:-published}
REPO=${GITHUB_REPOSITORY:-dikeckaan/mu300-linux}
[ -n "$TAG" ] || { echo "usage: $0 release|zips TAG [published|released]" >&2; exit 2; }
case "$TAG" in v[0-9]*) ;; *) echo "not a release tag: $TAG" >&2; exit 2 ;; esac
case "$TAG" in *[!0-9A-Za-z._-]*|*..*) echo "not a release tag: $TAG" >&2; exit 2 ;; esac
: "${TELEGRAM_BOT_TOKEN:?}" "${TELEGRAM_CHAT_ID:?}"

esc() { sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g'; }
URL=https://github.com/$REPO/releases/tag/$TAG
rel=$(gh release view "$TAG" -R "$REPO" --json name,isPrerelease,publishedAt,assets)
pre=$(printf '%s' "$rel" | jq -r .isPrerelease)
nassets=$(printf '%s' "$rel" | jq -r '.assets|length')
nzips=$(printf '%s' "$rel" | jq -r '[.assets[].name|select(endswith(".zip"))]|length')

case "$MODE" in
release)
    # "released" also fires for a release published as a release straight away, right after "published": one
    # message then, not two. A promotion comes later: the release has been there for a while.
    if [ "$ACTION" = released ]; then
        age=$(( $(date +%s) - $(date -d "$(printf '%s' "$rel" | jq -r .publishedAt)" +%s 2>/dev/null || echo 0) ))
        [ "$age" -gt 600 ] || { echo "published just now: the 'published' event announces it"; exit 0; }
        head="✅ <b>MU300 Linux $(printf '%s' "$TAG" | esc)</b> is now the <b>latest release</b> — artık <b>son sürüm</b>."
        body="Tested on the boards; <code>mu300-update apply</code> on a device takes it, the installer and the Magisk zips ship it.
Cihazda <code>mu300-update apply</code> ile gelir; kurulum aracı ve Magisk zip'leri de bu sürümü kurar."
    elif [ "$pre" = true ]; then
        head="🧪 <b>MU300 Linux $(printf '%s' "$TAG" | esc)</b> — pre-release / ön sürüm"
        body="Being tested on the boards; becomes the latest release when it passes. <code>MU300_RELEASE=$(printf '%s' "$TAG" | esc) mu300-update apply</code> tries it now.
Cihazlarda deneniyor; geçerse son sürüm olur."
    else
        head="🚀 <b>MU300 Linux $(printf '%s' "$TAG" | esc)</b> — new release / yeni sürüm"
        body="<code>mu300-update apply</code> on a device takes it. Cihazda <code>mu300-update apply</code> ile gelir."
    fi
    # the "New in TAG" list of the wiki Home on main (the docs usually land after the tag): one line per bullet
    # (wrapped lines joined), bold kept, links to the wiki's own pages dropped, other links reduced to their text
    new=$(gh api "repos/$REPO/contents/wiki/Home.md" --jq .content 2>/dev/null | base64 -d 2>/dev/null |
        awk -v t="$TAG" '
            $0 ~ "^## New in " t { on=1; next }
            on && /^## / { exit }
            on && /^\* / { if (b != "") print b; b = $0; next }
            on && /^  +[^ ]/ && b != "" { sub(/^ +/, " "); b = b $0 }
            END { if (b != "") print b }' |
        sed -E 's/ *\[([^]]*)\]\([A-Za-z0-9_-]+\)//g; s/\[([^]]*)\]\([^)]*\)/\1/g; s/`//g; s/ +$//' | esc |
        sed -E 's/\*\*([^*]+)\*\*/<b>\1<\/b>/g; s/^\* /• /' | head -n 8) || new=
    text="$head
$body"
    [ -z "$new" ] || text="$text

<b>New in this release / Bu sürümde yeni:</b>
$new"
    text="$text

$nassets files: images, kernels 5.4 / 6.18 / 7.2, mu300-update. $URL
Wiki: https://github.com/$REPO/wiki/Home"
    ;;
zips)
    text="📦 <b>Magisk zips for MU300 Linux $(printf '%s' "$TAG" | esc)</b> are attached — Magisk zip'leri hazır: $nzips files (Ubuntu 24.04 / 26.04, OpenWrt, OpenWrt + panel; kernels 5.4 / 6.18 / 7.2).
Install from Magisk > Modules on the device, no computer needed. Cihazda Magisk > Modüller'den, bilgisayarsız.
$URL"
    ;;
*) echo "usage: $0 release|zips TAG [published|released]" >&2; exit 2 ;;
esac

# MU300_TG_DRY=1: print the message instead of sending it (a look at the text, tests)
[ "${MU300_TG_DRY:-}" != 1 ] || { printf '%s\n' "$text"; exit 0; }
# the token stays out of the command line and the log: curl reads the URL from a config file
cfg=$(mktemp); trap 'rm -f "$cfg"' EXIT
printf 'url = "https://api.telegram.org/bot%s/sendMessage"\n' "$TELEGRAM_BOT_TOKEN" > "$cfg"
out=$(jq -n --arg c "$TELEGRAM_CHAT_ID" --arg t "$text" \
        '{chat_id: $c, text: $t, parse_mode: "HTML", disable_web_page_preview: true}' |
      curl -sS -K "$cfg" -H 'Content-Type: application/json' --data-binary @-)
printf '%s' "$out" | jq -e .ok >/dev/null || { echo "telegram: $(printf '%s' "$out" | jq -r '.description // "no answer"')" >&2; exit 1; }
echo "telegram: sent ($MODE $TAG $ACTION)"
