# Portable CMake generation, no runtime path or external build dependency.
if(EXISTS "${INPUT}")
  file(READ "${INPUT}" ca_hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ca_bytes "${ca_hex}")
  file(WRITE "${OUTPUT}" "static const unsigned char vt_ca_bundle[]={${ca_bytes}0};\n")
else()
  file(WRITE "${OUTPUT}" "static const unsigned char vt_ca_bundle[]={0};\n")
endif()
