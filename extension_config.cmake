# This file is included by DuckDB's build system. It specifies which extension to load

# DONT_LINK is required, not a preference.
#
# storage_compat embeds a COMPLETE second DuckDB. Linking it statically into libduckdb
# would put two DuckDBs in one library, and the linker says so - every static data
# member is defined twice:
#
#   multiple definition of `duckdb::LogicalType::VARCHAR'
#   multiple definition of `duckdb::DConstants::INVALID_INDEX'
#   multiple definition of `duckdb::TableCatalogEntry::Name'
#   ... (const data in COMDAT groups; symbol localization cannot help here, and
#        should not - the static link is simply not a meaningful thing to do for
#        this extension)
#
# As a loadable extension there is no conflict: the embedded engine lives inside the
# .duckdb_extension and reaches the host only through the sc:: boundary.
duckdb_extension_load(storage_compat
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    DONT_LINK
)
