# Verilator testbenches. Enable with -DDROMOS_VERILATOR=ON.
# Verilator >= 5.0 is required; set VERILATOR_ROOT if it is not on the default path.
find_package(verilator 5.0 REQUIRED HINTS $ENV{VERILATOR_ROOT} /opt/verilator/share/verilator
             /usr/local/share/verilator /usr/share/verilator)

set(RTL ${PROJECT_SOURCE_DIR}/rtl)

function(dromos_tb name top)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE dromos_ref)
  # Verilator's generated code is not ours to lint.
  target_compile_options(${name} PRIVATE -Wno-conversion -Wno-shadow -Wno-pedantic -Wno-unused-parameter)
  verilate(${name}
    SOURCES ${ARGN}
    TOP_MODULE ${top}
    PREFIX V${top}
    TRACE_FST
    VERILATOR_ARGS -Wall --x-assign unique --x-initial unique -O3)
endfunction()

dromos_tb(tb_book_bitmap book_bitmap ${RTL}/engine/book_bitmap.sv)
add_test(NAME rtl_book_bitmap COMMAND tb_book_bitmap 200000 1)
