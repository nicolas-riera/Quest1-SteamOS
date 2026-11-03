// Minimal JSON reader, enough for Meta's camera_calibration_v2.json: shared by driver_quest1 (camera.cpp) and
// src/camtap/ptprovider.cpp.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace q1json {

struct Json
{
	enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
	double num = 0;
	std::string str;
	std::vector<Json> arr;
	std::map<std::string, Json> obj;

	const Json *Get(const char *key) const
	{
		if (type != Obj)
			return nullptr;
		auto it = obj.find(key);
		return it == obj.end() ? nullptr : &it->second;
	}
	bool Numbers(double *out, size_t n) const
	{
		if (type != Arr || arr.size() < n)
			return false;
		for (size_t i = 0; i < n; i++) {
			if (arr[i].type != Num)
				return false;
			out[i] = arr[i].num;
		}
		return true;
	}
};

struct JsonParser
{
	const char *p, *end;
	int depth = 0;

	void Ws()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}
	bool String(std::string &out)
	{
		if (p >= end || *p != '"')
			return false;
		p++;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end) {
				p++;
				char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; // \uXXXX kept as 'u' + digits
				out += c;
			} else {
				out += *p;
			}
			p++;
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}
	bool Value(Json &v)
	{
		if (++depth > 32)
			return false;
		Ws();
		if (p >= end)
			return false;
		bool ok = true;
		if (*p == '{') {
			v.type = Json::Obj;
			p++;
			Ws();
			if (p < end && *p == '}') {
				p++;
			} else {
				for (;;) {
					Ws();
					std::string key;
					if (!String(key))
						return false;
					Ws();
					if (p >= end || *p != ':')
						return false;
					p++;
					if (!Value(v.obj[key]))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == '}') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '[') {
			v.type = Json::Arr;
			p++;
			Ws();
			if (p < end && *p == ']') {
				p++;
			} else {
				for (;;) {
					v.arr.emplace_back();
					if (!Value(v.arr.back()))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == ']') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '"') {
			v.type = Json::Str;
			ok = String(v.str);
		} else if (!strncmp(p, "true", std::min<size_t>(4, end - p)) && end - p >= 4) {
			v.type = Json::Bool, v.num = 1, p += 4;
		} else if (!strncmp(p, "false", std::min<size_t>(5, end - p)) && end - p >= 5) {
			v.type = Json::Bool, p += 5;
		} else if (!strncmp(p, "null", std::min<size_t>(4, end - p)) && end - p >= 4) {
			p += 4;
		} else {
			std::string tmp(p, std::min<size_t>(64, end - p));
			char *e = nullptr;
			v.type = Json::Num;
			v.num = strtod(tmp.c_str(), &e);
			if (e == tmp.c_str())
				return false;
			p += e - tmp.c_str();
		}
		depth--;
		return ok;
	}
};

inline bool ParseJsonFile(const char *path, Json &out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (1u << 20))
		s.append(buf, n);
	fclose(f);
	JsonParser jp{s.data(), s.data() + s.size()};
	return jp.Value(out);
}

} // namespace q1json
