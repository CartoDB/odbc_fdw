#!/bin/bash

BASEDIR=$(readlink -f $0 | xargs dirname)

load_config()
{
    . "$BASEDIR/../config/$1"
}

load_hive_fixtures()
{
    load_config "hive.config"
    $HIVE_HOME/bin/beeline -u jdbc:hive2://${host}:${port} -f "$BASEDIR/../fixtures/hive_fixtures.sql"
}

load_postgres_fixtures()
{
    load_config "postgres.config"
    createdb -h $host -p $port -U $user -w $dbname
    psql -h $host -p $port -U $user -d $dbname -f "$BASEDIR/../fixtures/postgres_fixtures.sql"
}

load_mysql_fixtures()
{
    load_config "mysql.config"
    echo "create database if not exists fdw_tests" | mysql -u $user
    mysql -u $user -D $dbname < "$BASEDIR/../fixtures/mysql_fixtures.sql"
}

load_sqlserver_fixtures()
{
    load_config "sqlserver.config"
    tsql -S $host -U $user -P $password < "$BASEDIR/../fixtures/sqlserver_fixtures.sql"
}

load_all()
{
    BASEDIR=$(readlink -f $0 | xargs dirname)
    load_hive_fixtures $BASEDIR
    load_postgres_fixtures $BASEDIR
    load_mysql_fixtures $BASEDIR
    load_sqlserver_fixtures $BASEDIR
}

load_all
