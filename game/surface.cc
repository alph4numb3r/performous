#include "surface.hh"

#include "filemagic.hh"
#include "fs.hh"
#include "configuration.hh"
#include "video_driver.hh"
#include "image.hh"
#include "screen.hh"

#include <fstream>
#include <stdexcept>
#include <sstream>
#include <vector>

#include <cctype>

#include <boost/cstdint.hpp>
#include <boost/format.hpp>
using boost::uint32_t;

Shader& getShader(std::string const& name) {
	return ScreenManager::getSingletonPtr()->window().shader(name);  // FIXME
}

float Dimensions::screenY() const {
	switch (m_screenAnchor) {
	  case CENTER: return 0.0;
	  case TOP: return -0.5 * virtH();
	  case BOTTOM: return 0.5 * virtH();
	}
	throw std::logic_error("Dimensions::screenY(): unknown m_screenAnchor value");
}


template <typename T> void loader(T& target, fs::path const& name) {
	std::string const filename = name.string();
	if (!fs::exists(name)) throw std::runtime_error("File not found: " + filename);

	if ( filemagic::SVG(name) ) {
		// caching is now handled by loadSVG itself.
		loadSVG(target, filename);

	} else if ( filemagic::JPEG(name) ) {
		// this should catch the case where album art lies about being a PNG but really
		// is JPEG with a PNG extension. (caught by the file magic being JPEG)
		loadJPEG(target, filename);

	} else if ( filemagic::PNG(name) ) {
		loadPNG(target, filename);

	} else throw std::runtime_error("Unable to load the image: " + filename);
}

Texture::Texture(std::string const& filename) { loader(*this, filename); }
Surface::Surface(std::string const& filename) { loader(*this, filename); }

// Stuff for converting pix::Format into OpenGL enum values
namespace {
	struct PixFmt {
		PixFmt(): swap() {} // Required by std::map
		PixFmt(GLenum f, GLenum t, bool s): format(f), type(t), swap(s) {}
		GLenum format;
		GLenum type;
		bool swap;
	};
	struct PixFormats {
		typedef std::map<pix::Format, PixFmt> Map;
		Map m;
		PixFormats() {
			using namespace pix;
			m[RGB] = PixFmt(GL_RGB, GL_UNSIGNED_BYTE, false);
			m[BGR] = PixFmt(GL_BGR, GL_UNSIGNED_BYTE, true);
			m[CHAR_RGBA] = PixFmt(GL_RGBA, GL_UNSIGNED_BYTE, false);
			m[INT_ARGB] = PixFmt(GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, true);
		}
	} pixFormats;
	PixFmt const& getPixFmt(pix::Format format) {
		PixFormats::Map::const_iterator it = pixFormats.m.find(format);
		if (it != pixFormats.m.end()) return it->second;
		throw std::logic_error("Unknown pixel format");
	}
	GLint internalFormat() { return GL_EXT_framebuffer_sRGB ? GL_SRGB_ALPHA : GL_RGBA; }
}

void Texture::load(unsigned int width, unsigned int height, pix::Format format, unsigned char const* buffer, float ar) {
	glutil::GLErrorChecker glerror("Texture::load");
	m_ar = ar ? ar : double(width) / height;
	UseTexture texture(*this);
	// When texture area is small, bilinear filter the closest mipmap
	glTexParameterf(type(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
	// When texture area is large, bilinear filter the original
	glTexParameterf(type(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	// The texture wraps over at the edges (repeat)
	glTexParameterf(type(), GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(type(), GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameterf(type(), GL_TEXTURE_MAX_LEVEL, GLEW_VERSION_3_0 ? 4 : 0);  // Mipmaps currently b0rked on Intel, so disable them...
	glerror.check("glTexParameterf");

	// Anisotropy is potential trouble maker
	if (GLEW_EXT_texture_filter_anisotropic) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 16.0f);
	glerror.check("MAX_ANISOTROPY_EXT");

	PixFmt const& f = getPixFmt(format);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, f.swap);
	// Load the data into texture
	if ((isPow2(width) && isPow2(height)) || GLEW_ARB_texture_non_power_of_two) { // Can directly load the texture
		glTexImage2D(type(), 0, internalFormat(), width, height, 0, f.format, f.type, buffer);
	} else {
		int newWidth = prevPow2(width);
		int newHeight = prevPow2(height);
		std::vector<uint32_t> outBuf(newWidth * newHeight);
		gluScaleImage(f.format, width, height, f.type, buffer, newWidth, newHeight, f.type, &outBuf[0]);
		// BIG FAT WARNING: Do not even think of using ARB_texture_rectangle here!
		// Every developer of the game so far has tried doing so, but it just cannot work.
		// (1) no repeat => cannot texture
		// (2) coordinates not normalized => would require special hackery elsewhere
		// Just don't do it in Surface class, thanks. -Tronic
		glTexImage2D(type(), 0, internalFormat(), newWidth, newHeight, 0, f.format, f.type, &outBuf[0]);
	}
	glGenerateMipmap(type());
}

void Surface::load(unsigned int width, unsigned int height, pix::Format format, unsigned char const* buffer, float ar) {
	glutil::GLErrorChecker glerror("Surface::load");
	using namespace pix;
	// Initialize dimensions
	m_width = width; m_height = height;
	if (dimensions.ar() == 0.0) dimensions = Dimensions(ar != 0.0f ? ar : float(width) / height).fixedWidth(1.0f);
	// Load the data into texture
	UseTexture texture(m_texture);
	PixFmt const& f = getPixFmt(format);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, f.swap);
	glTexImage2D(m_texture.type(), 0, internalFormat(), width, height, 0, f.format, f.type, buffer);
}

void Surface::draw() const {
	if (m_width * m_height > 0.0) m_texture.draw(dimensions, TexCoords(tex.x1 * m_width, tex.y1 * m_height, tex.x2 * m_width, tex.y2 * m_height));
}

Surface::Surface(cairo_surface_t* _surf) {
	unsigned int w = cairo_image_surface_get_width(_surf);
	unsigned int h = cairo_image_surface_get_height(_surf);
	load(w, h, pix::INT_ARGB, cairo_image_surface_get_data(_surf));
}
