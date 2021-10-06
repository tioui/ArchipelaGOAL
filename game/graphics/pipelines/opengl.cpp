/*!
 * @file opengl.cpp
 * Lower-level OpenGL implementation.
 */

#include <memory>
#include <mutex>
#include <condition_variable>

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_impl_glfw.h"
#include "third-party/imgui/imgui_impl_opengl3.h"

#include "opengl.h"

#include "game/graphics/gfx.h"
#include "game/graphics/display.h"
#include "game/graphics/opengl_renderer/OpenGLRenderer.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/graphics/dma/dma_copy.h"
#include "game/system/newpad.h"
#include "common/log/log.h"
#include "common/goal_constants.h"
#include "game/runtime.h"
#include "common/util/Timer.h"
#include "game/graphics/opengl_renderer/debug_gui.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

namespace {

struct GraphicsData {
  // vsync
  std::mutex sync_mutex;
  std::condition_variable sync_cv;

  // dma chain transfer
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  FixedChunkDmaCopier dma_copier;

  // texture pool
  std::shared_ptr<TexturePool> texture_pool;

  // temporary opengl renderer
  OpenGLRenderer ogl_renderer;

  OpenGlDebugGui debug_gui;

  Serializer loaded_dump;

  void serialize(Serializer& ser) {
    dma_copier.serialize_last_result(ser);
    ogl_renderer.serialize(ser);
  }

  GraphicsData()
      : dma_copier(EE_MAIN_MEM_SIZE),
        texture_pool(std::make_shared<TexturePool>()),
        ogl_renderer(texture_pool) {}
};

std::unique_ptr<GraphicsData> g_gfx_data;

void SetDisplayCallbacks(GLFWwindow* d) {
  glfwSetKeyCallback(
      d, [](GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int /*mods*/) {
        if (action == GlfwKeyAction::Press) {
          // lg::debug("KEY PRESS:   key: {} scancode: {} mods: {:X}", key, scancode, mods);
          Pad::OnKeyPress(key);
        } else if (action == GlfwKeyAction::Release) {
          // lg::debug("KEY RELEASE: key: {} scancode: {} mods: {:X}", key, scancode, mods);
          Pad::OnKeyRelease(key);
        }
      });
}

void ErrorCallback(int err, const char* msg) {
  lg::error("GLFW ERR {}: " + std::string(msg), err);
}

bool HasError() {
  const char* ptr;
  if (glfwGetError(&ptr) != GLFW_NO_ERROR) {
    lg::error("glfw error: {}", ptr);
    return true;
  } else {
    return false;
  }
}

}  // namespace

static bool gl_inited = false;
static int gl_init(GfxSettings& settings) {
  if (glfwSetErrorCallback(ErrorCallback) != NULL) {
    lg::warn("glfwSetErrorCallback has been re-set!");
  }

  if (glfwInit() == GLFW_FALSE) {
    lg::error("glfwInit error");
    return 1;
  }

  // request an OpenGL 4.3 Core context
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);  // 4.3
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // core profile, not compat
  // debug check
  if (settings.debug) {
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
  } else {
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_FALSE);
  }

  return 0;
}

static void gl_exit() {
  g_gfx_data.reset();
  glfwTerminate();
  glfwSetErrorCallback(NULL);
  gl_inited = false;
}

static std::shared_ptr<GfxDisplay> gl_make_main_display(int width,
                                                        int height,
                                                        const char* title,
                                                        GfxSettings& settings) {
  GLFWwindow* window = glfwCreateWindow(width, height, title, NULL, NULL);

  if (!window) {
    lg::error("gl_make_main_display failed - Could not create display window");
    return NULL;
  }

  glfwMakeContextCurrent(window);

  if (!gl_inited && !gladLoadGL()) {
    lg::error("GL init fail");
    return NULL;
  }
  g_gfx_data = std::make_unique<GraphicsData>();
  gl_inited = true;

  // enable vsync by default
  // glfwSwapInterval(1);
  glfwSwapInterval(settings.vsync);

  SetDisplayCallbacks(window);
  Pad::initialize();

  if (HasError()) {
    lg::error("gl_make_main_display error");
    return NULL;
  }

  std::shared_ptr<GfxDisplay> display = std::make_shared<GfxDisplay>(window);
  // lg::debug("init display #x{:x}", (uintptr_t)display);

  // setup imgui

  // check that version of the library is okay
  IMGUI_CHECKVERSION();

  // this does initialization for stuff like the font data
  ImGui::CreateContext();

  // set up to get inputs for this window
  ImGui_ImplGlfw_InitForOpenGL(window, true);

  // NOTE: imgui's setup calls functions that may fail intentionally, and attempts to disable error
  // reporting so these errors are invisible. But it does not work, and some weird X11 default
  // cursor error is set here that we clear.
  glfwGetError(NULL);

  // set up the renderer
  ImGui_ImplOpenGL3_Init("#version 130");

  return display;
}

static void gl_kill_display(GfxDisplay* display) {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(display->window_glfw);
}

void make_gfx_dump() {
  Timer ser_timer;
  Serializer ser;

  // save the dma chain and renderer state
  g_gfx_data->serialize(ser);
  auto result = ser.get_save_result();
  Timer compression_timer;
  auto compressed = compression::compress_zstd(result.first, result.second);
  lg::info("Serialized graphics state in {:.1f} ms, {:.3f} MB, compressed {:.3f} MB {:.1f} ms",
           ser_timer.getMs(), ((double)result.second) / (1 << 20),
           ((double)compressed.size() / (1 << 20)), compression_timer.getMs());

  file_util::create_dir_if_needed(file_util::get_file_path({"gfx_dumps"}));
  file_util::write_binary_file(
      file_util::get_file_path({"gfx_dumps", g_gfx_data->debug_gui.dump_name()}), compressed.data(),
      compressed.size());
}

void render_game_frame(int width, int height) {
  // wait for a copied chain.
  bool got_chain = false;
  {
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    // note: there's a timeout here. If the engine is messed up and not sending us frames,
    // we still want to run the glfw loop.
    got_chain = g_gfx_data->dma_cv.wait_for(lock, std::chrono::milliseconds(20),
                                            [=] { return g_gfx_data->has_data_to_render; });
  }

  // render that chain.
  if (got_chain) {
    //      g_gfx_data->ogl_renderer.render(DmaFollower(g_gfx_data->dma_copier.get_last_input_data(),
    //                                                  g_gfx_data->dma_copier.get_last_input_offset()),
    //                                      width, height);

    // we want to serialize before rendering
    if (g_gfx_data->debug_gui.want_save()) {
      make_gfx_dump();
      g_gfx_data->debug_gui.want_save() = false;
    }

    auto& chain = g_gfx_data->dma_copier.get_last_result();
    g_gfx_data->frame_idx_of_input_data = g_gfx_data->frame_idx;
    g_gfx_data->ogl_renderer.render(DmaFollower(chain.data.data(), chain.start_offset), width,
                                    height, g_gfx_data->debug_gui.should_draw_render_debug(),
                                    false);
  }

  // before vsync, mark the chain as rendered.
  {
    // should be fine to remove this mutex if the game actually waits for vsync to call
    // send_chain again. but let's be safe for now.
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    g_gfx_data->has_data_to_render = false;
    g_gfx_data->sync_cv.notify_all();
  }
}

void render_dump_frame(int width, int height) {
  Timer deser_timer;
  if (g_gfx_data->debug_gui.want_dump_load()) {
    auto data = file_util::read_binary_file(
        file_util::get_file_path({"gfx_dumps", g_gfx_data->debug_gui.dump_name()}));
    auto decompressed = compression::decompress_zstd(data.data(), data.size());
    g_gfx_data->loaded_dump = Serializer(decompressed.data(), decompressed.size());
  }

  g_gfx_data->loaded_dump.reset_load();
  g_gfx_data->serialize(g_gfx_data->loaded_dump);

  if (g_gfx_data->debug_gui.want_dump_load()) {
    lg::info("Loaded and deserialized graphics state in {:.1f} ms, {:.3f} MB", deser_timer.getMs(),
             ((double)g_gfx_data->loaded_dump.data_size()) / (1 << 20));
  }
  g_gfx_data->debug_gui.want_dump_load() = false;

  auto& chain = g_gfx_data->dma_copier.get_last_result();
  g_gfx_data->ogl_renderer.render(DmaFollower(chain.data.data(), chain.start_offset), width, height,
                                  g_gfx_data->debug_gui.should_draw_render_debug(), true);
}

static void gl_render_display(GfxDisplay* display) {
  GLFWwindow* window = display->window_glfw;

  // poll events
  glfwPollEvents();
  glfwMakeContextCurrent(window);
  Pad::update_gamepads();

  // imgui start of frame
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  int width, height;
  glfwGetFramebufferSize(window, &width, &height);

  if (g_gfx_data->debug_gui.want_dump_replay()) {
    render_dump_frame(width, height);
  } else if (g_gfx_data->debug_gui.should_advance_frame()) {
    render_game_frame(width, height);
  }

  // render imgui
  g_gfx_data->debug_gui.draw(g_gfx_data->dma_copier.get_last_result().stats);
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  // actual vsync
  g_gfx_data->debug_gui.finish_frame();
  glfwSwapBuffers(window);
  g_gfx_data->debug_gui.start_frame();

  // toggle even odd and wake up engine waiting on vsync.
  if (!g_gfx_data->debug_gui.want_dump_replay()) {
    std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
    g_gfx_data->frame_idx++;

    g_gfx_data->sync_cv.notify_all();
  }

  // exit if display window was closed
  if (glfwWindowShouldClose(window)) {
    // Display::KillDisplay(window);
    MasterExit = 2;
  }
}

/*!
 * Wait for the next vsync. Returns 0 or 1 depending on if frame is even or odd.
 * Called from the game thread, on a GOAL stack.
 */
u32 gl_vsync() {
  if (!g_gfx_data) {
    return 0;
  }

  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  auto init_frame = g_gfx_data->frame_idx_of_input_data;
  g_gfx_data->sync_cv.wait(lock, [=] { return g_gfx_data->frame_idx > init_frame; });

  return g_gfx_data->frame_idx & 1;
}

u32 gl_sync_path() {
  if (!g_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  if (!g_gfx_data->has_data_to_render) {
    return 0;
  }
  g_gfx_data->sync_cv.wait(lock, [=] { return !g_gfx_data->has_data_to_render; });
  return 0;
}

/*!
 * Send DMA to the renderer.
 * Called from the game thread, on a GOAL stack.
 */
void gl_send_chain(const void* data, u32 offset) {
  if (g_gfx_data) {
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    if (g_gfx_data->has_data_to_render) {
      lg::error(
          "Gfx::send_chain called when the graphics renderer has pending data. Was this called "
          "multiple times per frame?");
      return;
    }

    // we copy the dma data and give a copy of it to the render.
    // the copy has a few advantages:
    // - if the game code has a bug and corrupts the DMA buffer, the renderer won't see it.
    // - the copied DMA is much smaller than the entire game memory, so it can be dumped to a file
    //    separate of the entire RAM.
    // - it verifies the DMA data is valid early on.
    // but it may also be pretty expensive. Both the renderer and the game wait on this to complete.

    // The renderers should just operate on DMA chains, so eliminating this step in the future may
    // be easy.

    g_gfx_data->dma_copier.run(data, offset);

    g_gfx_data->has_data_to_render = true;
    g_gfx_data->dma_cv.notify_all();
  }
}

void gl_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  // block
  if (g_gfx_data && !g_gfx_data->debug_gui.want_dump_replay()) {
    // just pass it to the texture pool.
    // the texture pool will take care of locking.
    // we don't want to lock here for the entire duration of the conversion.
    g_gfx_data->texture_pool->handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr);
  }
}

void gl_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_gfx_data && !g_gfx_data->debug_gui.want_dump_replay()) {
    g_gfx_data->texture_pool->relocate(destination, source, format);
  }
}

void gl_poll_events() {
  glfwPollEvents();
}

const GfxRendererModule moduleOpenGL = {
    gl_init,                // init
    gl_make_main_display,   // make_main_display
    gl_kill_display,        // kill_display
    gl_render_display,      // render_display
    gl_exit,                // exit
    gl_vsync,               // vsync
    gl_sync_path,           // sync_path
    gl_send_chain,          // send_chain
    gl_texture_upload_now,  // texture_upload_now
    gl_texture_relocate,    // texture_relocate
    gl_poll_events,         // poll_events
    GfxPipeline::OpenGL,    // pipeline
    "OpenGL 3.3"            // name
};
