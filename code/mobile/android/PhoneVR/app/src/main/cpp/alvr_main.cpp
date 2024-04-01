#include "alvr_client_core.h"
#include "cardboard.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <deque>
#include <jni.h>
#include <map>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common.h"

void log(AlvrLogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buf[1024];
    int count = vsnprintf(buf, sizeof(buf), format, args);
    if (count > (int) sizeof(buf))
        count = (int) sizeof(buf);
    if (count > 0 && buf[count - 1] == '\n')
        buf[count - 1] = '\0';

    alvr_log(level, buf);

    va_end(args);
}

#define error(...) log(ALVR_LOG_LEVEL_ERROR, __VA_ARGS__)
#define info(...)  log(ALVR_LOG_LEVEL_INFO, __VA_ARGS__)
#define debug(...) log(ALVR_LOG_LEVEL_DEBUG, __VA_ARGS__)

uint64_t HEAD_ID = alvr_path_string_to_id("/user/head");

// Note: the Cardboard SDK cannot estimate display time and an heuristic is used instead.
const uint64_t VSYNC_QUEUE_INTERVAL_NS = 50e6;
const float FLOOR_HEIGHT = 1.5;
const int MAXIMUM_TRACKING_FRAMES = 360;

struct Pose {
    float position[3];
    AlvrQuat orientation;
};

GLuint LoadGLShader(GLenum type, const char *shader_source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &shader_source, nullptr);
    glCompileShader(shader);

    // Get the compilation status.
    GLint compile_status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);

    // If the compilation failed, delete the shader and show an error.
    if (compile_status == 0) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len == 0) {
            return 0;
        }

        std::vector<char> info_string(info_len);
        glGetShaderInfoLog(shader, info_string.size(), nullptr, info_string.data());
        // LOGE("Could not compile shader of type %d: %s", type, info_string.data());
        glDeleteShader(shader);
        return 0;
    } else {
        return shader;
    }
}

namespace {
    // Simple shaders to render camera Texture files without any lighting.
    constexpr const char *camVertexShader =
        R"glsl(
    uniform mat4 u_MVP;
    attribute vec4 a_Position;
    attribute vec2 a_UV;
    varying vec2 v_UV;

    void main() {
      v_UV = a_UV;
      gl_Position = a_Position;
    })glsl";

    constexpr const char *camFragmentShader =
        R"glsl(
    #extension GL_OES_EGL_image_external : require
    precision mediump float;
    varying vec2 v_UV;
    uniform samplerExternalOES sTexture;
    void main() {
        gl_FragColor = texture2D(sTexture, v_UV);
    })glsl";

    static int passthrough_program_ = 0;
    static int texture_position_param_ = 0;
    static int texture_uv_param_ = 0;
    static int texture_mvp_param_ = 0;

    float passthrough_tex_coords[] = {0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0};
}   // namespace

struct NativeContext {
    JavaVM *javaVm = nullptr;
    jobject javaContext = nullptr;

    CardboardHeadTracker *headTracker = nullptr;
    CardboardLensDistortion *lensDistortion = nullptr;
    CardboardDistortionRenderer *distortionRenderer = nullptr;

    int screenWidth = 0;
    int screenHeight = 0;

    bool renderingParamsChanged = true;
    bool glContextRecreated = false;

    bool running = false;
    bool streaming = false;
    bool passthrough = false;
    std::thread inputThread;

    // Une one texture per eye, no need for swapchains.
    GLuint lobbyTextures[2] = {};
    GLuint streamTextures[2] = {};

    GLuint cameraTexture = 0;
    GLuint passthroughTexture = 0;
    CardboardEyeTextureDescription passthrough_left_eye;
    CardboardEyeTextureDescription passthrough_right_eye;

    GLuint passthroughDepthRenderBuffer = 0;
    GLuint passthroughFramebuffer = 0;

    float passthrough_vertices[8];
    float passthrough_size = 1.0;

    float eyeOffsets[2] = {};
};

NativeContext CTX = {};

int64_t GetBootTimeNano() {
    struct timespec res = {};
    clock_gettime(CLOCK_BOOTTIME, &res);
    return (res.tv_sec * 1e9) + res.tv_nsec;
}

// Inverse unit quaternion
AlvrQuat inverseQuat(AlvrQuat q) { return {-q.x, -q.y, -q.z, q.w}; }

void cross(float a[3], float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void quatVecMultiply(AlvrQuat q, float v[3], float out[3]) {
    float rv[3], rrv[3];
    float r[3] = {q.x, q.y, q.z};
    cross(r, v, rv);
    cross(r, rv, rrv);
    for (int i = 0; i < 3; i++) {
        out[i] = v[i] + 2 * (q.w * rv[i] + rrv[i]);
    }
}

AlvrFov getFov(CardboardEye eye) {
    float f[4];
    CardboardLensDistortion_getFieldOfView(CTX.lensDistortion, eye, f);

    AlvrFov fov = {};
    fov.left = -f[0];
    fov.right = f[1];
    fov.up = f[3];
    fov.down = -f[2];

    return fov;
}

Pose getPose(uint64_t timestampNs) {
    Pose pose = {};

    float pos[3];
    float q[4];
    CardboardHeadTracker_getPose(CTX.headTracker, (int64_t) timestampNs, kLandscapeLeft, pos, q);

    auto inverseOrientation = AlvrQuat{q[0], q[1], q[2], q[3]};
    pose.orientation = inverseQuat(inverseOrientation);

    // FIXME: The position is calculated wrong. It behaves correctly when leaning side to side but
    // the overall position is wrong when facing left, right or back. float positionBig[3] = {pos[0]
    // * 5, pos[1] * 5, pos[2] * 5}; float headPos[3]; quatVecMultiply(pose.orientation,
    // positionBig, headPos);

    pose.position[0] = 0;   //-headPos[0];
    pose.position[1] = /*-headPos[1]*/ +FLOOR_HEIGHT;
    pose.position[2] = 0;   //-headPos[2];

    debug("returning pos (%f,%f,%f) orient (%f, %f, %f, %f)",
          pos[0],
          pos[1],
          pos[2],
          q[0],
          q[1],
          q[2],
          q[3]);
    return pose;
}

void createPassthroughPlane(NativeContext *ctx) {
    float size = ctx->passthrough_size;
    float x0 = -size, y0 = size;    // Top left
    float x1 = size, y1 = size;     // Top right
    float x2 = size, y2 = -size;    // Bottom right
    float x3 = -size, y3 = -size;   // Bottom left

    ctx->passthrough_vertices[0] = x3;
    ctx->passthrough_vertices[1] = y3;
    ctx->passthrough_vertices[2] = x2;
    ctx->passthrough_vertices[3] = y2;
    ctx->passthrough_vertices[4] = x0;
    ctx->passthrough_vertices[5] = y0;
    ctx->passthrough_vertices[6] = x1;
    ctx->passthrough_vertices[7] = y1;
}

void inputThread() {
    auto deadline = std::chrono::steady_clock::now();

    info("inputThread: thread staring...");
    while (CTX.streaming) {
        debug("inputThread: streaming...");
        uint64_t targetTimestampNs = GetBootTimeNano() + alvr_get_head_prediction_offset_ns();

        Pose headPose = getPose(targetTimestampNs);

        AlvrDeviceMotion headMotion = {};
        headMotion.device_id = HEAD_ID;
        headMotion.position[0] = headPose.position[0];
        headMotion.position[1] = headPose.position[1];
        headMotion.position[2] = headPose.position[2];
        headMotion.orientation = headPose.orientation;

        alvr_send_tracking(targetTimestampNs, &headMotion, 1 /* , nullptr, nullptr */);

        deadline += std::chrono::nanoseconds((uint64_t) (1e9 / 60.f / 3));
        std::this_thread::sleep_until(deadline);
    }
}

// extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
//     CTX.javaVm = vm;
//     return JNI_VERSION_1_6;
// }

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_initializeNative(
    JNIEnv *env, jobject obj, jint screenWidth, jint screenHeight) {
    CTX.javaVm = jVM;
    CTX.javaContext = env->NewGlobalRef(obj);

    uint32_t viewWidth = std::max(screenWidth, screenHeight) / 2;
    uint32_t viewHeight = std::min(screenWidth, screenHeight);

    float refreshRatesBuffer[1] = {60.f};

    alvr_initialize((void *) CTX.javaVm,
                    (void *) CTX.javaContext,
                    viewWidth,
                    viewHeight,
                    refreshRatesBuffer,
                    1,
                    false);

    Cardboard_initializeAndroid(CTX.javaVm, CTX.javaContext);
    CTX.headTracker = CardboardHeadTracker_create();
    createPassthroughPlane(&CTX);
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_destroyNative(JNIEnv *,
                                                                                        jobject) {
    alvr_destroy_opengl();
    alvr_destroy();

    CardboardHeadTracker_destroy(CTX.headTracker);
    CTX.headTracker = nullptr;
    CardboardLensDistortion_destroy(CTX.lensDistortion);
    CTX.lensDistortion = nullptr;
    CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
    CTX.distortionRenderer = nullptr;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_resumeNative(JNIEnv *,
                                                                                       jobject) {
    CardboardHeadTracker_resume(CTX.headTracker);

    CTX.renderingParamsChanged = true;

    uint8_t *buffer;
    int size;
    CardboardQrCode_getSavedDeviceParams(&buffer, &size);
    if (size == 0) {
        CardboardQrCode_scanQrCodeAndSaveDeviceParams();
    }
    CardboardQrCode_destroy(buffer);

    CTX.running = true;
    alvr_resume();
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_pauseNative(JNIEnv *,
                                                                                      jobject) {
    alvr_pause();

    if (CTX.running) {
        CTX.running = false;
    }

    CardboardHeadTracker_pause(CTX.headTracker);
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_setPassthroughActiveNative(JNIEnv *,
                                                                   jobject,
                                                                   jboolean activate) {
    CTX.passthrough = activate;
    CTX.renderingParamsChanged = true;
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_setPassthroughSizeNative(JNIEnv *, jobject, jfloat size) {
    CTX.passthrough_size = size;
    createPassthroughPlane(&CTX);
}

extern "C" JNIEXPORT jint JNICALL
Java_viritualisres_phonevr_ALVRActivity_surfaceCreatedNative(JNIEnv *, jobject) {
    alvr_initialize_opengl();

    const int obj_vertex_shader = LoadGLShader(GL_VERTEX_SHADER, camVertexShader);
    const int obj_fragment_shader = LoadGLShader(GL_FRAGMENT_SHADER, camFragmentShader);

    passthrough_program_ = glCreateProgram();
    glAttachShader(passthrough_program_, obj_vertex_shader);
    glAttachShader(passthrough_program_, obj_fragment_shader);
    glLinkProgram(passthrough_program_);

    glUseProgram(passthrough_program_);
    texture_position_param_ = glGetAttribLocation(passthrough_program_, "a_Position");
    texture_uv_param_ = glGetAttribLocation(passthrough_program_, "a_UV");
    texture_mvp_param_ = glGetUniformLocation(passthrough_program_, "u_MVP");

    // TODO initialize plane mesh and texture
    glGenTextures(1, &CTX.cameraTexture);
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, CTX.cameraTexture);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    CTX.glContextRecreated = true;
    return CTX.cameraTexture;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_setScreenResolutionNative(
    JNIEnv *, jobject, jint width, jint height) {
    CTX.screenWidth = width;
    CTX.screenHeight = height;

    CTX.renderingParamsChanged = true;
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_sendBatteryLevel(
    JNIEnv *, jobject, jfloat level, jboolean plugged) {
    alvr_send_battery(HEAD_ID, level, plugged);
}

void cleanupPassthrough() {
    if (CTX.passthroughDepthRenderBuffer != 0) {
        glDeleteRenderbuffers(1, &CTX.passthroughDepthRenderBuffer);
        CTX.passthroughDepthRenderBuffer = 0;
    }
    if (CTX.passthroughFramebuffer != 0) {
        glDeleteFramebuffers(1, &CTX.passthroughFramebuffer);
        CTX.passthroughFramebuffer = 0;
    }
    if (CTX.passthroughTexture != 0) {
        glDeleteTextures(1, &CTX.passthroughTexture);
        CTX.passthroughTexture = 0;
    }
}

void passthroughSetup() {
    // Create render texture.
    glGenTextures(1, &CTX.passthroughTexture);
    glBindTexture(GL_TEXTURE_2D, CTX.passthroughTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB,
                 CTX.screenWidth,
                 CTX.screenHeight,
                 0,
                 GL_RGB,
                 GL_UNSIGNED_BYTE,
                 0);

    // Generate depth buffer to perform depth test.
    glGenRenderbuffers(1, &CTX.passthroughDepthRenderBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, CTX.passthroughDepthRenderBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, CTX.screenWidth, CTX.screenHeight);

    // Create render target.
    glGenFramebuffers(1, &CTX.passthroughFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, CTX.passthroughFramebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, CTX.passthroughTexture, 0);
    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, CTX.passthroughDepthRenderBuffer);
}

extern "C" JNIEXPORT void JNICALL Java_viritualisres_phonevr_ALVRActivity_renderNative(JNIEnv *,
                                                                                       jobject) {
    if (CTX.renderingParamsChanged) {
        info("renderingParamsChanged, processing new params");
        uint8_t *buffer;
        int size;
        CardboardQrCode_getSavedDeviceParams(&buffer, &size);

        if (size == 0) {
            return;
        }

        info("renderingParamsChanged, sending new params to alvr");
        if (CTX.lensDistortion) {
            CardboardLensDistortion_destroy(CTX.lensDistortion);
            CTX.lensDistortion = nullptr;
        }
        info("renderingParamsChanged, destroyed distortion");
        CTX.lensDistortion =
            CardboardLensDistortion_create(buffer, size, CTX.screenWidth, CTX.screenHeight);

        CardboardQrCode_destroy(buffer);
        *buffer = 0;

        // cleanupPassthrough();
        // if (CTX.passthrough){
        //     passthroughSetup();
        // }

        if (CTX.distortionRenderer) {
            CardboardDistortionRenderer_destroy(CTX.distortionRenderer);
            CTX.distortionRenderer = nullptr;
        }
        CTX.distortionRenderer = CardboardOpenGlEs2DistortionRenderer_create();

        for (int eye = 0; eye < 2; eye++) {
            CardboardMesh mesh;
            CardboardLensDistortion_getDistortionMesh(
                CTX.lensDistortion, (CardboardEye) eye, &mesh);
            CardboardDistortionRenderer_setMesh(CTX.distortionRenderer, &mesh, (CardboardEye) eye);

            float matrix[16] = {};
            CardboardLensDistortion_getEyeFromHeadMatrix(
                CTX.lensDistortion, (CardboardEye) eye, matrix);
            CTX.eyeOffsets[eye] = matrix[12];
        }

        AlvrFov fovArr[2] = {getFov((CardboardEye) 0), getFov((CardboardEye) 1)};
        info("renderingParamsChanged, sending new view configs (FOV) to alvr");
        alvr_send_views_config(fovArr, CTX.eyeOffsets[0] - CTX.eyeOffsets[1]);
    }

    // Note: if GL context is recreated, old resources are already freed.
    if (CTX.renderingParamsChanged && !CTX.glContextRecreated) {
        info("Pausing ALVR since glContext is not recreated, deleting textures");
        alvr_pause_opengl();
        cleanupPassthrough();
        glDeleteTextures(2, CTX.lobbyTextures);
    }

    if (CTX.renderingParamsChanged || CTX.glContextRecreated) {
        if (CTX.passthrough) {
            passthroughSetup();
        } else {
            info("Rebuilding, binding textures, Resuming ALVR since glContextRecreated %b, "
                 "renderingParamsChanged %b",
                 CTX.renderingParamsChanged,
                 CTX.glContextRecreated);
            glGenTextures(2, CTX.lobbyTextures);

            for (auto &lobbyTexture : CTX.lobbyTextures) {
                glBindTexture(GL_TEXTURE_2D, lobbyTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D,
                             0,
                             GL_RGB,
                             CTX.screenWidth / 2,
                             CTX.screenHeight,
                             0,
                             GL_RGB,
                             GL_UNSIGNED_BYTE,
                             nullptr);
            }

            const uint32_t *targetViews[2] = {(uint32_t *) &CTX.lobbyTextures[0],
                                              (uint32_t *) &CTX.lobbyTextures[1]};
            alvr_resume_opengl(CTX.screenWidth / 2, CTX.screenHeight, targetViews, 1);
        }
        CTX.renderingParamsChanged = false;
        CTX.glContextRecreated = false;
    }

    AlvrEvent event;
    while (alvr_poll_event(&event)) {
        if (event.tag == ALVR_EVENT_HUD_MESSAGE_UPDATED) {
            auto message_length = alvr_hud_message(nullptr);
            auto message_buffer = std::vector<char>(message_length);

            alvr_hud_message(&message_buffer[0]);
            info("ALVR Poll Event: HUD Message Update - %s", &message_buffer[0]);

            if (message_length > 0)
                alvr_update_hud_message_opengl(&message_buffer[0]);
        }
        if (event.tag == ALVR_EVENT_STREAMING_STARTED) {
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, generating and binding "
                 "textures...");
            auto config = event.STREAMING_STARTED;

            glGenTextures(2, CTX.streamTextures);

            for (auto &streamTexture : CTX.streamTextures) {
                glBindTexture(GL_TEXTURE_2D, streamTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D,
                             0,
                             GL_RGB,
                             config.view_width,
                             config.view_height,
                             0,
                             GL_RGB,
                             GL_UNSIGNED_BYTE,
                             nullptr);
            }

            AlvrFov fovArr[2] = {getFov((CardboardEye) 0), getFov((CardboardEye) 1)};
            alvr_send_views_config(fovArr, CTX.eyeOffsets[0] - CTX.eyeOffsets[1]);

            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, View configs sent...");

            auto leftIntHandle = (uint32_t) CTX.streamTextures[0];
            auto rightIntHandle = (uint32_t) CTX.streamTextures[1];
            const uint32_t *textureHandles[2] = {&leftIntHandle, &rightIntHandle};

            auto render_config = AlvrStreamConfig{};
            render_config.view_resolution_width = config.view_width;
            render_config.view_resolution_height = config.view_height;
            render_config.swapchain_textures = textureHandles;
            render_config.swapchain_length = 1;
            render_config.enable_foveation = config.enable_foveation;
            render_config.foveation_center_size_x = config.foveation_center_shift_x;
            render_config.foveation_center_size_y = config.foveation_center_size_y;
            render_config.foveation_center_shift_x = config.foveation_center_shift_x;
            render_config.foveation_center_shift_y = config.foveation_center_shift_y;
            render_config.foveation_edge_ratio_x = config.foveation_edge_ratio_x;
            render_config.foveation_edge_ratio_y = config.foveation_edge_ratio_y;

            alvr_start_stream_opengl(render_config);

            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STARTED, opengl stream started and input "
                 "Thread started...");
            CTX.streaming = true;
            CTX.inputThread = std::thread(inputThread);

        } else if (event.tag == ALVR_EVENT_STREAMING_STOPPED) {
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STOPPED, Waiting for inputThread to "
                 "join...");
            CTX.streaming = false;
            CTX.inputThread.join();

            glDeleteTextures(2, CTX.streamTextures);
            info("ALVR Poll Event: ALVR_EVENT_STREAMING_STOPPED, Stream stopped deleted textures.");
        }
    }

    CardboardEyeTextureDescription viewsDescs[2] = {};
    for (auto &viewsDesc : viewsDescs) {
        viewsDesc.left_u = 0.0;
        viewsDesc.right_u = 1.0;
        viewsDesc.top_v = 1.0;
        viewsDesc.bottom_v = 0.0;
    }

    if (CTX.passthrough) {
        glBindFramebuffer(GL_FRAMEBUFFER, CTX.passthroughFramebuffer);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Draw Passthrough video for each eye
        for (int eye = 0; eye < 2; ++eye) {
            glViewport(
                eye == kLeft ? 0 : CTX.screenWidth / 2, 0, CTX.screenWidth / 2, CTX.screenHeight);

            glUseProgram(passthrough_program_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, CTX.cameraTexture);

            // Draw Mesh
            glEnableVertexAttribArray(texture_position_param_);
            glVertexAttribPointer(
                texture_position_param_, 2, GL_FLOAT, false, 0, CTX.passthrough_vertices);
            glEnableVertexAttribArray(texture_uv_param_);
            glVertexAttribPointer(texture_uv_param_, 2, GL_FLOAT, false, 0, passthrough_tex_coords);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            viewsDescs[eye].left_u = 0.5 * eye;          // 0 for left, 0.5 for right
            viewsDescs[eye].right_u = 0.5 + 0.5 * eye;   // 0.5 for left, 1.0 for right
        }
        viewsDescs[0].texture = CTX.passthroughTexture;
        viewsDescs[1].texture = CTX.passthroughTexture;
    } else if (CTX.streaming) {
        void *streamHardwareBuffer = nullptr;
        auto timestampNs = alvr_get_frame(&streamHardwareBuffer);

        if (timestampNs == -1) {
            return;
        }

        uint32_t swapchainIndices[2] = {0, 0};
        alvr_render_stream_opengl(streamHardwareBuffer, swapchainIndices);

        alvr_report_submit(timestampNs, 0);

        viewsDescs[0].texture = CTX.streamTextures[0];
        viewsDescs[1].texture = CTX.streamTextures[1];
    } else {
        Pose pose = getPose(GetBootTimeNano() + VSYNC_QUEUE_INTERVAL_NS);

        AlvrViewInput viewInputs[2] = {};
        for (int eye = 0; eye < 2; eye++) {
            float headToEye[3] = {CTX.eyeOffsets[eye], 0.0, 0.0};
            float rotatedHeadToEye[3];
            quatVecMultiply(pose.orientation, headToEye, rotatedHeadToEye);

            viewInputs[eye].orientation = pose.orientation;
            viewInputs[eye].position[0] = pose.position[0] - rotatedHeadToEye[0];
            viewInputs[eye].position[1] = pose.position[1] - rotatedHeadToEye[1];
            viewInputs[eye].position[2] = pose.position[2] - rotatedHeadToEye[2];
            viewInputs[eye].fov = getFov((CardboardEye) eye);
            viewInputs[eye].swapchain_index = 0;
        }
        alvr_render_lobby_opengl(viewInputs);

        viewsDescs[0].texture = CTX.lobbyTextures[0];
        viewsDescs[1].texture = CTX.lobbyTextures[1];
    }

    // Note: the Cardboard SDK does not support reprojection!
    // todo: manually implement it?

    // info("nativeRendered: Rendering to Display...");
    CardboardDistortionRenderer_renderEyeToDisplay(CTX.distortionRenderer,
                                                   0,
                                                   0,
                                                   0,
                                                   CTX.screenWidth,
                                                   CTX.screenHeight,
                                                   &viewsDescs[0],
                                                   &viewsDescs[1]);
}

extern "C" JNIEXPORT void JNICALL
Java_viritualisres_phonevr_ALVRActivity_switchViewerNative(JNIEnv *, jobject) {
    CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}
