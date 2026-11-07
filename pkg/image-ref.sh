#!/bin/sh
# Full reference of the build image for this checkout:
#   sh pkg/image-ref.sh <owner>   ->   ghcr.io/<owner lowercase>/moonlight-tizen-build:deps-<hash>
owner=${1:?usage: image-ref.sh <owner>}
echo "ghcr.io/$(echo "$owner" | tr 'A-Z' 'a-z')/moonlight-tizen-build:$(sh "$(dirname "$0")/image-tag.sh")"
