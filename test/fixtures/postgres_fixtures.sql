DROP TABLE IF EXISTS postgres_test_table;
DROP TABLE IF EXISTS existent_table_in_schema_public;
DROP TABLE IF EXISTS test_schema.test_table_in_schema;
DROP TABLE IF EXISTS postgres_var_test_table;

-- To test a normal import from public schema
CREATE TABLE postgres_test_table (
    id integer PRIMARY KEY,
    varchar_example varchar(40),
    text_example text,
    integer_example integer,
    numeric_example numeric,
    timestamp_example   timestamp,
    boolean_example boolean
);
INSERT INTO postgres_test_table VALUES (1, 'example', 'example', 100, 10.12, '2016-01-01 00:00:00', true);

-- To tests the import of tables from non-existent schema
CREATE TABLE existent_table_in_schema_public (
    id integer PRIMARY KEY,
    data varchar(40)
);
INSERT INTO existent_table_in_schema_public VALUES (1, 'example');

-- To test the import from other schema than public
CREATE SCHEMA test_schema;
CREATE TABLE test_schema.test_table_in_schema (
    id integer PRIMARY KEY,
    data varchar(40)
);
INSERT INTO test_schema.test_table_in_schema VALUES (1, 'example');

-- To test reading variable size data
CREATE TABLE postgres_var_test_table(varch text, varbin bytea);
INSERT INTO postgres_var_test_table
  SELECT string_agg(to_hex(n), ''), ('\x' || string_agg(to_hex(n),''))::bytea FROM generate_series(1,2000) n;
