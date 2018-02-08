/*----------------------------------------------------------
 *
 *		  foreign-data wrapper for ODBC
 *
 * Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * This software is released under the PostgreSQL Licence.
 *
 * Author: Zheng Yang <zhengyang4k@gmail.com>
 * Updated to 9.2+ by Gunnar "Nick" Bluth <nick@pro-open.de>
 *	 based on tds_fdw code from Geoff Montee
 *
 * IDENTIFICATION
 *		odbc_fdw/odbc_fdw.c
 *
 *----------------------------------------------------------
 */

/* Debug mode flag */
/*
#define DEBUG
*/
#include "postgres.h"
#include <string.h>

#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "storage/lock.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "nodes/nodes.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"

#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/planmain.h"

#include "executor/spi.h"

#include <stdio.h>
#include <sql.h>
#include <sqlext.h>

PG_MODULE_MAGIC;

#define PROCID_TEXTEQ 67
#define PROCID_TEXTCONST 25

/* Provisional limit to name lengths in characters */
#define MAXIMUM_CATALOG_NAME_LEN 255
#define MAXIMUM_SCHEMA_NAME_LEN 255
#define MAXIMUM_TABLE_NAME_LEN 255
#define MAXIMUM_COLUMN_NAME_LEN 255

/* Maximum GetData buffer size */
#define MAXIMUM_BUFFER_SIZE 8192

/*
 * Numbers of the columns returned by SQLTables:
 * 1: TABLE_CAT (ODBC 3.0) TABLE_QUALIFIER (ODBC 2.0) -- database name
 * 2: TABLE_SCHEM (ODBC 3.0) TABLE_OWNER (ODBC 2.0)   -- schema name
 * 3: TABLE_NAME
 * 4: TABLE_TYPE
 * 5: REMARKS
 */
#define SQLTABLES_CATALOG_COLUMN 1
#define SQLTABLES_SCHEMA_COLUMN 2
#define SQLTABLES_NAME_COLUMN 3

#define ODBC_SQLSTATE_FRACTIONAL_TRUNCATION "01S07"
typedef struct odbcFdwOptions
{
	char	   *schema;			/* Foreign schema name */
	char	   *table;			/* Foreign table */
	char	   *prefix;			/* Prefix for imported foreign table names */
	char	   *sql_query;		/* SQL query (overrides table) */
	char	   *sql_count;		/* SQL query for counting results */
	char	   *encoding;		/* Character encoding name */

	List	   *connection_list;	/* ODBC connection attributes */

	List	   *mapping_list;	/* Column name mapping */
} odbcFdwOptions;

typedef struct odbcFdwExecutionState
{
	AttInMetadata *attinmeta;
	odbcFdwOptions options;
	SQLHSTMT	stmt;
	int			num_of_result_cols;
	int			num_of_table_cols;
	StringInfoData *table_columns;
	bool		first_iteration;
	List	   *col_position_mask;
	List	   *col_size_array;
	List	   *col_conversion_array;
	char	   *sql_count;
	int			encoding;
} odbcFdwExecutionState;

struct odbcFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Array of valid options
 * In addition to this, any option with a name prefixed
 * by odbc_ is accepted as an ODBC connection attribute
 * and can be defined in foreign servier, user mapping or
 * table statements.
 * Note that dsn and driver can be defined by
 * prefixed or non-prefixed options.
 */
static struct odbcFdwOption valid_options[] =
{
	/* Foreign server options */
	{"dsn", ForeignServerRelationId},
	{"driver", ForeignServerRelationId},
	{"encoding", ForeignServerRelationId},

	/* Foreign table options */
	{"schema", ForeignTableRelationId},
	{"table", ForeignTableRelationId},
	{"prefix", ForeignTableRelationId},
	{"sql_query", ForeignTableRelationId},
	{"sql_count", ForeignTableRelationId},

	/* Sentinel */
	{NULL, InvalidOid}
};

typedef enum
{
	TEXT_CONVERSION, HEX_CONVERSION, BIN_CONVERSION, BOOL_CONVERSION
} ColumnConversion;

/*
 * SQL functions
 */
extern Datum odbc_fdw_handler(PG_FUNCTION_ARGS);
extern Datum odbc_fdw_validator(PG_FUNCTION_ARGS);
extern Datum odbc_tables_list(PG_FUNCTION_ARGS);
extern Datum odbc_table_size(PG_FUNCTION_ARGS);
extern Datum odbc_query_size(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(odbc_fdw_handler);
PG_FUNCTION_INFO_V1(odbc_fdw_validator);
PG_FUNCTION_INFO_V1(odbc_tables_list);
PG_FUNCTION_INFO_V1(odbc_table_size);
PG_FUNCTION_INFO_V1(odbc_query_size);

/*
 * FDW callback routines
 */
static void odbcExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void odbcBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *odbcIterateForeignScan(ForeignScanState *node);
static void odbcReScanForeignScan(ForeignScanState *node);
static void odbcEndForeignScan(ForeignScanState *node);
static void odbcGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void odbcEstimateCosts(PlannerInfo *root, RelOptInfo *baserel, Cost *startup_cost, Cost *total_cost, Oid foreigntableid);
static void odbcGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static bool odbcAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages);
static ForeignScan *odbcGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan);
List	   *odbcImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid);

/*
 * helper functions
 */
static bool odbcIsValidOption(const char *option, Oid context);
static void check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type);
static const char *empty_string_if_null(char *string);
static void extract_odbcFdwOptions(List *options_list, odbcFdwOptions *extracted_options);
static void init_odbcFdwOptions(odbcFdwOptions *options);
static void copy_odbcFdwOptions(odbcFdwOptions *to, odbcFdwOptions *from);
static void odbc_connection(odbcFdwOptions *options, SQLHENV *env, SQLHDBC *dbc);
static void sql_data_type(SQLSMALLINT odbc_data_type, SQLULEN column_size, SQLSMALLINT decimal_digits, SQLSMALLINT nullable, StringInfo sql_type);
static void odbcGetOptions(Oid server_oid, List *add_options, odbcFdwOptions *extracted_options);
static void odbcGetTableOptions(Oid foreigntableid, odbcFdwOptions *extracted_options);
static void odbcGetTableSize(odbcFdwOptions *options, unsigned int *size);
static void check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type);
static void odbcConnStr(StringInfoData *conn_str, odbcFdwOptions *options);
static char *get_schema_name(odbcFdwOptions *options);
static inline bool is_blank_string(const char *s);
static Oid	oid_from_server_name(char *serverName);

/*
 * Check if string pointer is NULL or points to empty string
 */
static inline bool
is_blank_string(const char *s)
{
	return s == NULL || s[0] == '\0';
}

Datum
odbc_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	/* FIXME */
	fdwroutine->GetForeignRelSize = odbcGetForeignRelSize;
	fdwroutine->GetForeignPaths = odbcGetForeignPaths;
	fdwroutine->AnalyzeForeignTable = odbcAnalyzeForeignTable;
	fdwroutine->GetForeignPlan = odbcGetForeignPlan;
	fdwroutine->ExplainForeignScan = odbcExplainForeignScan;
	fdwroutine->BeginForeignScan = odbcBeginForeignScan;
	fdwroutine->IterateForeignScan = odbcIterateForeignScan;
	fdwroutine->ReScanForeignScan = odbcReScanForeignScan;
	fdwroutine->EndForeignScan = odbcEndForeignScan;
	fdwroutine->ImportForeignSchema = odbcImportForeignSchema;
	PG_RETURN_POINTER(fdwroutine);
}

static void
init_odbcFdwOptions(odbcFdwOptions *options)
{
	memset(options, 0, sizeof(odbcFdwOptions));
}

static void
copy_odbcFdwOptions(odbcFdwOptions *to, odbcFdwOptions *from)
{
	if (to && from)
	{
		*to = *from;
	}
}

/*
 * Avoid NULL string: return original string, or empty string if NULL
 */
static const char *
empty_string_if_null(char *string)
{
	static const char *empty_string = "";

	return string == NULL ? empty_string : string;
}

static const char odbc_attribute_prefix[] = "odbc_";
static const int odbc_attribute_prefix_len = sizeof(odbc_attribute_prefix) - 1; /* strlen(odbc_attribute_
																				 * prefix); */

static bool
is_odbc_attribute(const char *defname)
{
	return (strlen(defname) > odbc_attribute_prefix_len && strncmp(defname, odbc_attribute_prefix, odbc_attribute_prefix_len) == 0);
}

/* These ODBC attributes names are always uppercase */
static const char *normalized_attributes[] = {"DRIVER", "DSN", "UID", "PWD"};
static const char *
normalized_attribute(const char *attribute_name)
{
	int			i;

	for (i = 0; i < sizeof(normalized_attributes) / sizeof(normalized_attributes[0]); i++)
	{
		if (strcasecmp(attribute_name, normalized_attributes[i]) == 0)
		{
			attribute_name = normalized_attributes[i];
			break;
		}
	}
	return attribute_name;
}

static const char *
get_odbc_attribute_name(const char *defname)
{
	int			offset = is_odbc_attribute(defname) ? odbc_attribute_prefix_len : 0;

	return normalized_attribute(defname + offset);
}

static void
extract_odbcFdwOptions(List *options_list, odbcFdwOptions *extracted_options)
{
	ListCell   *lc;

#ifdef DEBUG
	elog(DEBUG1, "extract_init_odbcFdwOptions");
#endif

	init_odbcFdwOptions(extracted_options);

	/* Loop through the options, and get the foreign table options */
	foreach(lc, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "dsn") == 0)
		{
			extracted_options->connection_list = lappend(extracted_options->connection_list, def);
			continue;
		}

		if (strcmp(def->defname, "driver") == 0)
		{
			extracted_options->connection_list = lappend(extracted_options->connection_list, def);
			continue;
		}

		if (strcmp(def->defname, "schema") == 0)
		{
			extracted_options->schema = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "table") == 0)
		{
			extracted_options->table = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "prefix") == 0)
		{
			extracted_options->prefix = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "sql_query") == 0)
		{
			extracted_options->sql_query = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "sql_count") == 0)
		{
			extracted_options->sql_count = defGetString(def);
			continue;
		}

		if (strcmp(def->defname, "encoding") == 0)
		{
			extracted_options->encoding = defGetString(def);
			continue;
		}

		if (is_odbc_attribute(def->defname))
		{
			extracted_options->connection_list = lappend(extracted_options->connection_list, def);
			continue;
		}

		/* Column mapping goes here */

		/*
		 * TODO: is this useful? if so, how can columns names coincident with
		 * option names be escaped?
		 */
		extracted_options->mapping_list = lappend(extracted_options->mapping_list, def);
	}
}

/*
 * Get the schema name from the options
 */
static char *
get_schema_name(odbcFdwOptions *options)
{
	return options->schema;
}

/*
 * Establish ODBC connection
 */
static void
odbc_connection(odbcFdwOptions *options, SQLHENV *env, SQLHDBC *dbc)
{
	StringInfoData conn_str;
	SQLCHAR		OutConnStr[1024];
	SQLSMALLINT OutConnStrLen;
	SQLRETURN	ret;

	odbcConnStr(&conn_str, options);

	/* Allocate an environment handle */
	SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, env);
	/* We want ODBC 3 support */
	SQLSetEnvAttr(*env, SQL_ATTR_ODBC_VERSION, (void *) SQL_OV_ODBC3, 0);

	/* Allocate a connection handle */
	SQLAllocHandle(SQL_HANDLE_DBC, *env, dbc);
	/* Connect to the DSN */
	ret = SQLDriverConnect(*dbc, NULL, (SQLCHAR *) conn_str.data, SQL_NTS,
					  OutConnStr, 1024, &OutConnStrLen, SQL_DRIVER_COMPLETE);
	check_return(ret, "Connecting to driver", dbc, SQL_HANDLE_DBC);
}

/*
 * Validate function
 */
Datum
odbc_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *svr_schema = NULL;
	char	   *svr_table = NULL;
	char	   *svr_prefix = NULL;
	char	   *sql_query = NULL;
	char	   *sql_count = NULL;
	ListCell   *cell;

#ifdef DEBUG
	elog(DEBUG1, "odbc_fdw_validator");
#endif

	/*
	 * Check that the necessary options: address, port, database
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		/* Complain invalid options */
		if (!odbcIsValidOption(def->defname, catalog))
		{
			struct odbcFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);
			}

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
					 ));
		}

		/*
		 * TODO: detect redundant connection attributes and missing required
		 * attributs (dsn or driver) Complain about redundent options
		 */
		if (strcmp(def->defname, "schema") == 0)
		{
			if (!is_blank_string(svr_schema))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options: schema (%s)", defGetString(def))
						 ));

			svr_schema = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (!is_blank_string(svr_table))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options: table (%s)", defGetString(def))
						 ));

			svr_table = defGetString(def);
		}
		else if (strcmp(def->defname, "prefix") == 0)
		{
			if (!is_blank_string(svr_prefix))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options: prefix (%s)", defGetString(def))
						 ));

			svr_prefix = defGetString(def);
		}
		else if (strcmp(def->defname, "sql_query") == 0)
		{
			if (sql_query)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options: sql_query (%s)", defGetString(def))
						 ));

			sql_query = defGetString(def);
		}
		else if (strcmp(def->defname, "sql_count") == 0)
		{
			if (!is_blank_string(sql_count))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options: sql_count (%s)", defGetString(def))
						 ));

			sql_count = defGetString(def);
		}
	}

	PG_RETURN_VOID();
}

/*
 * Map ODBC data types to PostgreSQL
 */
static void
sql_data_type(
			  SQLSMALLINT odbc_data_type,
			  SQLULEN column_size,
			  SQLSMALLINT decimal_digits,
			  SQLSMALLINT nullable,
			  StringInfo sql_type
)
{
	initStringInfo(sql_type);
	switch (odbc_data_type)
	{
		case SQL_CHAR:
		case SQL_WCHAR:
			appendStringInfo(sql_type, "char(%u)", (unsigned) column_size);
			break;
		case SQL_VARCHAR:
		case SQL_WVARCHAR:
			if (column_size <= 255 && column_size > 0)
			{
				appendStringInfo(sql_type, "varchar(%u)", (unsigned) column_size);
			}
			else
			{
				appendStringInfo(sql_type, "text");
			}
			break;
		case SQL_LONGVARCHAR:
		case SQL_WLONGVARCHAR:
			appendStringInfo(sql_type, "text");
			break;
		case SQL_DECIMAL:
			appendStringInfo(sql_type, "decimal(%u,%d)", (unsigned) column_size, decimal_digits);
			break;
		case SQL_NUMERIC:
			appendStringInfo(sql_type, "numeric(%u,%d)", (unsigned) column_size, decimal_digits);
			break;
		case SQL_INTEGER:
			appendStringInfo(sql_type, "integer");
			break;
		case SQL_REAL:
			appendStringInfo(sql_type, "real");
			break;
		case SQL_FLOAT:
			appendStringInfo(sql_type, "real");
			break;
		case SQL_DOUBLE:
			appendStringInfo(sql_type, "float8");
			break;
		case SQL_BIT:

			/*
			 * Use boolean instead of bit(1) because: * binary types are not
			 * yet fully supported * boolean is more commonly used in PG *
			 * With options BoolsAsChar=0 this allows preserving boolean
			 * columns from pSQL ODBC.
			 */
			appendStringInfo(sql_type, "boolean");
			break;
		case SQL_SMALLINT:
		case SQL_TINYINT:
			appendStringInfo(sql_type, "smallint");
			break;
		case SQL_BIGINT:
			appendStringInfo(sql_type, "bigint");
			break;

			/*-----------------------------------------------------------
			 * TODO: Implement these cases properly. See #23
			 *
			 * case SQL_BINARY:
			 *      appendStringInfo(sql_type, "bit(%u)", (unsigned)column_size);
			 *      break;
			 *-----------------------------------------------------------
			 */
		case SQL_VARBINARY:
			if (column_size > 0)
				appendStringInfo(sql_type, "varbit(%u)", (unsigned)column_size);
			else
				appendStringInfo(sql_type,"bytea");
			break;
		case SQL_LONGVARBINARY:
			appendStringInfo(sql_type, "bytea");
			break;
		case SQL_TYPE_DATE:
		case SQL_DATE:
			appendStringInfo(sql_type, "date");
			break;
		case SQL_TYPE_TIME:
		case SQL_TIME:
			appendStringInfo(sql_type, "time");
			break;
		case SQL_TYPE_TIMESTAMP:
		case SQL_TIMESTAMP:
			appendStringInfo(sql_type, "timestamp");
			break;
		case SQL_GUID:
			appendStringInfo(sql_type, "uuid");
			break;
	};
}

static SQLULEN
minimum_buffer_size(SQLSMALLINT odbc_data_type)
{
	switch (odbc_data_type)
	{
		case SQL_DECIMAL:
		case SQL_NUMERIC:
			return 32;
		case SQL_INTEGER:
			return 12;
		case SQL_REAL:
		case SQL_FLOAT:
			return 18;
		case SQL_DOUBLE:
			return 26;
		case SQL_SMALLINT:
		case SQL_TINYINT:
			return 6;
		case SQL_BIGINT:
			return 21;
		case SQL_TYPE_DATE:
		case SQL_DATE:
			return 10;
		case SQL_TYPE_TIME:
		case SQL_TIME:
			return 8;
		case SQL_TYPE_TIMESTAMP:
		case SQL_TIMESTAMP:
			return 20;
		default:
			return 0;
	};
}

/*
 * Fetch the options for a server and options list
 */
static void
odbcGetOptions(Oid server_oid, List *add_options, odbcFdwOptions *extracted_options)
{
	ForeignServer *server;
	UserMapping *mapping;
	List	   *options;

#ifdef DEBUG
	elog(DEBUG1, "odbcGetOptions");
#endif

	server = GetForeignServer(server_oid);
	mapping = GetUserMapping(GetUserId(), server_oid);

	options = NIL;
	options = list_concat(options, list_copy(add_options));
	options = list_concat(options, list_copy(server->options));
	options = list_concat(options, list_copy(mapping->options));

	extract_odbcFdwOptions(options, extracted_options);
}

/*
 * Fetch the options for a odbc_fdw foreign table.
 */
static void
odbcGetTableOptions(Oid foreigntableid, odbcFdwOptions *extracted_options)
{
	ForeignTable *table;

#ifdef DEBUG
	elog(DEBUG1, "odbcGetTableOptions");
#endif

	table = GetForeignTable(foreigntableid);
	odbcGetOptions(table->serverid, table->options, extracted_options);
}

static void
check_return(SQLRETURN ret, char *msg, SQLHANDLE handle, SQLSMALLINT type)
{
	int			err_code = ERRCODE_SYSTEM_ERROR;

#ifdef DEBUG
	SQLINTEGER	i = 0;
	SQLINTEGER	native;
	SQLCHAR		state[7];
	SQLCHAR text[256];
	SQLSMALLINT len;
	SQLRETURN	diag_ret;

	if (SQL_SUCCEEDED(ret))
		elog(DEBUG1, "Successful result: %s", msg);
#endif

	if (!SQL_SUCCEEDED(ret))
	{
#ifdef DEBUG
		elog(DEBUG1, "Error result (%d): %s", ret, msg);
		if (handle)
		{
			do
			{
				diag_ret = SQLGetDiagRec(type, handle, ++i, state, &native, text,
										 sizeof(text), &len);
				if (SQL_SUCCEEDED(diag_ret))
					elog(DEBUG1, " %s:%ld:%ld:%s\n", state, (long int) i, (long int) native, text);
			}
			while (diag_ret == SQL_SUCCESS);
		}
#endif
		ereport(ERROR, (errcode(err_code), errmsg("%s", msg)));
	}
}

/*
 * Get name qualifier char
 */
static void
getNameQualifierChar(SQLHDBC dbc, StringInfoData *nq_char)
{
	SQLCHAR		name_qualifier_char[2];

#ifdef DEBUG
	elog(DEBUG1, "getNameQualifierChar");
#endif

	SQLGetInfo(dbc,
			   SQL_CATALOG_NAME_SEPARATOR,
			   (SQLPOINTER) &name_qualifier_char,
			   2,
			   NULL);
	name_qualifier_char[1] = 0; /* some drivers fail to copy the trailing zero */

	initStringInfo(nq_char);
	appendStringInfo(nq_char, "%s", (char *) name_qualifier_char);
}

/*
 * Get quote cahr
 */
static void
getQuoteChar(SQLHDBC dbc, StringInfoData *q_char)
{
	SQLCHAR		quote_char[2];

#ifdef DEBUG
	elog(DEBUG1, "getQuoteChar");
#endif

	SQLGetInfo(dbc,
			   SQL_IDENTIFIER_QUOTE_CHAR,
			   (SQLPOINTER) &quote_char,
			   2,
			   NULL);
	quote_char[1] = 0;			/* some drivers fail to copy the trailing zero */

	initStringInfo(q_char);
	appendStringInfo(q_char, "%s", (char *) quote_char);
}

static bool
appendConnAttribute(bool sep, StringInfoData *conn_str, const char *name, const char *value)
{
	static const char *sep_str = ";";

	if (!is_blank_string(value))
	{
		if (sep)
			appendStringInfoString(conn_str, sep_str);
		appendStringInfo(conn_str, "%s=%s", name, value);
		sep = TRUE;
	}
	return sep;
}

static void
odbcConnStr(StringInfoData *conn_str, odbcFdwOptions *options)
{
	bool		sep = FALSE;
	ListCell   *lc;

	initStringInfo(conn_str);

	foreach(lc, options->connection_list)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		sep = appendConnAttribute(sep, conn_str, get_odbc_attribute_name(def->defname), defGetString(def));
	}
#ifdef DEBUG
	elog(DEBUG1, "CONN STR: %s", conn_str->data);
#endif
}

/*
 * get table size of a table
 */
static void
odbcGetTableSize(odbcFdwOptions *options, unsigned int *size)
{
	SQLHENV		env;
	SQLHDBC		dbc;
	SQLHSTMT	stmt;
	SQLRETURN	ret;

	StringInfoData sql_str;

	SQLUBIGINT	table_size;
	SQLLEN		indicator;

	StringInfoData name_qualifier_char;
	StringInfoData quote_char;

	const char *schema_name;

	schema_name = get_schema_name(options);

	odbc_connection(options, &env, &dbc);

	/* Allocate a statement handle */
	SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

	if (is_blank_string(options->sql_count))
	{
		/* Get quote char */
		getQuoteChar(dbc, &quote_char);

		/* Get name qualifier char */
		getNameQualifierChar(dbc, &name_qualifier_char);

		initStringInfo(&sql_str);
		if (is_blank_string(options->sql_query))
		{
			if (is_blank_string(schema_name))
			{
				appendStringInfo(&sql_str, "SELECT COUNT(*) FROM %s%s%s",
						   quote_char.data, options->table, quote_char.data);
			}
			else
			{
				appendStringInfo(&sql_str, "SELECT COUNT(*) FROM %s%s%s%s%s%s%s",
							   quote_char.data, schema_name, quote_char.data,
								 name_qualifier_char.data,
						   quote_char.data, options->table, quote_char.data);
			}
		}
		else
		{
			if (options->sql_query[strlen(options->sql_query) - 1] == ';')
			{
				/* Remove trailing semicolon if present */
				options->sql_query[strlen(options->sql_query) - 1] = 0;
			}
			appendStringInfo(&sql_str, "SELECT COUNT(*) FROM (%s) AS _odbc_fwd_count_wrapped", options->sql_query);
		}
	}
	else
	{
		initStringInfo(&sql_str);
		appendStringInfo(&sql_str, "%s", options->sql_count);
	}

#ifdef DEBUG
	elog(DEBUG1, "Count query: %s", sql_str.data);
#endif

	ret = SQLExecDirect(stmt, (SQLCHAR *) sql_str.data, SQL_NTS);
	check_return(ret, "Executing ODBC query", stmt, SQL_HANDLE_STMT);
	if (SQL_SUCCEEDED(ret))
	{
		SQLFetch(stmt);
		/* retrieve column data as a big int */
		ret = SQLGetData(stmt, 1, SQL_C_UBIGINT, &table_size, 0, &indicator);
		if (SQL_SUCCEEDED(ret))
		{
			*size = (unsigned int) table_size;
#ifdef DEBUG
			elog(DEBUG1, "Count query result: %lu", table_size);
#endif
		}
	}
	else
	{
		elog(WARNING, "Error getting the table %s size", options->table);
	}

	/* Free handles, and disconnect */
	if (stmt)
	{
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		stmt = NULL;
	}
	if (dbc)
	{
		SQLFreeHandle(SQL_HANDLE_DBC, dbc);
		dbc = NULL;
	}
	if (env)
	{
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		env = NULL;
	}
	if (dbc)
		SQLDisconnect(dbc);
}

static int
strtoint(const char *nptr, char **endptr, int base)
{
	long		val = strtol(nptr, endptr, base);

	return (int) val;
}

static Oid
oid_from_server_name(char *serverName)
{
	char	   *serverOidString;
	char		sql[1024];
	int			serverOid;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	int			ret;

	if ((ret = SPI_connect()) < 0)
	{
		elog(ERROR, "oid_from_server_name: SPI_connect returned %d", ret);
	}

	sprintf(sql, "SELECT oid FROM pg_foreign_server where srvname = '%s'", serverName);
	if ((ret = SPI_execute(sql, true, 1)) != SPI_OK_SELECT)
	{
		elog(ERROR, "oid_from_server_name: Get server name from Oid query Failed, SP_exec returned %d.", ret);
	}

	if (SPI_tuptable->vals[0] != NULL)
	{
		tupdesc = SPI_tuptable->tupdesc;
		tuple = SPI_tuptable->vals[0];

		serverOidString = SPI_getvalue(tuple, tupdesc, 1);
		serverOid = strtoint(serverOidString, NULL, 10);
	}
	else
	{
		elog(ERROR, "Foreign server %s doesn't exist", serverName);
	}

	SPI_finish();
	return serverOid;
}

Datum
odbc_table_size(PG_FUNCTION_ARGS)
{
	char	   *serverName = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *tableName = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *defname = "table";
	unsigned int tableSize;
	List	   *tableOptions = NIL;
	Node	   *val = (Node *) makeString(tableName);
	DefElem    *elem = (DefElem *) makeDefElem(defname, val);
	Oid			serverOid = oid_from_server_name(serverName);
	odbcFdwOptions options;

	tableOptions = lappend(tableOptions, elem);
	odbcGetOptions(serverOid, tableOptions, &options);
	odbcGetTableSize(&options, &tableSize);

	PG_RETURN_INT32(tableSize);
}

Datum
odbc_query_size(PG_FUNCTION_ARGS)
{
	char	   *serverName = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *sqlQuery = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *defname = "sql_query";
	unsigned int querySize;
	List	   *queryOptions = NIL;
	Node	   *val = (Node *) makeString(sqlQuery);
	DefElem    *elem = (DefElem *) makeDefElem(defname, val);
	Oid			serverOid = oid_from_server_name(serverName);
	odbcFdwOptions options;

	queryOptions = lappend(queryOptions, elem);
	odbcGetOptions(serverOid, queryOptions, &options);
	odbcGetTableSize(&options, &querySize);

	PG_RETURN_INT32(querySize);
}

/*
 * Get the list of tables for the current datasource
 */
typedef struct
{
	SQLSMALLINT TargetType;
	SQLPOINTER	TargetValuePtr;
	SQLINTEGER	BufferLength;
	SQLLEN		StrLen_or_Ind;
} DataBinding;

typedef struct
{
	Oid			serverOid;
	DataBinding *tableResult;
	SQLHSTMT	stmt;
	SQLCHAR		schema;
	SQLCHAR		name;
	SQLUINTEGER rowLimit;
	SQLUINTEGER currentRow;
} TableDataCtx;


Datum
odbc_tables_list(PG_FUNCTION_ARGS)
{
	SQLHENV		env;
	SQLHDBC		dbc;
	SQLHSTMT	stmt;
	SQLUSMALLINT i;
	SQLUSMALLINT numColumns = 5;
	SQLUSMALLINT bufferSize = 1024;
	SQLUINTEGER rowLimit;
	SQLUINTEGER currentRow;
	SQLRETURN	retCode;

	FuncCallContext *funcctx;
	TupleDesc	tupdesc;
	TableDataCtx *datafctx;
	MemoryContext oldcontext;
	DataBinding *tableResult;
	AttInMetadata *attinmeta;

	if (SRF_IS_FIRSTCALL())
	{
		char	   *serverName = text_to_cstring(PG_GETARG_TEXT_PP(0));
		int			serverOid = oid_from_server_name(serverName);
		odbcFdwOptions options;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		datafctx = (TableDataCtx *) palloc(sizeof(TableDataCtx));
		tableResult = (DataBinding *) palloc(numColumns * sizeof(DataBinding));


		rowLimit = PG_GETARG_INT32(1);
		currentRow = 0;

		odbcGetOptions(serverOid, NULL, &options);
		odbc_connection(&options, &env, &dbc);
		SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

		for (i = 0; i < numColumns; i++)
		{
			tableResult[i].TargetType = SQL_C_CHAR;
			tableResult[i].BufferLength = (bufferSize + 1);
			tableResult[i].TargetValuePtr = palloc(sizeof(char) * tableResult[i].BufferLength);
		}

		for (i = 0; i < numColumns; i++)
		{
			retCode = SQLBindCol(stmt, i + 1, tableResult[i].TargetType, tableResult[i].TargetValuePtr, tableResult[i].BufferLength, &(tableResult[i].StrLen_or_Ind));
		}

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		attinmeta = TupleDescGetAttInMetadata(tupdesc);

		datafctx->serverOid = serverOid;
		datafctx->tableResult = tableResult;
		datafctx->stmt = stmt;
		datafctx->rowLimit = rowLimit;
		datafctx->currentRow = currentRow;
		funcctx->user_fctx = datafctx;
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	datafctx = funcctx->user_fctx;
	stmt = datafctx->stmt;
	tableResult = datafctx->tableResult;
	rowLimit = datafctx->rowLimit;
	currentRow = datafctx->currentRow;
	attinmeta = funcctx->attinmeta;

	retCode = SQLTables(stmt, NULL, SQL_NTS, NULL, SQL_NTS, NULL, SQL_NTS, (SQLCHAR *) "TABLE", SQL_NTS);
	if (SQL_SUCCEEDED(retCode = SQLFetch(stmt)) && (rowLimit == 0 || currentRow < rowLimit))
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		values = (char **) palloc(2 * sizeof(char *));
		values[0] = (char *) palloc(256 * sizeof(char));
		values[1] = (char *) palloc(256 * sizeof(char));
		snprintf(values[0], 256, "%s", (char *) tableResult[SQLTABLES_SCHEMA_COLUMN - 1].TargetValuePtr);
		snprintf(values[1], 256, "%s", (char *) tableResult[SQLTABLES_NAME_COLUMN - 1].TargetValuePtr);
		tuple = BuildTupleFromCStrings(attinmeta, values);
		result = HeapTupleGetDatum(tuple);
		currentRow++;
		datafctx->currentRow = currentRow;
		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * get quals in the select if there is one
 */
static void
odbcGetQual(Node *node, TupleDesc tupdesc, List *col_mapping_list, char **key, char **value, bool *pushdown)
{
	ListCell   *col_mapping;

	*key = NULL;
	*value = NULL;
	*pushdown = false;

#ifdef DEBUG
	elog(DEBUG1, "odbcGetQual");
#endif

	if (!node)
		return;

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		Node	   *left,
				   *right;
		Index		varattno;

		if (list_length(op->args) != 2)
			return;

		left = list_nth(op->args, 0);
		if (!IsA(left, Var))

			return;

		varattno = ((Var *) left)->varattno;

		right = list_nth(op->args, 1);

		if (IsA(right, Const))
		{
			StringInfoData buf;

			initStringInfo(&buf);
			/* And get the column and value... */
			*key = NameStr(tupdesc->attrs[varattno - 1]->attname);

			if (((Const *) right)->consttype == PROCID_TEXTCONST)
				*value = TextDatumGetCString(((Const *) right)->constvalue);
			else
			{
				return;
			}

			/* convert qual keys to mapped couchdb attribute name */
			foreach(col_mapping, col_mapping_list)
			{
				DefElem    *def = (DefElem *) lfirst(col_mapping);

				if (strcmp(def->defname, *key) == 0)
				{
					*key = defGetString(def);
					break;
				}
			}

			/*
			 * We can push down this qual if: - The operatory is TEXTEQ - The
			 * qual is on the _id column (in addition, _rev column can be also
			 * valid)
			 */

			if (op->opfuncid == PROCID_TEXTEQ)
				*pushdown = true;

			return;
		}
	}
	return;
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
odbcIsValidOption(const char *option, Oid context)
{
	struct odbcFdwOption *opt;

#ifdef DEBUG
	elog(DEBUG1, "odbcIsValidOption");
#endif

	/* Check if the options presents in the valid option list */
	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}

	/* ODBC attributes are valid in any context */
	if (is_odbc_attribute(option))
	{
		return true;
	}

	/* Foreign table may have anything as a mapping option */
	if (context == ForeignTableRelationId)
		return true;
	else
		return false;
}

static void
odbcGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	unsigned int table_size = 0;
	odbcFdwOptions options;

#ifdef DEBUG
	elog(DEBUG1, "odbcGetForeignRelSize");
#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(foreigntableid, &options);

	odbcGetTableSize(&options, &table_size);

	baserel->rows = table_size;
	baserel->tuples = baserel->rows;
}

static void
odbcEstimateCosts(PlannerInfo *root, RelOptInfo *baserel, Cost *startup_cost, Cost *total_cost, Oid foreigntableid)
{
	unsigned int table_size = 0;
	odbcFdwOptions options;

#ifdef DEBUG
	elog(DEBUG1, "----> finishing odbcEstimateCosts");
#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(foreigntableid, &options);

	odbcGetTableSize(&options, &table_size);

	*startup_cost = 25;

	*total_cost = baserel->rows + *startup_cost;

#ifdef DEBUG
	ereport(DEBUG1,
			(errmsg("----> finishing odbcEstimateCosts")
			 ));
#endif

}

static void
odbcGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	Cost		startup_cost;
	Cost		total_cost;

#ifdef DEBUG
	ereport(DEBUG,
			(errmsg("----> starting odbcGetForeignPaths")
			 ));
#endif

	odbcEstimateCosts(root, baserel, &startup_cost, &total_cost, foreigntableid);

	add_path(baserel,
			 (Path *) create_foreignscan_path(root, baserel, baserel->rows, startup_cost, total_cost,
						   NIL, NULL, NULL, NIL /* no fdw_private list */ ));

#ifdef DEBUG
	ereport(DEBUG1,
			(errmsg("----> finishing odbcGetForeignPaths")
			 ));
#endif
}

static bool
odbcAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages)
{
#ifdef DEBUG
	ereport(DEBUG,
			(errmsg("----> starting odbcAnalyzeForeignTable")
			 ));
#endif

#ifdef DEBUG
	ereport(DEBUG1,
			(errmsg("----> finishing odbcAnalyzeForeignTable")
			 ));
#endif

	return false;
}

static ForeignScan *
odbcGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
				   Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;

#ifdef DEBUG
	ereport(DEBUG,
			(errmsg("----> starting odbcGetForeignPlan")
			 ));
#endif

	scan_clauses = extract_actual_clauses(scan_clauses, false);

#ifdef DEBUG
	ereport(DEBUG1,
			(errmsg("----> finishing odbcGetForeignPlan")
			 ));
#endif

	return make_foreignscan(tlist, scan_clauses,
							scan_relid, NIL, NIL,
							NIL /* fdw_scan_tlist */ , NIL,		/* fdw_recheck_quals */
							NULL /* outer_plan */ );
}

/*
 * odbcBeginForeignScan
 *
 */
static void
odbcBeginForeignScan(ForeignScanState *node, int eflags)
{
	SQLHENV		env;
	SQLHDBC		dbc;
	odbcFdwExecutionState *festate;
	SQLSMALLINT result_columns;
	SQLHSTMT	stmt;
	SQLRETURN	ret;

#ifdef DEBUG
	char		dsn[256];
	char		desc[256];
	SQLSMALLINT dsn_ret;
	SQLSMALLINT desc_ret;
	SQLUSMALLINT direction;
#endif

	odbcFdwOptions options;

	Relation	rel;
	int			num_of_columns;
	StringInfoData *columns;
	int			i;
	ListCell   *col_mapping;
	StringInfoData sql;
	StringInfoData col_str;
	StringInfoData name_qualifier_char;
	StringInfoData quote_char;

	char	   *qual_key = NULL;
	char	   *qual_value = NULL;
	bool		pushdown = FALSE;

	const char *schema_name;
	int			encoding = -1;

#ifdef DEBUG
	elog(DEBUG1, "odbcBeginForeignScan");
#endif

	/* Fetch the foreign table options */
	odbcGetTableOptions(RelationGetRelid(node->ss.ss_currentRelation), &options);

	schema_name = get_schema_name(&options);

	odbc_connection(&options, &env, &dbc);

	/* Get quote char */
	getQuoteChar(dbc, &quote_char);

	/* Get name qualifier char */
	getNameQualifierChar(dbc, &name_qualifier_char);

	if (!is_blank_string(options.encoding))
	{
		encoding = pg_char_to_encoding(options.encoding);
		if (encoding < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
					 errmsg("invalid encoding name \"%s\"", options.encoding)
					 ));
		}
	}

	/* Fetch the table column info */
	rel = heap_open(RelationGetRelid(node->ss.ss_currentRelation), AccessShareLock);
	num_of_columns = rel->rd_att->natts;
	columns = (StringInfoData *) palloc(sizeof(StringInfoData) * num_of_columns);
	initStringInfo(&col_str);
	for (i = 0; i < num_of_columns; i++)
	{
		StringInfoData col;
		StringInfoData mapping;
		bool		mapped;

		/* retrieve the column name */
		initStringInfo(&col);
		appendStringInfo(&col, "%s", NameStr(rel->rd_att->attrs[i]->attname));
		mapped = FALSE;

		/*
		 * check if the column name is mapping to a different name in remote
		 * table
		 */
		foreach(col_mapping, options.mapping_list)
		{
			DefElem    *def = (DefElem *) lfirst(col_mapping);

			if (strcmp(def->defname, col.data) == 0)
			{
				initStringInfo(&mapping);
				appendStringInfo(&mapping, "%s", defGetString(def));
				mapped = TRUE;
				break;
			}
		}

		/* decide which name is going to be used */
		if (mapped)
			columns[i] = mapping;
		else
			columns[i] = col;
		appendStringInfo(&col_str, i == 0 ? "%s%s%s" : ",%s%s%s", (char *) quote_char.data, columns[i].data, (char *) quote_char.data);
	}
	heap_close(rel, NoLock);

	/* See if we've got a qual we can push down */
	if (node->ss.ps.plan->qual)
	{
		ListCell   *lc;

		foreach(lc, node->ss.ps.qual)
		{
			/* Only the first qual can be pushed down to remote DBMS */
			ExprState  *state = lfirst(lc);

			odbcGetQual((Node *) state->expr, node->ss.ss_currentRelation->rd_att, options.mapping_list, &qual_key, &qual_value, &pushdown);
			if (pushdown)
				break;
		}
	}

	/* Construct the SQL statement used for remote querying */
	initStringInfo(&sql);
	if (!is_blank_string(options.sql_query))
	{
		/* Use custom query if it's available */
		appendStringInfo(&sql, "%s", options.sql_query);
	}
	else
	{
		/* Get options.table */
		if (is_blank_string(schema_name))
		{
			appendStringInfo(&sql, "SELECT %s FROM %s%s%s", col_str.data,
							 (char *) quote_char.data, options.table, (char *) quote_char.data);
		}
		else
		{
			appendStringInfo(&sql, "SELECT %s FROM %s%s%s%s%s%s%s", col_str.data,
			 (char *) quote_char.data, schema_name, (char *) quote_char.data,
							 (char *) name_qualifier_char.data,
							 (char *) quote_char.data, options.table, (char *) quote_char.data);
		}
		if (pushdown)
		{
			appendStringInfo(&sql, " WHERE %s%s%s = '%s'",
							 (char *) quote_char.data, qual_key, (char *) quote_char.data, qual_value);
		}
	}

	/* Allocate a statement handle */
	SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);

#ifdef DEBUG
	elog(DEBUG1, "Executing query: %s", sql.data);
#endif

	/* Retrieve a list of rows */
	ret = SQLExecDirect(stmt, (SQLCHAR *) sql.data, SQL_NTS);
	check_return(ret, "Executing ODBC query", stmt, SQL_HANDLE_STMT);
	SQLNumResultCols(stmt, &result_columns);

	festate = (odbcFdwExecutionState *) palloc(sizeof(odbcFdwExecutionState));
	festate->attinmeta = TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);
	copy_odbcFdwOptions(&(festate->options), &options);
	festate->stmt = stmt;
	festate->table_columns = columns;
	festate->num_of_table_cols = num_of_columns;

	/*
	 * prepare for the first iteration, there will be some precalculation
	 * needed in the first iteration
	 */
	festate->first_iteration = TRUE;
	festate->encoding = encoding;
	node->fdw_state = (void *) festate;
}

/*
 * odbcIterateForeignScan
 *
 */
static TupleTableSlot *
odbcIterateForeignScan(ForeignScanState *node)
{
	EState	   *executor_state = node->ss.ps.state;
	MemoryContext prev_context;

	/* ODBC API return status */
	SQLRETURN	ret;
	odbcFdwExecutionState *festate = (odbcFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	SQLSMALLINT columns;
	char	  **values;
	HeapTuple	tuple;
	StringInfoData col_data;
	SQLHSTMT	stmt = festate->stmt;
	bool		first_iteration = festate->first_iteration;
	int			num_of_table_cols = festate->num_of_table_cols;
	int			num_of_result_cols;
	StringInfoData *table_columns = festate->table_columns;
	List	   *col_position_mask = NIL;
	List	   *col_size_array = NIL;
	List	   *col_conversion_array = NIL;

#ifdef DEBUG
	elog(DEBUG1, "odbcIterateForeignScan");
#endif

	ret = SQLFetch(stmt);

	SQLNumResultCols(stmt, &columns);

	/*
	 * If this is the first iteration, we need to calculate the mask for
	 * column mapping as well as the column size
	 */
	if (first_iteration == TRUE)
	{
		SQLCHAR    *ColumnName;
		SQLSMALLINT NameLengthPtr;
		SQLSMALLINT DataTypePtr;
		SQLULEN		ColumnSizePtr;
		SQLSMALLINT DecimalDigitsPtr;
		SQLSMALLINT NullablePtr;
		int			i;
		int			k;
		bool		found;

		StringInfoData sql_type;

		/*
		 * Allocate memory for the masks in a memory context that persists
		 * between IterateForeignScan calls
		 */
		prev_context = MemoryContextSwitchTo(executor_state->es_query_cxt);
		col_position_mask = NIL;
		col_size_array = NIL;
		col_conversion_array = NIL;
		num_of_result_cols = columns;
		/* Obtain the column information of the first row. */
		for (i = 1; i <= columns; i++)
		{
			ColumnConversion conversion = TEXT_CONVERSION;

			found = FALSE;
			ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);
			SQLDescribeCol(stmt,
						   i,	/* ColumnName */
						   ColumnName,
						   sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN,	/* BufferLength */
						   &NameLengthPtr,
						   &DataTypePtr,
						   &ColumnSizePtr,
						   &DecimalDigitsPtr,
						   &NullablePtr);

			sql_data_type(DataTypePtr, ColumnSizePtr, DecimalDigitsPtr, NullablePtr, &sql_type);
			if (strcmp("bytea", (char *) sql_type.data) == 0)
			{
				conversion = HEX_CONVERSION;
			}
			if (strcmp("boolean", (char *) sql_type.data) == 0)
			{
				conversion = BOOL_CONVERSION;
			}
			else if (strncmp("bit(", (char *) sql_type.data, 4) == 0 || strncmp("varbit(", (char *) sql_type.data, 7) == 0)
			{
				conversion = BIN_CONVERSION;
			}

			/* Get the position of the column in the FDW table */
			for (k = 0; k < num_of_table_cols; k++)
			{
				if (strcmp(table_columns[k].data, (char *) ColumnName) == 0)
				{
					SQLULEN		min_size = minimum_buffer_size(DataTypePtr);
					SQLULEN		max_size = MAXIMUM_BUFFER_SIZE;

					found = TRUE;
					col_position_mask = lappend_int(col_position_mask, k);
					if (ColumnSizePtr < min_size)
						ColumnSizePtr = min_size;
					if (ColumnSizePtr > max_size)
						ColumnSizePtr = max_size;

					col_size_array = lappend_int(col_size_array, (int) ColumnSizePtr);
					col_conversion_array = lappend_int(col_conversion_array, (int) conversion);
					break;
				}
			}
			/* if current column is not used by the foreign table */
			if (!found)
			{
				col_position_mask = lappend_int(col_position_mask, -1);
				col_size_array = lappend_int(col_size_array, -1);
				col_conversion_array = lappend_int(col_conversion_array, 0);
			}
			pfree(ColumnName);
		}
		festate->num_of_result_cols = num_of_result_cols;
		festate->col_position_mask = col_position_mask;
		festate->col_size_array = col_size_array;
		festate->col_conversion_array = col_conversion_array;
		festate->first_iteration = FALSE;

		MemoryContextSwitchTo(prev_context);
	}
	else
	{
		num_of_result_cols = festate->num_of_result_cols;
		col_position_mask = festate->col_position_mask;
		col_size_array = festate->col_size_array;
		col_conversion_array = festate->col_conversion_array;
	}

	ExecClearTuple(slot);
	if (SQL_SUCCEEDED(ret))
	{
		SQLSMALLINT i;

		values = (char **) palloc(sizeof(char *) * columns);

		/* Loop through the columns */
		for (i = 1; i <= columns; i++)
		{
			SQLLEN		indicator;
			char	   *buf;

			int			mask_index = i - 1;
			int			col_size = list_nth_int(col_size_array, mask_index);
			int			mapped_pos = list_nth_int(col_position_mask, mask_index);
			ColumnConversion conversion = list_nth_int(col_conversion_array, mask_index);

			/* Ignore this column if position is marked as invalid */
			if (mapped_pos == -1)
				continue;

			buf = (char *) palloc(sizeof(char) * (col_size + 1));

			/* retrieve column data as a zero-terminated string */

			/*
			 * TODO: binary fields (SQL_C_BIT, SQL_C_BINARY) do not have a
			 * trailing zero; they should be copied as now but without adding
			 * 1 to col_size, or using SQL_C_BIT or SQL_C_BINARY and then
			 * encoded into a binary PG literal (e.g. X'...' or B'...') For
			 * floating point types we should use SQL_C_FLOAT/SQL_C_DOUBLE to
			 * avoid precision loss. For date/time/timestamp these structures
			 * can be used:
			 * SQL_C_TYPE_DATE/SQL_C_TYPE_TIME/SQL_C_TYPE_TIMESTAMP. And
			 * finally, SQL_C_NUMERIC and SQL_C_GUID could also be used.
			 */
			buf[0] = 0;
			ret = SQLGetData(stmt, i, SQL_C_CHAR,
							 buf, sizeof(char) * (col_size + 1), &indicator);

			if (ret == SQL_SUCCESS_WITH_INFO)
			{
				SQLCHAR		sqlstate[5];

				SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlstate, NULL, NULL, 0, NULL);
				if (strcmp((char *) sqlstate, ODBC_SQLSTATE_FRACTIONAL_TRUNCATION) == 0)
				{
					/*
					 * Fractional truncation has occured; at this point we
					 * cannot obtain the lost digits
					 */
					if (buf[col_size])
					{
						/* The driver has omitted the trailing */
						char	   *buf2 = (char *) palloc(sizeof(char) * (col_size + 2));

						strncpy(buf2, buf, col_size + 1);
						buf2[col_size + 1] = 0;
						pfree(buf);
						buf = buf2;
					}
					elog(NOTICE, "Truncating number: %s", buf);
				}
				else
				{
					/*
					 * The output is incomplete, we need to obtain the rest of
					 * the data
					 */
					char	   *accum_buffer;
					size_t		accum_buffer_size;
					size_t		accum_used;

					if (indicator == SQL_NO_TOTAL)
					{
						/* Unknown total size, must copy part by part */
						accum_buffer_size = 0;
						accum_buffer = NULL;
						accum_used = 0;
						while (1)
						{
							size_t		buf_len = buf[col_size] ? col_size + 1 : col_size;

							/* Allocate new accumulation buffer if necessary */
							if (accum_used + buf_len > accum_buffer_size)
							{
								char	   *new_buff;

								accum_buffer_size = accum_buffer_size == 0 ? col_size * 2 : accum_buffer_size * 2;
								new_buff = (char *) palloc(sizeof(char) * (accum_buffer_size + 1));
								if (accum_buffer)
								{
									memmove(new_buff, accum_buffer, accum_used);
									pfree(accum_buffer);
								}
								accum_buffer = new_buff;
								accum_buffer[accum_used] = 0;
							}
							/* Copy part to the accumulation buffer */
							strncpy(accum_buffer + accum_used, buf, buf_len);
							accum_used += buf_len;
							accum_buffer[accum_used] = 0;
							/* Get new part */
							if (ret != SQL_SUCCESS_WITH_INFO)
								break;
							ret = SQLGetData(stmt, i, SQL_C_CHAR, buf, sizeof(char) * (col_size + 1), &indicator);
						};

					}
					else
					{
						/* We need to retrieve indicator more characters */
						size_t		buf_len = buf[col_size] ? col_size + 1 : col_size;

						accum_buffer_size = buf_len + indicator;
						accum_buffer = (char *) palloc(sizeof(char) * (accum_buffer_size + 1));
						strncpy(accum_buffer, buf, buf_len);
						accum_buffer[buf_len] = 0;
						ret = SQLGetData(stmt, i, SQL_C_CHAR, accum_buffer + buf_len, sizeof(char) * (indicator + 1), &indicator);
					}
					pfree(buf);
					buf = accum_buffer;
				}
			}

			if (SQL_SUCCEEDED(ret))
			{
				/* Handle null columns */
				if (indicator == SQL_NULL_DATA)
				{
					/*
					 * BuildTupleFromCStrings expects NULLs to be NULL
					 * pointers
					 */
					values[mapped_pos] = NULL;
				}
				else
				{
					if (festate->encoding != -1)
					{
						/* Convert character encoding */
						buf = pg_any_to_server(buf, strlen(buf), festate->encoding);
					}
					initStringInfo(&col_data);
					switch (conversion)
					{
						case TEXT_CONVERSION:
							appendStringInfoString(&col_data, buf);
							break;
						case HEX_CONVERSION:
							appendStringInfoString(&col_data, "\\x");
							appendStringInfoString(&col_data, buf);
							break;
						case BOOL_CONVERSION:
							if (buf[0] == 0)
								strcpy(buf, "F");
							else if (buf[0] == 1)
								strcpy(buf, "T");
							appendStringInfoString(&col_data, buf);
							break;
						case BIN_CONVERSION:
							ereport(ERROR,
									(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							   errmsg("Bit string columns are not supported")
									 ));
							break;
					}

					values[mapped_pos] = col_data.data;
				}
			}
			pfree(buf);
		}

		tuple = BuildTupleFromCStrings(festate->attinmeta, values);
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
		pfree(values);
	}

	return slot;
}

/*
 * odbcExplainForeignScan
 *
 */
static void
odbcExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	odbcFdwExecutionState *festate;
	unsigned int table_size = 0;

#ifdef DEBUG
	elog(DEBUG1, "odbcExplainForeignScan");
#endif

	festate = (odbcFdwExecutionState *) node->fdw_state;

	odbcGetTableSize(&(festate->options), &table_size);

	/* Suppress file size if we're not showing cost details */
	if (es->costs)
	{
		ExplainPropertyLong("Foreign Table Size", table_size, es);
	}
}

/*
 * odbcEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
odbcEndForeignScan(ForeignScanState *node)
{
	odbcFdwExecutionState *festate;

#ifdef DEBUG
	elog(DEBUG1, "odbcEndForeignScan");
#endif

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	festate = (odbcFdwExecutionState *) node->fdw_state;
	if (festate)
	{
		if (festate->stmt)
		{
			SQLFreeHandle(SQL_HANDLE_STMT, festate->stmt);
			festate->stmt = NULL;
		}
	}
}

/*
 * odbcReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
odbcReScanForeignScan(ForeignScanState *node)
{
#ifdef DEBUG
	elog(DEBUG1, "odbcReScanForeignScan");
#endif
}


static void
appendQuotedString(StringInfo buffer, const char *text)
{
	static const char SINGLE_QUOTE = '\'';
	const char *p;

	appendStringInfoChar(buffer, SINGLE_QUOTE);

	while (*text)
	{
		p = text;

		while (*p && *p != SINGLE_QUOTE)
		{
			p++;
		}
		appendBinaryStringInfo(buffer, text, p - text);
		if (*p == SINGLE_QUOTE)
		{
			appendStringInfoChar(buffer, SINGLE_QUOTE);
			appendStringInfoChar(buffer, SINGLE_QUOTE);
			p++;
		}
		text = p;
	}

	appendStringInfoChar(buffer, SINGLE_QUOTE);
}

static void
appendOption(StringInfo str, bool first, const char *option_name, const char *option_value)
{
	if (!first)
	{
		appendStringInfo(str, ",\n");
	}
	appendStringInfo(str, "\"%s\" ", option_name);
	appendQuotedString(str, option_value);
}

List *
odbcImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	/* TODO: review memory management in this function; any leaks? */
	odbcFdwOptions options;

	List	   *create_statements = NIL;
	List	   *tables = NIL;
	List	   *table_columns = NIL;
	ListCell   *tables_cell;
	ListCell   *table_columns_cell;
	RangeVar   *table_rangevar;

	SQLHENV		env;
	SQLHDBC		dbc;
	SQLHSTMT	query_stmt;
	SQLHSTMT	columns_stmt;
	SQLHSTMT	tables_stmt;
	SQLRETURN	ret;
	SQLSMALLINT result_columns;
	StringInfoData col_str;
	SQLCHAR    *ColumnName;
	SQLCHAR    *TableName;
	SQLSMALLINT NameLength;
	SQLSMALLINT DataType;
	SQLULEN		ColumnSize;
	SQLSMALLINT DecimalDigits;
	SQLSMALLINT Nullable;
	int			i;
	StringInfoData sql_type;
	SQLLEN		indicator;
	const char *schema_name;
	bool		missing_foreign_schema = FALSE;

#ifdef DEBUG
	elog(DEBUG1, "odbcImportForeignSchema");
#endif

	odbcGetOptions(serverOid, stmt->options, &options);

	schema_name = get_schema_name(&options);
	if (schema_name == NULL)
	{
		schema_name = stmt->remote_schema;
		missing_foreign_schema = TRUE;
	}
	else if (is_blank_string(schema_name))
	{
		/*
		 * This allows overriding and removing the schema, which is necessary
		 * for some schema-less ODBC data sources (e.g. Hive)
		 */
		schema_name = NULL;
	}

	if (!is_blank_string(options.sql_query))
	{
		/* Generate foreign table for a query */
		if (is_blank_string(options.table))
		{
			elog(ERROR, "Must provide 'table' option to name the foreign table");
		}

		odbc_connection(&options, &env, &dbc);

		/* Allocate a statement handle */
		SQLAllocHandle(SQL_HANDLE_STMT, dbc, &query_stmt);

		/* Retrieve a list of rows */
		ret = SQLExecDirect(query_stmt, (SQLCHAR *) options.sql_query, SQL_NTS);
		check_return(ret, "Executing ODBC query", query_stmt, SQL_HANDLE_STMT);

		SQLNumResultCols(query_stmt, &result_columns);

		initStringInfo(&col_str);
		ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);

		for (i = 1; i <= result_columns; i++)
		{
			SQLDescribeCol(query_stmt,
						   i,	/* ColumnName */
						   ColumnName,
						   sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN,	/* BufferLength */
						   &NameLength,
						   &DataType,
						   &ColumnSize,
						   &DecimalDigits,
						   &Nullable);

			sql_data_type(DataType, ColumnSize, DecimalDigits, Nullable, &sql_type);
			if (is_blank_string(sql_type.data))
			{
				elog(NOTICE, "Data type not supported (%d) for column %s", DataType, ColumnName);
				continue;
			}
			if (i > 1)
			{
				appendStringInfo(&col_str, ", ");
			}
			appendStringInfo(&col_str, "\"%s\" %s", ColumnName, (char *) sql_type.data);
		}
		SQLCloseCursor(query_stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, query_stmt);

		tables = lappend(tables, (void *) options.table);
		table_columns = lappend(table_columns, (void *) col_str.data);
	}
	else
	{
		/* Reflect one or more foreign tables */
		if (!is_blank_string(options.table))
		{
			tables = lappend(tables, (void *) options.table);
		}
		else if (stmt->list_type == FDW_IMPORT_SCHEMA_ALL || stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			/* Will obtain the foreign tables with SQLTables() */

			SQLCHAR    *table_schema = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_SCHEMA_NAME_LEN);

			odbc_connection(&options, &env, &dbc);

			/* Allocate a statement handle */
			SQLAllocHandle(SQL_HANDLE_STMT, dbc, &tables_stmt);

			ret = SQLTables(
							tables_stmt,
							NULL, 0,	/* Catalog:
										 * (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS
										 * would include also tables from
										 * internal catalogs */
							NULL, 0,	/* Schema: we avoid filtering by
										 * schema here to avoid problems with
										 * some drivers */
							NULL, 0,	/* Table */
							(SQLCHAR *) "TABLE", SQL_NTS		/* Type of table (we're
																 * not interested in
																 * views, temporary
																 * tables, etc.) */
				);
			check_return(ret, "Obtaining ODBC tables", tables_stmt, SQL_HANDLE_STMT);

			initStringInfo(&col_str);
			while (SQL_SUCCESS == ret)
			{
				ret = SQLFetch(tables_stmt);
				if (SQL_SUCCESS == ret)
				{
					int			excluded = FALSE;

					/*
					 * Some drivers (or possibly foreign sources) such as msodbc/SQLServer
					 * get upset if these columns are fetched out of order. So do
					 * catalog (if we're doing it), then Schema, then Table name.
					 */

					/*-------------------------------------------------------
					 * Since we haven't specified SQL_ALL_CATALOGS in the call
					 * to SQLTables we shouldn't get tables from special
					 * catalogs and only from the regular catalog of the
					 * database (the catalog name is usually the name of the
					 * database or blank, but depends on the driver and may
					 * vary, and can be obtained with: 
					 *   SQLCHAR *table_catalog = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_CATALOG_NAME_LEN); 
					 *   SQLGetData(tables_stmt, SQLTABLES_CATALOG_COLUMN, SQL_C_CHAR, table_catalog, MAXIMUM_CATALOG_NAME_LEN, &indicator);
					 *---------------------------------------------------------
					 */

					/*
					 * Since we're not filtering the SQLTables call by schema
					 * we must exclude here tables that belong to other
					 * schemas. For some ODBC drivers tables may not be
					 * organized into schemas and the schema of the table will
					 * be blank. So we only reject tables for which the schema
					 * is not blank and different from the desired schema:
					 */
					ret = SQLGetData(tables_stmt, SQLTABLES_SCHEMA_COLUMN, SQL_C_CHAR, table_schema, MAXIMUM_SCHEMA_NAME_LEN, &indicator);
					if (SQL_SUCCESS == ret)
					{
						if (!is_blank_string((char *) table_schema) && strcmp((char *) table_schema, schema_name))
						{
							excluded = TRUE;
						}
					}
					else
					{
						/*
						 * Some drivers don't support schemas and may return
						 * an error code here; in that case we must avoid
						 * using an schema to query the table columns.
						 */
						schema_name = NULL;
					}

					TableName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_TABLE_NAME_LEN);
					ret = SQLGetData(tables_stmt, SQLTABLES_NAME_COLUMN, SQL_C_CHAR, TableName, MAXIMUM_TABLE_NAME_LEN, &indicator);
					check_return(ret, "Reading table name", tables_stmt, SQL_HANDLE_STMT);

					/*
					 * And now we'll handle tables excluded by an EXCEPT
					 * clause
					 */
					if (!excluded && stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
					{
						foreach(tables_cell, stmt->table_list)
						{
							table_rangevar = (RangeVar *) lfirst(tables_cell);
							if (strcmp((char *) TableName, table_rangevar->relname) == 0)
							{
								excluded = TRUE;
							}
						}
					}

					if (!excluded)
					{
						tables = lappend(tables, (void *) TableName);
					}
				}
			}

			SQLCloseCursor(tables_stmt);

			SQLFreeHandle(SQL_HANDLE_STMT, tables_stmt);
		}
		else if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO)
		{
			foreach(tables_cell, stmt->table_list)
			{
				table_rangevar = (RangeVar *) lfirst(tables_cell);
				tables = lappend(tables, (void *) table_rangevar->relname);
			}
		}
		else
		{
			elog(ERROR, "Unknown list type in IMPORT FOREIGN SCHEMA");
		}
		foreach(tables_cell, tables)
		{
			char	   *table_name = (char *) lfirst(tables_cell);

			odbc_connection(&options, &env, &dbc);

			/* Allocate a statement handle */
			SQLAllocHandle(SQL_HANDLE_STMT, dbc, &columns_stmt);

			ret = SQLColumns(
							 columns_stmt,
							 NULL, 0,
							 (SQLCHAR *) schema_name, SQL_NTS,
							 (SQLCHAR *) table_name, SQL_NTS,
							 NULL, 0
				);
			check_return(ret, "Obtaining ODBC columns", columns_stmt, SQL_HANDLE_STMT);

			i = 0;
			initStringInfo(&col_str);
			ColumnName = (SQLCHAR *) palloc(sizeof(SQLCHAR) * MAXIMUM_COLUMN_NAME_LEN);
			while (SQL_SUCCESS == ret)
			{
				ret = SQLFetch(columns_stmt);
				if (SQL_SUCCESS == ret)
				{
					ret = SQLGetData(columns_stmt, 4, SQL_C_CHAR, ColumnName, MAXIMUM_COLUMN_NAME_LEN, &indicator);

					/*
					 * check_return(ret, "Reading column name", columns_stmt,
					 * SQL_HANDLE_STMT);
					 */
					ret = SQLGetData(columns_stmt, 5, SQL_C_SSHORT, &DataType, MAXIMUM_COLUMN_NAME_LEN, &indicator);

					/*
					 * check_return(ret, "Reading column type", columns_stmt,
					 * SQL_HANDLE_STMT);
					 */
					ret = SQLGetData(columns_stmt, 7, SQL_C_SLONG, &ColumnSize, 0, &indicator);

					/*
					 * check_return(ret, "Reading column size", columns_stmt,
					 * SQL_HANDLE_STMT);
					 */
					ret = SQLGetData(columns_stmt, 9, SQL_C_SSHORT, &DecimalDigits, 0, &indicator);

					/*
					 * check_return(ret, "Reading column decimals",
					 * columns_stmt, SQL_HANDLE_STMT);
					 */
					ret = SQLGetData(columns_stmt, 11, SQL_C_SSHORT, &Nullable, 0, &indicator);

					/*
					 * check_return(ret, "Reading column nullable",
					 * columns_stmt, SQL_HANDLE_STMT);
					 */
					sql_data_type(DataType, ColumnSize, DecimalDigits, Nullable, &sql_type);
					if (is_blank_string(sql_type.data))
					{
						elog(NOTICE, "Data type not supported (%d) for column %s", DataType, ColumnName);
						continue;
					}
					if (++i > 1)
					{
						appendStringInfo(&col_str, ", ");
					}
					appendStringInfo(&col_str, "\"%s\" %s", ColumnName, (char *) sql_type.data);
				}
			}
			SQLCloseCursor(columns_stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, columns_stmt);
			table_columns = lappend(table_columns, (void *) col_str.data);
		}
	}

	/* Generate create statements */
	table_columns_cell = list_head(table_columns);
	foreach(tables_cell, tables)
	{
		/* temporarily define vars here... */
		char	   *table_name = (char *) lfirst(tables_cell);
		char	   *columns = (char *) lfirst(table_columns_cell);
		StringInfoData create_statement;
		ListCell   *option;
		int			option_count = 0;
		const char *prefix = empty_string_if_null(options.prefix);

		table_columns_cell = lnext(table_columns_cell);

		initStringInfo(&create_statement);
		appendStringInfo(&create_statement, "CREATE FOREIGN TABLE \"%s\".\"%s%s\" (", stmt->local_schema, prefix, (char *) table_name);
		appendStringInfo(&create_statement, "%s", columns);
		appendStringInfo(&create_statement, ") SERVER %s\n", stmt->server_name);
		appendStringInfo(&create_statement, "OPTIONS (\n");
		foreach(option, stmt->options)
		{
			DefElem    *def = (DefElem *) lfirst(option);

			appendOption(&create_statement, ++option_count == 1, def->defname, defGetString(def));
		}
		if (is_blank_string(options.table))
		{
			appendOption(&create_statement, ++option_count == 1, "table", table_name);
		}
		if (missing_foreign_schema)
		{
			appendOption(&create_statement, ++option_count == 1, "schema", schema_name);
		}
		appendStringInfo(&create_statement, ");");
		elog(DEBUG1, "CREATE: %s", create_statement.data);
		create_statements = lappend(create_statements, (void *) create_statement.data);
	}

	return create_statements;
}
