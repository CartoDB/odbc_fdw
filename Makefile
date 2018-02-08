##########################################################################
#
#                foreign-data wrapper for ODBC
#
# Copyright (c) 2011, PostgreSQL Global Development Group
# Copyright (c) 2016, CARTO
#
# This software is released under the PostgreSQL Licence
#
# Author: Zheng Yang <zhengyang4k@gmail.com>
#
# IDENTIFICATION
#                 odbc_fdw/Makefile
#
##########################################################################

MODULE_big = odbc_fdw
OBJS = odbc_fdw.o
PGVERSION ?= 9.5

EXTENSION = odbc_fdw
DATA = odbc_fdw--0.0.1.sql \
  odbc_fdw--0.1.0.sql \
  odbc_fdw--0.2.0.sql \
  odbc_fdw--0.0.1--0.1.0.sql \
  odbc_fdw--0.1.0--0.0.1.sql \
  odbc_fdw--0.1.0--0.2.0.sql \
  odbc_fdw--0.2.0--0.1.0.sql

TEST_DIR = test/
REGRESS = $(notdir $(basename $(sort $(wildcard $(TEST_DIR)/sql/*test.sql))))
REGRESS_OPTS = --inputdir='$(TEST_DIR)' --outputdir='$(TEST_DIR)' --user='postgres' --load-extension=odbc_fdw

SHLIB_LINK = -lodbc

ifdef DEBUG
override CFLAGS += -DDEBUG -g -O0
endif

PG_CONFIG = /usr/lib/postgresql/$(PGVERSION)/bin/pg_config
#PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

GENERATED_SQL_FILES = $(wildcard $(TEST_DIR)/sql/*.sql)

integration_tests:
	bash test/scripts/tests-generator.sh
	make installcheck
