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
    : > "$1/UNSIGNED" 2>/dev/null && return 0
    docker run --rm -v "$PWD":/project -w /project "$KISS_IDF_IMAGE" \
        touch "/project/$1/UNSIGNED"
}
