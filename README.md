ODBC FDW for PostgreSQL 9.5+ 
============================
[![Travis Build Status](https://travis-ci.org/CartoDB/odbc_fdw.svg?branch=master)](https://travis-ci.org/CartoDB/odbc_fdw)
[![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/mlt/odbc_fdw?branch=appveyor&svg=true)](https://ci.appveyor.com/project/PurpleI2P/odbc-fdw "Get your fresh Windows build here!")

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for
remote databases using Open Database Connectivity [ODBC](http://msdn.microsoft.com/en-us/library/ms714562(v=VS.85).aspx).

This was originally developed by Zheng Yang in 2011,
with contributions by Gunnar "Nick" Bluth from 2014
and further developed by CARTO since 2016.

While we don’t provide direct technical support to Open Source
installations, it is possible to engage in technical conversations
with the community and part of the CARTO team (including some team
members like Solutions, Support, Backend, and Frontend engineers) in
our [Google Groups
forum](https://groups.google.com/forum/#!forum/cartodb) and [GIS Stack
Exchange](https://gis.stackexchange.com/questions/tagged/carto).

Requirements
------------

To compile and install this extension, assuming a Linux OS,
the libraries and header files for ODBC and PostgreSQL are needed,
e.g. in Ubuntu this can be provided by the `unixodbc-dev`
and `postgresql-server-dev-9.5` system packages.

To make use of the extension ODBC drivers for the data sources to
be used must be installed in the system and reflected
in the `/etc/odbcinst.ini` file.

Driver requirements
--------------------

- odbc-postgresql: >= 9.x
- libmyodbc: >=  5.1
- FreeTDS: >= 1.0
- hive-odbc-native: >= 2.1

Building and Installing
-----------------------

The extension can be built and installed with:

```sh
make
sudo make install
```

Usage
-----

The `OPTION` clause of the `CREATE SERVER`, `CREATE FOREIGN TABLE`
and  `IMPORT FOREIGN SCHEMA` commands is used to define both
the ODBC attributes to define a connection to an ODBC data source
and some additional parameters to specify the table or query that
will be accessed as a foreign table.

The following options to define ODBC attributes should be defined in
the server definition (`CREATE SERVER`).

option   | description
-------- | -----------
`dsn`    | The Database Source Name of the foreign database system you're connecting to.
`driver` | The name of the ODBC driver to use (needed if no dsn is used)

Any other ODBC connection attribute is driver-dependent, and should be defined by
an option named as the attribute prepended by the prefix `odbc_`.
For example `odbc_server`,   `odbc_port`, `odbc_uid`, `odbc_pwd`, etc.

The DSN and Driver can also be defined by the prefixed options
`odbc_DSN`  and `odbc_DRIVER` repectively.

The odbc_ prefixed options can be defined either in the server, user mapping
or foreign table statements.

If the ODBC driver requires case-sensitive attribute names, the
`odbc_` option names will have to be quoted with double quotes (`""`),
for example `OPTIONS ( "odbc_SERVER" '127.0.0.1' )`.
Attributes `DSN`, `DRIVER`, `UID` and `PWD` are automatically uppercased
and don't need quoting.

If an ODBC attribute value contains special characters such as `=` or `;`
it will require quoting with curly braces (`{}`), for example:
for example `OPTIONS ( "odbc_PWD" '{xyz=abc}' )`.

odbc_ option names may need to be quoted with "" if the driver
requires case-sensitive names (otherwise the names are passed as lowercase,
except for UID & PWD)
odbc_ option values may need to be quoted with {} if they contain
characters such as =; ...
(but PG driver doesn't seem to support them)
(the driver name and DNS should always support this quoting, since they aren't
handled by the driver)


Usually you'll want to define authentication-related attributes
in a `CREATE USER MAPPING` statement, so that they are determined by
the connected PostgreSQL role, but that's not a requirement: any attribute
can be define in any of the statements; when a foreign table is access
the SERVER, USER MAPPING and FOREIGN TABLE options will be combined
to produce an ODBC connection string.

The next options are used to define the table or query to connect a
foreign table to. They should be defined either in `CREATE FOREIGN TABLE`
or `IMPORT FOREIGN SCHEMA` statements:

option     | description
---------- | -----------
`schema`   | The schema of the database to query.
`table`    | The name of the table to query. Also the name of the foreign table to create in the case of queries.
`sql_query`| Optional: User defined SQL statement for querying the foreign table(s). This overrides the `table` parameters. This should use the syntax of ODBC driver used.
`sql_count`| Optional: User defined SQL statement for counting number of records in the foreign table(s). This should use the syntax of ODBC driver used.
`prefix`   | For IMPORT FOREIGN SCHEMA: a prefix for foreign table names. This can be used to prepend a prefix to the names of tables imported from an external database.

Note that if the `prefix` option is used and only one specific foreign table is to be imported,
the `table` option is necessary (to specify the unprefixed, remote table name). In this case
it is better not to include a `LIMIT TO` clause (otherwise it has to reference the *prefixed* table name).

Example
-------

Assuming that the `odbc_fdw` is installed and available
in your database (`CREATE EXTENSION odbc_fdw`), and that
you have a DNS `test` defined for some ODBC datasource which
has a table named `dblist` in a schema named `test`:

```sql
CREATE SERVER odbc_server
  FOREIGN DATA WRAPPER odbc_fdw
  OPTIONS (dsn 'test');

CREATE FOREIGN TABLE
  odbc_table (
    id integer,
    name varchar(255),
    desc text,
    users float4,
    createdtime timestamp
  )
  SERVER odbc_server
  OPTIONS (
    odbc_DATABASE 'myplace',
    schema 'test',
    sql_query 'select description,id,name,created_datetime,sd,users from `test`.`dblist`',
    sql_count 'select count(id) from `test`.`dblist`'
  );

CREATE USER MAPPING FOR postgres
  SERVER odbc_server
  OPTIONS (odbc_UID 'root', odbc_PWD '');
```

Note that no DSN is required; we can define connection attributes,
including the name of the ODBC driver, individually:

```sql
CREATE SERVER odbc_server
  FOREIGN DATA WRAPPER odbc_fdw
  OPTIONS (
    odbc_DRIVER 'MySQL',
	odbc_SERVER '192.168.1.17',
	encoding 'iso88591'
  );
```

The need to know about the columns of the table(s) to be queried
ad its types can be obviated by using the `IMPORT FOREIGN SCHEMA`
statement. By using the same OPTIONS as for `CREATE FOREIGN TABLE`
we can import as a foreign table the results of an arbitrary
query performed through the ODBC driver:

```sql
IMPORT FOREIGN SCHEMA test
  FROM SERVER odbc_server
  INTO public
  OPTIONS (
    odbc_DATABASE 'myplace',
    table 'odbc_table', -- this will be the name of the created foreign table
    sql_query 'select description,id,name,created_datetime,sd,users from `test`.`dblist`'
  );
```

LIMITATIONS
-----------

* Column, schema, table names should not be longer than the limit stablished by
  PostgreSQL ([NAMEDATALEN](https://www.postgresql.org/docs/9.5/static/sql-syntax-lexical.html#SQL-SYNTAX-IDENTIFIERS))
* Only the following column types are currently fully suported:
  - SQL_CHAR
  - SQL_WCHAR
  - SQL_VARCHAR
  - SQL_WVARCHAR
  - SQL_LONGVARCHAR
  - SQL_WLONGVARCHAR
  - SQL_DECIMAL
  - SQL_NUMERIC
  - SQL_INTEGER
  - SQL_REAL
  - SQL_FLOAT
  - SQL_DOUBLE
  - SQL_SMALLINT
  - SQL_TINYINT
  - SQL_BIGINT
  - SQL_DATE
  - SQL_TYPE_TIME
  - SQL_TIME
  - SQL_TIMESTAMP
  - SQL_GUID
* Foreign encodings are supported with the  `encoding` option
  for any enconding supported by PostgreSQL and compatible with the
  local database. The encoding must be identified with the
  name used by [PostgreSQL](https://www.postgresql.org/docs/9.5/static/multibyte.html).
