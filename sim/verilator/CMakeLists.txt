# Verilator testbenches. Enable with -DDROMOS_VERILATOR=ON.
# Verilator >= 5.0 is required; set VERILATOR_ROOT if it is not on the default path.
find_package(verilator 5.0 REQUIRED HINTS $ENV{VERILATOR_ROOT} /opt/verilator/share/verilator
             /usr/local/share/verilator /usr/share/verilator)

set(RTL ${PROJECT_SOURCE_DIR}/rtl)

# dromos_tb(<exe> <source.cpp> <top> [ARGS <verilator args>...] SOURCES <rtl files>...)
function(dromos_tb name src top)
  cmake_parse_arguments(TB "" "" "ARGS;SOURCES;DEFS" ${ARGN})
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE dromos_ref)
  # Verilator's generated code is not ours to lint.
  target_compile_options(${name} PRIVATE -Wno-conversion -Wno-shadow -Wno-pedantic -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-maybe-uninitialized)
  target_compile_definitions(${name} PRIVATE ${TB_DEFS})
  verilate(${name}
    SOURCES ${TB_SOURCES}
    TOP_MODULE ${top}
    PREFIX V${name}
    TRACE_FST
    VERILATOR_ARGS -Wall --x-assign unique --x-initial unique -O3 ${TB_ARGS})
endfunction()

dromos_tb(tb_book_bitmap tb_book_bitmap.cpp book_bitmap
  SOURCES ${RTL}/engine/book_bitmap.sv
  DEFS TB_HEADER="Vtb_book_bitmap.h")
add_test(NAME rtl_book_bitmap COMMAND tb_book_bitmap 200000 1)

# book_core, full-size pool (the default engine configuration).
dromos_tb(tb_book_core tb_book_core.cpp book_core
  ARGS -GNSYM=4 -GPOOL=65536
  SOURCES ${RTL}/common/dromos_pkg.sv ${RTL}/engine/book_core.sv
  DEFS TB_HEADER="Vtb_book_core.h" TB_MODEL=Vtb_book_core TB_NSYM=4 TB_POOL=65536)
add_test(NAME rtl_book_core COMMAND tb_book_core --seeds 4 --msgs 20000)
add_test(NAME rtl_book_core_backpressure COMMAND tb_book_core --seeds 2 --first 11 --msgs 10000 --ready 40)

# book_core with a 64-slot pool: pool-full rejects and heavy slot reuse.
dromos_tb(tb_book_core_small tb_book_core.cpp book_core
  ARGS -GNSYM=2 -GPOOL=64
  SOURCES ${RTL}/common/dromos_pkg.sv ${RTL}/engine/book_core.sv
  DEFS TB_HEADER="Vtb_book_core_small.h" TB_MODEL=Vtb_book_core_small TB_NSYM=2 TB_POOL=64)
add_test(NAME rtl_book_core_small_pool COMMAND tb_book_core_small --seeds 4 --msgs 20000 --ready 70)

# Directed scenarios from test_engine, replayed through book_core with the
# same engine configuration the scenarios use (4 symbols, 1024-slot pool).
dromos_tb(tb_book_core_directed tb_book_core.cpp book_core
  ARGS -GNSYM=4 -GPOOL=1024
  SOURCES ${RTL}/common/dromos_pkg.sv ${RTL}/engine/book_core.sv
  DEFS TB_HEADER="Vtb_book_core_directed.h" TB_MODEL=Vtb_book_core_directed TB_NSYM=4 TB_POOL=1024)
add_test(NAME rtl_directed_record
  COMMAND ${CMAKE_COMMAND} -E env DROMOS_RECORD=${CMAKE_CURRENT_BINARY_DIR}/directed.rec $<TARGET_FILE:test_engine>)
set_tests_properties(rtl_directed_record PROPERTIES FIXTURES_SETUP directed_rec)
add_test(NAME rtl_book_core_directed
  COMMAND tb_book_core_directed --replay ${CMAKE_CURRENT_BINARY_DIR}/directed.rec)
add_test(NAME rtl_book_core_directed_backpressure
  COMMAND tb_book_core_directed --replay ${CMAKE_CURRENT_BINARY_DIR}/directed.rec --ready 30)
set_tests_properties(rtl_book_core_directed rtl_book_core_directed_backpressure
  PROPERTIES FIXTURES_REQUIRED directed_rec)

# Same scenarios on a 2-slot pool (slot_reuse_bumps_generation's own config):
# every other scenario then runs into pool-full rejects as well.
dromos_tb(tb_book_core_pool2 tb_book_core.cpp book_core
  ARGS -GNSYM=4 -GPOOL=2
  SOURCES ${RTL}/common/dromos_pkg.sv ${RTL}/engine/book_core.sv
  DEFS TB_HEADER="Vtb_book_core_pool2.h" TB_MODEL=Vtb_book_core_pool2 TB_NSYM=4 TB_POOL=2)
add_test(NAME rtl_book_core_directed_pool2
  COMMAND tb_book_core_pool2 --replay ${CMAKE_CURRENT_BINARY_DIR}/directed.rec --ready 50)
set_tests_properties(rtl_book_core_directed_pool2 PROPERTIES FIXTURES_REQUIRED directed_rec)
