/* deqpC3Kernel.cpp - for the Core 3 module only (build_module.sh): delibs'
 * case conversions without C++ locales (deStringUtil.cpp's use
 * std::locale::classic(), libstdc++'s locale code, which wants glibc and is
 * not in the module). ASCII, as the "C" locale they ask for.
 */
#include "deStringUtil.hpp"

#include <string>

namespace de
{

char toLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

char toUpper(char c)
{
    return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
}

bool isUpper(char c)
{
    return c >= 'A' && c <= 'Z';
}

bool isLower(char c)
{
    return c >= 'a' && c <= 'z';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string toLower(const std::string &str)
{
    std::string r(str);
    for (size_t i = 0; i < r.size(); i++)
        r[i] = toLower(r[i]);
    return r;
}

std::string toUpper(const std::string &str)
{
    std::string r(str);
    for (size_t i = 0; i < r.size(); i++)
        r[i] = toUpper(r[i]);
    return r;
}

std::string capitalize(const std::string &str)
{
    std::string r(str);
    if (!r.empty())
        r[0] = toUpper(r[0]);
    return r;
}

} // namespace de
