//
//  Created by Gabriel Calero & Cristian Duarte on 09/27/2016
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#include "GLESBackend.h"
#include "../gl/GLBuffer.h"

namespace gpu {
    namespace gles {

        class GLESBuffer : public gpu::gl::GLBuffer {
            using Parent = gpu::gl::GLBuffer;
            static GLuint allocate() {
                GLuint result;
                glGenBuffers(1, &result);
                return result;
            }

        public:
            GLESBuffer(const std::weak_ptr<gl::GLBackend>& backend, const Buffer& buffer, GLESBuffer* original) : Parent(backend, buffer, allocate()) {
                glBindBuffer(GL_ARRAY_BUFFER, _buffer);
                glBufferData(GL_ARRAY_BUFFER, _size, nullptr, GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);

                if (original && original->_size) {
                    glBindBuffer(GL_COPY_WRITE_BUFFER, _buffer);
                    glBindBuffer(GL_COPY_READ_BUFFER, original->_buffer);
                    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, original->_size);
                    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                    glBindBuffer(GL_COPY_READ_BUFFER, 0);
                    (void)CHECK_GL_ERROR();
                }
                Backend::setGPUObject(buffer, this);
            }

            void transfer() override {
                glBindBuffer(GL_ARRAY_BUFFER, _buffer);
                (void)CHECK_GL_ERROR();
                Size offset;
                Size size;
                Size currentPage { 0 };
                auto data = _gpuObject._renderSysmem.readData();
                while (_gpuObject._renderPages.getNextTransferBlock(offset, size, currentPage)) {
                    glBufferSubData(GL_ARRAY_BUFFER, offset, size, data + offset);
                    (void)CHECK_GL_ERROR();
                }
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                (void)CHECK_GL_ERROR();
                _gpuObject._renderPages._flags &= ~PageManager::DIRTY;
            }
        };
    }
}

using namespace gpu;
using namespace gpu::gl;
using namespace gpu::gles;


GLuint GLESBackend::getBufferID(const Buffer& buffer) {
    return GLESBuffer::getId<GLESBuffer>(*this, buffer);
}

GLBuffer* GLESBackend::syncGPUObject(const Buffer& buffer) {
    return GLESBuffer::sync<GLESBuffer>(*this, buffer);
}
