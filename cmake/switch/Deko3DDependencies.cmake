# Copyright 2026 Dolphin Emulator Project
# Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

include(FetchContent)
find_package(Git REQUIRED)

function(cemu_deko_apply_patch source patch)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --check "${patch}"
    WORKING_DIRECTORY "${source}"
    RESULT_VARIABLE check_result OUTPUT_QUIET ERROR_QUIET)
  if(check_result EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change "${patch}"
      WORKING_DIRECTORY "${source}"
      RESULT_VARIABLE apply_result)
    if(NOT apply_result EQUAL 0)
      message(FATAL_ERROR "Failed to apply ${patch}")
    endif()
  else()
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --ignore-space-change --reverse --check "${patch}"
      WORKING_DIRECTORY "${source}"
      RESULT_VARIABLE reverse_result OUTPUT_QUIET ERROR_QUIET)
    if(NOT reverse_result EQUAL 0)
      message(FATAL_ERROR "Shader compiler source does not match ${patch}")
    endif()
  endif()
endfunction()

FetchContent_Declare(cemu_uam
  GIT_REPOSITORY https://github.com/PalindromicBreadLoaf/uam.git
  GIT_TAG c0d76aa9107d67edcd35a1b717ace87ee7939c68
)
FetchContent_MakeAvailable(cemu_uam)
set(UAM_ROOT "${cemu_uam_SOURCE_DIR}" CACHE PATH "Root of the pinned uam checkout")

if(NOT EXISTS "${UAM_ROOT}/source/compiler_iface.cpp")
  message(FATAL_ERROR
    "UAM source checkout is incomplete: ${UAM_ROOT}")
endif()

cemu_deko_apply_patch("${UAM_ROOT}"
  "${CMAKE_CURRENT_LIST_DIR}/patches/uam-32-vertex-attributes.patch")

find_program(UAM_BISON NAMES bison)
find_program(UAM_FLEX NAMES flex)
find_program(UAM_PYTHON NAMES python3 python)
foreach(_tool UAM_BISON UAM_FLEX UAM_PYTHON)
  if(NOT ${_tool})
    message(FATAL_ERROR "${_tool} not found. bison, flex and python3 are needed to build uam.")
  endif()
endforeach()

execute_process(COMMAND "${UAM_PYTHON}" -c "import mako.template"
  RESULT_VARIABLE _uam_mako_result ERROR_QUIET)
if(NOT _uam_mako_result EQUAL 0)
  message(FATAL_ERROR "uam requires Mako for ${UAM_PYTHON}; install python-mako or pip install Mako")
endif()

set(_uam_gen "${CMAKE_BINARY_DIR}/uam-generated")
file(MAKE_DIRECTORY "${_uam_gen}/glsl/glcpp")

add_custom_command(
  OUTPUT "${_uam_gen}/glsl/glsl_parser.cpp" "${_uam_gen}/glsl/glsl_parser.h"
  COMMAND "${UAM_BISON}" -o "${_uam_gen}/glsl/glsl_parser.cpp" -p _mesa_glsl_
          --defines="${_uam_gen}/glsl/glsl_parser.h" "${UAM_ROOT}/mesa-imported/glsl/glsl_parser.yy"
  DEPENDS "${UAM_ROOT}/mesa-imported/glsl/glsl_parser.yy"
  COMMENT "Generating uam GLSL parser")

add_custom_command(
  OUTPUT "${_uam_gen}/glsl/glsl_lexer.cpp"
  COMMAND "${UAM_FLEX}" -o "${_uam_gen}/glsl/glsl_lexer.cpp"
          "${UAM_ROOT}/mesa-imported/glsl/glsl_lexer.ll"
  DEPENDS "${UAM_ROOT}/mesa-imported/glsl/glsl_lexer.ll"
  COMMENT "Generating uam GLSL lexer")

add_custom_command(
  OUTPUT "${_uam_gen}/glsl/glcpp/glcpp-parse.c" "${_uam_gen}/glsl/glcpp/glcpp-parse.h"
  COMMAND "${UAM_BISON}" -o "${_uam_gen}/glsl/glcpp/glcpp-parse.c" -p glcpp_parser_
          --defines="${_uam_gen}/glsl/glcpp/glcpp-parse.h"
          "${UAM_ROOT}/mesa-imported/glsl/glcpp/glcpp-parse.y"
  DEPENDS "${UAM_ROOT}/mesa-imported/glsl/glcpp/glcpp-parse.y"
  COMMENT "Generating uam GLSL preprocessor parser")

add_custom_command(
  OUTPUT "${_uam_gen}/glsl/glcpp/glcpp-lex.c"
  COMMAND "${UAM_FLEX}" -o "${_uam_gen}/glsl/glcpp/glcpp-lex.c"
          "${UAM_ROOT}/mesa-imported/glsl/glcpp/glcpp-lex.l"
  DEPENDS "${UAM_ROOT}/mesa-imported/glsl/glcpp/glcpp-lex.l"
  COMMENT "Generating uam GLSL preprocessor lexer")

foreach(_variant enum constant strings)
  if(_variant STREQUAL "enum")
    set(_out "${_uam_gen}/glsl/ir_expression_operation.h")
  else()
    set(_out "${_uam_gen}/glsl/ir_expression_operation_${_variant}.h")
  endif()
  add_custom_command(
    OUTPUT "${_out}"
    COMMAND "${UAM_PYTHON}" "${UAM_ROOT}/mesa-imported/glsl/ir_expression_operation.py" ${_variant}
            > "${_out}"
    DEPENDS "${UAM_ROOT}/mesa-imported/glsl/ir_expression_operation.py"
    COMMENT "Generating uam ${_out}")
  list(APPEND _uam_generated "${_out}")
endforeach()

list(APPEND _uam_generated
  "${_uam_gen}/glsl/glsl_parser.cpp"
  "${_uam_gen}/glsl/glsl_lexer.cpp"
  "${_uam_gen}/glsl/glcpp/glcpp-parse.c"
  "${_uam_gen}/glsl/glcpp/glcpp-lex.c"
)

set(_uam_sources
  source/compiler_iface.cpp
  source/glsl_frontend.cpp
  source/mini-os.c
  source/tgsi_support.cpp

  mesa-imported/codegen/nv50_ir.cpp
  mesa-imported/codegen/nv50_ir_bb.cpp
  mesa-imported/codegen/nv50_ir_build_util.cpp
  mesa-imported/codegen/nv50_ir_emit_gk110.cpp
  mesa-imported/codegen/nv50_ir_emit_gm107.cpp
  mesa-imported/codegen/nv50_ir_emit_nv50.cpp
  mesa-imported/codegen/nv50_ir_emit_nvc0.cpp
  mesa-imported/codegen/nv50_ir_from_tgsi.cpp
  mesa-imported/codegen/nv50_ir_graph.cpp
  mesa-imported/codegen/nv50_ir_lowering_gm107.cpp
  mesa-imported/codegen/nv50_ir_lowering_nv50.cpp
  mesa-imported/codegen/nv50_ir_lowering_nvc0.cpp
  mesa-imported/codegen/nv50_ir_peephole.cpp
  mesa-imported/codegen/nv50_ir_print.cpp
  mesa-imported/codegen/nv50_ir_ra.cpp
  mesa-imported/codegen/nv50_ir_ssa.cpp
  mesa-imported/codegen/nv50_ir_target.cpp
  mesa-imported/codegen/nv50_ir_target_gm107.cpp
  mesa-imported/codegen/nv50_ir_target_nv50.cpp
  mesa-imported/codegen/nv50_ir_target_nvc0.cpp
  mesa-imported/codegen/nv50_ir_util.cpp

  mesa-imported/compiler/blob.c
  mesa-imported/compiler/glsl_types.cpp
  mesa-imported/compiler/shader_enums.c

  mesa-imported/cso_cache/cso_cache.c
  mesa-imported/cso_cache/cso_hash.c

  mesa-imported/glsl/ast_array_index.cpp
  mesa-imported/glsl/ast_expr.cpp
  mesa-imported/glsl/ast_function.cpp
  mesa-imported/glsl/ast_to_hir.cpp
  mesa-imported/glsl/ast_type.cpp
  mesa-imported/glsl/builtin_functions.cpp
  mesa-imported/glsl/builtin_types.cpp
  mesa-imported/glsl/builtin_variables.cpp
  mesa-imported/glsl/generate_ir.cpp
  mesa-imported/glsl/glsl_parser_extras.cpp
  mesa-imported/glsl/glsl_symbol_table.cpp
  mesa-imported/glsl/hir_field_selection.cpp
  mesa-imported/glsl/ir.cpp
  mesa-imported/glsl/ir_array_refcount.cpp
  mesa-imported/glsl/ir_basic_block.cpp
  mesa-imported/glsl/ir_builder.cpp
  mesa-imported/glsl/ir_clone.cpp
  mesa-imported/glsl/ir_constant_expression.cpp
  mesa-imported/glsl/ir_equals.cpp
  mesa-imported/glsl/ir_expression_flattening.cpp
  mesa-imported/glsl/ir_function.cpp
  mesa-imported/glsl/ir_function_can_inline.cpp
  mesa-imported/glsl/ir_function_detect_recursion.cpp
  mesa-imported/glsl/ir_hierarchical_visitor.cpp
  mesa-imported/glsl/ir_hv_accept.cpp
  mesa-imported/glsl/ir_print_visitor.cpp
  mesa-imported/glsl/ir_reader.cpp
  mesa-imported/glsl/ir_rvalue_visitor.cpp
  mesa-imported/glsl/ir_set_program_inouts.cpp
  mesa-imported/glsl/ir_validate.cpp
  mesa-imported/glsl/ir_variable_refcount.cpp
  mesa-imported/glsl/link_atomics.cpp
  mesa-imported/glsl/link_functions.cpp
  mesa-imported/glsl/link_interface_blocks.cpp
  mesa-imported/glsl/link_uniform_block_active_visitor.cpp
  mesa-imported/glsl/link_uniform_blocks.cpp
  mesa-imported/glsl/link_uniform_initializers.cpp
  mesa-imported/glsl/link_uniforms.cpp
  mesa-imported/glsl/link_varyings.cpp
  mesa-imported/glsl/linker.cpp
  mesa-imported/glsl/linker_util.cpp
  mesa-imported/glsl/loop_analysis.cpp
  mesa-imported/glsl/loop_unroll.cpp
  mesa-imported/glsl/lower_blend_equation_advanced.cpp
  mesa-imported/glsl/lower_buffer_access.cpp
  mesa-imported/glsl/lower_const_arrays_to_uniforms.cpp
  mesa-imported/glsl/lower_cs_derived.cpp
  mesa-imported/glsl/lower_discard.cpp
  mesa-imported/glsl/lower_discard_flow.cpp
  mesa-imported/glsl/lower_distance.cpp
  mesa-imported/glsl/lower_if_to_cond_assign.cpp
  mesa-imported/glsl/lower_instructions.cpp
  mesa-imported/glsl/lower_int64.cpp
  mesa-imported/glsl/lower_jumps.cpp
  mesa-imported/glsl/lower_mat_op_to_vec.cpp
  mesa-imported/glsl/lower_named_interface_blocks.cpp
  mesa-imported/glsl/lower_noise.cpp
  mesa-imported/glsl/lower_offset_array.cpp
  mesa-imported/glsl/lower_output_reads.cpp
  mesa-imported/glsl/lower_packed_varyings.cpp
  mesa-imported/glsl/lower_packing_builtins.cpp
  mesa-imported/glsl/lower_shared_reference.cpp
  mesa-imported/glsl/lower_subroutine.cpp
  mesa-imported/glsl/lower_tess_level.cpp
  mesa-imported/glsl/lower_texture_projection.cpp
  mesa-imported/glsl/lower_ubo_reference.cpp
  mesa-imported/glsl/lower_variable_index_to_cond_assign.cpp
  mesa-imported/glsl/lower_vec_index_to_cond_assign.cpp
  mesa-imported/glsl/lower_vec_index_to_swizzle.cpp
  mesa-imported/glsl/lower_vector.cpp
  mesa-imported/glsl/lower_vector_derefs.cpp
  mesa-imported/glsl/lower_vector_insert.cpp
  mesa-imported/glsl/lower_vertex_id.cpp
  mesa-imported/glsl/opt_algebraic.cpp
  mesa-imported/glsl/opt_array_splitting.cpp
  mesa-imported/glsl/opt_conditional_discard.cpp
  mesa-imported/glsl/opt_constant_folding.cpp
  mesa-imported/glsl/opt_constant_propagation.cpp
  mesa-imported/glsl/opt_constant_variable.cpp
  mesa-imported/glsl/opt_copy_propagation_elements.cpp
  mesa-imported/glsl/opt_dead_builtin_variables.cpp
  mesa-imported/glsl/opt_dead_builtin_varyings.cpp
  mesa-imported/glsl/opt_dead_code.cpp
  mesa-imported/glsl/opt_dead_code_local.cpp
  mesa-imported/glsl/opt_dead_functions.cpp
  mesa-imported/glsl/opt_flatten_nested_if_blocks.cpp
  mesa-imported/glsl/opt_flip_matrices.cpp
  mesa-imported/glsl/opt_function_inlining.cpp
  mesa-imported/glsl/opt_if_simplification.cpp
  mesa-imported/glsl/opt_minmax.cpp
  mesa-imported/glsl/opt_rebalance_tree.cpp
  mesa-imported/glsl/opt_redundant_jumps.cpp
  mesa-imported/glsl/opt_structure_splitting.cpp
  mesa-imported/glsl/opt_swizzle.cpp
  mesa-imported/glsl/opt_tree_grafting.cpp
  mesa-imported/glsl/opt_vectorize.cpp
  mesa-imported/glsl/propagate_invariance.cpp
  mesa-imported/glsl/s_expression.cpp
  mesa-imported/glsl/serialize.cpp
  mesa-imported/glsl/standalone_scaffolding.cpp
  mesa-imported/glsl/string_to_uint_map.cpp
  mesa-imported/glsl/glcpp/pp.c

  mesa-imported/main/imports.c
  mesa-imported/main/shaderimage.c
  mesa-imported/main/uniform_query.cpp
  mesa-imported/main/uniforms.c

  mesa-imported/program/ir_to_mesa.cpp
  mesa-imported/program/prog_instruction.c
  mesa-imported/program/prog_parameter.c
  mesa-imported/program/symbol_table.c

  mesa-imported/state_tracker/st_format.c
  mesa-imported/state_tracker/st_glsl_to_tgsi.cpp
  mesa-imported/state_tracker/st_glsl_to_tgsi_array_merge.cpp
  mesa-imported/state_tracker/st_glsl_to_tgsi_private.cpp
  mesa-imported/state_tracker/st_glsl_to_tgsi_temprename.cpp
  mesa-imported/state_tracker/st_glsl_types.cpp

  mesa-imported/tgsi/tgsi_aa_point.c
  mesa-imported/tgsi/tgsi_build.c
  mesa-imported/tgsi/tgsi_dump.c
  mesa-imported/tgsi/tgsi_emulate.c
  mesa-imported/tgsi/tgsi_from_mesa.c
  mesa-imported/tgsi/tgsi_info.c
  mesa-imported/tgsi/tgsi_iterate.c
  mesa-imported/tgsi/tgsi_lowering.c
  mesa-imported/tgsi/tgsi_parse.c
  mesa-imported/tgsi/tgsi_point_sprite.c
  mesa-imported/tgsi/tgsi_sanity.c
  mesa-imported/tgsi/tgsi_scan.c
  mesa-imported/tgsi/tgsi_strings.c
  mesa-imported/tgsi/tgsi_text.c
  mesa-imported/tgsi/tgsi_transform.c
  mesa-imported/tgsi/tgsi_two_side.c
  mesa-imported/tgsi/tgsi_ureg.c
  mesa-imported/tgsi/tgsi_util.c

  mesa-imported/util/bitscan.c
  mesa-imported/util/half_float.c
  mesa-imported/util/hash_table.c
  mesa-imported/util/ralloc.c
  mesa-imported/util/set.c
  mesa-imported/util/string_buffer.c
  mesa-imported/util/strtod.c
  mesa-imported/util/u_bitmask.c
  mesa-imported/util/u_debug.c
  mesa-imported/util/u_format.c
)
list(TRANSFORM _uam_sources PREPEND "${UAM_ROOT}/")

set(_uam_bridge "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Deko3D/UamBridge.cpp")
set(_uam_stderr "${CMAKE_SOURCE_DIR}/src/Cafe/HW/Latte/Renderer/Deko3D/UamStderr.h")

add_library(uam_objects STATIC ${_uam_sources} ${_uam_generated} "${_uam_bridge}" "${_uam_stderr}")

target_include_directories(uam_objects PRIVATE
  "${_uam_gen}"
  "${_uam_gen}/glsl"
  "${UAM_ROOT}/mesa-imported"
  "${UAM_ROOT}/source"
  "${CMAKE_SOURCE_DIR}/src"
)

target_compile_definitions(uam_objects PRIVATE
  PACKAGE_STRING="uam"
  DESKTOP
  _USE_MATH_DEFINES
  _GNU_SOURCE
  HAVE_POSIX_MEMALIGN
  NDEBUG
)

set_target_properties(uam_objects PROPERTIES
  C_STANDARD 99
  C_EXTENSIONS ON
  CXX_STANDARD 11
  CXX_EXTENSIONS ON
  INTERPROCEDURAL_OPTIMIZATION OFF
)
target_compile_options(uam_objects PRIVATE -w -fno-lto -fno-strict-aliasing -include "${_uam_stderr}")

set_source_files_properties("${UAM_ROOT}/source/compiler_iface.cpp" PROPERTIES
  COMPILE_DEFINITIONS "glsl_frontend_init=UamFrontendInitOnce;glsl_frontend_exit=UamFrontendExitOnce")
set_source_files_properties("${UAM_ROOT}/source/glsl_frontend.cpp" PROPERTIES
  COMPILE_DEFINITIONS "_mesa_glsl_compile_shader=UamCompileShader")

set(_uam_isolated "${CMAKE_BINARY_DIR}/uam-generated/uam_isolated.o")
add_custom_command(
  OUTPUT "${_uam_isolated}"
  COMMAND "${CMAKE_LINKER}" -r --force-group-allocation
          --whole-archive "$<TARGET_FILE:uam_objects>" --no-whole-archive
          -o "${_uam_isolated}.tmp"
  COMMAND "${CMAKE_OBJCOPY}" --keep-global-symbol=UamCompileGlsl "${_uam_isolated}.tmp"
          "${_uam_isolated}"
  COMMAND "${CMAKE_COMMAND}" -E rm -f "${_uam_isolated}.tmp"
  DEPENDS uam_objects
  COMMENT "Isolating the native shader compiler")
add_custom_target(uam_isolate DEPENDS "${_uam_isolated}")

add_library(uam INTERFACE)
add_dependencies(uam uam_isolate)
target_link_libraries(uam INTERFACE "${_uam_isolated}")

find_path(DEKO3D_INCLUDE_DIR deko3d.hpp PATHS "${DEVKITPRO}/libnx/include" REQUIRED)
find_library(DEKO3D_LIBRARY deko3d PATHS "${DEVKITPRO}/libnx/lib" REQUIRED)
add_library(deko3d INTERFACE)
target_include_directories(deko3d INTERFACE "${DEKO3D_INCLUDE_DIR}")
target_compile_definitions(deko3d INTERFACE DK_HPP_SUPPORT_VECTOR)
target_link_libraries(deko3d INTERFACE "${DEKO3D_LIBRARY}")

FetchContent_Declare(deko_spirv_cross
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
  GIT_TAG ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b)
FetchContent_GetProperties(deko_spirv_cross)
if(NOT deko_spirv_cross_POPULATED)
  FetchContent_Populate(deko_spirv_cross)
endif()
set(DEKO_SPIRV_CROSS_ROOT "${deko_spirv_cross_SOURCE_DIR}")
cemu_deko_apply_patch("${DEKO_SPIRV_CROSS_ROOT}"
  "${CMAKE_CURRENT_LIST_DIR}/patches/spirv-cross-shadow1d.patch")
add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/lsfg-deko3d" "${CMAKE_BINARY_DIR}/lsfg-deko3d")
