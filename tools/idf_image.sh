# The device compiler, named by DIGEST rather than by the v6.0.1 tag.
# Sourced by both release lanes; not executable on its own.
#
# A tag is mutable. Espressif can repush espressif/idf:v6.0.1 whenever they
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
KISS_IDF_IMAGE=espressif/idf@sha256:efc19fae2f52fc6873630c668da26aa834139a063b5fb73a46ebc0dbd217b587  # v6.0.1
