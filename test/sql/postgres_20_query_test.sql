SELECT * FROM postgres_test_table;
SELECT * FROM query_postgres_test_table;
SELECT * FROM existent_table_in_schema_public;
SELECT * FROM test_table_in_schema;
SELECT * FROM ODBCTablesList('postgres_fdw', 1);
SELECT * FROM ODBCTableSize('postgres_fdw', 'postgres_test_table');
SELECT * FROM ODBCQuerySize('postgres_fdw', 'select * from postgres_test_table');
\x
SELECT * FROM postgres_var_test_table;
\x