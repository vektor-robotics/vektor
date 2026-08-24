#!/usr/bin/env bash
set -euo pipefail

# Run phase one, reboot the disposable VM, then run phase two. The external VM
# runner retains the packages and invokes this script again after reboot.
phase=${1:?phase one or two required}
old_package=${2:?old package path required}
new_package=${3:?new package path required}
if [ "$phase" = one ]; then
  sudo apt-get install -y "$old_package"
  test -f /lib/systemd/system/vektor-agent.service
  sudo install -d /var/lib/vektor
  sudo touch /var/lib/vektor/.lifecycle-qualified
  exit 0
fi
test "$phase" = two
test -f /var/lib/vektor/.lifecycle-qualified
sudo apt-get install -y "$new_package"
dpkg-query -W vektor
sudo apt-get install -y "$old_package"
dpkg-query -W vektor
sudo apt-get purge -y vektor
test ! -e /lib/systemd/system/vektor-agent.service
test ! -e /var/lib/vektor
test ! -e /var/log/vektor
