cmake_policy(SET CMP0135 NEW)

include(FetchContent)

FetchContent_Declare(argon
        GIT_REPOSITORY https://github.com/stellar-aria/argon
        GIT_TAG 3779c5315a978a2f8c892e320ac12ecbb77c3759
)
FetchContent_MakeAvailable(argon)

FetchContent_Declare(
        etl
        GIT_REPOSITORY https://github.com/ETLCPP/etl
        GIT_TAG 20.40.0
)

FetchContent_MakeAvailable(etl)

FetchContent_Declare(freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG V11.1.0
)
set(FREERTOS_PORT GCC_ARM_CA9 CACHE STRING "" FORCE)
set(FREERTOS_CONFIG_FILE_DIRECTORY
        "${CMAKE_SOURCE_DIR}/src/OSLikeStuff/freertos" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(freertos_kernel)
