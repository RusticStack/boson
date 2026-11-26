include_guard(GLOBAL)

# Profile-guided optimisation for the executables (MSVC). Three-step flow, scripted in bench/pgo.ps1:
#   1. -DBOSON_PGO=GEN  build: executables link with /GENPROFILE (instrumented, writes <target>!N.pgc files)
#   2. run the training scenarios (boson_bench + boson_bench_app / boson_browser --bench), pgomgr /merge
#   3. -DBOSON_PGO=USE  relink: /USEPROFILE with the merged <target>.pgd copied to ${BOSON_PGO_DIR}
# The profile data is only valid for the same object files, so USE re-links the GEN build tree.
set(BOSON_PGO "OFF" CACHE STRING "Profile-guided optimisation: OFF | GEN | USE (Release + MSVC only)")
set_property(CACHE BOSON_PGO PROPERTY STRINGS OFF GEN USE)
set(BOSON_PGO_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Directory holding the merged <target>.pgd profiles")

# boson_enable_pgo(<target>...): applies the current BOSON_PGO mode to executables that have a training scenario.
function(boson_enable_pgo)
    if(NOT MSVC OR BOSON_PGO STREQUAL "OFF")
        return()
    endif()
    foreach(target IN LISTS ARGN)
        if(BOSON_PGO STREQUAL "GEN")
            target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:/LTCG;/GENPROFILE:PGD=${BOSON_PGO_DIR}/${target}.pgd>")
        elseif(BOSON_PGO STREQUAL "USE")
            target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:/LTCG;/USEPROFILE:PGD=${BOSON_PGO_DIR}/${target}.pgd>")
        else()
            message(FATAL_ERROR "BOSON_PGO must be OFF, GEN or USE")
        endif()
    endforeach()
endfunction()
