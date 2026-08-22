// Firmware update from the SD card, so the one moment an airgapped signer had
// to meet a computer is gone.
//
// The device never trusts a file because of its name. What it trusts is TWO
// signatures over the same bytes, both checked before the new slot is ever made
// bootable -- the secp256r1 one esp_ota verifies against the key inside the
// running firmware, and an SLH-DSA one verified by kiss_pqsig.c -- and then the
// fact that the new firmware boots far enough to say so. Everything below is
// arranged around those gates.
//
// The second signature is there because the first one is an elliptic curve
// signature, and whoever can forge the release key can hand every KISS signer an
// image it installs and trusts. SLH-DSA rests on SHA-256 instead. See
// kiss_pqsig.h; the image carries it as an 8 KB trailer, which the device holds
// back so the bytes reaching the slot are the bytes espsecure signed.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "platform_sd.h"

#define WFW_VER_LEN 32

// How many names the scan reads descriptors for. The card is meant to hold one
// .bin -- G_FW_WHERE_B says so in 21 locales -- and this was 8, chosen as
// "surely enough". It is the window, not the directory: platform_sd_list_firmware
// streams the whole card and keeps the first WFW_SCAN_MAX by name, so anything
// past it is never opened and never judged. Eight files sorting ahead of the
// real image hid it, and if one of those eight was a genuine older release the
// device offered THAT instead -- correctly signed, so nothing downstream could
// object. 24 matches the PSBT list's cap, for the same reason: far past any
// honest card, and the count below says so when it is not enough.
#define WFW_SCAN_MAX 24

enum {
    WFW_OK             =   0,
    WFW_ERR_NO_CARD    =  -1,
    WFW_ERR_NO_FILE    =  -2,   // card is there, no *.bin on it
    WFW_ERR_UNREADABLE =  -3,   // not an app image, or the descriptor is junk
    WFW_ERR_TOO_BIG    =  -4,   // will not fit the receiving slot
    WFW_ERR_OLDER      =  -5,   // downgrade: offered, but behind a warning
    WFW_ERR_SAME       =  -6,   // already running this version
    WFW_ERR_UNSIGNED   =  -7,   // this build cannot verify signatures at all
    WFW_ERR_REJECTED   =  -8,   // signature did not check out
    WFW_ERR_WRITE      =  -9,
    WFW_ERR_CARD_GONE  = -10,
    // The second signature. An image that fails this one passed the secp256r1
    // check -- so it really is a KISS image, correctly signed with the release
    // key -- and was still refused, because the post quantum signature over the
    // same bytes was missing or wrong. Separate from WFW_ERR_REJECTED so the
    // screen can say which lock did not open.
    WFW_ERR_PQ_REJECTED = -11,
};

typedef struct {
    char   name[SD_NAME_LEN];
    char   version[WFW_VER_LEN];
    char   project[WFW_VER_LEN];
    size_t size;        // bytes of image on the card
    size_t slot;        // bytes the receiving slot holds
    int    cmp;         // version vs running: <0 older, 0 same, >0 newer
    int    status;      // WFW_OK when installable, otherwise why not
    int    examined;    // images whose descriptor was actually read
    int    on_card;     // .bin files the card holds: > examined means a window
} wfw_image_t;          // was hit and the answer is about a subset

// What this firmware calls itself. Reads the running app's own descriptor on
// device; the compiled in string in the sim.
const char *kiss_fw_running_version(void);

// 0 when this build can verify an image signature, WFW_ERR_UNSIGNED when it
// cannot. A build made without the release signing key has no key to check
// against, and rather than quietly installing whatever it is handed, the update
// screen says so and refuses. See sdkconfig.defaults.
int kiss_fw_available(void);

// Find the image on the card and judge it. Fills *out even when it returns an
// error, so the screen can name the file it is refusing instead of saying
// nothing was found. Returns out->status.
int kiss_fw_scan(wfw_image_t *out);

// Semver with a prerelease tail, which is what the VERSION file carries
// ("0.1.0-beta7"). Returns <0, 0 or >0 for a older, equal or newer than b.
//
// Split out and exported because it is the one piece of this file that decides
// something on its own and can be tested without a card or a flash chip:
// getting "0.1.0-beta10" vs "0.1.0-beta7" backwards is a silent downgrade
// offered as an upgrade.
int kiss_fw_version_cmp(const char *a, const char *b);

// Pull the version and project name out of an ESP app image's descriptor.
// `hdr` is the first bytes of the image; needs at least WFW_DESC_MIN of them.
// Returns 0, or WFW_ERR_UNREADABLE when the magic is absent.
#define WFW_DESC_MIN 128
int kiss_fw_desc_parse(const uint8_t *hdr, size_t len,
                         char *ver, size_t vn, char *proj, size_t pn);

// Write the image to the slot that is not running. Reports 0..100 as it goes;
// the callback runs on the caller's task, so the UI drives its own redraw.
// Returns WFW_OK, and the caller reboots. Nothing is made bootable unless the
// signature verified, so a failure here always leaves the running slot intact.
typedef void (*wfw_progress_fn)(int pct, void *ud);
int kiss_fw_install(const wfw_image_t *img, wfw_progress_fn cb, void *ud);

// Should this boot confirm the slot? The three things a new image can break
// that a reboot into the previous one would undo: it cannot sign, the panel it
// draws on never answered, or storage would not open. Every one of them
// is fatal to the device as a signer and every one of them can arrive with an
// update, so any of them leaves the slot on trial.
//
// A pure function, and exported, because app_main is the one file no gate on
// this project compiles -- the desktop builds all define SIMULATOR and stop at
// build_game. Left as an `if` up there, the decision that says whether a bad
// image becomes permanent would be the only safety gate on the device with no
// test behind it at all.
bool kiss_fw_confirm_ok(bool sign_ok, bool touch_ok, bool storage_ok);

// Confirm the firmware that is running actually works. Called once the home
// screen is up: anything that reboots before this -- crash, watchdog, a hand on
// the power -- gives the device back to the slot that was working. Harmless to
// call when the running slot was never on trial.
void kiss_fw_mark_valid(void);

// Sim only. A desktop build has no flash and no signing key, so every screen
// past "cannot be checked" is unreachable without this -- and a screen the walk
// cannot reach is a screen no gate has ever measured, in any locale. Same shape
// as platform_sd's test hooks, and there for the same reason.
//
// set_available forces what kiss_fw_available() answers; set_install forces
// what kiss_fw_install() returns, after reporting `steps` progress ticks.
void kiss_fw_test_set_available(int rc);
void kiss_fw_test_set_install(int rc, int steps);
