find_package(UHD)
find_package(Boost REQUIRED system thread regex)
find_package(Threads REQUIRED)
find_package(Volk REQUIRED)
find_package(MAVSDK REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(TBB REQUIRED)

set(JSON_Install OFF CACHE INTERNAL "")
set(USED_LIBS ${Boost_SYSTEM_LIBRARY} ${Boost_THREAD_LIBRARY} ${Boost_REGEX_LIBRARY})

set(privatelibs
    uhd
    Threads::Threads
    SQLite::SQLite3
    pthread
    dl
    Volk::volk
    MAVSDK::mavsdk
    TBB::tbb
    numa
    ${USED_LIBS}
)
