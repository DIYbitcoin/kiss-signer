# The device compiler, named by DIGEST rather than by the v6.1 tag.
# Sourced by both release lanes; not executable on its own.
#
# A tag is mutable. Espressif can repush espressif/idf:v6.1 whenever they
# like, and then someone rebuilding this commit a year from now gets different
# bytes than the hashes published beside the release, with nothing in the repo
# to say which half moved. The reproducible-build workflow rests entirely on
# the toolchain being the same toolchain, so it is named the only way that can
# be checked.
#
# This is the multi-arch INDEX digest, so it resolves on an amd64 CI runner and
# on an arm64 Mac alike -- the two machines that actually build releases here.
#
# Moving to a new IDF: read the new index digest with
#   docker buildx imagetools inspect espressif/idf:<tag>
# or, without buildx, from the Docker-Content-Digest header of a registry
# manifest request, and change the comment and the digest together. Do not
# take it from `docker image inspect` on a fresh pull of a single arch unless
# it reports the index digest, which is what RepoDigests holds.
KISS_IDF_IMAGE=espressif/idf@sha256:81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c  # v6.1

# Both release lanes drop a marker beside the image when the build is unsigned,
# and both were writing it with a plain redirect. On a Linux runner that is
# "Permission denied": the IDF image runs as root, a bind mount keeps that uid,
# and the build directory the container just created does not belong to the
# user holding the shell. Docker Desktop maps ownership to the caller, so it
# worked on the machine both scripts were written on and failed on CI.
#
# One helper in the shared file rather than the same fallback pasted twice --
# the encrypted lane was fixed on its own first and the plain lane failed the
# next run with the identical line.
kiss_mark_unsigned() {
    # The 2>/dev/null has to wrap the whole group: redirections are set up
    # left to right, so on the bare form the failing > runs first and its
    # "Permission denied" still reaches the log, reading like the error
    # that killed the build when the fallback below went on to succeed.
    { : > "$1/UNSIGNED"; } 2>/dev/null && return 0
    docker run --rm -v "$PWD":/project -w /project "$KISS_IDF_IMAGE" \
        touch "/project/$1/UNSIGNED"
}

# The encrypted recipe's UPDATE lane builds a bootloader it must not ship, and
# leaving it in the build directory is the one way that lane could hurt a
# board: it is unsigned, and it sits at the offset a hand-typed write-flash
# would put it. A fresh board burned with a bootloader carrying fewer than
# three key digests revokes the slots it did not fill on first boot and can
# never be rotated again, which is exactly the failure the whole three-key
# root exists to avoid. So the file goes, and a note takes its place saying
# why, for whoever opens the directory expecting the four files the burn
# recipe lists.
#
# Same root-ownership fallback as the marker above, and for the same reason:
# on CI the build directory belongs to the container's root, so rm and the
# note both have to be done from inside the image.
kiss_drop_update_bootloader() {
    local d="$1"
    local note
    note=$(cat <<'EOF'
There is no bootloader here, on purpose.

This build came out of the UPDATE lane of tools/build_encrypted_release.sh,
which signs the app alone with the one secure boot key the fleet is running
on. The other two keys of the root stay offline, so this machine cannot sign a
bootloader with all three -- and a bootloader signed with fewer than three
would burn a board that can never rotate its keys. It was deleted rather than
left here to be flashed by hand.

A burned board never takes a new bootloader anyway: its ROM only runs the one
whose digests it burned on first boot. What it takes is the app this lane
produced, from an SD card.

To burn a NEW board you need the whole root and the other lane:
    bash tools/build_encrypted_release.sh
EOF
)
    if rm -f "$d/bootloader/bootloader.bin" 2>/dev/null \
       && printf '%s\n' "$note" > "$d/bootloader/WHY-NO-BOOTLOADER.txt" 2>/dev/null
    then
        return 0
    fi
    printf '%s\n' "$note" | docker run --rm -i \
        -v "$PWD":/project -w /project "$KISS_IDF_IMAGE" \
        sh -c "rm -f '/project/$d/bootloader/bootloader.bin' && \
               cat > '/project/$d/bootloader/WHY-NO-BOOTLOADER.txt'"
}
