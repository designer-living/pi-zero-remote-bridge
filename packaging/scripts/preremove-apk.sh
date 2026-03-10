#!/bin/sh
set -e

rc-service remote-bridge stop || true
rc-update del remote-bridge default || true
