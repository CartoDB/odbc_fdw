CREATE SERVER postgres_fdw
FOREIGN DATA WRAPPER odbc_fdw
OPTIONS (
  odbc_DRIVER '${driver}',
  odbc_SERVER '${host}',
  odbc_PORT '${port}',
  odbc_DATABASE '${dbname}'
);
CREATE USER MAPPING FOR postgres SERVER postgres_fdw
OPTIONS (
  odbc_UID '${user}',
  odbc_PWD '${password}'
);
IMPORT FOREIGN SCHEMA public
    FROM SERVER postgres_fdw
    INTO public
    OPTIONS(
      table 'query_postgres_test_table',
      "odbc_BoolsAsChar" '0',
      "odbc_ByteaAsLongVarBinary" '1',
      sql_query 'select count(1) from postgres_test_table'
);
IMPORT FOREIGN SCHEMA public
    FROM SERVER postgres_fdw
    INTO public
    OPTIONS(
      table 'postgres_test_table',
      "odbc_BoolsAsChar" '0',
      "odbc_ByteaAsLongVarBinary" '1'
);
IMPORT FOREIGN SCHEMA nonexistent_schema
    FROM SERVER postgres_fdw
    INTO public
    OPTIONS(
      table 'existent_table_in_schema_public',
      "odbc_BoolsAsChar" '0',
      "odbc_ByteaAsLongVarBinary" '1'
);
IMPORT FOREIGN SCHEMA test_schema
    FROM SERVER postgres_fdw
    INTO public
    OPTIONS(
      table 'test_table_in_schema',
      "odbc_BoolsAsChar" '0',
      "odbc_ByteaAsLongVarBinary" '1'
);
IMPORT FOREIGN SCHEMA public
    FROM SERVER postgres_fdw
    INTO public
    OPTIONS(
      table 'postgres_var_test_table',
      "odbc_BoolsAsChar" '0',
      "odbc_ByteaAsLongVarBinary" '1'
);