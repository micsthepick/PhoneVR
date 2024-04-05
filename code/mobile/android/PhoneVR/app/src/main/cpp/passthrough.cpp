#include "alvr_client_core.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <vector>

#include "passthrough.h"

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

    static int passthroughProgram_ = 0;
    static int texturePositionParam_ = 0;
    static int textureUvParam_ = 0;
    static int textureMvpParam_ = 0;

    float passthroughTexCoords[] = {0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0};
}   // namespace

void passthrough_createPlane(PassthroughInfo *info) {
    float size = info->passthroughSize;
    float x0 = -size, y0 = size;    // Top left
    float x1 = size, y1 = size;     // Top right
    float x2 = size, y2 = -size;    // Bottom right
    float x3 = -size, y3 = -size;   // Bottom left

    info->passthroughVertices[0] = x3;
    info->passthroughVertices[1] = y3;
    info->passthroughVertices[2] = x2;
    info->passthroughVertices[3] = y2;
    info->passthroughVertices[4] = x0;
    info->passthroughVertices[5] = y0;
    info->passthroughVertices[6] = x1;
    info->passthroughVertices[7] = y1;
}

GLuint passthrough_init(PassthroughInfo *info) {
    const int obj_vertex_shader = LoadGLShader(GL_VERTEX_SHADER, camVertexShader);
    const int obj_fragment_shader = LoadGLShader(GL_FRAGMENT_SHADER, camFragmentShader);

    passthroughProgram_ = glCreateProgram();
    glAttachShader(passthroughProgram_, obj_vertex_shader);
    glAttachShader(passthroughProgram_, obj_fragment_shader);
    glLinkProgram(passthroughProgram_);

    glUseProgram(passthroughProgram_);
    texturePositionParam_ = glGetAttribLocation(passthroughProgram_, "a_Position");
    textureUvParam_ = glGetAttribLocation(passthroughProgram_, "a_UV");
    textureMvpParam_ = glGetUniformLocation(passthroughProgram_, "u_MVP");

    // TODO initialize plane mesh and texture
    glGenTextures(1, &(info->cameraTexture));
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, info->cameraTexture);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return info->cameraTexture;
}

void passthrough_cleanup(PassthroughInfo *info) {
    if (info->passthroughDepthRenderBuffer != 0) {
        glDeleteRenderbuffers(1, &(info->passthroughDepthRenderBuffer));
        info->passthroughDepthRenderBuffer = 0;
    }
    if (info->passthroughFramebuffer != 0) {
        glDeleteFramebuffers(1, &info->passthroughFramebuffer);
        info->passthroughFramebuffer = 0;
    }
    if (info->passthroughTexture != 0) {
        glDeleteTextures(1, &(info->passthroughTexture));
        info->passthroughTexture = 0;
    }
}

void passthrough_setup(PassthroughInfo *info) {
    // Create render texture.
    glGenTextures(1, &(info->passthroughTexture));
    glBindTexture(GL_TEXTURE_2D, info->passthroughTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB,
                 *(info->screenWidth),
                 *(info->screenHeight),
                 0,
                 GL_RGB,
                 GL_UNSIGNED_BYTE,
                 0);

    // Generate depth buffer to perform depth test.
    glGenRenderbuffers(1, &(info->passthroughDepthRenderBuffer));
    glBindRenderbuffer(GL_RENDERBUFFER, info->passthroughDepthRenderBuffer);
    glRenderbufferStorage(
        GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, *(info->screenWidth), *(info->screenHeight));

    // Create render target.
    glGenFramebuffers(1, &(info->passthroughFramebuffer));
    glBindFramebuffer(GL_FRAMEBUFFER, info->passthroughFramebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, info->passthroughTexture, 0);
    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, info->passthroughDepthRenderBuffer);
}

void passthrough_render(PassthroughInfo *info, CardboardEyeTextureDescription viewsDescs[]) {
    glBindFramebuffer(GL_FRAMEBUFFER, info->passthroughFramebuffer);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Draw Passthrough video for each eye
    for (int eye = 0; eye < 2; ++eye) {
        glViewport(eye == kLeft ? 0 : *(info->screenWidth) / 2,
                   0,
                   *(info->screenWidth) / 2,
                   *(info->screenHeight));

        glUseProgram(passthroughProgram_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, info->cameraTexture);

        // Draw Mesh
        glEnableVertexAttribArray(texturePositionParam_);
        glVertexAttribPointer(
                texturePositionParam_, 2, GL_FLOAT, false, 0, info->passthroughVertices);
        glEnableVertexAttribArray(textureUvParam_);
        glVertexAttribPointer(textureUvParam_, 2, GL_FLOAT, false, 0, passthroughTexCoords);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        viewsDescs[eye].left_u = 0.5 * eye;          // 0 for left, 0.5 for right
        viewsDescs[eye].right_u = 0.5 + 0.5 * eye;   // 0.5 for left, 1.0 for right
    }
    viewsDescs[0].texture = info->passthroughTexture;
    viewsDescs[1].texture = info->passthroughTexture;
}