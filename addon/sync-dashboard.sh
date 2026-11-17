#!/bin/sh
# The add-on image is built from this folder only, so it carries its own copy of
# the dashboard. Run after changing esphome/components/tigo_rs485/dashboard/index.html.
set -e
cd "$(dirname "$0")"
mkdir -p www
cp ../esphome/components/tigo_rs485/dashboard/index.html www/index.html
echo "www/index.html updated"
