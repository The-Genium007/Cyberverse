add_library(${CMAKE_PROJECT_NAME} SHARED "")

set(PROJECT_HEADER_FILES
  Main.h
  NetworkGameSystem.h
  Utils.h
  PlayerActionTracker.h
        PlayerSync/TamponInterpolation.h
        CommandLine.h
)

set(PROJECT_SRC_FILES
  Main.cpp
  NetworkGameSystem.cpp
  TesseraEsthetiqueV.cpp
  TesseraSpawnEnrichi.cpp
  PlayerActionTracker.cpp
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${PROJECT_HEADER_FILES} ${PROJECT_SRC_FILES})

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PROJECT_HEADER_FILES} ${PROJECT_SRC_FILES})

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE RED4ext::SDK)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE zpp_bits)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE GameNetworkingSockets::shared)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE RedLib)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE protocol)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE flatbuffers_hdr)

# Symboles de debogage (PDB) meme en Release, sans rien changer a l'optimisation (2026-09-26).
# Sans eux, un plantage de cette DLL reste un decalage illisible pour toujours : 33 plantages
# releves sur le PC de Lucas en un mois, venant de 26 builds, et aucun PDB nulle part
# (Tessera, docs/superpowers/specs/2026-09-26-plantages-du-jeu-design.md). /DEBUG change les
# defauts du linker (NOREF, NOICF) : on remet REF et ICF pour garder le binaire de Release.
if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()
