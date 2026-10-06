# tinyactors for PostgreSQL (PGXS). The tinyactors libraries are built as
# position-independent static libraries (build/addon/lib, mise/core.rb) and
# linked in:
#
#   mise run pg:build      (or: make PG_CONFIG=/path/to/pg_config TA_BUILD=...)
#   mise run pg:test       a temporary server, pg_regress + concurrency tests
#
EXTENSION   = tinyactors
MODULE_big  = tinyactors
OBJS        = src/tinyactors.o
DATA        = sql/tinyactors--0.1.0.sql
REGRESS     = basics timers outbox errors versions invoke invoke_src storelink
REGRESS_OPTS = --inputdir=test --outputdir=build/regress --load-extension=tinyactors

REPO        := $(abspath ../..)
TA_BUILD    ?= $(REPO)/build/addon/lib
TA_LIBS     = $(TA_BUILD)/libtinyactors_host.a $(TA_BUILD)/libtinyactors_system.a $(TA_BUILD)/libtinyactors_core.a $(TA_BUILD)/libtinyactors_compile.a \
              $(TA_BUILD)/libtinyactors_datamodel_ecmascript.a $(TA_BUILD)/libtinyactors_image.a \
              $(TA_BUILD)/libtinyactors_base.a $(TA_BUILD)/libtinyactors_quickjs.a
PG_CPPFLAGS = -I$(REPO)/include
PG_CFLAGS   = -std=gnu11 -Wno-declaration-after-statement -Werror
SHLIB_LINK  = $(TA_LIBS) -lm
EXTRA_CLEAN = build

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

src/tinyactors.o: $(TA_LIBS)
