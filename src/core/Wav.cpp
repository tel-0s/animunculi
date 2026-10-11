#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Core
{
	namespace
	{
		std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
		std::uint32_t U32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
	}

	bool DecodeWav(std::span<const std::uint8_t> a_data, PcmClip& a_out, std::string& a_error)
	{
		const auto* d = a_data.data();
		const auto  n = a_data.size();
		if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
			a_error = "not a RIFF/WAVE file";
			return false;
		}
		std::uint16_t tag = 0, channels = 0, bits = 0;
		std::uint32_t rate = 0;
		const std::uint8_t* pcm = nullptr;
		std::size_t         pcmLen = 0;
		bool                haveFmt = false;

		std::size_t pos = 12;
		while (pos + 8 <= n) {
			const auto* id = d + pos;
			std::size_t size = U32(d + pos + 4);
			const std::size_t body = pos + 8;
			if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16 && body + 16 <= n) {
				tag = U16(d + body);
				channels = U16(d + body + 2);
				rate = U32(d + body + 4);
				bits = U16(d + body + 14);
				if (tag == 0xFFFE && size >= 40 && body + 26 <= n) {
					tag = U16(d + body + 24);  // SubFormat GUID begins with the real format tag
				}
				haveFmt = true;
			} else if (std::memcmp(id, "data", 4) == 0) {
				if (size == 0 || size == 0xFFFFFFFF || body + size > n) {
					size = n - body;  // streamed/truncated header
				}
				pcm = d + body;
				pcmLen = size;
				break;
			}
			if (size > n) {
				break;
			}
			pos = body + size + (size & 1);
		}
		if (!haveFmt || !pcm) {
			a_error = "WAV is missing its fmt or data chunk";
			return false;
		}
		const bool intPcm = tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
		const bool floatPcm = tag == 3 && bits == 32;
		if (!intPcm && !floatPcm) {
			a_error = "unsupported WAV encoding (format " + std::to_string(tag) + ", " + std::to_string(bits) + "-bit); use PCM or float";
			return false;
		}
		if (channels < 1 || channels > 2 || rate < 4000 || rate > 192000) {
			a_error = "unsupported WAV layout (" + std::to_string(channels) + " ch @ " + std::to_string(rate) + " Hz)";
			return false;
		}
		const std::size_t bytesPerSample = bits / 8u;
		const std::size_t count = pcmLen / bytesPerSample / channels * channels;
		a_out.sampleRate = rate;
		a_out.channels = channels;
		a_out.samples.resize(count);
		for (std::size_t i = 0; i < count; ++i) {
			const auto* s = pcm + i * bytesPerSample;
			float       v = 0.f;
			if (floatPcm) {
				std::memcpy(&v, s, 4);
				if (!std::isfinite(v)) {
					v = 0.f;
				}
			} else {
				switch (bits) {
				case 8:
					v = (static_cast<int>(s[0]) - 128) / 128.f;
					break;
				case 16:
					v = static_cast<std::int16_t>(U16(s)) / 32768.f;
					break;
				case 24:
					{
						std::int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
						if (x & 0x800000) {
							x |= ~0xFFFFFF;
						}
						v = x / 8388608.f;
						break;
					}
				case 32:
					v = static_cast<float>(static_cast<std::int32_t>(U32(s)) / 2147483648.0);
					break;
				}
			}
			a_out.samples[i] = std::clamp(v, -1.f, 1.f);
		}
		if (a_out.frames() == 0) {
			a_error = "WAV contains no audio";
			return false;
		}
		return true;
	}

	std::vector<std::int16_t> ToInt16(const PcmClip& a_clip)
	{
		std::vector<std::int16_t> out(a_clip.samples.size());
		for (std::size_t i = 0; i < out.size(); ++i) {
			out[i] = static_cast<std::int16_t>(std::lround(std::clamp(a_clip.samples[i], -1.f, 1.f) * 32767.f));
		}
		return out;
	}

	std::vector<Viseme> AnalyseVisemes(const PcmClip& a_clip)
	{
		std::vector<Viseme> out;
		const std::size_t   frames = a_clip.frames();
		if (!frames || !a_clip.sampleRate) {
			return out;
		}
		const std::size_t hop = std::max<std::size_t>(1, a_clip.sampleRate / kVisemeRate);
		const std::size_t win = hop * 2;
		const std::size_t steps = (frames + hop - 1) / hop;

		auto mono = [&](std::size_t f) {
			if (f >= frames) {
				return 0.f;
			}
			if (a_clip.channels == 1) {
				return a_clip.samples[f];
			}
			return 0.5f * (a_clip.samples[f * 2] + a_clip.samples[f * 2 + 1]);
		};

		std::vector<float> rms(steps), ratio(steps);
		for (std::size_t i = 0; i < steps; ++i) {
			const std::size_t start = i * hop;
			double            e = 0, de = 0;
			float             prev = mono(start);
			for (std::size_t k = 0; k < win; ++k) {
				const float x = mono(start + k);
				e += x * x;
				de += (x - prev) * (x - prev);
				prev = x;
			}
			rms[i] = static_cast<float>(std::sqrt(e / win));
			// Energy of the first difference relative to the signal's: grows with frequency, a cheap
			// "brightness" proxy separating ee/eh (spread) from oh/oo (rounded) and fricatives.
			ratio[i] = e > 1e-9 ? static_cast<float>(de / e) : 0.f;
		}

		// Normalise loudness against the clip's own loud frames (95th percentile).
		std::vector<float> sorted = rms;
		std::sort(sorted.begin(), sorted.end());
		const float ref = std::max(sorted[static_cast<std::size_t>(0.95 * (sorted.size() - 1))], 1e-4f);

		out.resize(steps);
		float open = 0.f, bright = 0.5f;
		for (std::size_t i = 0; i < steps; ++i) {
			const float db = 20.f * std::log10(std::max(rms[i], 1e-6f) / ref);  // 0 dB = loud speech
			float       target = std::clamp((db + 32.f) / 32.f, 0.f, 1.f);        // -32 dB..0 dB -> 0..1
			target = target * target * (3.f - 2.f * target);                     // smoothstep
			open += (target - open) * (target > open ? 0.6f : 0.3f);             // fast attack, slower release

			const float b = std::clamp((std::log10(std::max(ratio[i], 1e-4f)) + 1.7f) / 1.7f, 0.f, 1.f);
			if (target > 0.05f) {
				bright += (b - bright) * 0.35f;
			}
			out[i].open = open;
			out[i].bright = bright;
		}
		// Lips press together just before a syllable starts from silence (b/m/p-like closure).
		for (std::size_t i = 3; i < steps; ++i) {
			if (out[i].open > 0.35f && out[i - 1].open < 0.12f && out[i - 2].open < 0.12f) {
				out[i - 1].closed = std::max(out[i - 1].closed, 0.9f);
				out[i - 2].closed = std::max(out[i - 2].closed, 0.6f);
			}
		}
		return out;
	}

	void VisemeToPhonemes(const Viseme& a_v, float a_out[16])
	{
		std::fill(a_out, a_out + 16, 0.f);
		const float o = std::clamp(a_v.open, 0.f, 1.f);
		const float b = std::clamp(a_v.bright, 0.f, 1.f);
		const float mid = 1.f - std::min(1.f, std::abs(b - 0.5f) * 2.f);
		a_out[0] = o * (0.45f + 0.35f * mid);                         // Aah
		a_out[1] = std::max(0.f, o - 0.65f) * 1.6f;                   // BigAah (loud)
		a_out[2] = std::clamp(a_v.closed, 0.f, 1.f) * (1.f - o);      // BMP
		a_out[5] = o * std::max(0.f, b - 0.62f) * 1.5f;               // Eee
		a_out[6] = o * mid * 0.45f;                                   // Eh
		a_out[11] = o * std::max(0.f, 0.45f - b) * 1.6f;              // Oh
		a_out[12] = o * std::max(0.f, 0.28f - b) * 1.8f;              // OohQ
		for (int i = 0; i < 16; ++i) {
			a_out[i] = std::clamp(a_out[i], 0.f, 1.f);
		}
	}

	double ReadingSeconds(std::string_view a_text)
	{
		std::size_t words = 0;
		bool        inWord = false;
		for (const char c : a_text) {
			const bool space = c == ' ' || c == '\n' || c == '\t';
			if (!space && !inWord) {
				++words;
			}
			inWord = !space;
		}
		return std::clamp(0.8 + words / 2.7, 1.5, 14.0);
	}
}
