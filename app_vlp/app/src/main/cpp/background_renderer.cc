/*
 * Copyright 2017 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This modules handles drawing the passthrough camera image into the OpenGL
// scene.

#include "background_renderer.h"

#include <type_traits>

#include "util.h"

namespace hello_ar {
namespace {
// Positions of the quad vertices in clip space (X, Y).
const GLfloat kVertices[] = {
    -1.0f, -1.0f, +1.0f, -1.0f, -1.0f, +1.0f, +1.0f, +1.0f,
};

constexpr char kCameraVertexShaderFilename[] = "shaders/screenquad.vert";
constexpr char kCameraFragmentShaderFilename[] = "shaders/screenquad.frag";

constexpr char kDepthVisualizerVertexShaderFilename[] =
    "shaders/background_show_depth_color_visualization.vert";
constexpr char kDepthVisualizerFragmentShaderFilename[] =
    "shaders/background_show_depth_color_visualization.frag";
constexpr char kDepthColorPaletteImageFilename[] =
    "models/depth_color_palette.png";

void UpdateUvsIfNeeded(const ArSession* session, const ArFrame* frame,
                       bool* uvs_initialized, float* transformed_uvs) {
  constexpr int kPanelVerticesCount = 4;
  int32_t geometry_changed = 0;
  ArFrame_getDisplayGeometryChanged(session, frame, &geometry_changed);
  if (geometry_changed != 0 || !(*uvs_initialized)) {
    ArFrame_transformCoordinates2d(
        session, frame, AR_COORDINATES_2D_OPENGL_NORMALIZED_DEVICE_COORDINATES,
        kPanelVerticesCount, kVertices,
        AR_COORDINATES_2D_TEXTURE_NORMALIZED, transformed_uvs);
    *uvs_initialized = true;
  }
}

}  // namespace

void BackgroundRenderer::InitializeGlContent(AAssetManager* asset_manager,
                                             int depth_texture_id) {
  // Defines the default background, which is the color camera image.
  glGenTextures(1, &camera_texture_id_);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, camera_texture_id_);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  camera_program_ =
      util::CreateProgram(kCameraVertexShaderFilename,
                          kCameraFragmentShaderFilename, asset_manager);
  if (!camera_program_) {
    LOGE("Could not create program.");
  }

  camera_texture_uniform_ = glGetUniformLocation(camera_program_, "sTexture");
  camera_position_attrib_ = glGetAttribLocation(camera_program_, "a_Position");
  camera_tex_coord_attrib_ = glGetAttribLocation(camera_program_, "a_TexCoord");

  // Defines the color palette to use when rendering depth.
  glGenTextures(1, &depth_color_palette_id_);
  glBindTexture(GL_TEXTURE_2D, depth_color_palette_id_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (!util::LoadPngFromAssetManager(GL_TEXTURE_2D,
                                     kDepthColorPaletteImageFilename)) {
    LOGE("Could not load png texture for depth color palette.");
  }

  // Defines the depth visualization background, which shows the current depth.
  depth_program_ = util::CreateProgram(kDepthVisualizerVertexShaderFilename,
                                       kDepthVisualizerFragmentShaderFilename,
                                       asset_manager);
  if (!depth_program_) {
    LOGE("Could not create program.");
  }

  depth_texture_uniform_ =
      glGetUniformLocation(depth_program_, "u_DepthTexture");
  depth_color_palette_uniform_ =
      glGetUniformLocation(depth_program_, "u_ColorMap");
  depth_position_attrib_ = glGetAttribLocation(depth_program_, "a_Position");
  depth_tex_coord_attrib_ = glGetAttribLocation(depth_program_, "a_TexCoord");

  depth_texture_id_ = depth_texture_id;
}

void BackgroundRenderer::Draw(const ArSession* session, const ArFrame* frame,
                              bool debug_show_depth_map) {
  static_assert(std::extent<decltype(kVertices)>::value == kNumVertices * 2,
                "Incorrect kVertices length");

  UpdateUvsIfNeeded(session, frame, &uvs_initialized_, transformed_uvs_);

  int64_t frame_timestamp;
  ArFrame_getTimestamp(session, frame, &frame_timestamp);
  if (frame_timestamp == 0) {
    // Suppress rendering if the camera did not produce the first frame yet.
    // This is to avoid drawing possible leftover data from previous sessions if
    // the texture is reused.
    return;
  }

  if (depth_texture_id_ == -1 || depth_color_palette_id_ == -1 ||
      camera_texture_id_ == -1) {
    return;
  }

  glDepthMask(GL_FALSE);

  if (debug_show_depth_map) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_texture_id_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depth_color_palette_id_);
    glUseProgram(depth_program_);
    glUniform1i(depth_texture_uniform_, 0);
    glUniform1i(depth_color_palette_uniform_, 1);

    // Set the vertex positions and texture coordinates.
    glVertexAttribPointer(depth_position_attrib_, 2, GL_FLOAT, false, 0,
                          kVertices);
    glVertexAttribPointer(depth_tex_coord_attrib_, 2, GL_FLOAT, false, 0,
                          transformed_uvs_);
    glEnableVertexAttribArray(depth_position_attrib_);
    glEnableVertexAttribArray(depth_tex_coord_attrib_);
  } else {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, camera_texture_id_);
    glUseProgram(camera_program_);
    glUniform1i(camera_texture_uniform_, 0);

    // Set the vertex positions and texture coordinates.
    glVertexAttribPointer(camera_position_attrib_, 2, GL_FLOAT, false, 0,
                          kVertices);
    glVertexAttribPointer(camera_tex_coord_attrib_, 2, GL_FLOAT, false, 0,
                          transformed_uvs_);
    glEnableVertexAttribArray(camera_position_attrib_);
    glEnableVertexAttribArray(camera_tex_coord_attrib_);
  }

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // Disable vertex arrays
  if (debug_show_depth_map) {
    glDisableVertexAttribArray(depth_position_attrib_);
    glDisableVertexAttribArray(depth_tex_coord_attrib_);
  } else {
    glDisableVertexAttribArray(camera_position_attrib_);
    glDisableVertexAttribArray(camera_tex_coord_attrib_);
  }

  glUseProgram(0);
  glDepthMask(GL_TRUE);
  util::CheckGlError("BackgroundRenderer::Draw() error");
}

GLuint BackgroundRenderer::GetTextureId() const { return camera_texture_id_; }

void BackgroundRenderer::DrawDepthPanel(const ArSession* session,
                                        const ArFrame* frame, float x0,
                                        float y0, float x1, float y1) {
  UpdateUvsIfNeeded(session, frame, &uvs_initialized_, transformed_uvs_);
  DrawDepthPanelWithTexture(depth_texture_id_, transformed_uvs_, x0, y0, x1, y1);
}

void BackgroundRenderer::DrawDepthPanelWithTexture(GLuint depth_texture_id,
                                                   const float* tex_uvs,
                                                   float x0, float y0,
                                                   float x1, float y1) {
  if (depth_texture_id == 0 || depth_color_palette_id_ == -1 || tex_uvs == nullptr) {
    return;
  }

  const GLfloat panel_vertices[] = {
      x0, y0, x1, y0, x0, y1, x1, y1,
  };

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, depth_texture_id);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, depth_color_palette_id_);
  glUseProgram(depth_program_);
  glUniform1i(depth_texture_uniform_, 0);
  glUniform1i(depth_color_palette_uniform_, 1);

  glVertexAttribPointer(depth_position_attrib_, 2, GL_FLOAT, false, 0,
                        panel_vertices);
  glVertexAttribPointer(depth_tex_coord_attrib_, 2, GL_FLOAT, false, 0,
                        tex_uvs);
  glEnableVertexAttribArray(depth_position_attrib_);
  glEnableVertexAttribArray(depth_tex_coord_attrib_);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(depth_position_attrib_);
  glDisableVertexAttribArray(depth_tex_coord_attrib_);

  glUseProgram(0);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
}

}  // namespace hello_ar
