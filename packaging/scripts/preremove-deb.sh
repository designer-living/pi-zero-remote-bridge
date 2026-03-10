#!/bin/sh
set -e

systemctl stop remote-bridge || true
systemctl disable remote-bridge || true
