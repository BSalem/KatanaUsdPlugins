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
#include "usdKatana/cache.h"

#include <set>
#include <utility>
#include <vector>

#include <pxr/pxr.h>

#include <pxr/base/arch/systemInfo.h>
#include <pxr/base/tf/instantiateSingleton.h>
#include <pxr/base/trace/trace.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stageCacheContext.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <boost/regex.hpp>

#include <pystring/pystring.h>

#include "usdKatana/debugCodes.h"
#include "usdKatana/locks.h"

PXR_NAMESPACE_OPEN_SCOPE


TF_INSTANTIATE_SINGLETON(UsdKatanaCache);


namespace
{
    template <typename fnAttrT, typename podT>
    bool AddSimpleTypedSdfAttribute(SdfPrimSpecHandle prim,
            const std::string & attrName, const fnAttrT & valueAttr, 
            const FnAttribute::IntAttribute & forceArrayAttr, 
            SdfValueTypeName scalarType)
    {
        if (!prim)
        {
            return false;
        }
        
        if (!valueAttr.isValid())
        {
            return false;
        }
        
        bool isArray;
        if (forceArrayAttr.isValid())
        {
            isArray = forceArrayAttr.getValue();
        }
        else
        {
            isArray = valueAttr.getNumberOfValues() != 1;
        }
        auto sdfAttr = SdfAttributeSpec::New(prim, attrName,
                isArray ? scalarType.GetArrayType() : scalarType);
        
        if (!sdfAttr)
        {
            return false;
        }
        
        if (isArray)
        {
            VtArray<podT> vtArray;
            
            auto sample = valueAttr.getNearestSample(0.0f);
            vtArray.assign(&sample[0],
                    &sample[0] + valueAttr.getNumberOfValues());
            
            sdfAttr->SetDefaultValue(VtValue(vtArray));
        }
        else
        {
            sdfAttr->SetDefaultValue(
                    VtValue(valueAttr.getValue(typename fnAttrT::value_type(),
                            false)));
        }
        return true;
    }
}  // namespace

SdfLayerRefPtr& UsdKatanaCache::_FindOrCreateSessionLayer(FnAttribute::GroupAttribute sessionAttr,
                                                          const std::string& rootLocation,
                                                          const std::string& isolatePath)
{
    // Grab a reader lock for reading the _sessionKeyCache
    boost::upgrade_lock<boost::upgrade_mutex>
                readerLock(UsdKatanaGetSessionCacheLock());
    
    
    std::string cacheKey = _ComputeCacheKey(sessionAttr, rootLocation);
    
    // Open the usd stage
    
    if (_sessionKeyCache.find(cacheKey) == _sessionKeyCache.end())
    {
        boost::upgrade_to_unique_lock<boost::upgrade_mutex>
                    writerLock(readerLock);
        SdfLayerRefPtr sessionLayer = SdfLayer::CreateAnonymous(".usda");
        _sessionKeyCache[cacheKey] = sessionLayer;
        
        
        std::string rootLocationPlusSlash = rootLocation + "/";
        
        
        FnAttribute::GroupAttribute variantsAttr =
                sessionAttr.getChildByName("variants");
        for (int64_t i = 0, e = variantsAttr.getNumberOfChildren(); i != e;
                ++i)
        {
            std::string entryName = FnAttribute::DelimiterDecode(
                    variantsAttr.getChildName(i));
            
            FnAttribute::GroupAttribute entryVariantSets =
                    variantsAttr.getChildByIndex(i);
            
            if (entryVariantSets.getNumberOfChildren() == 0)
            {
                continue;
            }
            
            if (!pystring::startswith(entryName, rootLocationPlusSlash))
            {
                continue;
            }

            const SdfPath varSelPath(isolatePath + pystring::slice(entryName, rootLocation.size()));
            for (int64_t i = 0, e = entryVariantSets.getNumberOfChildren();
                    i != e; ++i)
            {
                std::string variantSetName = entryVariantSets.getChildName(i);
                
                FnAttribute::StringAttribute variantValueAttr =
                        entryVariantSets.getChildByIndex(i);
                if (!variantValueAttr.isValid())
                {
                    continue;
                }

                const std::string variantSetSelection = variantValueAttr.getValue("", false);
                SdfPrimSpecHandle spec = SdfCreatePrimInLayer(
                        sessionLayer, varSelPath.GetPrimPath());
                if (spec)
                {
                    std::pair<std::string, std::string> sel = 
                            varSelPath.GetVariantSelection();
                    spec->SetVariantSelection(variantSetName,
                            variantSetSelection);
                }
            }
        }
        
        
        FnAttribute::GroupAttribute activationsAttr =
                sessionAttr.getChildByName("activations");
        for (int64_t i = 0, e = activationsAttr.getNumberOfChildren(); i != e;
                ++i)
        {
            std::string entryName = FnAttribute::DelimiterDecode(
                    activationsAttr.getChildName(i));
            
            FnAttribute::IntAttribute stateAttr =
                    activationsAttr.getChildByIndex(i);
            
            if (stateAttr.getNumberOfValues() != 1)
            {
                continue;
            }
            
            if (!pystring::startswith(entryName, rootLocationPlusSlash))
            {
                continue;
            }

            const SdfPath activationsPath(isolatePath +
                                          pystring::slice(entryName, rootLocation.size()));
            SdfPrimSpecHandle spec =
                SdfCreatePrimInLayer(sessionLayer, activationsPath.GetPrimPath());
            spec->SetActive(stateAttr.getValue());
        }
        
        FnAttribute::GroupAttribute attrsAttr =
                sessionAttr.getChildByName("attrs");
        
        for (int64_t i = 0, e = attrsAttr.getNumberOfChildren(); i != e;
                ++i)
        {
            std::string entryName = FnAttribute::DelimiterDecode(
                    attrsAttr.getChildName(i));
            
            FnAttribute::GroupAttribute entryAttr =
                    attrsAttr.getChildByIndex(i);            
            
            if (!pystring::startswith(entryName, rootLocationPlusSlash))
            {
                continue;
            }

            const SdfPath attrsPath(isolatePath + pystring::slice(entryName, rootLocation.size()));
            SdfPrimSpecHandle spec = SdfCreatePrimInLayer(sessionLayer, attrsPath.GetPrimPath());
            if (!spec)
            {
                continue;
            }
            
            for (int64_t i = 0, e = entryAttr.getNumberOfChildren(); i != e;
                ++i)
            {
                std::string attrName = entryAttr.getChildName(i);
                FnAttribute::GroupAttribute attrDef =
                        entryAttr.getChildByIndex(i);

                FnAttribute::IntAttribute forceArrayAttr = 
                    attrDef.getChildByName("forceArray");
                
                
                FnAttribute::DataAttribute valueAttr =
                        attrDef.getChildByName("value");
                if (!valueAttr.isValid())
                {
                    continue;
                }
                
                // TODO, additional SdfValueTypes, blocking, metadata
                
                switch (valueAttr.getType())
                {
                case kFnKatAttributeTypeInt:
                {
                    AddSimpleTypedSdfAttribute<
                            FnAttribute::IntAttribute, int>(
                            spec, attrName, valueAttr, forceArrayAttr,
                            SdfValueTypeNames->Int);
                    
                    break;
                }
                case kFnKatAttributeTypeFloat:
                {
                    AddSimpleTypedSdfAttribute<
                            FnAttribute::FloatAttribute, float>(
                            spec, attrName, valueAttr, forceArrayAttr,
                            SdfValueTypeNames->Float);
                    
                    break;
                }
                case kFnKatAttributeTypeDouble:
                {
                    AddSimpleTypedSdfAttribute<
                            FnAttribute::DoubleAttribute, double>(
                            spec, attrName, valueAttr, forceArrayAttr,
                            SdfValueTypeNames->Double);
                    break;
                }
                case kFnKatAttributeTypeString:
                {
                    AddSimpleTypedSdfAttribute<
                            FnAttribute::StringAttribute, std::string>(
                            spec, attrName, valueAttr, forceArrayAttr,
                            SdfValueTypeNames->String);
                    
                    break;
                }
                default:
                    break;
                };
            }
        }
        


        FnAttribute::GroupAttribute metadataAttr =
                sessionAttr.getChildByName("metadata");
        for (int64_t i = 0, e = metadataAttr.getNumberOfChildren(); i != e;
                ++i)
        {            
            std::string entryName = FnAttribute::DelimiterDecode(
                    metadataAttr.getChildName(i));
            
            FnAttribute::GroupAttribute entryAttr =
                    metadataAttr.getChildByIndex(i);            
            
            if (!pystring::startswith(entryName, rootLocationPlusSlash))
            {
                continue;
            }

            const SdfPath metadataPath(isolatePath +
                                       pystring::slice(entryName, rootLocation.size()));
            SdfPrimSpecHandle spec = SdfCreatePrimInLayer(sessionLayer, metadataPath.GetPrimPath());
            if (!spec)
            {
                continue;
            }
            
            
            // Currently support only metadata at the prim level
            FnAttribute::GroupAttribute primEntries =
                    entryAttr.getChildByName("prim");
            for (int64_t i = 0, e = primEntries.getNumberOfChildren(); i < e; ++i)
            {
                FnAttribute::GroupAttribute attrDefGrp =
                        primEntries.getChildByIndex(i);
                std::string attrName = primEntries.getChildName(i);
                
                std::string typeName  = FnAttribute::StringAttribute(
                        attrDefGrp.getChildByName("type")).getValue("", false);
                if (typeName == "SdfInt64ListOp")
                {
                    FnAttribute::IntAttribute valueAttr;
                    
                    SdfInt64ListOp listOp;
                    std::vector<int64_t> itemList;
                    
                    auto convertFnc = [](
                            FnAttribute::IntAttribute intAttr,
                            std::vector<int64_t> & outputItemList)
                    {
                        outputItemList.clear();
                        if (intAttr.getNumberOfValues() == 0)
                        {
                            return;
                        }
                        
                        auto sample = intAttr.getNearestSample(0);
                        outputItemList.reserve(sample.size());
                        outputItemList.insert(outputItemList.end(),
                                sample.begin(), sample.end());
                    };
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.explicit");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetExplicitItems(itemList);
                    }
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.added");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetAddedItems(itemList);
                    }
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.deleted");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetDeletedItems(itemList);
                    }
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.ordered");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetOrderedItems(itemList);
                    }
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.prepended");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetPrependedItems(itemList);
                    }
                    
                    valueAttr = attrDefGrp.getChildByName("listOp.appended");
                    if (valueAttr.isValid())
                    {
                        convertFnc(valueAttr, itemList);
                        listOp.SetAppendedItems(itemList);
                    }
                    
                    spec->SetInfo(TfToken(attrName), VtValue(listOp));
                }
            }
        }

        FnAttribute::StringAttribute dynamicSublayersAttr =
                sessionAttr.getChildByName("subLayers");

        if (dynamicSublayersAttr.getNumberOfValues() > 0)
        {
            FnAttribute::StringAttribute::array_type dynamicSublayers =
                dynamicSublayersAttr.getNearestSample(0);
            if (dynamicSublayersAttr.getTupleSize() != 2 || dynamicSublayers.size() % 2 != 0)
            {
                TF_CODING_ERROR("sublayers must contain a list of two-tuples [(rootLocation, sublayerIdentifier)]");
            }

            std::set<std::string> subLayersSet;
            std::vector<std::string> subLayers;
            for (size_t i = 0; i < dynamicSublayers.size(); i += 2)
            {
                std::string sublayerRootLocation = dynamicSublayers[i];
                if (sublayerRootLocation == rootLocation && strlen(dynamicSublayers[i + 1]) > 0)
                {
                    if (subLayersSet.find(dynamicSublayers[i + 1]) == subLayersSet.end())
                    {
                        subLayers.push_back(dynamicSublayers[i+1]);
                        subLayersSet.insert(dynamicSublayers[i+1]);
                    }
                    else
                    {
                        TF_CODING_ERROR("Cannot add same sublayer twice.");
                    }
                }
            }
            sessionLayer->SetSubLayerPaths(subLayers);
        }
    }
    
    return _sessionKeyCache[cacheKey];
}

/* static */
void
UsdKatanaCache::_SetMutedLayers(
    const UsdStageRefPtr &stage, const std::string &layerRegex) 
{
    // Trace this function to track its performance
    TRACE_FUNCTION();

    // Unmute layers that are currently muted, but not requested to be muted
    SdfLayerHandleVector stageLayers = stage->GetUsedLayers();

    bool regexIsEmpty = layerRegex == "" || layerRegex == "^$";
    
    boost::regex regex(layerRegex);

    TF_FOR_ALL(stageLayer, stageLayers)
    {
        SdfLayerHandle layer = *stageLayer;
        if (!layer) {
            continue;
        }
        std::string layerPath = layer->GetRepositoryPath();
        const std::string layerIdentifier = layer->GetIdentifier();

        bool match = false;
        
        if (!regexIsEmpty)
        {
            if (boost::regex_match(layerIdentifier, regex))
            {
                match = true;
            }
        }
        
        if (!match && stage->IsLayerMuted(layerIdentifier)) {
            TF_DEBUG(USDKATANA_CACHE_RENDERER).Msg("{USD RENDER CACHE} "
                                "Unmuting Layer: '%s'\n",
                                layerIdentifier.c_str());
            stage->UnmuteLayer(layerIdentifier);
        }

        if (match && !stage->IsLayerMuted(layerIdentifier)) {
            TF_DEBUG(USDKATANA_CACHE_RENDERER).Msg("{USD RENDER CACHE} "
                    "Muting Layer: '%s'\n",
                    layerIdentifier.c_str());
            stage->MuteLayer(layerIdentifier);
        }
    }
}

UsdKatanaCache::UsdKatanaCache() 
{
}

void
UsdKatanaCache::Flush()
{
    // Flushing is writing, grab writer locks for the caches.
    boost::unique_lock<boost::upgrade_mutex>
                rendererWriterLock(UsdKatanaGetRendererCacheLock());
    boost::unique_lock<boost::upgrade_mutex>
                sessionWriterLock(UsdKatanaGetSessionCacheLock());

    UsdUtilsStageCache::Get().Clear();
    _sessionKeyCache.clear();
}


static std::string
_ResolvePath(const std::string& path)
{
    return ArGetResolver().Resolve(path);
}

// UsdStage::OpenMasked doesn't participate with the active UsdStageCache.
// Use of a UsdStageCacheRequest subclass lets work with the cache for masked
// stages without having to manually lock.
// 
// The assumption is that external consumers of the UsdStageCache which don't
// go through UsdKatanaCache will take the first otherwise matching stage
// independent of masking or session layer. Those consumers are not typically
// active in the katanaBin process but may be in the render. While the
// interactive process can result in multiple session or mask specific copies
// of the same stage (via interactive edits), that's not likely to be relevant
// to the renderboot process.
//
// NOTE: This does not own the reference to the provided mask so its lifetime
//       must be externally managed.
//
//       Additionally, UsdStagePopulationMask::All() should be sent in for an
//       empty mask. That's only relevant internal to this file as this class
//       is not exposed.
class UsdIn_StageOpenRequest : public UsdStageCacheRequest
{
public:
    UsdIn_StageOpenRequest(UsdStage::InitialLoadSet load,
                           SdfLayerHandle const& rootLayer,
                           SdfLayerHandle const& sessionLayer,
                           ArResolverContext const& pathResolverContext,
                           const UsdStagePopulationMask& mask)
        : _rootLayer(rootLayer),
          _sessionLayer(sessionLayer),
          _pathResolverContext(pathResolverContext),
          _initialLoadSet(load),
          _mask(mask)
    {}

    virtual ~UsdIn_StageOpenRequest() {}

    virtual bool IsSatisfiedBy(UsdStageRefPtr const &stage) const
    {
        // NOTE: no need to compare the mask as the session layer key
        //       already incorporates the masks value.
        return _rootLayer == stage->GetRootLayer() &&
            _sessionLayer == stage->GetSessionLayer() &&
            _pathResolverContext == stage->GetPathResolverContext();
    }
    
    virtual bool IsSatisfiedBy(UsdStageCacheRequest const &pending) const
    {
        auto req = dynamic_cast<UsdIn_StageOpenRequest const*>(&pending);
        if (!req)
        {
            return false;
        }

        return _rootLayer == req->_rootLayer && _sessionLayer == req->_sessionLayer &&
               _pathResolverContext == req->_pathResolverContext;
        // NOTE: no need to compare the mask as the session layer key
        // already incorporates the masks value.
        // _mask == req->_mask;
    }
    virtual UsdStageRefPtr Manufacture()
    {
        return UsdStage::OpenMasked(_rootLayer, _sessionLayer, 
                            _pathResolverContext,
                            _mask,
                            _initialLoadSet);
    }
    
private:
    SdfLayerHandle _rootLayer;
    SdfLayerHandle _sessionLayer;
    ArResolverContext _pathResolverContext;
    UsdStage::InitialLoadSet _initialLoadSet;
    const UsdStagePopulationMask & _mask;
};

// While the population mask is not part of the session layer, it's delivered
// along with the GroupAttribute which describes the session layer so that
// it's incorporated in the same cache key. Other uses of population masks
// may want to keep the mask mutable for a given stage, UsdIn ensures that
// they are unique copies as it's possible (although usually discouraged) to
// have simultaneous states active at once.
void FillPopulationMaskFromSessionAttr(
        FnAttribute::GroupAttribute sessionAttr,
        const std::string & sessionRootLocation,
        const std::string & isolatePath,
        UsdStagePopulationMask & mask)
{
    FnAttribute::StringAttribute maskAttr =
            sessionAttr.getChildByName("mask");
    
    if (maskAttr.getNumberOfValues() > 0)
    {
        std::string rootLocationPlusSlash = sessionRootLocation + "/";
        
        auto values = maskAttr.getNearestSample(0.0f);
        
        for (auto i : values)
        {
            if (!pystring::startswith(i, rootLocationPlusSlash))
            {
                continue;
            }
            
            std::string primPath = pystring::slice(i,
                    sessionRootLocation.size());
            // If We have an isolatepath set, we must prefix it
            if (!isolatePath.empty() && isolatePath[0] == '/')
            {
                if (primPath[0] == '/')
                {
                    primPath = isolatePath + primPath;
                } else {
                    primPath = isolatePath + "/" + primPath;
                }
            }
            
            mask.Add(SdfPath(primPath));
        }
    }
    
    if (mask.IsEmpty())
    {
        mask = UsdStagePopulationMask::All();
    }
}


UsdStageRefPtr UsdKatanaCache::GetStage(
        std::string const& fileName, 
        FnAttribute::GroupAttribute sessionAttr,
        const std::string & sessionRootLocation,
        const std::string & isolatePath,
        std::string const& ignoreLayerRegex,
        bool forcePopulate)
{
    TF_DEBUG(USDKATANA_CACHE_STAGE).Msg(
            "{USD STAGE CACHE} Creating and caching UsdStage for "
            "given filePath @%s@, which resolves to @%s@\n", 
            fileName.c_str(), _ResolvePath(fileName).c_str());

    if (SdfLayerRefPtr rootLayer = SdfLayer::FindOrOpen(fileName)) {
        SdfLayerRefPtr& sessionLayer =
            _FindOrCreateSessionLayer(sessionAttr, sessionRootLocation, isolatePath);

        UsdStageCache& stageCache = UsdUtilsStageCache::Get();

        UsdStagePopulationMask mask;
        FillPopulationMaskFromSessionAttr(
                sessionAttr, sessionRootLocation, isolatePath, mask);
        
        const UsdStage::InitialLoadSet load = 
            (forcePopulate ? UsdStage::LoadAll : UsdStage::LoadNone);

        auto result = stageCache.RequestStage(UsdIn_StageOpenRequest(
            load, rootLayer, sessionLayer, ArGetResolver().GetCurrentContext(), mask));

        UsdStageRefPtr stage = result.first;
        
        if (result.second)
        {
            TF_DEBUG(USDKATANA_CACHE_STAGE).Msg(
                    "{USD STAGE CACHE} Loaded stage "
                    "(%s, forcePopulate=%s) "
                    "with UsdStage address '%lx'\n"
                    "and sessionAttr hash '%s'\n",
                    fileName.c_str(),
                    forcePopulate?"true":"false",
                    (size_t)stage.operator->(),
                    sessionAttr.getHash().str().c_str());
        }
        else
        {
            TF_DEBUG(USDKATANA_CACHE_STAGE).Msg(
                    "{USD STAGE CACHE} Fetching cached stage "
                    "(%s, forcePopulate=%s) "
                    "with UsdStage address '%lx'\n"
                    "and sessionAttr hash '%s'\n",
                    fileName.c_str(),
                    forcePopulate?"true":"false",
                    (size_t)stage.operator->(),
                    sessionAttr.getHash().str().c_str());
        }
        
        // Mute layers according to a regex.
        _SetMutedLayers(stage, ignoreLayerRegex);

        return stage;
    }
    
    static UsdStageRefPtr NULL_STAGE;
    return NULL_STAGE;
}


UsdStageRefPtr
UsdKatanaCache::GetUncachedStage(std::string const& fileName, 
                            FnAttribute::GroupAttribute sessionAttr,
                            const std::string & sessionRootLocation,
                            const std::string & isolatePath,
                            std::string const& ignoreLayerRegex,
                            bool forcePopulate)
{
    TF_DEBUG(USDKATANA_CACHE_STAGE).Msg(
            "{USD STAGE CACHE} Creating UsdStage for "
            "given filePath @%s@, which resolves to @%s@\n", 
            fileName.c_str(), _ResolvePath(fileName).c_str());

    if (SdfLayerRefPtr rootLayer = SdfLayer::FindOrOpen(fileName)) {
        SdfLayerRefPtr& sessionLayer =
            _FindOrCreateSessionLayer(sessionAttr, sessionRootLocation, isolatePath);
        UsdStagePopulationMask mask;
        FillPopulationMaskFromSessionAttr(sessionAttr, sessionRootLocation, isolatePath, mask);

        const UsdStage::InitialLoadSet load = 
            (forcePopulate ? UsdStage::LoadAll : UsdStage::LoadNone);
        
        // OpenMasked is always uncached
        UsdStageRefPtr const stage = UsdStage::OpenMasked(rootLayer, sessionLayer, 
                ArGetResolver().GetCurrentContext(), mask,
                load);

        TF_DEBUG(USDKATANA_CACHE_STAGE).Msg(
                    "{USD STAGE CACHE} Loaded uncached stage "
                    "(%s, forcePopulate=%s) "
                    "with UsdStage address '%lx'\n"
                    "and sessionAttr hash '%s'\n",
                    fileName.c_str(),
                    forcePopulate?"true":"false",
                    (size_t)stage.operator->(),
                    sessionAttr.getHash().str().c_str());
        
        // Mute layers according to a regex.
        _SetMutedLayers(stage, ignoreLayerRegex);

        return stage;
    }
    
    return UsdStageRefPtr();
}


void UsdKatanaCache::FlushStage(const UsdStageRefPtr & stage)
{
    UsdStageCache& stageCache = UsdUtilsStageCache::Get();
    
    stageCache.Erase(stage);
}


std::string UsdKatanaCache::_ComputeCacheKey(
    FnAttribute::GroupAttribute sessionAttr,
    const std::string& rootLocation) {
    return FnAttribute::GroupAttribute(
        // replace invalid sessionAttr with empty valid group for consistency
        // with external queries based on "info.usd.outputSession"
        "s", sessionAttr.isValid() ? sessionAttr : FnAttribute::GroupAttribute(true),
        "r", FnAttribute::StringAttribute(rootLocation), true)
        .getHash()
        .str();
}

SdfLayerRefPtr UsdKatanaCache::FindSessionLayer(
    FnAttribute::GroupAttribute sessionAttr,
    const std::string& rootLocation) {
    std::string cacheKey = _ComputeCacheKey(sessionAttr, rootLocation);
    return FindSessionLayer(cacheKey);
}

SdfLayerRefPtr UsdKatanaCache::FindSessionLayer(
    const std::string& cacheKey) {
    boost::upgrade_lock<boost::upgrade_mutex>
                readerLock(UsdKatanaGetSessionCacheLock());
    const auto& it = _sessionKeyCache.find(cacheKey);
    if (it != _sessionKeyCache.end()) {
        return it->second;
    }
    return NULL;
}



SdfLayerRefPtr UsdKatanaCache::FindOrCreateSessionLayer(
        const std::string& sessionAttrXML,
        const std::string& rootLocation)
{
    FnAttribute::GroupAttribute sessionAttr =
            FnAttribute::Attribute::parseXML(sessionAttrXML.c_str());
    
    if (!sessionAttr.isValid())
    {
        sessionAttr = FnAttribute::GroupAttribute(true);
    }
    
    return _FindOrCreateSessionLayer(sessionAttr, rootLocation);
}



PXR_NAMESPACE_CLOSE_SCOPE

