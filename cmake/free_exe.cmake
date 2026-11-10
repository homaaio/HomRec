# cmake -DEXE=<path to .exe> -P free_exe.cmake
# Moves a locked (running) exe out of the way so the linker can write a new one.
# The old copy is called <name>_old.exe; it is deleted next time, or stays if it is still running.
if(NOT EXISTS "${EXE}")
    return()
endif()
get_filename_component(dir "${EXE}" DIRECTORY)
get_filename_component(name "${EXE}" NAME_WE)
set(old "${dir}/${name}_old.exe")
file(REMOVE "${old}")
execute_process(COMMAND ${CMAKE_COMMAND} -E rename "${EXE}" "${old}" RESULT_VARIABLE res ERROR_QUIET)
if(NOT res EQUAL 0)
    message(WARNING "free_exe: can't move ${EXE} aside - close ${name}.exe (also in the tray) and build again")
endif()
