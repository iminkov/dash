project(project_dash_dart_if)

# Get DART interface version number:
set(DASH_DART_IF_INCLUDE_DIR ${DASH_DART_IF_INCLUDE_DIR} PARENT_SCOPE)

## Installation

# Headers
install(
  DIRECTORY ${DASH_DART_IF_INCLUDE_DIR}/dash DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)

# Helper to let Qt Creator capture headers
FILE(GLOB_RECURSE DartIfFiles "${DASH_DART_IF_INCLUDE_DIR}/dash/*.h")
add_custom_target(dart-if SOURCES ${DartIfFiles})
