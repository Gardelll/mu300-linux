// What the owner of the site sets, in one place.
//
// MIRROR_BASE: where this browser can download a release's Magisk zips itself (mirror.js), for a device that has no
// internet of its own. Empty: there is no mirror, and the page offers the device's own download and, last, a file
// the user downloaded. To turn it on, three things (docs/web-installer.md, "The release mirror"):
//   1. a public repository for the mirror, and the release workflow's MIRROR_REPO variable and MIRROR_TOKEN secret,
//      so that every release is published there in parts;
//   2. its address here: "https://raw.githubusercontent.com/OWNER/REPO/" (with the last slash);
//   3. https://raw.githubusercontent.com added to connect-src in the Content-Security-Policy of the three
//      web-installer pages (tests/test_web_installer.py holds the two together).
export const MIRROR_BASE = "";
