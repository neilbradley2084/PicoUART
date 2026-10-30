# Turns Web/index.html into a C array for the web server (run by CMakeLists.txt)
#   cmake -DINPUT=<html> -DOUTPUT=<c file> -P Embed.cmake

file(READ "${INPUT}" HEX HEX)
string(LENGTH "${HEX}" LEN)
math(EXPR SIZE "${LEN} / 2")
string(REGEX REPLACE "(................................)" "\\1\n" HEX "${HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BODY "${HEX}")
string(REPLACE "\n" "\n\t" BODY "${BODY}")
file(WRITE "${OUTPUT}" "// Generated from Source/Web/index.html - edit that file, not this one\n\n#include \"HttpPage.h\"\n\nconst uint8_t g_u8HttpPage[] =\n{\n\t${BODY}\n};\n\nconst uint32_t g_u32HttpPageSize = ${SIZE};\n")
