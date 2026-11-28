#ifndef ECUIO_JSON_BUILD_H
#define ECUIO_JSON_BUILD_H

#include "json.h"

#include <cstdint>
#include <string>
#include <utility>

namespace ecuio_json {

inline Value text_value(const std::string& s)
{
    Value v;
    v.type = Type::string;
    v.text = s;
    return v;
}

inline Value whole_value(int64_t n)
{
    Value v;
    v.type = Type::number;
    v.integer = true;
    v.in_int64 = true;
    v.whole = n;
    v.number = static_cast<double>(n);
    return v;
}

inline Value empty_of(Type t)
{
    Value v;
    v.type = t;
    return v;
}

inline void add(Value& object, const std::string& key, Value value)
{
    Member m;
    m.key = key;
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

}  // namespace ecuio_json

#endif
