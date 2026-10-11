#include "Text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Core
{
	namespace
	{
		constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		constexpr std::array<std::int8_t, 256> MakeDecodeTable()
		{
			std::array<std::int8_t, 256> t{};
			for (auto& v : t) {
				v = -1;
			}
			for (int i = 0; i < 64; ++i) {
				t[static_cast<unsigned char>(kB64[i])] = static_cast<std::int8_t>(i);
			}
			t[static_cast<unsigned char>('-')] = 62;  // accept base64url too
			t[static_cast<unsigned char>('_')] = 63;
			return t;
		}
		constexpr auto kDecode = MakeDecodeTable();
	}

	std::string Base64Encode(std::span<const std::uint8_t> a_data)
	{
		std::string out;
		out.reserve((a_data.size() + 2) / 3 * 4);
		std::size_t i = 0;
		for (; i + 2 < a_data.size(); i += 3) {
			const std::uint32_t v = (a_data[i] << 16) | (a_data[i + 1] << 8) | a_data[i + 2];
			out += kB64[(v >> 18) & 63];
			out += kB64[(v >> 12) & 63];
			out += kB64[(v >> 6) & 63];
			out += kB64[v & 63];
		}
		if (const auto rest = a_data.size() - i; rest == 1) {
			const std::uint32_t v = a_data[i] << 16;
			out += kB64[(v >> 18) & 63];
			out += kB64[(v >> 12) & 63];
			out += "==";
		} else if (rest == 2) {
			const std::uint32_t v = (a_data[i] << 16) | (a_data[i + 1] << 8);
			out += kB64[(v >> 18) & 63];
			out += kB64[(v >> 12) & 63];
			out += kB64[(v >> 6) & 63];
			out += '=';
		}
		return out;
	}

	std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view a_text)
	{
		std::vector<std::uint8_t> out;
		out.reserve(a_text.size() * 3 / 4);
		std::uint32_t acc = 0;
		int           bits = 0;
		for (const char c : a_text) {
			if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
				continue;
			}
			const auto v = kDecode[static_cast<unsigned char>(c)];
			if (v < 0) {
				return std::nullopt;
			}
			acc = (acc << 6) | static_cast<std::uint32_t>(v);
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
			}
		}
		return out;
	}

	std::string Lower(std::string_view a_text)
	{
		std::string out{ a_text };
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	bool IEquals(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
			return std::tolower(x) == std::tolower(y);
		});
	}

	bool IContains(std::string_view a_haystack, std::string_view a_needle)
	{
		if (a_needle.empty()) {
			return true;
		}
		const auto it = std::search(a_haystack.begin(), a_haystack.end(), a_needle.begin(), a_needle.end(),
			[](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
		return it != a_haystack.end();
	}

	std::optional<std::uint32_t> ParseFormID(std::string_view a_text, bool a_allowBare)
	{
		while (!a_text.empty() && a_text.front() == ' ') {
			a_text.remove_prefix(1);
		}
		while (!a_text.empty() && a_text.back() == ' ') {
			a_text.remove_suffix(1);
		}
		if (a_text.size() > 2 && a_text[0] == '0' && (a_text[1] == 'x' || a_text[1] == 'X')) {
			a_text.remove_prefix(2);
		} else if (!a_allowBare) {
			return std::nullopt;
		}
		if (a_text.empty() || a_text.size() > 8) {
			return std::nullopt;
		}
		std::uint32_t v = 0;
		for (const char c : a_text) {
			v <<= 4;
			if (c >= '0' && c <= '9') {
				v |= static_cast<std::uint32_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v |= static_cast<std::uint32_t>(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v |= static_cast<std::uint32_t>(c - 'A' + 10);
			} else {
				return std::nullopt;
			}
		}
		return v;
	}

	std::string FormatFormID(std::uint32_t a_id)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "0x%08X", a_id);
		return buf;
	}

	bool SecureEquals(std::string_view a, std::string_view b)
	{
		unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
		const std::size_t n = std::max(a.size(), b.size());
		for (std::size_t i = 0; i < n; ++i) {
			const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
			const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
			diff |= static_cast<unsigned char>(x ^ y);
		}
		return diff == 0;
	}

	std::string CleanDisplayText(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			const char c = a_text[i];
			if (c == '<') {
				// <Alias=Foo> or <Global=Bar>: keep the name part so "Bring <Alias=Item> to <Alias=Npc>"
				// becomes "Bring Item to Npc".
				const auto close = a_text.find('>', i);
				if (close != std::string_view::npos) {
					const auto inner = a_text.substr(i + 1, close - i - 1);
					const auto eq = inner.find('=');
					if (eq != std::string_view::npos) {
						out.append(inner.substr(eq + 1));
					}
					i = close;
					continue;
				}
			}
			const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
			if (space) {
				if (!out.empty() && out.back() != ' ') {
					out += ' ';
				}
			} else {
				out += c;
			}
		}
		while (!out.empty() && out.back() == ' ') {
			out.pop_back();
		}
		return out;
	}

	float WrapDegrees(float a_deg)
	{
		float d = std::fmod(a_deg + 180.f, 360.f);
		if (d < 0) {
			d += 360.f;
		}
		return d - 180.f;
	}

	std::string BearingWord(float a_relativeDegrees)
	{
		const float d = WrapDegrees(a_relativeDegrees);
		const float a = std::abs(d);
		const char* side = d < 0 ? "left" : "right";
		if (a <= 22.5f) {
			return "ahead";
		}
		if (a <= 67.5f) {
			return std::string("ahead-") + side;
		}
		if (a <= 112.5f) {
			return side;
		}
		if (a <= 157.5f) {
			return std::string("behind-") + side;
		}
		return "behind";
	}

	std::string CompassWord(float a_degrees)
	{
		static constexpr const char* kNames[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
		float d = std::fmod(a_degrees, 360.f);
		if (d < 0) {
			d += 360.f;
		}
		return kNames[static_cast<int>((d + 22.5f) / 45.f) % 8];
	}

	std::string BookToText(std::string_view a_in)
	{
		std::string out;
		out.reserve(a_in.size());
		// Ensure the output ends with at least a_count newlines (never at the very start).
		auto newlines = [&](int a_count) {
			while (!out.empty() && out.back() == ' ') {
				out.pop_back();
			}
			if (out.empty()) {
				return;
			}
			int have = 0;
			for (auto it = out.rbegin(); it != out.rend() && *it == '\n'; ++it) {
				++have;
			}
			for (; have < a_count; ++have) {
				out += '\n';
			}
		};
		static constexpr std::pair<std::string_view, char> kEntities[] = {
			{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }
		};
		for (std::size_t i = 0; i < a_in.size(); ++i) {
			const char c = a_in[i];
			if (c == '[' && IEquals(a_in.substr(i, 11), "[pagebreak]")) {
				newlines(2);
				if (!out.empty()) {
					out += "---\n\n";
				}
				i += 10;
				continue;
			}
			if (c == '<') {
				const auto close = a_in.find('>', i);
				if (close == std::string_view::npos) {
					break;
				}
				const auto inner = a_in.substr(i + 1, close - i - 1);
				const auto tag = Lower(inner);
				const auto name = tag.substr(0, tag.find_first_of(" \t="));  // "p", "/p", "br", "br/", "font", ...
				if (tag.starts_with("alias=") || tag.starts_with("global=") || tag.starts_with("basename=")) {
					out.append(inner.substr(inner.find('=') + 1));  // <Alias=Player> -> Player
				} else if (name == "br" || name == "br/") {
					newlines(1);
				} else if (name == "p" || name == "/p" || name == "div" || name == "/div" || name == "li") {
					newlines(2);
				}
				i = close;
				continue;
			}
			if (c == '&') {
				bool matched = false;
				for (auto [ent, ch] : kEntities) {
					if (a_in.substr(i, ent.size()) == ent) {
						if (!(ch == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n'))) {
							out += ch;
						}
						i += ent.size() - 1;
						matched = true;
						break;
					}
				}
				if (matched) {
					continue;
				}
			}
			if (c == '\r') {
				continue;
			}
			if (c == '\n') {
				newlines(1);  // explicit line breaks inside paragraphs stay single newlines
				continue;
			}
			if ((c == ' ' || c == '\t') && (out.empty() || out.back() == ' ' || out.back() == '\n')) {
				continue;
			}
			out += c == '\t' ? ' ' : c;
		}
		while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
			out.pop_back();
		}
		std::string compact;  // at most one blank line in a row
		int         run = 0;
		for (const char c : out) {
			run = c == '\n' ? run + 1 : 0;
			if (run <= 2) {
				compact += c;
			}
		}
		return compact;
	}
}
