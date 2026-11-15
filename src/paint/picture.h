// Copyright 2026 Ian Karlsson
//! \file
//! The pictures `<img>` elements show: the bytes the host gives for an src decoded into pixels, kept by the src for
//! every element that names it, and handed to the host (and the GL backend) as images are.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qdom
{

struct Picture
{
	enum class State : uint8_t
	{
		Wanted,  //!< to be asked for at the next frame
		Asked,   //!< asked for, not answered yet
		Ready,
		Failed,  //!< no bytes, or bytes that are no picture
	};
	State state = State::Wanted;
	int id = 0;  //!< its image's number: negative, a still picture
	int width = 0, height = 0;
	std::vector<uint8_t> rgba;
	uint32_t uploaded = 0;  //!< the hand-over it was last given in (Dom::image_generation)

	bool settled() const { return state == State::Ready || state == State::Failed; }
};

//! PNG or JPEG bytes decoded into RGBA rows; false when they are neither.
bool decode_picture(const void *data, size_t size, int &width, int &height, std::vector<uint8_t> &rgba);

} // namespace qdom
