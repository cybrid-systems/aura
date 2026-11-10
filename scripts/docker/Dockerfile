# Issue #4907: aura runtime image (no toolchain). Built from a bundle made by
# scripts/tools/bundle.sh; one image per architecture, merged into a manifest list.
#   docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura:nightly main.aura
FROM ubuntu:24.04
# #5067: the Ubuntu base image labels itself version=24.04. Override that
# with the Aura release version and the git revision. Empty defaults keep
# `docker build` usable outside the release workflow.
ARG AURA_VERSION=dev
ARG AURA_REVISION=unknown
LABEL org.opencontainers.image.title="aura" \
      org.opencontainers.image.version="${AURA_VERSION}" \
      org.opencontainers.image.revision="${AURA_REVISION}" \
      org.opencontainers.image.source="https://github.com/cybrid-systems/aura"
ARG BUNDLE
ADD ${BUNDLE} /opt/
RUN ln -s /opt/aura-*/bin/aura /usr/local/bin/aura && aura -e '(display 1)'
WORKDIR /w
ENTRYPOINT ["aura"]
