// Filename: glGraphicsStateGuardian_src.cxx
// Created by:  drose (02Feb99)
// Updated by: fperazzi, PandaSE (05May10) (added
//   get_supports_cg_profile)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#include "config_util.h"
#include "displayRegion.h"
#include "renderBuffer.h"
#include "geom.h"
#include "geomVertexData.h"
#include "geomTriangles.h"
#include "geomTristrips.h"
#include "geomTrifans.h"
#include "geomLines.h"
#include "geomLinestrips.h"
#include "geomPoints.h"
#include "geomVertexReader.h"
#include "graphicsWindow.h"
#include "lens.h"
#include "perspectiveLens.h"
#include "directionalLight.h"
#include "pointLight.h"
#include "spotlight.h"
#include "planeNode.h"
#include "fog.h"
#include "clockObject.h"
#include "string_utils.h"
#include "nodePath.h"
#include "dcast.h"
#include "pvector.h"
#include "vector_string.h"
#include "string_utils.h"
#include "pnmImage.h"
#include "config_gobj.h"
#include "lightMutexHolder.h"
#include "indirectLess.h"
#include "pStatTimer.h"
#include "load_prc_file.h"
#include "bamCache.h"
#include "bamCacheRecord.h"
#include "alphaTestAttrib.h"
#include "clipPlaneAttrib.h"
#include "colorWriteAttrib.h"
#include "cullFaceAttrib.h"
#include "depthOffsetAttrib.h"
#include "depthWriteAttrib.h"
#include "fogAttrib.h"
#include "lightAttrib.h"
#include "materialAttrib.h"
#include "rescaleNormalAttrib.h"
#include "scissorAttrib.h"
#include "shadeModelAttrib.h"
#include "stencilAttrib.h"
#include "graphicsEngine.h"
#include "shaderGenerator.h"

#if defined(HAVE_CG) && !defined(OPENGLES)
#include "Cg/cgGL.h"
#endif

#include <algorithm>

TypeHandle CLP(GraphicsStateGuardian)::_type_handle;

PStatCollector CLP(GraphicsStateGuardian)::_load_display_list_pcollector("Draw:Transfer data:Display lists");
PStatCollector CLP(GraphicsStateGuardian)::_primitive_batches_display_list_pcollector("Primitive batches:Display lists");
PStatCollector CLP(GraphicsStateGuardian)::_vertices_display_list_pcollector("Vertices:Display lists");
PStatCollector CLP(GraphicsStateGuardian)::_vertices_immediate_pcollector("Vertices:Immediate mode");
PStatCollector CLP(GraphicsStateGuardian)::_memory_barrier_pcollector("Draw:Memory barriers");
PStatCollector CLP(GraphicsStateGuardian)::_vertex_array_update_pcollector("Draw:Update arrays");
PStatCollector CLP(GraphicsStateGuardian)::_texture_update_pcollector("Draw:Update texture");
PStatCollector CLP(GraphicsStateGuardian)::_fbo_bind_pcollector("Draw:Bind FBO");
PStatCollector CLP(GraphicsStateGuardian)::_check_error_pcollector("Draw:Check errors");

#ifdef OPENGLES_2
PT(Shader) CLP(GraphicsStateGuardian)::_default_shader = NULL;
#endif

// The following noop functions are assigned to the corresponding
// glext function pointers in the class, in case the functions are not
// defined by the GL, just so it will always be safe to call the
// extension functions.

static void APIENTRY
null_glPointParameterfv(GLenum, const GLfloat *) {
}

static void APIENTRY
null_glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                         GLsizei count, GLenum type, const GLvoid *indices) {
  // If we don't support glDrawRangeElements(), just use the original
  // glDrawElements() instead.
  glDrawElements(mode, count, type, indices);
}

static void APIENTRY
null_glActiveTexture(GLenum gl_texture_stage) {
  // If we don't support multitexture, we'd better not try to request
  // a texture beyond the first texture stage.
  nassertv(gl_texture_stage == GL_TEXTURE0);
}

static void APIENTRY
null_glBlendEquation(GLenum) {
}

static void APIENTRY
null_glBlendColor(GLclampf, GLclampf, GLclampf, GLclampf) {
}

#ifdef OPENGLES_2
// We have a default shader that will be applied when there
// isn't any shader applied (e.g. if it failed to compile).
// We need this because OpenGL ES 2.x does not have
// a fixed-function pipeline.
// This default shader just outputs a red color, telling
// the user that something went wrong.
CPT(Shader::ShaderFile) default_shader_name = new Shader::ShaderFile("default-shader");
CPT(Shader::ShaderFile) default_shader_body = new Shader::ShaderFile("\
uniform mediump mat4 p3d_ModelViewProjectionMatrix;\
attribute highp vec4 p3d_Vertex;\
void main(void) {\
  gl_Position = p3d_ModelViewProjectionMatrix * p3d_Vertex;\
}\n",
"void main(void) {\
  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\
}\n", "", "", "");
#endif


////////////////////////////////////////////////////////////////////
//     Function: uchar_bgr_to_rgb
//  Description: Recopies the given array of pixels, converting from
//               BGR to RGB arrangement.
////////////////////////////////////////////////////////////////////
static void
uchar_bgr_to_rgb(unsigned char *dest, const unsigned char *source,
                 int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest += 3;
    source += 3;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: uchar_bgra_to_rgba
//  Description: Recopies the given array of pixels, converting from
//               BGRA to RGBA arrangement.
////////////////////////////////////////////////////////////////////
static void
uchar_bgra_to_rgba(unsigned char *dest, const unsigned char *source,
                   int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest[3] = source[3];
    dest += 4;
    source += 4;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: ushort_bgr_to_rgb
//  Description: Recopies the given array of pixels, converting from
//               BGR to RGB arrangement.
////////////////////////////////////////////////////////////////////
static void
ushort_bgr_to_rgb(unsigned short *dest, const unsigned short *source,
                  int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest += 3;
    source += 3;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: ushort_bgra_to_rgba
//  Description: Recopies the given array of pixels, converting from
//               BGRA to RGBA arrangement.
////////////////////////////////////////////////////////////////////
static void
ushort_bgra_to_rgba(unsigned short *dest, const unsigned short *source,
                    int num_pixels) {
  for (int i = 0; i < num_pixels; i++) {
    dest[0] = source[2];
    dest[1] = source[1];
    dest[2] = source[0];
    dest[3] = source[3];
    dest += 4;
    source += 4;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: fix_component_ordering
//  Description: Reverses the order of the components within the
//               image, to convert (for instance) GL_BGR to GL_RGB.
//               Returns the byte pointer representing the converted
//               image, or the original image if it is unchanged.
//
//               new_image must be supplied; it is the PTA_uchar that
//               will be used to hold the converted image if required.
//               It will be modified only if the conversion is
//               necessary, in which case the data will be stored
//               there, and this pointer will be returned.  If the
//               conversion is not necessary, this pointer will be
//               left unchanged.
////////////////////////////////////////////////////////////////////
static const unsigned char *
fix_component_ordering(PTA_uchar &new_image,
                       const unsigned char *orig_image, size_t orig_image_size,
                       GLenum external_format, Texture *tex) {
  const unsigned char *result = orig_image;

  switch (external_format) {
  case GL_RGB:
    switch (tex->get_component_type()) {
    case Texture::T_unsigned_byte:
      new_image = PTA_uchar::empty_array(orig_image_size);
      uchar_bgr_to_rgb(new_image, orig_image, orig_image_size / 3);
      result = new_image;
      break;

    case Texture::T_unsigned_short:
      new_image = PTA_uchar::empty_array(orig_image_size);
      ushort_bgr_to_rgb((unsigned short *)new_image.p(),
                        (const unsigned short *)orig_image,
                        orig_image_size / 6);
      result = new_image;
      break;

    default:
      break;
    }
    break;

  case GL_RGBA:
    switch (tex->get_component_type()) {
    case Texture::T_unsigned_byte:
      new_image = PTA_uchar::empty_array(orig_image_size);
      uchar_bgra_to_rgba(new_image, orig_image, orig_image_size / 4);
      result = new_image;
      break;

    case Texture::T_unsigned_short:
      new_image = PTA_uchar::empty_array(orig_image_size);
      ushort_bgra_to_rgba((unsigned short *)new_image.p(),
                          (const unsigned short *)orig_image,
                          orig_image_size / 8);
      result = new_image;
      break;

    default:
      break;
    }
    break;

  default:
    break;
  }

  return result;
}

//#--- Zhao Nov/2011
string CLP(GraphicsStateGuardian)::get_driver_vendor() { return _gl_vendor; }
string CLP(GraphicsStateGuardian)::get_driver_renderer() { return _gl_renderer; }

string CLP(GraphicsStateGuardian)::get_driver_version() { return _gl_version; }
int CLP(GraphicsStateGuardian)::get_driver_version_major() { return _gl_version_major; }
int CLP(GraphicsStateGuardian)::get_driver_version_minor() { return _gl_version_minor; }
int CLP(GraphicsStateGuardian)::get_driver_shader_version_major() { return _gl_shadlang_ver_major; }
int CLP(GraphicsStateGuardian)::get_driver_shader_version_minor() { return _gl_shadlang_ver_minor; }

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::Constructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
CLP(GraphicsStateGuardian)::
CLP(GraphicsStateGuardian)(GraphicsEngine *engine, GraphicsPipe *pipe) :
  GraphicsStateGuardian(CS_yup_right, engine, pipe)
{
  _error_count = 0;

  // Hack.  Turn on the flag that we turned off at a higher level,
  // since we know this works properly in OpenGL, and we want the
  // performance benefit it gives us.
  _prepared_objects->_support_released_buffer_cache = true;

  // Assume that we will get a hardware-accelerated context, unless
  // the window tells us otherwise.
  _is_hardware = true;

  // calling glGetError() forces a sync, this turns it on if you want to.
  _check_errors = gl_check_errors;
  _force_flush = gl_force_flush;

  _scissor_enabled = false;

#ifdef DO_PSTATS
  if (gl_finish) {
    GLCAT.warning()
      << "The config variable gl-finish is set to true.  This may have a substantial negative impact on your render performance.\n";
  }
#endif  // DO_PSTATS
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::Destructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
CLP(GraphicsStateGuardian)::
~CLP(GraphicsStateGuardian)() {
  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "GLGraphicsStateGuardian " << this << " destructing\n";
  }

  close_gsg();

  if (_stencil_render_states) {
    delete _stencil_render_states;
    _stencil_render_states = 0;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::debug_callback
//       Access: Public, Static
//  Description: This is called by the GL if an error occurs, if
//               gl_debug has been enabled (and the driver supports
//               the GL_ARB_debug_output extension).
////////////////////////////////////////////////////////////////////
#ifndef OPENGLES_1
void CLP(GraphicsStateGuardian)::
debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, GLvoid *userParam) {
  // Determine how to map the severity level.
  NotifySeverity level;
  switch (severity) {
  case GL_DEBUG_SEVERITY_HIGH:
    level = NS_error;
    break;

  case GL_DEBUG_SEVERITY_MEDIUM:
    level = NS_warning;
    break;

  case GL_DEBUG_SEVERITY_LOW:
    level = NS_info;
    break;

  case GL_DEBUG_SEVERITY_NOTIFICATION:
    level = NS_debug;
    break;

  default:
    level = NS_fatal; //???
    break;
  }

  string msg_str(message, length);
  GLCAT.out(level) << msg_str << "\n";

#ifndef NDEBUG
  if (level >= gl_debug_abort_level.get_value()) {
    abort();
  }
#endif
}
#endif  // OPENGLES_1

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::reset
//       Access: Public, Virtual
//  Description: Resets all internal state as if the gsg were newly
//               created.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
reset() {
  free_pointers();
  GraphicsStateGuardian::reset();

  // Build _inv_state_mask as a mask of 1's where we don't care, and
  // 0's where we do care, about the state.
  //_inv_state_mask = RenderState::SlotMask::all_on();
  _inv_state_mask.clear_bit(ShaderAttrib::get_class_slot());
  _inv_state_mask.clear_bit(AlphaTestAttrib::get_class_slot());
  _inv_state_mask.clear_bit(AntialiasAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ClipPlaneAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ColorAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ColorScaleAttrib::get_class_slot());
  _inv_state_mask.clear_bit(CullFaceAttrib::get_class_slot());
  _inv_state_mask.clear_bit(DepthOffsetAttrib::get_class_slot());
  _inv_state_mask.clear_bit(DepthTestAttrib::get_class_slot());
  _inv_state_mask.clear_bit(DepthWriteAttrib::get_class_slot());
  _inv_state_mask.clear_bit(RenderModeAttrib::get_class_slot());
  _inv_state_mask.clear_bit(RescaleNormalAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ShadeModelAttrib::get_class_slot());
  _inv_state_mask.clear_bit(TransparencyAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ColorWriteAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ColorBlendAttrib::get_class_slot());
  _inv_state_mask.clear_bit(TextureAttrib::get_class_slot());
  _inv_state_mask.clear_bit(TexGenAttrib::get_class_slot());
  _inv_state_mask.clear_bit(TexMatrixAttrib::get_class_slot());
  _inv_state_mask.clear_bit(MaterialAttrib::get_class_slot());
  _inv_state_mask.clear_bit(LightAttrib::get_class_slot());
  _inv_state_mask.clear_bit(StencilAttrib::get_class_slot());
  _inv_state_mask.clear_bit(FogAttrib::get_class_slot());
  _inv_state_mask.clear_bit(ScissorAttrib::get_class_slot());

  // Output the vendor and version strings.
  query_gl_version();

  if (_gl_version_major == 0) {
    // Couldn't get GL.  Fail.
    mark_new();
    return;
  }

  // Save the extensions tokens.
  _extensions.clear();
  save_extensions((const char *)glGetString(GL_EXTENSIONS));
  get_extra_extensions();
  report_extensions();

  // Initialize OpenGL debugging output first, if enabled and supported.
  _supports_debug = false;
  _use_object_labels = false;
#ifndef OPENGLES_1
  if (gl_debug) {
    PFNGLDEBUGMESSAGECALLBACKPROC _glDebugMessageCallback;
    PFNGLDEBUGMESSAGECONTROLPROC _glDebugMessageControl;

    if (is_at_least_gl_version(4, 3) || has_extension("GL_KHR_debug")) {
#ifdef OPENGLES
      _glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)
        get_extension_func("glDebugMessageCallbackKHR");
      _glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLPROC)
        get_extension_func("glDebugMessageControlKHR");
      _glObjectLabel = (PFNGLOBJECTLABELPROC)
        get_extension_func("glObjectLabelKHR");
#else
      _glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)
        get_extension_func("glDebugMessageCallback");
      _glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLPROC)
        get_extension_func("glDebugMessageControl");
      _glObjectLabel = (PFNGLOBJECTLABELPROC)
        get_extension_func("glObjectLabel");
#endif
      glEnable(GL_DEBUG_OUTPUT); // Not supported in ARB version
      _supports_debug = true;
      _use_object_labels = gl_debug_object_labels;

    } else if (has_extension("GL_ARB_debug_output")) {
      _glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)
        get_extension_func("glDebugMessageCallbackARB");
      _glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLPROC)
        get_extension_func("glDebugMessageControlARB");
      _supports_debug = true;
    }

    if (_supports_debug) {
      // Set the categories we want to listen to.
      _glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH,
                             0, NULL, GLCAT.is_error());
      _glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM,
                             0, NULL, GLCAT.is_warning());
      _glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW,
                             0, NULL, GLCAT.is_info());
      _glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
                             0, NULL, GLCAT.is_debug());

      // Enable the callback.
      _glDebugMessageCallback((GLDEBUGPROC) &debug_callback, (void*)this);
      if (gl_debug_synchronous) {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      }

      GLCAT.debug() << "gl-debug enabled.\n";
    } else {
      GLCAT.debug() << "gl-debug enabled, but NOT supported.\n";
    }
  } else {
    GLCAT.debug() << "gl-debug NOT enabled.\n";

    // However, still check if it is supported.
    _supports_debug = is_at_least_gl_version(4, 3)
                   || has_extension("GL_KHR_debug")
                   || has_extension("GL_ARB_debug_output");
  }
#endif  // OPENGLES_1

  _supported_geom_rendering =
    Geom::GR_indexed_point |
    Geom::GR_point | Geom::GR_point_uniform_size |
    Geom::GR_indexed_other |
    Geom::GR_triangle_strip | Geom::GR_triangle_fan |
    Geom::GR_line_strip |
    Geom::GR_flat_last_vertex;

  _supports_point_parameters = false;

  if (is_at_least_gl_version(1, 4)) {
    _supports_point_parameters = true;
    _glPointParameterfv = (PFNGLPOINTPARAMETERFVPROC)
      get_extension_func("glPointParameterfv");

  } else if (has_extension("GL_ARB_point_parameters")) {
    _supports_point_parameters = true;
    _glPointParameterfv = (PFNGLPOINTPARAMETERFVPROC)
      get_extension_func("glPointParameterfvARB");
  }
  if (_supports_point_parameters) {
    if (_glPointParameterfv == NULL) {
      GLCAT.warning()
        << "glPointParameterfv advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_point_parameters = false;
    }
  }
  if (_supports_point_parameters) {
    _supported_geom_rendering |= Geom::GR_point_perspective | Geom::GR_point_scale;
  } else {
    _glPointParameterfv = null_glPointParameterfv;
  }

  _supports_point_sprite = has_extension("GL_ARB_point_sprite") || has_extension("GL_OES_point_sprite");
  if (_supports_point_sprite) {
    // It appears that the point_sprite extension doesn't support
    // texture transforms on the generated texture coordinates.  How
    // inconsistent.  Because of this, we don't advertise
    // GR_point_sprite_tex_matrix.
    _supported_geom_rendering |= Geom::GR_point_sprite;
  }

#ifndef OPENGLES
  _glPrimitiveRestartIndex = NULL;

  if (is_at_least_gl_version(4, 3) || has_extension("GL_ARB_ES3_compatibility")) {
    // As long as we enable this, OpenGL will always use the highest possible index
    // for a numeric type as strip cut index, which coincides with our convention.
    // This saves us a call to glPrimitiveRestartIndex.
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    _supported_geom_rendering |= Geom::GR_strip_cut_index;

  } else if (is_at_least_gl_version(3, 1)) {
    glEnable(GL_PRIMITIVE_RESTART);
    _supported_geom_rendering |= Geom::GR_strip_cut_index;

    _glPrimitiveRestartIndex = (PFNGLPRIMITIVERESTARTINDEXPROC)
      get_extension_func("glPrimitiveRestartIndex");

  } else if (has_extension("GL_NV_primitive_restart")) {
    glEnable(GL_PRIMITIVE_RESTART_NV);
    _supported_geom_rendering |= Geom::GR_strip_cut_index;

    _glPrimitiveRestartIndex = (PFNGLPRIMITIVERESTARTINDEXPROC)
      get_extension_func("glPrimitiveRestartIndexNV");
  }
#endif

  _supports_vertex_blend = has_extension("GL_ARB_vertex_blend");

  if (_supports_vertex_blend) {
    _glWeightPointer = (PFNGLWEIGHTPOINTERARBPROC)
      get_extension_func("glWeightPointerARB");
    _glVertexBlend = (PFNGLVERTEXBLENDARBPROC)
      get_extension_func("glVertexBlendARB");
    _glWeightfv = (PFNGLWEIGHTFVARBPROC)
      get_extension_func("glWeightfvARB");
    _glWeightdv = (PFNGLWEIGHTDVARBPROC)
      get_extension_func("glWeightdvARB");

    if (_glWeightPointer == NULL || _glVertexBlend == NULL ||
        GLfv(_glWeight) == NULL) {
      GLCAT.warning()
        << "Vertex blending advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_vertex_blend = false;
    }
  }

#ifndef OPENGLES
  if (_supports_vertex_blend) {
    glEnable(GL_WEIGHT_SUM_UNITY_ARB);

    GLint max_vertex_units = 0;
    glGetIntegerv(GL_MAX_VERTEX_UNITS_ARB, &max_vertex_units);
    _max_vertex_transforms = max_vertex_units;
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "max vertex transforms = " << _max_vertex_transforms << "\n";
    }
  }
#endif

  _supports_matrix_palette = false;
#ifndef OPENGLES
  if (has_extension("GL_ARB_matrix_palette")) {
    _supports_matrix_palette = true;
    _glCurrentPaletteMatrix = (PFNGLCURRENTPALETTEMATRIXARBPROC)
      get_extension_func("glCurrentPaletteMatrixARB");
    _glMatrixIndexPointer = (PFNGLMATRIXINDEXPOINTERARBPROC)
      get_extension_func("glMatrixIndexPointerARB");
    _glMatrixIndexuiv = (PFNGLMATRIXINDEXUIVARBPROC)
      get_extension_func("glMatrixIndexuivARB");

    if (_glCurrentPaletteMatrix == NULL ||
        _glMatrixIndexPointer == NULL ||
        _glMatrixIndexuiv == NULL) {
      GLCAT.warning()
        << "Matrix palette advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_matrix_palette = false;
    }
  }
#else
  if (has_extension("GL_OES_matrix_palette")) {
    _supports_matrix_palette = true;
    _glCurrentPaletteMatrix = (PFNGLCURRENTPALETTEMATRIXOESPROC)
      get_extension_func("glCurrentPaletteMatrixOES");
    _glMatrixIndexPointer = (PFNGLMATRIXINDEXPOINTEROESPROC)
      get_extension_func("glMatrixIndexPointerOES");

    if (_glCurrentPaletteMatrix == NULL ||
        _glMatrixIndexPointer == NULL) {
      GLCAT.warning()
        << "Matrix palette advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_matrix_palette = false;
    }
  }
#endif

  /*
    The matrix_palette support in this module is completely untested
    (because I don't happen to have a card handy whose driver supports
    this extension), so I have this ConfigVariable set to
    unconditionally set this flag off for now, to protect the unwary.
    When we have shown that the code works, we should remove this bit.
    In the meantime, you must put both "matrix-palette 1" and
    "gl-matrix-palette 1" in your Config.prc to exercise the new
    code. */
  if (!gl_matrix_palette) {
    if (_supports_matrix_palette) {
      if (GLCAT.is_debug()) {
        GLCAT.debug() << "Forcing off matrix palette support.\n";
      }
    }
    _supports_matrix_palette = false;
  }

  if (_supports_matrix_palette) {
    GLint max_palette_matrices = 0;
#ifdef OPENGLES_1
    glGetIntegerv(GL_MAX_PALETTE_MATRICES_OES, &max_palette_matrices);
#endif
#ifndef OPENGLES
    glGetIntegerv(GL_MAX_PALETTE_MATRICES_ARB, &max_palette_matrices);
#endif
    _max_vertex_transform_indices = max_palette_matrices;
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "max vertex transform indices = " << _max_vertex_transform_indices << "\n";
    }
  }

  _supports_draw_range_elements = false;

  if (is_at_least_gl_version(1, 2)) {
    _supports_draw_range_elements = true;
    _glDrawRangeElements = (PFNGLDRAWRANGEELEMENTSPROC)
      get_extension_func("glDrawRangeElements");

  } else if (has_extension("GL_EXT_draw_range_elements")) {
    _supports_draw_range_elements = true;
    _glDrawRangeElements = (PFNGLDRAWRANGEELEMENTSPROC)
      get_extension_func("glDrawRangeElementsEXT");
  }
  if (_supports_draw_range_elements) {
    if (_glDrawRangeElements == NULL) {
      GLCAT.warning()
        << "glDrawRangeElements advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_draw_range_elements = false;
    }
  }
  if (!_supports_draw_range_elements) {
    _glDrawRangeElements = null_glDrawRangeElements;
  }

#ifdef OPENGLES
  _supports_depth_texture = has_extension("GL_OES_depth_texture");
  _supports_depth24 = has_extension("GL_OES_depth24");
  _supports_depth32 = has_extension("GL_OES_depth32");
#else
  _supports_depth_texture =
    has_extension("GL_ARB_depth_texture") || is_at_least_gl_version(1, 4);
#endif

  _supports_depth_stencil = false;
  if (_supports_depth_texture) {
    _supports_depth_stencil =
      has_extension("GL_EXT_packed_depth_stencil") || has_extension("GL_OES_packed_depth_stencil");
  }

  _supports_3d_texture = false;

  if (is_at_least_gl_version(1, 2)) {
    _supports_3d_texture = true;

    _glTexImage3D = (PFNGLTEXIMAGE3DPROC_P)
      get_extension_func("glTexImage3D");
    _glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)
      get_extension_func("glTexSubImage3D");
    _glCopyTexSubImage3D = (PFNGLCOPYTEXSUBIMAGE3DPROC)
      get_extension_func("glCopyTexSubImage3D");

  } else if (has_extension("GL_EXT_texture3D")) {
    _supports_3d_texture = true;

    _glTexImage3D = (PFNGLTEXIMAGE3DPROC_P)
      get_extension_func("glTexImage3DEXT");
    _glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)
      get_extension_func("glTexSubImage3DEXT");

    _glCopyTexSubImage3D = NULL;
    if (has_extension("GL_EXT_copy_texture")) {
      _glCopyTexSubImage3D = (PFNGLCOPYTEXSUBIMAGE3DPROC)
        get_extension_func("glCopyTexSubImage3DEXT");
    }

  } else if (has_extension("GL_OES_texture_3D")) {
    _supports_3d_texture = true;

    _glTexImage3D = (PFNGLTEXIMAGE3DPROC_P)
      get_extension_func("glTexImage3DOES");
    _glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)
      get_extension_func("glTexSubImage3DOES");
    _glCopyTexSubImage3D = (PFNGLCOPYTEXSUBIMAGE3DPROC)
      get_extension_func("glCopyTexSubImage3DOES");

#ifdef OPENGLES_2
    _glFramebufferTexture3D = (PFNGLFRAMEBUFFERTEXTURE3DOES)
      get_extension_func("glFramebufferTexture3DOES");
#endif
  }

  if (_supports_3d_texture) {
    if (_glTexImage3D == NULL || _glTexSubImage3D == NULL) {
      GLCAT.warning()
        << "3-D textures advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_3d_texture = false;
    }
  }

  _supports_tex_storage = false;

  if (is_at_least_gl_version(4, 2) || has_extension("GL_ARB_texture_storage")) {
    _supports_tex_storage = true;

    _glTexStorage1D = (PFNGLTEXSTORAGE1DPROC)
      get_extension_func("glTexStorage1D");
    _glTexStorage2D = (PFNGLTEXSTORAGE2DPROC)
      get_extension_func("glTexStorage2D");
    _glTexStorage3D = (PFNGLTEXSTORAGE3DPROC)
      get_extension_func("glTexStorage3D");

  } else if (has_extension("GL_EXT_texture_storage")) { // GLES case
    _supports_tex_storage = true;

    _glTexStorage1D = (PFNGLTEXSTORAGE1DPROC)
      get_extension_func("glTexStorage1DEXT");
    _glTexStorage2D = (PFNGLTEXSTORAGE2DPROC)
      get_extension_func("glTexStorage2DEXT");
    _glTexStorage3D = (PFNGLTEXSTORAGE3DPROC)
      get_extension_func("glTexStorage3DEXT");
  }

  if (_supports_tex_storage) {
    if (_glTexStorage1D == NULL || _glTexStorage2D == NULL || _glTexStorage3D == NULL) {
      GLCAT.warning()
        << "Immutable texture storage advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_tex_storage = false;
    }
  }

  _supports_2d_texture_array = false;
#ifndef OPENGLES
  _supports_2d_texture_array = has_extension("GL_EXT_texture_array");
  if (_supports_2d_texture_array) {
    _glFramebufferTextureLayer = (PFNGLFRAMEBUFFERTEXTURELAYERPROC)
      get_extension_func("glFramebufferTextureLayerEXT");
  } else {
    // ARB_geometry_shader4 also provides a version.
    _glFramebufferTextureLayer = NULL;
  }
#endif

#ifdef OPENGLES_2
  _supports_cube_map = true;
#else
  _supports_cube_map =
    has_extension("GL_ARB_texture_cube_map") || is_at_least_gl_version(1, 3) ||
    has_extension("GL_OES_texture_cube_map");
#endif

#ifndef OPENGLES
  if (_supports_cube_map && gl_cube_map_seamless) {
    if (is_at_least_gl_version(3, 2) || has_extension("GL_ARB_seamless_cube_map")) {
      glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }
  }
#endif

  _supports_texture_srgb = false;
  if (is_at_least_gl_version(2, 1) || has_extension("GL_EXT_texture_sRGB")) {
    _supports_texture_srgb = true;

  } else if (has_extension("GL_EXT_sRGB")) { // GLES case.
    _supports_texture_srgb = true;
  }

  _supports_compressed_texture = false;

#ifdef OPENGLES
  _supports_compressed_texture = true;

  // Supported in the core.  1D textures are not supported by OpenGL ES.
  _glCompressedTexImage1D = NULL;
  _glCompressedTexImage2D = glCompressedTexImage2D;
  _glCompressedTexSubImage1D = NULL;
  _glCompressedTexSubImage2D = glCompressedTexSubImage2D;
  _glGetCompressedTexImage = NULL;

  _glCompressedTexImage3D = NULL;
  _glCompressedTexSubImage3D = NULL;
#ifdef OPENGLES_2
  if (_supports_3d_texture) {
    _glCompressedTexImage3D = (PFNGLCOMPRESSEDTEXIMAGE3DPROC)
      get_extension_func("glCompressedTexImage3DOES");
    _glCompressedTexSubImage3D = (PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)
      get_extension_func("glCompressedTexSubImageOES");
  }
#endif

#else
  if (is_at_least_gl_version(1, 3)) {
    _supports_compressed_texture = true;

    _glCompressedTexImage1D = (PFNGLCOMPRESSEDTEXIMAGE1DPROC)
      get_extension_func("glCompressedTexImage1D");
    _glCompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)
      get_extension_func("glCompressedTexImage2D");
    _glCompressedTexImage3D = (PFNGLCOMPRESSEDTEXIMAGE3DPROC)
      get_extension_func("glCompressedTexImage3D");
    _glCompressedTexSubImage1D = (PFNGLCOMPRESSEDTEXSUBIMAGE1DPROC)
      get_extension_func("glCompressedTexSubImage1D");
    _glCompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)
      get_extension_func("glCompressedTexSubImage2D");
    _glCompressedTexSubImage3D = (PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)
      get_extension_func("glCompressedTexSubImage3D");
    _glGetCompressedTexImage = (PFNGLGETCOMPRESSEDTEXIMAGEPROC)
      get_extension_func("glGetCompressedTexImage");

  } else if (has_extension("GL_ARB_texture_compression")) {
    _supports_compressed_texture = true;

    _glCompressedTexImage1D = (PFNGLCOMPRESSEDTEXIMAGE1DPROC)
      get_extension_func("glCompressedTexImage1DARB");
    _glCompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)
      get_extension_func("glCompressedTexImage2DARB");
    _glCompressedTexImage3D = (PFNGLCOMPRESSEDTEXIMAGE3DPROC)
      get_extension_func("glCompressedTexImage3DARB");
    _glCompressedTexSubImage1D = (PFNGLCOMPRESSEDTEXSUBIMAGE1DPROC)
      get_extension_func("glCompressedTexSubImage1DARB");
    _glCompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)
      get_extension_func("glCompressedTexSubImage2DARB");
    _glCompressedTexSubImage3D = (PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)
      get_extension_func("glCompressedTexSubImage3DARB");
    _glGetCompressedTexImage = (PFNGLGETCOMPRESSEDTEXIMAGEPROC)
      get_extension_func("glGetCompressedTexImageARB");
  }

  if (_supports_compressed_texture) {
    if (_glCompressedTexImage1D == NULL ||
        _glCompressedTexImage2D == NULL ||
        _glCompressedTexImage3D == NULL ||
        _glCompressedTexSubImage1D == NULL ||
        _glCompressedTexSubImage2D == NULL ||
        _glCompressedTexSubImage3D == NULL ||
        _glGetCompressedTexImage == NULL) {
      GLCAT.warning()
        << "Compressed textures advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_compressed_texture = false;
    }
  }
#endif

  if (_supports_compressed_texture) {
#ifndef OPENGLES
    _compressed_texture_formats.set_bit(Texture::CM_on);
#endif

    GLint num_compressed_formats = 0;
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &num_compressed_formats);
    GLint *formats = (GLint *)PANDA_MALLOC_ARRAY(num_compressed_formats * sizeof(GLint));
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats);
    for (int i = 0; i < num_compressed_formats; ++i) {
      switch (formats[i]) {
      case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
      case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        _compressed_texture_formats.set_bit(Texture::CM_dxt1);
        break;
#ifdef OPENGLES
      case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
      case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
        _compressed_texture_formats.set_bit(Texture::CM_pvr1_2bpp);
        break;
      case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
      case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
        _compressed_texture_formats.set_bit(Texture::CM_pvr1_4bpp);
        break;
#else
      case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        _compressed_texture_formats.set_bit(Texture::CM_dxt3);
        break;

      case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        _compressed_texture_formats.set_bit(Texture::CM_dxt5);
        break;

      case GL_COMPRESSED_RGB_FXT1_3DFX:
      case GL_COMPRESSED_RGBA_FXT1_3DFX:
        _compressed_texture_formats.set_bit(Texture::CM_fxt1);
        break;
#endif

      default:
        break;
      }
    }
    PANDA_FREE_ARRAY(formats);
  }

#ifdef OPENGLES_2
  _supports_bgr = false;
#else
  _supports_bgr =
    has_extension("GL_EXT_bgra") || is_at_least_gl_version(1, 2);
#endif

#ifdef OPENGLES_2
  _supports_rescale_normal = true;
#else
  _supports_rescale_normal =
    gl_support_rescale_normal &&
    (has_extension("GL_EXT_rescale_normal") || is_at_least_gl_version(1, 2));
#endif

  _supports_packed_dabc = /*gl_support_packed_dabc &&*/
    has_extension("GL_ARB_vertex_array_bgra") || has_extension("GL_EXT_vertex_array_bgra");

  _supports_multisample =
    has_extension("GL_ARB_multisample") || is_at_least_gl_version(1, 3);

#ifdef OPENGLES_2
  _supports_generate_mipmap = true;
#else
  _supports_generate_mipmap =
    has_extension("GL_SGIS_generate_mipmap") || is_at_least_gl_version(1, 4) || is_at_least_gles_version(1, 1);
#endif

  _supports_multitexture = false;

#ifdef OPENGLES
  _supports_tex_non_pow2 =
    has_extension("GL_OES_texture_npot");
#else
  _supports_tex_non_pow2 =
    has_extension("GL_ARB_texture_non_power_of_two");
#endif

#ifdef OPENGLES_2 // OpenGL ES 2.0 doesn't support multitexturing.
  _supports_multitexture = false;
#else
  if (is_at_least_gl_version(1, 3) || is_at_least_gles_version(1, 1)) {
    _supports_multitexture = true;

    _glActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func("glActiveTexture");
    _glClientActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func("glClientActiveTexture");
    _glMultiTexCoord1f = (PFNGLMULTITEXCOORD1FPROC)
      get_extension_func("glMultiTexCoord1f");
    _glMultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)
      get_extension_func("glMultiTexCoord2f");
    _glMultiTexCoord3f = (PFNGLMULTITEXCOORD3FPROC)
      get_extension_func("glMultiTexCoord3f");
    _glMultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)
      get_extension_func("glMultiTexCoord4f");
    _glMultiTexCoord1d = (PFNGLMULTITEXCOORD1DPROC)
      get_extension_func("glMultiTexCoord1d");
    _glMultiTexCoord2d = (PFNGLMULTITEXCOORD2DPROC)
      get_extension_func("glMultiTexCoord2d");
    _glMultiTexCoord3d = (PFNGLMULTITEXCOORD3DPROC)
      get_extension_func("glMultiTexCoord3d");
    _glMultiTexCoord4d = (PFNGLMULTITEXCOORD4DPROC)
      get_extension_func("glMultiTexCoord4d");

  } else if (has_extension("GL_ARB_multitexture")) {
    _supports_multitexture = true;

    _glActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func("glActiveTextureARB");
    _glClientActiveTexture = (PFNGLACTIVETEXTUREPROC)
      get_extension_func("glClientActiveTextureARB");
    _glMultiTexCoord1f = (PFNGLMULTITEXCOORD1FPROC)
      get_extension_func("glMultiTexCoord1fARB");
    _glMultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)
      get_extension_func("glMultiTexCoord2fARB");
    _glMultiTexCoord3f = (PFNGLMULTITEXCOORD3FPROC)
      get_extension_func("glMultiTexCoord3fARB");
    _glMultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)
      get_extension_func("glMultiTexCoord4fARB");
    _glMultiTexCoord1d = (PFNGLMULTITEXCOORD1DPROC)
      get_extension_func("glMultiTexCoord1dARB");
    _glMultiTexCoord2d = (PFNGLMULTITEXCOORD2DPROC)
      get_extension_func("glMultiTexCoord2dARB");
    _glMultiTexCoord3d = (PFNGLMULTITEXCOORD3DPROC)
      get_extension_func("glMultiTexCoord3dARB");
    _glMultiTexCoord4d = (PFNGLMULTITEXCOORD4DPROC)
      get_extension_func("glMultiTexCoord4dARB");
  }

  if (_supports_multitexture) {
    if (_glActiveTexture == NULL || _glClientActiveTexture == NULL
#ifdef SUPPORT_IMMEDIATE_MODE
        || GLf(_glMultiTexCoord1) == NULL || GLf(_glMultiTexCoord2) == NULL
        || GLf(_glMultiTexCoord3) == NULL || GLf(_glMultiTexCoord4) == NULL
#endif
        ) {
      GLCAT.warning()
        << "Multitexture advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_multitexture = false;
    }
  }
#endif

  if (!_supports_multitexture) {
    _glActiveTexture = null_glActiveTexture;
    _glClientActiveTexture = null_glActiveTexture;
  }

  if (has_extension("GL_ARB_depth_texture")) {
    _supports_depth_texture = true;
  }

  if (_supports_depth_texture &&
      has_extension("GL_ARB_shadow") &&
      has_extension("GL_ARB_fragment_program_shadow")) {
    _supports_shadow_filter = true;
  }
  if (_gl_vendor.substr(0,3)=="ATI") {
    // ATI drivers have never provided correct shadow support.
    _supports_shadow_filter = false;
  }

  _supports_texture_combine =
    has_extension("GL_ARB_texture_env_combine") || is_at_least_gl_version(1, 3) || is_at_least_gles_version(1, 1);
  _supports_texture_saved_result =
    has_extension("GL_ARB_texture_env_crossbar") || has_extension("GL_OES_texture_env_crossbar") || is_at_least_gl_version(1, 4);
  _supports_texture_dot3 =
    has_extension("GL_ARB_texture_env_dot3") || is_at_least_gl_version(1, 3) || is_at_least_gles_version(1, 1);

  _supports_buffers = false;

  if (is_at_least_gl_version(1, 5) || is_at_least_gles_version(1, 1)) {
    _supports_buffers = true;

    _glGenBuffers = (PFNGLGENBUFFERSPROC)
      get_extension_func("glGenBuffers");
    _glBindBuffer = (PFNGLBINDBUFFERPROC)
      get_extension_func("glBindBuffer");
    _glBufferData = (PFNGLBUFFERDATAPROC)
      get_extension_func("glBufferData");
    _glBufferSubData = (PFNGLBUFFERSUBDATAPROC)
      get_extension_func("glBufferSubData");
    _glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)
      get_extension_func("glDeleteBuffers");

  } else if (has_extension("GL_ARB_vertex_buffer_object")) {
    _supports_buffers = true;

    _glGenBuffers = (PFNGLGENBUFFERSPROC)
      get_extension_func("glGenBuffersARB");
    _glBindBuffer = (PFNGLBINDBUFFERPROC)
      get_extension_func("glBindBufferARB");
    _glBufferData = (PFNGLBUFFERDATAPROC)
      get_extension_func("glBufferDataARB");
    _glBufferSubData = (PFNGLBUFFERSUBDATAPROC)
      get_extension_func("glBufferSubDataARB");
    _glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)
      get_extension_func("glDeleteBuffersARB");
  }

  if (_supports_buffers) {
    if (_glGenBuffers == NULL || _glBindBuffer == NULL ||
        _glBufferData == NULL || _glBufferSubData == NULL ||
        _glDeleteBuffers == NULL) {
      GLCAT.warning()
        << "Buffers advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_buffers = false;
    }
  }

#if defined(HAVE_CG) && !defined(OPENGLES)
  if (cgGLIsProfileSupported(CG_PROFILE_ARBFP1) &&
      cgGLIsProfileSupported(CG_PROFILE_ARBVP1)) {
    _supports_basic_shaders = true;
    if (basic_shaders_only) {
      _shader_caps._active_vprofile = (int)CG_PROFILE_ARBVP1;
      _shader_caps._active_fprofile = (int)CG_PROFILE_ARBFP1;
      _shader_caps._active_gprofile = (int)CG_PROFILE_UNKNOWN; // No geometry shader if only using basic
    } else {
      _shader_caps._active_vprofile = (int)cgGLGetLatestProfile(CG_GL_VERTEX);
      _shader_caps._active_fprofile = (int)cgGLGetLatestProfile(CG_GL_FRAGMENT);
      _shader_caps._active_gprofile = (int)cgGLGetLatestProfile(CG_GL_GEOMETRY);

      // cgGLGetLatestProfile doesn't seem to return anything other
      // arbvp1/arbfp1 on non-NVIDIA cards, which is severely limiting.
      // Actually, it seems that these profiles are horribly broken on these
      // cards.  Let's not do this.
      //if ((_shader_caps._active_vprofile == CG_PROFILE_ARBVP1 ||
      //     _shader_caps._active_fprofile == CG_PROFILE_ARBFP1) &&
      //    cgGLIsProfileSupported(CG_PROFILE_GLSLV) &&
      //    cgGLIsProfileSupported(CG_PROFILE_GLSLF)) {

      //  // So, if this happens, we set it to GLSL, which is
      //  // usually supported on all cards.
      //  _shader_caps._active_vprofile = (int)CG_PROFILE_GLSLV;
      //  _shader_caps._active_fprofile = (int)CG_PROFILE_GLSLF;
#if CG_VERSION_NUM >= 2200
      //  if (cgGLIsProfileSupported(CG_PROFILE_GLSLG)) {
      //    _shader_caps._active_gprofile = (int)CG_PROFILE_GLSLG;
      //  }
#endif
      //}
    }
    _shader_caps._ultimate_vprofile = (int)CG_PROFILE_VP40;
    _shader_caps._ultimate_fprofile = (int)CG_PROFILE_FP40;
    _shader_caps._ultimate_gprofile = (int)CG_PROFILE_GPU_GP;

    _glBindProgram = (PFNGLBINDPROGRAMARBPROC)
      get_extension_func("glBindProgramARB");

    // Bug workaround for radeons.
    if (_shader_caps._active_fprofile == CG_PROFILE_ARBFP1) {
      if (has_extension("GL_ATI_draw_buffers")) {
        _shader_caps._bug_list.insert(Shader::SBUG_ati_draw_buffers);
      }
    }
  }
#endif // HAVE_CG


#ifdef OPENGLES_2
  _supports_glsl = true;
  _supports_geometry_shaders = false;
  _supports_tessellation_shaders = false;
#else
  #ifdef OPENGLES_1
    _supports_glsl = false;
    _supports_geometry_shaders = false;
    _supports_tessellation_shaders = false;
  #else
    _supports_glsl = is_at_least_gl_version(2, 0) || has_extension("GL_ARB_shading_language_100");
    _supports_geometry_shaders = is_at_least_gl_version(3, 2) || has_extension("GL_ARB_geometry_shader4");
    _supports_tessellation_shaders = is_at_least_gl_version(4, 0) || has_extension("GL_ARB_tessellation_shader");
  #endif
#endif
  _shader_caps._supports_glsl = _supports_glsl;

  _supports_compute_shaders = false;
#ifndef OPENGLES
  if (is_at_least_gl_version(4, 3) || has_extension("GL_ARB_compute_shader")) {
    _glDispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)
      get_extension_func("glDispatchCompute");

    if (_glDispatchCompute != NULL) {
      _supports_compute_shaders = true;
    }
  }
#endif

#ifndef OPENGLES
  if (_supports_glsl) {
    _glAttachShader = (PFNGLATTACHSHADERPROC)
       get_extension_func("glAttachShader");
    _glBindAttribLocation = (PFNGLBINDATTRIBLOCATIONPROC)
       get_extension_func("glBindAttribLocation");
    _glCompileShader = (PFNGLCOMPILESHADERPROC)
       get_extension_func("glCompileShader");
    _glCreateProgram = (PFNGLCREATEPROGRAMPROC)
       get_extension_func("glCreateProgram");
    _glCreateShader = (PFNGLCREATESHADERPROC)
       get_extension_func("glCreateShader");
    _glDeleteProgram = (PFNGLDELETEPROGRAMPROC)
       get_extension_func("glDeleteProgram");
    _glDeleteShader = (PFNGLDELETESHADERPROC)
       get_extension_func("glDeleteShader");
    _glDetachShader = (PFNGLDETACHSHADERPROC)
       get_extension_func("glDetachShader");
    _glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)
      get_extension_func("glDisableVertexAttribArray");
    _glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)
      get_extension_func("glEnableVertexAttribArray");
    _glGetActiveAttrib = (PFNGLGETACTIVEATTRIBPROC)
       get_extension_func("glGetActiveAttrib");
    _glGetActiveUniform = (PFNGLGETACTIVEUNIFORMPROC)
       get_extension_func("glGetActiveUniform");
    _glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)
      get_extension_func("glGetAttribLocation");
    _glGetProgramiv = (PFNGLGETPROGRAMIVPROC)
       get_extension_func("glGetProgramiv");
    _glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)
       get_extension_func("glGetProgramInfoLog");
    _glGetShaderiv = (PFNGLGETSHADERIVPROC)
       get_extension_func("glGetShaderiv");
    _glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)
       get_extension_func("glGetShaderInfoLog");
    _glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)
       get_extension_func("glGetUniformLocation");
    _glLinkProgram = (PFNGLLINKPROGRAMPROC)
       get_extension_func("glLinkProgram");
    _glShaderSource = (PFNGLSHADERSOURCEPROC_P)
       get_extension_func("glShaderSource");
    _glUseProgram = (PFNGLUSEPROGRAMPROC)
       get_extension_func("glUseProgram");
    _glUniform4f = (PFNGLUNIFORM4FPROC)
       get_extension_func("glUniform4f");
    _glUniform1i = (PFNGLUNIFORM1IPROC)
       get_extension_func("glUniform1i");
    _glUniform1fv = (PFNGLUNIFORM1FVPROC)
       get_extension_func("glUniform1fv");
    _glUniform2fv = (PFNGLUNIFORM2FVPROC)
       get_extension_func("glUniform2fv");
    _glUniform3fv = (PFNGLUNIFORM3FVPROC)
       get_extension_func("glUniform3fv");
    _glUniform4fv = (PFNGLUNIFORM4FVPROC)
       get_extension_func("glUniform4fv");
    _glUniform1iv = (PFNGLUNIFORM1IVPROC)
       get_extension_func("glUniform1iv");
    _glUniform2iv = (PFNGLUNIFORM2IVPROC)
       get_extension_func("glUniform2iv");
    _glUniform3iv = (PFNGLUNIFORM3IVPROC)
       get_extension_func("glUniform3iv");
    _glUniform4iv = (PFNGLUNIFORM4IVPROC)
       get_extension_func("glUniform4iv");
    _glUniformMatrix3fv = (PFNGLUNIFORMMATRIX3FVPROC)
       get_extension_func("glUniformMatrix3fv");
    _glUniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)
       get_extension_func("glUniformMatrix4fv");
    _glValidateProgram = (PFNGLVALIDATEPROGRAMPROC)
       get_extension_func("glValidateProgram");
    _glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)
       get_extension_func("glVertexAttribPointer");
    _glVertexAttribIPointer = (PFNGLVERTEXATTRIBIPOINTERPROC)
       get_extension_func("glVertexAttribIPointer");
    _glVertexAttribLPointer = (PFNGLVERTEXATTRIBLPOINTERPROC)
       get_extension_func("glVertexAttribLPointer");

    if (_supports_geometry_shaders) {
      _glProgramParameteri = (PFNGLPROGRAMPARAMETERIPROC)
        get_extension_func("glProgramParameteri");
      _glFramebufferTexture = (PFNGLFRAMEBUFFERTEXTUREARBPROC)
        get_extension_func("glFramebufferTextureARB");

      if (_glFramebufferTextureLayer == NULL) {
        _glFramebufferTextureLayer = (PFNGLFRAMEBUFFERTEXTURELAYERPROC)
          get_extension_func("glFramebufferTextureLayerARB");
      }
    }
    if (_supports_tessellation_shaders) {
      _glPatchParameteri = (PFNGLPATCHPARAMETERIPROC)
         get_extension_func("glPatchParameteri");
    }
  }
#endif

#ifdef OPENGLES_2
  _glAttachShader = glAttachShader;
  _glBindAttribLocation = glBindAttribLocation;
  _glCompileShader = glCompileShader;
  _glCreateProgram = glCreateProgram;
  _glCreateShader = glCreateShader;
  _glDeleteProgram = glDeleteProgram;
  _glDeleteShader = glDeleteShader;
  _glDetachShader = glDetachShader;
  _glDisableVertexAttribArray = glDisableVertexAttribArray;
  _glEnableVertexAttribArray = glEnableVertexAttribArray;
  _glGetActiveAttrib = glGetActiveAttrib;
  _glGetActiveUniform = glGetActiveUniform;
  _glGetAttribLocation = glGetAttribLocation;
  _glGetProgramiv = glGetProgramiv;
  _glGetProgramInfoLog = glGetProgramInfoLog;
  _glGetShaderiv = glGetShaderiv;
  _glGetShaderInfoLog = glGetShaderInfoLog;
  _glGetUniformLocation = glGetUniformLocation;
  _glLinkProgram = glLinkProgram;
  _glShaderSource = (PFNGLSHADERSOURCEPROC_P) glShaderSource;
  _glUseProgram = glUseProgram;
  _glUniform4f = glUniform4f;
  _glUniform1i = glUniform1i;
  _glUniform1fv = glUniform1fv;
  _glUniform2fv = glUniform2fv;
  _glUniform3fv = glUniform3fv;
  _glUniform4fv = glUniform4fv;
  _glUniformMatrix3fv = glUniformMatrix3fv;
  _glUniformMatrix4fv = glUniformMatrix4fv;
  _glValidateProgram = glValidateProgram;
  _glVertexAttribPointer = glVertexAttribPointer;
  _glVertexAttribIPointer = NULL;
  _glVertexAttribLPointer = NULL;

  // We need to have a default shader to apply in case
  // something didn't happen to have a shader applied, or
  // if it failed to compile. This default shader just outputs
  // a red color, indicating that something went wrong.
  if (_default_shader == NULL) {
    _default_shader = new Shader(default_shader_name, default_shader_body, Shader::SL_GLSL);
  }
#endif

#ifdef OPENGLES_2
  // In OpenGL ES 2.x, FBO's are supported in the core.
  _supports_framebuffer_object = true;
  _glIsRenderbuffer = glIsRenderbuffer;
  _glBindRenderbuffer = glBindRenderbuffer;
  _glDeleteRenderbuffers = glDeleteRenderbuffers;
  _glGenRenderbuffers = glGenRenderbuffers;
  _glRenderbufferStorage = glRenderbufferStorage;
  _glGetRenderbufferParameteriv = glGetRenderbufferParameteriv;
  _glIsFramebuffer = glIsFramebuffer;
  _glBindFramebuffer = glBindFramebuffer;
  _glDeleteFramebuffers = glDeleteFramebuffers;
  _glGenFramebuffers = glGenFramebuffers;
  _glCheckFramebufferStatus = glCheckFramebufferStatus;
  _glFramebufferTexture1D = NULL;
  _glFramebufferTexture2D = glFramebufferTexture2D;
  _glFramebufferRenderbuffer = glFramebufferRenderbuffer;
  _glGetFramebufferAttachmentParameteriv = glGetFramebufferAttachmentParameteriv;
  _glGenerateMipmap = glGenerateMipmap;
#else
  //TODO: add ARB/3.0 version

  _supports_framebuffer_object = false;
  if (has_extension("GL_EXT_framebuffer_object")) {
    _supports_framebuffer_object = true;
    _glIsRenderbuffer = (PFNGLISRENDERBUFFEREXTPROC)
      get_extension_func("glIsRenderbufferEXT");
    _glBindRenderbuffer = (PFNGLBINDRENDERBUFFEREXTPROC)
      get_extension_func("glBindRenderbufferEXT");
    _glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSEXTPROC)
      get_extension_func("glDeleteRenderbuffersEXT");
    _glGenRenderbuffers = (PFNGLGENRENDERBUFFERSEXTPROC)
      get_extension_func("glGenRenderbuffersEXT");
    _glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEEXTPROC)
      get_extension_func("glRenderbufferStorageEXT");
    _glGetRenderbufferParameteriv = (PFNGLGETRENDERBUFFERPARAMETERIVEXTPROC)
      get_extension_func("glGetRenderbufferParameterivEXT");
    _glIsFramebuffer = (PFNGLISFRAMEBUFFEREXTPROC)
      get_extension_func("glIsFramebufferEXT");
    _glBindFramebuffer = (PFNGLBINDFRAMEBUFFEREXTPROC)
      get_extension_func("glBindFramebufferEXT");
    _glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSEXTPROC)
      get_extension_func("glDeleteFramebuffersEXT");
    _glGenFramebuffers = (PFNGLGENFRAMEBUFFERSEXTPROC)
      get_extension_func("glGenFramebuffersEXT");
    _glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)
      get_extension_func("glCheckFramebufferStatusEXT");
    _glFramebufferTexture1D = (PFNGLFRAMEBUFFERTEXTURE1DEXTPROC)
      get_extension_func("glFramebufferTexture1DEXT");
    _glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)
      get_extension_func("glFramebufferTexture2DEXT");
    _glFramebufferTexture3D = (PFNGLFRAMEBUFFERTEXTURE3DEXTPROC)
      get_extension_func("glFramebufferTexture3DEXT");
    _glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC)
      get_extension_func("glFramebufferRenderbufferEXT");
    _glGetFramebufferAttachmentParameteriv = (PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVEXTPROC)
      get_extension_func("glGetFramebufferAttachmentParameterivEXT");
    _glGenerateMipmap = (PFNGLGENERATEMIPMAPEXTPROC)
      get_extension_func("glGenerateMipmapEXT");
  }
#ifdef OPENGLES
  else if (has_extension("GL_OES_framebuffer_object")) {
    _supports_framebuffer_object = true;
    _glIsRenderbuffer = (PFNGLISRENDERBUFFEROESPROC)
      get_extension_func("glIsRenderbufferOES");
    _glBindRenderbuffer = (PFNGLBINDRENDERBUFFEROESPROC)
      get_extension_func("glBindRenderbufferOES");
    _glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSOESPROC)
      get_extension_func("glDeleteRenderbuffersOES");
    _glGenRenderbuffers = (PFNGLGENRENDERBUFFERSOESPROC)
      get_extension_func("glGenRenderbuffersOES");
    _glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEOESPROC)
      get_extension_func("glRenderbufferStorageOES");
    _glGetRenderbufferParameteriv = (PFNGLGETRENDERBUFFERPARAMETERIVOESPROC)
      get_extension_func("glGetRenderbufferParameterivOES");
    _glIsFramebuffer = (PFNGLISFRAMEBUFFEROESPROC)
      get_extension_func("glIsFramebufferOES");
    _glBindFramebuffer = (PFNGLBINDFRAMEBUFFEROESPROC)
      get_extension_func("glBindFramebufferOES");
    _glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSOESPROC)
      get_extension_func("glDeleteFramebuffersOES");
    _glGenFramebuffers = (PFNGLGENFRAMEBUFFERSOESPROC)
      get_extension_func("glGenFramebuffersOES");
    _glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSOESPROC)
      get_extension_func("glCheckFramebufferStatusOES");
    _glFramebufferTexture1D = NULL;
    _glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DOESPROC)
      get_extension_func("glFramebufferTexture2DOES");
    _glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFEROESPROC)
      get_extension_func("glFramebufferRenderbufferOES");
    _glGetFramebufferAttachmentParameteriv = (PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVOESPROC)
      get_extension_func("glGetFramebufferAttachmentParameterivOES");
    _glGenerateMipmap = (PFNGLGENERATEMIPMAPOESPROC)
      get_extension_func("glGenerateMipmapOES");
  }
#endif  // OPENGLES
#endif

  _supports_framebuffer_multisample = false;
  if ( has_extension("GL_EXT_framebuffer_multisample") ) {
    _supports_framebuffer_multisample = true;
    _glRenderbufferStorageMultisample = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)
      get_extension_func("glRenderbufferStorageMultisampleEXT");
  }

  _supports_framebuffer_multisample_coverage_nv = false;
  if ( has_extension("GL_NV_framebuffer_multisample_coverage") ) {
    _supports_framebuffer_multisample_coverage_nv = true;
    _glRenderbufferStorageMultisampleCoverage = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLECOVERAGENVPROC)
      get_extension_func("glRenderbufferStorageMultisampleCoverageNV");
  }

  _supports_framebuffer_blit = false;
  if ( has_extension("GL_EXT_framebuffer_blit") ) {
    _supports_framebuffer_blit = true;
    _glBlitFramebuffer = (PFNGLBLITFRAMEBUFFEREXTPROC)
      get_extension_func("glBlitFramebufferEXT");
  }

  _glDrawBuffers = NULL;
  _glClearBufferfv = NULL;
#ifndef OPENGLES
  if (is_at_least_gl_version(2, 0)) {
    _glDrawBuffers = (PFNGLDRAWBUFFERSPROC)
      get_extension_func("glDrawBuffers");
  } else if (has_extension("GL_ARB_draw_buffers")) {
    _glDrawBuffers = (PFNGLDRAWBUFFERSPROC)
      get_extension_func("glDrawBuffersARB");
  }

  _max_color_targets = 1;
  if (_glDrawBuffers != 0) {
    GLint max_draw_buffers = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
    _max_color_targets = max_draw_buffers;
  }

  if (is_at_least_gl_version(3, 0)) {
    _glClearBufferfv = (PFNGLCLEARBUFFERFVPROC)
      get_extension_func("glClearBufferfv");
  }
#endif  // OPENGLES

#ifndef OPENGLES
  _supports_viewport_arrays = false;

  if (is_at_least_gl_version(4, 1) || has_extension("GL_ARB_viewport_array")) {
    _glViewportArrayv = (PFNGLVIEWPORTARRAYVPROC)
      get_extension_func("glViewportArrayv");
    _glScissorArrayv = (PFNGLSCISSORARRAYVPROC)
      get_extension_func("glScissorArrayv");
    _glDepthRangeArrayv = (PFNGLDEPTHRANGEARRAYVPROC)
      get_extension_func("glDepthRangeArrayv");

    if (_glViewportArrayv == NULL || _glScissorArrayv == NULL || _glDepthRangeArrayv == NULL) {
      GLCAT.warning()
          << "Viewport arrays advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
    } else {
      _supports_viewport_arrays = true;
    }
  }
#endif  // OPENGLES

  _max_fb_samples = 0;
#ifndef OPENGLES
  if (_supports_framebuffer_multisample) {
    GLint max_samples;
    glGetIntegerv(GL_MAX_SAMPLES_EXT, &max_samples);
    _max_fb_samples = max_samples;
  }
#endif

  _supports_occlusion_query = false;
  if (gl_support_occlusion_query) {
    if (is_at_least_gl_version(1, 5)) {
      _supports_occlusion_query = true;

      _glGenQueries = (PFNGLGENQUERIESPROC)
        get_extension_func("glGenQueries");
      _glBeginQuery = (PFNGLBEGINQUERYPROC)
        get_extension_func("glBeginQuery");
      _glEndQuery = (PFNGLENDQUERYPROC)
        get_extension_func("glEndQuery");
      _glDeleteQueries = (PFNGLDELETEQUERIESPROC)
        get_extension_func("glDeleteQueries");
      _glGetQueryiv = (PFNGLGETQUERYIVPROC)
        get_extension_func("glGetQueryiv");
      _glGetQueryObjectuiv = (PFNGLGETQUERYOBJECTUIVPROC)
        get_extension_func("glGetQueryObjectuiv");

    } else if (has_extension("GL_ARB_occlusion_query")) {
      _supports_occlusion_query = true;
      _glGenQueries = (PFNGLGENQUERIESPROC)
        get_extension_func("glGenQueriesARB");
      _glBeginQuery = (PFNGLBEGINQUERYPROC)
        get_extension_func("glBeginQueryARB");
      _glEndQuery = (PFNGLENDQUERYPROC)
        get_extension_func("glEndQueryARB");
      _glDeleteQueries = (PFNGLDELETEQUERIESPROC)
        get_extension_func("glDeleteQueriesARB");
      _glGetQueryiv = (PFNGLGETQUERYIVPROC)
        get_extension_func("glGetQueryivARB");
      _glGetQueryObjectuiv = (PFNGLGETQUERYOBJECTUIVPROC)
        get_extension_func("glGetQueryObjectuivARB");
    }
  }

#ifndef OPENGLES
  if (_supports_occlusion_query) {
    if (_glGenQueries == NULL || _glBeginQuery == NULL ||
        _glEndQuery == NULL || _glDeleteQueries == NULL ||
        _glGetQueryiv == NULL || _glGetQueryObjectuiv == NULL) {
      GLCAT.warning()
        << "Occlusion queries advertised as supported by OpenGL runtime, but could not get pointers to extension functions.\n";
      _supports_occlusion_query = false;
    } else {
      GLint num_bits;
      _glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS, &num_bits);
      if (num_bits == 0) {
        _supports_occlusion_query = false;
      }
      if (GLCAT.is_debug()) {
        GLCAT.debug()
          << "Occlusion query counter provides " << num_bits << " bits.\n";
      }
    }
  }
#endif

  _supports_timer_query = false;
#if defined(DO_PSTATS) && !defined(OPENGLES)
  if (is_at_least_gl_version(3, 3) || has_extension("GL_ARB_timer_query")) {
    _supports_timer_query = true;

    _glQueryCounter = (PFNGLQUERYCOUNTERPROC)
      get_extension_func("glQueryCounter");
    _glGetQueryObjecti64v = (PFNGLGETQUERYOBJECTI64VPROC)
      get_extension_func("glGetQueryObjecti64v");
    _glGetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)
      get_extension_func("glGetQueryObjectui64v");

    _glGetInteger64v = (PFNGLGETINTEGER64VPROC)
      get_extension_func("glGetInteger64v");
  }
#endif

#ifdef OPENGLES_2
  // In OpenGL ES 2.x, this is supported in the core.
  _glBlendEquation = glBlendEquation;
#else
  _glBlendEquation = NULL;
  bool supports_blend_equation = false;
  if (is_at_least_gl_version(1, 2)) {
    supports_blend_equation = true;
    _glBlendEquation = (PFNGLBLENDEQUATIONPROC)
      get_extension_func("glBlendEquation");
  } else if (has_extension("GL_OES_blend_subtract")) {
    supports_blend_equation = true;
    _glBlendEquation = (PFNGLBLENDEQUATIONPROC)
      get_extension_func("glBlendEquationOES");
  } else if (has_extension("GL_EXT_blend_minmax")) {
    supports_blend_equation = true;
    _glBlendEquation = (PFNGLBLENDEQUATIONPROC)
      get_extension_func("glBlendEquationEXT");
  }
  if (supports_blend_equation && _glBlendEquation == NULL) {
    GLCAT.warning()
      << "BlendEquation advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
  }
  if (_glBlendEquation == NULL) {
    _glBlendEquation = null_glBlendEquation;
  }
#endif

#ifdef OPENGLES_2
  // In OpenGL ES 2.x, this is supported in the core.
  _glBlendColor = glBlendColor;
#else
  _glBlendColor = NULL;
  bool supports_blend_color = false;
  if (is_at_least_gl_version(1, 2)) {
    supports_blend_color = true;
    _glBlendColor = (PFNGLBLENDCOLORPROC)
      get_extension_func("glBlendColor");
  } else if (has_extension("GL_EXT_blend_color")) {
    supports_blend_color = true;
    _glBlendColor = (PFNGLBLENDCOLORPROC)
      get_extension_func("glBlendColorEXT");
  }
  if (supports_blend_color && _glBlendColor == NULL) {
    GLCAT.warning()
      << "BlendColor advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
  }
  if (_glBlendColor == NULL) {
    _glBlendColor = null_glBlendColor;
  }
#endif

#ifdef OPENGLES
  _edge_clamp = GL_CLAMP_TO_EDGE;
#else
  _edge_clamp = GL_CLAMP;
  if (has_extension("GL_SGIS_texture_edge_clamp") ||
      is_at_least_gl_version(1, 2) || is_at_least_gles_version(1, 1)) {
    _edge_clamp = GL_CLAMP_TO_EDGE;
  }
#endif

  _border_clamp = _edge_clamp;
#ifndef OPENGLES
  if (gl_support_clamp_to_border &&
      (has_extension("GL_ARB_texture_border_clamp") ||
       is_at_least_gl_version(1, 3))) {
    _border_clamp = GL_CLAMP_TO_BORDER;
  }
#endif

  _mirror_repeat = GL_REPEAT;
  if (has_extension("GL_ARB_texture_mirrored_repeat") ||
      is_at_least_gl_version(1, 4) ||
      has_extension("GL_OES_texture_mirrored_repeat")) {
    _mirror_repeat = GL_MIRRORED_REPEAT;
  }

  _mirror_clamp = _edge_clamp;
  _mirror_edge_clamp = _edge_clamp;
  _mirror_border_clamp = _border_clamp;
#ifndef OPENGLES
  if (has_extension("GL_EXT_texture_mirror_clamp")) {
    _mirror_clamp = GL_MIRROR_CLAMP_EXT;
    _mirror_edge_clamp = GL_MIRROR_CLAMP_TO_EDGE_EXT;
    _mirror_border_clamp = GL_MIRROR_CLAMP_TO_BORDER_EXT;
  }
#endif

  if (_supports_multisample) {
    GLint sample_buffers = 0;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sample_buffers);
    if (sample_buffers != 1) {
      // Even if the API supports multisample, we might have ended up
      // with a framebuffer that doesn't have any multisample bits.
      // (It's also possible the graphics card doesn't provide any
      // framebuffers with multisample.)  In this case, we don't
      // really support the multisample API's, since they won't do
      // anything.
      _supports_multisample = false;
    }
  }

  GLint max_texture_size = 0;
  GLint max_3d_texture_size = 0;
  GLint max_2d_texture_array_layers = 0;
  GLint max_cube_map_size = 0;

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  _max_texture_dimension = max_texture_size;

  if (_supports_3d_texture) {
#ifndef OPENGLES_1
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_3d_texture_size);
#endif
    _max_3d_texture_dimension = max_3d_texture_size;
  } else {
    _max_3d_texture_dimension = 0;
  }
#ifndef OPENGLES
  if(_supports_2d_texture_array) {
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS_EXT, &max_2d_texture_array_layers);
    _max_2d_texture_array_layers = max_2d_texture_array_layers;
  }
#endif
  if (_supports_cube_map) {
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &max_cube_map_size);
    _max_cube_map_dimension = max_cube_map_size;
  } else {
    _max_cube_map_dimension = 0;
  }

  GLint max_elements_vertices = 0, max_elements_indices = 0;
#ifndef OPENGLES
  glGetIntegerv(GL_MAX_ELEMENTS_VERTICES, &max_elements_vertices);
  glGetIntegerv(GL_MAX_ELEMENTS_INDICES, &max_elements_indices);
  if (max_elements_vertices > 0) {
    _max_vertices_per_array = max_elements_vertices;
  }
  if (max_elements_indices > 0) {
    _max_vertices_per_primitive = max_elements_indices;
  }
#endif  // OPENGLES

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max texture dimension = " << _max_texture_dimension
      << ", max 3d texture = " << _max_3d_texture_dimension
      << ", max 2d texture array = " << max_2d_texture_array_layers
      << ", max cube map = " << _max_cube_map_dimension << "\n";
    GLCAT.debug()
      << "max_elements_vertices = " << max_elements_vertices
      << ", max_elements_indices = " << max_elements_indices << "\n";
    if (_supports_buffers) {
      if (vertex_buffers) {
        GLCAT.debug()
          << "vertex buffer objects are supported.\n";
      } else {
        GLCAT.debug()
          << "vertex buffer objects are supported (but not enabled).\n";
      }
    } else {
      GLCAT.debug()
        << "vertex buffer objects are NOT supported.\n";
    }

#ifdef SUPPORT_IMMEDIATE_MODE
    if (!vertex_arrays) {
      GLCAT.debug()
        << "immediate mode commands will be used instead of vertex arrays.\n";
    }
#endif

    if (!_supports_compressed_texture) {
      GLCAT.debug()
        << "Texture compression is not supported.\n";

    } else {
      GLint num_compressed_formats = 0;
      glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &num_compressed_formats);
      if (num_compressed_formats == 0) {
        GLCAT.debug()
          << "No specific compressed texture formats are supported.\n";
      } else {
        GLCAT.debug()
          << "Supported compressed texture formats:\n";
        GLint *formats = (GLint *)PANDA_MALLOC_ARRAY(num_compressed_formats * sizeof(GLint));
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats);
        for (int i = 0; i < num_compressed_formats; ++i) {
          switch (formats[i]) {
          case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGB_S3TC_DXT1_EXT\n";
            break;

          case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_S3TC_DXT1_EXT\n";
            break;
#ifdef OPENGLES
          case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG\n";
            break;

          case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG\n";
            break;

          case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG\n";
            break;

          case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG\n";
            break;
#else
          case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_S3TC_DXT3_EXT\n";
            break;

          case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_S3TC_DXT5_EXT\n";
            break;

          case GL_COMPRESSED_RGB_FXT1_3DFX:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGB_FXT1_3DFX\n";
            break;

          case GL_COMPRESSED_RGBA_FXT1_3DFX:
            GLCAT.debug(false) << "  GL_COMPRESSED_RGBA_FXT1_3DFX\n";
            break;
#endif

          default:
            GLCAT.debug(false)
              << "  Unknown compressed format 0x" << hex << formats[i]
              << dec << "\n";
          }
        }
        PANDA_FREE_ARRAY(formats);
      }
    }
  }

  _num_active_texture_stages = 0;

  // Check availability of anisotropic texture filtering.
  _supports_anisotropy = false;
  _max_anisotropy = 1.0;
  if (has_extension("GL_EXT_texture_filter_anisotropic")) {
    GLfloat max_anisotropy;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_anisotropy);
    _max_anisotropy = (PN_stdfloat)max_anisotropy;
    _supports_anisotropy = true;
  }

  // Check availability of image read/write functionality in shaders.
  _max_image_units = 0;
#ifndef OPENGLES
  if (is_at_least_gl_version(4, 2) || has_extension("GL_ARB_shader_image_load_store")) {
    _glBindImageTexture = (PFNGLBINDIMAGETEXTUREPROC)
      get_extension_func("glBindImageTexture");
    _glMemoryBarrier = (PFNGLMEMORYBARRIERPROC)
      get_extension_func("glMemoryBarrier");

    glGetIntegerv(GL_MAX_IMAGE_UNITS, &_max_image_units);

  } else if (has_extension("GL_EXT_shader_image_load_store")) {
    _glBindImageTexture = (PFNGLBINDIMAGETEXTUREPROC)
      get_extension_func("glBindImageTextureEXT");
    _glMemoryBarrier = (PFNGLMEMORYBARRIERPROC)
      get_extension_func("glMemoryBarrierEXT");

    glGetIntegerv(GL_MAX_IMAGE_UNITS_EXT, &_max_image_units);

  } else {
    _glBindImageTexture = NULL;
    _glMemoryBarrier = NULL;
  }

  // Check availability of multi-bind functions.
  _supports_multi_bind = false;
  if (is_at_least_gl_version(4, 4) || has_extension("GL_ARB_multi_bind")) {
    _glBindTextures = (PFNGLBINDTEXTURESPROC)
      get_extension_func("glBindTextures");
    _glBindImageTextures = (PFNGLBINDIMAGETEXTURESPROC)
      get_extension_func("glBindImageTextures");

    if (_glBindTextures != NULL && _glBindImageTextures != NULL) {
      _supports_multi_bind = true;
    } else {
      GLCAT.warning()
        << "ARB_multi_bind advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
    }
  }

  if (is_at_least_gl_version(4, 3) || has_extension("GL_ARB_internalformat_query2")) {
    _glGetInternalformativ = (PFNGLGETINTERNALFORMATIVPROC)
      get_extension_func("glGetInternalformativ");

    if (_glGetInternalformativ == NULL) {
      GLCAT.warning()
        << "ARB_internalformat_query2 advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
    }
  }

  _supports_bindless_texture = false;
  if (has_extension("GL_ARB_bindless_texture")) {
    _glGetTextureHandle = (PFNGLGETTEXTUREHANDLEPROC)
      get_extension_func("glGetTextureHandleARB");
    _glMakeTextureHandleResident = (PFNGLMAKETEXTUREHANDLERESIDENTPROC)
      get_extension_func("glMakeTextureHandleResidentARB");
    _glUniformHandleui64 = (PFNGLUNIFORMHANDLEUI64PROC)
      get_extension_func("glUniformHandleui64ARB");

    if (_glGetTextureHandle == NULL || _glMakeTextureHandleResident == NULL ||
       _glUniformHandleui64 == NULL) {
      GLCAT.warning()
        << "GL_ARB_bindless_texture advertised as supported by OpenGL runtime, but could not get pointers to extension function.\n";
    } else {
      _supports_bindless_texture = true;
    }
  }
#endif

#ifndef OPENGLES
  _supports_get_program_binary = false;

  if (is_at_least_gl_version(4, 1) || has_extension("GL_ARB_get_program_binary")) {
    _glGetProgramBinary = (PFNGLGETPROGRAMBINARYPROC)
      get_extension_func("glGetProgramBinary");

    if (_glGetProgramBinary != NULL) {
      _supports_get_program_binary = true;
    }
  }
#endif

  report_my_gl_errors();

  if (support_stencil) {
    GLint num_stencil_bits;
    glGetIntegerv(GL_STENCIL_BITS, &num_stencil_bits);
    _supports_stencil = (num_stencil_bits != 0);
  }

  _supports_stencil_wrap =
    has_extension("GL_EXT_stencil_wrap") || has_extension("GL_OES_stencil_wrap");
  _supports_two_sided_stencil = has_extension("GL_EXT_stencil_two_side");
  if (_supports_two_sided_stencil) {
    _glActiveStencilFaceEXT = (PFNGLACTIVESTENCILFACEEXTPROC)
      get_extension_func("glActiveStencilFaceEXT");
  }
  else {
    _glActiveStencilFaceEXT = 0;
  }

#ifndef OPENGLES
  // Some drivers expose one, some expose the other. ARB seems to be the newer one.
  if (has_extension("GL_ARB_draw_instanced")) {
    _glDrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)
      get_extension_func("glDrawArraysInstancedARB");
    _glDrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)
      get_extension_func("glDrawElementsInstancedARB");
    _supports_geometry_instancing = true;
  } else if (has_extension("GL_EXT_draw_instanced")) {
    _glDrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)
      get_extension_func("glDrawArraysInstancedEXT");
    _glDrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)
      get_extension_func("glDrawElementsInstancedEXT");
    _supports_geometry_instancing = true;
  } else {
    _glDrawElementsInstanced = 0;
    _glDrawArraysInstanced = 0;
  }
#endif

  _auto_rescale_normal = false;

  // Ensure the initial state is what we say it should be (in some
  // cases, we don't want the GL default settings; in others, we have
  // to force the point with some drivers that aren't strictly
  // compliant w.r.t. initial settings).
  glFrontFace(GL_CCW);
#ifndef OPENGLES_2
  glDisable(GL_LINE_SMOOTH);
  glDisable(GL_POINT_SMOOTH);
#ifndef OPENGLES
  glDisable(GL_POLYGON_SMOOTH);
#endif  // OPENGLES

  if (_supports_multisample) {
    glDisable(GL_MULTISAMPLE);
  }
#endif

  // Set up all the enabled/disabled flags to GL's known initial
  // values: everything off.
  _multisample_mode = 0;
  _line_smooth_enabled = false;
  _point_smooth_enabled = false;
  _polygon_smooth_enabled = false;
  _stencil_test_enabled = false;
  _blend_enabled = false;
  _depth_test_enabled = false;
  _fog_enabled = false;
  _alpha_test_enabled = false;
  _polygon_offset_enabled = false;
  _flat_shade_model = false;
  _decal_level = 0;
  _tex_gen_point_sprite = false;

#ifndef OPENGLES
  // Dither is on by default in GL; let's turn it off
  glDisable(GL_DITHER);
#endif  // OPENGLES
  _dithering_enabled = false;

#ifndef OPENGLES_1
  _current_shader = (Shader *)NULL;
  _current_shader_context = (ShaderContext *)NULL;
  _vertex_array_shader = (Shader *)NULL;
  _vertex_array_shader_context = (ShaderContext *)NULL;
  _texture_binding_shader = (Shader *)NULL;
  _texture_binding_shader_context = (ShaderContext *)NULL;
#endif

#ifdef OPENGLES_2
  _max_lights = 0;
#else
  // Count the max number of lights
  GLint max_lights = 0;
  glGetIntegerv(GL_MAX_LIGHTS, &max_lights);
  _max_lights = max_lights;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max lights = " << _max_lights << "\n";
  }
#endif

#ifdef OPENGLES_2
  _max_clip_planes = 0;
#else
  // Count the max number of clipping planes
  GLint max_clip_planes = 0;
  glGetIntegerv(GL_MAX_CLIP_PLANES, &max_clip_planes);
  _max_clip_planes = max_clip_planes;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "max clip planes = " << _max_clip_planes << "\n";
  }
#endif

  if (_supports_multitexture) {
    GLint max_texture_stages = 0;
#ifdef OPENGLES_2
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_texture_stages);
#else
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &max_texture_stages);
#endif
    _max_texture_stages = max_texture_stages;

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "max texture stages = " << _max_texture_stages << "\n";
    }
  }
  _current_vbuffer_index = 0;
  _current_ibuffer_index = 0;
  _current_fbo = 0;
  _auto_antialias_mode = false;
  _render_mode = RenderModeAttrib::M_filled;
  _point_size = 1.0f;
  _point_perspective = false;

  _vertex_blending_enabled = false;

  report_my_gl_errors();

#ifndef OPENGLES_2
  if (gl_cheap_textures) {
    GLCAT.info()
      << "Setting glHint() for fastest textures.\n";
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
  }

  // Use per-vertex fog if per-pixel fog requires SW renderer
  glHint(GL_FOG_HINT, GL_DONT_CARE);
#endif

  GLint num_red_bits = 0;
  glGetIntegerv(GL_RED_BITS, &num_red_bits);
  if (num_red_bits < 8) {
    glEnable(GL_DITHER);
    _dithering_enabled = true;
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "frame buffer depth = " << num_red_bits
        << " bits/channel, enabling dithering\n";
    }
  }

  _error_count = 0;

  report_my_gl_errors();

  void gl_set_stencil_functions (StencilRenderStates *stencil_render_states);
  gl_set_stencil_functions(_stencil_render_states);

#if defined(HAVE_CG) && !defined(OPENGLES)

  typedef struct {
    CGprofile cg_profile;
    int shader_model;
  } CG_PROFILE_TO_SHADER_MODEL;

  static CG_PROFILE_TO_SHADER_MODEL cg_profile_to_shader_model_array[] = {
    // gp5fp - OpenGL fragment profile for GeForce 400 Series and up
    (CGprofile)7017, /*CG_PROFILE_GP5FP,*/
    SM_50,

    // gp4fp - OpenGL fragment profile for G8x (GeForce 8xxx and up)
    (CGprofile)7010, /*CG_PROFILE_GP4FP,*/
    SM_40,

    // fp40 - OpenGL fragment profile for NV4x (GeForce 6xxx and 7xxx
    // Series, NV4x-based Quadro FX, etc.)
    CG_PROFILE_FP40,
    SM_30,

    // fp30 - OpenGL fragment profile for NV3x (GeForce FX, Quadro FX, etc.)
    CG_PROFILE_FP30,
    SM_2X,

    // This OpenGL profile corresponds to the per-fragment
    // functionality introduced by GeForce FX and other DirectX 9
    // GPUs.
    CG_PROFILE_ARBFP1,
    SM_20,

    // fp20 - OpenGL fragment profile for NV2x (GeForce3, GeForce4 Ti,
    // Quadro DCC, etc.)
    CG_PROFILE_FP20,
    SM_11,

    // no shader support
    CG_PROFILE_UNKNOWN,
    SM_00,
  };

  int index;
  CG_PROFILE_TO_SHADER_MODEL *cg_profile_to_shader_model;

  index = 0;
  cg_profile_to_shader_model = cg_profile_to_shader_model_array;
  while (cg_profile_to_shader_model->shader_model != SM_00) {
    if (cgGLIsProfileSupported(cg_profile_to_shader_model->cg_profile)) {
      _shader_model = cg_profile_to_shader_model->shader_model;
      break;
    }
    cg_profile_to_shader_model++;
  }

  // DisplayInformation may have better shader model detection
  {
    GraphicsPipe *pipe;
    DisplayInformation *display_information;

    pipe = this->get_pipe();
    if (pipe) {
      display_information = pipe->get_display_information ();
      if (display_information) {
        if (display_information->get_shader_model() > _shader_model) {
          _shader_model = display_information->get_shader_model();
        }
      }
    }
  }
  _auto_detect_shader_model = _shader_model;

  CGprofile vertex_profile;
  CGprofile pixel_profile;

  vertex_profile = cgGLGetLatestProfile(CG_GL_VERTEX);
  pixel_profile = cgGLGetLatestProfile(CG_GL_FRAGMENT);
  if (GLCAT.is_debug()) {
#if CG_VERSION_NUM >= 2200
    GLCAT.debug() << "Supported Cg profiles:\n";
    int num_profiles = cgGetNumSupportedProfiles();
    for (int i = 0; i < num_profiles; ++i) {
      CGprofile profile = cgGetSupportedProfile(i);
      if (cgGLIsProfileSupported(profile)) {
        GLCAT.debug() << "  " << cgGetProfileString(profile) << "\n";
      }
    }
#endif  // CG_VERSION_NUM >= 2200

    GLCAT.debug()
      << "\nCg vertex profile = " << cgGetProfileString(vertex_profile) << "  id = " << vertex_profile
      << "\nCg pixel profile = " << cgGetProfileString(pixel_profile) << "  id = " << pixel_profile
      << "\nshader model = " << _shader_model
      << "\n";
  }

#endif

  // Now that the GSG has been initialized, make it available for
  // optimizations.
  add_gsg(this);
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::finish
//       Access: Public, Virtual
//  Description: Force the graphics card to finish drawing before
//               returning.  !!!!!HACK WARNING!!!!
//               glfinish does not actually wait for the graphics card to finish drawing
//               only for draw calls to finish.  Thus flip may not happene
//               immediately.  Instead we read a single pixel from
//               the framebuffer.  This forces the graphics card to
//               finish drawing the frame before returning.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
finish() {
  // Rather than call glfinish which returns immediately if
  // draw commands have been submitted, we will read a single pixel
  // from the frame.  That will force the graphics card to finish
  // drawing before it is called
  char data[4];
  glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,&data);
  //glFinish();
}

////////////////////////////////////////////////////////////////////
//     Function: GraphicsStateGuardian::clear
//       Access: Public
//  Description: Clears the framebuffer within the current
//               DisplayRegion, according to the flags indicated by
//               the given DrawableRegion object.
//
//               This does not set the DisplayRegion first.  You
//               should call prepare_display_region() to specify the
//               region you wish the clear operation to apply to.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
clear(DrawableRegion *clearable) {
  PStatGPUTimer timer(this, _clear_pcollector);
  report_my_gl_errors();

  if (!clearable->is_any_clear_active()) {
    return;
  }

  set_state_and_transform(RenderState::make_empty(), _internal_transform);

  int mask = 0;

#ifndef OPENGLES
  if (_current_fbo != 0 && _glClearBufferfv != NULL) {
    // We can use glClearBuffer to clear all the color attachments,
    // which protects us from the overhead of having to call set_draw_buffer
    // for every single attachment.
    int index = 0;

    if (_current_properties->get_color_bits() > 0) {
      if (_current_properties->is_stereo()) {
        // Clear both left and right attachments.
        if (clearable->get_clear_active(GraphicsOutput::RTP_color)) {
          LColorf v = LCAST(float, clearable->get_clear_value(GraphicsOutput::RTP_color));
          _glClearBufferfv(GL_COLOR, index, v.get_data());
          _glClearBufferfv(GL_COLOR, index + 1, v.get_data());
        }
        index += 2;
      } else {
        if (clearable->get_clear_active(GraphicsOutput::RTP_color)) {
          LColorf v = LCAST(float, clearable->get_clear_value(GraphicsOutput::RTP_color));
          _glClearBufferfv(GL_COLOR, index, v.get_data());
        }
        ++index;
      }
    }
    for (int i = 0; i < _current_properties->get_aux_rgba(); ++i) {
      int layerid = GraphicsOutput::RTP_aux_rgba_0 + i;
      if (clearable->get_clear_active(layerid)) {
        LColorf v = LCAST(float, clearable->get_clear_value(layerid));
        _glClearBufferfv(GL_COLOR, index, v.get_data());
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_hrgba(); ++i) {
      int layerid = GraphicsOutput::RTP_aux_hrgba_0 + i;
      if (clearable->get_clear_active(layerid)) {
        LColorf v = LCAST(float, clearable->get_clear_value(layerid));
        _glClearBufferfv(GL_COLOR, index, v.get_data());
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_float(); ++i) {
      int layerid = GraphicsOutput::RTP_aux_float_0 + i;
      if (clearable->get_clear_active(layerid)) {
        LColorf v = LCAST(float, clearable->get_clear_value(layerid));
        _glClearBufferfv(GL_COLOR, index, v.get_data());
      }
      ++index;
    }
  } else
#endif
  {
    if (_current_properties->get_aux_mask() != 0) {
      for (int i = 0; i < _current_properties->get_aux_rgba(); ++i) {
        int layerid = GraphicsOutput::RTP_aux_rgba_0 + i;
        int layerbit = RenderBuffer::T_aux_rgba_0 << i;
        if (clearable->get_clear_active(layerid)) {
          LColor v = clearable->get_clear_value(layerid);
          glClearColor(v[0], v[1], v[2], v[3]);
          set_draw_buffer(layerbit);
          glClear(GL_COLOR_BUFFER_BIT);
        }
      }
      for (int i = 0; i < _current_properties->get_aux_hrgba(); ++i) {
        int layerid = GraphicsOutput::RTP_aux_hrgba_0 + i;
        int layerbit = RenderBuffer::T_aux_hrgba_0 << i;
        if (clearable->get_clear_active(layerid)) {
          LColor v = clearable->get_clear_value(layerid);
          glClearColor(v[0], v[1], v[2], v[3]);
          set_draw_buffer(layerbit);
          glClear(GL_COLOR_BUFFER_BIT);
        }
      }
      for (int i = 0; i < _current_properties->get_aux_float(); ++i) {
        int layerid = GraphicsOutput::RTP_aux_float_0 + i;
        int layerbit = RenderBuffer::T_aux_float_0 << i;
        if (clearable->get_clear_active(layerid)) {
          LColor v = clearable->get_clear_value(layerid);
          glClearColor(v[0], v[1], v[2], v[3]);
          set_draw_buffer(layerbit);
          glClear(GL_COLOR_BUFFER_BIT);
        }
      }

      // In the past, it was possible to set the draw buffer
      // once in prepare_display_region and then forget about it.
      // Now, with aux layers, it is necessary to occasionally
      // change the draw buffer.  In time, I think there will need
      // to be a draw buffer attrib.  Until then, this little hack
      // to put things back the way they were after
      // prepare_display_region will do.

      set_draw_buffer(_draw_buffer_type);
    }

    if (_current_properties->get_color_bits() > 0) {
      if (clearable->get_clear_color_active()) {
        LColor v = clearable->get_clear_color();
        glClearColor(v[0], v[1], v[2], v[3]);
        if (gl_color_mask) {
          glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        _state_mask.clear_bit(ColorWriteAttrib::get_class_slot());
        mask |= GL_COLOR_BUFFER_BIT;
      }
    }
  }

  if (clearable->get_clear_depth_active()) {
#ifdef OPENGLES
    glClearDepthf(clearable->get_clear_depth());
#else
    glClearDepth(clearable->get_clear_depth());
#endif  // OPENGLES
#ifdef GSG_VERBOSE
    GLCAT.spam()
      << "glDepthMask(GL_TRUE)" << endl;
#endif
    glDepthMask(GL_TRUE);
    _state_mask.clear_bit(DepthWriteAttrib::get_class_slot());
    mask |= GL_DEPTH_BUFFER_BIT;
  }

  if (clearable->get_clear_stencil_active()) {
    glClearStencil(clearable->get_clear_stencil());
    mask |= GL_STENCIL_BUFFER_BIT;
  }

  glClear(mask);

  if (GLCAT.is_spam()) {
    GLCAT.spam() << "glClear(";
    if (mask & GL_COLOR_BUFFER_BIT) {
      GLCAT.spam(false) << "GL_COLOR_BUFFER_BIT|";
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
      GLCAT.spam(false) << "GL_DEPTH_BUFFER_BIT|";
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
      GLCAT.spam(false) << "GL_STENCIL_BUFFER_BIT|";
    }
#ifndef OPENGLES
    if (mask & GL_ACCUM_BUFFER_BIT) {
      GLCAT.spam(false) << "GL_ACCUM_BUFFER_BIT|";
    }
#endif
    GLCAT.spam(false) << ")" << endl;
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_display_region
//       Access: Public, Virtual
//  Description: Prepare a display region for rendering (set up
//               scissor region and viewport)
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
prepare_display_region(DisplayRegionPipelineReader *dr) {
  nassertv(dr != (DisplayRegionPipelineReader *)NULL);
  GraphicsStateGuardian::prepare_display_region(dr);

  int l, b, w, h;
  dr->get_region_pixels(l, b, w, h);
  _viewport_x = l;
  _viewport_y = b;
  _viewport_width = w;
  _viewport_height = h;
  GLint x = GLint(l);
  GLint y = GLint(b);
  GLsizei width = GLsizei(w);
  GLsizei height = GLsizei(h);

  _draw_buffer_type = dr->get_object()->get_draw_buffer_type() & _current_properties->get_buffer_mask() & _stereo_buffer_mask;
  _draw_buffer_type |= _current_properties->get_aux_mask();
  set_draw_buffer(_draw_buffer_type);

  if (dr->get_scissor_enabled()) {
    glEnable(GL_SCISSOR_TEST);
    _scissor_enabled = true;
  } else {
    glDisable(GL_SCISSOR_TEST);
    _scissor_enabled = false;
  }

#ifndef OPENGLES
  if (_supports_viewport_arrays) {
    int count = dr->get_num_regions();
    GLfloat *viewports = (GLfloat *)alloca(sizeof(GLfloat) * 4 * count);
    GLint *scissors = (GLint *)alloca(sizeof(GLint) * 4 * count);

    for (int i = 0; i < count; ++i) {
      GLint *sr = scissors + i * 4;
      dr->get_region_pixels(i, sr[0], sr[1], sr[2], sr[3]);
      GLfloat *vr = viewports + i * 4;
      vr[0] = (GLfloat) sr[0];
      vr[1] = (GLfloat) sr[1];
      vr[2] = (GLfloat) sr[2];
      vr[3] = (GLfloat) sr[3];
    }
    _glViewportArrayv(0, count, viewports);
    if (dr->get_scissor_enabled()) {
      _glScissorArrayv(0, count, scissors);
    }

  } else
#endif  // OPENGLES
  {
    glViewport(x, y, width, height);
    if (dr->get_scissor_enabled()) {
      glScissor(x, y, width, height);
    }
  }

  report_my_gl_errors();
  do_point_size();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::clear_before_callback
//       Access: Public, Virtual
//  Description: Resets any non-standard graphics state that might
//               give a callback apoplexy.  Some drivers require that
//               the graphics state be restored to neutral before
//               performing certain operations.  In OpenGL, for
//               instance, this closes any open vertex buffers.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
clear_before_callback() {
  disable_standard_vertex_arrays();
  unbind_buffers();

  // Some callbacks may quite reasonably assume that the active
  // texture stage is still set to stage 0.  CEGUI, in particular,
  // makes this assumption.
  _glActiveTexture(GL_TEXTURE0);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::calc_projection_mat
//       Access: Public, Virtual
//  Description: Given a lens, calculates the appropriate projection
//               matrix for use with this gsg.  Note that the
//               projection matrix depends a lot upon the coordinate
//               system of the rendering API.
//
//               The return value is a TransformState if the lens is
//               acceptable, NULL if it is not.
////////////////////////////////////////////////////////////////////
CPT(TransformState) CLP(GraphicsStateGuardian)::
calc_projection_mat(const Lens *lens) {
  if (lens == (Lens *)NULL) {
    return NULL;
  }

  if (!lens->is_linear()) {
    return NULL;
  }

  // The projection matrix must always be right-handed Y-up, even if
  // our coordinate system of choice is otherwise, because certain GL
  // calls (specifically glTexGen(GL_SPHERE_MAP)) assume this kind of
  // a coordinate system.  Sigh.  In order to implement a Z-up (or
  // other arbitrary) coordinate system, we'll use a Y-up projection
  // matrix, and store the conversion to our coordinate system of
  // choice in the modelview matrix.

  LMatrix4 result =
    LMatrix4::convert_mat(CS_yup_right, lens->get_coordinate_system()) *
    lens->get_projection_mat(_current_stereo_channel);

  if (_scene_setup->get_inverted()) {
    // If the scene is supposed to be inverted, then invert the
    // projection matrix.
    result *= LMatrix4::scale_mat(1.0f, -1.0f, 1.0f);
  }

  return TransformState::make_mat(result);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_lens
//       Access: Public, Virtual
//  Description: Makes the current lens (whichever lens was most
//               recently specified with set_scene()) active, so
//               that it will transform future rendered geometry.
//               Normally this is only called from the draw process,
//               and usually it is called by set_scene().
//
//               The return value is true if the lens is acceptable,
//               false if it is not.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
prepare_lens() {
#ifndef OPENGLES_2
  if (GLCAT.is_spam()) {
    GLCAT.spam()
      << "glMatrixMode(GL_PROJECTION): " << _projection_mat->get_mat() << endl;
  }

  glMatrixMode(GL_PROJECTION);
  GLPf(LoadMatrix)(_projection_mat->get_mat().get_data());
  report_my_gl_errors();

  do_point_size();
#endif

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GraphicsStateGuardian::begin_frame
//       Access: Public, Virtual
//  Description: Called before each frame is rendered, to allow the
//               GSG a chance to do any internal cleanup before
//               beginning the frame.
//
//               The return value is true if successful (in which case
//               the frame will be drawn and end_frame() will be
//               called later), or false if unsuccessful (in which
//               case nothing will be drawn and end_frame() will not
//               be called).
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
begin_frame(Thread *current_thread) {
  if (!GraphicsStateGuardian::begin_frame(current_thread)) {
    return false;
  }
  report_my_gl_errors();

#ifdef DO_PSTATS
  _vertices_display_list_pcollector.clear_level();
  _vertices_immediate_pcollector.clear_level();
  _primitive_batches_display_list_pcollector.clear_level();
#endif

#ifndef NDEBUG
  _show_texture_usage = false;
  if (gl_show_texture_usage) {
    // When this is true, then every other second, we show the usage
    // textures instead of the real textures.
    double now = ClockObject::get_global_clock()->get_frame_time();
    int this_second = (int)floor(now);
    if (this_second & 1) {
      _show_texture_usage = true;
      _show_texture_usage_index = this_second >> 1;

      int max_size = gl_show_texture_usage_max_size;
      if (max_size != _show_texture_usage_max_size) {
        // Remove the cache of usage textures; we've changed the max
        // size.
        UsageTextures::iterator ui;
        for (ui = _usage_textures.begin();
             ui != _usage_textures.end();
             ++ui) {
          GLuint index = (*ui).second;
          glDeleteTextures(1, &index);
        }
        _usage_textures.clear();
        _show_texture_usage_max_size = max_size;
      }
    }
  }
#endif  // NDEBUG

#ifdef DO_PSTATS
  /*if (_supports_timer_query) {
    // Measure the difference between the OpenGL clock and the
    // PStats clock.
    GLint64 time_ns;
    _glGetInteger64v(GL_TIMESTAMP, &time_ns);
    _timer_delta = time_ns * -0.000000001;
    _timer_delta += PStatClient::get_global_pstats()->get_real_time();
  }*/
#endif

#ifndef OPENGLES_1
  if (_current_properties->get_srgb_color()) {
    glEnable(GL_FRAMEBUFFER_SRGB);
  }
#endif

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GraphicsStateGuardian::begin_scene
//       Access: Public, Virtual
//  Description: Called between begin_frame() and end_frame() to mark
//               the beginning of drawing commands for a "scene"
//               (usually a particular DisplayRegion) within a frame.
//               All 3-D drawing commands, except the clear operation,
//               must be enclosed within begin_scene() .. end_scene().
//
//               The return value is true if successful (in which case
//               the scene will be drawn and end_scene() will be
//               called later), or false if unsuccessful (in which
//               case nothing will be drawn and end_scene() will not
//               be called).
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
begin_scene() {
  return GraphicsStateGuardian::begin_scene();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_scene
//       Access: Protected, Virtual
//  Description: Called between begin_frame() and end_frame() to mark
//               the end of drawing commands for a "scene" (usually a
//               particular DisplayRegion) within a frame.  All 3-D
//               drawing commands, except the clear operation, must be
//               enclosed within begin_scene() .. end_scene().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_scene() {
  GraphicsStateGuardian::end_scene();

  _dlights.clear();
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_frame
//       Access: Public, Virtual
//  Description: Called after each frame is rendered, to allow the
//               GSG a chance to do any internal cleanup after
//               rendering the frame, and before the window flips.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_frame(Thread *current_thread) {
  report_my_gl_errors();

#ifndef OPENGLES_1
  if (_current_properties->get_srgb_color()) {
    glDisable(GL_FRAMEBUFFER_SRGB);
  }
#endif

#ifdef DO_PSTATS
  // Check for textures, etc., that are no longer resident.  These
  // calls might be measurably expensive, and they don't have any
  // benefit unless we are actually viewing PStats, so don't do them
  // unless we're connected.  That will just mean that we'll count
  // everything as resident until the user connects PStats, at which
  // point it will then correct the assessment.  No harm done.
  if (PStatClient::is_connected()) {
    check_nonresident_texture(_prepared_objects->_texture_residency.get_inactive_resident());
    check_nonresident_texture(_prepared_objects->_texture_residency.get_active_resident());

    // OpenGL provides no methods for querying whether a buffer object
    // (vertex buffer) is resident.  In fact, the API appears geared
    // towards the assumption that such buffers are always resident.
    // OK.
  }
#endif

#ifndef OPENGLES_1
  // This breaks shaders across multiple regions.
  if (_vertex_array_shader_context != 0) {
    _vertex_array_shader_context->disable_shader_vertex_arrays();
    _vertex_array_shader = (Shader *)NULL;
    _vertex_array_shader_context = (ShaderContext *)NULL;
  }
  if (_texture_binding_shader_context != 0) {
    _texture_binding_shader_context->disable_shader_texture_bindings();
    _texture_binding_shader = (Shader *)NULL;
    _texture_binding_shader_context = (ShaderContext *)NULL;
  }
  if (_current_shader_context != 0) {
    _current_shader_context->unbind();
    _current_shader = (Shader *)NULL;
    _current_shader_context = (ShaderContext *)NULL;
  }
#endif

  // Calling glFlush() at the end of the frame is particularly
  // necessary if this is a single-buffered visual, so that the frame
  // will be finished drawing before we return to the application.
  // It's not clear what effect this has on our total frame time.
  //if (_force_flush || _current_properties->is_single_buffered()) {
  //  gl_flush();
  //}
  maybe_gl_finish();

  GraphicsStateGuardian::end_frame(current_thread);

  // Flush any PCollectors specific to this kind of GSG.
  _primitive_batches_display_list_pcollector.flush_level();
  _vertices_display_list_pcollector.flush_level();
  _vertices_immediate_pcollector.flush_level();

  // Now is a good time to delete any pending display lists.
#ifndef OPENGLES
  {
    LightMutexHolder holder(_lock);
    if (!_deleted_display_lists.empty()) {
      DeletedNames::iterator ddli;
      for (ddli = _deleted_display_lists.begin();
           ddli != _deleted_display_lists.end();
           ++ddli) {
        if (GLCAT.is_debug()) {
          GLCAT.debug()
            << "releasing display list index " << (int)(*ddli) << "\n";
        }
        glDeleteLists((*ddli), 1);
      }
      _deleted_display_lists.clear();
    }

    // And deleted queries, too, unless we're using query timers
    // in which case we'll need to reuse lots of them.
    if (!get_timer_queries_active() && !_deleted_queries.empty()) {
      if (GLCAT.is_spam()) {
        DeletedNames::iterator dqi;
        for (dqi = _deleted_queries.begin();
             dqi != _deleted_queries.end();
             ++dqi) {
          GLCAT.spam()
            << "releasing query index " << (int)(*dqi) << "\n";
        }
      }
      _glDeleteQueries(_deleted_queries.size(), &_deleted_queries[0]);
      _deleted_queries.clear();
    }
  }
#endif  // OPENGLES

#ifndef NDEBUG
  if (_check_errors || (_supports_debug && gl_debug)) {
    report_my_gl_errors();
  } else {
    PStatTimer timer(_check_error_pcollector);

    // If _check_errors is false, we still want to check for errors
    // once during this frame, so that we know if anything went wrong.
    GLenum error_code = glGetError();
    if (error_code != GL_NO_ERROR) {
      int error_count = 0;
      bool deactivate = !report_errors_loop(__LINE__, __FILE__, error_code, error_count);

      if (error_count == 1) {
        GLCAT.error()
          << "An OpenGL error (" << get_error_string(error_code)
          << ") has occurred.";
      } else {
        GLCAT.error()
          << error_count << " OpenGL errors have occurred.";
      }

      if (_supports_debug) {
        GLCAT.error(false) << "  Set gl-debug #t "
          << "in your PRC file to display more information.\n";
      } else {
        GLCAT.error(false) << "  Set gl-check-errors #t "
          << "in your PRC file to display more information.\n";
      }

      if (deactivate) {
        panic_deactivate();
      }
    }
  }
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_draw_primitives
//       Access: Public, Virtual
//  Description: Called before a sequence of draw_primitive()
//               functions are called, this should prepare the vertex
//               data for rendering.  It returns true if the vertices
//               are ok, false to abort this group of primitives.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
begin_draw_primitives(const GeomPipelineReader *geom_reader,
                      const GeomMunger *munger,
                      const GeomVertexDataPipelineReader *data_reader,
                      bool force) {
#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "begin_draw_primitives: " << *(data_reader->get_object()) << "\n";
  }
#endif  // NDEBUG

  if (!GraphicsStateGuardian::begin_draw_primitives(geom_reader, munger, data_reader, force)) {
    return false;
  }
  nassertr(_data_reader != (GeomVertexDataPipelineReader *)NULL, false);

  _geom_display_list = 0;

  if (_auto_antialias_mode) {
    switch (geom_reader->get_primitive_type()) {
    case GeomPrimitive::PT_polygons:
    case GeomPrimitive::PT_patches:
      setup_antialias_polygon();
      break;
    case GeomPrimitive::PT_points:
      setup_antialias_point();
      break;
    case GeomPrimitive::PT_lines:
      setup_antialias_line();
      break;
    case GeomPrimitive::PT_none:
      break;
    }

    int transparency_slot = TransparencyAttrib::get_class_slot();
    int color_write_slot = ColorWriteAttrib::get_class_slot();
    int color_blend_slot = ColorBlendAttrib::get_class_slot();
    if (!_state_mask.get_bit(transparency_slot) ||
        !_state_mask.get_bit(color_write_slot) ||
        !_state_mask.get_bit(color_blend_slot)) {
      do_issue_blending();
      _state_mask.set_bit(transparency_slot);
      _state_mask.set_bit(color_write_slot);
      _state_mask.set_bit(color_blend_slot);
    }
  }

  const GeomVertexAnimationSpec &animation =
    _data_reader->get_format()->get_animation();
  bool hardware_animation = (animation.get_animation_type() == Geom::AT_hardware);
#ifndef OPENGLES
  if (hardware_animation) {
    // Set up the transform matrices for vertex blending.
    nassertr(_supports_vertex_blend, false);
    glEnable(GL_VERTEX_BLEND_ARB);
    _glVertexBlend(animation.get_num_transforms());

    const TransformTable *table = _data_reader->get_transform_table();
    if (table != (TransformTable *)NULL) {
      if (animation.get_indexed_transforms()) {
        nassertr(_supports_matrix_palette, false);
        // We are loading the indexed matrix palette.  The ARB decided
        // to change this interface from that for the list of
        // nonindexed matrices, to make it easier to load an arbitrary
        // number of matrices.
        glEnable(GL_MATRIX_PALETTE_ARB);

        glMatrixMode(GL_MATRIX_PALETTE_ARB);

        for (int i = 0; i < table->get_num_transforms(); ++i) {
          LMatrix4 mat;
          table->get_transform(i)->mult_matrix(mat, _internal_transform->get_mat());
          _glCurrentPaletteMatrix(i);
          GLPf(LoadMatrix)(mat.get_data());
        }

        // Presumably loading the matrix palette does not step on the
        // GL_MODELVIEW matrix?

      } else {
        // We are loading the list of nonindexed matrices.  This is a
        // little clumsier.

        if (_supports_matrix_palette) {
          glDisable(GL_MATRIX_PALETTE_ARB);
        }

        // GL_MODELVIEW0 and 1 are different than the rest.
        int i = 0;
        if (i < table->get_num_transforms()) {
          LMatrix4 mat;
          table->get_transform(i)->mult_matrix(mat, _internal_transform->get_mat());
          glMatrixMode(GL_MODELVIEW0_ARB);
          GLPf(LoadMatrix)(mat.get_data());
          ++i;
        }
        if (i < table->get_num_transforms()) {
          LMatrix4 mat;
          table->get_transform(i)->mult_matrix(mat, _internal_transform->get_mat());
          glMatrixMode(GL_MODELVIEW1_ARB);
          GLPf(LoadMatrix)(mat.get_data());
          ++i;
        }
        while (i < table->get_num_transforms()) {
          LMatrix4 mat;
          table->get_transform(i)->mult_matrix(mat, _internal_transform->get_mat());
          glMatrixMode(GL_MODELVIEW2_ARB + i - 2);
          GLPf(LoadMatrix)(mat.get_data());
          ++i;
        }

        // Setting the GL_MODELVIEW0 matrix steps on the world matrix,
        // so we have to set a flag to reload the world matrix later.
        _transform_stale = true;
      }
    }
    _vertex_blending_enabled = true;

  } else {
    // We're not using vertex blending.
    if (_vertex_blending_enabled) {
      glDisable(GL_VERTEX_BLEND_ARB);
      if (_supports_matrix_palette) {
        glDisable(GL_MATRIX_PALETTE_ARB);
      }
      _vertex_blending_enabled = false;
    }

    if (_transform_stale) {
      glMatrixMode(GL_MODELVIEW);
      GLPf(LoadMatrix)(_internal_transform->get_mat().get_data());
    }
  }
#endif

#ifndef OPENGLES_2
  if (_data_reader->is_vertex_transformed()) {
    // If the vertex data claims to be already transformed into clip
    // coordinates, wipe out the current projection and modelview
    // matrix (so we don't attempt to transform it again).
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
  }
#endif

#ifndef OPENGLES  // Display lists not supported by OpenGL ES.
  if (geom_reader->get_usage_hint() == Geom::UH_static &&
      _data_reader->get_usage_hint() == Geom::UH_static &&
      display_lists && (!hardware_animation || display_list_animation)) {
    // If the geom claims to be totally static, try to build it into
    // a display list.

    // Before we compile or call a display list, make sure the current
    // buffers are unbound, or the nVidia drivers may crash.
    unbind_buffers();

    GeomContext *gc = ((Geom *)geom_reader->get_object())->prepare_now(get_prepared_objects(), this);
    nassertr(gc != (GeomContext *)NULL, false);
    CLP(GeomContext) *ggc = DCAST(CLP(GeomContext), gc);
    const CLP(GeomMunger) *gmunger = DCAST(CLP(GeomMunger), _munger);

    UpdateSeq modified = max(geom_reader->get_modified(), _data_reader->get_modified());
    if (ggc->get_display_list(_geom_display_list, gmunger, modified)) {
      // If it hasn't been modified, just play the display list again.
      if (GLCAT.is_spam()) {
        GLCAT.spam()
          << "calling display list " << (int)_geom_display_list << "\n";
      }

      glCallList(_geom_display_list);
#ifdef DO_PSTATS
      _vertices_display_list_pcollector.add_level(ggc->_num_verts);
      _primitive_batches_display_list_pcollector.add_level(1);
#endif

      // And now we don't need to do anything else for this geom.
      _geom_display_list = 0;
      end_draw_primitives();
      return false;
    }

    // Since we start this collector explicitly, we have to be sure to
    // stop it again.
    _load_display_list_pcollector.start();

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "compiling display list " << (int)_geom_display_list << "\n";
    }

    // If it has been modified, or this is the first time, then we
    // need to build the display list up.
    if (gl_compile_and_execute) {
      glNewList(_geom_display_list, GL_COMPILE_AND_EXECUTE);
    } else {
      glNewList(_geom_display_list, GL_COMPILE);
    }

#ifdef DO_PSTATS
    // Count up the number of vertices used by primitives in the Geom,
    // for PStats reporting.
    ggc->_num_verts = 0;
    for (int i = 0; i < geom_reader->get_num_primitives(); i++) {
      ggc->_num_verts += geom_reader->get_primitive(i)->get_num_vertices();
    }
#endif
  }
#endif  // OPENGLES

  // Enable the appropriate vertex arrays, and disable any
  // extra vertex arrays used by the previous rendering mode.
#ifdef SUPPORT_IMMEDIATE_MODE
  _use_sender = !vertex_arrays;
#endif

  {
    //PStatGPUTimer timer(this, _vertex_array_update_pcollector);
#ifdef OPENGLES_1
    if (!update_standard_vertex_arrays(force)) {
      return false;
    }
#else
    if (_current_shader_context == 0) {
      // No shader.
      if (_vertex_array_shader_context != 0) {
        _vertex_array_shader_context->disable_shader_vertex_arrays();
      }
      if (!update_standard_vertex_arrays(force)) {
        return false;
      }
    } else {
      // Shader.
      if (_vertex_array_shader_context == 0 || _vertex_array_shader_context->uses_standard_vertex_arrays()) {
        // Previous shader used standard arrays.
        if (_current_shader_context->uses_standard_vertex_arrays()) {
          // So does the current, so update them.
          if (!update_standard_vertex_arrays(force)) {
            return false;
          }
        } else {
          // The current shader does not, so disable them entirely.
          disable_standard_vertex_arrays();
        }
      }
      if (_current_shader_context->uses_custom_vertex_arrays()) {
        // The current shader also uses custom vertex arrays.
        if (!_current_shader_context->
            update_shader_vertex_arrays(_vertex_array_shader_context, force)) {
          return false;
        }
      } else {
        _vertex_array_shader_context->disable_shader_vertex_arrays();
      }
    }

    _vertex_array_shader = _current_shader;
    _vertex_array_shader_context = _current_shader_context;
#endif  // OPENGLES_1
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::update_standard_vertex_arrays
//       Access: Protected
//  Description: Disables any unneeded vertex arrays that
//               were previously enabled, and enables any vertex
//               arrays that are needed that were not previously
//               enabled (or, sets up an immediate-mode sender).
//               Called only from begin_draw_primitives.
//               Used only when the standard (non-shader) pipeline
//               is about to be used - glShaderContexts are responsible
//               for setting up their own vertex arrays.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
update_standard_vertex_arrays(bool force) {
#ifndef OPENGLES_2
  const GeomVertexAnimationSpec &animation =
    _data_reader->get_format()->get_animation();
  bool hardware_animation = (animation.get_animation_type() == Geom::AT_hardware);
#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    // We must use immediate mode to render primitives.
    _sender.clear();

    _sender.add_column(_data_reader, InternalName::get_normal(),
                       NULL, NULL, GLPf(Normal3), NULL);
#ifndef NDEBUG
    if (_show_texture_usage) {
      // In show_texture_usage mode, all colors are white, so as not
      // to contaminate the texture color.
      GLPf(Color4)(1.0f, 1.0f, 1.0f, 1.0f);
    } else
#endif // NDEBUG
      if (!_sender.add_column(_data_reader, InternalName::get_color(),
                              NULL, NULL, GLPf(Color3), GLPf(Color4))) {
        // If we didn't have a color column, the item color is white.
        GLPf(Color4)(1.0f, 1.0f, 1.0f, 1.0f);
      }

    // Now set up each of the active texture coordinate stages--or at
    // least those for which we're not generating texture coordinates
    // automatically.
    int max_stage_index = _target_texture->get_num_on_ff_stages();
    int stage_index = 0;
    while (stage_index < max_stage_index) {
      TextureStage *stage = _target_texture->get_on_ff_stage(stage_index);
      if (!_target_tex_gen->has_gen_texcoord_stage(stage)) {
        // This stage is not one of the stages that doesn't need
        // texcoords issued for it.
        const InternalName *name = stage->get_texcoord_name();
        if (stage_index == 0) {
          // Use the original functions for stage 0, in case we don't
          // support multitexture.
          _sender.add_column(_data_reader, name,
                             GLPf(TexCoord1), GLPf(TexCoord2),
                             GLPf(TexCoord3), GLPf(TexCoord4));

        } else {
          // Other stages require the multitexture functions.
          _sender.add_texcoord_column(_data_reader, name, stage_index,
                                      GLf(_glMultiTexCoord1), GLf(_glMultiTexCoord2),
                                      GLf(_glMultiTexCoord3), GLf(_glMultiTexCoord4));
        }
      }

      ++stage_index;
    }

    // Be sure also to disable any texture stages we had enabled before.
    while (stage_index < _last_max_stage_index) {
      _glClientActiveTexture(GL_TEXTURE0 + stage_index);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      ++stage_index;
    }
    _last_max_stage_index = max_stage_index;

    if (_supports_vertex_blend) {
      if (hardware_animation) {
        // Issue the weights and/or transform indices for vertex blending.
        _sender.add_vector_column(_data_reader, InternalName::get_transform_weight(),
                                  GLfv(_glWeight));

        if (animation.get_indexed_transforms()) {
          // Issue the matrix palette indices.
          _sender.add_vector_uint_column(_data_reader, InternalName::get_transform_index(),
                                         _glMatrixIndexuiv);
        }
      }
    }

    // We must add vertex last, because glVertex3f() is the key
    // function call that actually issues the vertex.
    _sender.add_column(_data_reader, InternalName::get_vertex(),
                       NULL, GLPf(Vertex2), GLPf(Vertex3), GLPf(Vertex4));

  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    // We may use vertex arrays or buffers to render primitives.
    const GeomVertexArrayDataHandle *array_reader;
    const unsigned char *client_pointer;
    int num_values;
    Geom::NumericType numeric_type;
    int start;
    int stride;

    if (_data_reader->get_normal_info(array_reader, numeric_type,
                                      start, stride)) {
      if (!setup_array_data(client_pointer, array_reader, force)) {
        return false;
      }
      glNormalPointer(get_numeric_type(numeric_type), stride,
                      client_pointer + start);
      glEnableClientState(GL_NORMAL_ARRAY);
    } else {
      glDisableClientState(GL_NORMAL_ARRAY);
    }

#ifndef NDEBUG
    if (_show_texture_usage) {
      // In show_texture_usage mode, all colors are white, so as not
      // to contaminate the texture color.
      glDisableClientState(GL_COLOR_ARRAY);
      GLPf(Color4)(1.0f, 1.0f, 1.0f, 1.0f);
    } else
#endif // NDEBUG
      if (_data_reader->get_color_info(array_reader, num_values, numeric_type,
                                       start, stride)) {
        if (!setup_array_data(client_pointer, array_reader, force)) {
          return false;
        }
        if (numeric_type == Geom::NT_packed_dabc) {
          glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE,
                         stride, client_pointer + start);
        } else {
          glColorPointer(num_values, get_numeric_type(numeric_type),
                         stride, client_pointer + start);
        }
        glEnableClientState(GL_COLOR_ARRAY);
      } else {
        glDisableClientState(GL_COLOR_ARRAY);

        // Since we don't have per-vertex color, the implicit color is
        // white.
        GLPf(Color4)(1.0f, 1.0f, 1.0f, 1.0f);
      }

    // Now set up each of the active texture coordinate stages--or at
    // least those for which we're not generating texture coordinates
    // automatically.
    int max_stage_index = _target_texture->get_num_on_ff_stages();
    int stage_index = 0;
    while (stage_index < max_stage_index) {
      _glClientActiveTexture(GL_TEXTURE0 + stage_index);
      TextureStage *stage = _target_texture->get_on_ff_stage(stage_index);
      if (!_target_tex_gen->has_gen_texcoord_stage(stage)) {
        // This stage is not one of the stages that doesn't need
        // texcoords issued for it.
        const InternalName *name = stage->get_texcoord_name();

        if (_data_reader->get_array_info(name, array_reader, num_values,
                                         numeric_type, start, stride)) {
          // The vertex data does have texcoords for this stage.
          if (!setup_array_data(client_pointer, array_reader, force)) {
            return false;
          }
          glTexCoordPointer(num_values, get_numeric_type(numeric_type),
                               stride, client_pointer + start);
          glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        } else {
          // The vertex data doesn't have texcoords for this stage (even
          // though they're needed).
          glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
      } else {
        // No texcoords are needed for this stage.
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      }

      ++stage_index;
    }

    // Be sure also to disable any texture stages we had enabled before.
    while (stage_index < _last_max_stage_index) {
      _glClientActiveTexture(GL_TEXTURE0 + stage_index);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      ++stage_index;
    }
    _last_max_stage_index = max_stage_index;

#ifndef OPENGLES
    if (_supports_vertex_blend) {
      if (hardware_animation) {
        // Issue the weights and/or transform indices for vertex blending.
        if (_data_reader->get_array_info(InternalName::get_transform_weight(),
                                         array_reader, num_values, numeric_type,
                                         start, stride)) {
          if (!setup_array_data(client_pointer, array_reader, force)) {
            return false;
          }
          _glWeightPointer(num_values, get_numeric_type(numeric_type),
                           stride, client_pointer + start);
          glEnableClientState(GL_WEIGHT_ARRAY_ARB);
        } else {
          glDisableClientState(GL_WEIGHT_ARRAY_ARB);
        }

        if (animation.get_indexed_transforms()) {
          // Issue the matrix palette indices.
          if (_data_reader->get_array_info(InternalName::get_transform_index(),
                                           array_reader, num_values, numeric_type,
                                           start, stride)) {
            if (!setup_array_data(client_pointer, array_reader, force)) {
              return false;
            }
            _glMatrixIndexPointer(num_values, get_numeric_type(numeric_type),
                                  stride, client_pointer + start);
            glEnableClientState(GL_MATRIX_INDEX_ARRAY_ARB);
          } else {
            glDisableClientState(GL_MATRIX_INDEX_ARRAY_ARB);
          }
        }

      } else {
        glDisableClientState(GL_WEIGHT_ARRAY_ARB);
        if (_supports_matrix_palette) {
          glDisableClientState(GL_MATRIX_INDEX_ARRAY_ARB);
        }
      }
    }
#endif

    // There's no requirement that we add vertices last, but we do
    // anyway.
    if (_data_reader->get_vertex_info(array_reader, num_values, numeric_type,
                                      start, stride)) {
      if (!setup_array_data(client_pointer, array_reader, force)) {
        return false;
      }
      glVertexPointer(num_values, get_numeric_type(numeric_type),
                         stride, client_pointer + start);
      glEnableClientState(GL_VERTEX_ARRAY);
    }
  }
#endif  // OPENGLES_2
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::unbind_buffers
//       Access: Protected
//  Description: Ensures the vertex and array buffers are no longer
//               bound.  Some graphics drivers crash if these are left
//               bound indiscriminantly.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
unbind_buffers() {
  if (_current_vbuffer_index != 0) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "unbinding vertex buffer\n";
    }
    _glBindBuffer(GL_ARRAY_BUFFER, 0);
    _current_vbuffer_index = 0;
  }
  if (_current_ibuffer_index != 0) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "unbinding index buffer\n";
    }
    _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    _current_ibuffer_index = 0;
  }

  disable_standard_vertex_arrays();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::disable_standard_vertex_arrays
//       Access: Protected
//  Description: Used to disable all the standard vertex arrays that
//               are currently enabled.  glShaderContexts are
//               responsible for setting up their own vertex arrays,
//               but before they can do so, the standard vertex
//               arrays need to be disabled to get them "out of the
//               way."  Called only from begin_draw_primitives.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
disable_standard_vertex_arrays() {
#ifndef OPENGLES_2
#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) return;
#endif

  glDisableClientState(GL_NORMAL_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  GLPf(Color4)(1.0f, 1.0f, 1.0f, 1.0f);

  for (int stage_index=0; stage_index < _last_max_stage_index; stage_index++) {
    _glClientActiveTexture(GL_TEXTURE0 + stage_index);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  }
  _last_max_stage_index = 0;

#ifndef OPENGLES
  if (_supports_vertex_blend) {
    glDisableClientState(GL_WEIGHT_ARRAY_ARB);
    if (_supports_matrix_palette) {
      glDisableClientState(GL_MATRIX_INDEX_ARRAY_ARB);
    }
  }
#endif

  glDisableClientState(GL_VERTEX_ARRAY);
  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_triangles
//       Access: Public, Virtual
//  Description: Draws a series of disconnected triangles.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_triangles(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());

#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_triangles: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_simple_primitives(reader, GL_TRIANGLES);

  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    int num_vertices = reader->get_num_vertices();
    _vertices_tri_pcollector.add_level(num_vertices);
    _primitive_batches_tri_pcollector.add_level(1);

    if (reader->is_indexed()) {
      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }

#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawElementsInstanced(GL_TRIANGLES, num_vertices,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer, _instance_count);
      } else
#endif
      {
        _glDrawRangeElements(GL_TRIANGLES,
                             reader->get_min_vertex(),
                             reader->get_max_vertex(),
                             num_vertices,
                             get_numeric_type(reader->get_index_type()),
                             client_pointer);
      }
    } else {
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawArraysInstanced(GL_TRIANGLES,
                               reader->get_first_vertex(),
                               num_vertices, _instance_count);
      } else
#endif
      {
        glDrawArrays(GL_TRIANGLES,
                        reader->get_first_vertex(),
                        num_vertices);
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_tristrips
//       Access: Public, Virtual
//  Description: Draws a series of triangle strips.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_tristrips(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());

  report_my_gl_errors();

#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_tristrips: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_composite_primitives(reader, GL_TRIANGLE_STRIP);

  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    if (connect_triangle_strips && _render_mode != RenderModeAttrib::M_wireframe) {
      // One long triangle strip, connected by the degenerate vertices
      // that have already been set up within the primitive.
      int num_vertices = reader->get_num_vertices();
      _vertices_tristrip_pcollector.add_level(num_vertices);
      _primitive_batches_tristrip_pcollector.add_level(1);
      if (reader->is_indexed()) {
        const unsigned char *client_pointer;
        if (!setup_primitive(client_pointer, reader, force)) {
          return false;
        }
#ifndef OPENGLES
        if (_supports_geometry_instancing && _instance_count > 0) {
          _glDrawElementsInstanced(GL_TRIANGLE_STRIP, num_vertices,
                                   get_numeric_type(reader->get_index_type()),
                                   client_pointer, _instance_count);
        } else
#endif
        {
          _glDrawRangeElements(GL_TRIANGLE_STRIP,
                               reader->get_min_vertex(),
                               reader->get_max_vertex(),
                               num_vertices,
                               get_numeric_type(reader->get_index_type()),
                               client_pointer);
        }
      } else {
#ifndef OPENGLES
        if (_supports_geometry_instancing && _instance_count > 0) {
          _glDrawArraysInstanced(GL_TRIANGLE_STRIP,
                                 reader->get_first_vertex(),
                                 num_vertices, _instance_count);
        } else
#endif
        {
          glDrawArrays(GL_TRIANGLE_STRIP,
                          reader->get_first_vertex(),
                          num_vertices);
        }
      }

    } else {
      // Send the individual triangle strips, stepping over the
      // degenerate vertices.
      CPTA_int ends = reader->get_ends();

      _primitive_batches_tristrip_pcollector.add_level(ends.size());
      if (reader->is_indexed()) {
        const unsigned char *client_pointer;
        if (!setup_primitive(client_pointer, reader, force)) {
          return false;
        }
        int index_stride = reader->get_index_stride();
        GeomVertexReader mins(reader->get_mins(), 0);
        GeomVertexReader maxs(reader->get_maxs(), 0);
        nassertr(reader->get_mins()->get_num_rows() == (int)ends.size() &&
                 reader->get_maxs()->get_num_rows() == (int)ends.size(), false);

        unsigned int start = 0;
        for (size_t i = 0; i < ends.size(); i++) {
          _vertices_tristrip_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
          if (_supports_geometry_instancing && _instance_count > 0) {
            _glDrawElementsInstanced(GL_TRIANGLE_STRIP, ends[i] - start,
                                     get_numeric_type(reader->get_index_type()),
                                     client_pointer + start * index_stride,
                                     _instance_count);
          } else
#endif
          {
            _glDrawRangeElements(GL_TRIANGLE_STRIP,
                                 mins.get_data1i(), maxs.get_data1i(),
                                 ends[i] - start,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer + start * index_stride);
          }
          start = ends[i] + 2;
        }
      } else {
        unsigned int start = 0;
        int first_vertex = reader->get_first_vertex();
        for (size_t i = 0; i < ends.size(); i++) {
          _vertices_tristrip_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
          if (_supports_geometry_instancing && _instance_count > 0) {
            _glDrawArraysInstanced(GL_TRIANGLE_STRIP, first_vertex + start,
                                   ends[i] - start, _instance_count);
          } else
#endif
          {
            glDrawArrays(GL_TRIANGLE_STRIP, first_vertex + start,
                            ends[i] - start);
          }
          start = ends[i] + 2;
        }
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_trifans
//       Access: Public, Virtual
//  Description: Draws a series of triangle fans.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_trifans(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());
#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_trifans: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_composite_primitives(reader, GL_TRIANGLE_FAN);
  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    // Send the individual triangle fans.  There's no connecting fans
    // with degenerate vertices, so no worries about that.
    CPTA_int ends = reader->get_ends();

    _primitive_batches_trifan_pcollector.add_level(ends.size());
    if (reader->is_indexed()) {
      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }
      int index_stride = reader->get_index_stride();
      GeomVertexReader mins(reader->get_mins(), 0);
      GeomVertexReader maxs(reader->get_maxs(), 0);
      nassertr(reader->get_mins()->get_num_rows() == (int)ends.size() &&
               reader->get_maxs()->get_num_rows() == (int)ends.size(), false);

      unsigned int start = 0;
      for (size_t i = 0; i < ends.size(); i++) {
        _vertices_trifan_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
        if (_supports_geometry_instancing && _instance_count > 0) {
          _glDrawElementsInstanced(GL_TRIANGLE_FAN, ends[i] - start,
                                   get_numeric_type(reader->get_index_type()),
                                   client_pointer + start * index_stride,
                                   _instance_count);
        } else
#endif
        {
          _glDrawRangeElements(GL_TRIANGLE_FAN,
                               mins.get_data1i(), maxs.get_data1i(), ends[i] - start,
                               get_numeric_type(reader->get_index_type()),
                               client_pointer + start * index_stride);
        }
        start = ends[i];
      }
    } else {
      unsigned int start = 0;
      int first_vertex = reader->get_first_vertex();
      for (size_t i = 0; i < ends.size(); i++) {
        _vertices_trifan_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
        if (_supports_geometry_instancing && _instance_count > 0) {
          _glDrawArraysInstanced(GL_TRIANGLE_FAN, first_vertex + start,
                                 ends[i] - start, _instance_count);
        } else
#endif
        {
          glDrawArrays(GL_TRIANGLE_FAN, first_vertex + start,
                          ends[i] - start);
        }
        start = ends[i];
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_patches
//       Access: Public, Virtual
//  Description: Draws a series of "patches", which can only be
//               processed by a tessellation shader.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_patches(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());

#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_patches: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

  if (!get_supports_tessellation_shaders()) {
    return false;
  }

#ifndef OPENGLES
  _glPatchParameteri(GL_PATCH_VERTICES, reader->get_object()->get_num_vertices_per_primitive());

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_simple_primitives(reader, GL_PATCHES);

  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    int num_vertices = reader->get_num_vertices();
    _vertices_patch_pcollector.add_level(num_vertices);
    _primitive_batches_patch_pcollector.add_level(1);

    if (reader->is_indexed()) {
      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }

#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawElementsInstanced(GL_PATCHES, num_vertices,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer, _instance_count);
      } else
#endif
      {
        _glDrawRangeElements(GL_PATCHES,
                             reader->get_min_vertex(),
                             reader->get_max_vertex(),
                             num_vertices,
                             get_numeric_type(reader->get_index_type()),
                             client_pointer);
      }
    } else {
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawArraysInstanced(GL_PATCHES,
                               reader->get_first_vertex(),
                               num_vertices, _instance_count);
      } else
#endif
      {
        glDrawArrays(GL_PATCHES,
                        reader->get_first_vertex(),
                        num_vertices);
      }
    }
  }

#endif // OPENGLES

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_lines
//       Access: Public, Virtual
//  Description: Draws a series of disconnected line segments.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_lines(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());
#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_lines: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_simple_primitives(reader, GL_LINES);
  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    int num_vertices = reader->get_num_vertices();
    _vertices_other_pcollector.add_level(num_vertices);
    _primitive_batches_other_pcollector.add_level(1);

    if (reader->is_indexed()) {
      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawElementsInstanced(GL_LINES, num_vertices,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer, _instance_count);
      } else
#endif
      {
        _glDrawRangeElements(GL_LINES,
                             reader->get_min_vertex(),
                             reader->get_max_vertex(),
                             num_vertices,
                             get_numeric_type(reader->get_index_type()),
                             client_pointer);
      }
    } else {
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawArraysInstanced(GL_LINES,
                               reader->get_first_vertex(),
                               num_vertices, _instance_count);
      } else
#endif
      {
        glDrawArrays(GL_LINES,
                        reader->get_first_vertex(),
                        num_vertices);
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_linestrips
//       Access: Public, Virtual
//  Description: Draws a series of line strips.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_linestrips(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());

  report_my_gl_errors();

#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_linestrips: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_composite_primitives(reader, GL_LINE_STRIP);

  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    if (reader->is_indexed() &&
        (_supported_geom_rendering & GeomEnums::GR_strip_cut_index) != 0) {
      // One long triangle strip, connected by strip cut indices.
#ifndef OPENGLES
      if (_glPrimitiveRestartIndex != NULL) {
        _glPrimitiveRestartIndex(reader->get_strip_cut_index());
      }
#endif

      int num_vertices = reader->get_num_vertices();
      _vertices_other_pcollector.add_level(num_vertices);
      _primitive_batches_other_pcollector.add_level(1);

      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawElementsInstanced(GL_LINE_STRIP, num_vertices,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer, _instance_count);
      } else
#endif
      {
        _glDrawRangeElements(GL_LINE_STRIP,
                             reader->get_min_vertex(),
                             reader->get_max_vertex(),
                             num_vertices,
                             get_numeric_type(reader->get_index_type()),
                             client_pointer);
      }
    } else {
      // Send the individual line strips, stepping over the
      // strip-cut indices.
      CPTA_int ends = reader->get_ends();

      _primitive_batches_other_pcollector.add_level(ends.size());
      if (reader->is_indexed()) {
        const unsigned char *client_pointer;
        if (!setup_primitive(client_pointer, reader, force)) {
          return false;
        }
        int index_stride = reader->get_index_stride();
        GeomVertexReader mins(reader->get_mins(), 0);
        GeomVertexReader maxs(reader->get_maxs(), 0);
        nassertr(reader->get_mins()->get_num_rows() == (int)ends.size() &&
                 reader->get_maxs()->get_num_rows() == (int)ends.size(), false);

        unsigned int start = 0;
        for (size_t i = 0; i < ends.size(); i++) {
          _vertices_other_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
          if (_supports_geometry_instancing && _instance_count > 0) {
            _glDrawElementsInstanced(GL_LINE_STRIP, ends[i] - start,
                                     get_numeric_type(reader->get_index_type()),
                                     client_pointer + start * index_stride,
                                     _instance_count);
          } else
#endif
          {
            _glDrawRangeElements(GL_LINE_STRIP,
                                 mins.get_data1i(), maxs.get_data1i(),
                                 ends[i] - start,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer + start * index_stride);
          }
          start = ends[i] + 1;
        }
      } else {
        unsigned int start = 0;
        int first_vertex = reader->get_first_vertex();
        for (size_t i = 0; i < ends.size(); i++) {
          _vertices_other_pcollector.add_level(ends[i] - start);
#ifndef OPENGLES
          if (_supports_geometry_instancing && _instance_count > 0) {
            _glDrawArraysInstanced(GL_LINE_STRIP, first_vertex + start,
                                   ends[i] - start, _instance_count);
          } else
#endif
          {
            glDrawArrays(GL_LINE_STRIP, first_vertex + start,
                            ends[i] - start);
          }
          start = ends[i] + 1;
        }
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_points
//       Access: Public, Virtual
//  Description: Draws a series of disconnected points.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
draw_points(const GeomPrimitivePipelineReader *reader, bool force) {
  PStatGPUTimer timer(this, _draw_primitive_pcollector, reader->get_current_thread());
#ifndef NDEBUG
  if (GLCAT.is_spam()) {
    GLCAT.spam() << "draw_points: " << *(reader->get_object()) << "\n";
  }
#endif  // NDEBUG

#ifdef SUPPORT_IMMEDIATE_MODE
  if (_use_sender) {
    draw_immediate_simple_primitives(reader, GL_POINTS);
  } else
#endif  // SUPPORT_IMMEDIATE_MODE
  {
    int num_vertices = reader->get_num_vertices();
    _vertices_other_pcollector.add_level(num_vertices);
    _primitive_batches_other_pcollector.add_level(1);

    if (reader->is_indexed()) {
      const unsigned char *client_pointer;
      if (!setup_primitive(client_pointer, reader, force)) {
        return false;
      }
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawElementsInstanced(GL_POINTS, num_vertices,
                                 get_numeric_type(reader->get_index_type()),
                                 client_pointer, _instance_count);
      } else
#endif
      {
        _glDrawRangeElements(GL_POINTS,
                             reader->get_min_vertex(),
                             reader->get_max_vertex(),
                             num_vertices,
                             get_numeric_type(reader->get_index_type()),
                             client_pointer);
      }
    } else {
#ifndef OPENGLES
      if (_supports_geometry_instancing && _instance_count > 0) {
        _glDrawArraysInstanced(GL_POINTS,
                               reader->get_first_vertex(),
                               num_vertices, _instance_count);
      } else
#endif
      {
        glDrawArrays(GL_POINTS,
                        reader->get_first_vertex(),
                        num_vertices);
      }
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_draw_primitives()
//       Access: Public, Virtual
//  Description: Called after a sequence of draw_primitive()
//               functions are called, this should do whatever cleanup
//               is appropriate.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_draw_primitives() {
#ifndef OPENGLES  // Display lists not supported by OpenGL ES.
  if (_geom_display_list != 0) {
    // If we were building a display list, close it now.
    glEndList();
    _load_display_list_pcollector.stop();

    if (!gl_compile_and_execute) {
      glCallList(_geom_display_list);
    }
    _primitive_batches_display_list_pcollector.add_level(1);
  }
  _geom_display_list = 0;

  // Clean up the vertex blending state.
  if (_vertex_blending_enabled) {
    glDisable(GL_VERTEX_BLEND_ARB);
    if (_supports_matrix_palette) {
      glDisable(GL_MATRIX_PALETTE_ARB);
    }
    _vertex_blending_enabled = false;
  }
#endif

#ifndef OPENGLES_2
  if (_transform_stale) {
    glMatrixMode(GL_MODELVIEW);
    GLPf(LoadMatrix)(_internal_transform->get_mat().get_data());
  }

  if (_data_reader->is_vertex_transformed()) {
    // Restore the matrices that we pushed above.
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
  }
#endif

  GraphicsStateGuardian::end_draw_primitives();
  maybe_gl_finish();
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_memory_barrier
//       Access: Public
//  Description: Issues the given memory barriers, and clears the
//               list of textures marked as incoherent for the given
//               bits.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
issue_memory_barrier(GLbitfield barriers) {
#ifndef OPENGLES
  if (!gl_enable_memory_barriers || _glMemoryBarrier == NULL) {
    return;
  }

  PStatGPUTimer timer(this, _memory_barrier_pcollector);

  if (GLCAT.is_debug()) {
    GLCAT.debug() << "Issuing memory barriers:";
  }

  _glMemoryBarrier(barriers);

  // Indicate that barriers no longer need to be issued for
  // the relevant lists of textures.
  if (barriers & GL_TEXTURE_FETCH_BARRIER_BIT) {
    _textures_needing_fetch_barrier.clear();
    GLCAT.debug(false) << " texture_fetch";
  }

  if (barriers & GL_SHADER_IMAGE_ACCESS_BARRIER_BIT) {
    _textures_needing_image_access_barrier.clear();
    GLCAT.debug(false) << " shader_image_access";
  }

  if (barriers & GL_TEXTURE_UPDATE_BARRIER_BIT) {
    _textures_needing_update_barrier.clear();
    GLCAT.debug(false) << " texture_update";
  }

  if (barriers & GL_FRAMEBUFFER_BARRIER_BIT) {
    _textures_needing_framebuffer_barrier.clear();
    GLCAT.debug(false) << " framebuffer";
  }

  GLCAT.debug(false) << "\n";

  report_my_gl_errors();
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_texture
//       Access: Public, Virtual
//  Description: Creates whatever structures the GSG requires to
//               represent the texture internally, and returns a
//               newly-allocated TextureContext object with this data.
//               It is the responsibility of the calling function to
//               later call release_texture() with this same pointer
//               (which will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a texture.  Instead, call Texture::prepare().
////////////////////////////////////////////////////////////////////
TextureContext *CLP(GraphicsStateGuardian)::
prepare_texture(Texture *tex, int view) {
  PStatGPUTimer timer(this, _prepare_texture_pcollector);

  report_my_gl_errors();
  // Make sure we'll support this texture when it's rendered.  Don't
  // bother to prepare it if we won't.
  switch (tex->get_texture_type()) {
  case Texture::TT_3d_texture:
    if (!_supports_3d_texture) {
      GLCAT.warning()
        << "3-D textures are not supported by this OpenGL driver.\n";
      return NULL;
    }
    break;

  case Texture::TT_2d_texture_array:
    if (!_supports_2d_texture_array) {
      GLCAT.warning()
        << "2-D texture arrays are not supported by this OpenGL driver.\n";
      return NULL;
    }
    break;

  case Texture::TT_cube_map:
    if (!_supports_cube_map) {
      GLCAT.warning()
        << "Cube map textures are not supported by this OpenGL driver.\n";
      return NULL;
    }

  default:
    break;
  }

  CLP(TextureContext) *gtc = new CLP(TextureContext)(this, _prepared_objects, tex, view);
  report_my_gl_errors();

  return gtc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::update_texture
//       Access: Public, Virtual
//  Description: Ensures that the current Texture data is refreshed
//               onto the GSG.  This means updating the texture
//               properties and/or re-uploading the texture image, if
//               necessary.  This should only be called within the
//               draw thread.
//
//               If force is true, this function will not return until
//               the texture has been fully uploaded.  If force is
//               false, the function may choose to upload a simple
//               version of the texture instead, if the texture is not
//               fully resident (and if get_incomplete_render() is
//               true).
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
update_texture(TextureContext *tc, bool force) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

  if (gtc->was_image_modified() || !gtc->_has_storage) {
    PStatGPUTimer timer(this, _texture_update_pcollector);

    // If the texture image was modified, reload the texture.
    apply_texture(tc);
    if (gtc->was_properties_modified()) {
      specify_texture(gtc);
    }
    bool okflag = upload_texture(gtc, force);
    if (!okflag) {
      GLCAT.error()
        << "Could not load " << *gtc->get_texture() << "\n";
      return false;
    }

  } else if (gtc->was_properties_modified()) {
    PStatGPUTimer timer(this, _texture_update_pcollector);

    // If only the properties have been modified, we don't necessarily
    // need to reload the texture.
    apply_texture(tc);
    if (specify_texture(gtc)) {
      // Actually, looks like the texture *does* need to be reloaded.
      gtc->mark_needs_reload();
      bool okflag = upload_texture(gtc, force);
      if (!okflag) {
        GLCAT.error()
          << "Could not load " << *gtc->get_texture() << "\n";
        return false;
      }

    } else {
      // The texture didn't need reloading, but mark it fully updated
      // now.
      gtc->mark_loaded();
    }
  }

  gtc->enqueue_lru(&_prepared_objects->_graphics_memory_lru);

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_texture
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               texture.  This function should never be called
//               directly; instead, call Texture::release() (or simply
//               let the Texture destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_texture(TextureContext *tc) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);
  delete gtc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::extract_texture_data
//       Access: Public, Virtual
//  Description: This method should only be called by the
//               GraphicsEngine.  Do not call it directly; call
//               GraphicsEngine::extract_texture_data() instead.
//
//               This method will be called in the draw thread to
//               download the texture memory's image into its
//               ram_image value.  It returns true on success, false
//               otherwise.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
extract_texture_data(Texture *tex) {
  bool success = true;
  // Make sure the error stack is cleared out before we begin.
  report_my_gl_errors();

  int num_views = tex->get_num_views();
  for (int view = 0; view < num_views; ++view) {
    TextureContext *tc = tex->prepare_now(view, get_prepared_objects(), this);
    nassertr(tc != (TextureContext *)NULL, false);
    CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

    if (!do_extract_texture_data(gtc)) {
      success = false;
    }
  }

  return success;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_geom
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given geom, and returns a newly-allocated
//               GeomContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_geom() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a geom.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
GeomContext *CLP(GraphicsStateGuardian)::
prepare_geom(Geom *geom) {
  PStatGPUTimer timer(this, _prepare_geom_pcollector);
  return new CLP(GeomContext)(geom);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_geom
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               geom.  This function should never be called
//               directly; instead, call Geom::release() (or simply
//               let the Geom destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_geom(GeomContext *gc) {
  CLP(GeomContext) *ggc = DCAST(CLP(GeomContext), gc);
  ggc->release_display_lists();
  report_my_gl_errors();

  delete ggc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_shader
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
ShaderContext *CLP(GraphicsStateGuardian)::
prepare_shader(Shader *se) {
  PStatGPUTimer timer(this, _prepare_shader_pcollector);

#ifndef OPENGLES_1
  ShaderContext *result = NULL;

  switch (se->get_language()) {
  case Shader::SL_GLSL:
    result = new CLP(ShaderContext)(this, se);
    break;

#if defined(HAVE_CG) && !defined(OPENGLES)
  case Shader::SL_Cg:
    result = new CLP(CgShaderContext)(this, se);
    break;
#endif

  default:
    GLCAT.error()
      << "Tried to load shader with unsupported shader language!\n";
    return NULL;
  }

  if (result->valid()) {
    return result;
  }

  delete result;
#endif  // OPENGLES_1

  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_shader
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_shader(ShaderContext *sc) {
  delete sc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::record_deleted_display_list
//       Access: Public
//  Description: This is intended to be called only from the
//               GLGeomContext destructor.  It saves the indicated
//               display list index in the list to be deleted at the
//               end of the frame.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
record_deleted_display_list(GLuint index) {
  LightMutexHolder holder(_lock);
  _deleted_display_lists.push_back(index);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_vertex_buffer
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given data, and returns a newly-allocated
//               VertexBufferContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_vertex_buffer() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a buffer.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
VertexBufferContext *CLP(GraphicsStateGuardian)::
prepare_vertex_buffer(GeomVertexArrayData *data) {
  if (_supports_buffers) {
    PStatGPUTimer timer(this, _prepare_vertex_buffer_pcollector);

    CLP(VertexBufferContext) *gvbc = new CLP(VertexBufferContext)(this, _prepared_objects, data);
    _glGenBuffers(1, &gvbc->_index);

    if (GLCAT.is_debug() && gl_debug_buffers) {
      GLCAT.debug()
        << "creating vertex buffer " << (int)gvbc->_index << ": "
        << data->get_num_rows() << " vertices "
        << *data->get_array_format() << "\n";
    }

    report_my_gl_errors();
    apply_vertex_buffer(gvbc, data->get_handle(), false);
    return gvbc;
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_vertex_buffer
//       Access: Public
//  Description: Makes the data the currently available data for
//               rendering.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
apply_vertex_buffer(VertexBufferContext *vbc,
                    const GeomVertexArrayDataHandle *reader, bool force) {
  nassertr(_supports_buffers, false);
  if (reader->get_modified() == UpdateSeq::initial()) {
    // No need to re-apply.
    return true;
  }

  CLP(VertexBufferContext) *gvbc = DCAST(CLP(VertexBufferContext), vbc);

  if (_current_vbuffer_index != gvbc->_index) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "binding vertex buffer " << (int)gvbc->_index << "\n";
    }
    _glBindBuffer(GL_ARRAY_BUFFER, gvbc->_index);
    _current_vbuffer_index = gvbc->_index;
    gvbc->set_active(true);
  }

  if (gvbc->was_modified(reader)) {
    int num_bytes = reader->get_data_size_bytes();
    if (GLCAT.is_debug() && gl_debug_buffers) {
      GLCAT.debug()
        << "copying " << num_bytes
        << " bytes into vertex buffer " << (int)gvbc->_index << "\n";
    }
    if (num_bytes != 0) {
      const unsigned char *client_pointer = reader->get_read_pointer(force);
      if (client_pointer == NULL) {
        return false;
      }

      PStatGPUTimer timer(this, _load_vertex_buffer_pcollector, reader->get_current_thread());
      if (gvbc->changed_size(reader) || gvbc->changed_usage_hint(reader)) {
        _glBufferData(GL_ARRAY_BUFFER, num_bytes, client_pointer,
                      get_usage(reader->get_usage_hint()));

      } else {
        _glBufferSubData(GL_ARRAY_BUFFER, 0, num_bytes, client_pointer);
      }
      _data_transferred_pcollector.add_level(num_bytes);
    }

    gvbc->mark_loaded(reader);
  }
  gvbc->enqueue_lru(&_prepared_objects->_graphics_memory_lru);

  maybe_gl_finish();
  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_vertex_buffer
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               data.  This function should never be called
//               directly; instead, call Data::release() (or simply
//               let the Data destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_vertex_buffer(VertexBufferContext *vbc) {
  nassertv(_supports_buffers);

  CLP(VertexBufferContext) *gvbc = DCAST(CLP(VertexBufferContext), vbc);

  if (GLCAT.is_debug() && gl_debug_buffers) {
    GLCAT.debug()
      << "deleting vertex buffer " << (int)gvbc->_index << "\n";
  }

  // Make sure the buffer is unbound before we delete it.  Not
  // strictly necessary according to the OpenGL spec, but it might
  // help out a flaky driver, and we need to keep our internal state
  // consistent anyway.
  if (_current_vbuffer_index == gvbc->_index) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "unbinding vertex buffer\n";
    }
    _glBindBuffer(GL_ARRAY_BUFFER, 0);
    _current_vbuffer_index = 0;
  }

  _glDeleteBuffers(1, &gvbc->_index);
  report_my_gl_errors();

  gvbc->_index = 0;

  delete gvbc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::setup_array_data
//       Access: Public
//  Description: Internal function to bind a buffer object for the
//               indicated data array, if appropriate, or to unbind a
//               buffer object if it should be rendered from client
//               memory.
//
//               If the buffer object is bound, this function sets
//               client_pointer to NULL (representing the start of the
//               buffer object in server memory); if the buffer object
//               is not bound, this function sets client_pointer the
//               pointer to the data array in client memory, that is,
//               the data array passed in.
//
//               If force is not true, the function may return false
//               indicating the data is not currently available.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
setup_array_data(const unsigned char *&client_pointer,
                 const GeomVertexArrayDataHandle *array_reader,
                 bool force) {
  if (!_supports_buffers) {
    // No support for buffer objects; always render from client.
    client_pointer = array_reader->get_read_pointer(force);
    return (client_pointer != NULL);
  }
  if (!vertex_buffers || _geom_display_list != 0 ||
      array_reader->get_usage_hint() < gl_min_buffer_usage_hint) {
    // The array specifies client rendering only, or buffer objects
    // are configured off.
    if (_current_vbuffer_index != 0) {
      if (GLCAT.is_spam() && gl_debug_buffers) {
        GLCAT.spam()
          << "unbinding vertex buffer\n";
      }
      _glBindBuffer(GL_ARRAY_BUFFER, 0);
      _current_vbuffer_index = 0;
    }
    client_pointer = array_reader->get_read_pointer(force);
    return (client_pointer != NULL);
  }

  // Prepare the buffer object and bind it.
  VertexBufferContext *vbc = ((GeomVertexArrayData *)array_reader->get_object())->prepare_now(get_prepared_objects(), this);
  nassertr(vbc != (VertexBufferContext *)NULL, false);
  if (!apply_vertex_buffer(vbc, array_reader, force)) {
    return false;
  }

  // NULL is the OpenGL convention for the first byte of the buffer object.
  client_pointer = NULL;
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::prepare_index_buffer
//       Access: Public, Virtual
//  Description: Creates a new retained-mode representation of the
//               given data, and returns a newly-allocated
//               IndexBufferContext pointer to reference it.  It is the
//               responsibility of the calling function to later
//               call release_index_buffer() with this same pointer (which
//               will also delete the pointer).
//
//               This function should not be called directly to
//               prepare a buffer.  Instead, call Geom::prepare().
////////////////////////////////////////////////////////////////////
IndexBufferContext *CLP(GraphicsStateGuardian)::
prepare_index_buffer(GeomPrimitive *data) {
  if (_supports_buffers) {
    PStatGPUTimer timer(this, _prepare_index_buffer_pcollector);

    CLP(IndexBufferContext) *gibc = new CLP(IndexBufferContext)(this, _prepared_objects, data);
    _glGenBuffers(1, &gibc->_index);

    if (GLCAT.is_debug() && gl_debug_buffers) {
      GLCAT.debug()
        << "creating index buffer " << (int)gibc->_index << ": "
        << data->get_num_vertices() << " indices ("
        << data->get_vertices()->get_array_format()->get_column(0)->get_numeric_type()
        << ")\n";
    }

    report_my_gl_errors();
    GeomPrimitivePipelineReader reader(data, Thread::get_current_thread());
    apply_index_buffer(gibc, &reader, false);
    return gibc;
  }

  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_index_buffer
//       Access: Public
//  Description: Makes the data the currently available data for
//               rendering.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
apply_index_buffer(IndexBufferContext *ibc,
                   const GeomPrimitivePipelineReader *reader,
                   bool force) {
  nassertr(_supports_buffers, false);
  if (reader->get_modified() == UpdateSeq::initial()) {
    // No need to re-apply.
    return true;
  }

  CLP(IndexBufferContext) *gibc = DCAST(CLP(IndexBufferContext), ibc);

  if (_current_ibuffer_index != gibc->_index) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "binding index buffer " << (int)gibc->_index << "\n";
    }
    _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gibc->_index);
    _current_ibuffer_index = gibc->_index;
    gibc->set_active(true);
  }

  if (gibc->was_modified(reader)) {
    int num_bytes = reader->get_data_size_bytes();
    if (GLCAT.is_debug() && gl_debug_buffers) {
      GLCAT.debug()
        << "copying " << num_bytes
        << " bytes into index buffer " << (int)gibc->_index << "\n";
    }
    if (num_bytes != 0) {
      const unsigned char *client_pointer = reader->get_read_pointer(force);
      if (client_pointer == NULL) {
        return false;
      }

      PStatGPUTimer timer(this, _load_index_buffer_pcollector, reader->get_current_thread());
      if (gibc->changed_size(reader) || gibc->changed_usage_hint(reader)) {
        _glBufferData(GL_ELEMENT_ARRAY_BUFFER, num_bytes, client_pointer,
                      get_usage(reader->get_usage_hint()));

      } else {
        _glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, num_bytes,
                         client_pointer);
      }
      _data_transferred_pcollector.add_level(num_bytes);
    }
    gibc->mark_loaded(reader);
  }
  gibc->enqueue_lru(&_prepared_objects->_graphics_memory_lru);

  maybe_gl_finish();
  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::release_index_buffer
//       Access: Public, Virtual
//  Description: Frees the GL resources previously allocated for the
//               data.  This function should never be called
//               directly; instead, call Data::release() (or simply
//               let the Data destruct).
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
release_index_buffer(IndexBufferContext *ibc) {
  nassertv(_supports_buffers);

  CLP(IndexBufferContext) *gibc = DCAST(CLP(IndexBufferContext), ibc);

  if (GLCAT.is_debug() && gl_debug_buffers) {
    GLCAT.debug()
      << "deleting index buffer " << (int)gibc->_index << "\n";
  }

  // Make sure the buffer is unbound before we delete it.  Not
  // strictly necessary according to the OpenGL spec, but it might
  // help out a flaky driver, and we need to keep our internal state
  // consistent anyway.
  if (_current_ibuffer_index == gibc->_index) {
    if (GLCAT.is_spam() && gl_debug_buffers) {
      GLCAT.spam()
        << "unbinding index buffer\n";
    }
    _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    _current_ibuffer_index = 0;
  }

  _glDeleteBuffers(1, &gibc->_index);
  report_my_gl_errors();

  gibc->_index = 0;

  delete gibc;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::setup_primitive
//       Access: Public
//  Description: Internal function to bind a buffer object for the
//               indicated primitive's index list, if appropriate, or
//               to unbind a buffer object if it should be rendered
//               from client memory.
//
//               If the buffer object is bound, this function sets
//               client_pointer to NULL (representing the start of the
//               buffer object in server memory); if the buffer object
//               is not bound, this function sets client_pointer to to
//               the data array in client memory, that is, the data
//               array passed in.
//
//               If force is not true, the function may return false
//               indicating the data is not currently available.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
setup_primitive(const unsigned char *&client_pointer,
                const GeomPrimitivePipelineReader *reader,
                bool force) {
  if (!_supports_buffers) {
    // No support for buffer objects; always render from client.
    client_pointer = reader->get_read_pointer(force);
    return (client_pointer != NULL);
  }
  if (!vertex_buffers || _geom_display_list != 0 ||
      reader->get_usage_hint() == Geom::UH_client) {
    // The array specifies client rendering only, or buffer objects
    // are configured off.
    if (_current_ibuffer_index != 0) {
      if (GLCAT.is_spam() && gl_debug_buffers) {
        GLCAT.spam()
          << "unbinding index buffer\n";
      }
      _glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      _current_ibuffer_index = 0;
    }
    client_pointer = reader->get_read_pointer(force);
    return (client_pointer != NULL);
  }

  // Prepare the buffer object and bind it.
  IndexBufferContext *ibc = ((GeomPrimitive *)reader->get_object())->prepare_now(get_prepared_objects(), this);
  nassertr(ibc != (IndexBufferContext *)NULL, false);
  if (!apply_index_buffer(ibc, reader, force)) {
    return false;
  }

  // NULL is the OpenGL convention for the first byte of the buffer object.
  client_pointer = NULL;
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_occlusion_query
//       Access: Public, Virtual
//  Description: Begins a new occlusion query.  After this call, you
//               may call begin_draw_primitives() and
//               draw_triangles()/draw_whatever() repeatedly.
//               Eventually, you should call end_occlusion_query()
//               before the end of the frame; that will return a new
//               OcclusionQueryContext object that will tell you how
//               many pixels represented by the bracketed geometry
//               passed the depth test.
//
//               It is not valid to call begin_occlusion_query()
//               between another begin_occlusion_query()
//               .. end_occlusion_query() sequence.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
begin_occlusion_query() {
#ifdef OPENGLES  // Occlusion queries not supported by OpenGL ES.
  nassertv(false);

#else
  nassertv(_supports_occlusion_query);
  nassertv(_current_occlusion_query == (OcclusionQueryContext *)NULL);
  PT(CLP(OcclusionQueryContext)) query = new CLP(OcclusionQueryContext)(this);

  _glGenQueries(1, &query->_index);

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "beginning occlusion query index " << (int)query->_index << "\n";
  }

  _glBeginQuery(GL_SAMPLES_PASSED, query->_index);
  _current_occlusion_query = query;

  report_my_gl_errors();
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_occlusion_query
//       Access: Public, Virtual
//  Description: Ends a previous call to begin_occlusion_query().
//               This call returns the OcclusionQueryContext object
//               that will (eventually) report the number of pixels
//               that passed the depth test between the call to
//               begin_occlusion_query() and end_occlusion_query().
////////////////////////////////////////////////////////////////////
PT(OcclusionQueryContext) CLP(GraphicsStateGuardian)::
end_occlusion_query() {
#ifdef OPENGLES  // Occlusion queries not supported by OpenGL ES.
  nassertr(false, NULL);
  return NULL;

#else
  nassertr(_current_occlusion_query != (OcclusionQueryContext *)NULL, NULL);
  PT(OcclusionQueryContext) result = _current_occlusion_query;

  GLuint index = DCAST(CLP(OcclusionQueryContext), result)->_index;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "ending occlusion query index " << (int)index << "\n";
  }

  _current_occlusion_query = NULL;
  _glEndQuery(GL_SAMPLES_PASSED);

  // Temporary hack to try working around an apparent driver bug on
  // iMacs.  Occlusion queries sometimes incorrectly report 0 samples,
  // unless we stall the pipe to keep fewer than a certain maximum
  // number of queries pending at once.
  static ConfigVariableInt limit_occlusion_queries("limit-occlusion-queries", 0);
  if (limit_occlusion_queries > 0) {
    if (index > (unsigned int)limit_occlusion_queries) {
      PStatGPUTimer timer(this, _wait_occlusion_pcollector);
      GLuint result;
      _glGetQueryObjectuiv(index - (unsigned int)limit_occlusion_queries,
                           GL_QUERY_RESULT, &result);
    }
  }

  report_my_gl_errors();

  return result;
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::issue_timer_query
//       Access: Public, Virtual
//  Description: Adds a timer query to the command stream, associated
//               with the given PStats collector index.
////////////////////////////////////////////////////////////////////
PT(TimerQueryContext) CLP(GraphicsStateGuardian)::
issue_timer_query(int pstats_index) {
#if defined(DO_PSTATS) && !defined(OPENGLES)
  nassertr(_supports_timer_query, NULL);

  PT(CLP(TimerQueryContext)) query;

  // Hack
  if (pstats_index == _command_latency_pcollector.get_index()) {
    query = new CLP(LatencyQueryContext)(this, pstats_index);
  } else {
    query = new CLP(TimerQueryContext)(this, pstats_index);
  }

  if (_deleted_queries.size() >= 1) {
    query->_index = _deleted_queries.back();
    _deleted_queries.pop_back();
  } else {
    _glGenQueries(1, &query->_index);

    if (GLCAT.is_spam()) {
      GLCAT.spam() << "Generating query for " << pstats_index
                   << ": " << query->_index << "\n";
    }
  }

  if (_use_object_labels) {
    // Assign a label to it based on the PStatCollector name.
    const PStatClient *client = PStatClient::get_global_pstats();
    string name = client->get_collector_fullname(pstats_index & 0x7fff);
    _glObjectLabel(GL_QUERY, query->_index, name.size(), name.data());
  }

  // Issue the timestamp query.
  _glQueryCounter(query->_index, GL_TIMESTAMP);

  _pending_timer_queries.push_back(DCAST(TimerQueryContext, query));

  return DCAST(TimerQueryContext, query);

#else
  return NULL;
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::dispatch_compute
//       Access: Public, Virtual
//  Description: Dispatches a currently bound compute shader using
//               the given work group counts.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
dispatch_compute(int num_groups_x, int num_groups_y, int num_groups_z) {
#ifndef OPENGLES
  maybe_gl_finish();

  PStatGPUTimer timer(this, _compute_dispatch_pcollector);
  nassertv(_supports_compute_shaders);
  nassertv(_current_shader_context != NULL);
  _glDispatchCompute(num_groups_x, num_groups_y, num_groups_z);

  maybe_gl_finish();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::make_geom_munger
//       Access: Public, Virtual
//  Description: Creates a new GeomMunger object to munge vertices
//               appropriate to this GSG for the indicated state.
////////////////////////////////////////////////////////////////////
PT(GeomMunger) CLP(GraphicsStateGuardian)::
make_geom_munger(const RenderState *state, Thread *current_thread) {
  PT(CLP(GeomMunger)) munger = new CLP(GeomMunger)(this, state);
  return GeomMunger::register_munger(munger, current_thread);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::framebuffer_copy_to_texture
//       Access: Public, Virtual
//  Description: Copy the pixels within the indicated display
//               region from the framebuffer into texture memory.
//
//               If z > -1, it is the cube map index or layer index
//               into which to copy.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
framebuffer_copy_to_texture(Texture *tex, int view, int z,
                            const DisplayRegion *dr, const RenderBuffer &rb) {
  nassertr(tex != NULL && dr != NULL, false);
  set_read_buffer(rb._buffer_type);
  if (gl_color_mask) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }

  int xo, yo, w, h;
  dr->get_region_pixels(xo, yo, w, h);
  tex->set_size_padded(w, h, tex->get_z_size());

  if (tex->get_compression() == Texture::CM_default) {
    // Unless the user explicitly turned on texture compression, turn
    // it off for the copy-to-texture case.
    tex->set_compression(Texture::CM_off);
  }

  // Sanity check everything.
  if (z >= 0) {
    if (z >= tex->get_z_size()) {
      // This can happen, when textures with different layer counts
      // are attached to a buffer.  We simply ignore this if it happens.
      return false;
    }

    if ((w != tex->get_x_size()) ||
        (h != tex->get_y_size())) {
      return false;
    }

    if (tex->get_texture_type() == Texture::TT_cube_map) {
      if (!_supports_cube_map) {
        return false;
      }

      nassertr(z < 6, false);
      if (w != h) {
        return false;
      }

    } else if (tex->get_texture_type() == Texture::TT_3d_texture) {
      if (!_supports_3d_texture) {
        return false;
      }

    } else if (tex->get_texture_type() == Texture::TT_2d_texture_array) {
      if (!_supports_2d_texture_array) {
        return false;
      }

    } else {
      GLCAT.error()
        << "Don't know how to copy framebuffer to texture " << *tex << "\n";
    }
  } else {
    nassertr(tex->get_texture_type() == Texture::TT_2d_texture, false);
  }

  // Match framebuffer format if necessary.
  if (tex->get_match_framebuffer_format()) {

    switch (tex->get_format()) {
    case Texture::F_depth_component:
    case Texture::F_depth_component16:
    case Texture::F_depth_component24:
    case Texture::F_depth_component32:
    case Texture::F_depth_stencil:
      // Don't remap if we're one of these special format.
      break;

    default:
      // If the texture is a color format, we want to match the
      // presence of sRGB and alpha according to the framebuffer.
      if (_current_properties->get_srgb_color()) {
        if (_current_properties->get_alpha_bits()) {
          tex->set_format(Texture::F_srgb_alpha);
        } else {
          tex->set_format(Texture::F_srgb);
        }
      } else {
        if (_current_properties->get_alpha_bits()) {
          tex->set_format(Texture::F_rgba);
        } else {
          tex->set_format(Texture::F_rgb);
        }
      }
    }
  }

  TextureContext *tc = tex->prepare_now(view, get_prepared_objects(), this);
  nassertr(tc != (TextureContext *)NULL, false);
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

  apply_texture(gtc);
  bool needs_reload = specify_texture(gtc);

  GLenum target = get_texture_target(tex->get_texture_type());
  GLint internal_format = get_internal_image_format(tex);
  int width = tex->get_x_size();
  int height = tex->get_y_size();
  int depth = tex->get_z_size();

  bool uses_mipmaps = tex->uses_mipmaps() && !gl_ignore_mipmaps;
  if (uses_mipmaps) {
    if (_supports_generate_mipmap) {
#ifndef OPENGLES_2
      if (_glGenerateMipmap == NULL) {
        glTexParameteri(target, GL_GENERATE_MIPMAP, true);
      }
#endif
    } else {
      // If we can't auto-generate mipmaps, do without mipmaps.
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      uses_mipmaps = false;
    }
  }

  bool new_image = needs_reload || gtc->was_image_modified();

  if (z >= 0) {
    if (target == GL_TEXTURE_CUBE_MAP) {
      // Copy to a cube map face, which is treated as a 2D texture.
      target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + z;
      depth = 1;
      z = -1;

      // Cube map faces seem to have trouble with CopyTexSubImage, so we
      // always reload the image.
      new_image = true;
    }
  }

  if (!gtc->_has_storage ||
      internal_format != gtc->_internal_format ||
      uses_mipmaps != gtc->_uses_mipmaps ||
      width != gtc->_width ||
      height != gtc->_height ||
      depth != gtc->_depth) {
    // If the texture properties have changed, we need to reload the
    // image.
    new_image = true;
  }

  if (new_image && gtc->_immutable) {
    gtc->reset_data();
    glBindTexture(target, gtc->_index);
  }

#ifndef OPENGLES
  if (gtc->needs_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT)) {
    // Make sure that any incoherent writes to this texture have been synced.
    issue_memory_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
  }
#endif

  if (z >= 0) {
    if (new_image) {
      // These won't be used because we pass a NULL image, but we still
      // have to specify them.  Might as well use the actual values.
      GLint external_format = get_external_image_format(tex);
      GLint component_type = get_component_type(tex->get_component_type());
      _glTexImage3D(target, 0, internal_format, width, height, depth, 0, external_format, component_type, NULL);
    }

    _glCopyTexSubImage3D(target, 0, 0, 0, z, xo, yo, w, h);
  } else {
    if (new_image) {
      // We have to create a new image.
      // It seems that OpenGL accepts a size higher than the framebuffer,
      // but if we run into trouble we'll have to replace this with
      // something smarter.
      glCopyTexImage2D(target, 0, internal_format, xo, yo, width, height, 0);
    } else {
      // We can overlay the existing image.
      glCopyTexSubImage2D(target, 0, 0, 0, xo, yo, w, h);
    }
  }

  if (uses_mipmaps && _glGenerateMipmap != NULL) {
    _glGenerateMipmap(target);
  }

  gtc->_has_storage = true;
  gtc->_uses_mipmaps = uses_mipmaps;
  gtc->_internal_format = internal_format;
  gtc->_width = width;
  gtc->_height = height;
  gtc->_depth = depth;

  gtc->mark_loaded();
  gtc->enqueue_lru(&_prepared_objects->_graphics_memory_lru);

  report_my_gl_errors();

  // Force reload of texture state, since we've just monkeyed with it.
  _state_mask.clear_bit(TextureAttrib::get_class_slot());

  return true;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::framebuffer_copy_to_ram
//       Access: Public, Virtual
//  Description: Copy the pixels within the indicated display region
//               from the framebuffer into system memory, not texture
//               memory.  Returns true on success, false on failure.
//
//               This completely redefines the ram image of the
//               indicated texture.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
framebuffer_copy_to_ram(Texture *tex, int view, int z,
                        const DisplayRegion *dr, const RenderBuffer &rb) {
  nassertr(tex != NULL && dr != NULL, false);
  set_read_buffer(rb._buffer_type);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (gl_color_mask) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }

  // Bug fix for RE, RE2, and VTX - need to disable texturing in order
  // for glReadPixels() to work
  // NOTE: reading the depth buffer is *much* slower than reading the
  // color buffer
  set_state_and_transform(RenderState::make_empty(), _internal_transform);

  int xo, yo, w, h;
  dr->get_region_pixels(xo, yo, w, h);

  Texture::ComponentType component_type = tex->get_component_type();
  bool color_mode = false;

  Texture::Format format = tex->get_format();
  switch (format) {
  case Texture::F_depth_stencil:
    if (_current_properties->get_float_depth()) {
      component_type = Texture::T_float;
    } else {
      component_type = Texture::T_unsigned_int_24_8;
    }
    break;

  case Texture::F_depth_component:
    if (_current_properties->get_float_depth()) {
      component_type = Texture::T_float;
    } else if (_current_properties->get_depth_bits() <= 8) {
      component_type = Texture::T_unsigned_byte;
    } else if (_current_properties->get_depth_bits() <= 16) {
      component_type = Texture::T_unsigned_short;
    } else {
      component_type = Texture::T_float;
    }
    break;

  default:
    color_mode = true;
    if (_current_properties->get_srgb_color()) {
      if (_current_properties->get_alpha_bits()) {
        format = Texture::F_srgb_alpha;
      } else {
        format = Texture::F_srgb;
      }
    } else {
      if (_current_properties->get_alpha_bits()) {
        format = Texture::F_rgba;
      } else {
        format = Texture::F_rgb;
      }
    }
    if (_current_properties->get_float_color()) {
      component_type = Texture::T_float;
    } else if (_current_properties->get_color_bits() <= 24) {
      component_type = Texture::T_unsigned_byte;
    } else {
      component_type = Texture::T_unsigned_short;
    }
  }

  Texture::TextureType texture_type;
  int z_size;
  //TODO: should be extended to support 3D textures and 2D arrays.
  if (z >= 0) {
    texture_type = Texture::TT_cube_map;
    z_size = 6;
  } else {
    texture_type = Texture::TT_2d_texture;
    z_size = 1;
  }

  if (tex->get_x_size() != w || tex->get_y_size() != h ||
      tex->get_z_size() != z_size ||
      tex->get_component_type() != component_type ||
      tex->get_format() != format ||
      tex->get_texture_type() != texture_type) {

    // Re-setup the texture; its properties have changed.
    tex->setup_texture(texture_type, w, h, z_size,
                       component_type, format);
  }

  nassertr(z < tex->get_z_size(), false);

  GLenum external_format = get_external_image_format(tex);

  if (GLCAT.is_spam()) {
    GLCAT.spam()
      << "glReadPixels(" << xo << ", " << yo << ", " << w << ", " << h << ", ";
    switch (external_format) {
    case GL_DEPTH_COMPONENT:
      GLCAT.spam(false) << "GL_DEPTH_COMPONENT, ";
      break;
    case GL_DEPTH_STENCIL:
      GLCAT.spam(false) << "GL_DEPTH_STENCIL, ";
      break;
    case GL_RGB:
      GLCAT.spam(false) << "GL_RGB, ";
      break;
    case GL_RGBA:
      GLCAT.spam(false) << "GL_RGBA, ";
      break;
#ifndef OPENGLES
    case GL_BGR:
      GLCAT.spam(false) << "GL_BGR, ";
      break;
#endif
    case GL_BGRA:
      GLCAT.spam(false) << "GL_BGRA, ";
      break;
    default:
      GLCAT.spam(false) << "unknown, ";
      break;
    }
    switch (get_component_type(component_type)) {
    case GL_UNSIGNED_BYTE:
      GLCAT.spam(false) << "GL_UNSIGNED_BYTE";
      break;
    case GL_UNSIGNED_SHORT:
      GLCAT.spam(false) << "GL_UNSIGNED_SHORT";
      break;
    case GL_FLOAT:
      GLCAT.spam(false) << "GL_FLOAT";
      break;
#ifndef OPENGLES_1
    case GL_INT:
      GLCAT.spam(false) << "GL_INT";
      break;
#endif
    default:
      GLCAT.spam(false) << "unknown";
      break;
    }
    GLCAT.spam(false)
      << ")" << endl;
  }

  unsigned char *image_ptr = tex->modify_ram_image();
  size_t image_size = tex->get_ram_image_size();
  if (z >= 0 || view > 0) {
    image_size = tex->get_expected_ram_page_size();
    if (z >= 0) {
      image_ptr += z * image_size;
    }
    if (view > 0) {
      image_ptr += (view * tex->get_z_size()) * image_size;
    }
  }

  glReadPixels(xo, yo, w, h, external_format,
               get_component_type(component_type), image_ptr);

  // We may have to reverse the byte ordering of the image if GL
  // didn't do it for us.
  if (color_mode && !_supports_bgr) {
    PTA_uchar new_image;
    const unsigned char *result =
      fix_component_ordering(new_image, image_ptr, image_size,
                             external_format, tex);
    if (result != image_ptr) {
      memcpy(image_ptr, result, image_size);
    }
  }

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_fog
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
apply_fog(Fog *fog) {
#ifndef OPENGLES_2
  Fog::Mode fmode = fog->get_mode();
  glFogf(GL_FOG_MODE, get_fog_mode_type(fmode));

  if (fmode == Fog::M_linear) {
    PN_stdfloat onset, opaque;
    fog->get_linear_range(onset, opaque);
    glFogf(GL_FOG_START, onset);
    glFogf(GL_FOG_END, opaque);

  } else {
    // Exponential fog is always camera-relative.
    glFogf(GL_FOG_DENSITY, fog->get_exp_density());
  }

  call_glFogfv(GL_FOG_COLOR, fog->get_color());
  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_transform
//       Access: Protected
//  Description: Sends the indicated transform matrix to the graphics
//               API to be applied to future vertices.
//
//               This transform is the internal_transform, already
//               converted into the GSG's internal coordinate system.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_transform() {
#ifndef OPENGLES_2
  // OpenGL ES 2 does not support glLoadMatrix.

  const TransformState *transform = _internal_transform;
  if (GLCAT.is_spam()) {
    GLCAT.spam()
      << "glLoadMatrix(GL_MODELVIEW): " << transform->get_mat() << endl;
  }

  DO_PSTATS_STUFF(_transform_state_pcollector.add_level(1));
  glMatrixMode(GL_MODELVIEW);
  GLPf(LoadMatrix)(transform->get_mat().get_data());

  if (_auto_rescale_normal) {
    do_auto_rescale_normal();
  }
#endif
  _transform_stale = false;

#ifndef OPENGLES_1
  if (_current_shader_context) {
    _current_shader_context->issue_parameters(Shader::SSD_transform);
  }
#endif

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_shade_model
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_shade_model() {
#ifndef OPENGLES_2
  const ShadeModelAttrib *target_shade_model = DCAST(ShadeModelAttrib, _target_rs->get_attrib_def(ShadeModelAttrib::get_class_slot()));
  switch (target_shade_model->get_mode()) {
  case ShadeModelAttrib::M_smooth:
    glShadeModel(GL_SMOOTH);
    _flat_shade_model = false;
    break;

  case ShadeModelAttrib::M_flat:
    glShadeModel(GL_FLAT);
    _flat_shade_model = true;
    break;
  }
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_shader
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_shader(bool state_has_changed) {
#ifndef OPENGLES_1
  ShaderContext *context = 0;
  Shader *shader = (Shader *)(_target_shader->get_shader());

#ifdef OPENGLES_2
  // If we don't have a shader, apply the default shader.
  if (!shader) {
    shader = _default_shader;
  }

#endif
  if (shader) {
    context = shader->prepare_now(get_prepared_objects(), this);
  }
#ifdef OPENGLES_2
  // If it failed, try applying the default shader.
  if (shader != _default_shader && (context == 0 || !context->valid())) {
    shader = _default_shader;
    context = shader->prepare_now(get_prepared_objects(), this);
  }
#endif

  if (context == 0 || (context->valid() == false)) {
    if (_current_shader_context != 0) {
      _current_shader_context->unbind();
      _current_shader = 0;
      _current_shader_context = 0;
    }
  } else {
    if (context != _current_shader_context) {
      // Use a completely different shader than before.
      // Unbind old shader, bind the new one.
      if (_current_shader_context != 0) {
        _current_shader_context->unbind();
      }
      context->bind();
      _current_shader = shader;
      _current_shader_context = context;
      context->issue_parameters(Shader::SSD_shaderinputs);
    } else {
#ifdef OPENGLES_2
      context->bind(false);
#endif
      if (state_has_changed) {
        // Use the same shader as before, but with new input arguments.
        context->issue_parameters(Shader::SSD_shaderinputs);
      }
    }
  }

  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_render_mode
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_render_mode() {
  const RenderModeAttrib *target_render_mode = DCAST(RenderModeAttrib, _target_rs->get_attrib_def(RenderModeAttrib::get_class_slot()));
  _render_mode = target_render_mode->get_mode();
  _point_size = target_render_mode->get_thickness();
  _point_perspective = target_render_mode->get_perspective();

#ifndef OPENGLES  // glPolygonMode not supported by OpenGL ES.
  switch (_render_mode) {
  case RenderModeAttrib::M_unchanged:
  case RenderModeAttrib::M_filled:
  case RenderModeAttrib::M_filled_flat:
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    break;

  case RenderModeAttrib::M_wireframe:
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    break;

  case RenderModeAttrib::M_point:
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    break;

  default:
    GLCAT.error()
      << "Unknown render mode " << (int)_render_mode << endl;
  }
#endif  // OPENGLES

  // The thickness affects both the line width and the point size.
  glLineWidth(_point_size);
#ifndef OPENGLES_2
  glPointSize(_point_size);
#endif
  report_my_gl_errors();

  do_point_size();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_antialias
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_antialias() {
  const AntialiasAttrib *target_antialias = DCAST(AntialiasAttrib, _target_rs->get_attrib_def(AntialiasAttrib::get_class_slot()));
  if (target_antialias->get_mode_type() == AntialiasAttrib::M_auto) {
    // In this special mode, we must enable antialiasing on a
    // case-by-case basis, because we enable it differently for
    // polygons and for points and lines.
    _auto_antialias_mode = true;

  } else {
    // Otherwise, explicitly enable or disable according to the bits
    // that are set.  But if multisample is requested and supported,
    // don't use the other bits at all (they will be ignored by GL
    // anyway).
    _auto_antialias_mode = false;
    unsigned short mode = target_antialias->get_mode();

    if (_supports_multisample &&
        (mode & AntialiasAttrib::M_multisample) != 0) {
      enable_multisample_antialias(true);

    } else {
      enable_multisample_antialias(false);
      enable_line_smooth((mode & AntialiasAttrib::M_line) != 0);
      enable_point_smooth((mode & AntialiasAttrib::M_point) != 0);
      enable_polygon_smooth((mode & AntialiasAttrib::M_polygon) != 0);
    }
  }

#ifndef OPENGLES_2
  switch (target_antialias->get_mode_quality()) {
  case AntialiasAttrib::M_faster:
    glHint(GL_LINE_SMOOTH_HINT, GL_FASTEST);
    glHint(GL_POINT_SMOOTH_HINT, GL_FASTEST);
#ifndef OPENGLES
    glHint(GL_POLYGON_SMOOTH_HINT, GL_FASTEST);
#endif  // OPENGLES
    break;

  case AntialiasAttrib::M_better:
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
#ifndef OPENGLES
    glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
#endif  // OPENGLES
    break;

  default:
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    glHint(GL_POINT_SMOOTH_HINT, GL_DONT_CARE);
#ifndef OPENGLES
    glHint(GL_POLYGON_SMOOTH_HINT, GL_DONT_CARE);
#endif  // OPENGLES
    break;
  }
#endif

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_rescale_normal
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_rescale_normal() {
#ifndef OPENGLES_2 // OpenGL ES 2.0 doesn't support rescaling normals.
  const RescaleNormalAttrib *target_rescale_normal = DCAST(RescaleNormalAttrib, _target_rs->get_attrib_def(RescaleNormalAttrib::get_class_slot()));
  RescaleNormalAttrib::Mode mode = target_rescale_normal->get_mode();

  _auto_rescale_normal = false;

  switch (mode) {
  case RescaleNormalAttrib::M_none:
    glDisable(GL_NORMALIZE);
    if (_supports_rescale_normal && support_rescale_normal) {
      glDisable(GL_RESCALE_NORMAL);
    }
    break;

  case RescaleNormalAttrib::M_rescale:
    if (_supports_rescale_normal && support_rescale_normal) {
      glEnable(GL_RESCALE_NORMAL);
      glDisable(GL_NORMALIZE);
    } else {
      glEnable(GL_NORMALIZE);
    }
    break;

  case RescaleNormalAttrib::M_normalize:
    glEnable(GL_NORMALIZE);
    if (_supports_rescale_normal && support_rescale_normal) {
      glDisable(GL_RESCALE_NORMAL);
    }
    break;

  case RescaleNormalAttrib::M_auto:
    _auto_rescale_normal = true;
    do_auto_rescale_normal();
    break;

  default:
    GLCAT.error()
      << "Unknown rescale_normal mode " << (int)mode << endl;
  }
  report_my_gl_errors();
#endif
}

// PandaCompareFunc - 1 + 0x200 === GL_NEVER, etc.  order is sequential
#define PANDA_TO_GL_COMPAREFUNC(PANDACMPFUNC) (PANDACMPFUNC-1 +0x200)

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_depth_test
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_depth_test() {
  const DepthTestAttrib *target_depth_test = DCAST(DepthTestAttrib, _target_rs->get_attrib_def(DepthTestAttrib::get_class_slot()));
  DepthTestAttrib::PandaCompareFunc mode = target_depth_test->get_mode();
  if (mode == DepthTestAttrib::M_none) {
    enable_depth_test(false);
  } else {
    enable_depth_test(true);
    glDepthFunc(PANDA_TO_GL_COMPAREFUNC(mode));
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_alpha_test
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_alpha_test() {
  if (_target_shader->get_flag(ShaderAttrib::F_subsume_alpha_test)) {
    enable_alpha_test(false);
  } else {
    const AlphaTestAttrib *target_alpha_test = DCAST(AlphaTestAttrib, _target_rs->get_attrib_def(AlphaTestAttrib::get_class_slot()));
    AlphaTestAttrib::PandaCompareFunc mode = target_alpha_test->get_mode();
    if (mode == AlphaTestAttrib::M_none) {
      enable_alpha_test(false);
    } else {
      nassertv(GL_NEVER == (AlphaTestAttrib::M_never-1+0x200));
#ifndef OPENGLES_2
      glAlphaFunc(PANDA_TO_GL_COMPAREFUNC(mode), target_alpha_test->get_reference_alpha());
#endif
      enable_alpha_test(true);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_depth_write
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_depth_write() {
  const DepthWriteAttrib *target_depth_write = DCAST(DepthWriteAttrib, _target_rs->get_attrib_def(DepthWriteAttrib::get_class_slot()));
  DepthWriteAttrib::Mode mode = target_depth_write->get_mode();
  if (mode == DepthWriteAttrib::M_off) {
#ifdef GSG_VERBOSE
    GLCAT.spam()
      << "glDepthMask(GL_FALSE)" << endl;
#endif
    glDepthMask(GL_FALSE);
  } else {
#ifdef GSG_VERBOSE
    GLCAT.spam()
      << "glDepthMask(GL_TRUE)" << endl;
#endif
    glDepthMask(GL_TRUE);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_cull_face
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_cull_face() {
  const CullFaceAttrib *target_cull_face = DCAST(CullFaceAttrib, _target_rs->get_attrib_def(CullFaceAttrib::get_class_slot()));
  CullFaceAttrib::Mode mode = target_cull_face->get_effective_mode();

  switch (mode) {
  case CullFaceAttrib::M_cull_none:
    glDisable(GL_CULL_FACE);
    break;
  case CullFaceAttrib::M_cull_clockwise:
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    break;
  case CullFaceAttrib::M_cull_counter_clockwise:
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    break;
  default:
    GLCAT.error()
      << "invalid cull face mode " << (int)mode << endl;
    break;
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_fog
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_fog() {
  const FogAttrib *target_fog = DCAST(FogAttrib, _target_rs->get_attrib_def(FogAttrib::get_class_slot()));
  if (!target_fog->is_off()) {
    enable_fog(true);
    Fog *fog = target_fog->get_fog();
    nassertv(fog != (Fog *)NULL);
    apply_fog(fog);
  } else {
    enable_fog(false);
  }
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_depth_offset
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_depth_offset() {
  const DepthOffsetAttrib *target_depth_offset = DCAST(DepthOffsetAttrib, _target_rs->get_attrib_def(DepthOffsetAttrib::get_class_slot()));
  int offset = target_depth_offset->get_offset();

  if (offset != 0) {
    // The relationship between these two parameters is a little
    // unclear and poorly explained in the GL man pages.
    glPolygonOffset((GLfloat) -offset, (GLfloat) -offset);
    enable_polygon_offset(true);

  } else {
    enable_polygon_offset(false);
  }

  PN_stdfloat min_value = target_depth_offset->get_min_value();
  PN_stdfloat max_value = target_depth_offset->get_max_value();
#ifdef GSG_VERBOSE
    GLCAT.spam()
      << "glDepthRange(" << min_value << ", " << max_value << ")" << endl;
#endif
#ifdef OPENGLES
  // OpenGL ES uses a single-precision call.
  glDepthRangef((GLclampf)min_value, (GLclampf)max_value);
#else
  // Mainline OpenGL uses a double-precision call.
  glDepthRange((GLclampd)min_value, (GLclampd)max_value);
#endif  // OPENGLES

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_material
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_material() {
#ifndef OPENGLES_2 // OpenGL ES 2.0 doesn't support materials.
  static Material empty;
  const Material *material;

  const MaterialAttrib *target_material = DCAST(MaterialAttrib, _target_rs->get_attrib_def(MaterialAttrib::get_class_slot()));

  if (target_material == (MaterialAttrib *)NULL ||
      target_material->is_off()) {
    material = &empty;
  } else {
    material = target_material->get_material();
  }

  bool has_material_force_color = _has_material_force_color;

#ifndef NDEBUG
  if (_show_texture_usage) {
    // In show_texture_usage mode, all colors are white, so as not
    // to contaminate the texture color.  This means we disable
    // lighting materials too.
    material = &empty;
    has_material_force_color = false;
  }
#endif  // NDEBUG

#ifdef OPENGLES
  const GLenum face = GL_FRONT_AND_BACK;
#else
  GLenum face = material->get_twoside() ? GL_FRONT_AND_BACK : GL_FRONT;
#endif

  call_glMaterialfv(face, GL_SPECULAR, material->get_specular());
  call_glMaterialfv(face, GL_EMISSION, material->get_emission());
  glMaterialf(face, GL_SHININESS, min(material->get_shininess(), (PN_stdfloat)128.0));

  if (material->has_ambient() && material->has_diffuse()) {
    // The material has both an ambient and diffuse specified.  This
    // means we do not need glMaterialColor().
    glDisable(GL_COLOR_MATERIAL);
    call_glMaterialfv(face, GL_AMBIENT, material->get_ambient());
    call_glMaterialfv(face, GL_DIFFUSE, material->get_diffuse());

  } else if (material->has_ambient()) {
    // The material specifies an ambient, but not a diffuse component.
    // The diffuse component comes from the object's color.
    call_glMaterialfv(face, GL_AMBIENT, material->get_ambient());
    if (has_material_force_color) {
      glDisable(GL_COLOR_MATERIAL);
      call_glMaterialfv(face, GL_DIFFUSE, _material_force_color);
    } else {
#ifndef OPENGLES
      glColorMaterial(face, GL_DIFFUSE);
#endif  // OPENGLES
      glEnable(GL_COLOR_MATERIAL);
    }

  } else if (material->has_diffuse()) {
    // The material specifies a diffuse, but not an ambient component.
    // The ambient component comes from the object's color.
    call_glMaterialfv(face, GL_DIFFUSE, material->get_diffuse());
    if (has_material_force_color) {
      glDisable(GL_COLOR_MATERIAL);
      call_glMaterialfv(face, GL_AMBIENT, _material_force_color);
    } else {
#ifndef OPENGLES
      glColorMaterial(face, GL_AMBIENT);
#endif  // OPENGLES
      glEnable(GL_COLOR_MATERIAL);
    }

  } else {
    // The material specifies neither a diffuse nor an ambient
    // component.  Both components come from the object's color.
    if (has_material_force_color) {
      glDisable(GL_COLOR_MATERIAL);
      call_glMaterialfv(face, GL_AMBIENT, _material_force_color);
      call_glMaterialfv(face, GL_DIFFUSE, _material_force_color);
    } else {
#ifndef OPENGLES
      glColorMaterial(face, GL_AMBIENT_AND_DIFFUSE);
#endif  // OPENGLES
      glEnable(GL_COLOR_MATERIAL);
    }
  }

#ifndef OPENGLES
  glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, material->get_local());
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, material->get_twoside());

  if (gl_separate_specular_color) {
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
  } else {
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
  }
#endif

  report_my_gl_errors();
#endif  // OPENGLES_2
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_blending
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_blending() {
  // Handle the color_write attrib.  If color_write is off, then
  // all the other blending-related stuff doesn't matter.  If the
  // device doesn't support color-write, we use blending tricks
  // to effectively disable color write.
  const ColorWriteAttrib *target_color_write = DCAST(ColorWriteAttrib, _target_rs->get_attrib_def(ColorWriteAttrib::get_class_slot()));

  unsigned int color_channels =
    target_color_write->get_channels() & _color_write_mask;
  if (_target_shader->get_flag(ShaderAttrib::F_disable_alpha_write)) {
    color_channels &= ~(ColorWriteAttrib::C_alpha);
  }
  if (color_channels == ColorWriteAttrib::C_off) {
    int color_write_slot = ColorWriteAttrib::get_class_slot();
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    if (gl_color_mask) {
      enable_blend(false);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else {
      enable_blend(true);
      _glBlendEquation(GL_FUNC_ADD);
      glBlendFunc(GL_ZERO, GL_ONE);
    }
    return;
  } else {
    if (gl_color_mask) {
      glColorMask((color_channels & ColorWriteAttrib::C_red) != 0,
                     (color_channels & ColorWriteAttrib::C_green) != 0,
                     (color_channels & ColorWriteAttrib::C_blue) != 0,
                     (color_channels & ColorWriteAttrib::C_alpha) != 0);
    }
  }


  const ColorBlendAttrib *target_color_blend = DCAST(ColorBlendAttrib, _target_rs->get_attrib_def(ColorBlendAttrib::get_class_slot()));
  CPT(ColorBlendAttrib) color_blend = target_color_blend;
  ColorBlendAttrib::Mode color_blend_mode = target_color_blend->get_mode();

  const TransparencyAttrib *target_transparency = DCAST(TransparencyAttrib, _target_rs->get_attrib_def(TransparencyAttrib::get_class_slot()));
  TransparencyAttrib::Mode transparency_mode = target_transparency->get_mode();

  _color_blend_involves_color_scale = color_blend->involves_color_scale();

  // Is there a color blend set?
  if (color_blend_mode != ColorBlendAttrib::M_none) {
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(get_blend_equation_type(color_blend_mode));
    glBlendFunc(get_blend_func(color_blend->get_operand_a()),
                   get_blend_func(color_blend->get_operand_b()));

    if (_color_blend_involves_color_scale) {
      // Apply the current color scale to the blend mode.
      _glBlendColor(_current_color_scale[0], _current_color_scale[1],
                    _current_color_scale[2], _current_color_scale[3]);

    } else {
      LColor c = color_blend->get_color();
      _glBlendColor(c[0], c[1], c[2], c[3]);
    }
    return;
  }

  // No color blend; is there a transparency set?
  switch (transparency_mode) {
  case TransparencyAttrib::M_none:
  case TransparencyAttrib::M_binary:
    break;

  case TransparencyAttrib::M_alpha:
  case TransparencyAttrib::M_dual:
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return;

  case TransparencyAttrib::M_multisample:
    // We need to enable *both* of these in M_multisample case.
    enable_multisample_alpha_one(true);
    enable_multisample_alpha_mask(true);
    enable_blend(false);
    return;

  case TransparencyAttrib::M_multisample_mask:
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(true);
    enable_blend(false);
    return;

  default:
    GLCAT.error()
      << "invalid transparency mode " << (int)transparency_mode << endl;
    break;
  }

  if (_line_smooth_enabled || _point_smooth_enabled) {
    // If we have either of these turned on, we also need to have
    // blend mode enabled in order to see it.
    enable_multisample_alpha_one(false);
    enable_multisample_alpha_mask(false);
    enable_blend(true);
    _glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    return;
  }

  // For best polygon smoothing, we need:
  // (1) a frame buffer that supports alpha
  // (2) sort polygons front-to-back
  // (3) glBlendFunc(GL_SRC_ALPHA_SATURATE, GL_ONE);
  //
  // Since these modes have other implications for the application, we
  // don't attempt to do this by default.  If you really want good
  // polygon smoothing (and you don't have multisample support), do
  // all this yourself.

  // Nothing's set, so disable blending.
  enable_multisample_alpha_one(false);
  enable_multisample_alpha_mask(false);
  enable_blend(false);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(PointLight *light_obj, const NodePath &light, int light_id) {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_bind_point_pcollector("Draw:Set State:Light:Bind:Point");
  //  PStatGPUTimer timer(this, _draw_set_state_light_bind_point_pcollector);

  GLenum id = get_light_id(light_id);
  static const LColor black(0.0f, 0.0f, 0.0f, 1.0f);
  call_glLightfv(id, GL_AMBIENT, black);
  call_glLightfv(id, GL_DIFFUSE, get_light_color(light_obj));
  call_glLightfv(id, GL_SPECULAR, light_obj->get_specular_color());

  // Position needs to specify x, y, z, and w
  // w == 1 implies non-infinite position
  CPT(TransformState) transform = light.get_transform(_scene_setup->get_scene_root().get_parent());
  LPoint3 pos = light_obj->get_point() * transform->get_mat();

  LPoint4 fpos(pos[0], pos[1], pos[2], 1.0f);
  call_glLightfv(id, GL_POSITION, fpos);

  // GL_SPOT_DIRECTION is not significant when cutoff == 180

  // Exponent == 0 implies uniform light distribution
  glLightf(id, GL_SPOT_EXPONENT, 0.0f);

  // Cutoff == 180 means uniform point light source
  glLightf(id, GL_SPOT_CUTOFF, 180.0f);

  const LVecBase3 &att = light_obj->get_attenuation();
  glLightf(id, GL_CONSTANT_ATTENUATION, att[0]);
  glLightf(id, GL_LINEAR_ATTENUATION, att[1]);
  glLightf(id, GL_QUADRATIC_ATTENUATION, att[2]);

  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(DirectionalLight *light_obj, const NodePath &light, int light_id) {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_bind_directional_pcollector("Draw:Set State:Light:Bind:Directional");
  //  PStatGPUTimer timer(this, _draw_set_state_light_bind_directional_pcollector);

  pair<DirectionalLights::iterator, bool> lookup = _dlights.insert(DirectionalLights::value_type(light, DirectionalLightFrameData()));
  DirectionalLightFrameData &fdata = (*lookup.first).second;
  if (lookup.second) {
    // The light was not computed yet this frame.  Compute it now.
    CPT(TransformState) transform = light.get_transform(_scene_setup->get_scene_root().get_parent());
    LVector3 dir = light_obj->get_direction() * transform->get_mat();
    fdata._neg_dir.set(-dir[0], -dir[1], -dir[2], 0);
  }

  GLenum id = get_light_id( light_id );
  static const LColor black(0.0f, 0.0f, 0.0f, 1.0f);
  call_glLightfv(id, GL_AMBIENT, black);
  call_glLightfv(id, GL_DIFFUSE, get_light_color(light_obj));
  call_glLightfv(id, GL_SPECULAR, light_obj->get_specular_color());

  // Position needs to specify x, y, z, and w.
  // w == 0 implies light is at infinity
  call_glLightfv(id, GL_POSITION, fdata._neg_dir);

  // GL_SPOT_DIRECTION is not significant when cutoff == 180
  // In this case, position x, y, z specifies direction

  // Exponent == 0 implies uniform light distribution
  glLightf(id, GL_SPOT_EXPONENT, 0.0f);

  // Cutoff == 180 means uniform point light source
  glLightf(id, GL_SPOT_CUTOFF, 180.0f);

  // Default attenuation values (only spotlight and point light can
  // modify these)
  glLightf(id, GL_CONSTANT_ATTENUATION, 1.0f);
  glLightf(id, GL_LINEAR_ATTENUATION, 0.0f);
  glLightf(id, GL_QUADRATIC_ATTENUATION, 0.0f);

  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_light
//       Access: Public, Virtual
//  Description: Called the first time a particular light has been
//               bound to a given id within a frame, this should set
//               up the associated hardware light with the light's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_light(Spotlight *light_obj, const NodePath &light, int light_id) {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_bind_spotlight_pcollector("Draw:Set State:Light:Bind:Spotlight");
  //  PStatGPUTimer timer(this, _draw_set_state_light_bind_spotlight_pcollector);

  Lens *lens = light_obj->get_lens();
  nassertv(lens != (Lens *)NULL);

  GLenum id = get_light_id(light_id);
  static const LColor black(0.0f, 0.0f, 0.0f, 1.0f);
  call_glLightfv(id, GL_AMBIENT, black);
  call_glLightfv(id, GL_DIFFUSE, get_light_color(light_obj));
  call_glLightfv(id, GL_SPECULAR, light_obj->get_specular_color());

  // Position needs to specify x, y, z, and w
  // w == 1 implies non-infinite position
  CPT(TransformState) transform = light.get_transform(_scene_setup->get_scene_root().get_parent());
  const LMatrix4 &light_mat = transform->get_mat();
  LPoint3 pos = lens->get_nodal_point() * light_mat;
  LVector3 dir = lens->get_view_vector() * light_mat;

  LPoint4 fpos(pos[0], pos[1], pos[2], 1.0f);
  call_glLightfv(id, GL_POSITION, fpos);
  call_glLightfv(id, GL_SPOT_DIRECTION, dir);

  glLightf(id, GL_SPOT_EXPONENT, light_obj->get_exponent());
  glLightf(id, GL_SPOT_CUTOFF, lens->get_hfov() * 0.5f);

  const LVecBase3 &att = light_obj->get_attenuation();
  glLightf(id, GL_CONSTANT_ATTENUATION, att[0]);
  glLightf(id, GL_LINEAR_ATTENUATION, att[1]);
  glLightf(id, GL_QUADRATIC_ATTENUATION, att[2]);

  report_my_gl_errors();
#endif
}

#ifdef SUPPORT_IMMEDIATE_MODE
////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_immediate_simple_primitives
//       Access: Protected
//  Description: Uses the ImmediateModeSender to draw a series of
//               primitives of the indicated type.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_immediate_simple_primitives(const GeomPrimitivePipelineReader *reader, GLenum mode) {
  int num_vertices = reader->get_num_vertices();
  _vertices_immediate_pcollector.add_level(num_vertices);
  glBegin(mode);

  if (reader->is_indexed()) {
    for (int v = 0; v < num_vertices; ++v) {
      _sender.set_vertex(reader->get_vertex(v));
      _sender.issue_vertex();
    }

  } else {
    _sender.set_vertex(reader->get_first_vertex());
    for (int v = 0; v < num_vertices; ++v) {
      _sender.issue_vertex();
    }
  }

  glEnd();
}
#endif  // SUPPORT_IMMEDIATE_MODE

#ifdef SUPPORT_IMMEDIATE_MODE
////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::draw_immediate_composite_primitives
//       Access: Protected
//  Description: Uses the ImmediateModeSender to draw a series of
//               primitives of the indicated type.  This form is for
//               primitive types like tristrips which must involve
//               several begin/end groups.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
draw_immediate_composite_primitives(const GeomPrimitivePipelineReader *reader, GLenum mode) {
  int num_vertices = reader->get_num_vertices();
  _vertices_immediate_pcollector.add_level(num_vertices);
  CPTA_int ends = reader->get_ends();
  int num_unused_vertices_per_primitive = reader->get_object()->get_num_unused_vertices_per_primitive();

  if (reader->is_indexed()) {
    int begin = 0;
    CPTA_int::const_iterator ei;
    for (ei = ends.begin(); ei != ends.end(); ++ei) {
      int end = (*ei);

      glBegin(mode);
      for (int v = begin; v < end; ++v) {
        _sender.set_vertex(reader->get_vertex(v));
        _sender.issue_vertex();
      }
      glEnd();

      begin = end + num_unused_vertices_per_primitive;
    }

  } else {
    _sender.set_vertex(reader->get_first_vertex());
    int begin = 0;
    CPTA_int::const_iterator ei;
    for (ei = ends.begin(); ei != ends.end(); ++ei) {
      int end = (*ei);

      glBegin(mode);
      for (int v = begin; v < end; ++v) {
        _sender.issue_vertex();
      }
      glEnd();

      begin = end + num_unused_vertices_per_primitive;
    }
  }
}
#endif  // SUPPORT_IMMEDIATE_MODE

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::gl_flush
//       Access: Protected, Virtual
//  Description: Calls glFlush().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
gl_flush() const {
  PStatTimer timer(_flush_pcollector);
  glFlush();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::gl_get_error
//       Access: Protected, Virtual
//  Description: Returns the result of glGetError().
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
gl_get_error() const {
  if (_check_errors) {
    PStatTimer timer(_check_error_pcollector);
    return glGetError();
  } else {
    return GL_NO_ERROR;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::report_errors_loop
//       Access: Protected, Static
//  Description: The internal implementation of report_errors().
//               Don't call this function; use report_errors()
//               instead.  The return value is true if everything is
//               ok, or false if we should shut down.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
report_errors_loop(int line, const char *source_file, GLenum error_code,
                   int &error_count) {
  while ((gl_max_errors < 0 || error_count < gl_max_errors) &&
         (error_code != GL_NO_ERROR)) {
    GLCAT.error()
      << "at " << line << " of " << source_file << " : "
      << get_error_string(error_code) << "\n";

    error_code = glGetError();
    error_count++;
  }

  return (error_code == GL_NO_ERROR);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_error_string
//       Access: Protected, Static
//  Description: Returns an error string for an OpenGL error code.
////////////////////////////////////////////////////////////////////
string CLP(GraphicsStateGuardian)::
get_error_string(GLenum error_code) {
  // We used to use gluErrorString here, but I (rdb) took it out
  // because that was really the only function we used from GLU.
  // The idea with the error table was taken from SGI's sample implementation.
  static const char *error_strings[GL_OUT_OF_MEMORY - GL_INVALID_ENUM + 1] = {
    "invalid enumerant",
    "invalid value",
    "invalid operation",
    "stack overflow",
    "stack underflow",
    "out of memory",
  };

  if (error_code == GL_NO_ERROR) {
    return "no error";
#ifndef OPENGLES
  } else if (error_code == GL_TABLE_TOO_LARGE) {
    return "table too large";
#endif
  } else if (error_code >= GL_INVALID_ENUM && error_code <= GL_OUT_OF_MEMORY) {
    return error_strings[error_code - GL_INVALID_ENUM];
  }

  // Other error, somehow?  Just display the error code then.
  ostringstream strm;
  strm << "GL error " << (int)error_code;

  return strm.str();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::show_gl_string
//       Access: Protected
//  Description: Outputs the result of glGetString() on the indicated
//               tag.  The output string is returned.
////////////////////////////////////////////////////////////////////
string CLP(GraphicsStateGuardian)::
show_gl_string(const string &name, GLenum id) {
  string result;

  const GLubyte *text = glGetString(id);

  if (text == (const GLubyte *)NULL) {
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "Unable to query " << name << "\n";
    }
  } else {
    result = (const char *)text;
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << name << " = " << result << "\n";
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::query_gl_version
//       Access: Protected, Virtual
//  Description: Queries the runtime version of OpenGL in use.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
query_gl_version() {
  _gl_vendor = show_gl_string("GL_VENDOR", GL_VENDOR);
  _gl_renderer = show_gl_string("GL_RENDERER", GL_RENDERER);

  _gl_version_major = 0;
  _gl_version_minor = 0;

  const GLubyte *text = glGetString(GL_VERSION);
  if (text == (const GLubyte *)NULL) {
    GLCAT.debug()
      << "Unable to query GL_VERSION\n";
  } else {
    string version((const char *)text);
    _gl_version = version;

    string input = version;

    // Skip any initial words that don't begin with a digit.
    while (!input.empty() && !isdigit(input[0])) {
      size_t space = input.find(' ');
      if (space == string::npos) {
        break;
      }
      size_t next = space + 1;
      while (next < input.length() && isspace(input[next])) {
        ++next;
      }
      input = input.substr(next);
    }

    // Truncate after the first space.
    size_t space = input.find(' ');
    if (space != string::npos) {
      input = input.substr(0, space);
    }

    vector_string components;
    tokenize(input, components, ".");
    if (components.size() >= 1) {
      string_to_int(components[0], _gl_version_major);
    }
    if (components.size() >= 2) {
      string_to_int(components[1], _gl_version_minor);
    }

    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "GL_VERSION = " << version << ", decoded to "
        << _gl_version_major << "." << _gl_version_minor
        << "\n";
    }
#ifndef OPENGLES
    if (_gl_version_major==1) {
        const char *extstr = (const char *) glGetString(GL_EXTENSIONS);
        if (extstr != NULL) {
            if (strstr( extstr, "GL_ARB_shading_language_100") != NULL)
            {
                _gl_shadlang_ver_major = 1;
                _gl_shadlang_ver_minor = 0;
            }
        }
    }
    else if (_gl_version_major >= 2) {
        const char *verstr = (const char *) glGetString(GL_SHADING_LANGUAGE_VERSION);
        if ((verstr == NULL) || (sscanf(verstr, "%d.%d", &_gl_shadlang_ver_major, &_gl_shadlang_ver_minor) != 2))
        {
            GLCAT.warning()  << "Invalid GL_SHADING_LANGUAGE_VERSION format.\n";
        }
    }
#endif
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::save_extensions
//       Access: Protected
//  Description: Separates the string returned by GL_EXTENSIONS (or
//               glx or wgl extensions) into its individual tokens
//               and saves them in the _extensions member.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
save_extensions(const char *extensions) {
  if (extensions != (const char *)NULL) {
    vector_string tokens;
    extract_words(extensions, tokens);

    vector_string::iterator ti;
    for (ti = tokens.begin(); ti != tokens.end(); ++ti) {
      _extensions.insert(*ti);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_extra_extensions
//       Access: Protected, Virtual
//  Description: This may be redefined by a derived class (e.g. glx or
//               wgl) to get whatever further extensions strings may
//               be appropriate to that interface, in addition to the
//               GL extension strings return by glGetString().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
get_extra_extensions() {
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::report_extensions
//       Access: Protected
//  Description: Outputs the list of GL extensions to notify, if debug
//               mode is enabled.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
report_extensions() const {
  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "GL Extensions:\n";
    pset<string>::const_iterator ei;
    for (ei = _extensions.begin(); ei != _extensions.end(); ++ei) {
      GLCAT.debug() << (*ei) << "\n";
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::has_extension
//       Access: Protected
//  Description: Returns true if the indicated extension is reported
//               by the GL system, false otherwise.  The extension
//               name is case-sensitive.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
has_extension(const string &extension) const {

  bool state;

  state = _extensions.find(extension) != _extensions.end();
  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "HAS EXT " << extension << " " << state << "\n";
  }

  return state;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_extension_func
//       Access: Public
//  Description: Returns the pointer to the GL extension function with
//               the indicated name, or NULL if the function is not
//               available.
////////////////////////////////////////////////////////////////////
void *CLP(GraphicsStateGuardian)::
get_extension_func(const char *name) {
  // First, look in the static-compiled namespace.  If we were
  // compiled to expect at least a certain minimum runtime version of
  // OpenGL, then we can expect those extension functions to be
  // available at compile time.  Somewhat more reliable than poking
  // around in the runtime pointers.
  static struct {
    const char *name;
    void *fptr;
  } compiled_function_table[] = {
#ifdef EXPECT_GL_VERSION_1_2
    { "glBlendColor", (void *)&glBlendColor },
    { "glBlendEquation", (void *)&glBlendEquation },
    { "glDrawRangeElements", (void *)&glDrawRangeElements },
    { "glTexImage3D", (void *)&glTexImage3D },
    { "glTexSubImage3D", (void *)&glTexSubImage3D },
    { "glCopyTexSubImage3D", (void *)&glCopyTexSubImage3D },
#endif
#ifdef EXPECT_GL_VERSION_1_3
    { "glActiveTexture", (void *)&glActiveTexture },
    { "glClientActiveTexture", (void *)&glClientActiveTexture },
    { "glCompressedTexImage1D", (void *)&glCompressedTexImage1D },
    { "glCompressedTexImage2D", (void *)&glCompressedTexImage2D },
    { "glCompressedTexImage3D", (void *)&glCompressedTexImage3D },
    { "glCompressedTexSubImage1D", (void *)&glCompressedTexSubImage1D },
    { "glCompressedTexSubImage2D", (void *)&glCompressedTexSubImage2D },
    { "glCompressedTexSubImage3D", (void *)&glCompressedTexSubImage3D },
    { "glGetCompressedTexImage", (void *)&glGetCompressedTexImage },
    { "glMultiTexCoord1f", (void *)&glMultiTexCoord1f },
    { "glMultiTexCoord2", (void *)&glMultiTexCoord2 },
    { "glMultiTexCoord3", (void *)&glMultiTexCoord3 },
    { "glMultiTexCoord4", (void *)&glMultiTexCoord4 },
#endif
#ifdef EXPECT_GL_VERSION_1_4
    { "glPointParameterfv", (void *)&glPointParameterfv },
#endif
#ifdef EXPECT_GL_VERSION_1_5
    { "glBeginQuery", (void *)&glBeginQuery },
    { "glBindBuffer", (void *)&glBindBuffer },
    { "glBufferData", (void *)&glBufferData },
    { "glBufferSubData", (void *)&glBufferSubData },
    { "glDeleteBuffers", (void *)&glDeleteBuffers },
    { "glDeleteQueries", (void *)&glDeleteQueries },
    { "glEndQuery", (void *)&glEndQuery },
    { "glGenBuffers", (void *)&glGenBuffers },
    { "glGenQueries", (void *)&glGenQueries },
    { "glGetQueryObjectuiv", (void *)&glGetQueryObjectuiv },
    { "glGetQueryiv", (void *)&glGetQueryiv },
#endif
#ifdef OPENGLES
    { "glActiveTexture", (void *)&glActiveTexture },
#ifndef OPENGLES_2
    { "glClientActiveTexture", (void *)&glClientActiveTexture },
#endif
    { "glBindBuffer", (void *)&glBindBuffer },
    { "glBufferData", (void *)&glBufferData },
    { "glBufferSubData", (void *)&glBufferSubData },
    { "glDeleteBuffers", (void *)&glDeleteBuffers },
    { "glGenBuffers", (void *)&glGenBuffers },
#endif
    { NULL, NULL }
  };

  int i = 0;
  while (compiled_function_table[i].name != NULL) {
    if (strcmp(compiled_function_table[i].name, name) == 0) {
      return compiled_function_table[i].fptr;
    }
    ++i;
  }

  // If the extension function wasn't compiled in, then go get it from
  // the runtime.  There's a different interface for each API.
  return do_get_extension_func(name);
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_get_extension_func
//       Access: Public, Virtual
//  Description: This is the virtual implementation of
//               get_extension_func().  Each API-specific GL
//               implementation will map this method to the
//               appropriate API call to retrieve the extension
//               function pointer.  Returns NULL if the function is
//               not available.
////////////////////////////////////////////////////////////////////
void *CLP(GraphicsStateGuardian)::
do_get_extension_func(const char *) {
  return NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_draw_buffer
//       Access: Protected
//  Description: Sets up the glDrawBuffer to render into the buffer
//               indicated by the RenderBuffer object.  This only sets
//               up the color and aux bits; it does not affect the depth,
//               stencil, accum layers.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_draw_buffer(int rbtype) {
#ifndef OPENGLES  // Draw buffers not supported by OpenGL ES.
  if (_current_fbo) {

    GLuint buffers[16];
    int nbuffers = 0;
    int index = 0;
    if (_current_properties->get_color_bits() > 0) {
      if (rbtype & RenderBuffer::T_left) {
        buffers[nbuffers++] = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
      if (_current_properties->is_stereo()) {
        if (rbtype & RenderBuffer::T_right) {
          buffers[nbuffers++] = GL_COLOR_ATTACHMENT0_EXT + index;
        }
        ++index;
      }
    }
    for (int i = 0; i < _current_properties->get_aux_rgba(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_rgba_0 << i)) {
        buffers[nbuffers++] = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_hrgba(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_hrgba_0 << i)) {
        buffers[nbuffers++] = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_float(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_float_0 << i)) {
        buffers[nbuffers++] = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    _glDrawBuffers(nbuffers, buffers);

  } else {

    switch (rbtype & RenderBuffer::T_color) {
    case RenderBuffer::T_front:
      glDrawBuffer(GL_FRONT);
      break;

    case RenderBuffer::T_back:
      glDrawBuffer(GL_BACK);
      break;

    case RenderBuffer::T_right:
      glDrawBuffer(GL_RIGHT);
      break;

    case RenderBuffer::T_left:
      glDrawBuffer(GL_LEFT);
      break;

    case RenderBuffer::T_front_right:
      nassertv(_current_properties->is_stereo());
      glDrawBuffer(GL_FRONT_RIGHT);
      break;

    case RenderBuffer::T_front_left:
      nassertv(_current_properties->is_stereo());
      glDrawBuffer(GL_FRONT_LEFT);
      break;

    case RenderBuffer::T_back_right:
      nassertv(_current_properties->is_stereo());
      glDrawBuffer(GL_BACK_RIGHT);
      break;

    case RenderBuffer::T_back_left:
      nassertv(_current_properties->is_stereo());
      glDrawBuffer(GL_BACK_LEFT);
      break;

    default:
      break;
    }
  }
#endif  // OPENGLES

  // Also ensure that any global color channels are masked out.
  if (gl_color_mask) {
    glColorMask((_color_write_mask & ColorWriteAttrib::C_red) != 0,
                (_color_write_mask & ColorWriteAttrib::C_green) != 0,
                (_color_write_mask & ColorWriteAttrib::C_blue) != 0,
                (_color_write_mask & ColorWriteAttrib::C_alpha) != 0);
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_read_buffer
//       Access: Protected
//  Description: Sets up the glReadBuffer to render into the buffer
//               indicated by the RenderBuffer object.  This only sets
//               up the color bits; it does not affect the depth,
//               stencil, accum layers.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_read_buffer(int rbtype) {
#ifndef OPENGLES  // Draw buffers not supported by OpenGL ES.
  if (rbtype & (RenderBuffer::T_depth | RenderBuffer::T_stencil)) {
    // Special case: don't have to call ReadBuffer for these.
    return;
  }

  if (_current_fbo) {
    GLuint buffer = GL_COLOR_ATTACHMENT0_EXT;
    int index = 1;
    if (_current_properties->is_stereo()) {
      if (rbtype & RenderBuffer::T_right) {
        buffer = GL_COLOR_ATTACHMENT1_EXT;
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_rgba(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_rgba_0 << i)) {
        buffer = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_hrgba(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_hrgba_0 << i)) {
        buffer = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    for (int i = 0; i < _current_properties->get_aux_float(); ++i) {
      if (rbtype & (RenderBuffer::T_aux_float_0 << i)) {
        buffer = GL_COLOR_ATTACHMENT0_EXT + index;
      }
      ++index;
    }
    glReadBuffer(buffer);

  } else {

    switch (rbtype & RenderBuffer::T_color) {
    case RenderBuffer::T_front:
      glReadBuffer(GL_FRONT);
      break;

    case RenderBuffer::T_back:
      glReadBuffer(GL_BACK);
      break;

    case RenderBuffer::T_right:
      glReadBuffer(GL_RIGHT);
      break;

    case RenderBuffer::T_left:
      glReadBuffer(GL_LEFT);
      break;

    case RenderBuffer::T_front_right:
      glReadBuffer(GL_FRONT_RIGHT);
      break;

    case RenderBuffer::T_front_left:
      glReadBuffer(GL_FRONT_LEFT);
      break;

    case RenderBuffer::T_back_right:
      glReadBuffer(GL_BACK_RIGHT);
      break;

    case RenderBuffer::T_back_left:
      glReadBuffer(GL_BACK_LEFT);
      break;

    default:
      break;
    }
  }

  report_my_gl_errors();
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_numeric_type
//       Access: Protected, Static
//  Description: Maps from the Geom's internal numeric type symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_numeric_type(Geom::NumericType numeric_type) {
  switch (numeric_type) {
  case Geom::NT_uint16:
    return GL_UNSIGNED_SHORT;

#ifndef OPENGLES_1
  case Geom::NT_uint32:
    return GL_UNSIGNED_INT;
#endif

  case Geom::NT_uint8:
  case Geom::NT_packed_dcba:
  case Geom::NT_packed_dabc:
    return GL_UNSIGNED_BYTE;

  case Geom::NT_float32:
    return GL_FLOAT;

#ifndef OPENGLES
  case Geom::NT_float64:
    return GL_DOUBLE;
#endif

  case Geom::NT_stdfloat:
    // Shouldn't happen, display error.
    break;
  }

  GLCAT.error()
    << "Invalid NumericType value (" << (int)numeric_type << ")\n";
  return GL_UNSIGNED_BYTE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_target
//       Access: Protected
//  Description: Maps from the Texture's texture type symbols to
//               GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_target(Texture::TextureType texture_type) const {
  switch (texture_type) {
#ifndef OPENGLES
  case Texture::TT_1d_texture:
    return GL_TEXTURE_1D;
#endif

  case Texture::TT_2d_texture:
    return GL_TEXTURE_2D;

  case Texture::TT_3d_texture:
    if (_supports_3d_texture) {
#ifndef OPENGLES_1
      return GL_TEXTURE_3D;
#endif
    } else {
      return GL_NONE;
    }
  case Texture::TT_2d_texture_array:
    if (_supports_2d_texture_array) {
#ifndef OPENGLES
      return GL_TEXTURE_2D_ARRAY_EXT;
#endif
    } else {
      return GL_NONE;
    }
  case Texture::TT_cube_map:
    if (_supports_cube_map) {
      return GL_TEXTURE_CUBE_MAP;
    } else {
      return GL_NONE;
    }
  }

  GLCAT.error() << "Invalid Texture::TextureType value!\n";
  return GL_TEXTURE_2D;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_wrap_mode
//       Access: Protected
//  Description: Maps from the Texture's internal wrap mode symbols to
//               GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_wrap_mode(Texture::WrapMode wm) const {
  if (gl_ignore_clamp) {
    return GL_REPEAT;
  }
  switch (wm) {
  case Texture::WM_clamp:
    return _edge_clamp;

  case Texture::WM_repeat:
    return GL_REPEAT;

  case Texture::WM_mirror:
    return _mirror_repeat;

  case Texture::WM_mirror_once:
    return _mirror_border_clamp;

  case Texture::WM_border_color:
    return _border_clamp;

  case Texture::WM_invalid:
    break;
  }
  GLCAT.error() << "Invalid Texture::WrapMode value!\n";
  return _edge_clamp;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_panda_wrap_mode
//       Access: Protected, Static
//  Description: Maps from the GL's internal wrap mode symbols to
//               Panda's.
////////////////////////////////////////////////////////////////////
Texture::WrapMode CLP(GraphicsStateGuardian)::
get_panda_wrap_mode(GLenum wm) {
  switch (wm) {
#ifndef OPENGLES
  case GL_CLAMP:
#endif
  case GL_CLAMP_TO_EDGE:
    return Texture::WM_clamp;

#ifndef OPENGLES
  case GL_CLAMP_TO_BORDER:
    return Texture::WM_border_color;
#endif

  case GL_REPEAT:
    return Texture::WM_repeat;

#ifndef OPENGLES
  case GL_MIRROR_CLAMP_EXT:
  case GL_MIRROR_CLAMP_TO_EDGE_EXT:
    return Texture::WM_mirror;

  case GL_MIRROR_CLAMP_TO_BORDER_EXT:
    return Texture::WM_mirror_once;
#endif
  }
  GLCAT.error() << "Unexpected GL wrap mode " << (int)wm << "\n";
  return Texture::WM_clamp;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_filter_type
//       Access: Protected, Static
//  Description: Maps from the Texture's internal filter type symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_texture_filter_type(Texture::FilterType ft, bool ignore_mipmaps) {
  if (gl_ignore_filters) {
    return GL_NEAREST;

  } else if (ignore_mipmaps) {
    switch (ft) {
    case Texture::FT_nearest_mipmap_nearest:
    case Texture::FT_nearest:
      return GL_NEAREST;
    case Texture::FT_linear:
    case Texture::FT_linear_mipmap_nearest:
    case Texture::FT_nearest_mipmap_linear:
    case Texture::FT_linear_mipmap_linear:
      return GL_LINEAR;
    case Texture::FT_shadow:
      return GL_LINEAR;
    case Texture::FT_default:
    case Texture::FT_invalid:
      break;
    }

  } else {
    switch (ft) {
    case Texture::FT_nearest:
      return GL_NEAREST;
    case Texture::FT_linear:
      return GL_LINEAR;
    case Texture::FT_nearest_mipmap_nearest:
      return GL_NEAREST_MIPMAP_NEAREST;
    case Texture::FT_linear_mipmap_nearest:
      return GL_LINEAR_MIPMAP_NEAREST;
    case Texture::FT_nearest_mipmap_linear:
      return GL_NEAREST_MIPMAP_LINEAR;
    case Texture::FT_linear_mipmap_linear:
      return GL_LINEAR_MIPMAP_LINEAR;
    case Texture::FT_shadow:
      return GL_LINEAR;
    case Texture::FT_default:
    case Texture::FT_invalid:
      break;
    }
  }
  GLCAT.error() << "Invalid Texture::FilterType value!\n";
  return GL_NEAREST;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_panda_filter_type
//       Access: Protected, Static
//  Description: Maps from the GL's internal filter type symbols
//               to Panda's.
////////////////////////////////////////////////////////////////////
Texture::FilterType CLP(GraphicsStateGuardian)::
get_panda_filter_type(GLenum ft) {
  switch (ft) {
  case GL_NEAREST:
    return Texture::FT_nearest;
  case GL_LINEAR:
    return Texture::FT_linear;
  case GL_NEAREST_MIPMAP_NEAREST:
    return Texture::FT_nearest_mipmap_nearest;
  case GL_LINEAR_MIPMAP_NEAREST:
    return Texture::FT_linear_mipmap_nearest;
  case GL_NEAREST_MIPMAP_LINEAR:
    return Texture::FT_nearest_mipmap_linear;
  case GL_LINEAR_MIPMAP_LINEAR:
    return Texture::FT_linear_mipmap_linear;
  }
  GLCAT.error() << "Unexpected GL filter type " << (int)ft << "\n";
  return Texture::FT_linear;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_component_type
//       Access: Protected, Static
//  Description: Maps from the Texture's internal ComponentType symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_component_type(Texture::ComponentType component_type) {
  switch (component_type) {
  case Texture::T_unsigned_byte:
    return GL_UNSIGNED_BYTE;
  case Texture::T_unsigned_short:
    return GL_UNSIGNED_SHORT;
  case Texture::T_float:
    return GL_FLOAT;
  case Texture::T_unsigned_int_24_8:
    if (_supports_depth_stencil) {
      return GL_UNSIGNED_INT_24_8_EXT;
    } else {
      return GL_UNSIGNED_BYTE;
    }
  case Texture::T_int:
#ifndef OPENGLES_1
    return GL_INT;
#endif
  default:
    GLCAT.error() << "Invalid Texture::Type value!\n";
    return GL_UNSIGNED_BYTE;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_external_image_format
//       Access: Protected
//  Description: Maps from the Texture's Format symbols
//               to GL's.
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_external_image_format(Texture *tex) const {
  Texture::CompressionMode compression = tex->get_ram_image_compression();
  Texture::Format format = tex->get_format();
  if (compression != Texture::CM_off &&
      get_supports_compressed_texture_format(compression)) {
    switch (compression) {
    case Texture::CM_on:
#ifndef OPENGLES
      switch (format) {
      case Texture::F_color_index:
      case Texture::F_depth_component:
      case Texture::F_depth_component16:
      case Texture::F_depth_component24:
      case Texture::F_depth_component32:
      case Texture::F_depth_stencil:
        // This shouldn't be possible.
        nassertr(false, GL_RGB);
        break;

      case Texture::F_rgba:
      case Texture::F_rgbm:
      case Texture::F_rgba4:
      case Texture::F_rgba8:
      case Texture::F_rgba12:
      case Texture::F_rgba16:
      case Texture::F_rgba32:
        return GL_COMPRESSED_RGBA;

      case Texture::F_rgb:
      case Texture::F_rgb5:
      case Texture::F_rgba5:
      case Texture::F_rgb8:
      case Texture::F_rgb12:
      case Texture::F_rgb332:
      case Texture::F_rgb16:
      case Texture::F_rgb32:
        return GL_COMPRESSED_RGB;

      case Texture::F_alpha:
        return GL_COMPRESSED_ALPHA;

      case Texture::F_red:
      case Texture::F_green:
      case Texture::F_blue:
      case Texture::F_r16:
      case Texture::F_r32:
      case Texture::F_r32i:
        return GL_COMPRESSED_RED;

      case Texture::F_rg16:
      case Texture::F_rg32:
        return GL_COMPRESSED_RG;

      case Texture::F_luminance:
        return GL_COMPRESSED_LUMINANCE;

      case Texture::F_luminance_alpha:
      case Texture::F_luminance_alphamask:
        return GL_COMPRESSED_LUMINANCE_ALPHA;

      case Texture::F_srgb:
        return GL_COMPRESSED_SRGB;

      case Texture::F_srgb_alpha:
        return GL_COMPRESSED_SRGB_ALPHA;

      case Texture::F_sluminance:
        return GL_COMPRESSED_SLUMINANCE;

      case Texture::F_sluminance_alpha:
        return GL_COMPRESSED_SLUMINANCE_ALPHA;
      }
#endif
      break;

    case Texture::CM_dxt1:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
      } else
#endif
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
      } else {
        return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
      }

#ifndef OPENGLES
    case Texture::CM_dxt3:
      if (format == Texture::F_srgb || format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT;
      } else {
        return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
      }

    case Texture::CM_dxt5:
      if (format == Texture::F_srgb || format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
      } else {
        return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
      }

    case Texture::CM_fxt1:
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_FXT1_3DFX;
      } else {
        return GL_COMPRESSED_RGB_FXT1_3DFX;
      }

#else
    case Texture::CM_pvr1_2bpp:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT;
      } else
#endif  // OPENGLES_1
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG;
      } else {
        return GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG;
      }

    case Texture::CM_pvr1_4bpp:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT;
      } else
#endif  // OPENGLES_1
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
      } else {
        return GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG;
      }
#endif

    case Texture::CM_default:
    case Texture::CM_off:
    case Texture::CM_dxt2:
    case Texture::CM_dxt4:
      // This shouldn't happen.
      nassertr(false, GL_RGB);
      break;
    }
  }

  switch (format) {
#ifndef OPENGLES
  case Texture::F_color_index:
    return GL_COLOR_INDEX;
#endif
  case Texture::F_depth_component:
  case Texture::F_depth_component16:
  case Texture::F_depth_component24:
  case Texture::F_depth_component32:
    return GL_DEPTH_COMPONENT;
  case Texture::F_depth_stencil:
    return _supports_depth_stencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
#ifndef OPENGLES
  case Texture::F_red:
  case Texture::F_r16:
  case Texture::F_r32:
    return GL_RED;
  case Texture::F_green:
    return GL_GREEN;
  case Texture::F_blue:
    return GL_BLUE;
#endif
  case Texture::F_alpha:
    return GL_ALPHA;
#ifndef OPENGLES_1
  case Texture::F_rg16:
  case Texture::F_rg32:
    return GL_RG;
#endif
  case Texture::F_rgb:
  case Texture::F_rgb5:
  case Texture::F_rgb8:
  case Texture::F_rgb12:
  case Texture::F_rgb332:
  case Texture::F_rgb16:
  case Texture::F_rgb32:
  case Texture::F_srgb:
#ifdef OPENGLES
    return GL_RGB;
#else
    return _supports_bgr ? GL_BGR : GL_RGB;
#endif
  case Texture::F_rgba:
  case Texture::F_rgbm:
  case Texture::F_rgba4:
  case Texture::F_rgba5:
  case Texture::F_rgba8:
  case Texture::F_rgba12:
  case Texture::F_rgba16:
  case Texture::F_rgba32:
  case Texture::F_srgb_alpha:
#ifdef OPENGLES_2
    return GL_RGBA;
#else
    return _supports_bgr ? GL_BGRA : GL_RGBA;
#endif
  case Texture::F_luminance:
  case Texture::F_sluminance:
    return GL_LUMINANCE;
  case Texture::F_luminance_alphamask:
  case Texture::F_luminance_alpha:
  case Texture::F_sluminance_alpha:
    return GL_LUMINANCE_ALPHA;

#ifndef OPENGLES
  case Texture::F_r32i:
    return GL_RED_INTEGER;
#endif
  }
  GLCAT.error()
    << "Invalid Texture::Format value in get_external_image_format(): "
    << (int)tex->get_format() << "\n";
  return GL_RGB;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_internal_image_format
//       Access: Protected
//  Description: Maps from the Texture's Format symbols to a
//               suitable internal format for GL textures.
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_internal_image_format(Texture *tex) const {
  Texture::CompressionMode compression = tex->get_compression();
  if (compression == Texture::CM_default) {
    compression = (compressed_textures) ? Texture::CM_on : Texture::CM_off;
  }
  Texture::Format format = tex->get_format();
  if (tex->get_render_to_texture()) {
    // no compression for render targets
    compression = Texture::CM_off;
  }
  bool is_3d = (tex->get_texture_type() == Texture::TT_3d_texture ||
                tex->get_texture_type() == Texture::TT_2d_texture_array);

  if (get_supports_compressed_texture_format(compression)) {
    switch (compression) {
    // For now, we don't support generic compression with OpenGL ES.
#ifndef OPENGLES
    case Texture::CM_on:
      // The user asked for just generic compression.  OpenGL supports
      // requesting just generic compression, but we'd like to go ahead
      // and request a specific type (if we can figure out an
      // appropriate choice), since that makes saving the result as a
      // pre-compressed texture more dependable--this way, we will know
      // which compression algorithm was applied.
      switch (format) {
      case Texture::F_color_index:
      case Texture::F_depth_component:
      case Texture::F_depth_component16:
      case Texture::F_depth_component24:
      case Texture::F_depth_component32:
      case Texture::F_depth_stencil:
      case Texture::F_r32i:
        // Unsupported; fall through to below.
        break;

      case Texture::F_rgbm:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_FXT1_3DFX;
        }
        return GL_COMPRESSED_RGBA;

      case Texture::F_rgba4:
        if (get_supports_compressed_texture_format(Texture::CM_dxt3) && !is_3d) {
          return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_FXT1_3DFX;
        }
        return GL_COMPRESSED_RGBA;

      case Texture::F_rgba:
      case Texture::F_rgba8:
      case Texture::F_rgba12:
      case Texture::F_rgba16:
      case Texture::F_rgba32:
        if (get_supports_compressed_texture_format(Texture::CM_dxt5) && !is_3d) {
          return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_FXT1_3DFX;
        }
        return GL_COMPRESSED_RGBA;

      case Texture::F_rgb:
      case Texture::F_rgb5:
      case Texture::F_rgba5:
      case Texture::F_rgb8:
      case Texture::F_rgb12:
      case Texture::F_rgb332:
      case Texture::F_rgb16:
      case Texture::F_rgb32:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_FXT1_3DFX;
        }
        return GL_COMPRESSED_RGB;

      case Texture::F_alpha:
        if (get_supports_compressed_texture_format(Texture::CM_dxt5) && !is_3d) {
          return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_FXT1_3DFX;
        }
        return GL_COMPRESSED_ALPHA;

      case Texture::F_red:
      case Texture::F_green:
      case Texture::F_blue:
      case Texture::F_r16:
      case Texture::F_r32:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_FXT1_3DFX;
        }
        return GL_COMPRESSED_RED;

      case Texture::F_rg16:
      case Texture::F_rg32:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_FXT1_3DFX;
        }
        return GL_COMPRESSED_RG;

      case Texture::F_luminance:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGB_FXT1_3DFX;
        }
        return GL_COMPRESSED_LUMINANCE;

      case Texture::F_luminance_alpha:
      case Texture::F_luminance_alphamask:
        if (get_supports_compressed_texture_format(Texture::CM_dxt5) && !is_3d) {
          return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        } else if (get_supports_compressed_texture_format(Texture::CM_fxt1) && !is_3d) {
          return GL_COMPRESSED_RGBA_FXT1_3DFX;
        }
        return GL_COMPRESSED_LUMINANCE_ALPHA;

      case Texture::F_srgb:
        if (get_supports_compressed_texture_format(Texture::CM_dxt1) && !is_3d) {
          return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
        }
        return GL_COMPRESSED_SRGB;

      case Texture::F_srgb_alpha:
        if (get_supports_compressed_texture_format(Texture::CM_dxt5) && !is_3d) {
          return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
        }
        return GL_COMPRESSED_SRGB_ALPHA;

      case Texture::F_sluminance:
        return GL_COMPRESSED_SLUMINANCE;

      case Texture::F_sluminance_alpha:
        return GL_COMPRESSED_SLUMINANCE_ALPHA;
      }
      break;
#endif

    case Texture::CM_dxt1:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
      } else
#endif
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
      } else {
        return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
      }

#ifndef OPENGLES
    case Texture::CM_dxt3:
      if (format == Texture::F_srgb || format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT;
      } else {
        return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
      }

    case Texture::CM_dxt5:
      if (format == Texture::F_srgb || format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
      } else {
        return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
      }

    case Texture::CM_fxt1:
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_FXT1_3DFX;
      } else {
        return GL_COMPRESSED_RGB_FXT1_3DFX;
      }
#else
    case Texture::CM_pvr1_2bpp:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT;
      } else
#endif  // OPENGLES_1
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG;
      } else {
        return GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG;
      }

    case Texture::CM_pvr1_4bpp:
#ifndef OPENGLES_1
      if (format == Texture::F_srgb_alpha) {
        return GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT;
      } else if (format == Texture::F_srgb) {
        return GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT;
      } else
#endif  // OPENGLES_1
      if (Texture::has_alpha(format)) {
        return GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
      } else {
        return GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG;
      }
#endif

    case Texture::CM_default:
    case Texture::CM_off:
    case Texture::CM_dxt2:
    case Texture::CM_dxt4:
      // No compression: fall through to below.
      break;
    }
  }

  switch (format) {
#ifndef OPENGLES
  case Texture::F_color_index:
    return GL_COLOR_INDEX;
#endif

  case Texture::F_depth_stencil:
    if (_supports_depth_stencil) {
#ifndef OPENGLES_1
      if (tex->get_component_type() == Texture::T_float) {
        return GL_DEPTH32F_STENCIL8;
      } else
#endif
      {
        return GL_DEPTH_STENCIL;
      }
    }
    // Fall through.

  case Texture::F_depth_component:
#ifndef OPENGLES
    if (tex->get_component_type() == Texture::T_float) {
      return GL_DEPTH_COMPONENT32F;
    } else
#endif
    {
      return GL_DEPTH_COMPONENT;
    }
  case Texture::F_depth_component16:
#ifdef OPENGLES
    return GL_DEPTH_COMPONENT16_OES;
#else
    return GL_DEPTH_COMPONENT16;
#endif

  case Texture::F_depth_component24:
#ifdef OPENGLES
    if (_supports_depth24) {
      return GL_DEPTH_COMPONENT24_OES;
    } else {
      return GL_DEPTH_COMPONENT16_OES;
    }
#else
    return GL_DEPTH_COMPONENT24;
#endif

  case Texture::F_depth_component32:
#ifdef OPENGLES
    if (_supports_depth32) {
      return GL_DEPTH_COMPONENT32_OES;
    } else if (_supports_depth24) {
      return GL_DEPTH_COMPONENT24_OES;
    } else {
      return GL_DEPTH_COMPONENT16_OES;
    }
#else
    if (tex->get_component_type() == Texture::T_float) {
      return GL_DEPTH_COMPONENT32F;
    } else {
      return GL_DEPTH_COMPONENT32;
    }
#endif

  case Texture::F_rgba:
  case Texture::F_rgbm:
#ifndef OPENGLES_1
    if (tex->get_component_type() == Texture::T_float) {
      return GL_RGBA16F;
    } else
#endif
    {
      return GL_RGBA;
    }

  case Texture::F_rgba4:
    return GL_RGBA4;

#ifdef OPENGLES
  case Texture::F_rgba8:
    return GL_RGBA8_OES;
  case Texture::F_rgba12:
    return GL_RGBA;
#else
  case Texture::F_rgba8:
    return GL_RGBA8;
  case Texture::F_rgba12:
    return GL_RGBA12;
#endif  // OPENGLES
#ifndef OPENGLES_1
  case Texture::F_rgba16:
    return GL_RGBA16F;
  case Texture::F_rgba32:
    return GL_RGBA32F;
#endif  // OPENGLES

  case Texture::F_rgb:
    if (tex->get_component_type() == Texture::T_float) {
      return GL_RGB16F;
    } else {
      return GL_RGB;
    }

  case Texture::F_rgb5:
#ifdef OPENGLES
    // Close enough.
    return GL_RGB565_OES;
#else
    return GL_RGB5;
#endif
  case Texture::F_rgba5:
    return GL_RGB5_A1;

#ifdef OPENGLES
  case Texture::F_rgb8:
    return GL_RGB8_OES;
  case Texture::F_rgb12:
    return GL_RGB;
  case Texture::F_rgb16:
    return GL_RGB16F;
#else
  case Texture::F_rgb8:
    return GL_RGB8;
  case Texture::F_rgb12:
    return GL_RGB12;
  case Texture::F_rgb16:
    if (tex->get_component_type() == Texture::T_float) {
      return GL_RGB16F;
    } else {
      return GL_RGB16;
    }
#endif  // OPENGLES
  case Texture::F_rgb32:
    return GL_RGB32F;

#ifndef OPENGLES
  case Texture::F_rgb332:
    return GL_R3_G3_B2;
#endif

#if defined(OPENGLES_2)
  case Texture::F_r16:
    return GL_R16F_EXT;
  case Texture::F_rg16:
    return GL_RG16F_EXT;
#elif !defined(OPENGLES_1)
  case Texture::F_r16:
    if (tex->get_component_type() == Texture::T_float) {
      return GL_R16F;
    } else {
      return GL_R16;
    }
  case Texture::F_rg16:
    if (tex->get_component_type() == Texture::T_float) {
      return GL_RG16F;
    } else {
      return GL_RG16;
    }
#endif

#ifndef OPENGLES_1
  case Texture::F_r32:
    return GL_R32F;
  case Texture::F_rg32:
    return GL_RG32F;

  case Texture::F_red:
  case Texture::F_green:
  case Texture::F_blue:
    return GL_RED;
#endif

  case Texture::F_alpha:
    return GL_ALPHA;
  case Texture::F_luminance:
    return GL_LUMINANCE;
  case Texture::F_luminance_alpha:
  case Texture::F_luminance_alphamask:
    return GL_LUMINANCE_ALPHA;

#ifndef OPENGLES_1
  case Texture::F_srgb:
    return GL_SRGB8;
  case Texture::F_srgb_alpha:
    return GL_SRGB8_ALPHA8;
  case Texture::F_sluminance:
    return GL_SLUMINANCE8;
  case Texture::F_sluminance_alpha:
    return GL_SLUMINANCE8_ALPHA8;
#endif

#ifndef OPENGLES
  case Texture::F_r32i:
    return GL_R32I;
#endif

  default:
    GLCAT.error()
      << "Invalid image format in get_internal_image_format(): "
      << (int)tex->get_format() << "\n";
    return GL_RGB;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::is_mipmap_filter
//       Access: Protected, Static
//  Description: Returns true if the indicated GL minfilter type
//               represents a mipmap format, false otherwise.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
is_mipmap_filter(GLenum min_filter) {
  switch (min_filter) {
  case GL_NEAREST_MIPMAP_NEAREST:
  case GL_LINEAR_MIPMAP_NEAREST:
  case GL_NEAREST_MIPMAP_LINEAR:
  case GL_LINEAR_MIPMAP_LINEAR:
    return true;

  default:
    return false;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::is_compressed_format
//       Access: Protected, Static
//  Description: Returns true if the indicated GL internal format
//               represents a compressed texture format, false
//               otherwise.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
is_compressed_format(GLenum format) {
  switch (format) {
  case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
  case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
#ifdef OPENGLES
  case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
  case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
  case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
  case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
#else
  case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
  case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
  case GL_COMPRESSED_RGB_FXT1_3DFX:
  case GL_COMPRESSED_RGBA_FXT1_3DFX:

  case GL_COMPRESSED_RGB:
  case GL_COMPRESSED_RGBA:
  case GL_COMPRESSED_ALPHA:
  case GL_COMPRESSED_LUMINANCE:
  case GL_COMPRESSED_LUMINANCE_ALPHA:
#endif
    return true;

  default:
    return false;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_apply_mode_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's mode types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_apply_mode_type(TextureStage::Mode am) {
#ifndef OPENGLES_2
  switch (am) {
  case TextureStage::M_modulate: return GL_MODULATE;
  case TextureStage::M_decal: return GL_DECAL;
  case TextureStage::M_blend: return GL_BLEND;
  case TextureStage::M_replace: return GL_REPLACE;
  case TextureStage::M_add: return GL_ADD;
  case TextureStage::M_combine: return GL_COMBINE;
  case TextureStage::M_blend_color_scale: return GL_BLEND;
  case TextureStage::M_modulate_glow: return GL_MODULATE;
  case TextureStage::M_modulate_gloss: return GL_MODULATE;
  default:
    // Other modes shouldn't get here.  Fall through and error.
    break;
  }

  GLCAT.error()
    << "Invalid TextureStage::Mode value" << endl;
  return GL_MODULATE;
#else
  return 0;
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_combine_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's CombineMode types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_combine_type(TextureStage::CombineMode cm) {
#ifndef OPENGLES_2
  switch (cm) {
  case TextureStage::CM_undefined: // fall through
  case TextureStage::CM_replace: return GL_REPLACE;
  case TextureStage::CM_modulate: return GL_MODULATE;
  case TextureStage::CM_add: return GL_ADD;
  case TextureStage::CM_add_signed: return GL_ADD_SIGNED;
  case TextureStage::CM_interpolate: return GL_INTERPOLATE;
  case TextureStage::CM_subtract: return GL_SUBTRACT;
  case TextureStage::CM_dot3_rgb: return GL_DOT3_RGB;
  case TextureStage::CM_dot3_rgba: return GL_DOT3_RGBA;
  }
  GLCAT.error()
    << "Invalid TextureStage::CombineMode value" << endl;
#endif
  return GL_REPLACE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_src_type
//       Access: Protected
//  Description: Maps from the texture stage's CombineSource types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_src_type(TextureStage::CombineSource cs,
                     int last_stage, int last_saved_result,
                     int this_stage) const {
#ifndef OPENGLES_2
  switch (cs) {
  case TextureStage::CS_undefined: // fall through
  case TextureStage::CS_texture: return GL_TEXTURE;
  case TextureStage::CS_constant: return GL_CONSTANT;
  case TextureStage::CS_primary_color: return GL_PRIMARY_COLOR;
  case TextureStage::CS_constant_color_scale: return GL_CONSTANT;

  case TextureStage::CS_previous:
    if (last_stage == this_stage - 1) {
      return GL_PREVIOUS;
    } else if (last_stage == -1) {
      return GL_PRIMARY_COLOR;
    } else if (_supports_texture_saved_result) {
      return GL_TEXTURE0 + last_stage;
    } else {
      GLCAT.warning()
        << "Current OpenGL driver does not support texture crossbar blending.\n";
      return GL_PRIMARY_COLOR;
    }

  case TextureStage::CS_last_saved_result:
    if (last_saved_result == this_stage - 1) {
      return GL_PREVIOUS;
    } else if (last_saved_result == -1) {
      return GL_PRIMARY_COLOR;
    } else if (_supports_texture_saved_result) {
      return GL_TEXTURE0 + last_saved_result;
    } else {
      GLCAT.warning()
        << "Current OpenGL driver does not support texture crossbar blending.\n";
      return GL_PRIMARY_COLOR;
    }
  }

  GLCAT.error()
    << "Invalid TextureStage::CombineSource value" << endl;
#endif
  return GL_TEXTURE;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_operand_type
//       Access: Protected, Static
//  Description: Maps from the texture stage's CombineOperand types
//               to the corresponding OpenGL ids
////////////////////////////////////////////////////////////////////
GLint CLP(GraphicsStateGuardian)::
get_texture_operand_type(TextureStage::CombineOperand co) {
  switch (co) {
  case TextureStage::CO_undefined: // fall through
  case TextureStage::CO_src_alpha: return GL_SRC_ALPHA;
  case TextureStage::CO_one_minus_src_alpha: return GL_ONE_MINUS_SRC_ALPHA;
  case TextureStage::CO_src_color: return GL_SRC_COLOR;
  case TextureStage::CO_one_minus_src_color: return GL_ONE_MINUS_SRC_COLOR;
  }

  GLCAT.error()
    << "Invalid TextureStage::CombineOperand value" << endl;
  return GL_SRC_COLOR;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_fog_mode_type
//       Access: Protected, Static
//  Description: Maps from the fog types to gl version
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_fog_mode_type(Fog::Mode m) {
  switch(m) {
  case Fog::M_linear: return GL_LINEAR;
#ifndef OPENGLES_2
  case Fog::M_exponential: return GL_EXP;
  case Fog::M_exponential_squared: return GL_EXP2;
#endif
    /*
      case Fog::M_spline: return GL_FOG_FUNC_SGIS;
    */

  default:
    GLCAT.error() << "Invalid Fog::Mode value" << endl;
#ifdef OPENGLES_2
    return GL_LINEAR;
#else
    return GL_EXP;
#endif
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_blend_equation_type
//       Access: Protected, Static
//  Description: Maps from ColorBlendAttrib::Mode to glBlendEquation
//               value.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_blend_equation_type(ColorBlendAttrib::Mode mode) {
  switch (mode) {
  case ColorBlendAttrib::M_none:
  case ColorBlendAttrib::M_add:
    return GL_FUNC_ADD;

  case ColorBlendAttrib::M_subtract:
    return GL_FUNC_SUBTRACT;

  case ColorBlendAttrib::M_inv_subtract:
    return GL_FUNC_REVERSE_SUBTRACT;

#ifndef OPENGLES
  case ColorBlendAttrib::M_min:
    return GL_MIN;

  case ColorBlendAttrib::M_max:
    return GL_MAX;
#endif
  }

  GLCAT.error()
    << "Unknown color blend mode " << (int)mode << endl;
  return GL_FUNC_ADD;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_blend_func
//       Access: Protected, Static
//  Description: Maps from ColorBlendAttrib::Operand to glBlendFunc
//               value.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_blend_func(ColorBlendAttrib::Operand operand) {
  switch (operand) {
  case ColorBlendAttrib::O_zero:
    return GL_ZERO;

  case ColorBlendAttrib::O_one:
    return GL_ONE;

  case ColorBlendAttrib::O_incoming_color:
    return GL_SRC_COLOR;

  case ColorBlendAttrib::O_one_minus_incoming_color:
    return GL_ONE_MINUS_SRC_COLOR;

  case ColorBlendAttrib::O_fbuffer_color:
    return GL_DST_COLOR;

  case ColorBlendAttrib::O_one_minus_fbuffer_color:
    return GL_ONE_MINUS_DST_COLOR;

  case ColorBlendAttrib::O_incoming_alpha:
    return GL_SRC_ALPHA;

  case ColorBlendAttrib::O_one_minus_incoming_alpha:
    return GL_ONE_MINUS_SRC_ALPHA;

  case ColorBlendAttrib::O_fbuffer_alpha:
    return GL_DST_ALPHA;

  case ColorBlendAttrib::O_one_minus_fbuffer_alpha:
    return GL_ONE_MINUS_DST_ALPHA;

#ifndef OPENGLES_1
  case ColorBlendAttrib::O_constant_color:
  case ColorBlendAttrib::O_color_scale:
    return GL_CONSTANT_COLOR;

  case ColorBlendAttrib::O_one_minus_constant_color:
  case ColorBlendAttrib::O_one_minus_color_scale:
    return GL_ONE_MINUS_CONSTANT_COLOR;

  case ColorBlendAttrib::O_constant_alpha:
  case ColorBlendAttrib::O_alpha_scale:
    return GL_CONSTANT_ALPHA;

  case ColorBlendAttrib::O_one_minus_constant_alpha:
  case ColorBlendAttrib::O_one_minus_alpha_scale:
    return GL_ONE_MINUS_CONSTANT_ALPHA;
#endif

  case ColorBlendAttrib::O_incoming_color_saturate:
    return GL_SRC_ALPHA_SATURATE;
  }

  GLCAT.error()
    << "Unknown color blend operand " << (int)operand << endl;
  return GL_ZERO;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_usage
//       Access: Public, Static
//  Description: Maps from UsageHint to the GL symbol.
////////////////////////////////////////////////////////////////////
GLenum CLP(GraphicsStateGuardian)::
get_usage(Geom::UsageHint usage_hint) {
  switch (usage_hint) {
  case Geom::UH_stream:
#ifdef OPENGLES
    return GL_DYNAMIC_DRAW;
#else
    return GL_STREAM_DRAW;
#endif  // OPENGLES

  case Geom::UH_static:
  case Geom::UH_unspecified:
    return GL_STATIC_DRAW;

  case Geom::UH_dynamic:
    return GL_DYNAMIC_DRAW;

  case Geom::UH_client:
    break;
  }

  GLCAT.error()
    << "Unexpected usage_hint " << (int)usage_hint << endl;
  return GL_STATIC_DRAW;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::print_gfx_visual
//       Access: Public
//  Description: Prints a description of the current visual selected.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
print_gfx_visual() {
  GLint i;
  GLboolean j;
  cout << "Graphics Visual Info (# bits of each):" << endl;

  cout << "RGBA: ";
  glGetIntegerv( GL_RED_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_GREEN_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_BLUE_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_ALPHA_BITS, &i ); cout << i << endl;

#ifndef OPENGLES
  cout << "Accum RGBA: ";
  glGetIntegerv( GL_ACCUM_RED_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_ACCUM_GREEN_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_ACCUM_BLUE_BITS, &i ); cout << i << " ";
  glGetIntegerv( GL_ACCUM_ALPHA_BITS, &i ); cout << i << endl;

  glGetIntegerv( GL_INDEX_BITS, &i ); cout << "Color Index: " << i << endl;
#endif

  glGetIntegerv( GL_DEPTH_BITS, &i ); cout << "Depth: " << i << endl;
  glGetIntegerv( GL_ALPHA_BITS, &i ); cout << "Alpha: " << i << endl;
  glGetIntegerv( GL_STENCIL_BITS, &i ); cout << "Stencil: " << i << endl;

#ifndef OPENGLES
  glGetBooleanv( GL_DOUBLEBUFFER, &j ); cout << "DoubleBuffer? "
                                                << (int)j << endl;

  glGetBooleanv( GL_STEREO, &j ); cout << "Stereo? " << (int)j << endl;
#endif

  if (_supports_multisample) {
#ifndef OPENGLES_2
    glGetBooleanv( GL_MULTISAMPLE, &j ); cout << "Multisample? " << (int)j << endl;
#endif
    glGetIntegerv( GL_SAMPLES, &i ); cout << "Samples: " << i << endl;
  }

  glGetBooleanv( GL_BLEND, &j ); cout << "Blend? " << (int)j << endl;
#ifndef OPENGLES_2
  glGetBooleanv( GL_POINT_SMOOTH, &j ); cout << "Point Smooth? "
                                                << (int)j << endl;
  glGetBooleanv( GL_LINE_SMOOTH, &j ); cout << "Line Smooth? "
                                               << (int)j << endl;
#endif

#ifndef OPENGLES
  glGetIntegerv( GL_AUX_BUFFERS, &i ); cout << "Aux Buffers: " << i << endl;
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_light_color
//       Access: Public
//  Description: Returns the value that that should be issued as the
//               light's color, as scaled by the current value of
//               _light_color_scale, in the case of
//               color_scale_via_lighting.
////////////////////////////////////////////////////////////////////
LVecBase4 CLP(GraphicsStateGuardian)::
get_light_color(Light *light) const {
#ifndef NDEBUG
  if (_show_texture_usage) {
    // In show_texture_usage mode, all lights are white, so as not to
    // contaminate the texture color.
    return LVecBase4(1.0, 1.0, 1.0, 1.0);
  }
#endif  // NDEBUG

  const LColor &c = light->get_color();

  LVecBase4 light_color(c[0] * _light_color_scale[0],
                        c[1] * _light_color_scale[1],
                        c[2] * _light_color_scale[2],
                        c[3] * _light_color_scale[3]);
  return light_color;
}


////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::reissue_transforms
//       Access: Protected, Virtual
//  Description: Called by clear_state_and_transform() to ensure that
//               the current modelview and projection matrices are
//               properly loaded in the graphics state, after a
//               callback might have mucked them up.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
reissue_transforms() {
  prepare_lens();
  do_issue_transform();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_lighting
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable or disable the use of lighting overall.  This
//               is called by do_issue_light() according to whether any
//               lights are in use or not.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_lighting(bool enable) {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_enable_lighting_pcollector("Draw:Set State:Light:Enable lighting");
  //  PStatGPUTimer timer(this, _draw_set_state_light_enable_lighting_pcollector);

  if (enable) {
    glEnable(GL_LIGHTING);
  } else {
    glDisable(GL_LIGHTING);
  }
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_ambient_light
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               indicate the color of the ambient light that should
//               be in effect.  This is called by do_issue_light() after
//               all other lights have been enabled or disabled.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_ambient_light(const LColor &color) {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_ambient_pcollector("Draw:Set State:Light:Ambient");
  //  PStatGPUTimer timer(this, _draw_set_state_light_ambient_pcollector);

  LColor c = color;
  c.set(c[0] * _light_color_scale[0],
        c[1] * _light_color_scale[1],
        c[2] * _light_color_scale[2],
        c[3] * _light_color_scale[3]);
  call_glLightModelfv(GL_LIGHT_MODEL_AMBIENT, c);
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_light
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable the indicated light id.  A specific Light will
//               already have been bound to this id via bind_light().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_light(int light_id, bool enable) {
  //  static PStatCollector _draw_set_state_light_enable_light_pcollector("Draw:Set State:Light:Enable light");
  //  PStatGPUTimer timer(this, _draw_set_state_light_enable_light_pcollector);

  if (enable) {
    glEnable(get_light_id(light_id));
  } else {
    glDisable(get_light_id(light_id));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_bind_lights
//       Access: Protected, Virtual
//  Description: Called immediately before bind_light() is called,
//               this is intended to provide the derived class a hook
//               in which to set up some state (like transform) that
//               might apply to several lights.
//
//               The sequence is: begin_bind_lights() will be called,
//               then one or more bind_light() calls, then
//               end_bind_lights().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
begin_bind_lights() {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_begin_bind_pcollector("Draw:Set State:Light:Begin bind");
  //  PStatGPUTimer timer(this, _draw_set_state_light_begin_bind_pcollector);

  // We need to temporarily load a new matrix so we can define the
  // light in a known coordinate system.  We pick the transform of the
  // root.  (Alternatively, we could leave the current transform where
  // it is and compute the light position relative to that transform
  // instead of relative to the root, by composing with the matrix
  // computed by _internal_transform->invert_compose(render_transform).
  // But I think loading a completely new matrix is simpler.)
  CPT(TransformState) render_transform =
    _cs_transform->compose(_scene_setup->get_world_transform());

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  GLPf(LoadMatrix)(render_transform->get_mat().get_data());
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_bind_lights
//       Access: Protected, Virtual
//  Description: Called after before bind_light() has been called one
//               or more times (but before any geometry is issued or
//               additional state is changed), this is intended to
//               clean up any temporary changes to the state that may
//               have been made by begin_bind_lights().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_bind_lights() {
#ifndef OPENGLES_2
  //  static PStatCollector _draw_set_state_light_end_bind_pcollector("Draw:Set State:Light:End bind");
  //  PStatGPUTimer timer(this, _draw_set_state_light_end_bind_pcollector);

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::enable_clip_plane
//       Access: Protected, Virtual
//  Description: Intended to be overridden by a derived class to
//               enable the indicated clip_plane id.  A specific
//               PlaneNode will already have been bound to this id via
//               bind_clip_plane().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
enable_clip_plane(int plane_id, bool enable) {
  if (enable) {
    glEnable(get_clip_plane_id(plane_id));
  } else {
    glDisable(get_clip_plane_id(plane_id));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::begin_bind_clip_planes
//       Access: Protected, Virtual
//  Description: Called immediately before bind_clip_plane() is called,
//               this is intended to provide the derived class a hook
//               in which to set up some state (like transform) that
//               might apply to several clip_planes.
//
//               The sequence is: begin_bind_clip_planes() will be called,
//               then one or more bind_clip_plane() calls, then
//               end_bind_clip_planes().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
begin_bind_clip_planes() {
#ifndef OPENGLES_2
  // We need to temporarily load a new matrix so we can define the
  // clip_plane in a known coordinate system.  We pick the transform of the
  // root.  (Alternatively, we could leave the current transform where
  // it is and compute the clip_plane position relative to that transform
  // instead of relative to the root, by composing with the matrix
  // computed by _internal_transform->invert_compose(render_transform).
  // But I think loading a completely new matrix is simpler.)
  CPT(TransformState) render_transform =
    _cs_transform->compose(_scene_setup->get_world_transform());

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  GLPf(LoadMatrix)(render_transform->get_mat().get_data());
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_clip_plane
//       Access: Protected, Virtual
//  Description: Called the first time a particular clip_plane has been
//               bound to a given id within a frame, this should set
//               up the associated hardware clip_plane with the clip_plane's
//               properties.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_clip_plane(const NodePath &plane, int plane_id) {
  GLenum id = get_clip_plane_id(plane_id);

  CPT(TransformState) transform = plane.get_transform(_scene_setup->get_scene_root().get_parent());
  const PlaneNode *plane_node;
  DCAST_INTO_V(plane_node, plane.node());
  LPlane xformed_plane = plane_node->get_plane() * transform->get_mat();

#ifndef OPENGLES_2 // OpenGL ES 2.0 doesn't support clip planes at all.
#ifdef OPENGLES
  // OpenGL ES uses a single-precision call.
  LPlanef single_plane(LCAST(float, xformed_plane));
  glClipPlanef(id, single_plane.get_data());
#else
  // Mainline OpenGL uses a double-precision call.
  LPlaned double_plane(LCAST(double, xformed_plane));
  glClipPlane(id, double_plane.get_data());
#endif  // OPENGLES
#endif  // OPENGLES_2

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::end_bind_clip_planes
//       Access: Protected, Virtual
//  Description: Called after before bind_clip_plane() has been called one
//               or more times (but before any geometry is issued or
//               additional state is changed), this is intended to
//               clean up any temporary changes to the state that may
//               have been made by begin_bind_clip_planes().
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
end_bind_clip_planes() {
#ifndef OPENGLES_2
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::set_state_and_transform
//       Access: Public, Virtual
//  Description: Simultaneously resets the render state and the
//               transform state.
//
//               This transform specified is the "internal" net
//               transform, already converted into the GSG's internal
//               coordinate space by composing it to
//               get_cs_transform().  (Previously, this used to be the
//               "external" net transform, with the assumption that
//               that GSG would convert it internally, but that is no
//               longer the case.)
//
//               Special case: if (state==NULL), then the target
//               state is already stored in _target.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
set_state_and_transform(const RenderState *target,
                        const TransformState *transform) {
  report_my_gl_errors();
#ifndef NDEBUG
  if (gsg_cat.is_spam()) {
    gsg_cat.spam() << "Setting GSG state to " << (void *)target << ":\n";
    target->write(gsg_cat.spam(false), 2);
  }
#endif

  _state_pcollector.add_level(1);
  PStatGPUTimer timer1(this, _draw_set_state_pcollector);

  if (transform != _internal_transform) {
    //PStatGPUTimer timer(this, _draw_set_state_transform_pcollector);
    _transform_state_pcollector.add_level(1);
    _internal_transform = transform;
    do_issue_transform();
  }

  if (target == _state_rs && (_state_mask | _inv_state_mask).is_all_on()) {
    return;
  }
  _target_rs = target;

  _target_shader = DCAST(ShaderAttrib, _target_rs->get_attrib_def(ShaderAttrib::get_class_slot()));
#ifndef OPENGLES
  _instance_count = _target_shader->get_instance_count();
#endif
#ifndef OPENGLES_1
  if (_target_shader->auto_shader()) {
    // If we don't have a generated shader, make sure we have a ShaderGenerator, then generate the shader.
    CPT(RenderState) shader = _target_rs->get_auto_shader_state();
    if (shader->_generated_shader == NULL) {
      if (_shader_generator == NULL) {
        _shader_generator = new ShaderGenerator(this, _scene_setup->get_display_region()->get_window());
      }
      const_cast<RenderState*>(shader.p())->_generated_shader = DCAST(ShaderAttrib, _shader_generator->synthesize_shader(shader));
    }
    _target_shader = DCAST(ShaderAttrib, shader->_generated_shader);
  }
#endif

  int alpha_test_slot = AlphaTestAttrib::get_class_slot();
  if (_target_rs->get_attrib(alpha_test_slot) != _state_rs->get_attrib(alpha_test_slot) ||
      !_state_mask.get_bit(alpha_test_slot) ||
      (_target_shader->get_flag(ShaderAttrib::F_subsume_alpha_test) !=
       _state_shader->get_flag(ShaderAttrib::F_subsume_alpha_test))) {
    //PStatGPUTimer timer(this, _draw_set_state_alpha_test_pcollector);
    do_issue_alpha_test();
    _state_mask.set_bit(alpha_test_slot);
  }

  int antialias_slot = AntialiasAttrib::get_class_slot();
  if (_target_rs->get_attrib(antialias_slot) != _state_rs->get_attrib(antialias_slot) ||
      !_state_mask.get_bit(antialias_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_antialias_pcollector);
    do_issue_antialias();
    _state_mask.set_bit(antialias_slot);
  }

  int clip_plane_slot = ClipPlaneAttrib::get_class_slot();
  if (_target_rs->get_attrib(clip_plane_slot) != _state_rs->get_attrib(clip_plane_slot) ||
      !_state_mask.get_bit(clip_plane_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_clip_plane_pcollector);
    do_issue_clip_plane();
    _state_mask.set_bit(clip_plane_slot);
  }

  int color_slot = ColorAttrib::get_class_slot();
  int color_scale_slot = ColorScaleAttrib::get_class_slot();
  if (_target_rs->get_attrib(color_slot) != _state_rs->get_attrib(color_slot) ||
      _target_rs->get_attrib(color_scale_slot) != _state_rs->get_attrib(color_scale_slot) ||
      !_state_mask.get_bit(color_slot) ||
      !_state_mask.get_bit(color_scale_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_color_pcollector);
    do_issue_color();
    do_issue_color_scale();
    _state_mask.set_bit(color_slot);
    _state_mask.set_bit(color_scale_slot);
#ifndef OPENGLES_1
    if (_current_shader_context) {
      _current_shader_context->issue_parameters(Shader::SSD_color);
      _current_shader_context->issue_parameters(Shader::SSD_colorscale);
    }
#endif
  }

  int cull_face_slot = CullFaceAttrib::get_class_slot();
  if (_target_rs->get_attrib(cull_face_slot) != _state_rs->get_attrib(cull_face_slot) ||
      !_state_mask.get_bit(cull_face_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_cull_face_pcollector);
    do_issue_cull_face();
    _state_mask.set_bit(cull_face_slot);
  }

  int depth_offset_slot = DepthOffsetAttrib::get_class_slot();
  if (_target_rs->get_attrib(depth_offset_slot) != _state_rs->get_attrib(depth_offset_slot) ||
      !_state_mask.get_bit(depth_offset_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_depth_offset_pcollector);
    do_issue_depth_offset();
    _state_mask.set_bit(depth_offset_slot);
  }

  int depth_test_slot = DepthTestAttrib::get_class_slot();
  if (_target_rs->get_attrib(depth_test_slot) != _state_rs->get_attrib(depth_test_slot) ||
      !_state_mask.get_bit(depth_test_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_depth_test_pcollector);
    do_issue_depth_test();
    _state_mask.set_bit(depth_test_slot);
  }

  int depth_write_slot = DepthWriteAttrib::get_class_slot();
  if (_target_rs->get_attrib(depth_write_slot) != _state_rs->get_attrib(depth_write_slot) ||
      !_state_mask.get_bit(depth_write_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_depth_write_pcollector);
    do_issue_depth_write();
    _state_mask.set_bit(depth_write_slot);
  }

  int render_mode_slot = RenderModeAttrib::get_class_slot();
  if (_target_rs->get_attrib(render_mode_slot) != _state_rs->get_attrib(render_mode_slot) ||
      !_state_mask.get_bit(render_mode_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_render_mode_pcollector);
    do_issue_render_mode();
    _state_mask.set_bit(render_mode_slot);
  }

  int rescale_normal_slot = RescaleNormalAttrib::get_class_slot();
  if (_target_rs->get_attrib(rescale_normal_slot) != _state_rs->get_attrib(rescale_normal_slot) ||
      !_state_mask.get_bit(rescale_normal_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_rescale_normal_pcollector);
    do_issue_rescale_normal();
    _state_mask.set_bit(rescale_normal_slot);
  }

  int shade_model_slot = ShadeModelAttrib::get_class_slot();
  if (_target_rs->get_attrib(shade_model_slot) != _state_rs->get_attrib(shade_model_slot) ||
      !_state_mask.get_bit(shade_model_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_shade_model_pcollector);
    do_issue_shade_model();
    _state_mask.set_bit(shade_model_slot);
  }

  int transparency_slot = TransparencyAttrib::get_class_slot();
  int color_write_slot = ColorWriteAttrib::get_class_slot();
  int color_blend_slot = ColorBlendAttrib::get_class_slot();
  if (_target_rs->get_attrib(transparency_slot) != _state_rs->get_attrib(transparency_slot) ||
      _target_rs->get_attrib(color_write_slot) != _state_rs->get_attrib(color_write_slot) ||
      _target_rs->get_attrib(color_blend_slot) != _state_rs->get_attrib(color_blend_slot) ||
      !_state_mask.get_bit(transparency_slot) ||
      !_state_mask.get_bit(color_write_slot) ||
      !_state_mask.get_bit(color_blend_slot) ||
      (_target_shader->get_flag(ShaderAttrib::F_disable_alpha_write) !=
       _state_shader->get_flag(ShaderAttrib::F_disable_alpha_write))) {
    //PStatGPUTimer timer(this, _draw_set_state_blending_pcollector);
    do_issue_blending();
    _state_mask.set_bit(transparency_slot);
    _state_mask.set_bit(color_write_slot);
    _state_mask.set_bit(color_blend_slot);
  }

  if (_target_shader != _state_shader) {
    //PStatGPUTimer timer(this, _draw_set_state_shader_pcollector);
#ifndef OPENGLES_1
    do_issue_shader(true);
#endif
    _state_shader = _target_shader;
    _state_mask.clear_bit(TextureAttrib::get_class_slot());
  }
#ifdef OPENGLES_2
  else { // In the case of OpenGL ES 2.x, we need to glUseShader before we draw anything.
    do_issue_shader(false);
  }
#endif

  int texture_slot = TextureAttrib::get_class_slot();
  if (_target_rs->get_attrib(texture_slot) != _state_rs->get_attrib(texture_slot) ||
      !_state_mask.get_bit(texture_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_texture_pcollector);
    determine_target_texture();
    int prev_active = _num_active_texture_stages;
    do_issue_texture();

    // Since the TexGen and TexMatrix states depend partly on the
    // particular set of textures in use, we should force both of
    // those to be reissued every time we change the texture state.
    _state_mask.clear_bit(TexGenAttrib::get_class_slot());
    _state_mask.clear_bit(TexMatrixAttrib::get_class_slot());

    _state_texture = _target_texture;
    _state_mask.set_bit(texture_slot);
  }

  // If one of the previously-loaded TexGen modes modified the texture
  // matrix, then if either state changed, we have to change both of
  // them now.
  if (_tex_gen_modifies_mat) {
    int tex_gen_slot = TexGenAttrib::get_class_slot();
    int tex_matrix_slot = TexMatrixAttrib::get_class_slot();
    if (_target_rs->get_attrib(tex_gen_slot) != _state_rs->get_attrib(tex_gen_slot) ||
        _target_rs->get_attrib(tex_matrix_slot) != _state_rs->get_attrib(tex_matrix_slot) ||
        !_state_mask.get_bit(tex_gen_slot) ||
        !_state_mask.get_bit(tex_matrix_slot)) {
      _state_mask.clear_bit(tex_gen_slot);
      _state_mask.clear_bit(tex_matrix_slot);
    }
  }

  int tex_matrix_slot = TexMatrixAttrib::get_class_slot();
  if (_target_rs->get_attrib(tex_matrix_slot) != _state_rs->get_attrib(tex_matrix_slot) ||
      !_state_mask.get_bit(tex_matrix_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_tex_matrix_pcollector);
    do_issue_tex_matrix();
    _state_mask.set_bit(tex_matrix_slot);
  }

  int tex_gen_slot = TexGenAttrib::get_class_slot();
  if (_target_tex_gen != _state_tex_gen ||
      !_state_mask.get_bit(tex_gen_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_tex_gen_pcollector);
    do_issue_tex_gen();
    _state_tex_gen = _target_tex_gen;
    _state_mask.set_bit(tex_gen_slot);
  }

  int material_slot = MaterialAttrib::get_class_slot();
  if (_target_rs->get_attrib(material_slot) != _state_rs->get_attrib(material_slot) ||
      !_state_mask.get_bit(material_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_material_pcollector);
    do_issue_material();
    _state_mask.set_bit(material_slot);
#ifndef OPENGLES_1
    if (_current_shader_context) {
      _current_shader_context->issue_parameters(Shader::SSD_material);
    }
#endif
  }

  int light_slot = LightAttrib::get_class_slot();
  if (_target_rs->get_attrib(light_slot) != _state_rs->get_attrib(light_slot) ||
      !_state_mask.get_bit(light_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_light_pcollector);
    do_issue_light();
    _state_mask.set_bit(light_slot);
  }

  int stencil_slot = StencilAttrib::get_class_slot();
  if (_target_rs->get_attrib(stencil_slot) != _state_rs->get_attrib(stencil_slot) ||
      !_state_mask.get_bit(stencil_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_stencil_pcollector);
    do_issue_stencil();
    _state_mask.set_bit(stencil_slot);
  }

  int fog_slot = FogAttrib::get_class_slot();
  if (_target_rs->get_attrib(fog_slot) != _state_rs->get_attrib(fog_slot) ||
      !_state_mask.get_bit(fog_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_fog_pcollector);
    do_issue_fog();
    _state_mask.set_bit(fog_slot);
#ifndef OPENGLES_1
    if (_current_shader_context) {
      _current_shader_context->issue_parameters(Shader::SSD_fog);
    }
#endif
  }

  int scissor_slot = ScissorAttrib::get_class_slot();
  if (_target_rs->get_attrib(scissor_slot) != _state_rs->get_attrib(scissor_slot) ||
      !_state_mask.get_bit(scissor_slot)) {
    //PStatGPUTimer timer(this, _draw_set_state_scissor_pcollector);
    do_issue_scissor();
    _state_mask.set_bit(scissor_slot);
  }

  _state_rs = _target_rs;
  maybe_gl_finish();
  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::free_pointers
//       Access: Protected, Virtual
//  Description: Frees some memory that was explicitly allocated
//               within the glgsg.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
free_pointers() {
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_auto_rescale_normal
//       Access: Protected
//  Description: Issues the appropriate GL commands to either rescale
//               or normalize the normals according to the current
//               transform.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_auto_rescale_normal() {
#ifndef OPENGLES_2
  if (_internal_transform->has_identity_scale()) {
    // If there's no scale at all, don't do anything.
    glDisable(GL_NORMALIZE);
    if (GLCAT.is_spam()) {
      GLCAT.spam() << "glDisable(GL_NORMALIZE)\n";
    }
    if (_supports_rescale_normal && support_rescale_normal) {
      glDisable(GL_RESCALE_NORMAL);
      if (GLCAT.is_spam()) {
        GLCAT.spam() << "glDisable(GL_RESCALE_NORMAL)\n";
      }
    }

  } else if (_internal_transform->has_uniform_scale()) {
    // There's a uniform scale; use the rescale feature if available.
    if (_supports_rescale_normal && support_rescale_normal) {
      glEnable(GL_RESCALE_NORMAL);
      glDisable(GL_NORMALIZE);
      if (GLCAT.is_spam()) {
        GLCAT.spam() << "glEnable(GL_RESCALE_NORMAL)\n";
        GLCAT.spam() << "glDisable(GL_NORMALIZE)\n";
      }
    } else {
      glEnable(GL_NORMALIZE);
      if (GLCAT.is_spam()) {
        GLCAT.spam() << "glEnable(GL_NORMALIZE)\n";
      }
    }

  } else {
    // If there's a non-uniform scale, normalize everything.
    glEnable(GL_NORMALIZE);
    if (GLCAT.is_spam()) {
      GLCAT.spam() << "glEnable(GL_NORMALIZE)\n";
    }
    if (_supports_rescale_normal && support_rescale_normal) {
      glDisable(GL_RESCALE_NORMAL);
      if (GLCAT.is_spam()) {
        GLCAT.spam() << "glDisable(GL_RESCALE_NORMAL)\n";
      }
    }
  }
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_texture
//       Access: Protected, Virtual
//  Description: This is called by set_state_and_transform() when
//               the texture state has changed.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_texture() {
  DO_PSTATS_STUFF(_texture_state_pcollector.add_level(1));

#ifdef OPENGLES_1
  update_standard_texture_bindings();
#else
  if (_current_shader_context == 0 || !_current_shader_context->uses_custom_texture_bindings()) {
    // No shader, or a non-Cg shader.
    if (_texture_binding_shader_context != 0) {
      _texture_binding_shader_context->disable_shader_texture_bindings();
    }
    update_standard_texture_bindings();
  } else {
    if (_texture_binding_shader_context == 0) {
      disable_standard_texture_bindings();
      _current_shader_context->update_shader_texture_bindings(NULL);
    } else {
      _current_shader_context->
        update_shader_texture_bindings(_texture_binding_shader_context);
    }
  }

  _texture_binding_shader = _current_shader;
  _texture_binding_shader_context = _current_shader_context;
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::update_standard_texture_bindings
//       Access: Private
//  Description: Applies the appropriate set of textures for the
//               current state, using the standard fixed-function
//               pipeline.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
update_standard_texture_bindings() {
#ifndef OPENGLES_2
#ifndef NDEBUG
  if (_show_texture_usage) {
    update_show_usage_texture_bindings(-1);
    return;
  }
#endif // NDEBUG

  int num_stages = _target_texture->get_num_on_ff_stages();

#ifndef NDEBUG
  // Also check the _flash_texture.  If it is non-NULL, we need to
  // check to see if our flash_texture is in the texture stack here.
  // If so, then we need to call the special show_texture method
  // instead of the normal texture stack.
  if (_flash_texture != (Texture *)NULL) {
    double now = ClockObject::get_global_clock()->get_frame_time();
    int this_second = (int)floor(now);
    if (this_second & 1) {
      int show_stage_index = -1;
      for (int i = 0; i < num_stages && show_stage_index < 0; ++i) {
        TextureStage *stage = _target_texture->get_on_ff_stage(i);
        Texture *texture = _target_texture->get_on_texture(stage);
        if (texture == _flash_texture) {
          show_stage_index = i;
        }
      }

      if (show_stage_index >= 0) {
        update_show_usage_texture_bindings(show_stage_index);
        return;
      }
    }
  }
#endif  // NDEBUG

  nassertv(num_stages <= _max_texture_stages &&
           _num_active_texture_stages <= _max_texture_stages);

  _texture_involves_color_scale = false;

  int last_saved_result = -1;
  int last_stage = -1;
  int i;
  for (i = 0; i < num_stages; i++) {
    TextureStage *stage = _target_texture->get_on_ff_stage(i);
    Texture *texture = _target_texture->get_on_texture(stage);
    nassertv(texture != (Texture *)NULL);

    // Issue the texture on stage i.
    _glActiveTexture(GL_TEXTURE0 + i);

    // First, turn off the previous texture mode.
    glDisable(GL_TEXTURE_2D);
    if (_supports_cube_map) {
      glDisable(GL_TEXTURE_CUBE_MAP);
    }

#ifndef OPENGLES
    glDisable(GL_TEXTURE_1D);
    if (_supports_3d_texture) {
      glDisable(GL_TEXTURE_3D);
    }
#endif  // OPENGLES

    int view = get_current_tex_view_offset() + stage->get_tex_view_offset();
    TextureContext *tc = texture->prepare_now(view, _prepared_objects, this);
    if (tc == (TextureContext *)NULL) {
      // Something wrong with this texture; skip it.
      continue;
    }

    // Then, turn on the current texture mode.
    GLenum target = get_texture_target(texture->get_texture_type());
    if (target == GL_NONE) {
      // Unsupported texture mode.
      continue;
    }
#ifndef OPENGLES
    if (target == GL_TEXTURE_2D_ARRAY_EXT) {
      // Cannot be applied via the FFP.
      continue;
    }
#endif  // OPENGLES
    glEnable(target);

    if (!update_texture(tc, false)) {
      glDisable(target);
      continue;
    }
    apply_texture(tc);

    if (stage->involves_color_scale() && _color_scale_enabled) {
      LColor color = stage->get_color();
      color.set(color[0] * _current_color_scale[0],
                color[1] * _current_color_scale[1],
                color[2] * _current_color_scale[2],
                color[3] * _current_color_scale[3]);
      _texture_involves_color_scale = true;
      call_glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
    } else {
      call_glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, stage->get_color());
    }

    if (stage->get_mode() == TextureStage::M_decal) {
      if (texture->get_num_components() < 3 && _supports_texture_combine) {
        // Make a special case for 1- and 2-channel decal textures.
        // OpenGL does not define their use with GL_DECAL for some
        // reason, so implement them using the combiner instead.
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
        glTexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PREVIOUS);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);

      } else {
        // Normal 3- and 4-channel decal textures.
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
      }

    } else if (stage->get_mode() == TextureStage::M_combine) {
      if (!_supports_texture_combine) {
        GLCAT.warning()
          << "TextureStage::M_combine mode is not supported.\n";
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      } else {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, stage->get_rgb_scale());
        glTexEnvi(GL_TEXTURE_ENV, GL_ALPHA_SCALE, stage->get_alpha_scale());
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,
                     get_texture_combine_type(stage->get_combine_rgb_mode()));

        switch (stage->get_num_combine_rgb_operands()) {
        case 3:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB,
                       get_texture_src_type(stage->get_combine_rgb_source2(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB,
                       get_texture_operand_type(stage->get_combine_rgb_operand2()));
          // fall through

        case 2:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB,
                       get_texture_src_type(stage->get_combine_rgb_source1(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB,
                       get_texture_operand_type(stage->get_combine_rgb_operand1()));
          // fall through

        case 1:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB,
                       get_texture_src_type(stage->get_combine_rgb_source0(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB,
                       get_texture_operand_type(stage->get_combine_rgb_operand0()));
          // fall through

        default:
          break;
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,
                     get_texture_combine_type(stage->get_combine_alpha_mode()));

        switch (stage->get_num_combine_alpha_operands()) {
        case 3:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_ALPHA,
                       get_texture_src_type(stage->get_combine_alpha_source2(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA,
                       get_texture_operand_type(stage->get_combine_alpha_operand2()));
          // fall through

        case 2:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA,
                       get_texture_src_type(stage->get_combine_alpha_source1(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA,
                       get_texture_operand_type(stage->get_combine_alpha_operand1()));
          // fall through

        case 1:
          glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA,
                       get_texture_src_type(stage->get_combine_alpha_source0(),
                                            last_stage, last_saved_result, i));
          glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA,
                       get_texture_operand_type(stage->get_combine_alpha_operand0()));
          // fall through

        default:
          break;
        }
      }
    } else {
      GLint glmode = get_texture_apply_mode_type(stage->get_mode());
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, glmode);
    }

    if (stage->get_saved_result()) {
      // This texture's result will be "saved" for a future stage's
      // input.
      last_saved_result = i;
    } else {
      // This is a regular texture stage; it will be the "previous"
      // input for the next stage.
      last_stage = i;
    }
  }

  // Disable the texture stages that are no longer used.
  for (i = num_stages; i < _num_active_texture_stages; i++) {
    _glActiveTexture(GL_TEXTURE0 + i);
    glDisable(GL_TEXTURE_2D);
    if (_supports_cube_map) {
      glDisable(GL_TEXTURE_CUBE_MAP);
    }
#ifndef OPENGLES
    glDisable(GL_TEXTURE_1D);
    if (_supports_3d_texture) {
      glDisable(GL_TEXTURE_3D);
    }
#endif  // OPENGLES
  }

  // Save the count of texture stages for next time.
  _num_active_texture_stages = num_stages;

  report_my_gl_errors();
#endif  // OPENGLES_2
}


#ifndef NDEBUG
////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::update_show_usage_texture_bindings
//       Access: Private
//  Description: This is a special function that loads the usage
//               textures in gl-show-texture-usage mode, instead of
//               loading the actual used textures.
//
//               If the indicated stage_index is >= 0, then it is the
//               particular texture that is shown.  Otherwise, the
//               textures are rotated through based on
//               show_texture_usage_index.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
update_show_usage_texture_bindings(int show_stage_index) {
  int num_stages = _target_texture->get_num_on_ff_stages();

  nassertv(num_stages <= _max_texture_stages &&
           _num_active_texture_stages <= _max_texture_stages);

  _texture_involves_color_scale = false;

  // First, we walk through the list of textures and pretend to render
  // them all, even though we don't actually render them, just so
  // Panda will keep track of the list of "active" textures correctly
  // during the flash.
  int i;
  for (i = 0; i < num_stages; i++) {
    TextureStage *stage = _target_texture->get_on_ff_stage(i);
    Texture *texture = _target_texture->get_on_texture(stage);
    nassertv(texture != (Texture *)NULL);

    int view = get_current_tex_view_offset() + stage->get_tex_view_offset();
    TextureContext *tc = texture->prepare_now(view, _prepared_objects, this);
    if (tc == (TextureContext *)NULL) {
      // Something wrong with this texture; skip it.
      break;
    }
    tc->enqueue_lru(&_prepared_objects->_graphics_memory_lru);
  }

#ifndef OPENGLES_2
  // Disable all texture stages.
  for (i = 0; i < _num_active_texture_stages; i++) {
    _glActiveTexture(GL_TEXTURE0 + i);
#ifndef OPENGLES
    glDisable(GL_TEXTURE_1D);
#endif  // OPENGLES
    glDisable(GL_TEXTURE_2D);
    if (_supports_3d_texture) {
#ifndef OPENGLES_1
      glDisable(GL_TEXTURE_3D);
#endif  // OPENGLES_1
    }
    if (_supports_cube_map) {
      glDisable(GL_TEXTURE_CUBE_MAP);
    }
  }
#endif

  // Save the count of texture stages for next time.
  _num_active_texture_stages = num_stages;

  if (num_stages > 0) {
    // Now, pick just one texture stage to apply.
    if (show_stage_index >= 0 && show_stage_index < num_stages) {
      i = show_stage_index;
    } else {
      i = _show_texture_usage_index % num_stages;
    }

    TextureStage *stage = _target_texture->get_on_ff_stage(i);
    Texture *texture = _target_texture->get_on_texture(stage);
    nassertv(texture != (Texture *)NULL);

    // Choose the corresponding usage texture and apply it.
    _glActiveTexture(GL_TEXTURE0 + i);
#ifndef OPENGLES_2
    glEnable(GL_TEXTURE_2D);
#endif

    UsageTextureKey key(texture->get_x_size(), texture->get_y_size());
    UsageTextures::iterator ui = _usage_textures.find(key);
    if (ui == _usage_textures.end()) {
      // Need to create a new texture for this size.
      GLuint index;
      glGenTextures(1, &index);
      glBindTexture(GL_TEXTURE_2D, index);
      //TODO: this could be a lot simpler with glTexStorage2D
      // followed by a call to glClearTexImage.
      upload_usage_texture(texture->get_x_size(), texture->get_y_size());
      _usage_textures[key] = index;

    } else {
      // Just bind the previously-created texture.
      GLuint index = (*ui).second;
      glBindTexture(GL_TEXTURE_2D, index);
    }
  }

  report_my_gl_errors();
}
#endif  // NDEBUG

#ifndef NDEBUG
////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::upload_usage_texture
//       Access: Protected
//  Description: Uploads a special "usage" texture intended to be
//               applied only in gl-show-texture-usage mode, to reveal
//               where texture memory is being spent.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
upload_usage_texture(int width, int height) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "upload_usage_texture(" << width << ", " << height << ")\n";
  }

  static LColor colors[3] = {
    LColor(0.4, 0.5f, 0.8f, 1.0f),   // mipmap 0: blue
    LColor(1.0f, 1.0f, 0.0f, 1.0f),   // mipmap 1: yellow
    LColor(0.8f, 0.3, 0.3, 1.0f),   // mipmap 2 and higher: red
  };


  // Allocate a temporary array large enough to contain the toplevel
  // mipmap.
  PN_uint32 *buffer = (PN_uint32 *)PANDA_MALLOC_ARRAY(width * height * 4);

  int n = 0;
  while (true) {
    // Choose the color for the nth mipmap.
    LColor c = colors[min(n, 2)];

    // A simple union to store the colors values bytewise, and get the
    // answer wordwise, independently of machine byte-ordernig.
    union {
      struct {
        unsigned char r, g, b, a;
      } b;
      PN_uint32 w;
    } store;

    store.b.r = (unsigned char)(c[0] * 255.0f);
    store.b.g = (unsigned char)(c[1] * 255.0f);
    store.b.b = (unsigned char)(c[2] * 255.0f);
    store.b.a = 0xff;

    // Fill in the array.
    int num_pixels = width * height;
    for (int p = 0; p < num_pixels; ++p) {
      buffer[p] = store.w;
    }

    glTexImage2D(GL_TEXTURE_2D, n, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buffer);
    if (width == 1 && height == 1) {
      // That was the last mipmap level.
      break;
    }

    width = max(width >> 1, 1);
    height = max(height >> 1, 1);
    ++n;
  }

  PANDA_FREE_ARRAY(buffer);
}
#endif  // NDEBUG

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::disable_standard_texture_bindings
//       Access: Private
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
disable_standard_texture_bindings() {
#ifndef OPENGLES_2
  // Disable the texture stages that are no longer used.
  for (int i = 0; i < _num_active_texture_stages; i++) {
    _glActiveTexture(GL_TEXTURE0 + i);
#ifndef OPENGLES
    glDisable(GL_TEXTURE_1D);
#endif  // OPENGLES
    glDisable(GL_TEXTURE_2D);
    if (_supports_3d_texture) {
#ifndef OPENGLES_1
      glDisable(GL_TEXTURE_3D);
#endif  // OPENGLES_1
    }
    if (_supports_cube_map) {
      glDisable(GL_TEXTURE_CUBE_MAP);
    }
  }

  _num_active_texture_stages = 0;

  report_my_gl_errors();
#endif  // OPENGLES_2
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_tex_matrix
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_tex_matrix() {
#ifndef OPENGLES_2 // OpenGL ES 2 doesn't support texture matrices, I think.
  nassertv(_num_active_texture_stages <= _max_texture_stages);

  for (int i = 0; i < _num_active_texture_stages; i++) {
    TextureStage *stage = _target_texture->get_on_ff_stage(i);
    _glActiveTexture(GL_TEXTURE0 + i);

    glMatrixMode(GL_TEXTURE);

    const TexMatrixAttrib *target_tex_matrix = DCAST(TexMatrixAttrib, _target_rs->get_attrib_def(TexMatrixAttrib::get_class_slot()));
    if (target_tex_matrix->has_stage(stage)) {
      GLPf(LoadMatrix)(target_tex_matrix->get_mat(stage).get_data());
    } else {
      glLoadIdentity();

      // For some reason, the glLoadIdentity() call doesn't work on
      // my Dell laptop's IBM OpenGL driver, when used in
      // conjunction with glTexGen(), below.  But explicitly loading
      // an identity matrix does work.  But this buggy-driver
      // workaround might have other performance implications, so I
      // leave it out.
      // GLPf(LoadMatrix)(LMatrix4::ident_mat().get_data());
    }
  }
  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_tex_gen
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_tex_gen() {
  bool force_normal = false;

  nassertv(_num_active_texture_stages <= _max_texture_stages);

  // These are passed in for the four OBJECT_PLANE or EYE_PLANE
  // values; they effectively define an identity matrix that maps
  // the spatial coordinates one-for-one to UV's.  If you want a
  // mapping other than identity, use a TexMatrixAttrib (or a
  // TexProjectorEffect).
  static const PN_stdfloat s_data[4] = { 1, 0, 0, 0 };
  static const PN_stdfloat t_data[4] = { 0, 1, 0, 0 };
  static const PN_stdfloat r_data[4] = { 0, 0, 1, 0 };
  static const PN_stdfloat q_data[4] = { 0, 0, 0, 1 };

  _tex_gen_modifies_mat = false;

  bool got_point_sprites = false;

  for (int i = 0; i < _num_active_texture_stages; i++) {
    TextureStage *stage = _target_texture->get_on_ff_stage(i);
    _glActiveTexture(GL_TEXTURE0 + i);
#ifndef OPENGLES_2
    if (_supports_point_sprite) {
#ifdef OPENGLES
      glTexEnvi(GL_POINT_SPRITE_OES, GL_COORD_REPLACE_OES, GL_FALSE);
#else
      glTexEnvi(GL_POINT_SPRITE_ARB, GL_COORD_REPLACE_ARB, GL_FALSE);
#endif  // OPENGLES
    }
#endif  // OPENGLES_2

#ifndef OPENGLES  // TexGen not supported by OpenGL ES.
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
    glDisable(GL_TEXTURE_GEN_R);
    glDisable(GL_TEXTURE_GEN_Q);

    TexGenAttrib::Mode mode = _target_tex_gen->get_mode(stage);
    switch (mode) {
    case TexGenAttrib::M_off:
    case TexGenAttrib::M_light_vector:
      break;

    case TexGenAttrib::M_eye_sphere_map:
      glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
      glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
      glEnable(GL_TEXTURE_GEN_S);
      glEnable(GL_TEXTURE_GEN_T);
      force_normal = true;
      break;

    case TexGenAttrib::M_eye_cube_map:
      if (_supports_cube_map) {
        // We need to rotate the normals out of GL's coordinate
        // system and into the user's coordinate system.  We do this
        // by composing a transform onto the texture matrix.
        LMatrix4 mat = _inv_cs_transform->get_mat();
        mat.set_row(3, LVecBase3(0.0f, 0.0f, 0.0f));
        glMatrixMode(GL_TEXTURE);
        GLPf(MultMatrix)(mat.get_data());

        // Now we need to reset the texture matrix next time
        // around to undo this.
        _tex_gen_modifies_mat = true;

        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        force_normal = true;
      }
      break;

    case TexGenAttrib::M_world_cube_map:
      if (_supports_cube_map) {
        // We dynamically transform normals from eye space to world
        // space by applying the appropriate rotation transform to
        // the current texture matrix.  Unlike M_world_position, we
        // can't achieve this effect by monkeying with the modelview
        // transform, since the current modelview doesn't affect
        // GL_REFLECTION_MAP.
        CPT(TransformState) camera_transform = _scene_setup->get_camera_transform()->compose(_inv_cs_transform);

        LMatrix4 mat = camera_transform->get_mat();
        mat.set_row(3, LVecBase3(0.0f, 0.0f, 0.0f));
        glMatrixMode(GL_TEXTURE);
        GLPf(MultMatrix)(mat.get_data());

        // Now we need to reset the texture matrix next time
        // around to undo this.
        _tex_gen_modifies_mat = true;

        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        force_normal = true;
      }
      break;

    case TexGenAttrib::M_eye_normal:
      if (_supports_cube_map) {
        // We need to rotate the normals out of GL's coordinate
        // system and into the user's coordinate system.  We do this
        // by composing a transform onto the texture matrix.
        LMatrix4 mat = _inv_cs_transform->get_mat();
        mat.set_row(3, LVecBase3(0.0f, 0.0f, 0.0f));
        glMatrixMode(GL_TEXTURE);
        GLPf(MultMatrix)(mat.get_data());

        // Now we need to reset the texture matrix next time
        // around to undo this.
        _tex_gen_modifies_mat = true;

        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        force_normal = true;
      }
      break;

    case TexGenAttrib::M_world_normal:
      if (_supports_cube_map) {
        // We dynamically transform normals from eye space to world
        // space by applying the appropriate rotation transform to
        // the current texture matrix.  Unlike M_world_position, we
        // can't achieve this effect by monkeying with the modelview
        // transform, since the current modelview doesn't affect
        // GL_NORMAL_MAP.
        CPT(TransformState) camera_transform = _scene_setup->get_camera_transform()->compose(_inv_cs_transform);

        LMatrix4 mat = camera_transform->get_mat();
        mat.set_row(3, LVecBase3(0.0f, 0.0f, 0.0f));
        glMatrixMode(GL_TEXTURE);
        GLPf(MultMatrix)(mat.get_data());

        // Now we need to reset the texture matrix next time
        // around to undo this.
        _tex_gen_modifies_mat = true;

        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        force_normal = true;
      }
      break;

    case TexGenAttrib::M_eye_position:
      // To represent eye position correctly, we need to temporarily
      // load the coordinate-system transform.
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      GLPf(LoadMatrix)(_cs_transform->get_mat().get_data());

      glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
      glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
      glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
      glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);

      GLPfv(TexGen)(GL_S, GL_EYE_PLANE, s_data);
      GLPfv(TexGen)(GL_T, GL_EYE_PLANE, t_data);
      GLPfv(TexGen)(GL_R, GL_EYE_PLANE, r_data);
      GLPfv(TexGen)(GL_Q, GL_EYE_PLANE, q_data);

      glEnable(GL_TEXTURE_GEN_S);
      glEnable(GL_TEXTURE_GEN_T);
      glEnable(GL_TEXTURE_GEN_R);
      glEnable(GL_TEXTURE_GEN_Q);

      glMatrixMode(GL_MODELVIEW);
      glPopMatrix();
      break;

    case TexGenAttrib::M_world_position:
      // We achieve world position coordinates by using the eye
      // position mode, and loading the transform of the root
      // node--thus putting the "eye" at the root.
      {
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        CPT(TransformState) root_transform = _cs_transform->compose(_scene_setup->get_world_transform());
        GLPf(LoadMatrix)(root_transform->get_mat().get_data());
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
        glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);

        GLPfv(TexGen)(GL_S, GL_EYE_PLANE, s_data);
        GLPfv(TexGen)(GL_T, GL_EYE_PLANE, t_data);
        GLPfv(TexGen)(GL_R, GL_EYE_PLANE, r_data);
        GLPfv(TexGen)(GL_Q, GL_EYE_PLANE, q_data);

        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        glEnable(GL_TEXTURE_GEN_Q);

        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
      }
      break;

    case TexGenAttrib::M_point_sprite:
      if (_supports_point_sprite) {
#ifdef OPENGLES
        glTexEnvi(GL_POINT_SPRITE_OES, GL_COORD_REPLACE_OES, GL_TRUE);
#else
        glTexEnvi(GL_POINT_SPRITE_ARB, GL_COORD_REPLACE_ARB, GL_TRUE);
#endif
        got_point_sprites = true;
      }
      break;

    case TexGenAttrib::M_constant:
      // To generate a constant UV(w) coordinate everywhere, we use
      // EYE_LINEAR mode, but we construct a special matrix that
      // flattens the vertex position to zero and then adds our
      // desired value.
      {
        const LTexCoord3 &v = _target_tex_gen->get_constant_value(stage);

        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
        glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);

        LVecBase4 s(0.0f, 0.0f, 0.0f, v[0]);
        LVecBase4 t(0.0f, 0.0f, 0.0f, v[1]);
        LVecBase4 r(0.0f, 0.0f, 0.0f, v[2]);

        GLPfv(TexGen)(GL_S, GL_OBJECT_PLANE, s.get_data());
        GLPfv(TexGen)(GL_T, GL_OBJECT_PLANE, t.get_data());
        GLPfv(TexGen)(GL_R, GL_OBJECT_PLANE, r.get_data());
        GLPfv(TexGen)(GL_Q, GL_OBJECT_PLANE, q_data);

        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glEnable(GL_TEXTURE_GEN_R);
        glEnable(GL_TEXTURE_GEN_Q);
      }
      break;

    case TexGenAttrib::M_unused:
      break;
    }
#endif  // OPENGLES
  }

  if (got_point_sprites != _tex_gen_point_sprite) {
    _tex_gen_point_sprite = got_point_sprites;
#ifndef OPENGLES_2
#ifdef OPENGLES
    if (_tex_gen_point_sprite) {
      glEnable(GL_POINT_SPRITE_OES);
    } else {
      glDisable(GL_POINT_SPRITE_OES);
    }
#else
    if (_tex_gen_point_sprite) {
      glEnable(GL_POINT_SPRITE_ARB);
    } else {
      glDisable(GL_POINT_SPRITE_ARB);
    }
#endif  // OPENGLES
#endif  // OPENGLES_2
  }

  report_my_gl_errors();
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::specify_texture
//       Access: Protected
//  Description: Specifies the texture parameters.  Returns true if
//               the texture may need to be reloaded.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
specify_texture(CLP(TextureContext) *gtc) {
  nassertr(gtc->_handle == 0 /* can't modify tex with active handle */, false);

  Texture *tex = gtc->get_texture();

  GLenum target = get_texture_target(tex->get_texture_type());
  if (target == GL_NONE) {
    // Unsupported target (e.g. 3-d texturing on GL 1.1).
    return false;
  }

  glTexParameteri(target, GL_TEXTURE_WRAP_S,
                     get_texture_wrap_mode(tex->get_wrap_u()));
#ifndef OPENGLES
  if (target != GL_TEXTURE_1D) {
    glTexParameteri(target, GL_TEXTURE_WRAP_T,
                       get_texture_wrap_mode(tex->get_wrap_v()));
  }
#endif
#ifdef OPENGLES_2
  if (target == GL_TEXTURE_3D_OES) {
    glTexParameteri(target, GL_TEXTURE_WRAP_R_OES,
                       get_texture_wrap_mode(tex->get_wrap_w()));
  }
#endif
#ifndef OPENGLES
  if (target == GL_TEXTURE_3D) {
    glTexParameteri(target, GL_TEXTURE_WRAP_R,
                       get_texture_wrap_mode(tex->get_wrap_w()));
  }

  LColor border_color = tex->get_border_color();
  call_glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border_color);
#endif  // OPENGLES

  Texture::FilterType minfilter = tex->get_effective_minfilter();
  Texture::FilterType magfilter = tex->get_effective_magfilter();
  bool uses_mipmaps = Texture::is_mipmap(minfilter) && !gl_ignore_mipmaps;

#ifndef NDEBUG
  if (gl_force_mipmaps) {
    minfilter = Texture::FT_linear_mipmap_linear;
    magfilter = Texture::FT_linear;
    uses_mipmaps = true;
  }
#endif

  if (!tex->might_have_ram_image()) {
    // If it's a dynamically generated texture (that is, the RAM image
    // isn't available so it didn't pass through the CPU), we should
    // enable GL-generated mipmaps if we can.
    if (!_supports_generate_mipmap) {
      // However, if the GPU doesn't support mipmap generation, we
      // have to turn it off.
      uses_mipmaps = false;
    }
  }

  glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                     get_texture_filter_type(minfilter, !uses_mipmaps));
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER,
                     get_texture_filter_type(magfilter, true));

  // Set anisotropic filtering.
  if (_supports_anisotropy) {
    PN_stdfloat anisotropy = tex->get_effective_anisotropic_degree();
    anisotropy = min(anisotropy, _max_anisotropy);
    anisotropy = max(anisotropy, (PN_stdfloat)1.0);
    glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
  }

#ifndef OPENGLES
  if (tex->get_format() == Texture::F_depth_stencil ||
      tex->get_format() == Texture::F_depth_component ||
      tex->get_format() == Texture::F_depth_component16 ||
      tex->get_format() == Texture::F_depth_component24 ||
      tex->get_format() == Texture::F_depth_component32) {
    glTexParameteri(target, GL_DEPTH_TEXTURE_MODE_ARB, GL_INTENSITY);
    if (_supports_shadow_filter) {
      if ((tex->get_magfilter() == Texture::FT_shadow) ||
          (tex->get_minfilter() == Texture::FT_shadow)) {
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE_ARB);
        glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC_ARB, GL_LEQUAL);
      } else {
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE);
        glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC_ARB, GL_LEQUAL);
      }
    }
  }
#endif

  report_my_gl_errors();

  if (uses_mipmaps && !gtc->_uses_mipmaps) {
    // Suddenly we require mipmaps.  This means the texture may need
    // reloading.
    return true;
  }

  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::apply_texture
//       Access: Protected
//  Description: Updates OpenGL with the current information for this
//               texture, and makes it the current texture available
//               for rendering.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
apply_texture(TextureContext *tc) {
  CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), tc);

  gtc->set_active(true);
  GLenum target = get_texture_target(gtc->get_texture()->get_texture_type());
  if (target == GL_NONE) {
    return false;
  }

  if (gtc->_target != target) {
    // The target has changed.  That means we have to re-bind a new
    // texture object.
    gtc->reset_data();
    gtc->_target = target;
  }
  glBindTexture(target, gtc->_index);

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::upload_texture
//       Access: Protected
//  Description: Uploads the entire texture image to OpenGL, including
//               all pages.
//
//               The return value is true if successful, or false if
//               the texture has no image.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
upload_texture(CLP(TextureContext) *gtc, bool force) {
  PStatGPUTimer timer(this, _load_texture_pcollector);

  Texture *tex = gtc->get_texture();

  if (_effective_incomplete_render && !force) {
    bool has_image = _supports_compressed_texture ? tex->has_ram_image() : tex->has_uncompressed_ram_image();
    if (!has_image && tex->might_have_ram_image() &&
        tex->has_simple_ram_image() &&
        !_loader.is_null()) {
      // If we don't have the texture data right now, go get it, but in
      // the meantime load a temporary simple image in its place.
      async_reload_texture(gtc);
      has_image = _supports_compressed_texture ? tex->has_ram_image() : tex->has_uncompressed_ram_image();
      if (!has_image) {
        if (gtc->was_simple_image_modified()) {
          return upload_simple_texture(gtc);
        }
        return true;
      }
    }
  }

  CPTA_uchar image;
  if (_supports_compressed_texture) {
    image = tex->get_ram_image();
  } else {
    image = tex->get_uncompressed_ram_image();
  }

  Texture::CompressionMode image_compression;
  if (image.is_null()) {
    image_compression = Texture::CM_off;
  } else {
    image_compression = tex->get_ram_image_compression();
  }

  if (!get_supports_compressed_texture_format(image_compression)) {
    image = tex->get_uncompressed_ram_image();
    image_compression = Texture::CM_off;
  }

  int mipmap_bias = 0;

  int width = tex->get_x_size();
  int height = tex->get_y_size();
  int depth = tex->get_z_size();

  GLint internal_format = get_internal_image_format(tex);
  GLint external_format = get_external_image_format(tex);
  GLenum component_type = get_component_type(tex->get_component_type());

  if (GLCAT.is_debug()) {
    if (image.is_null()) {
      GLCAT.debug()
        << "loading texture with NULL image";
    } else if (image_compression != Texture::CM_off) {
      GLCAT.debug()
        << "loading pre-compressed texture";
    } else if (is_compressed_format(internal_format)) {
      GLCAT.debug()
        << "loading compressed texture";
    } else {
      GLCAT.debug()
        << "loading uncompressed texture";
    }
    GLCAT.debug(false) << " " << tex->get_name() << "\n";
  }

  // Ensure that the texture fits within the GL's specified limits.
  // Need to split dimensions because of texture arrays
  int max_dimension_x;
  int max_dimension_y;
  int max_dimension_z;

  switch (tex->get_texture_type()) {
  case Texture::TT_3d_texture:
    max_dimension_x = _max_3d_texture_dimension;
    max_dimension_y = _max_3d_texture_dimension;
    max_dimension_z = _max_3d_texture_dimension;
    break;

  case Texture::TT_cube_map:
    max_dimension_x = _max_cube_map_dimension;
    max_dimension_y = _max_cube_map_dimension;
    max_dimension_z = 6;
    break;

  case Texture::TT_2d_texture_array:
    max_dimension_x = _max_texture_dimension;
    max_dimension_y = _max_texture_dimension;
    max_dimension_z = _max_2d_texture_array_layers;
    break;

  default:
    max_dimension_x = _max_texture_dimension;
    max_dimension_y = _max_texture_dimension;
    max_dimension_z = 1;
  }

  if (max_dimension_x == 0 || max_dimension_y == 0 || max_dimension_z == 0) {
    // Guess this GL doesn't support cube mapping/3d textures/2d texture arrays.
    report_my_gl_errors();
    return false;
  }

  // If it doesn't fit, we have to reduce it on-the-fly.  We do this
  // by incrementing the mipmap_bias, so we're effectively loading a
  // lower mipmap level.  This requires generating the mipmaps on
  // the CPU if they haven't already been generated.  It would have
  // been better if the user had specified max-texture-dimension to
  // reduce the texture at load time instead; of course, the user
  // doesn't always know ahead of time what the hardware limits are.

  if ((max_dimension_x > 0 && max_dimension_y > 0 && max_dimension_z > 0) &&
      image_compression == Texture::CM_off) {
    while (tex->get_expected_mipmap_x_size(mipmap_bias) > max_dimension_x ||
           tex->get_expected_mipmap_y_size(mipmap_bias) > max_dimension_y ||
           tex->get_expected_mipmap_z_size(mipmap_bias) > max_dimension_z) {
      ++mipmap_bias;
    }

    if (mipmap_bias >= tex->get_num_ram_mipmap_images()) {
      // We need to generate some more mipmap images.
      if (tex->has_ram_image()) {
        tex->generate_ram_mipmap_images();
        if (mipmap_bias >= tex->get_num_ram_mipmap_images()) {
          // It didn't work.  Send the smallest we've got, and hope
          // for the best.
          mipmap_bias = tex->get_num_ram_mipmap_images() - 1;
        }
      }
    }

    width = tex->get_expected_mipmap_x_size(mipmap_bias);
    height = tex->get_expected_mipmap_y_size(mipmap_bias);
    depth = tex->get_expected_mipmap_z_size(mipmap_bias);

    if (mipmap_bias != 0) {
      GLCAT.info()
        << "Reducing image " << tex->get_name()
        << " from " << tex->get_x_size() << " x " << tex->get_y_size()
        << " x " << tex->get_z_size() << " to "
        << width << " x " << height << " x " << depth << "\n";
    }
  }

  if (image_compression != Texture::CM_off) {
    Texture::QualityLevel quality_level = tex->get_effective_quality_level();

#ifndef OPENGLES
    switch (quality_level) {
    case Texture::QL_fastest:
      glHint(GL_TEXTURE_COMPRESSION_HINT, GL_FASTEST);
      break;

    case Texture::QL_default:
    case Texture::QL_normal:
      glHint(GL_TEXTURE_COMPRESSION_HINT, GL_DONT_CARE);
      break;

    case Texture::QL_best:
      glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);
      break;
    }
#endif
  }

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  GLenum target = get_texture_target(tex->get_texture_type());
  bool uses_mipmaps = (tex->uses_mipmaps() && !gl_ignore_mipmaps) || gl_force_mipmaps;
  bool needs_reload = false;
  if (!gtc->_has_storage ||
      gtc->_uses_mipmaps != uses_mipmaps ||
      gtc->_internal_format != internal_format ||
      gtc->_width != width ||
      gtc->_height != height ||
      gtc->_depth != depth) {
    // We need to reload a new GL Texture object.
    needs_reload = true;
  }

  if (needs_reload && gtc->_immutable) {
    GLCAT.warning() << "Attempt to modify texture with immutable storage, recreating texture.\n";
    gtc->reset_data();
    glBindTexture(target, gtc->_index);
  }

  if (needs_reload) {
    if (_use_object_labels) {
      // This seems like a good time to assign a label for the debug messages.
      const string &name = tex->get_name();
      _glObjectLabel(GL_TEXTURE, gtc->_index, name.size(), name.data());
    }

    // Figure out whether mipmaps will be generated by the GPU or by
    // Panda (or not at all), and how many mipmap levels should be created.
    gtc->_generate_mipmaps = false;
    int num_levels = 1;
    CPTA_uchar image = tex->get_ram_mipmap_image(mipmap_bias);

    if (image.is_null()) {
      if (uses_mipmaps) {
        if (_supports_generate_mipmap) {
          num_levels = tex->get_expected_num_mipmap_levels() - mipmap_bias;
          gtc->_generate_mipmaps = true;
        } else {
          // If it can't, do without mipmaps.
          num_levels = 1;
          glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
      }

    } else {
      if (uses_mipmaps) {
        num_levels = tex->get_num_ram_mipmap_images() - mipmap_bias;

        if (num_levels <= 1) {
          // No RAM mipmap levels available.  Should we generate some?
          if (!_supports_generate_mipmap || !driver_generate_mipmaps ||
              image_compression != Texture::CM_off) {
            // Yes, the GL can't or won't generate them, so we need to.
            // Note that some drivers (nVidia) will *corrupt memory* if
            // you ask them to generate mipmaps for a pre-compressed
            // texture.
            tex->generate_ram_mipmap_images();
            num_levels = tex->get_num_ram_mipmap_images() - mipmap_bias;
          }
        }

        if (num_levels <= 1) {
          // We don't have mipmap levels in RAM.  Ask the GL to generate
          // them if it can.
          if (_supports_generate_mipmap) {
            num_levels = tex->get_expected_num_mipmap_levels() - mipmap_bias;
            gtc->_generate_mipmaps = true;
          } else {
            // If it can't, do without mipmaps.
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            num_levels = 1;
          }
        }
      }
    }

#ifndef OPENGLES // OpenGL ES doesn't have GL_TEXTURE_MAX_LEVEL.
    if (is_at_least_gl_version(1, 2)) {
      // By the time we get here, we have a pretty good prediction for
      // the number of mipmaps we're going to have, so tell the GL that's
      // all it's going to get.
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, num_levels - 1);
    }
#endif

#ifndef OPENGLES_2
    if (gtc->_generate_mipmaps && _glGenerateMipmap == NULL) {
      // The old, deprecated way to generate mipmaps.
      glTexParameteri(target, GL_GENERATE_MIPMAP, GL_TRUE);
    }
#endif

    // Allocate immutable storage for the texture, after which we can subload it.
    // Pre-allocating storage using glTexStorage is more efficient than using glTexImage
    // to load all of the individual images one by one later, but we are not allowed to
    // change the texture size or number of mipmap levels after this point.
    if (gl_immutable_texture_storage && _supports_tex_storage && !gtc->_has_storage) {
      if (GLCAT.is_debug()) {
        GLCAT.debug()
          << "allocating storage for texture " << tex->get_name() << ", " << width
           << " x " << height << " x " << depth << ", mipmaps " << num_levels
          << ", uses_mipmaps = " << uses_mipmaps << "\n";
      }

      switch (tex->get_texture_type()) {
      case Texture::TT_1d_texture:
        _glTexStorage1D(target, num_levels, internal_format, width);
        break;
      case Texture::TT_2d_texture:
      case Texture::TT_cube_map:
        _glTexStorage2D(target, num_levels, internal_format, width, height);
        break;
      case Texture::TT_3d_texture:
      case Texture::TT_2d_texture_array:
        _glTexStorage3D(target, num_levels, internal_format, width, height, depth);
        break;
      }

      gtc->_has_storage = true;
      gtc->_immutable = true;
      gtc->_uses_mipmaps = uses_mipmaps;
      gtc->_internal_format = internal_format;
      gtc->_width = width;
      gtc->_height = height;
      gtc->_depth = depth;
      needs_reload = false;
    }
  } else {
    // Maybe we need to generate mipmaps on the CPU.
    if (!image.is_null() && uses_mipmaps) {
      if (tex->get_num_ram_mipmap_images() - mipmap_bias <= 1) {
        // No RAM mipmap levels available.  Should we generate some?
        if (!_supports_generate_mipmap || !driver_generate_mipmaps ||
            image_compression != Texture::CM_off) {
          // Yes, the GL can't or won't generate them, so we need to.
          // Note that some drivers (nVidia) will *corrupt memory* if
          // you ask them to generate mipmaps for a pre-compressed
          // texture.
          tex->generate_ram_mipmap_images();
        }
      }
    }
  }

  bool success = true;
  if (tex->get_texture_type() == Texture::TT_cube_map) {
    // A cube map must load six different 2-d images (which are stored
    // as the six pages of the system ram image).
    if (!_supports_cube_map) {
      report_my_gl_errors();
      return false;
    }
    nassertr(target == GL_TEXTURE_CUBE_MAP, false);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_X,
       internal_format, external_format, component_type,
       true, 0, image_compression);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
       internal_format, external_format, component_type,
       true, 1, image_compression);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
       internal_format, external_format, component_type,
       true, 2, image_compression);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
       internal_format, external_format, component_type,
       true, 3, image_compression);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
       internal_format, external_format, component_type,
       true, 4, image_compression);

    success = success && upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias,
       GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
       internal_format, external_format, component_type,
       true, 5, image_compression);

  } else {
    // Any other kind of texture can be loaded all at once.
    success = upload_texture_image
      (gtc, needs_reload, uses_mipmaps, mipmap_bias, target,
       target, internal_format, external_format,
       component_type, false, 0, image_compression);
  }

  if (gtc->_generate_mipmaps && _glGenerateMipmap != NULL) {
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "generating mipmaps for texture " << tex->get_name() << ", "
        << width << " x " << height << " x " << depth
        << ", uses_mipmaps = " << uses_mipmaps << "\n";
    }
    _glGenerateMipmap(target);
  }

  maybe_gl_finish();

  if (success) {
    if (needs_reload) {
      gtc->_has_storage = true;
      gtc->_uses_mipmaps = uses_mipmaps;
      gtc->_internal_format = internal_format;
      gtc->_width = width;
      gtc->_height = height;
      gtc->_depth = depth;
    }

    if (!image.is_null()) {
      gtc->update_data_size_bytes(get_texture_memory_size(tex));
    }

    if (tex->get_post_load_store_cache()) {
      tex->set_post_load_store_cache(false);
      // OK, get the RAM image, and save it in a BamCache record.
      if (do_extract_texture_data(gtc)) {
        if (tex->has_ram_image()) {
          BamCache *cache = BamCache::get_global_ptr();
          PT(BamCacheRecord) record = cache->lookup(tex->get_fullpath(), "txo");
          if (record != (BamCacheRecord *)NULL) {
            record->set_data(tex, tex);
            cache->store(record);
          }
        }
      }
    }

    GraphicsEngine *engine = get_engine();
    nassertr(engine != (GraphicsEngine *)NULL, false);
    engine->texture_uploaded(tex);
    gtc->mark_loaded();

    report_my_gl_errors();
    return true;
  }

  report_my_gl_errors();
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::upload_texture_image
//       Access: Protected
//  Description: Loads a texture image, or one page of a cube map
//               image, from system RAM to texture memory.
//
//               texture_target is normally the same thing as
//               page_target; both represent the GL target onto which
//               the texture image is loaded, e.g. GL_TEXTURE_1D,
//               GL_TEXTURE_2D, etc.  The only time they may differ is
//               in the case of cube mapping, in which case
//               texture_target will be target for the overall
//               texture, e.g. GL_TEXTURE_CUBE_MAP, and page_target
//               will be the target for this particular page,
//               e.g. GL_TEXTURE_CUBE_MAP_POSITIVE_X.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
upload_texture_image(CLP(TextureContext) *gtc, bool needs_reload,
                     bool uses_mipmaps, int mipmap_bias,
                     GLenum texture_target, GLenum page_target,
                     GLint internal_format,
                     GLint external_format, GLenum component_type,
                     bool one_page_only, int z,
                     Texture::CompressionMode image_compression) {
  // Make sure the error stack is cleared out before we begin.
  clear_my_gl_errors();

  if (texture_target == GL_NONE) {
    // Unsupported target (e.g. 3-d texturing on GL 1.1).
    return false;
  }
  if (image_compression != Texture::CM_off && !_supports_compressed_texture) {
    return false;
  }

  Texture *tex = gtc->get_texture();
  nassertr(tex != (Texture *)NULL, false);

  CPTA_uchar image = tex->get_ram_mipmap_image(mipmap_bias);
  int width = tex->get_expected_mipmap_x_size(mipmap_bias);
  int height = tex->get_expected_mipmap_y_size(mipmap_bias);
  int depth = tex->get_expected_mipmap_z_size(mipmap_bias);

  // Determine the number of images to upload.
  int num_ram_mipmap_levels = 0;
  if (!image.is_null()) {
    if (uses_mipmaps) {
      num_ram_mipmap_levels = tex->get_num_ram_mipmap_images();
    } else {
      num_ram_mipmap_levels = 1;
    }
  }

#ifndef OPENGLES
  if (needs_reload || num_ram_mipmap_levels > 0) {
    // Make sure that any incoherent writes to this texture have been synced.
    if (gtc->needs_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT)) {
      issue_memory_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    }
  }
#endif

  if (!needs_reload) {
    // Try to subload the image over the existing GL Texture object,
    // possibly saving on texture memory fragmentation.

    if (GLCAT.is_debug()) {
      if (num_ram_mipmap_levels == 0) {
        GLCAT.debug()
          << "not loading NULL image for tex " << tex->get_name() << ", " << width << " x " << height
          << " x " << depth << ", z = " << z << ", uses_mipmaps = " << uses_mipmaps << "\n";
      } else {
        GLCAT.debug()
          << "updating image data of texture " << tex->get_name() << ", " << width << " x " << height
          << " x " << depth << ", z = " << z << ", mipmaps " << num_ram_mipmap_levels
          << ", uses_mipmaps = " << uses_mipmaps << "\n";
      }
    }

    for (int n = mipmap_bias; n < num_ram_mipmap_levels; ++n) {
      // we grab the mipmap pointer first, if it is NULL we grab the
      // normal mipmap image pointer which is a PTA_uchar
      const unsigned char *image_ptr = (unsigned char*)tex->get_ram_mipmap_pointer(n);
      CPTA_uchar ptimage;
      if (image_ptr == (const unsigned char *)NULL) {
        ptimage = tex->get_ram_mipmap_image(n);
        if (ptimage == (const unsigned char *)NULL) {
          GLCAT.warning()
            << "No mipmap level " << n << " defined for " << tex->get_name()
            << "\n";
          // No mipmap level n; stop here.
          break;
        }
        image_ptr = ptimage;
      }

      const unsigned char *orig_image_ptr = image_ptr;
      size_t view_size = tex->get_ram_mipmap_view_size(n);
      image_ptr += view_size * gtc->get_view();
      if (one_page_only) {
        view_size = tex->get_ram_mipmap_page_size(n);
        image_ptr += view_size * z;
      }
      nassertr(image_ptr >= orig_image_ptr && image_ptr + view_size <= orig_image_ptr + tex->get_ram_mipmap_image_size(n), false);

      PTA_uchar bgr_image;
      if (!_supports_bgr && image_compression == Texture::CM_off) {
        // If the GL doesn't claim to support BGR, we may have to reverse
        // the component ordering of the image.
        image_ptr = fix_component_ordering(bgr_image, image_ptr, view_size,
                                           external_format, tex);
      }

      int width = tex->get_expected_mipmap_x_size(n);
      int height = tex->get_expected_mipmap_y_size(n);
      int depth = tex->get_expected_mipmap_z_size(n);

#ifdef DO_PSTATS
      _data_transferred_pcollector.add_level(view_size);
#endif
      switch (texture_target) {
#ifdef OPENGLES_2
      case GL_TEXTURE_3D_OES:
#endif
#ifndef OPENGLES
      case GL_TEXTURE_3D:
#endif
#ifndef OPENGLES_1
        if (_supports_3d_texture) {
          if (image_compression == Texture::CM_off) {
            _glTexSubImage3D(page_target, n - mipmap_bias, 0, 0, 0, width, height, depth,
                             external_format, component_type, image_ptr);
          } else {
            _glCompressedTexSubImage3D(page_target, n - mipmap_bias, 0, 0, 0, width, height, depth,
                                       external_format, view_size, image_ptr);
          }
        } else {
          report_my_gl_errors();
          return false;
        }
        break;

      case GL_TEXTURE_1D:
        if (image_compression == Texture::CM_off) {
          glTexSubImage1D(page_target, n - mipmap_bias, 0, width,
                          external_format, component_type, image_ptr);
        } else {
          _glCompressedTexSubImage1D(page_target, n - mipmap_bias, 0, width,
                                     external_format, view_size, image_ptr);
        }
        break;
#endif
#ifndef OPENGLES
      case GL_TEXTURE_2D_ARRAY_EXT:
        if (_supports_2d_texture_array) {
          if (image_compression == Texture::CM_off) {
            _glTexSubImage3D(page_target, n - mipmap_bias, 0, 0, 0, width, height, depth,
                             external_format, component_type, image_ptr);
          } else {
            _glCompressedTexSubImage3D(page_target, n - mipmap_bias, 0, 0, 0, width, height, depth,
                                       external_format, view_size, image_ptr);
          }
        } else {
          report_my_gl_errors();
          return false;
        }
        break;
#endif
      default:
        if (image_compression == Texture::CM_off) {
          if (n==0) {
            // It's unfortunate that we can't adjust the width, too,
            // but TexSubImage2D doesn't accept a row-stride parameter.
            height = tex->get_y_size() - tex->get_pad_y_size();
          }
          glTexSubImage2D(page_target, n - mipmap_bias, 0, 0, width, height,
                          external_format, component_type, image_ptr);
        } else {
          _glCompressedTexSubImage2D(page_target, n - mipmap_bias, 0, 0, width, height,
                                     external_format, view_size, image_ptr);
        }
        break;
      }
    }

    // Did that fail?  If it did, we'll immediately try again, this
    // time loading the texture from scratch.
    GLenum error_code = gl_get_error();
    if (error_code != GL_NO_ERROR) {
      if (GLCAT.is_debug()) {
        GLCAT.debug()
          << "GL texture subload failed for " << tex->get_name()
          << " : " << get_error_string(error_code) << "\n";
      }
      needs_reload = true;
    }
  }

  if (needs_reload) {
    // Load the image up from scratch, creating a new GL Texture
    // object.
    if (GLCAT.is_debug()) {
      GLCAT.debug()
        << "loading new texture object for " << tex->get_name() << ", " << width
        << " x " << height << " x " << depth << ", z = " << z << ", mipmaps "
        << num_ram_mipmap_levels << ", uses_mipmaps = " << uses_mipmaps << "\n";
    }

    // If there is immutable storage, this is impossible to do, and we should
    // not have gotten here at all.
    nassertr(!gtc->_immutable, false);

    if (num_ram_mipmap_levels == 0) {
      if (GLCAT.is_debug()) {
        GLCAT.debug()
          << "  (initializing NULL image)\n";
      }

      if ((external_format == GL_DEPTH_STENCIL) && get_supports_depth_stencil()) {
#ifdef OPENGLES
        component_type = GL_UNSIGNED_INT_24_8_OES;
#else
        component_type = GL_UNSIGNED_INT_24_8_EXT;
#endif
      }

      // We don't have any RAM mipmap levels, so we create an uninitialized OpenGL
      // texture.  Presumably this will be used later for render-to-texture or so.
      switch (page_target) {
#ifndef OPENGLES
        case GL_TEXTURE_1D:
          glTexImage1D(page_target, 0, internal_format, width, 0, external_format, component_type, NULL);
          break;
        case GL_TEXTURE_2D_ARRAY:
#endif
#ifndef OPENGLES_1
        case GL_TEXTURE_3D:
          _glTexImage3D(page_target, 0, internal_format, width, height, depth, 0, external_format, component_type, NULL);
          break;
#endif
        default:
          glTexImage2D(page_target, 0, internal_format, width, height, 0, external_format, component_type, NULL);
          break;
      }
    }

    for (int n = mipmap_bias; n < num_ram_mipmap_levels; ++n) {
      const unsigned char *image_ptr = (unsigned char*)tex->get_ram_mipmap_pointer(n);
      CPTA_uchar ptimage;
      if (image_ptr == (const unsigned char *)NULL) {
        ptimage = tex->get_ram_mipmap_image(n);
        if (ptimage == (const unsigned char *)NULL) {
          GLCAT.warning()
            << "No mipmap level " << n << " defined for " << tex->get_name()
            << "\n";
          // No mipmap level n; stop here.
#ifndef OPENGLES
          if (is_at_least_gl_version(1, 2)) {
            // Tell the GL we have no more mipmaps for it to use.
            glTexParameteri(texture_target, GL_TEXTURE_MAX_LEVEL, n - mipmap_bias);
          }
#endif
          break;
        }
        image_ptr = ptimage;
      }

      const unsigned char *orig_image_ptr = image_ptr;
      size_t view_size = tex->get_ram_mipmap_view_size(n);
      image_ptr += view_size * gtc->get_view();
      if (one_page_only) {
        view_size = tex->get_ram_mipmap_page_size(n);
        image_ptr += view_size * z;
      }
      nassertr(image_ptr >= orig_image_ptr && image_ptr + view_size <= orig_image_ptr + tex->get_ram_mipmap_image_size(n), false);

      PTA_uchar bgr_image;
      if (!_supports_bgr && image_compression == Texture::CM_off) {
        // If the GL doesn't claim to support BGR, we may have to reverse
        // the component ordering of the image.
        image_ptr = fix_component_ordering(bgr_image, image_ptr, view_size,
                                           external_format, tex);
      }

      int width = tex->get_expected_mipmap_x_size(n);
      int height = tex->get_expected_mipmap_y_size(n);
      int depth = tex->get_expected_mipmap_z_size(n);

#ifdef DO_PSTATS
      _data_transferred_pcollector.add_level(view_size);
#endif
      switch (texture_target) {
#ifndef OPENGLES  // 1-d textures not supported by OpenGL ES.  Fall through.
      case GL_TEXTURE_1D:
        if (image_compression == Texture::CM_off) {
          glTexImage1D(page_target, n - mipmap_bias, internal_format,
                       width, 0,
                       external_format, component_type, image_ptr);
        } else {
          _glCompressedTexImage1D(page_target, n - mipmap_bias, external_format, width,
                                  0, view_size, image_ptr);
        }
        break;
#endif  // OPENGLES  // OpenGL ES will fall through.

#ifdef OPENGLES_2
      case GL_TEXTURE_3D_OES:
#endif
#ifndef OPENGLES
      case GL_TEXTURE_3D:
#endif
#ifndef OPENGLES_1
        if (_supports_3d_texture) {
          if (image_compression == Texture::CM_off) {
            _glTexImage3D(page_target, n - mipmap_bias, internal_format,
                          width, height, depth, 0,
                          external_format, component_type, image_ptr);
          } else {
            _glCompressedTexImage3D(page_target, n - mipmap_bias, external_format, width,
                                    height, depth,
                                    0, view_size, image_ptr);
          }
        } else {
          report_my_gl_errors();
          return false;
        }
        break;
#endif
#ifndef OPENGLES
      case GL_TEXTURE_2D_ARRAY_EXT:
        if (_supports_2d_texture_array) {
          if (image_compression == Texture::CM_off) {
            _glTexImage3D(page_target, n - mipmap_bias, internal_format,
                          width, height, depth, 0,
                          external_format, component_type, image_ptr);
          } else {
            _glCompressedTexImage3D(page_target, n - mipmap_bias, external_format, width,
                                    height, depth,
                                    0, view_size, image_ptr);
          }
        } else {
          report_my_gl_errors();
          return false;
        }
        break;
#endif

      default:
        if (image_compression == Texture::CM_off) {
          glTexImage2D(page_target, n - mipmap_bias, internal_format,
                       width, height, 0,
                       external_format, component_type, image_ptr);
        } else {
          _glCompressedTexImage2D(page_target, n - mipmap_bias, external_format,
                                  width, height, 0, view_size, image_ptr);
        }
      }
    }

    // Report the error message explicitly if the GL texture creation
    // failed.
    GLenum error_code = gl_get_error();
    if (error_code != GL_NO_ERROR) {
      GLCAT.error()
        << "GL texture creation failed for " << tex->get_name()
        << " : " << get_error_string(error_code) << "\n";

      gtc->_has_storage = false;
      return false;
    }
  }

  report_my_gl_errors();

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::upload_simple_texture
//       Access: Protected
//  Description: This is used as a standin for upload_texture
//               when the texture in question is unavailable (e.g. it
//               hasn't yet been loaded from disk).  Until the texture
//               image itself becomes available, we will render the
//               texture's "simple" image--a sharply reduced version
//               of the same texture.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
upload_simple_texture(CLP(TextureContext) *gtc) {
  report_my_gl_errors();

  PStatGPUTimer timer(this, _load_texture_pcollector);
  Texture *tex = gtc->get_texture();
  nassertr(tex != (Texture *)NULL, false);

  int internal_format = GL_RGBA;
#ifdef OPENGLES_2
  int external_format = GL_RGBA;
#else
  int external_format = GL_BGRA;
#endif

  const unsigned char *image_ptr = tex->get_simple_ram_image();
  if (image_ptr == (const unsigned char *)NULL) {
    return false;
  }

  size_t image_size = tex->get_simple_ram_image_size();
  PTA_uchar bgr_image;
  if (!_supports_bgr) {
    // If the GL doesn't claim to support BGR, we may have to reverse
    // the component ordering of the image.
    external_format = GL_RGBA;
    image_ptr = fix_component_ordering(bgr_image, image_ptr, image_size,
                                       external_format, tex);
  }

  int width = tex->get_simple_x_size();
  int height = tex->get_simple_y_size();
  int component_type = GL_UNSIGNED_BYTE;

  if (GLCAT.is_debug()) {
    GLCAT.debug()
      << "loading simple image for " << tex->get_name() << "\n";
  }

#ifndef OPENGLES
  // Turn off mipmaps for the simple texture.
  if (tex->uses_mipmaps()) {
    if (is_at_least_gl_version(1, 2)) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
  }
#endif

#ifdef DO_PSTATS
  _data_transferred_pcollector.add_level(image_size);
#endif

  glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
               width, height, 0,
               external_format, component_type, image_ptr);

  gtc->mark_simple_loaded();

  report_my_gl_errors();
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_texture_memory_size
//       Access: Protected
//  Description: Asks OpenGL how much texture memory is consumed by
//               the indicated texture (which is also the
//               currently-selected texture).
////////////////////////////////////////////////////////////////////
size_t CLP(GraphicsStateGuardian)::
get_texture_memory_size(Texture *tex) {
#ifdef OPENGLES  // Texture querying not supported on OpenGL ES.
  int width = tex->get_x_size();
  int height = tex->get_y_size();
  int depth = 1;
  int scale = 1;
  bool has_mipmaps = tex->uses_mipmaps();

  size_t num_bytes = 2;  // Temporary assumption?

#else
  GLenum target = get_texture_target(tex->get_texture_type());

  GLenum page_target = target;
  GLint scale = 1;
  if (target == GL_TEXTURE_CUBE_MAP) {
    // We need a particular page to get the level parameter from.
    page_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
    scale = 6;
  }

  GLint minfilter;
  glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &minfilter);
  bool has_mipmaps = is_mipmap_filter(minfilter);

  clear_my_gl_errors();

  GLint internal_format;
  glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);

  if (is_compressed_format(internal_format)) {
    // Try to get the compressed size.
    GLint image_size;
    glGetTexLevelParameteriv(page_target, 0,
                                GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &image_size);

    GLenum error_code = gl_get_error();
    if (error_code != GL_NO_ERROR) {
      if (GLCAT.is_debug()) {
        GLCAT.debug()
          << "Couldn't get compressed size for " << tex->get_name()
          << " : " << get_error_string(error_code) << "\n";
      }
      // Fall through to the noncompressed case.
    } else {
      return image_size * scale;
    }
  }

  // OK, get the noncompressed size.
  GLint red_size, green_size, blue_size, alpha_size,
    luminance_size, intensity_size;
  GLint depth_size = 0;
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_RED_SIZE, &red_size);
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_GREEN_SIZE, &green_size);
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_BLUE_SIZE, &blue_size);
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_ALPHA_SIZE, &alpha_size);
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_LUMINANCE_SIZE, &luminance_size);
  glGetTexLevelParameteriv(page_target, 0,
                              GL_TEXTURE_INTENSITY_SIZE, &intensity_size);
  if (_supports_depth_texture) {
    glGetTexLevelParameteriv(page_target, 0,
                                GL_TEXTURE_DEPTH_SIZE, &depth_size);
  }

  GLint width = 1, height = 1, depth = 1;
  glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_WIDTH, &width);
  glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_HEIGHT, &height);
  if (_supports_3d_texture || _supports_2d_texture_array) {
    glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_DEPTH, &depth);
  }

  report_my_gl_errors();

  size_t num_bits = (red_size + green_size + blue_size + alpha_size + luminance_size + intensity_size + depth_size);
  size_t num_bytes = (num_bits + 7) / 8;
#endif  // OPENGLES

  size_t result = num_bytes * width * height * depth * scale;
  if (has_mipmaps) {
    result = (result * 4) / 3;
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::check_nonresident_texture
//       Access: Private
//  Description: Checks the list of resident texture objects to see if
//               any have recently been evicted.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
check_nonresident_texture(BufferContextChain &chain) {
#ifndef OPENGLES  // Residency queries not supported by OpenGL ES.
  size_t num_textures = chain.get_count();
  if (num_textures == 0) {
    return;
  }

  CLP(TextureContext) **gtc_list = (CLP(TextureContext) **)alloca(num_textures * sizeof(CLP(TextureContext) *));
  GLuint *texture_list = (GLuint *)alloca(num_textures * sizeof(GLuint));
  size_t ti = 0;
  BufferContext *node = chain.get_first();
  while (node != (BufferContext *)NULL) {
    CLP(TextureContext) *gtc = DCAST(CLP(TextureContext), node);
    gtc_list[ti] = gtc;
    texture_list[ti] = gtc->_index;
    node = node->get_next();
    ++ti;
  }
  nassertv(ti == num_textures);
  GLboolean *results = (GLboolean *)alloca(num_textures * sizeof(GLboolean));
  bool all_resident = (glAreTexturesResident(num_textures, texture_list, results) != 0);

  report_my_gl_errors();

  if (!all_resident) {
    // Some are now nonresident.
    for (ti = 0; ti < num_textures; ++ti) {
      if (!results[ti]) {
        gtc_list[ti]->set_resident(false);
      }
    }
  }
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_extract_texture_data
//       Access: Protected
//  Description: The internal implementation of
//               extract_texture_data(), given an already-created
//               TextureContext.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
do_extract_texture_data(CLP(TextureContext) *gtc) {
  report_my_gl_errors();

  GLenum target = gtc->_target;
  if (target == GL_NONE) {
    return false;
  }

#ifndef OPENGLES
  // Make sure any incoherent writes to the texture have been synced.
  if (gtc->needs_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT)) {
    issue_memory_barrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
  }
#endif

  glBindTexture(target, gtc->_index);

  Texture *tex = gtc->get_texture();

  GLint wrap_u, wrap_v, wrap_w;
  GLint minfilter, magfilter;
  GLfloat border_color[4];

  glGetTexParameteriv(target, GL_TEXTURE_WRAP_S, &wrap_u);
  glGetTexParameteriv(target, GL_TEXTURE_WRAP_T, &wrap_v);
  wrap_w = GL_REPEAT;
  if (_supports_3d_texture) {
#ifdef OPENGLES_2
    glGetTexParameteriv(target, GL_TEXTURE_WRAP_R_OES, &wrap_w);
#endif
#ifndef OPENGLES
    glGetTexParameteriv(target, GL_TEXTURE_WRAP_R, &wrap_w);
#endif
  }
  if (_supports_2d_texture_array) {
#ifndef OPENGLES
    glGetTexParameteriv(target, GL_TEXTURE_WRAP_R, &wrap_w);
#endif
  }
  glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &minfilter);
  glGetTexParameteriv(target, GL_TEXTURE_MAG_FILTER, &magfilter);

#ifndef OPENGLES
  glGetTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border_color);
#endif

  GLenum page_target = target;
  if (target == GL_TEXTURE_CUBE_MAP) {
    // We need a particular page to get the level parameter from.
    page_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
  }

  GLint width = gtc->_width, height = gtc->_height, depth = gtc->_depth;
#ifndef OPENGLES
  glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_WIDTH, &width);
  if (target != GL_TEXTURE_1D) {
    glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_HEIGHT, &height);
  }

  if (_supports_3d_texture && target == GL_TEXTURE_3D) {
    glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_DEPTH, &depth);
  }
#ifndef OPENGLES
  else if (_supports_2d_texture_array && target == GL_TEXTURE_2D_ARRAY_EXT) {
    glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_DEPTH, &depth);
  }
#endif
  else if (target == GL_TEXTURE_CUBE_MAP) {
    depth = 6;
  }
#endif
  clear_my_gl_errors();
  if (width <= 0 || height <= 0 || depth <= 0) {
    GLCAT.error()
      << "No texture data for " << tex->get_name() << "\n";
    return false;
  }

  GLint internal_format = GL_RGBA;
#ifndef OPENGLES
  glGetTexLevelParameteriv(page_target, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
#endif  // OPENGLES

  // Make sure we were able to query those parameters properly.
  GLenum error_code = gl_get_error();
  if (error_code != GL_NO_ERROR) {
    GLCAT.error()
      << "Unable to query texture parameters for " << tex->get_name()
      << " : " << get_error_string(error_code) << "\n";

    return false;
  }

  Texture::ComponentType type = Texture::T_unsigned_byte;
  Texture::Format format = Texture::F_rgb;
  Texture::CompressionMode compression = Texture::CM_off;

  switch (internal_format) {
#ifndef OPENGLES
  case GL_COLOR_INDEX:
    format = Texture::F_color_index;
    break;
#endif
#if GL_DEPTH_COMPONENT != GL_DEPTH_COMPONENT24
  case GL_DEPTH_COMPONENT:
#endif
  case GL_DEPTH_COMPONENT16:
  case GL_DEPTH_COMPONENT24:
  case GL_DEPTH_COMPONENT32:
    type = Texture::T_unsigned_short;
    format = Texture::F_depth_component;
    break;
#ifndef OPENGLES
  case GL_DEPTH_COMPONENT32F:
    type = Texture::T_float;
    format = Texture::F_depth_component;
    break;
#endif
  case GL_DEPTH_STENCIL:
  case GL_DEPTH24_STENCIL8:
    type = Texture::T_unsigned_int_24_8;
    format = Texture::F_depth_stencil;
    break;
#ifndef OPENGLES_1
  case GL_DEPTH32F_STENCIL8:
    type = Texture::T_float;
    format = Texture::F_depth_stencil;
    break;
#endif
  case GL_RGBA:
  case 4:
    format = Texture::F_rgba;
    break;
  case GL_RGBA4:
    format = Texture::F_rgba4;
    break;
#ifdef OPENGLES
  case GL_RGBA8_OES:
    format = Texture::F_rgba8;
    break;
#else
  case GL_RGBA8:
    format = Texture::F_rgba8;
    break;
#endif
#ifndef OPENGLES
  case GL_RGBA12:
    type = Texture::T_unsigned_short;
    format = Texture::F_rgba12;
    break;
#endif

  case GL_RGB:
  case 3:
    format = Texture::F_rgb;
    break;
#ifndef OPENGLES
  case GL_RGB5:
    format = Texture::F_rgb5;
    break;
#endif
  case GL_RGB5_A1:
    format = Texture::F_rgba5;
    break;
#ifndef OPENGLES
  case GL_RGB8:
    format = Texture::F_rgb8;
    break;
  case GL_RGB12:
    format = Texture::F_rgb12;
    break;
  case GL_R3_G3_B2:
    format = Texture::F_rgb332;
    break;
#endif

#ifndef OPENGLES_1
  case GL_RGBA16F:
    type = Texture::T_float;
    format = Texture::F_rgba16;
    break;
  case GL_RGB16F:
    type = Texture::T_float;
    format = Texture::F_rgb16;
    break;
  case GL_RG16F:
    type = Texture::T_float;
    format = Texture::F_rg16;
    break;
  case GL_R16F:
    type = Texture::T_float;
    format = Texture::F_r16;
    break;
  case GL_RGBA32F:
    type = Texture::T_float;
    format = Texture::F_rgba32;
    break;
  case GL_RGB32F:
    type = Texture::T_float;
    format = Texture::F_rgb32;
    break;
  case GL_RG32F:
    type = Texture::T_float;
    format = Texture::F_rg32;
    break;
  case GL_R32F:
    type = Texture::T_float;
    format = Texture::F_r32;
    break;
#endif

#ifndef OPENGLES
  case GL_RGB16:
    type = Texture::T_unsigned_short;
    format = Texture::F_rgb16;
    break;
  case GL_RG16:
    type = Texture::T_unsigned_short;
    format = Texture::F_rg16;
    break;
  case GL_R16:
    type = Texture::T_unsigned_short;
    format = Texture::F_r16;
    break;
#endif

#ifdef OPENGLES_2
  case GL_RED_EXT:
  case GL_R8_EXT:
    format = Texture::F_red;
    break;
#endif
#ifndef OPENGLES
  case GL_R32I:
    type = Texture::T_int;
    format = Texture::F_r32i;
    break;
#endif

#ifndef OPENGLES
  case GL_RED:
    format = Texture::F_red;
    break;
  case GL_GREEN:
    format = Texture::F_green;
    break;
  case GL_BLUE:
    format = Texture::F_blue;
    break;
#endif  // OPENGLES
  case GL_ALPHA:
    format = Texture::F_alpha;
    break;
  case GL_LUMINANCE:
  case 1:
    format = Texture::F_luminance;
    break;
  case GL_LUMINANCE_ALPHA:
  case 2:
    format = Texture::F_luminance_alpha;
    break;

#ifndef OPENGLES_1
  case GL_SRGB:
  case GL_SRGB8:
    format = Texture::F_srgb;
    break;
  case GL_SRGB_ALPHA:
  case GL_SRGB8_ALPHA8:
    format = Texture::F_srgb_alpha;
    break;
  case GL_SLUMINANCE:
  case GL_SLUMINANCE8:
    format = Texture::F_sluminance;
    break;
  case GL_SLUMINANCE_ALPHA:
  case GL_SLUMINANCE8_ALPHA8:
    format = Texture::F_sluminance_alpha;
    break;
#endif

#ifndef OPENGLES
  case GL_COMPRESSED_RGB:
    format = Texture::F_rgb;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_RGBA:
    format = Texture::F_rgba;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_ALPHA:
    format = Texture::F_alpha;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_LUMINANCE:
    format = Texture::F_luminance;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_LUMINANCE_ALPHA:
    format = Texture::F_luminance_alpha;
    compression = Texture::CM_on;
    break;

  case GL_COMPRESSED_SRGB:
    format = Texture::F_srgb;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_SRGB_ALPHA:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_SLUMINANCE:
    format = Texture::F_sluminance;
    compression = Texture::CM_on;
    break;
  case GL_COMPRESSED_SLUMINANCE_ALPHA:
    format = Texture::F_sluminance_alpha;
    compression = Texture::CM_on;
    break;
#endif

  case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    format = Texture::F_rgb;
    compression = Texture::CM_dxt1;
    break;
  case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    format = Texture::F_rgbm;
    compression = Texture::CM_dxt1;
    break;
#ifndef OPENGLES_1
  case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
    format = Texture::F_srgb;
    compression = Texture::CM_dxt1;
    break;
  case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_dxt1;
    break;
#endif

#ifdef OPENGLES_2
  case GL_COMPRESSED_SRGB_PVRTC_2BPPV1_EXT:
    format = Texture::F_srgb;
    compression = Texture::CM_pvr1_2bpp;
    break;
  case GL_COMPRESSED_SRGB_ALPHA_PVRTC_2BPPV1_EXT:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_pvr1_2bpp;
    break;
  case GL_COMPRESSED_SRGB_PVRTC_4BPPV1_EXT:
    format = Texture::F_srgb;
    compression = Texture::CM_pvr1_4bpp;
    break;
  case GL_COMPRESSED_SRGB_ALPHA_PVRTC_4BPPV1_EXT:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_pvr1_4bpp;
    break;
#endif  // OPENGLES_2

#ifdef OPENGLES
  case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
    format = Texture::F_rgb;
    compression = Texture::CM_pvr1_2bpp;
    break;
  case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
    format = Texture::F_rgba;
    compression = Texture::CM_pvr1_2bpp;
    break;
  case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
    format = Texture::F_rgb;
    compression = Texture::CM_pvr1_4bpp;
    break;
  case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
    format = Texture::F_rgba;
    compression = Texture::CM_pvr1_4bpp;
    break;

#else
  case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    format = Texture::F_rgba;
    compression = Texture::CM_dxt3;
    break;
  case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    format = Texture::F_rgba;
    compression = Texture::CM_dxt5;
    break;
  case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_dxt3;
    break;
  case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
    format = Texture::F_srgb_alpha;
    compression = Texture::CM_dxt5;
    break;
  case GL_COMPRESSED_RGB_FXT1_3DFX:
    format = Texture::F_rgb;
    compression = Texture::CM_fxt1;
    break;
  case GL_COMPRESSED_RGBA_FXT1_3DFX:
    format = Texture::F_rgba;
    compression = Texture::CM_fxt1;
    break;
#endif
  default:
    GLCAT.warning()
      << "Unhandled internal format for " << tex->get_name()
      << " : " << hex << "0x" << internal_format << dec << "\n";
    return false;
  }

  // We don't want to call setup_texture() again; that resets too
  // much.  Instead, we'll just set the individual components.
  tex->set_x_size(width);
  tex->set_y_size(height);
  tex->set_z_size(depth);
  tex->set_component_type(type);
  tex->set_format(format);

  tex->set_wrap_u(get_panda_wrap_mode(wrap_u));
  tex->set_wrap_v(get_panda_wrap_mode(wrap_v));
  tex->set_wrap_w(get_panda_wrap_mode(wrap_w));
  tex->set_border_color(LColor(border_color[0], border_color[1],
                               border_color[2], border_color[3]));

  tex->set_minfilter(get_panda_filter_type(minfilter));
  //  tex->set_magfilter(get_panda_filter_type(magfilter));

  PTA_uchar image;
  size_t page_size = 0;

  if (!extract_texture_image(image, page_size, tex, target, page_target,
                             type, compression, 0)) {
    return false;
  }

  tex->set_ram_image(image, compression, page_size);

  if (tex->uses_mipmaps()) {
    // Also get the mipmap levels.
    GLint num_expected_levels = tex->get_expected_num_mipmap_levels();
    GLint highest_level = num_expected_levels;
#ifndef OPENGLES
    if (is_at_least_gl_version(1, 2)) {
      glGetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &highest_level);
      highest_level = min(highest_level, num_expected_levels);
    }
#endif
    for (int n = 1; n <= highest_level; ++n) {
      if (!extract_texture_image(image, page_size, tex, target, page_target,
                                 type, compression, n)) {
        return false;
      }
      tex->set_ram_mipmap_image(n, image, page_size);
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::extract_texture_image
//       Access: Protected
//  Description: Called from extract_texture_data(), this gets just
//               the image array for a particular mipmap level (or for
//               the base image).
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
extract_texture_image(PTA_uchar &image, size_t &page_size,
                      Texture *tex, GLenum target, GLenum page_target,
                      Texture::ComponentType type,
                      Texture::CompressionMode compression, int n) {
#ifdef OPENGLES  // Extracting texture data unsupported in OpenGL ES.
    nassertr(false, false);
    return false;
#else

  if (target == GL_TEXTURE_CUBE_MAP) {
    // A cube map, compressed or uncompressed.  This we must extract
    // one page at a time.

    // If the cube map is compressed, we assume that all the
    // compressed pages are exactly the same size.  OpenGL doesn't
    // make this assumption, but it happens to be true for all
    // currently extant compression schemes, and it makes things
    // simpler for us.  (It also makes things much simpler for the
    // graphics hardware, so it's likely to continue to be true for a
    // while at least.)

    GLenum external_format = get_external_image_format(tex);
    GLenum pixel_type = get_component_type(type);
    page_size = tex->get_expected_ram_mipmap_page_size(n);

    if (compression != Texture::CM_off) {
      GLint image_size;
      glGetTexLevelParameteriv(page_target, n,
                                  GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &image_size);
      nassertr(image_size <= (int)page_size, false);
      page_size = image_size;
    }

    image = PTA_uchar::empty_array(page_size * 6);

    for (int z = 0; z < 6; ++z) {
      page_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + z;

      if (compression == Texture::CM_off) {
        glGetTexImage(page_target, n, external_format, pixel_type,
                         image.p() + z * page_size);
      } else {
        _glGetCompressedTexImage(page_target, 0, image.p() + z * page_size);
      }
    }

  } else if (compression == Texture::CM_off) {
    // An uncompressed 1-d, 2-d, or 3-d texture.
    image = PTA_uchar::empty_array(tex->get_expected_ram_mipmap_image_size(n));
    GLenum external_format = get_external_image_format(tex);
    GLenum pixel_type = get_component_type(type);
    glGetTexImage(target, n, external_format, pixel_type, image.p());

  } else {
    // A compressed 1-d, 2-d, or 3-d texture.
    GLint image_size;
    glGetTexLevelParameteriv(target, n, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &image_size);
    page_size = image_size / tex->get_z_size();
    image = PTA_uchar::empty_array(image_size);

    // Some drivers (ATI!) seem to try to overstuff more bytes in the
    // array than they asked us to allocate (that is, more bytes than
    // GL_TEXTURE_COMPRESSED_IMAGE_SIZE), requiring us to overallocate
    // and then copy the result into our final buffer.  Sheesh.

    // We'll only do this for small textures (the ATI bug doesn't
    // *seem* to affect large textures), to save on the overhead of
    // the double-copy, and reduce risk from an overly-large alloca().
#ifndef NDEBUG
    static const int max_trouble_buffer = 102400;
#else
    static const int max_trouble_buffer = 1024;
#endif
    if (image_size < max_trouble_buffer) {
      static const int extra_space = 32;
      unsigned char *buffer = (unsigned char *)alloca(image_size + extra_space);
#ifndef NDEBUG
      // Tag the buffer with a specific byte so we can report on
      // whether that driver bug is still active.
      static unsigned char keep_token = 0x00;
      unsigned char token = ++keep_token;
      memset(buffer + image_size, token, extra_space);
#endif
      _glGetCompressedTexImage(target, n, buffer);
      memcpy(image.p(), buffer, image_size);
#ifndef NDEBUG
      int count = extra_space;
      while (count > 0 && buffer[image_size + count - 1] == token) {
        --count;
      }
      if (count != 0) {
        GLCAT.warning()
          << "GL graphics driver overfilled " << count
          << " bytes into a " << image_size
          << "-byte buffer provided to glGetCompressedTexImage()\n";
      }

      // This had better not equal the amount of buffer space we set
      // aside.  If it does, we assume the driver might have
      // overfilled even our provided extra buffer.
      nassertr(count != extra_space, true)
#endif  // NDEBUG
    } else {
      _glGetCompressedTexImage(target, n, image.p());
    }
  }

  // Now see if we were successful.
  GLenum error_code = gl_get_error();
  if (error_code != GL_NO_ERROR) {
    GLCAT.error()
      << "Unable to extract texture for " << *tex
      << ", mipmap level " << n
      << " : " << get_error_string(error_code) << "\n";
    nassertr(false, false);
    return false;
  }
  return true;
#endif  // OPENGLES
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_point_size
//       Access: Protected
//  Description: Internally sets the point size parameters after any
//               of the properties have changed that might affect
//               this.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_point_size() {
#ifndef OPENGLES_2
  if (!_point_perspective) {
    // Normal, constant-sized points.  Here _point_size is a width in
    // pixels.
    static LVecBase3f constant(1.0f, 0.0f, 0.0f);
    _glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, constant.get_data());

  } else {
    // Perspective-sized points.  Here _point_size is a width in 3-d
    // units.  To arrange that, we need to figure out the appropriate
    // scaling factor based on the current viewport and projection
    // matrix.
    LVector3 height(0.0f, _point_size, 1.0f);
    height = height * _projection_mat->get_mat();
    height = height * _internal_transform->get_scale()[1];
    PN_stdfloat s = height[1] * _viewport_height / _point_size;

    if (_current_lens->is_orthographic()) {
      // If we have an orthographic lens in effect, we don't actually
      // apply a perspective transform: we just scale the points once,
      // regardless of the distance from the camera.
      LVecBase3f constant(1.0f / (s * s), 0.0f, 0.0f);
      _glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, constant.get_data());

    } else {
      // Otherwise, we give it a true perspective adjustment.
      LVecBase3f square(0.0f, 0.0f, 1.0f / (s * s));
      _glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, square.get_data());
    }
  }

  report_my_gl_errors();
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::get_supports_cg_profile
//       Access: Public, Virtual
//  Description: Returns true if this particular GSG supports the
//               specified Cg Shader Profile.
////////////////////////////////////////////////////////////////////
bool CLP(GraphicsStateGuardian)::
get_supports_cg_profile(const string &name) const {
#if !defined(HAVE_CG) || defined(OPENGLES)
  return false;
#else
  CGprofile profile = cgGetProfile(name.c_str());

  if (profile == CG_PROFILE_UNKNOWN) {
    GLCAT.error() << name << ", unknown Cg-profile\n";
    return false;
  }
  return (cgGLIsProfileSupported(profile) != 0);
#endif
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::bind_fbo
//       Access: Protected
//  Description: Binds a framebuffer object.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
bind_fbo(GLuint fbo) {
  if (_current_fbo == fbo) {
    return;
  }

  PStatGPUTimer timer(this, _fbo_bind_pcollector);

  nassertv(_glBindFramebuffer != 0);
#if defined(OPENGLES_2)
  _glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#elif defined(OPENGLES_1)
  _glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
#else
  _glBindFramebuffer(GL_FRAMEBUFFER_EXT, fbo);
#endif

  _current_fbo = fbo;
}

////////////////////////////////////////////////////////////////////
//  GL stencil code section
////////////////////////////////////////////////////////////////////

static int gl_stencil_comparison_function_array [ ] = {
  GL_NEVER,
  GL_LESS,
  GL_EQUAL,
  GL_LEQUAL,
  GL_GREATER,
  GL_NOTEQUAL,
  GL_GEQUAL,
  GL_ALWAYS,
};

static int gl_stencil_operations_array [ ] = {
  GL_KEEP,
  GL_ZERO,
  GL_REPLACE,
#ifdef OPENGLES_1
  GL_INCR_WRAP_OES,
  GL_DECR_WRAP_OES,
#else
  GL_INCR_WRAP,
  GL_DECR_WRAP,
#endif
  GL_INVERT,

  GL_INCR,
  GL_DECR,
};

void __glActiveStencilFace (GraphicsStateGuardian *gsg, GLenum face) {
  CLP(GraphicsStateGuardian) *glgsg;

  glgsg = (CLP(GraphicsStateGuardian) *) gsg;
  if (gsg -> get_supports_two_sided_stencil ( ) &&
      glgsg -> _glActiveStencilFaceEXT) {
    if (face == GL_FRONT) {
      // glActiveStencilFaceEXT (GL_FRONT);
      glgsg -> _glActiveStencilFaceEXT (GL_FRONT);
    }
    else {
      // glActiveStencilFaceEXT (GL_BACK);
      glgsg -> _glActiveStencilFaceEXT (GL_BACK);
    }
  }
}

void gl_front_stencil_function (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {

  __glActiveStencilFace (stencil_render_states -> _gsg, GL_FRONT);
  glStencilFunc
    (
     gl_stencil_comparison_function_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_front_comparison_function)],
     stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_reference),
     stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_read_mask)
     );
}
void gl_front_stencil_operation (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {
  __glActiveStencilFace (stencil_render_states -> _gsg, GL_FRONT);
  glStencilOp
    (
     gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_front_stencil_fail_operation)],
     gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_front_stencil_pass_z_fail_operation)],
     gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_front_stencil_pass_z_pass_operation)]
     );
}

void gl_back_stencil_function (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {

  bool supports_two_sided_stencil;

  supports_two_sided_stencil = stencil_render_states -> _gsg -> get_supports_two_sided_stencil ( );
  if (supports_two_sided_stencil) {
    __glActiveStencilFace (stencil_render_states -> _gsg, GL_BACK);
    glStencilFunc
      (
       gl_stencil_comparison_function_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_back_comparison_function)],
       stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_reference),
       stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_read_mask)
       );
  }
}

void gl_back_stencil_operation (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {

  bool supports_two_sided_stencil;

  supports_two_sided_stencil = stencil_render_states -> _gsg -> get_supports_two_sided_stencil ( );
  if (supports_two_sided_stencil) {
    __glActiveStencilFace (stencil_render_states -> _gsg, GL_BACK);
    glStencilOp
      (
       gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_back_stencil_fail_operation)],
       gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_back_stencil_pass_z_fail_operation)],
       gl_stencil_operations_array [stencil_render_states -> get_stencil_render_state (StencilRenderStates::SRS_back_stencil_pass_z_pass_operation)]
       );
  }
}

void gl_front_back_stencil_function (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {
  gl_front_stencil_function (stencil_render_state, stencil_render_states);
  gl_back_stencil_function (stencil_render_state, stencil_render_states);
}

void gl_stencil_function (StencilRenderStates::StencilRenderState stencil_render_state, StencilRenderStates *stencil_render_states) {

  StencilType render_state_value;
  bool supports_two_sided_stencil;

  supports_two_sided_stencil = stencil_render_states -> _gsg -> get_supports_two_sided_stencil ( );

  render_state_value = stencil_render_states -> get_stencil_render_state (stencil_render_state);
  switch (stencil_render_state) {
  case StencilRenderStates::SRS_front_enable:
    if (render_state_value) {
      glEnable (GL_STENCIL_TEST);
    }
    else {
      glDisable (GL_STENCIL_TEST);
    }
    break;
#ifndef OPENGLES
  case StencilRenderStates::SRS_back_enable:
    if (supports_two_sided_stencil) {
      if (render_state_value) {
        glEnable (GL_STENCIL_TEST_TWO_SIDE_EXT);
      }
      else {
        glDisable (GL_STENCIL_TEST_TWO_SIDE_EXT);
      }
    }
    break;
#endif

  case StencilRenderStates::SRS_write_mask:
    glStencilMask (render_state_value);
    break;

  default:
    break;
  }
}

void gl_set_stencil_functions (StencilRenderStates *stencil_render_states) {

  if (stencil_render_states) {
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_front_enable, gl_stencil_function);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_back_enable, gl_stencil_function);

    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_front_comparison_function, gl_front_stencil_function);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_front_stencil_fail_operation,  gl_front_stencil_operation);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_front_stencil_pass_z_fail_operation, gl_front_stencil_operation);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_front_stencil_pass_z_pass_operation, gl_front_stencil_operation);

    // GL seems to support different read masks and/or reference values for front and back, but DX does not.
    // This needs to be cross-platform so do it the DX way by setting the same read mask and reference for both front and back.
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_reference, gl_front_back_stencil_function);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_read_mask, gl_front_back_stencil_function);

    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_write_mask, gl_stencil_function);

    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_back_comparison_function, gl_back_stencil_function);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_back_stencil_fail_operation, gl_back_stencil_operation);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_back_stencil_pass_z_fail_operation, gl_back_stencil_operation);
    stencil_render_states -> set_stencil_function (StencilRenderStates::SRS_back_stencil_pass_z_pass_operation, gl_back_stencil_operation);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_stencil
//       Access: Protected
//  Description: Set stencil render states.
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_stencil() {
  if (!_supports_stencil) {
    return;
  }

  const StencilAttrib *stencil = DCAST(StencilAttrib, _target_rs->get_attrib_def(StencilAttrib::get_class_slot()));

  StencilRenderStates *stencil_render_states;
  stencil_render_states = this -> _stencil_render_states;
  if (stencil && stencil_render_states) {

    // DEBUG
    if (false) {
      GLCAT.debug() << "STENCIL STATE CHANGE\n";
      GLCAT.debug() << "\n"
                    << "SRS_front_enable " << (int)stencil -> get_render_state (StencilAttrib::SRS_front_enable) << "\n"
                    << "SRS_back_enable " << (int)stencil -> get_render_state (StencilAttrib::SRS_back_enable) << "\n"
                    << "SRS_front_comparison_function " << (int)stencil -> get_render_state (StencilAttrib::SRS_front_comparison_function) << "\n"
                    << "SRS_front_stencil_fail_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_front_stencil_fail_operation) << "\n"
                    << "SRS_front_stencil_pass_z_fail_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_front_stencil_pass_z_fail_operation) << "\n"
                    << "SRS_front_stencil_pass_z_pass_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_front_stencil_pass_z_pass_operation) << "\n"
                    << "SRS_reference " << (int)stencil -> get_render_state (StencilAttrib::SRS_reference) << "\n"
                    << "SRS_read_mask " << (int)stencil -> get_render_state (StencilAttrib::SRS_read_mask) << "\n"
                    << "SRS_write_mask " << (int)stencil -> get_render_state (StencilAttrib::SRS_write_mask) << "\n"
                    << "SRS_back_comparison_function " << (int)stencil -> get_render_state (StencilAttrib::SRS_back_comparison_function) << "\n"
                    << "SRS_back_stencil_fail_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_back_stencil_fail_operation) << "\n"
                    << "SRS_back_stencil_pass_z_fail_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_back_stencil_pass_z_fail_operation) << "\n"
                    << "SRS_back_stencil_pass_z_pass_operation " << (int)stencil -> get_render_state (StencilAttrib::SRS_back_stencil_pass_z_pass_operation) << "\n";
    }

    {
      bool on;

      on = false;
      stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_enable, stencil -> get_render_state (StencilAttrib::SRS_front_enable));
      if (stencil -> get_render_state (StencilAttrib::SRS_front_enable)) {
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_comparison_function, stencil -> get_render_state (StencilAttrib::SRS_front_comparison_function));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_stencil_fail_operation, stencil -> get_render_state (StencilAttrib::SRS_front_stencil_fail_operation));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_stencil_pass_z_fail_operation, stencil -> get_render_state (StencilAttrib::SRS_front_stencil_pass_z_fail_operation));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_stencil_pass_z_pass_operation, stencil -> get_render_state (StencilAttrib::SRS_front_stencil_pass_z_pass_operation));
        on = true;
      }

      stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_enable, stencil -> get_render_state (StencilAttrib::SRS_back_enable));
      if (stencil -> get_render_state (StencilAttrib::SRS_back_enable)) {
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_comparison_function, stencil -> get_render_state (StencilAttrib::SRS_back_comparison_function));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_stencil_fail_operation, stencil -> get_render_state (StencilAttrib::SRS_back_stencil_fail_operation));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_stencil_pass_z_fail_operation, stencil -> get_render_state (StencilAttrib::SRS_back_stencil_pass_z_fail_operation));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_stencil_pass_z_pass_operation, stencil -> get_render_state (StencilAttrib::SRS_back_stencil_pass_z_pass_operation));
        on = true;
      }

      if (on) {
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_reference, stencil -> get_render_state (StencilAttrib::SRS_reference));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_read_mask, stencil -> get_render_state (StencilAttrib::SRS_read_mask));
        stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_write_mask, stencil -> get_render_state (StencilAttrib::SRS_write_mask));
      }
    }

    if (stencil -> get_render_state (StencilAttrib::SRS_clear)) {
      GLbitfield mask = 0;

      // clear stencil buffer
      glClearStencil(stencil -> get_render_state (StencilAttrib::SRS_clear_value));
      mask |= GL_STENCIL_BUFFER_BIT;
      glClear(mask);
    }
  }
  else {
    stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_front_enable, 0);
    stencil_render_states -> set_stencil_render_state (true, StencilRenderStates::SRS_back_enable, 0);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: GLGraphicsStateGuardian::do_issue_scissor
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
void CLP(GraphicsStateGuardian)::
do_issue_scissor() {
  const ScissorAttrib *target_scissor = DCAST(ScissorAttrib, _target_rs->get_attrib_def(ScissorAttrib::get_class_slot()));

  if (target_scissor->is_off()) {
    if (_scissor_enabled) {
      glDisable(GL_SCISSOR_TEST);
      _scissor_enabled = false;
    }
  } else {
    if (!_scissor_enabled) {
      glEnable(GL_SCISSOR_TEST);
      _scissor_enabled = true;
    }

    const LVecBase4 &frame = target_scissor->get_frame();

    int x = (int)(_viewport_x + _viewport_width * frame[0] + 0.5f);
    int y = (int)(_viewport_y + _viewport_height * frame[2] + 0.5f);
    int width = (int)(_viewport_width * (frame[1] - frame[0]) + 0.5f);
    int height = (int)(_viewport_height * (frame[3] - frame[2]) + 0.5f);

    glScissor(x, y, width, height);
  }
}
