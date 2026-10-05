#!/bin/sh
# Started at power-up by auris.service. Edit OPTIONS, then:
#   sudo systemctl restart auris
OPTIONS="--array cmb --output display --rotation 90 --pan 3 --tilt 3 --diffuse 1"

cd "$(dirname "$0")/.." || exit 1
# The ADCs lose their settings at power-off; the imager hears nothing without this.
./build-release/apps/cmb_adc_setup >/dev/null || exit 1
# shellcheck disable=SC2086
exec ./build-release/apps/imager_scope $OPTIONS
