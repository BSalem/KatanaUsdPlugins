// These files began life as part of the main USD distribution
// https://github.com/PixarAnimationStudios/USD.
// In 2019, Foundry and Pixar agreed Foundry should maintain and curate
// these plug-ins, and they moved to
// https://github.com/TheFoundryVisionmongers/KatanaUsdPlugins
// under the same Modified Apache 2.0 license, as shown below.
//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#ifndef USDKATANA_CACHE_H
#define USDKATANA_CACHE_H

#include <map>
#include <string>

#include <pxr/base/tf/singleton.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/declareHandles.h>
#include <pxr/usd/usd/stage.h>

#include <FnAttribute/FnAttribute.h>

#include "usdKatana/api.h"

PXR_NAMESPACE_OPEN_SCOPE


// Forward declare pointers.
SDF_DECLARE_HANDLES(SdfLayer);
typedef TfRefPtr<class UsdStage> UsdStageRefPtr;
class SdfPath;
class UsdPrim;

/*
 * Custom cache singleton class for katana. Hold the usd stage and renderer.
 * The stage returned by this cache helper is meant to be read only. The
 * session layer will be locked for editing.
 */
class UsdKatanaCache : public TfSingleton<UsdKatanaCache>
{
    friend class TfSingleton<UsdKatanaCache>;

    UsdKatanaCache();

    /// Construct a session layer from the groupAttr encoding of variants
    /// and deactivations -- or return a previously created one
    SdfLayerRefPtr& _FindOrCreateSessionLayer(FnAttribute::GroupAttribute sessionAttr,
                                              const std::string& rootLocation,
                                              const std::string& isolatePath = "");

    /// Mute layers by name
    static void _SetMutedLayers(
        const UsdStageRefPtr &stage, const std::string &layerRegex);

    std::string _ComputeCacheKey(FnAttribute::GroupAttribute sessionAttr,
        const std::string& rootLocation);

    std::map<std::string, SdfLayerRefPtr> _sessionKeyCache;

public:

    USDKATANA_API static UsdKatanaCache& GetInstance() {
        return TfSingleton<UsdKatanaCache>::GetInstance();
    }

    /// Clear all caches
    USDKATANA_API void Flush();

    /// Get (or create) a cached usd stage with a sessionLayer containing
    /// variant selections and activations (so far)
    USDKATANA_API UsdStageRefPtr GetStage(std::string const& fileName,
                            FnAttribute::GroupAttribute sessionAttr,
                            const std::string & sessionRootLocation,
                            const std::string & isolatePath,
                            std::string const& ignoreLayerRegex,
                            bool forcePopulate);

    // Equivalent to GetStage above but without caching
    UsdStageRefPtr GetUncachedStage(std::string const& fileName,
                            FnAttribute::GroupAttribute sessionAttr,
                            const std::string & sessionRootLocation,
                            const std::string & isolatePath,
                            std::string const& ignoreLayerRegex,
                            bool forcePopulate);

    /// Flushes an individual stage if present in the cache
    USDKATANA_API void FlushStage(const UsdStageRefPtr & stage);

    /// \brief Find a cached session layer if it exists.  Does NOT create.
    SdfLayerRefPtr FindSessionLayer(
        FnAttribute::GroupAttribute sessionAttr,
        const std::string& rootLocation);

    USDKATANA_API SdfLayerRefPtr FindSessionLayer(
        const std::string& cacheKey) ;

    USDKATANA_API SdfLayerRefPtr FindOrCreateSessionLayer(
        const std::string& sessionAttrXML,
        const std::string& rootLocation);
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // USDKATANA_CACHE_H
