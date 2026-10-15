set(SOURCES BlurayStateSerializer.cpp
            DVDFactoryInputStream.cpp
            DVDInputStream.cpp
            DVDInputStreamFFmpeg.cpp
            DVDInputStreamFile.cpp
            DVDInputStreamMemory.cpp
            DVDInputStreamNavigator.cpp
            DVDStateSerializer.cpp
            InputStreamAddon.cpp
            InputStreamMultiSource.cpp
            InputStreamPVRBase.cpp
            InputStreamPVRChannel.cpp
            InputStreamPVRRecording.cpp)

set(HEADERS BlurayStateSerializer.h
            DVDFactoryInputStream.h
            DVDInputStream.h
            DVDInputStreamFFmpeg.h
            DVDInputStreamFile.h
            DVDInputStreamMemory.h
            DVDInputStreamNavigator.h
            DVDStateSerializer.h
            DllDvdNav.h
            InputStreamAddon.h
            InputStreamMultiStreams.h
            InputStreamMultiSource.h
            InputStreamPVRBase.h
            InputStreamPVRChannel.h
            InputStreamPVRRecording.h)

if(TARGET ${APP_NAME_LC}::Bluray)
  list(APPEND SOURCES BlurayIsoCache.cpp
                      BlurayStreamCache.cpp
                      DVDInputStreamBluray.cpp
                      StreamCacheSetup.cpp)
  list(APPEND HEADERS BlurayIsoCache.h
                      BlurayStreamCache.h
                      DVDInputStreamBluray.h
                      StreamCacheSetup.h)
endif()

core_add_library(dvdinputstreams)
