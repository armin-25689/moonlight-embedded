/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#define FRAG_PARAM_YUVMAT 0
#define FRAG_PARAM_OFFSET 1
#define FRAG_PARAM_YUVORDER 2
#define FRAG_PARAM_PLANE0 3
#define FRAG_PARAM_PLANE1 4
#define FRAG_PARAM_PLANE2 5
#define FRAG_PARAM_CUTWIDTH 6

static const char* vertex_source = {
"#version 300 es\n"
"\n"
"layout (location = 0) in vec2 position;\n"
"layout (location = 1) in vec2 aTexCoord;\n"
"out vec2 tex_position;\n"
"\n"
"void main() {\n"
"  gl_Position = vec4(position, 0, 1);\n"
"  tex_position = aTexCoord;\n\n"
"}\n"
};

static const char* fragment_source_3plane = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D ymap;\n"
"uniform sampler2D umap;\n"
"uniform sampler2D vmap;\n"
"uniform float cutwidth;\n"
"\n"
"void main() {\n"
"  if (tex_position.x > cutwidth) {\n"
"    FragColor = vec4(0.0f,0.0f,0.0f,1.0f);\n"
"    return;\n"
"  }\n"
"  vec3 YCbCr = vec3(\n"
"    texture2D(ymap, tex_position).r,\n"
"    texture2D(umap, tex_position).r,\n"
"    texture2D(vmap, tex_position).r\n"
"  );\n"
"\n"
"  YCbCr -= offset;\n"
"  FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const char* fragment_source_nv12 = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D plane1;\n"
"uniform sampler2D plane2;\n"
"\n"
"void main() {\n"
"  vec3 YCbCr = vec3(\n"
"                    texture2D(plane1, tex_position)[0],\n"
"                    texture2D(plane2, tex_position).xy\n"
"                   );\n"
"\n"
"  YCbCr -= offset;\n"
"  FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const char* fragment_source_packed = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D vuyamap;\n"
"\n"
"void main() {\n"
"  vec4 vuya = texture2D(vuyamap, tex_position);\n"
"  vec3 YCbCr = vec3(vuya[yuvorder[0]], vuya[yuvorder[1]], vuya[yuvorder[2]]);"
"\n"
"  YCbCr -= offset;\n"
"  FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const float vertices[] = {
 // pos .... // tex coords
 1.0f, 1.0f, 1.0f, 0.0f,
 1.0f, -1.0f, 1.0f, 1.0f,
 -1.0f, -1.0f, 0.0f, 1.0f,
 -1.0f, 1.0f, 0.0f, 0.0f,
};

static const GLuint elements[] = {
  0, 1, 2,
  2, 3, 0
};

// 0 is vuyx_order, 1 is xvyu_order,  2 is yuvx_order
static const int plane_order[3][4] = { { 2, 1, 0, 3 }, { 1, 0, 2, 3 }, { 0, 1, 2, 3 } };
static const float limitedOffsets[] = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
static const float fullOffsets[] = { 0.0f, 128.0f / 255.0f, 128.0f / 255.0f };
static const float bt601Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.3917f, 2.0172f,
  1.5960f, -0.8129f, 0.0f
};
static const float bt601Full[] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.3441f, 1.7720f,
    1.4020f, -0.7141f, 0.0f
};
static const float bt709Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.2132f, 2.1124f,
  1.7927f, -0.5329f, 0.0f
};
static const float bt709Full[] = {
  1.0f, 1.0f, 1.0f,
  0.0f, -0.1873f, 1.8556f,
  1.5748f, -0.4681f, 0.0f
};
static const float bt2020Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.1874f, 2.1418f,
  1.6781f, -0.6505f, 0.0f
};
static const float bt2020Full[] = {
  1.0f, 1.0f, 1.0f,
  0.0f, -0.1646f, 1.8814f,
  1.4746f, -0.5714f, 0.0f
};
