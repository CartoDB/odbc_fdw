ODBC Foreign Data Wrapper for PostgreSQL
=========================================

This is a foreign data wrapper (FDW) to connect [PostgreSQL](https://www.postgresql.org/)
to remote databases using Open Database Connectivity [ODBC](http://msdn.microsoft.com/en-us/library/ms714562(v=VS.85).aspx).

[![Travis Build Status](https://travis-ci.org/CartoDB/odbc_fdw.svg?branch=master)](https://travis-ci.org/CartoDB/odbc_fdw)
[![AppVeyor Build Status](https://ci.appveyor.com/api/projects/status/github/CartoDB/odbc_fdw?branch=master&svg=true)](https://ci.appveyor.com/project/alberhander/odbc-fdw "Get your fresh Windows build here!")

Supports PostgreSQL 9.5+.

This FDW was originally developed by Zheng Yang in 2011,
with contributions by Gunnar "Nick" Bluth from 2014
and further developed by CARTO since 2016.

While we don’t provide direct technical support to Open Source
installations, it is possible to engage in technical conversations
with the community and part of the CARTO team (including some team
members like Solutions, Support, Backend, and Frontend engineers) in
our [Google Groups
forum](https://groups.google.com/forum/#!forum/cartodb) and [GIS Stack
Exchange](https://gis.stackexchange.com/questions/tagged/carto).

Contents
--------

1. [Features](#features)
2. [Supported platforms](#supported-platforms)
3. [Installation](#installation)
4. [Usage](#usage)
5. [Functions](#functions)
6. [Identifier case handling](#identifier-case-handling)
7. [Generated columns](#generated-columns)
8. [Character set handling](#character-set-handling)
9. [Examples](#examples)
10. [Limitations](#limitations)
11. [Contributing](#contributing)
12. [Useful links](#useful-links)
13. [License](#license)

Features
--------
## Common features

- `SELECT` from ODBC data source

## Pushdowning

**yet not described**.

Supported platforms
-------------------

`odbc_fdw` was developed on Linux, and should run on any
reasonably POSIX-compliant system.

`odbc_fdw` is designed to be compatible with PostgreSQL 9.5+.

Installation
------------
### Prerequisites

To compile and install this extension, assuming a Linux OS,
the libraries and header files for ODBC and PostgreSQL are needed,
e.g. in Ubuntu this can be provided by the `unixodbc-dev`
and `postgresql-server-dev-{version}` system packages for your
PostgreSQL veresion.

To make use of the extension ODBC drivers for the data sources to
be used must be installed in the system and reflected
in the `/etc/odbcinst.ini` file.

### Driver requirements
- `odbc-postgresql`: >= 9.x
- `libmyodbc`: >=  5.1
- `FreeTDS`: >= 1.0
- `hive-odbc-native`: >= 2.1

### Source installation
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

## CREATE SERVER options

`odbc_fdw` accepts the following options via the `CREATE SERVER` command:

- **dsn** as *string*, required

  The Database Source Name of the foreign database system you're connecting to.
  
- **driver** as *string*, optioanl

  The name of the ODBC driver to use (needed if no dsn is used).
  
- **encoding** as *string*, optional

  Name of encoding of datasource.
  
Any other ODBC connection attribute is driver-dependent, and should be defined by
an option named as the attribute prepended by the prefix `odbc_`.
For example `odbc_server`,   `odbc_port`, `odbc_uid`, `odbc_pwd`, etc.

  **The odbc_ prefixed options can be defined either in the `SERVER`, `USER MAPPING` or `FOREIGN TABLE` statements.**

The DSN and Driver can also be defined by the prefixed options
`odbc_DSN`  and `odbc_DRIVER` repectively.

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
handled by the driver).

## CREATE USER MAPPING options

`odbc_fdw` accepts the following options via the `CREATE USER MAPPING`
command:

- **username** or **odbc_UID** etc. as *string*

  Username to use when connecting to ODBC.

- **password** or **odbc_PWD** etc. as *string*

  Password to authenticate to the ODBC server with.
  
Usually you'll want to define authentication-related attributes
in a `CREATE USER MAPPING` statement, so that they are determined by
the connected PostgreSQL role, but that's not a requirement: any attribute
can be define in any of the statements; when a foreign table is access
the `SERVER`, `USER MAPPING` and `FOREIGN TABLE` options will be combined
to produce an ODBC connection string.

## CREATE FOREIGN TABLE options

`odbc_fdw` accepts the following table-level options via the
`CREATE FOREIGN TABLE` command.
    
- **schema** as *string*, optional

  The schema of the database to query.
  
- **table** as *string*, optional
  
  The name of the table to query. Also the name of the foreign table to create in the case of queries.
  
- **sql_query** as *string*, optional

  User defined SQL statement for querying the foreign table(s). This overrides the `table` parameters. This should use the syntax of ODBC driver used.
  
- **sql_count** as *string*, optional

  User defined SQL statement for counting number of records in the foreign table(s). This should use the syntax of ODBC driver used.

## IMPORT FOREIGN SCHEMA options

`odbc_fdw` supports [IMPORT FOREIGN SCHEMA](https://www.postgresql.org/docs/current/sql-importforeignschema.html) and 
 accepts the following custom options:
 
- **schema** as *string*, optional

  The schema of the database to query.
  
- **table** as *string*, optional
  
  The name of the table to query. Also the name of the foreign table to create in the case of queries.
  
- **prefix** as *string*, optional

  The prefix for foreign table names. This can be used to prepend a prefix to the names of tables imported from an external database.

Note that if the `prefix` option is used and only one specific foreign table is to be imported,
the `table` option is necessary (to specify the unprefixed, remote table name). In this case
it is better not to include a `LIMIT TO` clause (otherwise it has to reference the *prefixed* table name).

- **sql_query** as *string*, optional

  User defined SQL statement for querying the foreign table(s). This overrides the `table` parameters. This should use the syntax of ODBC driver used.
  
- **sql_count** as *string*, optional

  User defined SQL statement for counting number of records in the foreign table(s). This should use the syntax of ODBC driver used.
  
## TRUNCATE support

`odbc_fdw` don't implements the foreign data wrapper `TRUNCATE` API, available
from PostgreSQL 14. 

**yet not described**.

Functions
---------

As well as the standard `odbc_fdw_handler()` and `odbc_fdw_validator()`
functions, `odbc_fdw` provides the following user-callable utility functions:

Functions from this FDW in PostgreSQL catalog are **yet not described**.

Identifier case handling
------------------------

As PostgreSQL and ODBC take opposite approaches to case folding (PostgreSQL
folds identifiers to lower case by default, ODBC source usually folds to upper case), it's important
to be aware of potential issues with table and column names.

When defining foreign tables, PostgreSQL will pass any identifiers which do not
require quoting to ODBC as-is, defaulting to lower-case. ODBC *can* then
implictly fold these to upper case.

All rules and problems with ODBC identifiers **yet not tested and described**.

Generated columns
-----------------

Behavoiur within generated columns **yet not tested**. 

Note that while `odbc_fdw` will insert or update the generated column value
in ODBC datasouce, there is nothing to stop the value being modified within ODBC data source,
and hence no guarantee that in subsequent `SELECT` operations the column will
still contain the expected generated value. This limitation also applies to
`postgres_fdw`.

For more details on generated columns see:

- [Generated Columns](https://www.postgresql.org/docs/current/ddl-generated-columns.html)
- [CREATE FOREIGN TABLE](https://www.postgresql.org/docs/current/sql-createforeigntable.html)

Character set handling
----------------------

Encodings mapping between PostgeeSQL and ODBC **yet not described**.

Examples
--------

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

Limitations
-----------

* Column, schema, table names should not be longer than the limit stablished by
  PostgreSQL ([NAMEDATALEN](https://www.postgresql.org/docs/current/static/sql-syntax-lexical.html#SQL-SYNTAX-IDENTIFIERS))
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
  name used by [PostgreSQL](https://www.postgresql.org/docs/current/static/multibyte.html).
   
Contributing
------------

Pull requests is welcome.

Useful links
------------

### Source code and protoype, popular forks

 - https://github.com/bluthg/odbc_fdw
 - https://github.com/pgspider/odbc_fdw
 
 Reference FDW realisation, `postgres_fdw`
 - https://git.postgresql.org/gitweb/?p=postgresql.git;a=tree;f=contrib/postgres_fdw;hb=HEAD

### General FDW Documentation

 - https://www.postgresql.org/docs/current/ddl-foreign-data.html
 - https://www.postgresql.org/docs/current/sql-createforeigndatawrapper.html
 - https://www.postgresql.org/docs/current/sql-createforeigntable.html
 - https://www.postgresql.org/docs/current/sql-importforeignschema.html
 - https://www.postgresql.org/docs/current/fdwhandler.html
 - https://www.postgresql.org/docs/current/postgres-fdw.html

### Other FDWs

 - https://wiki.postgresql.org/wiki/Fdw
 - https://pgxn.org/tag/fdw/
 
License
-------

See the [`LICENSE`](LICENSE) file for full details.
