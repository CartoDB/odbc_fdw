CREATE SERVER mysql_fdw
FOREIGN DATA WRAPPER odbc_fdw
OPTIONS (
  odbc_DRIVER '${driver}',
  odbc_SERVER '${host}',
  odbc_PORT '${port}',
  odbc_DATABASE '${dbname}',
  encoding '${encoding}'
);
CREATE USER MAPPING FOR postgres SERVER mysql_fdw
OPTIONS (
  odbc_UID '${user}',
  odbc_PWD '${password}'
);
IMPORT FOREIGN SCHEMA fdw_tests
FROM SERVER mysql_fdw
INTO public;
IMPORT FOREIGN SCHEMA fdw_tests
    FROM SERVER mysql_fdw
    INTO public
    OPTIONS(
      table 'query_mysql_test_table',
      sql_query 'select count(1) from mysql_test_table'
);