/*
 * Copyright (C) 2010 The Android Open Source Project
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

#define LOG_TAG "OpenGLRenderer"

#include <GLES2/gl2.h>

#include <SkCanvas.h>

#include <utils/threads.h>

#include "TextureCache.h"
#include "Properties.h"

namespace android {
namespace uirenderer {

///////////////////////////////////////////////////////////////////////////////
// Constructors/destructor
///////////////////////////////////////////////////////////////////////////////

TextureCache::TextureCache():
        mCache(GenerationCache<SkBitmap*, Texture*>::kUnlimitedCapacity),
        mSize(0), mMaxSize(MB(DEFAULT_TEXTURE_CACHE_SIZE)) {
    char property[PROPERTY_VALUE_MAX];
    if (property_get(PROPERTY_TEXTURE_CACHE_SIZE, property, NULL) > 0) {
        LOGD("  Setting texture cache size to %sMB", property);
        setMaxSize(MB(atof(property)));
    } else {
        LOGD("  Using default texture cache size of %.2fMB", DEFAULT_TEXTURE_CACHE_SIZE);
    }

    init();
}

TextureCache::TextureCache(uint32_t maxByteSize):
        mCache(GenerationCache<SkBitmap*, Texture*>::kUnlimitedCapacity),
        mSize(0), mMaxSize(maxByteSize) {
    init();
}

TextureCache::~TextureCache() {
    Mutex::Autolock _l(mLock);
    mCache.clear();
}

void TextureCache::init() {
    mCache.setOnEntryRemovedListener(this);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
    LOGD("    Maximum texture dimension is %d pixels", mMaxTextureSize);
}

///////////////////////////////////////////////////////////////////////////////
// Size management
///////////////////////////////////////////////////////////////////////////////

uint32_t TextureCache::getSize() {
    Mutex::Autolock _l(mLock);
    return mSize;
}

uint32_t TextureCache::getMaxSize() {
    Mutex::Autolock _l(mLock);
    return mMaxSize;
}

void TextureCache::setMaxSize(uint32_t maxSize) {
    Mutex::Autolock _l(mLock);
    mMaxSize = maxSize;
    while (mSize > mMaxSize) {
        mCache.removeOldest();
    }
}

///////////////////////////////////////////////////////////////////////////////
// Callbacks
///////////////////////////////////////////////////////////////////////////////

void TextureCache::operator()(SkBitmap*& bitmap, Texture*& texture) {
    // This will be called already locked
    if (texture) {
        mSize -= texture->bitmapSize;
        TEXTURE_LOGD("TextureCache::callback: removed size, mSize = %d, %d",
                texture->bitmapSize, mSize);
        glDeleteTextures(1, &texture->id);
        delete texture;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Caching
///////////////////////////////////////////////////////////////////////////////

Texture* TextureCache::get(SkBitmap* bitmap) {
    mLock.lock();
    Texture* texture = mCache.get(bitmap);
    mLock.unlock();

    if (!texture) {
        if (bitmap->width() > mMaxTextureSize || bitmap->height() > mMaxTextureSize) {
            LOGW("Bitmap too large to be uploaded into a texture");
            return NULL;
        }

        const uint32_t size = bitmap->rowBytes() * bitmap->height();
        // Don't even try to cache a bitmap that's bigger than the cache
        if (size < mMaxSize) {
            mLock.lock();
            while (mSize + size > mMaxSize) {
                mCache.removeOldest();
            }
            mLock.unlock();
        }

        texture = new Texture;
        texture->bitmapSize = size;
        generateTexture(bitmap, texture, false);

        if (size < mMaxSize) {
            mLock.lock();
            mSize += size;
            TEXTURE_LOGD("TextureCache::get: create texture(0x%p): size, mSize = %d, %d",
                     bitmap, size, mSize);
            mCache.put(bitmap, texture);
            mLock.unlock();
        } else {
            texture->cleanup = true;
        }
    } else if (bitmap->getGenerationID() != texture->generation) {
        generateTexture(bitmap, texture, true);
    }

    return texture;
}

void TextureCache::remove(SkBitmap* bitmap) {
    Mutex::Autolock _l(mLock);
    mCache.remove(bitmap);
}

void TextureCache::clear() {
    Mutex::Autolock _l(mLock);
    mCache.clear();
    TEXTURE_LOGD("TextureCache:clear(), miSize = %d", mSize);
}

void TextureCache::generateTexture(SkBitmap* bitmap, Texture* texture, bool regenerate) {
    SkAutoLockPixels alp(*bitmap);

    if (!bitmap->readyToDraw()) {
        LOGE("Cannot generate texture from bitmap");
        return;
    }

    const bool resize = !regenerate || bitmap->width() != int(texture->width) ||
            bitmap->height() != int(texture->height);

    if (!regenerate) {
        glGenTextures(1, &texture->id);
    }

    texture->generation = bitmap->getGenerationID();
    texture->width = bitmap->width();
    texture->height = bitmap->height();

    glBindTexture(GL_TEXTURE_2D, texture->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, bitmap->bytesPerPixel());

    switch (bitmap->getConfig()) {
    case SkBitmap::kA8_Config:
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        uploadToTexture(resize, GL_ALPHA, bitmap->rowBytesAsPixels(), texture->height,
                GL_UNSIGNED_BYTE, bitmap->getPixels());
        texture->blend = true;
        break;
    case SkBitmap::kRGB_565_Config:
        uploadToTexture(resize, GL_RGB, bitmap->rowBytesAsPixels(), texture->height,
                GL_UNSIGNED_SHORT_5_6_5, bitmap->getPixels());
        texture->blend = false;
        break;
    case SkBitmap::kARGB_8888_Config:
        uploadToTexture(resize, GL_RGBA, bitmap->rowBytesAsPixels(), texture->height,
                GL_UNSIGNED_BYTE, bitmap->getPixels());
        // Do this after calling getPixels() to make sure Skia's deferred
        // decoding happened
        texture->blend = !bitmap->isOpaque();
        break;
    case SkBitmap::kIndex8_Config:
        uploadPalettedTexture(resize, bitmap, texture->width, texture->height);
        texture->blend = false;
        break;
    default:
        LOGW("Unsupported bitmap config: %d", bitmap->getConfig());
        break;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void TextureCache::uploadPalettedTexture(bool resize, SkBitmap* bitmap,
        uint32_t width, uint32_t height) {
    SkBitmap rgbaBitmap;
    rgbaBitmap.setConfig(SkBitmap::kARGB_8888_Config, width, height);
    rgbaBitmap.allocPixels();
    rgbaBitmap.eraseColor(0);

    SkCanvas canvas(rgbaBitmap);
    canvas.drawBitmap(*bitmap, 0.0f, 0.0f, NULL);

    uploadToTexture(resize, GL_RGBA, rgbaBitmap.rowBytesAsPixels(), height,
            GL_UNSIGNED_BYTE, rgbaBitmap.getPixels());
}

void TextureCache::uploadToTexture(bool resize, GLenum format, GLsizei width, GLsizei height,
        GLenum type, const GLvoid * data) {
    if (resize) {
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, data);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
    }
}

}; // namespace uirenderer
}; // namespace android
