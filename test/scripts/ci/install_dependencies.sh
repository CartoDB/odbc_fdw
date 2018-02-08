#!/bin/sh
set -ex

# Needed for the compilation of odbc_fdw.c
apt-get -y install unixodbc-dev

# Install PostgreSQL 9.5 from CARTO launchpad
apt-get -y install postgresql-9.5=9.5.2-3cdb2
apt-get -y install postgresql-server-dev-9.5=9.5.2-3cdb2
/etc/init.d/postgresql stop 9.5
# Install PostgreSQL 9.6
apt-get install postgresql-9.6
apt-get install postgresql-server-dev-9.6
/etc/init.d/postgresql stop 9.6
sed -i 's/port = 5433/port = 5432/g' /etc/postgresql/9.6/main/postgresql.conf

apt-get -y install odbcinst
apt-get -y install odbc-postgresql

# configure it to accept local connections from postgres
echo -e "# TYPE  DATABASE        USER            ADDRESS                 METHOD \nlocal   all             postgres                                trust\nlocal   all             all                                     trust\nhost    all             all             127.0.0.1/32            trust" \
  | sudo tee /etc/postgresql/9.5/main/pg_hba.conf
echo -e "# TYPE  DATABASE        USER            ADDRESS                 METHOD \nlocal   all             postgres                                trust\nlocal   all             all                                     trust\nhost    all             all             127.0.0.1/32            trust" \
  | sudo tee /etc/postgresql/9.6/main/pg_hba.conf

# Local MySQL
apt-get -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" install mysql-server-5.5
apt-get -y install libmyodbc

# Local HIVE installation
wget -P /opt http://apache.rediris.es/hadoop/core/hadoop-2.7.2/hadoop-2.7.2.tar.gz
wget -P /opt http://apache.rediris.es/hive/hive-2.1.1/apache-hive-2.1.1-bin.tar.gz
tar -xzvf /opt/hadoop-2.7.2.tar.gz -C /opt
tar -xzvf /opt/apache-hive-2.1.1-bin.tar.gz -C /opt
# We need the two instances of postgresql in the same port
sed -i -- 's/export JAVA_HOME=${JAVA_HOME}/export JAVA_HOME=\/usr\/lib\/jvm\/java-7-openjdk-amd64/g' $HADOOP_HOME/etc/hadoop/hadoop-env.sh
$HADOOP_HOME/bin/hdfs dfs -mkdir /tmp/warehouse
$HADOOP_HOME/bin/hdfs dfs -chmod g+w /tmp/warehouse
$HADOOP_HOME/bin/hdfs dfs -mkdir /tmp/warehouse/fdw_tests
$HADOOP_HOME/bin/hdfs dfs -chmod g+w /tmp/warehouse/fdw_tests
cp $TRAVIS_BUILD_DIR/test/fixtures/hive_example.csv /tmp/warehouse/fdw_tests

# Install Hiveserver2 driver
apt-get install libsasl2-modules-gssapi-mit
wget -P /tmp http://public-repo-1.hortonworks.com/HDP/hive-odbc/2.1.2.1002/debian/hive-odbc-native_2.1.2.1002-2_amd64.deb
dpkg -i /tmp/hive-odbc-native_2.1.2.1002-2_amd64.deb

# SQL Server
apt-get -y install freetds=1.00.14cdb7

# ODBC installtion ini file
cp $TRAVIS_BUILD_DIR/test/scripts/ci/odbcinst.ini /etc

