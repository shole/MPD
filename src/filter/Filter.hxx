// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#ifndef MPD_FILTER_HXX
#define MPD_FILTER_HXX

#include "pcm/AudioFormat.hxx"

#include <cassert>
#include <cstddef>
#include <span>

class Filter {
protected:
	AudioFormat out_audio_format;

	explicit Filter(AudioFormat _out_audio_format) noexcept
		:out_audio_format(_out_audio_format) {
		assert(out_audio_format.IsValid());
	}

public:
	virtual ~Filter() noexcept = default;

	/**
	 * Returns the #AudioFormat produced by FilterPCM().
	 */
	const AudioFormat &GetOutAudioFormat() const noexcept {
		return out_audio_format;
	}

	/**
	 * Reset the filter's state, e.g. drop/flush buffers.
	 */
	virtual void Reset() noexcept {
	}

	/**
	 * Filters a block of PCM data.
	 *
	 * Throws on error.
	 *
	 * @param src the input buffer
	 * @return the output buffer (will be invalidated by deleting
	 * this object or any call to Reset(), FilterPCM(), ReadMore()
	 * or Flush()); may be empty if no output is currently
	 * available
	 */
	virtual std::span<const std::byte> FilterPCM(std::span<const std::byte> src) = 0;

	/**
	 * Read more result data from the filter.  After each
	 * FilterPCM() call, this should be called repeatedly until it
	 * returns an empty span.
	 *
	 * Throws on error.
	 *
	 * @return the output buffer (will be invalidated by deleting
	 * this object or any call to Reset(), FilterPCM(), ReadMore()
	 * or Flush()); may be empty if no output is currently
	 * available
	 */
	virtual std::span<const std::byte> ReadMore() {
		return {};
	}

	/**
	 * Flush pending data and return it.  This should be called
	 * repeatedly until it returns an empty span.
	 *
	 * After calling this method, this object cannot be used again
         * (not even Reset() is allowed).
	 *
	 * Throws on error.
	 *
	 * @return pending data (will be invalidated by deleting this
	 * object or by any call to Flush())
	 */
	virtual std::span<const std::byte> Flush() {
		return {};
	}
};

#endif
