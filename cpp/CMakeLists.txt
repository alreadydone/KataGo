cmake_minimum_required(VERSION 3.12.3)
project(writedata)

set(CMAKE_CXX_STANDARD 14)
set(BUILD_MCTS 1)
set(USE_CUDA_BACKEND 1)
#set(BOOST_ROOT "C:/Users/XJY/.nuget/packages/boost/1.68.0")
set(BOOST_INDLUDEDIR "C:/Users/XJY/.nuget/packages/boost/1.68.0/lib/native/include/boost")

include_directories(external)
include_directories(external/tclap-1.2.1/include)

#------------------------------------------------------------------------------------

if(BUILD_WRITE)
  message("-DBUILD_WRITE=1 is set, building 'write' executable for writing neural net training data from sgfs or LZ data files")
endif()
if(BUILD_MCTS)
  message("-DBUILD_MCTS=1 is set, building 'main' executable for mcts-backed GTP engine and other tools")

  if(USE_CUDA_BACKEND)
    enable_language(CUDA)
    set(CUDA_STANDARD 11)
    set(NEURALNET_BACKEND_SOURCES neuralnet/cudabackend.cpp neuralnet/cudahelpers.cu)
    set(CMAKE_CUDA_FLAGS
      "-gencode arch=compute_30,code=sm_30 -gencode arch=compute_30,code=compute_30 -gencode arch=compute_37,code=sm_37 -gencode arch=compute_53,code=sm_53 -gencode arch=compute_53,code=compute_53 -gencode arch=compute_70,code=sm_70 -gencode arch=compute_70,code=compute_70"
      )

  else()
    message(FATAL_ERROR "Please specify -DUSE_CUDA_BACKEND=1 (right now CUDA the only option, but in the future there may be others)")
  endif()
endif()

if((NOT BUILD_WRITE) AND (NOT BUILD_MCTS))
  message(FATAL_ERROR "Please specify one or more of -DBUILD_WRITE=1 or -DBUILD_MCTS=1")
endif()

if(USE_TCMALLOC)
  message("-DUSE_TCMALLOC=1 is set, using tcmalloc as the allocator")
endif()

# set (Gperftools_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake/")
# find_package(Gperftools REQUIRED)

#------------------------------------------------------------------------------------



#------------------------------------------------------------------------------------

if(USE_TCMALLOC)
  find_library(TCMALLOC_LIB NAMES tcmalloc_minimal HINTS /usr)
  if(NOT TCMALLOC_LIB)
    message(FATAL_ERROR "Could not find tcmalloc")
  endif()
endif()

#------------------------------------------------------------------------------------

if(BUILD_WRITE)
  add_executable(write
    core/global.cpp
    core/hash.cpp
    core/md5.cpp
    core/rand.cpp
    core/sha2.cpp
    core/timer.cpp
    game/board.cpp
    game/rules.cpp
    game/boardhistory.cpp
    neuralnet/nninputs.cpp
    dataio/datapool.cpp
    dataio/lzparse.cpp
    dataio/sgf.cpp
	program/gitinfotemplate.h
    write.cpp
    )

  find_package(HDF5 REQUIRED COMPONENTS C CXX)

  if(HDF5_FOUND)
    include_directories(${HDF5_INCLUDE_DIRS})
    target_link_libraries (write ${HDF5_LIBRARIES})
  endif (HDF5_FOUND)

  # find_package(ZLIB REQUIRED)
  # if(ZLIB_FOUND)
  #   include_directories(${ZLIB_INCLUDE_DIRS})
  #   target_link_libraries(write ${ZLIB_LIBRARIES})
  # endif(ZLIB_FOUND)

endif()

#------------------------------------------------------------------------------------

if(BUILD_MCTS)
  add_executable(main
    core/global.cpp
    core/config_parser.cpp
    core/elo.cpp
    core/fancymath.cpp
    core/hash.cpp
    core/logger.cpp
    core/makedir.cpp
    core/md5.cpp
    core/rand.cpp
    core/sha2.cpp
    core/timer.cpp
    game/board.cpp
    game/rules.cpp
    game/boardhistory.cpp
    dataio/sgf.cpp
    dataio/numpywrite.cpp
    dataio/trainingwrite.cpp
    dataio/loadmodel.cpp
    dataio/lzparse.cpp
    neuralnet/nninputs.cpp
    neuralnet/modelversion.cpp
    neuralnet/nneval.cpp
    ${NEURALNET_BACKEND_SOURCES}
    search/timecontrols.cpp
    search/searchparams.cpp
    search/mutexpool.cpp
    search/search.cpp
    search/asyncbot.cpp
    search/distributiontable.cpp
    program/setup.cpp
    program/play.cpp
	program/gitinfotemplate.h
    tests/testboardarea.cpp
    tests/testboardbasic.cpp
    tests/testrules.cpp
    tests/testscore.cpp
    tests/testnninputs.cpp
    tests/testsearch.cpp
    tests/testtime.cpp
    tests/testtrainingwrite.cpp
    evalsgf.cpp
    gatekeeper.cpp
    gtp.cpp
    match.cpp
    matchauto.cpp
    selfplay.cpp
    misc.cpp
    runtests.cpp
    lzcost.cpp
    sandbox.cpp
    main.cpp
    )

  if(USE_CUDA_BACKEND)
    add_definitions(-DUSE_CUDA_BACKEND)
    find_package(CUDA REQUIRED)
    find_library(CUDNN_LIBRARY cudnn.lib E:/CUDA/lib/x64)
    include_directories(SYSTEM ${CUDA_INCLUDE_DIRS}) #SYSTEM is for suppressing some compiler warnings in thrust libraries
    target_link_libraries(main ${CUDNN_LIBRARY} ${CUDA_CUBLAS_LIBRARIES} ${CUDA_LIBRARIES})
  endif()

  include_directories("C:/Users/XJY/.nuget/packages/boost/1.68.0/lib/native/include")
  #find_package(ZLIB REQUIRED)
  #if(ZLIB_FOUND)
    include_directories("C:/Users/XJY/.nuget/packages/zlib/1.2.8.1/build/native/include")
    target_link_libraries(main C:/Users/XJY/.nuget/packages/zlib/1.2.8.1/build/native/lib/v110/x64/Release/static/cdecl/zlib.lib)
  #endif(ZLIB_FOUND)

  #find_library(LIBZIP_LIBRARY NAMES zip)
  include_directories("C:/Users/XJY/.nuget/packages/libzip/1.1.2.7/build/native/include")
  target_link_libraries(main "C:/Users/XJY/.nuget/packages/libzip/1.1.2.7/build/native/lib/x64/v140/Release/zip.lib")

  #find_package(Boost COMPONENTS system filesystem REQUIRED)
  target_link_libraries(main "C:/Users/XJY/.nuget/packages/boost_filesystem-vc141/1.68.0/lib/native/libboost_filesystem-vc141-mt-gd-x64-1_68.lib" "C:/Users/XJY/.nuget/packages/boost_system-vc141/1.68.0/lib/native/libboost_system-vc141-mt-gd-x64-1_68.lib")

  if(USE_TCMALLOC)
    target_link_libraries(main ${TCMALLOC_LIB})
  endif(USE_TCMALLOC)

endif()


#------------------------------------------------------------------------------------


if(CMAKE_COMPILER_IS_GNUCC)
  if(USE_TCMALLOC)
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -mfpmath=sse -g -O2 -Wall -Wextra -Wno-sign-compare -Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wdisabled-optimization -Wformat=2 -Winit-self -Wlogical-op -Wmissing-declarations -Wmissing-include-dirs -Wnoexcept -Woverloaded-virtual -Wredundant-decls -Wshadow -Wstrict-null-sentinel -Wstrict-overflow=1 -Wswitch-default -Wsizeof-pointer-memaccess -Wuninitialized -Winit-self -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
  else()
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -mfpmath=sse -g -O2 -Wall -Wextra -Wno-sign-compare -Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wdisabled-optimization -Wformat=2 -Winit-self -Wlogical-op -Wmissing-declarations -Wmissing-include-dirs -Wnoexcept -Woverloaded-virtual -Wredundant-decls -Wshadow -Wstrict-null-sentinel -Wstrict-overflow=1 -Wswitch-default -Wsizeof-pointer-memaccess -Wuninitialized -Winit-self")
  endif()
endif()
