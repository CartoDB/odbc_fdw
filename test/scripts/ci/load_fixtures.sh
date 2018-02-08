#!/bin/sh
set -ex

PG_VERSIONS=(9.5 9.6)

# We have to stop and start different versions of postgres because
# the tests are port dependant and we don't want to include complexity
# in there
for i in "${PG_VERSIONS[@]}"
do
  /etc/init.d/postgresql start $i
  bash $TRAVIS_BUILD_DIR/test/scripts/load_all_fixtures.sh
  PGVERSION=$i make clean && sudo PGVERSION=$i make install
  /etc/init.d/postgresql stop $i
done
