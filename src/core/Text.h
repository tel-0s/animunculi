#pragma once
// Small string/encoding helpers. Pure C++, unit-tested.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Core
{
	std::string                              Base64Encode(std::span<const std::uint8_t> a_data);
	std::optional<std::vector<std::uint8_t>> Base64Decode(std::string_view a_text);

	std::string Lower(std::string_view a_text);
	bool        IEquals(std::string_view a, std::string_view b);
	bool        IContains(std::string_view a_haystack, std::string_view a_needle);

	// "0x0001A696" / "0001A696" / "1A696" -> FormID. Requires the 0x prefix unless a_allowBare.
	std::optional<std::uint32_t> ParseFormID(std::string_view a_text, bool a_allowBare = false);
	std::string                  FormatFormID(std::uint32_t a_id);

	// Constant-time comparison for tokens.
	bool SecureEquals(std::string_view a, std::string_view b);

	// Strips "<Alias=Foo>"-style tokens and collapses whitespace (quest objective text).
	std::string CleanDisplayText(std::string_view a_text);

	// Skyrim book markup (<p>, <br>, <font>, <img>, [pagebreak], &entities;) to plain text.
	// Paragraphs are separated by blank lines; page breaks become a "---" line.
	std::string BookToText(std::string_view a_markup);

	// Absolute compass direction for a heading in degrees clockwise from north: "N", "NE", ...
	std::string CompassWord(float a_degrees);

	// Clock-face-free human bearing: "ahead", "ahead-right", "right", "behind-right", ...
	std::string BearingWord(float a_relativeDegrees);

	// Wrap to [-180, 180).
	float WrapDegrees(float a_deg);
}
