# Issue #4907: aura runtime image (no toolchain). Built from a bundle made by
# scripts/tools/bundle.sh; one image per architecture, merged into a manifest list.
#   docker run --rm -v "$PWD:/w" -w /w ghcr.io/cybrid-systems/aura main.aura
FROM ubuntu:24.04
ARG BUNDLE
ADD ${BUNDLE} /opt/
RUN ln -s /opt/aura-*/bin/aura /usr/local/bin/aura && aura -e '(display 1)'
WORKDIR /w
ENTRYPOINT ["aura"]
