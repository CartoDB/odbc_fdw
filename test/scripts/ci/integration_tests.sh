#!/bin/sh
set -ex

PG_VERSIONS=(9.5 9.6)

# Start and stop postgres is done because the instances share the same port
for i in "${PG_VERSIONS[@]}"
do
  /etc/init.d/postgresql start $i
  PGVERSION=$i make integration_tests || { cat test/regression.diffs; false; }
  /etc/init.d/postgresql stop $i
done
