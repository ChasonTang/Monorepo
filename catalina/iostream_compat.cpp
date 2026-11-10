// Provide the char stream instantiations added to newer system libc++ builds.
// Use system libc++ headers, the macOS 10.15 deployment target and ABI __1.
// RTTI and exceptions must stay enabled to match clients of these classes.
#include <sstream>

template class __attribute__((
    visibility("default"))) std::basic_stringbuf<char>;
template class __attribute__((
    visibility("default"))) std::basic_stringstream<char>;
template class __attribute__((
    visibility("default"))) std::basic_ostringstream<char>;
template class __attribute__((
    visibility("default"))) std::basic_istringstream<char>;
