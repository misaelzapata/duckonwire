#pragma once

#include <string>

namespace duckonwire {
inline std::string JsonString(const std::string &value) {
	std::string result = "\"";
	const char hex[] = "0123456789abcdef";
	for (unsigned char byte : value) {
		switch (byte) {
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (byte < 0x20) {
				result += "\\u00";
				result += hex[byte >> 4];
				result += hex[byte & 15];
			} else result += static_cast<char>(byte);
		}
	}
	return result + "\"";
}
} // namespace duckonwire
