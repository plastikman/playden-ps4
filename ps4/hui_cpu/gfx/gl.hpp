// Playden PS4 port - the few OpenGL names Playden's view uses, for the CPU renderer.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS4 has no desktop OpenGL. Hui's renderer is replaced by a CPU one with the same classes
// (hui_cpu/gfx/renderer.hpp, gl_batch.hpp); this header shadows hui's gfx/gl.hpp so view.cpp builds
// unchanged. Textures are CPU textures: glDeleteTextures frees them. The PNG capture path's
// framebuffer calls do nothing and report an incomplete framebuffer, so a capture fails cleanly.
#pragma once
#include <cstdint>

using GLuint = std::uint32_t;
using GLint = std::int32_t;
using GLenum = std::uint32_t;
using GLsizei = std::int32_t;
using GLbitfield = std::uint32_t;
using GLfloat = float;

constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum GL_RENDERBUFFER = 0x8D41;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000;
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x0100;
constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x0400;

namespace hui::gfx::cpu
{
void delete_textures(GLsizei count, const GLuint *textures);
} // namespace hui::gfx::cpu

inline void glDeleteTextures(GLsizei count, const GLuint *textures)
{
    hui::gfx::cpu::delete_textures(count, textures);
}
inline GLenum glGetError() { return GL_NO_ERROR; }
inline void glGenFramebuffers(GLsizei count, GLuint *names) { for (GLsizei i = 0; i < count; ++i) names[i] = 0; }
inline void glGenRenderbuffers(GLsizei count, GLuint *names) { for (GLsizei i = 0; i < count; ++i) names[i] = 0; }
inline void glDeleteFramebuffers(GLsizei, const GLuint *) {}
inline void glDeleteRenderbuffers(GLsizei, const GLuint *) {}
inline void glBindFramebuffer(GLenum, GLuint) {}
inline void glBindRenderbuffer(GLenum, GLuint) {}
inline void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei) {}
inline void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
inline GLenum glCheckFramebufferStatus(GLenum) { return 0; }
inline void glViewport(GLint, GLint, GLsizei, GLsizei) {}
inline void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
inline void glClear(GLbitfield) {}
inline void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) {}
