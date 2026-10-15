if(TARGET ${APP_NAME_LC}::Bluray)
  set(SOURCES TestBlurayStreamCache.cpp)

  core_add_test_library(videoplayer_inputstreams_test)
endif()
