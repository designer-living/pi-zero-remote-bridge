#!/bin/sh
set -e

systemctl stop remote-bridge || true
systemctl disable remote-bridge || true
systemctl stop bluetooth-pair-web || true
systemctl disable bluetooth-pair-web || true
