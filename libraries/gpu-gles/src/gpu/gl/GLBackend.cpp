//
//  GLBackend.cpp
//  libraries/gpu-gl-android/src/gpu/gl
//
//  Created by Cristian Duarte & Gabriel Calero on 9/21/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "GLBackend.h"

#include <mutex>
#include <queue>
#include <list>
#include <functional>
#include <glm/gtc/type_ptr.hpp>

#include "../gles/GLESBackend.h"

#if defined(NSIGHT_FOUND)
#include "nvToolsExt.h"
#endif

#include <GPUIdent.h>
#include <gl/QOpenGLContextWrapper.h>
#include <QtCore/QProcessEnvironment>

#include "GLTexture.h"
#include "GLShader.h"
using namespace gpu;
using namespace gpu::gl;

static GLBackend* INSTANCE{ nullptr };
static const char* GL_BACKEND_PROPERTY_NAME = "com.highfidelity.gl.backend";

BackendPointer GLBackend::createBackend() {
    // FIXME provide a mechanism to override the backend for testing
    // Where the gpuContext is initialized and where the TRUE Backend is created and assigned
    auto version = QOpenGLContextWrapper::currentContextVersion();
    std::shared_ptr<GLBackend> result;
    
    qDebug() << "Using OpenGL ES backend";
    result = std::make_shared<gpu::gles::GLESBackend>();

    result->initInput();
    result->initTransform();

    INSTANCE = result.get();
    void* voidInstance = &(*result);
    qApp->setProperty(GL_BACKEND_PROPERTY_NAME, QVariant::fromValue(voidInstance));

    gl::GLTexture::initTextureTransferHelper();
    return result;
}

GLBackend& getBackend() {
    if (!INSTANCE) {
        INSTANCE = static_cast<GLBackend*>(qApp->property(GL_BACKEND_PROPERTY_NAME).value<void*>());
    }
    return *INSTANCE;
}

bool GLBackend::makeProgram(Shader& shader, const Shader::BindingSet& slotBindings) {
    return GLShader::makeProgram(getBackend(), shader, slotBindings);
}

std::array<QString, 45> commandNames = { 
        {QString("draw"),QString("drawIndexed"),QString("drawInstanced"),QString("drawIndexedInstanced"),QString("multiDrawIndirect"),QString("multiDrawIndexedIndirect"),QString("setInputFormat"),QString("setInputBuffer"),QString("setIndexBuffer"),QString("setIndirectBuffer"),QString("setModelTransform"),QString("setViewTransform"),QString("setProjectionTransform"),QString("setViewportTransform"),QString("setDepthRangeTransform"),QString("setPipeline"),QString("setStateBlendFactor"),QString("setStateScissorRect"),QString("setUniformBuffer"),QString("setResourceTexture"),QString("setFramebuffer"),QString("clearFramebuffer"),QString("blit"),QString("generateTextureMips"),QString("beginQuery"),QString("endQuery"),QString("getQuery"),QString("resetStages"),QString("runLambda"),QString("startNamedCall"),QString("stopNamedCall"),QString("glUniform1i"),QString("glUniform1f"),QString("glUniform2f"),QString("glUniform3f"),QString("glUniform4f"),QString("glUniform3fv"),QString("glUniform4fv"),QString("glUniform4iv"),QString("glUniformMatrix3fv"),QString("glUniformMatrix4fv"),QString("glColor4f"),QString("pushProfileRange"),QString("popProfileRange"),QString("NUM_COMMANDS")}
};

GLBackend::CommandCall GLBackend::_commandCalls[Batch::NUM_COMMANDS] = 
{
    (&::gpu::gl::GLBackend::do_draw), 
    (&::gpu::gl::GLBackend::do_drawIndexed),
    (&::gpu::gl::GLBackend::do_drawInstanced),
    (&::gpu::gl::GLBackend::do_drawIndexedInstanced),
    (&::gpu::gl::GLBackend::do_multiDrawIndirect),
    (&::gpu::gl::GLBackend::do_multiDrawIndexedIndirect),

    (&::gpu::gl::GLBackend::do_setInputFormat),
    (&::gpu::gl::GLBackend::do_setInputBuffer),
    (&::gpu::gl::GLBackend::do_setIndexBuffer),
    (&::gpu::gl::GLBackend::do_setIndirectBuffer),

    (&::gpu::gl::GLBackend::do_setModelTransform),
    (&::gpu::gl::GLBackend::do_setViewTransform),
    (&::gpu::gl::GLBackend::do_setProjectionTransform),
    (&::gpu::gl::GLBackend::do_setViewportTransform),
    (&::gpu::gl::GLBackend::do_setDepthRangeTransform),

    (&::gpu::gl::GLBackend::do_setPipeline),
    (&::gpu::gl::GLBackend::do_setStateBlendFactor),
    (&::gpu::gl::GLBackend::do_setStateScissorRect),

    (&::gpu::gl::GLBackend::do_setUniformBuffer),
    (&::gpu::gl::GLBackend::do_setResourceTexture),

    (&::gpu::gl::GLBackend::do_setFramebuffer),
    (&::gpu::gl::GLBackend::do_clearFramebuffer),
    (&::gpu::gl::GLBackend::do_blit),
    (&::gpu::gl::GLBackend::do_generateTextureMips),

    (&::gpu::gl::GLBackend::do_beginQuery),
    (&::gpu::gl::GLBackend::do_endQuery),
    (&::gpu::gl::GLBackend::do_getQuery),

    (&::gpu::gl::GLBackend::do_resetStages),

    (&::gpu::gl::GLBackend::do_runLambda),

    (&::gpu::gl::GLBackend::do_startNamedCall),
    (&::gpu::gl::GLBackend::do_stopNamedCall),

    (&::gpu::gl::GLBackend::do_glUniform1i),
    (&::gpu::gl::GLBackend::do_glUniform1f),
    (&::gpu::gl::GLBackend::do_glUniform2f),
    (&::gpu::gl::GLBackend::do_glUniform3f),
    (&::gpu::gl::GLBackend::do_glUniform4f),
    (&::gpu::gl::GLBackend::do_glUniform3fv),
    (&::gpu::gl::GLBackend::do_glUniform4fv),
    (&::gpu::gl::GLBackend::do_glUniform4iv),
    (&::gpu::gl::GLBackend::do_glUniformMatrix3fv),
    (&::gpu::gl::GLBackend::do_glUniformMatrix4fv),

    (&::gpu::gl::GLBackend::do_glColor4f),

    (&::gpu::gl::GLBackend::do_pushProfileRange),
    (&::gpu::gl::GLBackend::do_popProfileRange),
};

void GLBackend::init() {
    static std::once_flag once;
    std::call_once(once, [] {
        QString vendor{ (const char*)glGetString(GL_VENDOR) };
        QString renderer{ (const char*)glGetString(GL_RENDERER) };
        qCDebug(gpugllogging) << "GL Version: " << QString((const char*) glGetString(GL_VERSION));
        qCDebug(gpugllogging) << "GL Shader Language Version: " << QString((const char*) glGetString(GL_SHADING_LANGUAGE_VERSION));
        qCDebug(gpugllogging) << "GL Vendor: " << vendor;
        qCDebug(gpugllogging) << "GL Renderer: " << renderer;
        GPUIdent* gpu = GPUIdent::getInstance(vendor, renderer); 
        // From here on, GPUIdent::getInstance()->getMumble() should efficiently give the same answers.
        qCDebug(gpugllogging) << "GPU:";
        qCDebug(gpugllogging) << "\tcard:" << gpu->getName();
        qCDebug(gpugllogging) << "\tdriver:" << gpu->getDriver();
        qCDebug(gpugllogging) << "\tdedicated memory:" << gpu->getMemory() << "MB";

        /*glewExperimental = true;
        GLenum err = glewInit();
        glGetError(); // clear the potential error from glewExperimental
        if (GLEW_OK != err) {
            // glewInit failed, something is seriously wrong.
            qCDebug(gpugllogging, "Error: %s\n", glewGetErrorString(err));
        }
        qCDebug(gpugllogging, "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION));
        */

    });
}

GLBackend::GLBackend() {
    _pipeline._cameraCorrectionBuffer._buffer->flush();
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &_uboAlignment);
}


GLBackend::~GLBackend() {
    resetStages();

    killInput();
    killTransform();
}

void GLBackend::renderPassTransfer(const Batch& batch) {
    const size_t numCommands = batch.getCommands().size();
    const Batch::Commands::value_type* command = batch.getCommands().data();
    const Batch::CommandOffsets::value_type* offset = batch.getCommandOffsets().data();

    _inRenderTransferPass = true;
    { // Sync all the buffers
        ANDROID_PROFILE(render, "syncGPUBuffer", 0xffaaffaa, 1)

        for (auto& cached : batch._buffers._items) {
            if (cached._data) {
                syncGPUObject(*cached._data);
            }
        }
    }

    { // Sync all the buffers
        ANDROID_PROFILE(render, "syncCPUTransform", 0xffaaaaff, 1)
        _transform._cameras.clear();
        _transform._cameraOffsets.clear();

        for (_commandIndex = 0; _commandIndex < numCommands; ++_commandIndex) {
            switch (*command) {
                case Batch::COMMAND_draw:
                case Batch::COMMAND_drawIndexed:
                case Batch::COMMAND_drawInstanced:
                case Batch::COMMAND_drawIndexedInstanced:
                case Batch::COMMAND_multiDrawIndirect:
                case Batch::COMMAND_multiDrawIndexedIndirect:
                    _transform.preUpdate(_commandIndex, _stereo);
                    break;

                case Batch::COMMAND_setViewportTransform:
                case Batch::COMMAND_setViewTransform:
                case Batch::COMMAND_setProjectionTransform: {
                    ANDROID_PROFILE_COMMAND(render, (int)(*command), 0xffeeaaff, 1)
                    CommandCall call = _commandCalls[(*command)];
                    (this->*(call))(batch, *offset);
                    break;
                }

                default:
                    break;
            }
            command++;
            offset++;
        }
    }

    { // Sync the transform buffers
        //PROFILE_RANGE(render_gpu_gl, "transferTransformState");
        ANDROID_PROFILE(render, "transferTransformState", 0xff0000ff, 1)
        transferTransformState(batch);
    }

    _inRenderTransferPass = false;
}

void GLBackend::renderPassDraw(const Batch& batch) {
    _currentDraw = -1;
    _transform._camerasItr = _transform._cameraOffsets.begin();
    const size_t numCommands = batch.getCommands().size();
    const Batch::Commands::value_type* command = batch.getCommands().data();
    const Batch::CommandOffsets::value_type* offset = batch.getCommandOffsets().data();
    for (_commandIndex = 0; _commandIndex < numCommands; ++_commandIndex) {
        switch (*command) {
            // Ignore these commands on this pass, taken care of in the transfer pass
            // Note we allow COMMAND_setViewportTransform to occur in both passes
            // as it both updates the transform object (and thus the uniforms in the 
            // UBO) as well as executes the actual viewport call
            case Batch::COMMAND_setModelTransform:
            case Batch::COMMAND_setViewTransform:
            case Batch::COMMAND_setProjectionTransform:
                break;

            case Batch::COMMAND_draw:
            case Batch::COMMAND_drawIndexed:
            case Batch::COMMAND_drawInstanced:
            case Batch::COMMAND_drawIndexedInstanced:
            case Batch::COMMAND_multiDrawIndirect:
            case Batch::COMMAND_multiDrawIndexedIndirect: {
                // updates for draw calls
                ++_currentDraw;
                updateInput();
                updateTransform(batch);
                updatePipeline();
                {ANDROID_PROFILE_COMMAND(render, (int)(*command), 0xff0000ff, 1)
                CommandCall call = _commandCalls[(*command)];
                (this->*(call))(batch, *offset);
                }
                break;
            }
            default: {
                ANDROID_PROFILE_COMMAND(render, (int)(*command), 0xffff00ff, 1)
                CommandCall call = _commandCalls[(*command)];
                (this->*(call))(batch, *offset);
                break;
            }
        }

        command++;
        offset++;
    }
}

void GLBackend::render(const Batch& batch) {
    ANDROID_PROFILE(render, "GLBackendRender", 0xffff00ff, 1)
    _transform._skybox = _stereo._skybox = batch.isSkyboxEnabled();
    // Allow the batch to override the rendering stereo settings
    // for things like full framebuffer copy operations (deferred lighting passes)
    bool savedStereo = _stereo._enable;
    if (!batch.isStereoEnabled()) {
        _stereo._enable = false;
    }
    
    {
        //PROFILE_RANGE(render_gpu_gl, "Transfer");
        ANDROID_PROFILE(render, "Transfer", 0xff0000ff, 1)
        renderPassTransfer(batch);
    }

    {
        //PROFILE_RANGE(render_gpu_gl, _stereo._enable ? "Render Stereo" : "Render");
        ANDROID_PROFILE(render, "RenderPassDraw", 0xff00ddff, 1)
        renderPassDraw(batch);
    }

    // Restore the saved stereo state for the next batch
    _stereo._enable = savedStereo;
}


void GLBackend::syncCache() {
    syncTransformStateCache();
    syncPipelineStateCache();
    syncInputStateCache();
    syncOutputStateCache();

    //glEnable(GL_LINE_SMOOTH);
    qDebug() << "TODO: GLBackend.cpp:syncCache GL_LINE_SMOOTH";
}

void GLBackend::setupStereoSide(int side) {
    ivec4 vp = _transform._viewport;
    vp.z /= 2;
    glViewport(vp.x + side * vp.z, vp.y, vp.z, vp.w);

#ifdef GPU_STEREO_CAMERA_BUFFER
#ifdef GPU_STEREO_DRAWCALL_DOUBLED
    //glVertexAttribI1i(14, side);
    glVertexAttribI4i(14, side, 0, 0, 0);

#endif
#else
    _transform.bindCurrentCamera(side);
#endif
}

void GLBackend::do_resetStages(const Batch& batch, size_t paramOffset) {
    resetStages();
}

void GLBackend::do_runLambda(const Batch& batch, size_t paramOffset) {
    std::function<void()> f = batch._lambdas.get(batch._params[paramOffset]._uint);
    f();
}

void GLBackend::do_startNamedCall(const Batch& batch, size_t paramOffset) {
    batch._currentNamedCall = batch._names.get(batch._params[paramOffset]._uint);
    _currentDraw = -1;
}

void GLBackend::do_stopNamedCall(const Batch& batch, size_t paramOffset) {
    batch._currentNamedCall.clear();
}

void GLBackend::resetStages() {
    resetInputStage();
    resetPipelineStage();
    resetTransformStage();
    resetUniformStage();
    resetResourceStage();
    resetOutputStage();
    resetQueryStage();

    (void) CHECK_GL_ERROR();
}


void GLBackend::do_pushProfileRange(const Batch& batch, size_t paramOffset) {
    auto name = batch._profileRanges.get(batch._params[paramOffset]._uint);
    profileRanges.push_back(name);
#if defined(NSIGHT_FOUND)
    nvtxRangePush(name.c_str());
#endif
}

void GLBackend::do_popProfileRange(const Batch& batch, size_t paramOffset) {
    profileRanges.pop_back();
#if defined(NSIGHT_FOUND)
    nvtxRangePop();
#endif
}

// TODO: As long as we have gl calls explicitely issued from interface
// code, we need to be able to record and batch these calls. THe long 
// term strategy is to get rid of any GL calls in favor of the HIFI GPU API

// As long as we don;t use several versions of shaders we can avoid this more complex code path
// #define GET_UNIFORM_LOCATION(shaderUniformLoc) _pipeline._programShader->getUniformLocation(shaderUniformLoc, isStereo());
#define GET_UNIFORM_LOCATION(shaderUniformLoc) shaderUniformLoc

void GLBackend::do_glUniform1i(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();

    glUniform1f(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 1]._int),
        batch._params[paramOffset + 0]._int);
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform1f(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();

    glUniform1f(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 1]._int),
        batch._params[paramOffset + 0]._float);
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform2f(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();
    glUniform2f(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 2]._int),
        batch._params[paramOffset + 1]._float,
        batch._params[paramOffset + 0]._float);
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform3f(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();
    glUniform3f(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 3]._int),
        batch._params[paramOffset + 2]._float,
        batch._params[paramOffset + 1]._float,
        batch._params[paramOffset + 0]._float);
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform4f(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();
    glUniform4f(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 4]._int),
        batch._params[paramOffset + 3]._float,
        batch._params[paramOffset + 2]._float,
        batch._params[paramOffset + 1]._float,
        batch._params[paramOffset + 0]._float);
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform3fv(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();
    glUniform3fv(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 2]._int),
        batch._params[paramOffset + 1]._uint,
        (const GLfloat*)batch.readData(batch._params[paramOffset + 0]._uint));

    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform4fv(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();

    GLint location = GET_UNIFORM_LOCATION(batch._params[paramOffset + 2]._int);
    GLsizei count = batch._params[paramOffset + 1]._uint;
    const GLfloat* value = (const GLfloat*)batch.readData(batch._params[paramOffset + 0]._uint);
    glUniform4fv(location, count, value);

    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniform4iv(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();
    glUniform4iv(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 2]._int),
        batch._params[paramOffset + 1]._uint,
        (const GLint*)batch.readData(batch._params[paramOffset + 0]._uint));

    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniformMatrix3fv(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();

    glUniformMatrix3fv(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 3]._int),
        batch._params[paramOffset + 2]._uint,
        batch._params[paramOffset + 1]._uint,
        (const GLfloat*)batch.readData(batch._params[paramOffset + 0]._uint));
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glUniformMatrix4fv(const Batch& batch, size_t paramOffset) {
    if (_pipeline._program == 0) {
        // We should call updatePipeline() to bind the program but we are not doing that
        // because these uniform setters are deprecated and we don;t want to create side effect
        return;
    }
    updatePipeline();

    glUniformMatrix4fv(
        GET_UNIFORM_LOCATION(batch._params[paramOffset + 3]._int),
        batch._params[paramOffset + 2]._uint,
        batch._params[paramOffset + 1]._uint,
        (const GLfloat*)batch.readData(batch._params[paramOffset + 0]._uint));
    (void)CHECK_GL_ERROR();
}

void GLBackend::do_glColor4f(const Batch& batch, size_t paramOffset) {

    glm::vec4 newColor(
        batch._params[paramOffset + 3]._float,
        batch._params[paramOffset + 2]._float,
        batch._params[paramOffset + 1]._float,
        batch._params[paramOffset + 0]._float);

    if (_input._colorAttribute != newColor) {
        _input._colorAttribute = newColor;
        glVertexAttrib4fv(gpu::Stream::COLOR, &_input._colorAttribute.r);
    }
    (void)CHECK_GL_ERROR();
}

void GLBackend::releaseBuffer(GLuint id, Size size) const {
    Lock lock(_trashMutex);
    _buffersTrash.push_back({ id, size });
}

void GLBackend::releaseExternalTexture(GLuint id, const Texture::ExternalRecycler& recycler) const {
    Lock lock(_trashMutex);
    _externalTexturesTrash.push_back({ id, recycler });
}

void GLBackend::releaseTexture(GLuint id, Size size) const {
    Lock lock(_trashMutex);
    _texturesTrash.push_back({ id, size });
}

void GLBackend::releaseFramebuffer(GLuint id) const {
    Lock lock(_trashMutex);
    _framebuffersTrash.push_back(id);
}

void GLBackend::releaseShader(GLuint id) const {
    Lock lock(_trashMutex);
    _shadersTrash.push_back(id);
}

void GLBackend::releaseProgram(GLuint id) const {
    Lock lock(_trashMutex);
    _programsTrash.push_back(id);
}

void GLBackend::releaseQuery(GLuint id) const {
    Lock lock(_trashMutex);
    _queriesTrash.push_back(id);
}

void GLBackend::queueLambda(const std::function<void()> lambda) const {
    Lock lock(_trashMutex);
    _lambdaQueue.push_back(lambda);
}

void GLBackend::recycle() const {
    {
        std::list<std::function<void()>> lamdbasTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_lambdaQueue, lamdbasTrash);
        }
        for (auto lambda : lamdbasTrash) {
            lambda();
        }
    }

    {
        std::vector<GLuint> ids;
        std::list<std::pair<GLuint, Size>> buffersTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_buffersTrash, buffersTrash);
        }
        ids.reserve(buffersTrash.size());
        for (auto pair : buffersTrash) {
            ids.push_back(pair.first);
        }
        if (!ids.empty()) {
            glDeleteBuffers((GLsizei)ids.size(), ids.data());
        }
    }

    {
        std::vector<GLuint> ids;
        std::list<GLuint> framebuffersTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_framebuffersTrash, framebuffersTrash);
        }
        ids.reserve(framebuffersTrash.size());
        for (auto id : framebuffersTrash) {
            ids.push_back(id);
        }
        if (!ids.empty()) {
            glDeleteFramebuffers((GLsizei)ids.size(), ids.data());
        }
    }

    {
        std::vector<GLuint> ids;
        std::list<std::pair<GLuint, Size>> texturesTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_texturesTrash, texturesTrash);
        }
        ids.reserve(texturesTrash.size());
        for (auto pair : texturesTrash) {
            ids.push_back(pair.first);
        }
        if (!ids.empty()) {
            glDeleteTextures((GLsizei)ids.size(), ids.data());
        }
    }

    {
        std::list<std::pair<GLuint, Texture::ExternalRecycler>> externalTexturesTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_externalTexturesTrash, externalTexturesTrash);
        }
        if (!externalTexturesTrash.empty()) {
            std::vector<GLsync> fences;  
            fences.resize(externalTexturesTrash.size());
            for (size_t i = 0; i < externalTexturesTrash.size(); ++i) {
                fences[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }
            // External texture fences will be read in another thread/context, so we need a flush
            glFlush();
            size_t index = 0;
            for (auto pair : externalTexturesTrash) {
                auto fence = fences[index++];
                pair.second(pair.first, fence);
            }
        }
    }

    {
        std::list<GLuint> programsTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_programsTrash, programsTrash);
        }
        for (auto id : programsTrash) {
            glDeleteProgram(id);
        }
    }

    {
        std::list<GLuint> shadersTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_shadersTrash, shadersTrash);
        }
        for (auto id : shadersTrash) {
            glDeleteShader(id);
        }
    }

    {
        std::vector<GLuint> ids;
        std::list<GLuint> queriesTrash;
        {
            Lock lock(_trashMutex);
            std::swap(_queriesTrash, queriesTrash);
        }
        ids.reserve(queriesTrash.size());
        for (auto id : queriesTrash) {
            ids.push_back(id);
        }
        if (!ids.empty()) {
            glDeleteQueries((GLsizei)ids.size(), ids.data());
        }
    }

#ifndef THREADED_TEXTURE_TRANSFER
    gl::GLTexture::_textureTransferHelper->process();
#endif
}

void GLBackend::setCameraCorrection(const Mat4& correction) {
    _transform._correction.correction = correction;
    _transform._correction.correctionInverse = glm::inverse(correction);
    _pipeline._cameraCorrectionBuffer._buffer->setSubData(0, _transform._correction);
    _pipeline._cameraCorrectionBuffer._buffer->flush();
}
