#pragma once
// RIFF/WAVE decoding and mouth-shape analysis. Pure C++, no game dependencies (unit-tested).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Core
{
	struct PcmClip
	{
		std::uint32_t      sampleRate{ 0 };
		std::uint16_t      channels{ 0 };
		std::vector<float> samples;  // interleaved, [-1, 1]

		[[nodiscard]] std::size_t frames() const { return channels ? samples.size() / channels : 0; }
		[[nodiscard]] double      seconds() const { return sampleRate ? static_cast<double>(frames()) / sampleRate : 0.0; }
	};

	// Decodes PCM 8/16/24/32-bit integer or 32-bit float WAV (incl. WAVE_FORMAT_EXTENSIBLE),
	// mono or stereo. Tolerates streamed headers with 0/0xFFFFFFFF sizes. Returns false and sets
	// a_error when the data is not something we can play.
	bool DecodeWav(std::span<const std::uint8_t> a_data, PcmClip& a_out, std::string& a_error);

	// Converts to 16-bit interleaved PCM for submission to XAudio2.
	std::vector<std::int16_t> ToInt16(const PcmClip& a_clip);

	// Per-frame visemes for amplitude-driven lip sync, sampled at kVisemeRate Hz.
	inline constexpr int kVisemeRate = 60;

	struct Viseme
	{
		float open{ 0.f };    // jaw/mouth opening, 0..1
		float bright{ 0.f };  // 0 = rounded (oh/oo), 1 = spread (ee/eh)
		float closed{ 0.f };  // lip closure pulse at onsets (b/m/p), 0..1
	};

	std::vector<Viseme> AnalyseVisemes(const PcmClip& a_clip);

	// Maps a viseme onto Skyrim's 16 phoneme morph weights
	// (Aah BigAah BMP ChJSh DST Eee Eh FV I K N Oh OohQ R Th W).
	void VisemeToPhonemes(const Viseme& a_v, float a_out[16]);

	// Reading-time estimate for a subtitle with no audio.
	double ReadingSeconds(std::string_view a_text);
}
